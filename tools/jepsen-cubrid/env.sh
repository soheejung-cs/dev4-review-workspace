# Jepsen for CUBRID — toolchain and output locations (source it; setup_env.sh installs what is missing).
# Jepsen 0.3.15's bifurcan is built for Java 21 (class file 65); a CLASSPATH set for the CUBRID JDBC driver must not
# leak into lein.
export JEPSEN_ENV=${JEPSEN_ENV:-$HOME/tools/jepsen-env}
export JAVA_HOME=$JEPSEN_ENV/jdk21
export LEIN_JAVA_CMD=$JAVA_HOME/bin/java
export LEIN_HOME=$JEPSEN_ENV/lein-home
export PATH=$JEPSEN_ENV:$JAVA_HOME/bin:$PATH
export JEPSEN_CUBRID_OUT=${JEPSEN_CUBRID_OUT:-$HOME/dev/scratch/jepsen-cubrid}   # store/, logs/, summary.txt, db/
unset CLASSPATH
