#!/bin/bash
# 수명·메모리 — 서버 모드에서 같은 스크립트를 R 라운드 돌리며 cub_server RSS 와 plandump 를 기록한다. 오류 경로 스크립트도 같은 방식으로.
#   bash lifetime.sh -i <install> [-d <db>] [-r <rounds>] <script.sql> [<err_script.sql>]
# 판정: RSS 가 첫 라운드 뒤 고정이면 정상. 라운드마다 수 MB 씩 단조 증가하면 누수 후보 → memmon(enable_memory_monitoring=yes) 으로 좁힌다.
set -u
INST=""; DB=demodb; ROUNDS=6
while getopts "i:d:r:" o; do case $o in i) INST=$OPTARG;; d) DB=$OPTARG;; r) ROUNDS=$OPTARG;; esac; done; shift $((OPTIND-1))
D=${INST/#\~/$HOME}; export CUBRID=$D CUBRID_DATABASES=$D/databases PATH=$D/bin:$PATH LD_LIBRARY_PATH=$D/lib
cubrid service stop >/dev/null 2>&1; cubrid server start "$DB" >/dev/null 2>&1; for i in $(seq 1 20); do cubrid server status 2>/dev/null | grep -q "$DB" && break; sleep 2; done
rss() { ps -o rss= -p "$(pgrep -x cub_server | head -1)" | tr -d ' '; }
for s in "$@"; do echo "== $s  start RSS=$(rss) KB"; for r in $(seq 1 $ROUNDS); do e=$(csql -u dba "$DB" -i "$s" 2>&1 | grep -c ERROR); echo "round$r RSS=$(rss) KB errors=$e"; done; done
echo "== plandump"; cubrid plandump "$DB" 2>/dev/null | sed -n 1,12p
echo "server err fatal/assert: $(grep -ci 'fatal\|assert' $(ls -t $D/log/server/*.err 2>/dev/null | head -1) 2>/dev/null || echo 0)"
cubrid server stop "$DB" >/dev/null 2>&1; cubrid service stop >/dev/null 2>&1
