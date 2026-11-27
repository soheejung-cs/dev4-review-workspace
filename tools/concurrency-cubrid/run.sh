#!/bin/bash
# run.sh <hermitage|parallel> [args...] — one tool of tools/concurrency-cubrid against a scratch database on the active
#   install, watching the server: its pid before and after, and new cub_server core files.
#   hermitage [scenario name prefix]      database conchm, created and deleted by the run (~5 min)
#   parallel <seconds> [client threads]   database concpx (600k + 300k rows), kept for the next run — delete it with
#                                         `db.sh down concpx`; its server is stopped at the end
# Output: $CONC_OUT/<tool>-<time>.log; one line per run appended to $CONC_OUT/summary.txt.
# exit: 0 when the tool found nothing and the server neither restarted nor dumped core.
# Do not run beside CTP, shell TCs, goto builds or benchmarks: they stop servers and share cub_master (H-05).
set -u
here=$(cd "$(dirname "$0")" && pwd)
export CONC_OUT=${CONC_OUT:-$HOME/dev/scratch/concurrency-cubrid}
JAVA=${JAVA:-$HOME/tools/jdk-21/bin/java}   # single-file source launch needs Java 11+; the system java is 8
JAR=$CUBRID/jdbc/cubrid_jdbc.jar
PORT=${BROKER_PORT:-33000}                  # BROKER1 of the active install
tool=${1:?usage: run.sh hermitage|parallel ...}
shift
case $tool in
    hermitage) db=conchm; main=Hermitage.java ;;
    parallel) db=concpx; main=ParallelInject.java ;;
    *) echo "usage: run.sh hermitage|parallel ..." >&2; exit 2 ;;
esac
mkdir -p "$CONC_OUT"
log=$CONC_OUT/$tool-$(date +%Y%m%d-%H%M%S).log
broker_was_up=$(cubrid broker status 2>&1 | grep -q "is not running" && echo no || echo yes)
cores0=$(bash "$here/db.sh" cores)
pid0=$(bash "$here/db.sh" up "$db") || exit 2
url="jdbc:cubrid:localhost:$PORT:$db:dba::?logOnException=false&connectTimeout=10"
echo "# $tool $* — $(readlink -f "$CUBRID") $(head -1 "$CUBRID/BUILD_REV" 2>/dev/null) — server pid $pid0" > "$log"
"$JAVA" -cp "$JAR" "$here/$main" "$url" "$@" >> "$log" 2>&1
rc=$?
pid1=$(bash "$here/db.sh" pid "$db")
cores1=$(bash "$here/db.sh" cores)
srv="server pid $pid0 -> ${pid1:-down}, new cores $((cores1 - cores0))"
last=$(grep -v '^#' "$log" | tail -1)
echo "# rc=$rc $srv" >> "$log"
if [ "$tool" = hermitage ]; then bash "$here/db.sh" down "$db"; else bash "$here/db.sh" stop "$db"; fi
[ "$broker_was_up" = no ] && cubrid broker stop > /dev/null 2>&1   # leave the broker as we found it
echo "$(date '+%F %T') $tool $* | rc=$rc | $srv | $last | $log" | tee -a "$CONC_OUT/summary.txt"
[ "$rc" = 0 ] && [ "$pid0" = "$pid1" ] && [ "$cores1" = "$cores0" ]
