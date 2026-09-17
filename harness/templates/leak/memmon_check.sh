#!/bin/bash
# In-house check: enable_memory_monitoring tags every allocation by file:line (malloc is
# redefined to cub_alloc in SERVER_MODE), so expr_compile.c must show up and must not grow.
set -u
I=${INSTALL:-/home/cubrid/release/CUBRID-expr-wt}
export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
DB=${DB:-mmdb}; N=${N:-2000}; TAG=${TAG:-mm}
W=$(cd "$(dirname "$0")" && pwd)/out; mkdir -p $W
set_conf () { grep -q "^$1=" $I/conf/cubrid.conf && sed -i "s/^$1=.*/$1=$2/" $I/conf/cubrid.conf || echo "$1=$2" >> $I/conf/cubrid.conf; }
cubrid server stop $DB > /dev/null 2>&1
set_conf cubrid_port_id 1623
set_conf enable_memory_monitoring yes
set_conf max_plan_cache_entries "${ENTRIES:-1000}"
set_conf max_plan_cache_clones "${CLONES:-0}"
[ -f $I/databases/$DB/${DB}_vinf ] || { mkdir -p $I/databases/$DB; ( cd $I/databases/$DB && cubrid createdb --db-volume-size=64M --log-volume-size=32M $DB en_US.utf8 ) > $W/$TAG.createdb 2>&1; }
cubrid server start $DB > $W/$TAG.start 2>&1; sleep 2
csql -u dba $DB -c "drop table if exists mm; create table mm (a int, n numeric(10,2), b int); insert into mm values (1,1.5,2),(2,2.5,3),(3,null,4);" > /dev/null 2>&1
for i in $(seq 1 $N); do echo "select a*2, n*1.5, a+b from mm where a*2 > 1;"; echo "select sum(a*2), avg(n*1.5) from mm where b > 1;"; done > $W/$TAG.sql
csql -u dba $DB -i $W/$TAG.sql > /dev/null 2>&1
cubrid memmon -o $W/$TAG.memmon.1 $DB > /dev/null 2>&1
csql -u dba $DB -i $W/$TAG.sql > /dev/null 2>&1
cubrid memmon -o $W/$TAG.memmon.2 $DB > /dev/null 2>&1
echo "== $TAG (entries=${ENTRIES:-1000} clones=${CLONES:-0}, 배치당 $((N*2)) 문장)"
for f in expr_compile query_opfunc query_aggregate query_executor; do
  a=$(grep -E "$f" $W/$TAG.memmon.1 | awk '{print $NF}' | head -1)
  b=$(grep -E "$f" $W/$TAG.memmon.2 | awk '{print $NF}' | head -1)
  printf "  %-18s batch1=%-12s batch2=%-12s\n" "$f" "${a:--}" "${b:--}"
done
echo "  --- 서버 총계"
grep -iE '^total|server total' $W/$TAG.memmon.1 $W/$TAG.memmon.2 | head -4
cubrid server stop $DB > /dev/null 2>&1
