#!/usr/bin/env bash
# mech_bench.sh <shapes.txt> <outdir> [N]
# 기법별 미시 벤치: 바꾼 메커니즘 하나만 도는 질의를 모양마다 하나씩(shapes.txt: "ID 설명|SQL", SQL 의 @H 자리에 힌트),
# 직렬(NO_PARALLEL_SCAN)·병렬로 예열 1 + N 회. 활성 설치본(~/CUBRID)·DB 는 DB 환경변수(기본 tpch).
# 팔(arm)을 바꿀 때는 호출 쪽에서 goto + conf 정렬 + host_idle_guard --wait 후 이 스크립트를 다시 부른다:
#   PR → REF → REF → PR 순서(교대)로 라운드를 돌리고 mech_summarize.py 로 요약한다.
# CBRD-27361(2026-10-18): TPC-H 합계(−23%)는 조인에 희석돼 기법의 효과(직렬 −61%)를 가렸고, CHAR IN 이 batch 를
# 타지 않던 것(직렬 16.6s → 고친 뒤 9.2s)도 이 벤치에서야 보였다.
set -u
SH=$1; OUT=$2; N=${3:-3}; DB=${DB:-tpch}; PIN=${PIN:-taskset -c 0-15}
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
