#!/usr/bin/env bash
# Exercise real shard construction, then the locale deletion CTP performs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(git -C "$HERE" rev-parse --show-toplevel)"
mkdir -p "$REPO/.git_ignored_dir/scratch/ctp-locale-tests"
SCRATCH="$(mktemp -d "$REPO/.git_ignored_dir/scratch/ctp-locale-tests/run.XXXXXX")"
export TMPDIR="$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT
sed '/^main "\$@"$/,$d' "$HERE/../scripts/ctp_run.sh" > "$SCRATCH/functions.sh"

# Source definitions only; no CTP process or container is started.
# shellcheck disable=SC1090
. "$SCRATCH/functions.sh"
PASS=0
ok() { PASS=$((PASS+1)); printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

stage() {
  local name="$1" suite="$2" preset="$3" overlay="${4:-0}"
  local i=0 # Match main's dynamically scoped shard-loop index.
  ARG_BUILD="$SCRATCH/$name/source"
  OUT="$SCRATCH/$name/out"
  WORK="$SCRATCH/$name/work"
  ARG_CTP="$SCRATCH/$name/CTP"
  SCN="$SCRATCH/$name/scenario"
  ARG_SUITE="$suite"; ARG_OVERLAY="$overlay"; ARG_CONF=""
  LOCALE_SO=""; LOCALE_SCRIPT=""; SUITE_HA=0
  case "$suite" in
    sql|medium) SUITE_STYLE=sqlresult; SUITE_EXT=sql; SUITE_SUBPATH="$suite" ;;
    shell) SUITE_STYLE=status; SUITE_EXT=sh; SUITE_SUBPATH=shell ;;
    ha_shell) SUITE_STYLE=status; SUITE_EXT=sh; SUITE_SUBPATH=HA/shell; SUITE_HA=1 ;;
  esac
  mkdir -p "$ARG_BUILD/conf" "$ARG_BUILD/lib" "$ARG_CTP/conf" "$SCN/one/cases" "$WORK"
  printf '# generated all-locales preset\nfr_FR\nde_DE\n' > "$ARG_BUILD/conf/cubrid_locales.all.txt"
  cp "$ARG_BUILD/conf/cubrid_locales.all.txt" "$ARG_BUILD/conf/cubrid_locales.txt"
  [ "$preset" = all ] || printf '# custom locale\nfr_FR /custom/fr.xml /custom/fr.so\n' > "$ARG_BUILD/conf/cubrid_locales.txt"
  cp "$ARG_BUILD/conf/cubrid_locales.txt" "$SCRATCH/$name/original.txt"
  : > "$ARG_BUILD/lib/libcubrid_all_locales.so"
  : > "$SCN/one/cases/a.$SUITE_EXT"
  printf 'one/cases/a.%s\n' "$SUITE_EXT" > "$WORK/shard_0.sql.txt"
  : > "$WORK/shard_0.exclusions.txt"
  build_shard_workdir 0
}

for suite in shell ha_shell; do
  stage "$suite" "$suite" all
  # Mimic resetCUBRID_linux before a case that does not call do_make_locale.
  rm "$OUT/shard_0/CUBRID/lib/libcubrid_all_locales.so"
  if grep -qEv '^[[:space:]]*(#|$)' "$OUT/shard_0/CUBRID/conf/cubrid_locales.txt"; then
    fail "$suite: staged conf still requires locale libraries deleted by CTP"
  fi
  cmp "$ARG_BUILD/conf/cubrid_locales.txt" "$SCRATCH/$suite/original.txt"
  if [ "$suite" = ha_shell ]; then
    cmp "$OUT/shard_0/CUBRID/conf/cubrid_locales.txt" "$OUT/shard_0/CUBRID.slave/conf/cubrid_locales.txt"
  fi
  ok "$suite: staged locale preset survives CTP reset; source install unchanged"
done

for suite in sql medium; do
  stage "$suite" "$suite" all
  cmp "$OUT/shard_0/CUBRID/conf/cubrid_locales.txt" "$SCRATCH/$suite/original.txt"
  ok "$suite: all-locales preset preserved"
done

stage custom shell custom
cmp "$OUT/shard_0/CUBRID/conf/cubrid_locales.txt" "$SCRATCH/custom/original.txt"
ok 'custom locale configuration is not silently rewritten'

# An overlay cannot be normalized by writing the shared lower install.
if ( set -e; stage overlay shell all 1 ) > "$SCRATCH/overlay.log" 2>&1; then
  fail 'overlay accepted an inherited all-locales preset without a private reset'
fi
grep -qF 'omit --overlay' "$SCRATCH/overlay.log"
cmp "$SCRATCH/overlay/source/conf/cubrid_locales.txt" "$SCRATCH/overlay/original.txt"
ok 'overlay requests a writable copy instead of modifying the source install'
echo "Locale staging: $PASS checks passed"
