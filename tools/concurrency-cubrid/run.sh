#!/bin/bash
# run.sh <hermitage|parallel> [args...] — one tool of tools/concurrency-cubrid against a scratch database on the active
#   install, watching the server: its pid before and after, and new cub_server core files.
#   hermitage [scenario name prefix]      database conchm, created and deleted by the run (~5 min)
#   parallel <seconds> [client threads]   database concpx (600k + 300k rows, PI_ROWS), kept for the next run — delete it with
#                                         `db.sh down concpx`; its server is stopped at the end
# CONC_DB names another database (e.g. one per build), PI_ACTIONS and PI_ROWS pass to ParallelInject.
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
    hermitage) db=${CONC_DB:-conchm}; main=Hermitage.java ;;
    parallel) db=${CONC_DB:-concpx}; main=ParallelInject.java ;;
    *) echo "usage: run.sh hermitage|parallel ..." >&2; exit 2 ;;
esac
mkdir -p "$CONC_OUT"
log=$CONC_OUT/$tool-$(date +%Y%m%d-%H%M%S).log
# a ThreadSanitizer install (goto <rev> tsan): the processes this run starts report into their own directory
tsan_dir=""
case "$(readlink -f "$CUBRID")" in
    */tsan/*) tsan_dir=$CONC_OUT/tsan/$(basename "$log" .log); mkdir -p "$tsan_dir"
              export TSAN_OPTIONS="log_path=$tsan_dir/tsan history_size=4 second_deadlock_stack=1" ;;
esac
broker_was_up=$(cubrid broker status 2>&1 | grep -q "is not running" && echo no || echo yes)
cores0=$(bash "$here/db.sh" cores)
pid0=$(bash "$here/db.sh" up "$db") || exit 2
url="jdbc:cubrid:localhost:$PORT:$db:dba::?logOnException=false&connectTimeout=10"
echo "# $tool $* — $(readlink -f "$CUBRID") $(head -1 "$CUBRID/BUILD_REV" 2>/dev/null) — server pid $pid0" > "$log"
# a hung client must not hold the run forever: parallel gets its seconds + 15 min, hermitage 30 min
limit=1800; [ "$tool" = parallel ] && limit=$(( ${1:-0} + 900 ))
timeout "$limit" "$JAVA" -cp "$JAR" "$here/$main" "$url" "$@" >> "$log" 2>&1 &
jpid=$!
# a stuck server thread in a debug build can flood the error log (2026-11-27: 20 GB in 30 min, a livelock logging every
# turn): stop the run when the database's current error log passes 2 GB
( while kill -0 "$jpid" 2>/dev/null; do
      sz=$(stat -c %s "$(readlink -f "$CUBRID/log/server/${db}_latest.err")" 2>/dev/null)
      if [ "${sz:-0}" -gt $((2 * 1024 * 1024 * 1024)) ]; then
          echo "# LOG FLOOD: the server error log passed 2 GB — the run is stopped (a stuck thread logging in a loop?)" >> "$log"
          kill "$jpid" 2>/dev/null
          break
      fi
      sleep 15
  done ) &
wpid=$!
wait "$jpid"
rc=$?
kill "$wpid" 2>/dev/null; wait "$wpid" 2>/dev/null   # reap the watcher quietly (no "Terminated" line)
pid1=$(bash "$here/db.sh" pid "$db")
cores1=$(bash "$here/db.sh" cores)
srv="server pid $pid0 -> ${pid1:-down}, new cores $((cores1 - cores0))"
last=$(grep -v '^#' "$log" | tail -1)
echo "# rc=$rc $srv" >> "$log"
if [ "$tool" = hermitage ]; then bash "$here/db.sh" down "$db"; else bash "$here/db.sh" stop "$db"; fi
[ "$broker_was_up" = no ] && cubrid broker stop > /dev/null 2>&1   # leave the broker as we found it
if [ -n "$tsan_dir" ]; then
    python3 "$here/tsan_summary.py" "$tsan_dir" --md "$tsan_dir/summary.md" >> "$log" 2>&1
    srv="$srv, tsan: $(sed -n 's/^\([0-9]* reports in [0-9]* files, [0-9]* places\).*/\1/p' "$log" | tail -1) ($tsan_dir)"
fi
echo "$(date '+%F %T') $tool $* | rc=$rc | $srv | $last | $log" | tee -a "$CONC_OUT/summary.txt"
[ "$rc" = 0 ] && [ "$pid0" = "$pid1" ] && [ "$cores1" = "$cores0" ]
