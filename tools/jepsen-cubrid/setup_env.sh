#!/bin/bash
# setup_env.sh — installs what the tool needs under $JEPSEN_ENV (default ~/tools/jepsen-env, no sudo) and prepares the
# output directory; safe to run again (skips what is there).
#   JDK 21 (Adoptium latest GA), Leiningen (stable script; it downloads its own jar), the test's Maven deps.
set -e
cd "$(dirname "$0")"; . ./env.sh
mkdir -p "$JEPSEN_ENV" "$JEPSEN_CUBRID_OUT"/{store,logs,db}
if [ ! -x "$JAVA_HOME/bin/java" ]; then
  echo "downloading JDK 21 into $JAVA_HOME"
  curl -sSL -o "$JEPSEN_ENV/jdk21.tar.gz" "https://api.adoptium.net/v3/binary/latest/21/ga/linux/x64/jdk/hotspot/normal/eclipse"
  mkdir -p "$JAVA_HOME" && tar xzf "$JEPSEN_ENV/jdk21.tar.gz" -C "$JAVA_HOME" --strip-components=1 && rm -f "$JEPSEN_ENV/jdk21.tar.gz"
fi
if [ ! -x "$JEPSEN_ENV/lein" ]; then
  echo "downloading Leiningen into $JEPSEN_ENV/lein"
  curl -sSL -o "$JEPSEN_ENV/lein" "https://raw.githubusercontent.com/technomancy/leiningen/stable/bin/lein" && chmod +x "$JEPSEN_ENV/lein"
fi
[ -L store ] || [ -e store ] || ln -s "$JEPSEN_CUBRID_OUT/store" store   # Jepsen writes ./store: keep it out of the repo
[ -f "$HOME/CUBRID/jdbc/cubrid_jdbc.jar" ] || { echo "no ~/CUBRID/jdbc/cubrid_jdbc.jar - is an install active?"; exit 2; }
"$JAVA_HOME/bin/java" -version 2>&1 | head -1
lein version
lein deps > "$JEPSEN_CUBRID_OUT/logs/lein_deps.log" 2>&1 && echo "deps ok"
