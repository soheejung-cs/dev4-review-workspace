#!/bin/bash
# run_one.sh <label> <jepsen test options...> — one Jepsen run against the active install; appends
#   "label | rc | valid | anomalies | ok/fail/info | checker errors | results" to $JEPSEN_CUBRID_OUT/summary.txt
# Do not run beside CTP, shell TCs, goto builds or benchmarks (they stop servers and share cub_master — H-05).
label=$1; shift
cd "$(dirname "$0")" && . ./env.sh
[ -e store ] || { echo "run ./setup_env.sh first"; exit 2; }
mkdir -p "$JEPSEN_CUBRID_OUT/logs"
broker_was_up=$(cubrid broker status 2>&1 | grep -q "is not running" && echo no || echo yes)
timeout 1800 lein run test --no-ssh --nodes localhost "$@" > "$JEPSEN_CUBRID_OUT/logs/$label.log" 2>&1; rc=$?
[ "$broker_was_up" = no ] && cubrid broker stop > /dev/null 2>&1      # leave the broker as we found it
res=$(readlink -f store/latest)/results.edn
python3 - "$label" "$rc" "$res" "$*" <<'PY' >> "$JEPSEN_CUBRID_OUT/summary.txt"
import re, sys
label, rc, res, args = sys.argv[1:5]
try:
    s = open(res).read()
except OSError:
    print("%s | rc=%s | no results (%s) | %s" % (label, rc, res, args)); sys.exit()
top = re.findall(r':valid\? (true|false|:unknown)', s)
anom = re.findall(r':anomaly-types \(([^)]*)\)', s)
st = re.search(r':ok-count (\d+),\s*:fail-count (\d+),\s*:info-count (\d+)', s)
errs = re.findall(r':(wrong-total|negative-value|lost-count|unexpected-count) (\d+)', s)
print("%s | rc=%s | valid=%s | anomalies=%s | ok/fail/info=%s | %s | %s"
      % (label, rc, top[-1] if top else '?', anom, '/'.join(st.groups()) if st else '?', errs[:4], res))
PY
tail -1 "$JEPSEN_CUBRID_OUT/summary.txt"
