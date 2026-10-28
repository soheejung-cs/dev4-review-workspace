#!/bin/bash
# 두 리비전의 goto 캐시로 시간 A/B 를 한 번에: usage: ab_pair.sh <revA9> <revB9> <db> <sql>... [N=5]
# 캐시가 없으면 빌드하지 않고 어떤 goto 명령이 필요한지만 알려 준다(빌드는 사용자가 정한다 — 빌드환경-규칙 §0).
# 2026-10-28(하네스 H-05·H-08): 공용 잠금을 여기서 쥐고(ab_time.sh 는 물려받는다) 확인→측정이 한 덩어리로 돈다.
#   호스트 게이트는 ab_time.sh 가 질의마다 --wait 로 한다(예전처럼 바쁘면 바로 끝내지 않고 기다린다).
set -u
A=${1:?revA}; B=${2:?revB}; DB=${3:?db}; shift 3
IA=$HOME/release/CUBRID-goto-$A; IB=$HOME/release/CUBRID-goto-$B
for I in $IA $IB; do [ -x $I/bin/cub_server ] || { echo "설치본 없음 또는 깨짐: $I — 먼저: nohup bash ~/bin/goto.sh $(basename $I | sed 's/CUBRID-goto-//') release > ~/dev/scratch/goto-$(basename $I | sed 's/CUBRID-goto-//').log 2>&1 &"; exit 2; }; done
ENVLOCK_LIB=${ENVLOCK_LIB:-$HOME/dev/docs/claude-workspace/tools/lib/envlock.sh}
if [ -f "$ENVLOCK_LIB" ]; then
  . "$ENVLOCK_LIB"; envlock_acquire "ab_pair $A $B $DB" --wait "${LOCK_WAIT:-0}" || exit 1
else
  echo "⚠ 잠금 라이브러리 없음($ENVLOCK_LIB) — 잠금 없이 진행: CTP·빌드와 겹치지 않는지 직접 확인" >&2
fi
for I in $IA $IB; do grep -q "^$DB" $I/databases/databases.txt || echo "⚠ $I 의 databases.txt 에 $DB 없음 — 등록 필요"; done
diff <(grep -E "^(data_buffer_size|parallelism|max_plan_cache)" $IA/conf/cubrid.conf | sort) <(grep -E "^(data_buffer_size|parallelism|max_plan_cache)" $IB/conf/cubrid.conf | sort) > /dev/null || echo "⚠ 두 설치본 conf(data_buffer_size/parallelism/plan cache)가 다르다 — 측정 계약 위반"
D=$(dirname "$0"); LOG=${LOG:-$HOME/dev/scratch/ab/pair_${A}_${B}_$(date +%H%M%S).log}; mkdir -p "$(dirname "$LOG")"
REF=$IA NEW=$IB DB=$DB N=${N:-5} bash $D/ab_time.sh "$@" > "$LOG" || { echo "ab_time.sh 실패(rc=$?) — $LOG"; exit 3; }
echo "REF=$A NEW=$B DB=$DB N=${N:-5} → $LOG"; python3.11 $D/ab_summarize.py "$LOG"
