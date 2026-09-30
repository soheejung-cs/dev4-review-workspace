#!/bin/bash
# SA 시간 A/B: 질의마다 REF·NEW 교대 N 회. 시간 = csql 의 "selected. (N sec)" 첫 괄호(마지막 괄호는 commit 시간).
# usage: NEW=<install> REF=<install> DB=<db> N=5 ab_time.sh q1.sql q2.sql ...   (출력 한 줄 = 한 샘플; ab_summarize.py 로 집계)
set -u
N=${N:-5}; DB=${DB:?DB name}; NEW=${NEW:?}; REF=${REF:?}
run () { local I=$1 SQL=$2; ( export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
  cd /tmp && csql -S -u dba $DB -i "$SQL" 2>&1 | grep 'selected' | grep -oE '\([0-9.]+ sec\)' | head -1 | grep -oE '[0-9.]+' ); }
for SQL in "$@"; do for i in $(seq 1 $N); do
  a=$(run $REF "$SQL"); b=$(run $NEW "$SQL")
  echo "$(basename "$SQL" .sql) rep=$i REF=${a:-ERR} NEW=${b:-ERR}"
done; done
