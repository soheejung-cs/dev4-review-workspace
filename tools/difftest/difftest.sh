#!/bin/bash
# SQL 차분 테스트 러너 — 같은 .sql 세트를 여러 설치본(SA 모드)에 돌려 정규화한 출력을 diff 한다.
#   bash difftest.sh -o <outdir> -s <suite.sql> [-s ...] [-d <db>] [-v <volume-install>] <tag>=<install> [<tag>=<install> ...]
#   예) bash difftest.sh -o out -s fz_iso.sql dev=~/release/CUBRID-goto-07aec2c5c pr=~/release/CUBRID-goto-cb0514bb7 mg=~/release/CUBRID-goto-3004fca56
# -v 를 주면 모든 설치본이 그 설치본의 databases/ (같은 볼륨) 를 쓴다 — 비용·플랜 비교처럼 통계·페이지 수가 같아야 할 때.
# 서버가 떠 있으면 SA(-S) 와 충돌하므로 먼저 멈춘다(활성 설치본 기준 `cubrid service stop`).
set -u
OUT=out; SUITES=(); DB=demodb; VOL=""
while getopts "o:s:d:v:" o; do case $o in o) OUT=$OPTARG;; s) SUITES+=("$OPTARG");; d) DB=$OPTARG;; v) VOL=$OPTARG;; esac; done; shift $((OPTIND-1))
[ ${#SUITES[@]} -gt 0 ] && [ $# -ge 2 ] || { sed -n 2,6p "$0"; exit 2; }
mkdir -p "$OUT"; cd "$OUT" || exit 2    # csql 은 cwd 에 csql.err 를 남긴다 — 리포 디렉터리에서 돌리지 않는다
if pgrep -x cub_server >/dev/null; then echo "cub_server 가 떠 있다 — SA 모드와 충돌. 먼저 멈춰라: $(for p in $(pgrep -x cub_server); do readlink /proc/$p/exe; done)"; exit 1; fi
norm() { grep -v '^$\|^Execute OK\|rows* selected\|Committed\|There are no results\|Current transaction\|Deprecated parameter' | sed -E 's/\([0-9.]+ sec\)//g; s/B\+tree: [0-9|]+|CLASS_OID: [0-9|]+|OID: [0-9|]+/OID/g'; }
TAGS=()
for kv in "$@"; do tag=${kv%%=*}; D=${kv#*=}; D=${D/#\~/$HOME}; TAGS+=("$tag")
  [ -x "$D/bin/csql" ] || { echo "$tag: $D/bin/csql 없음"; exit 2; }
  DBS=${VOL:+${VOL/#\~/$HOME}/databases}; DBS=${DBS:-$D/databases}
  for s in "${SUITES[@]}"; do b=$(basename "${s%.sql}")
    ( export CUBRID=$D CUBRID_DATABASES=$DBS PATH=$D/bin:$PATH LD_LIBRARY_PATH=$D/lib; timeout 900 "$D/bin/csql" -S -u dba "$DB" -i "$s" 2>&1 | norm > "${b}_${tag}.txt" )
    echo "$tag/$b: $(head -1 $D/BUILD_REV | cut -c1-20) errors=$(grep -c ERROR "${b}_${tag}.txt") lines=$(wc -l < "${b}_${tag}.txt")"
  done
done
base=${TAGS[0]}
for s in "${SUITES[@]}"; do b=$(basename "${s%.sql}")
  for t in "${TAGS[@]:1}"; do n=$(diff "${b}_${base}.txt" "${b}_${t}.txt" | grep -c '^[<>]'); echo "== $b: $base vs $t — diff $n 줄 ($([ $n = 0 ] && echo 같음 || echo "${b}_${base}-${t}.diff"))"; [ $n = 0 ] || diff "${b}_${base}.txt" "${b}_${t}.txt" > "${b}_${base}-${t}.diff"; done
done
rm -f csql.err
