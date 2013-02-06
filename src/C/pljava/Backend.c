/*
 * Copyright (c) 2004, 2005, 2006 TADA AB - Taby Sweden
 * Copyright (c) 2009, 2010, 2011 PostgreSQL Global Development Group
 *
 * Distributed under the terms shown in the file COPYRIGHT
 * found in the root folder of this project or at
 * http://wiki.tada.se/index.php?title=PLJava_License
 *
 * @author Thomas Hallgren
 */
#include <postgres.h>
#include <miscadmin.h>
#ifndef WIN32
#include <libpq/pqsignal.h>
#endif
#include <executor/spi.h>
#include <commands/trigger.h>
#include <utils/elog.h>
#include <utils/guc.h>
#include <fmgr.h>
#include <access/heapam.h>
#include <utils/syscache.h>
#include <catalog/catalog.h>
#include <catalog/pg_proc.h>
#include <catalog/pg_type.h>
#include <storage/ipc.h>
#include <storage/proc.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <libproc.h>
#include <syslog.h>

#include "org_postgresql_pljava_internal_Backend.h"
#include "pljava/Invocation.h"
#include "pljava/Function.h"
#include "pljava/HashMap.h"
#include "pljava/Exception.h"
#include "pljava/Backend.h"
#include "pljava/Session.h"
#include "pljava/SPI.h"
#include "pljava/type/String.h"
/* Example format: "/usr/local/pgsql/lib" */
#ifndef PKGLIBDIR
#error "PKGLIBDIR needs to be defined to compile this file."
#endif

/* Include the 'magic block' that PostgreSQL 8.2 and up will use to ensure
 * that a module is not loaded into an incompatible server.
 */ 
#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#ifdef PG_GETCONFIGOPTION
#error The macro PG_GETCONFIGOPTION needs to be renamed.
#endif

#if ( PGSQL_MAJOR_VER > 8 )
#if ( PGSQL_MINOR_VER > 0 )
#define PG_GETCONFIGOPTION(key) GetConfigOption(key, false, true)
#else
#define PG_GETCONFIGOPTION(key) GetConfigOption(key, true)
#endif
#else
#define PG_GETCONFIGOPTION(key) GetConfigOption(key)
#endif

#define LOCAL_REFERENCE_COUNT 128

jlong mainThreadId;

static JavaVM* s_javaVM = 0;
static jclass  s_Backend_class;
static jmethodID s_setTrusted;
static char* classpath;
static int   statementCacheSize;
static bool  pljavaDebug = false;
static bool  pljavaReleaseLingeringSavepoints;
static bool  s_currentTrust;
static int   s_javaLogLevel;
static char *vmoptions = " -Xdebug -Xrunjdwp:transport=dt_socket,server=y,suspend=n,address=localhost:9909 -Djaxp.debug=1";

bool integerDateTimes = false;

extern void Invocation_initialize(void);
extern void Exception_initialize(void);
extern void Exception_initialize2(void);
extern void HashMap_initialize(void);
extern void SPI_initialize(void);
extern void Type_initialize(void);
extern void Function_initialize(void);
extern void Session_initialize(void);
extern void PgSavepoint_initialize(void);
extern void XactListener_initialize(void);
extern void SubXactListener_initialize(void);
extern void SQLInputFromChunk_initialize(void);
extern void SQLOutputToChunk_initialize(void);
extern void SQLInputFromTuple_initialize(void);
extern void SQLOutputToTuple_initialize(void);

static void initPLJavaClasses(void)
{
	jfieldID tlField;
	JNINativeMethod backendMethods[] =
	{
		{
		"isCallingJava",
	  	"()Z",
	  	Java_org_postgresql_pljava_internal_Backend_isCallingJava
		},
		{
		"isReleaseLingeringSavepoints",
	  	"()Z",
	  	Java_org_postgresql_pljava_internal_Backend_isReleaseLingeringSavepoints
		},
		{
		"_getConfigOption",
		"(Ljava/lang/String;)Ljava/lang/String;",
		Java_org_postgresql_pljava_internal_Backend__1getConfigOption
		},
		{
		"_getStatementCacheSize",
		"()I",
		Java_org_postgresql_pljava_internal_Backend__1getStatementCacheSize
		},
		{
		"_log",
		"(ILjava/lang/String;)V",
		Java_org_postgresql_pljava_internal_Backend__1log
		},
		{
		"_clearFunctionCache",
		"()V",
		Java_org_postgresql_pljava_internal_Backend__1clearFunctionCache
		},
		{ 0, 0, 0 }
	};

	Exception_initialize();

	elog(DEBUG1, "Getting Backend class pljava.jar");
	s_Backend_class = PgObject_getJavaClass("org/postgresql/pljava/internal/Backend");
	elog(DEBUG1, "Backend class was there");
	PgObject_registerNatives2(s_Backend_class, backendMethods);

	tlField = PgObject_getStaticJavaField(s_Backend_class, "THREADLOCK", "Ljava/lang/Object;");
	JNI_setThreadLock(JNI_getStaticObjectField(s_Backend_class, tlField));

	Invocation_initialize();
	Exception_initialize2();
	SPI_initialize();
	Type_initialize();
	Function_initialize();
	Session_initialize();
	PgSavepoint_initialize();
	XactListener_initialize();
	SubXactListener_initialize();
	SQLInputFromChunk_initialize();
	SQLOutputToChunk_initialize();
	SQLInputFromTuple_initialize();
	SQLOutputToTuple_initialize();

	s_setTrusted = PgObject_getStaticJavaMethod(s_Backend_class, "setTrusted", "(Z)V");
}

/**
 *  Initialize security
 */
void Backend_setJavaSecurity(bool trusted)
{
	if(trusted != s_currentTrust)
	{
		/* GCJ has major issues here. Real work on SecurityManager and
		 * related classes has just started in version 4.0.0.
		 */
#ifndef GCJ
		JNI_callStaticVoidMethod(s_Backend_class, s_setTrusted, (jboolean)trusted);
		if(JNI_exceptionCheck())
		{
			JNI_exceptionDescribe();
			JNI_exceptionClear();
			ereport(ERROR, (
				errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("Unable to initialize java security")));
		}
#endif
		s_currentTrust = trusted;
	}
}

int Backend_setJavaLogLevel(int logLevel)
{
	int oldLevel = s_javaLogLevel;
	s_javaLogLevel = logLevel;
	return oldLevel;
}
	
/**
 * Special purpose logging function called from JNI when verbose is enabled.
 */
static jint JNICALL my_vfprintf(FILE* fp, const char* format, va_list args)
{
	char buf[1024];
	char* ep;
	char* bp = buf;

    vsnprintf(buf, sizeof(buf), format, args);

    /* Trim off trailing newline and other whitespace.
     */
	ep = bp + strlen(bp) - 1;
    while(ep >= bp && isspace(*ep))
 		--ep;
 	++ep;
 	*ep = 0;

    elog(s_javaLogLevel, "%s", buf);
    return 0;
}

#if !defined(WIN32)

static void pljavaStatementCancelHandler(int signum)
{
	if(!proc_exit_inprogress)
	{
		/* Never service the interrupt immediately. In order to find out if
		 * its safe, we would need to know what kind of threading mechanism
		 * the VM uses. That would count for a lot of conditional code.
		 */
		QueryCancelPending = true;
		InterruptPending = true;
	}
}

static void pljavaDieHandler(int signum)
{
	if(!proc_exit_inprogress)
	{
		/* Never service the interrupt immediately. In order to find out if
		 * its safe, we would need to know what kind of threading mechanism
		 * the VM uses. That would count for a lot of conditional code.
		 */
		ProcDiePending = true;
		InterruptPending = true;
	}
}

static void pljavaQuickDieHandler(int signum)
{
	/* Just die. No ereporting here since we don't know what thread this is.
	 */
	exit(1);
}

static sigjmp_buf recoverBuf;
static void terminationTimeoutHandler(int signum)
{
	kill(MyProcPid, SIGQUIT);
	
	/* Some sleep to get the SIGQUIT a chance to generate
	 * the needed output.
	 */
	pg_usleep(1);

	/* JavaVM did not die within the alloted time
	 */
	siglongjmp(recoverBuf, 1);
}
#endif

/*
 * proc_exit callback to tear down the JVM
 */
static void _destroyJavaVM(int status, Datum dummy)
{
	if(s_javaVM != 0)
	{
		Invocation ctx;
#if !defined(WIN32)
		pqsigfunc saveSigAlrm;

		Invocation_pushInvocation(&ctx, false);
		if(sigsetjmp(recoverBuf, 1) != 0)
		{
			elog(DEBUG1, "JavaVM destroyed with force");
			s_javaVM = 0;
			return;
		}

		saveSigAlrm = pqsignal(SIGALRM, terminationTimeoutHandler);
		enable_sig_alarm(5000, false);

		elog(DEBUG1, "Destroying JavaVM...");
		JNI_destroyVM(s_javaVM);
		disable_sig_alarm(false);
		pqsignal(SIGALRM, saveSigAlrm);
#else
		Invocation_pushInvocation(&ctx, false);
		elog(DEBUG1, "Destroying JavaVM...");
		JNI_destroyVM(s_javaVM);
#endif
		elog(DEBUG1, "JavaVM destroyed");
		s_javaVM = 0;
		currentInvocation = 0;
	}
}

typedef struct {
	JavaVMOption* options;
	unsigned int  size;
	unsigned int  capacity;
} JVMOptList;

static void JVMOptList_init(JVMOptList* jol)
{
	jol->options  = (JavaVMOption*)palloc(10 * sizeof(JavaVMOption));
	jol->size     = 0;
	jol->capacity = 10;
}

static void JVMOptList_delete(JVMOptList* jol)
{
	JavaVMOption* opt = jol->options;
	JavaVMOption* top = opt + jol->size;
	while(opt < top)
	{
		pfree(opt->optionString);
		opt++;
	}
	pfree(jol->options);
}

static void JVMOptList_add(JVMOptList* jol, const char* optString, void* extraInfo, bool mustCopy)
{
	JavaVMOption* added;

	int newPos = jol->size;
	if(newPos >= jol->capacity)
	{
		int newCap = jol->capacity * 2;
		JavaVMOption* newOpts = (JavaVMOption*)palloc(newCap * sizeof(JavaVMOption));
		memcpy(newOpts, jol->options, newPos * sizeof(JavaVMOption));
		pfree(jol->options);
		jol->options = newOpts;
		jol->capacity = newCap;
	}
	added = jol->options + newPos;
	if(mustCopy)
		optString = pstrdup(optString);

	added->optionString = (char*)optString;
	added->extraInfo    = extraInfo;
	jol->size++;

	elog(DEBUG1, "Added JVM option string \"%s\"", optString);		
}

/* Split JVM options. The string is split on whitespace unless the
 * whitespace is found within a string or is escaped by backslash. A
 * backslash escaped quote is not considered a string delimiter.
 */
static void addUserJVMOptions(JVMOptList* optList, const char *vmopts) {
	const char* cp = vmopts;
	
	if(cp != NULL)
	{
		StringInfoData buf;
		char quote = 0;
		char c;

		initStringInfo(&buf);
		for(;;)
		{
			c = *cp++;
			switch(c)
			{
				case 0:
					break;

				case '"':
				case '\'':
					if(quote == c)
						quote = 0;
					else
						quote = c;
					appendStringInfoChar(&buf, c);
					continue;

				case '\\':
					appendStringInfoChar(&buf, '\\');
					c = *cp++;	/* Interpret next character verbatim */
					if(c == 0)
						break;
					appendStringInfoChar(&buf, c);
					continue;
					
				default:
					if(quote == 0 && isspace((int)c))
					{
						while((c = *cp++) != 0)
						{
							if(!isspace((int)c))
								break;
						}

						if(c == 0)
							break;

						if(c != '-')
							appendStringInfoChar(&buf, ' ');
						else if(buf.len > 0)
						{
							/* Whitespace followed by '-' triggers new
							 * option declaration.
							 */
							JVMOptList_add(optList, buf.data, 0, true);
							buf.len = 0;
							buf.data[0] = 0;
						}
					}
					appendStringInfoChar(&buf, c);
					continue;
			}
			break;
		}
		if(buf.len > 0)
			JVMOptList_add(optList, buf.data, 0, true);
		pfree(buf.data);
	}
}

/**
 *  Initialize the session
 */
static void initJavaSession(void)
{
	jclass sessionClass = PgObject_getJavaClass("org/postgresql/pljava/internal/Session");
	jmethodID init = PgObject_getStaticJavaMethod(sessionClass, "init", "()J");
	mainThreadId = JNI_callStaticLongMethod(sessionClass, init);
	JNI_deleteLocalRef(sessionClass);

	if(JNI_exceptionCheck())
	{
		JNI_exceptionDescribe();
		JNI_exceptionClear();
		ereport(ERROR, (
			errcode(ERRCODE_INTERNAL_ERROR),
			errmsg("Unable to initialize java session")));
	}
}

static void checkIntTimeType(void)
{
	const char* idt = PG_GETCONFIGOPTION("integer_datetimes");

	integerDateTimes = (strcmp(idt, "on") == 0);
	elog(DEBUG1, integerDateTimes ? "Using integer_datetimes" : "Not using integer_datetimes");
}

static bool s_firstTimeInit = true;

static void initializeJavaVM(void)
{
	jboolean jstat;
	JavaVMInitArgs vm_args;
	JVMOptList optList;
    
	JVMOptList_init(&optList);

	if(s_firstTimeInit)
	{
		s_firstTimeInit = false;
		s_javaLogLevel = INFO;

		checkIntTimeType();
		HashMap_initialize();
	
	/*
		DefineCustomStringVariable(
			"pljava.vmoptions",
			"Options sent to the JVM when it is created",
			NULL,
			&vmoptions,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				NULL,
			#endif
			PGC_USERSET,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				0,
			#endif
			#if (PGSQL_MAJOR_VER > 9 || (PGSQL_MAJOR_VER == 9 && PGSQL_MINOR_VER > 0))
				NULL,
			#endif
			NULL, NULL);
	
		DefineCustomStringVariable(
			"pljava.classpath",
			"Classpath used by the JVM",
			NULL,
			&classpath,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				NULL,
			#endif
			PGC_USERSET,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				0,
			#endif
			#if (PGSQL_MAJOR_VER > 9 || (PGSQL_MAJOR_VER == 9 && PGSQL_MINOR_VER > 0))
				NULL,
			#endif
			NULL, NULL);
	*/
	
		DefineCustomIntVariable(
			"pljava.statement_cache_size",
			"Size of the prepared statement MRU cache",
			NULL,
			&statementCacheSize,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				11,
			#endif
			0, 512,
			PGC_USERSET,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				0,
			#endif
			#if (PGSQL_MAJOR_VER > 9 || (PGSQL_MAJOR_VER == 9 && PGSQL_MINOR_VER > 0))
				NULL,
			#endif
			NULL, NULL);
	
		DefineCustomBoolVariable(
			"pljava.release_lingering_savepoints",
			"If true, lingering savepoints will be released on function exit. If false, the will be rolled back",
			NULL,
			&pljavaReleaseLingeringSavepoints,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				false,
			#endif
			PGC_USERSET,
			#if (PGSQL_MAJOR_VER > 8 || (PGSQL_MAJOR_VER == 8 && PGSQL_MINOR_VER > 3))
				0,
			#endif
			#if (PGSQL_MAJOR_VER > 9 || (PGSQL_MAJOR_VER == 9 && PGSQL_MINOR_VER > 0))
				NULL,
			#endif
			NULL, NULL);
	
		EmitWarningsOnPlaceholders("pljava");
			s_firstTimeInit = false;
	}

#ifdef PLJAVA_DEBUG
	/* Hard setting for debug. Don't forget to recompile...
	 */
	pljavaDebug = true;
#endif

	addUserJVMOptions(&optList, vmoptions);
	
	/*
	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfo(&buf, "asdfasdf");
	appendBinaryStringInfo(&buf, "adfasdf",7);
	char *path = buf.data
	*/
	
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
	{
	    int ret;
        pid_t pid; 

        pid = getpid();
        ret = proc_pidpath (pid, pathbuf, sizeof(pathbuf));
        if ( ret <= 0 ) {
            syslog(LOG_ERR, "PID %d: proc_pidpath ();\n", pid);
            syslog(LOG_ERR, "    %s\n", strerror(errno));
        } else {
            syslog(LOG_ERR, "proc %d: %s\n", pid, pathbuf);
        }
    }
    char *lst = strrchr(pathbuf,'/'); // last slash in path
    int n = lst-pathbuf;
    StringInfoData buf;
    initStringInfo(&buf);
    appendStringInfo(&buf, "-Djava.class.path=");
    appendBinaryStringInfo(&buf, pathbuf, n+1);
    appendStringInfo(&buf,"../lib/pljava.jar");
    appendStringInfo(&buf,":");
    appendBinaryStringInfo(&buf, pathbuf, n+1);
    appendStringInfo(&buf,"../lib/classes/");

    effectiveClassPath = buf.data;
    syslog(LOG_ERR, "classpath = %s\n", effectiveClassPath);
    
	if(effectiveClassPath != 0)
	{
		JVMOptList_add(&optList, effectiveClassPath, 0, true);
	}

	/**
	 * As stipulated by JRT-2003
	 */
	JVMOptList_add(&optList, 
		"-Dsqlj.defaultconnection=jdbc:default:connection",
		0, true);

	JVMOptList_add(&optList, "vfprintf", (void*)my_vfprintf, true);
#ifndef GCJ
	JVMOptList_add(&optList, "-Xrs", 0, true);
#endif
	if(pljavaDebug)
	{
		elog(INFO, "Backend pid = %d. Attach the debugger and set pljavaDebug to false to continue", getpid());
		while(pljavaDebug)
			pg_usleep(1000000L);
	}

	vm_args.nOptions = optList.size;
	vm_args.options  = optList.options;
	vm_args.version  = JNI_VERSION_1_4;
	vm_args.ignoreUnrecognized = JNI_FALSE;

	elog(DEBUG1, "Creating JavaVM");

	jstat = JNI_createVM(&s_javaVM, &vm_args);

	if(jstat == JNI_OK && JNI_exceptionCheck())
	{
		JNI_exceptionDescribe();
		JNI_exceptionClear();
		jstat = JNI_ERR;
	}
	JVMOptList_delete(&optList);

	if(jstat != JNI_OK)
		ereport(ERROR, (errmsg("Failed to create Java VM")));

#if !defined(WIN32)
	pqsignal(SIGINT,  pljavaStatementCancelHandler);
	pqsignal(SIGTERM, pljavaDieHandler);
	pqsignal(SIGQUIT, pljavaQuickDieHandler);
#endif
	elog(DEBUG1, "JavaVM created");

	/* Register an on_proc_exit handler that destroys the VM
	 */
	on_proc_exit(_destroyJavaVM, 0);
	initPLJavaClasses();
	initJavaSession();
}

static Datum internalCallHandler(bool trusted, PG_FUNCTION_ARGS);

extern Datum javau_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(javau_call_handler);

/*
 * This is the entry point for all untrusted calls.
 */
Datum javau_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(false, fcinfo);
}

extern Datum java_call_handler(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(java_call_handler);

/*
 * This is the entry point for all trusted calls.
 */
Datum java_call_handler(PG_FUNCTION_ARGS)
{
	return internalCallHandler(true, fcinfo);
}

static Datum internalCallHandler(bool trusted, PG_FUNCTION_ARGS)
{
	Invocation ctx;
	Datum retval = 0;

	if(s_javaVM == 0)
	{
		Invocation_pushBootContext(&ctx);
		PG_TRY();
		{
			initializeJavaVM();
			Invocation_popBootContext();
		}
		PG_CATCH();
		{
			Invocation_popBootContext();

			/* JVM initialization failed for some reason. Destroy
			 * the VM if it exists. Perhaps the user will try
			 * fixing the pljava.classpath and make a new attempt.
			 */
			_destroyJavaVM(0, 0);			

			/* We can't stay here...
			 */
			PG_RE_THROW();
		}
		PG_END_TRY();

		/* Force initial setting
 		 */
		s_currentTrust = !trusted;
	}

	Invocation_pushInvocation(&ctx, trusted);
	PG_TRY();
	{
		Function function = Function_getFunction(fcinfo);
		if(CALLED_AS_TRIGGER(fcinfo))
		{
			/* Called as a trigger procedure
			 */
			retval = Function_invokeTrigger(function, fcinfo);
		}
		else
		{
			/* Called as a function
			 */
			retval = Function_invoke(function, fcinfo);
		}
		Invocation_popInvocation(false);
	}
	PG_CATCH();
	{
		Invocation_popInvocation(true);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return retval;
}

/****************************************
 * JNI methods
 ****************************************/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
	return JNI_VERSION_1_4;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _getConfigOption
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL
JNICALL Java_org_postgresql_pljava_internal_Backend__1getConfigOption(JNIEnv* env, jclass cls, jstring jkey)
{
	jstring result = 0;
	
	BEGIN_NATIVE
	char* key = String_createNTS(jkey);
	if(key != 0)
	{
		PG_TRY();
		{
			const char *value = PG_GETCONFIGOPTION(key);
			pfree(key);
			if(value != 0)
				result = String_createJavaStringFromNTS(value);
		}
		PG_CATCH();
		{
			Exception_throw_ERROR("GetConfigOption");
		}
		PG_END_TRY();
	}
	END_NATIVE
	return result;
}


/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _getStatementCacheSize
 * Signature: ()I
 */
JNIEXPORT jint JNICALL
Java_org_postgresql_pljava_internal_Backend__1getStatementCacheSize(JNIEnv* env, jclass cls)
{
	return statementCacheSize;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _log
 * Signature: (ILjava/lang/String;)V
 */
JNIEXPORT void JNICALL
JNICALL Java_org_postgresql_pljava_internal_Backend__1log(JNIEnv* env, jclass cls, jint logLevel, jstring jstr)
{
	BEGIN_NATIVE_NO_ERRCHECK
	char* str = String_createNTS(jstr);
	if(str != 0)
	{
		/* elog uses printf formatting but the logger does not so we must escape all
		 * '%' in the string.
		 */
		char c;
		const char* cp;
		int percentCount = 0;
		for(cp = str; (c = *cp) != 0; ++cp)
		{
			if(c == '%')
				++percentCount;
		}
	
		if(percentCount > 0)
		{
			/* Make room to expand all "%" to "%%"
			 */
			char* str2 = palloc((cp - str) + percentCount + 1);
			char* cp2 = str2;
	
			/* Expand... */
			for(cp = str; (c = *cp) != 0; ++cp)
			{
				if(c == '%')
					*cp2++ = c;
				*cp2++ = c;
			}
			*cp2 = 0;
			pfree(str);
			str = str2;
		}
	
		PG_TRY();
		{
			elog(logLevel, "%s", str);
			pfree(str);
		}
		PG_CATCH();
		{
			Exception_throw_ERROR("ereport");
		}
		PG_END_TRY();
	}
	END_NATIVE
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    isCallingJava
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend_isCallingJava(JNIEnv* env, jclass cls)
{
	return JNI_isCallingJava();
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    isReleaseLingeringSavepoints
 * Signature: ()Z
 */
JNIEXPORT jboolean JNICALL
Java_org_postgresql_pljava_internal_Backend_isReleaseLingeringSavepoints(JNIEnv* env, jclass cls)
{
	return pljavaReleaseLingeringSavepoints ? JNI_TRUE : JNI_FALSE;
}

/*
 * Class:     org_postgresql_pljava_internal_Backend
 * Method:    _clearFunctionCache
 * Signature: ()V
 */
JNIEXPORT void JNICALL
Java_org_postgresql_pljava_internal_Backend__1clearFunctionCache(JNIEnv* env, jclass cls)
{
	BEGIN_NATIVE_NO_ERRCHECK
	Function_clearFunctionCache();
	END_NATIVE
}
