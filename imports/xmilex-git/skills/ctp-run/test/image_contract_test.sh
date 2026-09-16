#!/usr/bin/env bash
# Scope migration regressions; fixtures only, no CTP or container execution.
set -eo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SKILL="$(cd "$HERE/.." && pwd)"
REPO="$(git -C "$SKILL" rev-parse --show-toplevel)"
mkdir -p "$REPO/.git_ignored_dir/scratch/ctp-image-tests"
SCRATCH="$(mktemp -d "$REPO/.git_ignored_dir/scratch/ctp-image-tests/run.XXXXXX")"
export TMPDIR="$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT
PASS=0
ok() { PASS=$((PASS+1)); printf '[PASS] %s\n' "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

# Load upstream conf functions without entering its command dispatch.
sed '/^case "$1" in$/,$d' "$SKILL/scripts/entrypoint.sh" > "$SCRATCH/entry-functions.sh"
mkdir -p "$SCRATCH/CTP/conf" "$SCRATCH/scenario & a|b" "$SCRATCH/default"
cat > "$SCRATCH/CTP/conf/sql.conf" <<EOF
[sql]
scenario=$SCRATCH/default
testcase_exclude_from_file=$SCRATCH/default-exclude
EOF
cp "$SCRATCH/CTP/conf/sql.conf" "$SCRATCH/stock.conf"
: > "$SCRATCH/default-exclude"
: > "$SCRATCH/exclude & a|b.txt"

compose() {
  env CTP_HOME="$SCRATCH/CTP" TEST_SUITE=sql "$@" bash -e -c '
    . "$1"
    resolve_category
    check_scope
    "$CONF_WRITER"
    pin_testcase_source
    apply_scope
  ' _ "$SCRATCH/entry-functions.sh"
}

compose TEST_SCENARIO="$SCRATCH/scenario & a|b" TEST_EXCLUDE="$SCRATCH/exclude & a|b.txt"
grep -qxF "scenario=$SCRATCH/scenario & a|b" "$SCRATCH/CTP/conf/sql_runtime.conf"
grep -qxF "testcase_exclude_from_file=$SCRATCH/exclude & a|b.txt" "$SCRATCH/CTP/conf/sql_runtime.conf"
cmp "$SCRATCH/stock.conf" "$SCRATCH/CTP/conf/sql.conf"
ok 'scope paths survive sed metacharacters; shipped SQL conf is unchanged'

compose TEST_EXCLUDE=
grep -qxF 'testcase_exclude_from_file=' "$SCRATCH/CTP/conf/sql_runtime.conf"
compose
cmp "$SCRATCH/stock.conf" "$SCRATCH/CTP/conf/sql_runtime.conf"
ok 'empty excludes nothing; an unset second run restores stock scope and exclusions'

for old in SHELL_SCENARIO HA_SCENARIO MEMORY_SCENARIO; do
  if compose "$old=" > "$SCRATCH/reject.log" 2>&1; then fail "$old was accepted"; fi
  grep -qF "$old is gone; use TEST_SCENARIO" "$SCRATCH/reject.log"
done
if compose TEST_EXCLUDE="$SCRATCH/missing" > "$SCRATCH/reject.log" 2>&1; then
  fail 'missing exclusion file was accepted'
fi
grep -qF 'not an absolute path to a file' "$SCRATCH/reject.log"
ok 'retired names (including empty) and missing exclusion files fail before execution'

cat > "$SCRATCH/CTP/conf/shell_ci.conf" <<EOF
scenario=$SCRATCH/default
testcase_exclude_from_file=$SCRATCH/default-exclude
testcase_update_yn=true
testcase_git_branch=develop
EOF
env CTP_HOME="$SCRATCH/CTP" TEST_SUITE=shell bash -e -c '
  . "$1"
  resolve_category
  "$CONF_WRITER"
  pin_testcase_source
' _ "$SCRATCH/entry-functions.sh"
grep -qxF 'testcase_update_yn=false' "$SCRATCH/CTP/conf/shell_runtime.conf"
grep -qxF 'testcase_update_yn=true' "$SCRATCH/CTP/conf/shell_ci.conf"
ok 'shell source pinning changes only the runtime conf'

# Exercise the host planner, not just the environment it prints. In particular,
# empty/custom exclusions must restore cases removed by the default list.
mkdir -p "$SCRATCH/tc/sql/_x/cases" "$SCRATCH/tc/sql/_y/cases"
touch "$SCRATCH/tc/sql/_x/cases/a.sql" "$SCRATCH/tc/sql/_x/cases/b.sql" "$SCRATCH/tc/sql/_y/cases/c.sql"
printf '%s\n' '_x/cases/b.sql' > "$SCRATCH/CTP/conf/exclusions.txt"
printf '%s\n' '_x/cases/a.sql' > "$SCRATCH/custom.txt"
plan() {
  local name="$1"; shift
  bash "$SKILL/scripts/ctp_run.sh" --dry-run --testcases "$SCRATCH/tc" --testcases-as-is \
    --ctp "$SCRATCH/CTP" --shards 1 --no-weights --no-colocate --out "$SCRATCH/$name" "$@" \
    > "$SCRATCH/$name.log" 2>&1
}
plan default --only _x
grep -qxF '_x/cases/a.sql' "$SCRATCH/default/shard_0/assigned_cases.txt"
[ "$(wc -l < "$SCRATCH/default/shard_0/assigned_cases.txt")" -eq 1 ]
plan empty --only _x --exclude ''
[ "$(wc -l < "$SCRATCH/empty/shard_0/assigned_cases.txt")" -eq 2 ]
plan custom --only _x --exclude "$SCRATCH/custom.txt"
grep -qxF '_x/cases/b.sql' "$SCRATCH/custom/shard_0/assigned_cases.txt"
[ "$(wc -l < "$SCRATCH/custom/shard_0/assigned_cases.txt")" -eq 1 ]
cmp "$SCRATCH/custom.txt" "$SCRATCH/custom/shard_0/exclusions.txt"
ok 'host subset and unset/empty/custom exclusions agree with the materialized plan'

mkdir -p "$SCRATCH/tc/shell/_x/cases" "$SCRATCH/tc/shell/config"
touch "$SCRATCH/tc/shell/_x/cases/a.sh" "$SCRATCH/tc/shell/_x/cases/b.sh"
printf '%s\n' 'shell/_x/cases/a.sh' > "$SCRATCH/shell-exclude.txt"
plan shell --suite shell --exclude "$SCRATCH/shell-exclude.txt"
grep -qxF '_x/cases/b.sh' "$SCRATCH/shell/shard_0/assigned_cases.txt"
[ "$(wc -l < "$SCRATCH/shell/shard_0/assigned_cases.txt")" -eq 1 ]
ok 'shell exclusions match individual .sh paths, using the category prefix'

for name in CTP_SCENARIO CTP_EXCLUDE SHELL_SCENARIO HA_SCENARIO MEMORY_SCENARIO TEST_SCENARIO TEST_EXCLUDE; do
  if plan "reject-$name" --env "$name="; then fail "--env $name bypassed planning"; fi
  grep -qE 'is retired|is managed by the runner' "$SCRATCH/reject-$name.log"
done
if plan missing --exclude "$SCRATCH/missing"; then fail 'host accepted missing exclusion file'; fi
grep -qF -- '--exclude file unreadable' "$SCRATCH/missing.log"
ok 'scope cannot bypass planning through --env; missing host exclusion files fail'

echo "Image contract: $PASS checks passed"
