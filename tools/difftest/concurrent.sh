#!/bin/bash
# 세션 간 간섭 검사 — 서버 모드에서 세션 스크립트 K개를 동시에 돌린 출력을 (1) 캐시가 남은 채 단독 (2) 서버 재기동(캐시 비움) 뒤 단독 과 비교한다.
#   bash concurrent.sh -o <outdir> -i <install> [-d <db>] [-r <rounds>] [-R <reset.sql>] <session1.sql> <session2.sql> ...
# 세션 스크립트는 같은 문장 텍스트를 공유하되 바인드 타입·키 범위를 달리 둔다(공유 플랜을 노리는 것). DML 이 공유 표에 누적되면 그 블록은 비교에서 뺀다.
# develop 은 "누가 먼저 컴파일했나" 로 답이 바뀌는 결함이 있어(2026-10-07 실측) 재기동 단독이 기준이다.
set -u
OUT=out; INST=""; DB=demodb; ROUNDS=2; RESET=""
while getopts "o:i:d:r:R:" o; do case $o in o) OUT=$OPTARG;; i) INST=$OPTARG;; d) DB=$OPTARG;; r) ROUNDS=$OPTARG;; R) RESET=$OPTARG;; esac; done; shift $((OPTIND-1))
[ -n "$INST" ] && [ $# -ge 2 ] || { sed -n 2,5p "$0"; exit 2; }
D=${INST/#\~/$HOME}; export CUBRID=$D CUBRID_DATABASES=$D/databases PATH=$D/bin:$PATH LD_LIBRARY_PATH=$D/lib
mkdir -p "$OUT"; cd "$OUT" || exit 2
norm() { grep -v '^$\|^Execute OK\|rows* selected\|Committed' | sed -E 's/\([0-9.]+ sec\)//g; s/line [0-9]+/line N/g; s/B\+tree: [0-9|]+|CLASS_OID: [0-9|]+|OID: [0-9|]+/OID/g'; }
up() { cubrid service stop >/dev/null 2>&1; cubrid server start "$DB" >/dev/null 2>&1; for i in $(seq 1 20); do cubrid server status 2>/dev/null | grep -q "$DB" && return; sleep 2; done; echo "서버 기동 실패"; exit 1; }
reset() { [ -n "$RESET" ] && csql -u dba "$DB" -i "$RESET" >/dev/null 2>&1; }
up
for r in $(seq 1 $ROUNDS); do reset; for s in "$@"; do b=$(basename "${s%.sql}"); ( csql -u dba "$DB" -i "$s" 2>&1 | norm > "${b}_conc_r$r.txt" ) & done; wait; done
reset; for s in "$@"; do b=$(basename "${s%.sql}"); csql -u dba "$DB" -i "$s" 2>&1 | norm > "${b}_solo.txt"; done
for s in "$@"; do b=$(basename "${s%.sql}"); up; reset; csql -u dba "$DB" -i "$s" 2>&1 | norm > "${b}_clean.txt"; done
cubrid server stop "$DB" >/dev/null 2>&1; cubrid service stop >/dev/null 2>&1
echo "== 세션별 diff 줄수 (동시 r1..rN vs 재기동 단독 | 캐시 단독 vs 재기동 단독)"
for s in "$@"; do b=$(basename "${s%.sql}"); printf "%-20s" "$b"; for r in $(seq 1 $ROUNDS); do printf " r%d=%s" $r "$(diff "${b}_conc_r$r.txt" "${b}_clean.txt" | grep -c '^[<>]')"; done; echo " | solo=$(diff "${b}_solo.txt" "${b}_clean.txt" | grep -c '^[<>]')"; done
echo "server err fatal/assert: $(grep -ci 'fatal\|assert' $(ls -t $D/log/server/*.err 2>/dev/null | head -1) 2>/dev/null || echo 0)"
