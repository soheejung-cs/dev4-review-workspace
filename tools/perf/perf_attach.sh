#!/usr/bin/env bash
# perf_attach.sh <db> <sql-file> <out.data> [freq]
# cub_server(<db>)의 모든 스레드에 perf 를 붙여 질의 하나를 잰다. 병렬 워커는 질의마다 새로 뜨므로 질의를 띄운 뒤 0.3초 후 스레드 목록으로 붙인다.
# (메모리 howto/프로파일러-위치: ~/tools/bin/perf 는 LD_LIBRARY_PATH 필요, -p 는 붙는 순간의 스레드만 잡는다)
set -u
DB=$1; SQL=$2; OUT=$3; F=${4:-997}
export LD_LIBRARY_PATH=$HOME/tools/lib:${LD_LIBRARY_PATH:-}
# perf 는 컨테이너마다 다르다(메모리 howto/프로파일러-위치 표): $PERF 가 있으면 그것, 없으면 ~/tools/bin/perf(.50·.51), 그것도 없으면 PATH 의 perf(.52 시스템 perf)
if [ -n "$PERF" ]; then PERF_BIN=$PERF; elif [ -x "$HOME/tools/bin/perf" ]; then PERF_BIN=$HOME/tools/bin/perf; export LD_LIBRARY_PATH=$HOME/tools/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}; else PERF_BIN=$(command -v perf); fi
[ -n "$PERF_BIN" ] || { echo "perf 없음 — PERF=<경로> 로 지정"; exit 1; }
PID=$(ps -o pid=,args= -u "$(id -u)" | awk -v db="$DB" '$2=="cub_server" && $3==db {print $1}')
[ -n "$PID" ] || { echo "cub_server $DB 없음"; exit 1; }
csql -u dba "$DB" -i "$SQL" > "$OUT.out" 2>&1 &
C=$!
sleep 0.3
"$PERF_BIN" record -F "$F" -g -o "$OUT" -t "$(ls /proc/$PID/task | paste -sd,)" -- sleep 3600 > /dev/null 2>&1 &
P=$!
wait $C
kill -INT $P; wait $P 2>/dev/null
echo "$(grep -oE 'selected\. \([0-9.]+ sec\)' "$OUT.out" | tail -1)  → $OUT"
