
cp ../pg_jinx/init_jni.c src/C/pljava/init_jni.c 

export PROJECT_DIR=/Users/r0ml/Repositories/Postgres/Transgres
export JAVA_HOME=${PROJECT_DIR}/Vendor/jdk1.7.jdk/Contents/Home
export JAVA_LCPATH=@loader_path/../../PlugIns/jdk1.7.jdk/Contents/Home/jre/lib/server

cd "${PROJECT_DIR}/pljava"

make -j1 "JAVA_HOME=${JAVA_HOME}" "PG_CONFIG=${PROJECT_DIR}/Vendor/postgres/bin/pg_config" CC=gcc CFLAGS="-g"

cd "${PROJECT_DIR}/pljava"
install_name_tool -add_rpath "${JAVA_LCPATH}" build/objs/pljava.so
cp build/objs/pljava.so "${PROJECT_DIR}/Vendor/postgres/lib/pljava.so"
cp build/pljava.jar "${PROJECT_DIR}/Vendor/postgres/lib/pljava.jar"
ditto build/classes "${PROJECT_DIR}/Vendor/postgres/lib/classes"
cp src/sql/pljava.control src/sql/pljava--*.sql "${PROJECT_DIR}/Vendor/postgres/share/extension/"
