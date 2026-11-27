#!/bin/bash
# db.sh <up|down|pid|cores> <dbname> — the scratch database of tools/concurrency-cubrid on the active install.
#   up     createdb under $CONC_OUT/db/<db> when it is not registered, start its server with core files on
#          (ulimit -c unlimited — the shell default here is 0), start the active install's broker ("already running"
#          is fine). Prints the server pid.
#   down   stop the server; deletedb and remove the empty directories only for a database this tool created under
#          $CONC_OUT/db (a database registered elsewhere is used as it is and never deleted — the Jepsen tool's rule).
#   stop   stop the server and keep the database (a loaded data set for the next run).
#   pid    the server pid from `cubrid server status`, empty when it is down.
#   cores  the number of core files in the system core directory (core_pattern /data/core/core.%e.%p.%h.%t). %e is the
#          name of the thread that died — a server worker dumps as core.transaction.<pid>… (2026-11-27: counting only
#          core.cub_server.* missed the first crash this tool found).
# Daemons write to $CONC_OUT/logs/daemon.log: a daemon started under a pipe holds it open (H-04, H-23).
# Do not run beside CTP, shell TCs, goto builds or benchmarks: they stop servers and share cub_master (H-05).
set -u
CONC_OUT=${CONC_OUT:-$HOME/dev/scratch/concurrency-cubrid}
cmd=${1:?usage: db.sh up|down|pid|cores <dbname>}
db=${2:-}
dir=$CONC_OUT/db/$db
log=$CONC_OUT/logs/daemon.log
CORE_DIR=/data/core
mkdir -p "$CONC_OUT/logs"
dbtxt=${CUBRID_DATABASES:-$CUBRID/databases}/databases.txt
registered () { awk -v d="$db" '$1 == d { print $2 }' "$dbtxt" 2>/dev/null; }
server_pid () { cubrid server status 2>/dev/null | sed -n "s/.*Server $db (rel [^,]*, pid \([0-9]*\)).*/\1/p"; }

# A ThreadSanitizer install (goto … tsan, ~/tsan/): every instrumented process the run starts (server, CAS) writes its
# reports to $CONC_OUT/tsan/tsan.<pid> — a daemon's stderr is not kept.
case "$(readlink -f "$CUBRID")" in
    */tsan/*) mkdir -p "$CONC_OUT/tsan"
              export TSAN_OPTIONS=${TSAN_OPTIONS:-"log_path=$CONC_OUT/tsan/tsan history_size=4 second_deadlock_stack=1"} ;;
esac

case $cmd in
up)
    path=$(registered)
    if [ -z "$path" ]; then
        mkdir -p "$dir"
        cubrid createdb -F "$dir" -L "$dir" --db-volume-size=512M --log-volume-size=256M "$db" en_US.utf8 >> "$log" 2>&1 \
            || { echo "createdb $db failed — see $log" >&2; exit 1; }
    elif [ "$path" != "$dir" ]; then
        echo "$db is registered at $path (outside $CONC_OUT/db) — used as it is, never deleted" >&2
    fi
    if [ -z "$(server_pid)" ]; then
        ( ulimit -c unlimited; cubrid server start "$db" >> "$log" 2>&1 )
    fi
    cubrid broker start >> "$log" 2>&1
    for _ in $(seq 60); do
        p=$(server_pid)
        [ -n "$p" ] && { echo "$p"; exit 0; }
        sleep 1
    done
    echo "server $db did not start — see $log" >&2
    exit 1 ;;
down)
    path=$(registered)
    cubrid server stop "$db" >> "$log" 2>&1
    if [ "$path" = "$dir" ]; then
        cubrid deletedb "$db" >> "$log" 2>&1
        find "$dir" -depth -type d -empty -delete 2>/dev/null
    fi ;;
stop)
    cubrid server stop "$db" >> "$log" 2>&1 ;;
pid)
    server_pid ;;
cores)
    ls "$CORE_DIR" 2>/dev/null | grep -c '^core\.' ;;
*)
    echo "usage: db.sh up|down|pid|cores <dbname>" >&2
    exit 2 ;;
esac
