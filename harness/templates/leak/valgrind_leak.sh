#!/bin/bash
# valgrind memcheck over a SA-mode run: the whole server runs in this one process, so the
# XASL cache and its retire path are exercised in-process and reachable by the leak checker.
set -u
I=${INSTALL:-/home/cubrid/release/CUBRID-expr-wt}
export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
DB=${DB:-vgdb}; N=${N:-60}; TAG=${TAG:-vg}
W=$(cd "$(dirname "$0")" && pwd)/out; mkdir -p $W
set_conf () { grep -q "^$1=" $I/conf/cubrid.conf && sed -i "s/^$1=.*/$1=$2/" $I/conf/cubrid.conf || echo "$1=$2" >> $I/conf/cubrid.conf; }
set_conf max_plan_cache_entries "${ENTRIES:-1000}"
set_conf max_plan_cache_clones "${CLONES:-0}"
[ -f $I/databases/$DB/${DB}_vinf ] || { mkdir -p $I/databases/$DB; ( cd $I/databases/$DB && cubrid createdb --db-volume-size=64M --log-volume-size=32M $DB en_US.utf8 ) > $W/$TAG.createdb 2>&1; }
{
  echo "drop table if exists vg;"
  echo "create table vg (a int, n numeric(10,2), b int);"
  echo "insert into vg values (1,1.5,2),(2,2.5,3),(3,null,4);"
  for i in $(seq 1 $N); do
    echo "select a*2, n*1.5, a+b from vg where a*2 > 1;"
    echo "select sum(a*2), avg(n*1.5) from vg where b > 1;"
  done
} > $W/$TAG.sql
valgrind --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite \
         --num-callers=25 --log-file=$W/$TAG.valgrind.log --error-limit=no \
         csql -S -u dba $DB -i $W/$TAG.sql > $W/$TAG.out 2>&1
echo "== $TAG (entries=${ENTRIES:-1000} clones=${CLONES:-0} N=$N) =="
grep -E 'definitely lost|indirectly lost|possibly lost|still reachable' $W/$TAG.valgrind.log | sed 's/^==[0-9]*== *//'
echo "-- expr_compile / xasl_cache 가 스택에 있는 definite 블록"
awk '/definitely lost in loss record/{blk=$0; buf=""} {buf=buf"\n"$0} /^==[0-9]*== *$/{if (buf ~ /expr_compile|xasl_cache|qexec_clear|expr_prog|expr_scan_pred/ && blk!="") print blk buf; blk=""; buf=""}' $W/$TAG.valgrind.log | head -60
