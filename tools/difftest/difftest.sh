#!/bin/bash
# SQL 차분 테스트 러너 — 같은 .sql 세트를 여러 설치본(SA 모드)에 돌려 정규화한 출력을 diff 한다.
#   bash difftest.sh -o <outdir> -s <suite.sql> [-s ...] [-d <db>] [-v <volume-install> | -D <databases-dir>] [-c <conf-dir>] <tag>=<install> [<tag>=<install> ...]
#   예) bash difftest.sh -o out -s fz_iso.sql dev=~/release/CUBRID-goto-07aec2c5c pr=~/release/CUBRID-goto-cb0514bb7 mg=~/release/CUBRID-goto-3004fca56
# -v 를 주면 모든 설치본이 그 설치본의 databases/ (같은 볼륨) 를 쓴다 — 비용·플랜 비교처럼 통계·페이지 수가 같아야 할 때.
# -D 는 databases.txt 가 있는 디렉터리를 직접 준다(스크래치에 만든 버릴 DB). -c 는 <conf-dir>/<tag>.conf 가 있으면 그 설치본을
#   CUBRID_CONF_FILE=<그 파일> 로 띄운다(설치본 conf 무편집 — modes.sh 가 쓴다, H-39).
# 같은 DB 를 서비스 중인 cub_server 가 있으면 SA(-S) 와 충돌하므로 멈추고 끝낸다(다른 DB 를 서비스 중인 서버는 상관없다).
set -u
OUT=out; SUITES=(); DB=demodb; VOL=""; DBDIR=""; CONFDIR=""
while getopts "o:s:d:v:D:c:" o; do case $o in o) OUT=$OPTARG;; s) SUITES+=("$OPTARG");; d) DB=$OPTARG;; v) VOL=$OPTARG;; D) DBDIR=$OPTARG;; c) CONFDIR=$OPTARG;; esac; done; shift $((OPTIND-1))
[ ${#SUITES[@]} -gt 0 ] && [ $# -ge 2 ] || { sed -n 2,8p "$0"; exit 2; }
abspath() { case $1 in /*) echo "$1";; *) echo "$PWD/$1";; esac; }
for i in "${!SUITES[@]}"; do SUITES[$i]=$(abspath "${SUITES[$i]/#\~/$HOME}"); done
[ -n "$CONFDIR" ] && CONFDIR=$(abspath "${CONFDIR/#\~/$HOME}")
[ -n "$DBDIR" ] && DBDIR=$(abspath "${DBDIR/#\~/$HOME}")
mkdir -p "$OUT"; cd "$OUT" || exit 2    # csql 은 cwd 에 csql.err 를 남긴다 — 리포 디렉터리에서 돌리지 않는다
# 서버 명령줄은 `cub_server <db>` — 같은 DB 를 서비스하는 서버만 SA 와 충돌한다. pgrep -f 는 자기 명령줄에 걸리므로 -x 로 찾고 /proc 로 본다
busy=""; for p in $(pgrep -x cub_server); do a=($(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)); [ "${a[1]:-}" = "$DB" ] && busy+=" $(readlink /proc/$p/exe)"; done
if [ -n "$busy" ]; then echo "cub_server 가 $DB 를 서비스 중 — SA 모드와 충돌. 그 서버를 멈추거나 -D 로 버릴 DB 를 써라:$busy"; exit 1; fi
norm() { grep -v '^$\|^Execute OK\|rows* selected\|Committed\|There are no results\|Current transaction\|Deprecated parameter' | sed -E 's/\([0-9.]+ sec\)//g; s/B\+tree: [0-9|]+|CLASS_OID: [0-9|]+|OID: [0-9|]+/OID/g'; }
TAGS=()
for kv in "$@"; do tag=${kv%%=*}; D=${kv#*=}; D=${D/#\~/$HOME}; TAGS+=("$tag")
  [ -x "$D/bin/csql" ] || { echo "$tag: $D/bin/csql 없음"; exit 2; }
  DBS=${VOL:+${VOL/#\~/$HOME}/databases}; DBS=${DBDIR:-${DBS:-$D/databases}}
  CF=""; [ -n "$CONFDIR" ] && [ -f "$CONFDIR/$tag.conf" ] && CF=$CONFDIR/$tag.conf
  for s in "${SUITES[@]}"; do b=$(basename "${s%.sql}")
    ( export CUBRID=$D CUBRID_DATABASES=$DBS PATH=$D/bin:$PATH LD_LIBRARY_PATH=$D/lib; unset CUBRID_CONF_FILE; [ -n "$CF" ] && export CUBRID_CONF_FILE=$CF
      timeout 900 "$D/bin/csql" -S -u dba "$DB" -i "$s" 2>&1 | norm > "${b}_${tag}.txt" )
    echo "$tag/$b: $(head -1 $D/BUILD_REV | cut -c1-20) errors=$(grep -c ERROR "${b}_${tag}.txt") lines=$(wc -l < "${b}_${tag}.txt")${CF:+ conf=$CF}"
  done
done
base=${TAGS[0]}
for s in "${SUITES[@]}"; do b=$(basename "${s%.sql}")
  for t in "${TAGS[@]:1}"; do n=$(diff "${b}_${base}.txt" "${b}_${t}.txt" | grep -c '^[<>]'); echo "== $b: $base vs $t — diff $n 줄 ($([ $n = 0 ] && echo 같음 || echo "${b}_${base}-${t}.diff"))"; [ $n = 0 ] || diff "${b}_${base}.txt" "${b}_${t}.txt" > "${b}_${base}-${t}.diff"; done
done
rm -f csql.err
