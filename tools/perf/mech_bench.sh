#!/usr/bin/env bash
# mech_bench.sh <shapes.txt> <outdir> [N]
# 기법별 미시 벤치: 바꾼 메커니즘 하나만 도는 질의를 모양마다 하나씩(shapes.txt: "ID 설명|SQL", SQL 의 @H 자리에 힌트),
# 직렬(NO_PARALLEL_SCAN)·병렬로 예열 1 + N 회. 활성 설치본(~/CUBRID)·DB 는 DB 환경변수(기본 tpch).
# 팔(arm)을 바꿀 때는 호출 쪽에서 goto + conf 정렬 후 이 스크립트를 다시 부른다(호스트 게이트·잠금은 스크립트가 한다):
#   PR → REF → REF → PR 순서(교대)로 라운드를 돌리고 mech_summarize.py 로 요약한다.
# CBRD-27361(2026-10-18): TPC-H 합계(−23%)는 조인에 희석돼 기법의 효과(직렬 −61%)를 가렸고, CHAR IN 이 batch 를
# 타지 않던 것(직렬 16.6s → 고친 뒤 9.2s)도 이 벤치에서야 보였다.
# 2026-10-28(하네스 H-05·H-08): 시작 때 공용 잠금(claude-workspace tools/lib/envlock.sh — CTP·goto 빌드가 측정 중 서버를 죽이거나
#   CPU 를 먹지 않게, 라이브러리가 없으면 경고만)과 호스트 게이트(~/bin/host_idle_guard.sh --wait — GATE=0 이면 건너뜀).
#   서버는 매 라운드 여기서 새로 띄우므로 XASL 플랜 캐시가 이전 라운드 것을 쓰지 않는다. 잠금이 찰 때 기다리려면 LOCK_WAIT=<초>.
set -u
SH=$1; OUT=$2; N=${3:-3}; DB=${DB:-tpch}; PIN=${PIN:-taskset -c 0-15}
SH=$(readlink -f "$SH")      # 아래에서 cwd 를 OUT 으로 바꾼다
ENVLOCK_LIB=${ENVLOCK_LIB:-$HOME/dev/docs/claude-workspace/tools/lib/envlock.sh}
if [ -f "$ENVLOCK_LIB" ]; then
  . "$ENVLOCK_LIB"; envlock_acquire "mech_bench $DB $(basename "$OUT")" --wait "${LOCK_WAIT:-0}" || exit 1
else
  echo "⚠ 잠금 라이브러리 없음($ENVLOCK_LIB) — 잠금 없이 진행: CTP·빌드와 겹치지 않는지 직접 확인" >&2
fi
if [ "${GATE:-1}" = 1 ]; then
  if [ -f ~/bin/host_idle_guard.sh ]; then
    bash ~/bin/host_idle_guard.sh --wait 180 || { echo "호스트가 180분 동안 바빴다 — 측정하지 않는다" >&2; exit 3; }
  else
    echo "⚠ ~/bin/host_idle_guard.sh 없음 — 게이트 없이 진행" >&2
  fi
fi
mkdir -p "$OUT"; cd "$OUT"   # csql 은 cwd 에 csql.err 를 남긴다
echo "$(date +%T) install=$(readlink -f ~/CUBRID) rev=$(head -1 ~/CUBRID/BUILD_REV 2>/dev/null)" > run.log
cubrid service stop > /dev/null 2>&1; sleep 2
$PIN cubrid server start "$DB" > server_start.log 2>&1 || { cat server_start.log; exit 1; }
sleep 3
: > times.tsv
while IFS='|' read -r name sql; do
  [ -z "$name" ] && continue
  id=${name%% *}
  for m in ser par; do
    if [ $m = ser ]; then h='/*+ NO_PARALLEL_SCAN */'; else h=''; fi
    q=${sql//@H/$h}
    { echo ";time on"; for r in $(seq 0 "$N"); do echo "$q"; done; } > "$id.$m.sql"
    $PIN timeout 1800 csql -C -u dba "$DB" -i "$id.$m.sql" > "$id.$m.out" 2>&1
    t=$(grep -oE 'selected\. \([0-9.]+ sec\)' "$id.$m.out" | grep -oE '[0-9]+\.[0-9]+' | tail -n +2 | tr '\n' ' ')
    printf '%s\t%s\t%s\n' "$id" "$m" "$t" | tee -a times.tsv
  done
done < "$SH"
cubrid server stop "$DB" > /dev/null 2>&1
echo "$(date +%T) end" >> run.log
