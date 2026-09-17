#!/bin/bash
# Two measured batches on one server: batch 1 includes warm-up, batch 2 is the steady-state signal.
set -u
I=${INSTALL:-/home/cubrid/release/CUBRID-expr-wt}
export CUBRID=$I CUBRID_DATABASES=$I/databases PATH=$I/bin:$PATH LD_LIBRARY_PATH=$I/lib
DB=${DB:-ncdb}; N=${N:-4000}
W=$(cd "$(dirname "$0")" && pwd)/out; mkdir -p $W
set_conf () { grep -q "^$1=" $I/conf/cubrid.conf && sed -i "s/^$1=.*/$1=$2/" $I/conf/cubrid.conf || echo "$1=$2" >> $I/conf/cubrid.conf; }
cubrid server stop $DB > /dev/null 2>&1
set_conf cubrid_port_id 1623; set_conf max_plan_cache_entries 1000; set_conf max_plan_cache_clones "${CLONES:-0}"
[ -f $I/databases/$DB/${DB}_vinf ] || { mkdir -p $I/databases/$DB; ( cd $I/databases/$DB && cubrid createdb --db-volume-size=64M --log-volume-size=32M $DB en_US.utf8 ) > $W/nc2_createdb.log 2>&1; }
cubrid server start $DB > $W/nc2_start.log 2>&1; sleep 2
csql -u dba $DB -c "drop table if exists nc; create table nc (a int, n numeric(10,2), b int); insert into nc values (1,1.5,2),(2,2.5,3),(3,null,4);" > /dev/null 2>&1
rss () { ps -o rss= -p "$(pgrep -x cub_server | head -1)" 2>/dev/null | tr -d ' '; }
for i in $(seq 1 $N); do echo "select a*2, n*1.5, a+b from nc where a*2 > 1;"; done > $W/nc2_loop.sql
echo "install=$(basename $I) clones=${CLONES:-0} N=$N"
csql -u dba $DB -i $W/nc2_loop.sql > /dev/null 2>&1; r1=$(rss)
csql -u dba $DB -i $W/nc2_loop.sql > /dev/null 2>&1; r2=$(rss)
csql -u dba $DB -i $W/nc2_loop.sql > /dev/null 2>&1; r3=$(rss)
echo "  after batch1=${r1}KB  batch2=${r2}KB (delta $((r2-r1))KB)  batch3=${r3}KB (delta $((r3-r2))KB)"
cubrid server stop $DB > /dev/null 2>&1
