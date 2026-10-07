#!/bin/bash
# 시스템 파라미터 모드 묶음으로 같은 세트를 돌린다 — cubrid.conf 에 줄을 덧붙였다가 끝나면 원복한다(설치본마다 conf 가 따로다).
#   bash modes.sh -o <outdir> -s <suite.sql> -m "<이름>:<k=v>,<k=v>" [-m ...] <tag>=<install> ...
#   예) -m "oracle:oracle_style_empty_string=yes,return_null_on_function_errors=yes,oracle_compat_number_behavior=yes" \
#       -m "concat:plus_as_concat=no,pipes_as_concat=no,no_backslash_escapes=no,unicode_input_normalization=yes,string_max_size_bytes=64" \
#       -m "nocache:max_plan_cache_entries=0"  -m "noclone:max_plan_cache_clones=0"
# 모드가 실제로 먹었는지는 출력으로 확인한다('' 가 NULL 로 나오나, '1'+'2' 가 3 인가). 상위 호출마다 difftest.sh 를 부른다.
set -u
OUT=out; SUITES=(); MODES=("default:")
while getopts "o:s:m:" o; do case $o in o) OUT=$OPTARG;; s) SUITES+=(-s "$OPTARG");; m) MODES+=("$OPTARG");; esac; done; shift $((OPTIND-1))
HERE=$(cd "$(dirname "$0")" && pwd)
for m in "${MODES[@]}"; do name=${m%%:*}; kvs=${m#*:}
  for kv in "$@"; do D=${kv#*=}; D=${D/#\~/$HOME}; cp "$D/conf/cubrid.conf" "$D/conf/cubrid.conf.difftest_bak"; [ -n "$kvs" ] && echo "$kvs" | tr ',' '\n' >> "$D/conf/cubrid.conf"; done
  echo "######## mode[$name] $kvs"; bash "$HERE/difftest.sh" -o "$OUT/$name" "${SUITES[@]}" "$@"
  for kv in "$@"; do D=${kv#*=}; D=${D/#\~/$HOME}; mv "$D/conf/cubrid.conf.difftest_bak" "$D/conf/cubrid.conf"; done
done
