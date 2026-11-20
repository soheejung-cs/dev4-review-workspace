#!/bin/sh
# Trace sweep for bitmap_heap_steps (tla.md §3-T): run test_trace.cpp's binary (built by unitseam with
# instrument.patch and -DTLA_TRACE, see README.md) with 2 or 3 workers, random OID splits and, every third run,
# a worker that fails in its index step; convert each run and check it against the PR model (MUT="pr") and,
# for sensitivity, the "leave_first" mutant.  Writes only under $1 (default ~/dev/scratch/tla8627-unit).
# usage: run.sh [outdir] [runs]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DEV4=$(cd "$HERE/../../../.." && pwd)
OUT=${1:-$HOME/dev/scratch/tla8627-unit}
N=${2:-30}
BIN="$OUT/build/efca4544a/unit.bin"
# stubs the two TUs reach that do nothing here (snapshot object, private free, thread entry lookup)
export UNITSEAM_ZERO=_Z27mvcc_is_mvcc_disabled_classPK13db_identifier,_ZN13mvcc_snapshotC1Ev,_ZN16mvcc_active_tranD1Ev,_ZN19parallel_index_scan20leaf_page_dispatcher14clear_last_keyEv,_ZN9cubthread9get_entryEv,db_private_free_release
SUM="$OUT/sweep.tsv"; printf 'run\tparams\tactions\toverlap\tpr\tleave_first\n' > "$SUM"
i=0
while [ $i -lt "$N" ]; do
  r=$(od -An -N6 -tu2 /dev/urandom | tr -s ' ' | sed 's/^ //'); set -- $r
  NW=$(( 2 + $1 % 2 ))
  if [ $((i % 3)) -eq 2 ]; then
    P="UT_WORKERS=$NW UT_SEED=$2 UT_SCENARIO=intr UT_FAIL_W=$(( $3 % NW )) UT_FAIL_AFTER=$(( $1 % 2 ))"
  else
    P="UT_WORKERS=$NW UT_SEED=$2"
  fi
  W="$OUT/sweep/$i"; rm -rf "$W"; mkdir -p "$W"
  (cd "$W" && env $P TLA_TRACE_DIR="$W" "$BIN" > run.log 2>&1)
  conv=$(python3 "$DEV4/tools/tla/trace_to_tla.py" "$W" --out "$W/TraceData.tla" --merged "$W/merged.json" \
         --events Mark,EndMarks,Leave,WaitPass,WaitStop,TakePart,Load,OverTry,Close 2>&1 | head -1)
  acts=$(echo "$conv" | sed -n 's/.*행동 \([0-9]*\).*/\1/p'); ov=$(echo "$conv" | sed -n 's/.*겹친 쌍 \([0-9]*\).*/\1/p')
  cp "$HERE/TraceCheck.tla" "$HERE/../BitmapHeapSteps.tla" "$HERE"/TraceCheck*.cfg "$W/"
  res() { python3 "$DEV4/tools/tla/tlc_run.py" "$W/TraceCheck.tla" --config "$W/$1" --out "$W/tlc_$1" --expect trace 2>&1 | grep -o 'TRACE [A-Z]*\|ERROR' | head -1; }
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' $i "$P" "$acts" "$ov" "$(res TraceCheck.cfg)" "$(res TraceCheck_leave_first.cfg)" >> "$SUM"
  i=$((i + 1))
done
column -t -s "$(printf '\t')" "$SUM"
