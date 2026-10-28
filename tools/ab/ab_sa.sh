#!/bin/bash
# SA 정합 A/B: NEW·REF 설치본에 같은 SQL, 타이밍 제거 후 byte 비교. usage: NEW=<install> REF=<install> ab_sa.sh <sql> [tag]
# 2026-10-28(하네스 H-05): 두 설치본에 DB 를 만들고(databases.txt 를 고친다) SA 로 여는 동안 공용 잠금을 쥔다 —
#   goto 의 등록 맞춤·CTP 와 겹치지 않게. 라이브러리가 없으면 경고만. 잠금이 차 있을 때 기다리려면 LOCK_WAIT=<초>.
set -u
W=${W:-$HOME/dev/scratch/ab}; mkdir -p "$W"
NEW=${NEW:?NEW install dir}; OLD=${REF:?REF install dir}
SQL=${1:?sql file}; TAG=${2:-$(basename "$SQL" .sql)}; DB=${DB:-abdb}
SQL=$(readlink -f "$SQL")   # 실행은 W 에서 한다 — 상대경로가 W 기준으로 풀리지 않게
ENVLOCK_LIB=${ENVLOCK_LIB:-$HOME/dev/docs/claude-workspace/tools/lib/envlock.sh}
if [ -f "$ENVLOCK_LIB" ]; then
  . "$ENVLOCK_LIB"; envlock_acquire "ab_sa $TAG" --wait "${LOCK_WAIT:-0}" || exit 1
else
  echo "⚠ 잠금 라이브러리 없음($ENVLOCK_LIB) — 잠금 없이 진행" >&2
fi
run () {  # $1=install $2=out
  local I=$1 OUT=$2
  ( export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
    mkdir -p $I/databases/$DB
    if [ ! -f $I/databases/$DB/${DB}_vinf ]; then
      ( cd $I/databases/$DB && cubrid createdb --db-volume-size=64M --log-volume-size=32M $DB en_US.utf8 ) > $OUT.createdb 2>&1
      [ -f $I/databases/$DB/${DB}_vinf ] || { echo "CREATEDB FAILED for $I"; tail -3 $OUT.createdb; exit 2; }
    fi
    cd "$W" && csql -S -u dba $DB -i "$SQL" > $OUT 2>&1 )   # cwd 를 W 로: csql.err 가 리포에 남지 않게
}
run $NEW $W/$TAG.new.out; run $OLD $W/$TAG.old.out
norm () { sed -E 's/\([0-9]+\.[0-9]+ sec\)//g' "$1"; }
if diff <(norm $W/$TAG.old.out) <(norm $W/$TAG.new.out) > $W/$TAG.diff; then
  echo "IDENTICAL ($TAG): $(grep -c '^' $W/$TAG.new.out) lines, errors=$(grep -c 'ERROR' $W/$TAG.new.out)"
else
  echo "DIFF ($TAG): $(grep -c '^[<>]' $W/$TAG.diff) lines -> $W/$TAG.diff"
fi
