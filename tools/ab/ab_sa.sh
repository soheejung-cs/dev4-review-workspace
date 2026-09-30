#!/bin/bash
# SA 정합 A/B: NEW·REF 설치본에 같은 SQL, 타이밍 제거 후 byte 비교. usage: NEW=<install> REF=<install> ab_sa.sh <sql> [tag]
set -u
W=${W:-$HOME/dev/scratch/ab}; mkdir -p "$W"
NEW=${NEW:?NEW install dir}; OLD=${REF:?REF install dir}
SQL=${1:?sql file}; TAG=${2:-$(basename "$SQL" .sql)}; DB=${DB:-abdb}
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
