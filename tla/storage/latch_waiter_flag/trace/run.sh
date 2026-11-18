#!/bin/sh
# Trace sweep for latch_waiter_flag (tla.md §3-T): run test_trace.cpp's binary (built by unitseam with
# instrument.patch and -DTLA_TRACE, see README.md) many times with different kinds / delays / timeouts,
# convert each run and check it against the model of its revision (head -> FIX="pr", base -> FIX="develop")
# and against the other one (sensitivity).  Writes only under $1 (default ~/dev/scratch/tla28163-unit).
# usage: run.sh [outdir] [runs-per-rev]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DEV4=$(cd "$HERE/../../../.." && pwd)
OUT=${1:-$HOME/dev/scratch/tla28163-unit}
N=${2:-24}
export UNITSEAM_ZERO=er_set,_Z31logtb_find_client_name_host_pidiPPKcS1_S1_Pi   # error-message plumbing only
SUM="$OUT/sweep.tsv"; printf 'rev\trun\tparams\tactions\toverlap\town_model\tother_model\n' > "$SUM"
i=0
while [ $i -lt "$N" ]; do
  # run 0..2: the TC shape; the rest: both requesters at once with random kinds, delays and timeouts
  if [ $i -lt 3 ]; then P="UT_SCENARIO=tc UT_K1_READ=0 UT_K2_READ=1 UT_DH_US=2000 UT_TIMEOUT_MS=3"
  elif [ $((i % 2)) -eq 1 ]; then
    # tight: both ask at once, H unfixes right at the latch timeout -- the wakeup and the give-up race
    r=$(od -An -N8 -tu2 /dev/urandom | tr -s ' ' | sed 's/^ //')
    set -- $r
    P="UT_SCENARIO=race UT_K1_READ=$(( $1 % 2 )) UT_K2_READ=$(( $2 % 2 )) UT_D1_US=0 UT_D2_US=$(( $3 % 50 )) UT_DH_US=$(( 1950 + $4 % 120 )) UT_TIMEOUT_MS=2"
  else
    r=$(od -An -N8 -tu2 /dev/urandom | tr -s ' ' | sed 's/^ //')
    set -- $r
    P="UT_SCENARIO=race UT_K1_READ=$(( $1 % 2 )) UT_K2_READ=$(( $2 % 2 )) UT_D1_US=$(( $3 % 3000 )) UT_D2_US=$(( $4 % 3000 )) UT_DH_US=$(( ($1 + $3) % 7000 )) UT_TIMEOUT_MS=$(( 1 + $2 % 4 ))"
  fi
  for rev in cae32d077 bb71348d6; do
    own=pr; other=develop; [ $rev = bb71348d6 ] && own=develop && other=pr
    W="$OUT/sweep/$rev/$i"; rm -rf "$W"; mkdir -p "$W"
    (cd "$W" && env $P TLA_TRACE_DIR="$W" "$OUT/build/$rev/unit.bin" > run.log 2>&1)
    conv=$(python3 "$DEV4/tools/tla/trace_to_tla.py" "$W" --out "$W/TraceData.tla" --merged "$W/merged.json" \
           --events Fix,Unfix,Woken,GiveUpStart,GiveUp 2>&1 | head -1)
    acts=$(echo "$conv" | sed -n 's/.*행동 \([0-9]*\).*/\1/p'); ov=$(echo "$conv" | sed -n 's/.*겹친 쌍 \([0-9]*\).*/\1/p')
    cp "$HERE/TraceCheck.tla" "$HERE/../LatchWaiterFlag.tla" "$HERE"/TraceCheck_*.cfg "$W/"
    res() { python3 "$DEV4/tools/tla/tlc_run.py" "$W/TraceCheck.tla" --config "$W/TraceCheck_$1.cfg" --out "$W/tlc_$1" --expect trace 2>&1 | grep -o 'TRACE [A-Z]*' | head -1; }
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $rev $i "$P" "$acts" "$ov" "$own:$(res $own)" "$other:$(res $other)" >> "$SUM"
  done
  i=$((i + 1))
done
column -t -s "$(printf '\t')" "$SUM"
