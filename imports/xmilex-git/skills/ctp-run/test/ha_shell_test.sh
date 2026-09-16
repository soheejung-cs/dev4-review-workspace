#!/usr/bin/env bash

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SKILL="$(cd "$HERE/.." && pwd)"
ENTRY="$SKILL/scripts/entrypoint.sh"
REPO="$(git -C "$SKILL" rev-parse --show-toplevel)"
SCRATCH_BASE="$REPO/.git_ignored_dir/scratch/wf220-recipe/selftest"

mkdir -p "$SCRATCH_BASE"
SCRATCH="$(mktemp -d "$SCRATCH_BASE/run.XXXXXX")"
export TMPDIR="$SCRATCH" TMP="$SCRATCH" TEMP="$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

PASS=0
FAIL=0
ok () { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$*"; }
bad () { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$*" >&2; }

extract_function ()
{
  local name="$1" output="$2"
  awk -v signature="function $name ()" '
    index($0, signature) == 1 { emit=1 }
    emit {
      print
      line=$0
      opens=gsub(/\{/, "{", line)
      line=$0
      closes=gsub(/\}/, "}", line)
      depth += opens - closes
      if (opens > 0) started=1
      if (started && depth == 0) exit
    }
  ' "$ENTRY" > "$output"
  [ -s "$output" ]
}

FUNCS="$SCRATCH/functions.sh"
: > "$FUNCS"
for fn in configure_ha_csql_port prepare_ha_shell_broker; do
  part="$SCRATCH/$fn.sh"
  if ! extract_function "$fn" "$part"; then
    bad "$fn exists in entrypoint.sh"
    printf 'RESULT: %d PASS / %d FAIL\n' "$PASS" "$FAIL"
    exit 1
  fi
  cat "$part" >> "$FUNCS"
done
# shellcheck disable=SC1090
. "$FUNCS"

make_ctp ()
{
  local root="$1" port_text="$2" anchors="${3:-1}"
  mkdir -p "$root/conf" "$root/shell/init_path"
  printf '%s\n' "$port_text" > "$root/conf/shell_ci.conf"
  : > "$root/shell/init_path/make_ha_upper.sh"
  local i
  for i in $(seq 1 "$anchors"); do
    printf '%s\n' 'run_on_slave -c "cubrid hb start;cubrid hb status"' \
      >> "$root/shell/init_path/make_ha_upper.sh"
  done
}

run_config ()
{
  local suite="$1" root="$2"
  TEST_SUITE="$suite" CTP_HOME="$root" CUBRID_CSQL_BROKER_PORT= \
    bash -c '. "$1"; configure_ha_csql_port >/dev/null || exit $?; printf "%s" "${CUBRID_CSQL_BROKER_PORT-}"' _ "$FUNCS"
}

VALID="$SCRATCH/valid"
make_ctp "$VALID" 'default.broker2.BROKER_PORT=24567'
if got="$(run_config ha_shell "$VALID")" && [ "$got" = 24567 ]; then
  ok "ha_shell derives and exports a non-default broker2 port"
else
  bad "ha_shell broker2 derivation/export (got '$got')"
fi

for spec in \
  'missing|default.broker1.BROKER_PORT=24567' \
  'duplicate|default.broker2.BROKER_PORT=24567
default.broker2.BROKER_PORT=24568' \
  'nonnumeric|default.broker2.BROKER_PORT=abc' \
  'zero|default.broker2.BROKER_PORT=0' \
  'high|default.broker2.BROKER_PORT=65536'; do
  name="${spec%%|*}"
  value="${spec#*|}"
  root="$SCRATCH/$name"
  make_ctp "$root" "$value"
  if run_config ha_shell "$root" > "$SCRATCH/$name.out" 2>&1; then
    bad "$name broker2 port is rejected"
  else
    ok "$name broker2 port is rejected"
  fi
done

NONHA="$SCRATCH/nonha"
make_ctp "$NONHA" 'default.broker2.BROKER_PORT=24567'
if got="$(run_config shell "$NONHA")" && [ -z "$got" ]; then
  ok "non-ha_shell suite leaves the folded-csql port unset"
else
  bad "non-ha_shell suite changed folded-csql port (got '$got')"
fi

run_prepare ()
{
  local suite="$1" root="$2" install="$3"
  TEST_SUITE="$suite" CTP_HOME="$root" CUBRID="$install" \
    bash -c '. "$1"; prepare_ha_shell_broker' _ "$FUNCS"
}

FOLDED="$SCRATCH/folded-install"
mkdir -p "$FOLDED/lib"
printf 'binary payload CUBRID_CSQL_BROKER_PORT payload\n' > "$FOLDED/lib/libcubridcs.so"

HELPER="$VALID/shell/init_path/make_ha_upper.sh"
if run_prepare ha_shell "$VALID" "$FOLDED"; then
  marker_line="$(grep -nF '# ctp-run: thin csql needs the slave broker' "$HELPER" | cut -d: -f1)"
  anchor_line="$(grep -nF 'run_on_slave -c "cubrid hb start;cubrid hb status"' "$HELPER" | cut -d: -f1)"
  start_line="$(grep -nF 'run_on_slave -c "cubrid broker start"' "$HELPER" | cut -d: -f1)"
  if [ "$marker_line" -eq $((anchor_line + 1)) ] && [ "$start_line" -eq $((anchor_line + 2)) ]; then
    ok "slave broker start is inserted directly after slave heartbeat"
  else
    bad "slave broker insertion order is wrong"
  fi
else
  bad "folded ha_shell helper patch succeeds"
fi

before="$(sha256sum "$HELPER")"
if run_prepare ha_shell "$VALID" "$FOLDED" && [ "$(sha256sum "$HELPER")" = "$before" ] \
   && [ "$(grep -cF '# ctp-run: thin csql needs the slave broker' "$HELPER")" -eq 1 ]; then
  ok "slave broker helper patch is idempotent"
else
  bad "slave broker helper patch is not idempotent"
fi

for spec in 'noanchor|0' 'twoanchors|2'; do
  name="${spec%%|*}"; anchors="${spec##*|}"
  root="$SCRATCH/$name"
  make_ctp "$root" 'default.broker2.BROKER_PORT=24567' "$anchors"
  if run_prepare ha_shell "$root" "$FOLDED" > "$SCRATCH/$name.prepare.out" 2>&1; then
    bad "$name helper anchor is rejected"
  else
    ok "$name helper anchor is rejected"
  fi
done

LEGACY="$SCRATCH/legacy-install"
mkdir -p "$LEGACY/lib"
printf 'legacy binary without folded-csql token\n' > "$LEGACY/lib/libcubridcs.so"
LEGACY_CTP="$SCRATCH/legacy-ctp"
make_ctp "$LEGACY_CTP" 'default.broker2.BROKER_PORT=24567'
legacy_helper="$LEGACY_CTP/shell/init_path/make_ha_upper.sh"
legacy_before="$(sha256sum "$legacy_helper")"
if run_prepare ha_shell "$LEGACY_CTP" "$LEGACY" \
   && [ "$(sha256sum "$legacy_helper")" = "$legacy_before" ]; then
  ok "legacy client library leaves HA helper unchanged"
else
  bad "legacy client library mutated HA helper"
fi

OTHER_CTP="$SCRATCH/other-ctp"
make_ctp "$OTHER_CTP" 'default.broker2.BROKER_PORT=24567'
other_helper="$OTHER_CTP/shell/init_path/make_ha_upper.sh"
other_before="$(sha256sum "$other_helper")"
if run_prepare shell "$OTHER_CTP" "$FOLDED" \
   && [ "$(sha256sum "$other_helper")" = "$other_before" ]; then
  ok "non-ha_shell suite leaves HA helper unchanged"
else
  bad "non-ha_shell suite mutated HA helper"
fi

printf 'RESULT: %d PASS / %d FAIL\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
