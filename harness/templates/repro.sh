#!/bin/bash
# 재현 스크립트 템플릿 — 하네스 규약: 버릴 DB · 격리 포트 · 종료/삭제 보장(trap) · 판정은 산출물 파일로
# 사용: cp harness/templates/repro.sh projects/<JIRA>/repro/<이름>.sh ; 아래 REPRO 절만 채운다
set -u
DB=${DB:-repro_$$}; PORT=${PORT:-$((20000 + RANDOM % 5000))}; OUT=${OUT:-$(pwd)/repro_$$.out}
CONF="$CUBRID/conf/cubrid.conf"; BAK="$CONF.bak_repro_$$"
cleanup() { cubrid server stop "$DB" >/dev/null 2>&1; cubrid deletedb "$DB" >/dev/null 2>&1; [ -f "$BAK" ] && mv -f "$BAK" "$CONF"; }
trap cleanup EXIT INT TERM
cp "$CONF" "$BAK"; sed -i "s/^cubrid_port_id=.*/cubrid_port_id=$PORT/" "$CONF"
cubrid createdb --db-volume-size=64M "$DB" en_US >/dev/null || exit 2
cubrid server start "$DB" >/dev/null || exit 2
# ---- REPRO: 여기부터 시나리오. 결과는 $OUT 에 'PASS'/'FAIL <이유>' 한 줄로 남긴다 ----
csql -u dba "$DB" -c "select 1" > "$OUT.q" 2>&1 && echo PASS > "$OUT" || echo "FAIL csql" > "$OUT"
# ---- REPRO 끝 ----
cat "$OUT"
