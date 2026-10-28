#!/bin/bash
# 시스템 파라미터 모드 묶음으로 같은 세트를 돌린다 — 설치본 conf 는 건드리지 않는다(H-39).
#   bash modes.sh -o <outdir> -s <suite.sql> -m "<이름>:<k=v>,<k=v>" [-m ...] [-d <db>] [-v <volume-install> | -D <databases-dir>] <tag>=<install> ...
#   예) -m "oracle:oracle_style_empty_string=yes,return_null_on_function_errors=yes,oracle_compat_number_behavior=yes" \
#       -m "concat:plus_as_concat=no,pipes_as_concat=no,no_backslash_escapes=no,unicode_input_normalization=yes,string_max_size_bytes=64" \
#       -m "nocache:max_plan_cache_entries=0"  -m "noclone:max_plan_cache_clones=0"
# 모드마다 <outdir>/<모드>/conf/<tag>.conf = 그 설치본 conf/cubrid.conf 사본 + 모드 줄을 쓰고, difftest.sh -c 가 그 설치본의 csql 을
# CUBRID_CONF_FILE=<사본> 으로 띄운다(system_parameter.c 가 환경변수 CUBRID_CONF_FILE 을 conf 경로로 읽는다 — 배운것 PR#8196 16번).
# 설치본 conf 를 고쳤다 되돌리지 않으므로 중간에 죽어도 잔재가 없고, 같은 설치본을 쓰는 JOB·CTP·다른 modes.sh 와 부딪히지 않는다.
# 적용 확인: 모드마다 `;get <키>` 를 기본 conf 와 모드 conf 로 한 번씩 돌려 <outdir>/<모드>/conf/<tag>.get.txt 에 남기고,
#   값이 기본과 같으면 "⚠ 안 바뀜"(이미 그 값이었거나 키가 틀림)을 찍는다. 값의 표기는 엔진이 바꾼다(yes→y, max_plan_cache_entries=0→-1).
set -u
OUT=out; SUITES=(); MODES=("default:"); PASS=()
while getopts "o:s:m:d:v:D:" o; do case $o in o) OUT=$OPTARG;; s) SUITES+=(-s "$OPTARG");; m) MODES+=("$OPTARG");; d|v|D) PASS+=(-$o "$OPTARG");; esac; done; shift $((OPTIND-1))
[ ${#SUITES[@]} -gt 0 ] && [ $# -ge 1 ] || { sed -n 2,6p "$0"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
case $OUT in /*) ;; *) OUT=$PWD/$OUT;; esac
DB=demodb; DBDIR=""; VOL=""; for ((i=0; i<${#PASS[@]}; i+=2)); do case ${PASS[$i]} in -d) DB=${PASS[$((i+1))]};; -D) DBDIR=${PASS[$((i+1))]/#\~/$HOME};; -v) VOL=${PASS[$((i+1))]/#\~/$HOME};; esac; done
getvals() {   # getvals <install> <conf|""> <keys...> → "k=v" 줄들 (SA ;get)
  local D=$1 CF=$2; shift 2; local DBS=${DBDIR:-${VOL:+$VOL/databases}}; DBS=${DBS:-$D/databases}
  local q; q=$(mktemp -p "$GETTMP" get.XXXX.sql); for k in "$@"; do echo ";get $k"; done > "$q"
  ( cd "$GETTMP" && export CUBRID=$D CUBRID_DATABASES=$DBS PATH=$D/bin:$PATH LD_LIBRARY_PATH=$D/lib && unset CUBRID_CONF_FILE && { [ -z "$CF" ] || export CUBRID_CONF_FILE=$CF; } \
    && timeout 120 "$D/bin/csql" -S -u dba "$DB" -i "$q" 2>&1 | grep -E '^[a-z_]+=' ); rm -f "$q"
}
busy=""; for p in $(pgrep -x cub_server); do a=($(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)); [ "${a[1]:-}" = "$DB" ] && busy+=" $(readlink /proc/$p/exe)"; done
[ -z "$busy" ] || { echo "cub_server 가 $DB 를 서비스 중 — SA 와 충돌. 그 서버를 멈추거나 -D 로 버릴 DB 를 써라:$busy"; exit 1; }
mkdir -p "$OUT"; GETTMP=$(mktemp -d -p "$OUT" .get.XXXX)
for m in "${MODES[@]}"; do name=${m%%:*}; kvs=${m#*:}; CD=$OUT/$name/conf; mkdir -p "$CD"
  keys=(); [ -n "$kvs" ] && IFS=',' read -r -a pairs <<< "$kvs" && for kv in "${pairs[@]}"; do keys+=("${kv%%=*}"); done
  for kv in "$@"; do tag=${kv%%=*}; D=${kv#*=}; D=${D/#\~/$HOME}
    cp "$D/conf/cubrid.conf" "$CD/$tag.conf"
    [ -n "$kvs" ] && { printf '\n# modes.sh mode[%s]\n' "$name"; echo "$kvs" | tr ',' '\n'; } >> "$CD/$tag.conf"
    if [ ${#keys[@]} -gt 0 ]; then
      getvals "$D" "" "${keys[@]}" > "$CD/$tag.get.default.txt"; getvals "$D" "$CD/$tag.conf" "${keys[@]}" > "$CD/$tag.get.txt"
      for k in "${keys[@]}"; do a=$(grep "^$k=" "$CD/$tag.get.default.txt"); b=$(grep "^$k=" "$CD/$tag.get.txt")
        if [ -z "$b" ]; then echo "⚠ mode[$name] $tag: ;get $k 가 값을 못 읽음 — 키 이름 확인"; elif [ "$a" = "$b" ]; then echo "⚠ mode[$name] $tag: $b — 기본과 같음(이미 그 값이었거나 적용 안 됨)"; fi
      done
      echo "mode[$name] $tag ;get: $(tr '\n' ' ' < "$CD/$tag.get.txt")"
    fi
  done
  echo "######## mode[$name] $kvs"; bash "$HERE/difftest.sh" -o "$OUT/$name" -c "$CD" "${PASS[@]}" "${SUITES[@]}" "$@"
done
rm -rf "$GETTMP"
