#!/usr/bin/env bash
# perf_attach.sh <db> <sql-file> <out.data> [freq]
# cub_server(<db>)의 모든 스레드에 perf 를 붙여 질의 하나를 잰다. 병렬 워커는 질의마다 새로 뜨므로 질의를 띄운 뒤 0.3초 후 스레드 목록으로 붙인다.
# (메모리 howto/프로파일러-위치: ~/tools/bin/perf 는 LD_LIBRARY_PATH 필요, -p 는 붙는 순간의 스레드만 잡는다)
set -u
DB=$1; SQL=$2; OUT=$3; F=${4:-997}
export LD_LIBRARY_PATH=$HOME/tools/lib:${LD_LIBRARY_PATH:-}
PID=$(ps -o pid=,args= -u "$(id -u)" | awk -v db="$DB" '$2=="cub_server" && $3==db {print $1}')
[ -n "$PID" ] || { echo "cub_server $DB 없음"; exit 1; }
csql -u dba "$DB" -i "$SQL" > "$OUT.out" 2>&1 &
C=$!
sleep 0.3
"$HOME/tools/bin/perf" record -F "$F" -g -o "$OUT" -t "$(ls /proc/$PID/task | paste -sd,)" -- sleep 3600 > /dev/null 2>&1 &
P=$!
wait $C
kill -INT $P; wait $P 2>/dev/null
echo "$(grep -oE 'selected\. \([0-9.]+ sec\)' "$OUT.out" | tail -1)  → $OUT"
