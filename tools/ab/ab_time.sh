#!/bin/bash
# SA 시간 A/B: 질의마다 REF·NEW 교대 N 회. 시간 = csql 의 "selected. (N sec)" 첫 괄호(마지막 괄호는 commit 시간).
# usage: NEW=<install> REF=<install> DB=<db> N=5 ab_time.sh q1.sql q2.sql ...   (출력 한 줄 = 한 샘플; ab_summarize.py 로 집계)
#   GATE=0 이면 호스트 게이트를 건너뛴다. LOCK_WAIT=<초> 면 잠금이 찰 때 기다린다.
# 2026-10-28(하네스 H-05·H-08): 측정 전제를 스크립트가 스스로 확인한다.
#   - 공용 잠금(claude-workspace tools/lib/envlock.sh)을 측정 내내 쥔다 — CTP 의 service stop·pkill cub·goto 빌드가 겹치면 수치가 무효.
#     라이브러리가 없는 환경에서는 경고만 하고 잠금 없이 돈다.
#   - 질의(SQL 파일)마다 시작 전에 호스트 게이트(~/bin/host_idle_guard.sh --wait): 컨테이너 안 ps 는 옆 컨테이너 부하를 못 본다.
#     게이트 결과는 '# gate …' 줄로 남긴다(ab_summarize.py 는 무시한다).
#   - csql 은 cwd 에 csql.err 를 남긴다 → /tmp 대신 W(기본 ~/dev/scratch/ab)에서 돈다.
set -u
N=${N:-5}; DB=${DB:?DB name}; NEW=${NEW:?}; REF=${REF:?}
W=${W:-$HOME/dev/scratch/ab}; mkdir -p "$W"
GATE=${GATE:-1}
ENVLOCK_LIB=${ENVLOCK_LIB:-$HOME/dev/docs/claude-workspace/tools/lib/envlock.sh}
if [ -f "$ENVLOCK_LIB" ]; then
  . "$ENVLOCK_LIB"; envlock_acquire "ab_time $DB" --wait "${LOCK_WAIT:-0}" || exit 1
else
  echo "⚠ 잠금 라이브러리 없음($ENVLOCK_LIB) — 잠금 없이 진행: CTP·빌드와 겹치지 않는지 직접 확인" >&2
fi
gate () {  # <질의> — 호스트가 한가해질 때까지(최대 180분) 기다리고 idle 을 '# gate' 줄로 남긴다
  [ "$GATE" = 1 ] || return 0
  local g=$HOME/bin/host_idle_guard.sh out
  [ -f "$g" ] || { echo "⚠ $g 없음 — 게이트 없이 진행" >&2; return 0; }
  out=$(bash "$g" --wait 180 2>&1) || { echo "$out" >&2; echo "호스트가 180분 동안 바빴다 — 중단" >&2; exit 3; }
  echo "# gate $1 $(echo "$out" | grep -oE 'idle=[0-9.]+%' | tail -1)"
}
run () { local I=$1 SQL=$2; ( export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
  cd "$W" && csql -S -u dba $DB -i "$SQL" 2>&1 | grep 'selected' | grep -oE '\([0-9.]+ sec\)' | head -1 | grep -oE '[0-9.]+' ); }
for SQL in "$@"; do
  SQL=$(readlink -f "$SQL")   # cwd 를 W 로 바꾸므로 상대경로를 절대경로로
  gate "$(basename "$SQL" .sql)"
  for i in $(seq 1 $N); do
    a=$(run $REF "$SQL"); b=$(run $NEW "$SQL")
    echo "$(basename "$SQL" .sql) rep=$i REF=${a:-ERR} NEW=${b:-ERR}"
  done
done
