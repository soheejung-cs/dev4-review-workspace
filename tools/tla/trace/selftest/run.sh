#!/bin/sh
# selftest — 계측 헤더 → trace_to_tla → tlc_run --expect trace 를 끝까지 돌린다. 기대: locked 는 CONFORMS, racy 는 DIVERGES.
# 쓰는 곳: $1(기본 ~/dev/scratch/tla-trace-selftest.<pid>) 아래에만.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOOLS=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HOME/dev/scratch/tla-trace-selftest.$$}
mkdir -p "$OUT"
gcc -O1 -pthread -DTLA_TRACE -o "$OUT/counter" "$HERE/counter.c"
rc=0
for mode in locked racy; do
  W="$OUT/$mode"; rm -rf "$W"; mkdir -p "$W/traces"
  cp "$HERE/Counter.tla" "$HERE/TraceCheck.tla" "$HERE/TraceCheck.cfg" "$W/"
  (cd "$W/traces" && TLA_TRACE_DIR=. "$OUT/counter" "$mode" 30)
  python3 "$TOOLS/trace_to_tla.py" "$W/traces" --out "$W/TraceData.tla" --events Inc
  set +e
  python3 "$TOOLS/tlc_run.py" "$W/TraceCheck.tla" --config "$W/TraceCheck.cfg" --out "$W/tlc" --expect trace
  got=$?
  set -e
  want=0; [ "$mode" = racy ] && want=1
  if [ "$got" = "$want" ]; then echo "selftest $mode: PASS"; else echo "selftest $mode: FAIL (exit $got, want $want)"; rc=1; fi
done
exit $rc
