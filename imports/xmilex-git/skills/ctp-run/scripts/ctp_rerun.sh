#!/usr/bin/env bash
#
# ctp_rerun.sh — re-run, locally and in isolation, exactly the CTP cases that
# failed in CI.
#
# The normal loop is: push, let CI run /run all, then reproduce the few failures
# locally. Fetching that failed list by hand is where runs go wrong — the wrong
# testcase branch, the wrong suite, or the whole 17k-case suite re-run to observe
# nine failures. This takes the CI job/PR URL and does it.
#
# INPUT (any one of):
#   https://github.com/CUBRID/cubrid/pull/<N>                 every failed suite of that PR
#   https://app.circleci.com/.../jobs/<job>                   one CircleCI job (sql | medium)
#   https://circleci.com/gh/CUBRID/cubrid/<job>               same, short form
#   https://github.com/CUBRID/cubrid/actions/runs/<id>        one gha-ci test_shell run
#   --failed-list <file> --suite <s> [--pr <N>]               a list you supply
#
# WHERE THE LIST COMES FROM
#   CircleCI (sql, medium): the public API needs no token —
#     /api/v2/project/gh/CUBRID/cubrid/<job>/tests gives every case with its
#     result, and `name` is already the CTP-relative path.
#   GitHub Actions (shell): the run uploads NO artifacts; the complete
#     failed.list lives only on the self-hosted runner's shared storage. What is
#     reachable is the `collect` job's log, whose summary table names the failing
#     cases — capped at 50 rows upstream. Past that cap this reproduces the first
#     50 and says so; the rest need CI's own `/run rerun <run_id>`.
#
# GRANULARITY
#   CI reports failing CASES; this re-runs their test DIRECTORIES. CTP sql cases
#   in one directory share state (a later case reads what an earlier one created),
#   so re-running a single case out of its directory reports failures that are
#   artefacts of the missing prefix. Always 1 shard, for the same reason.
#
# Copyright (c) 2024 CUBRID test-infra. Apache-2.0.

set -euo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNNER="$SELF_DIR/ctp_run.sh"
GH_REPO="CUBRID/cubrid"

info() { printf '[ctp-rerun] %s\n' "$*"; }
warn() { printf '[ctp-rerun] WARN: %s\n' "$*" >&2; }
die()  { printf '[ctp-rerun] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'
  cat <<'EOF'

OPTIONS
  --build <install>      CUBRID install to test            (default $CUBRID or ~/CUBRID)
  --out <dir>            output root                       (default alongside the runner's)
  --failed-list <file>   skip CI entirely; one case path per line
  --suite <s>            required with --failed-list
  --pr <N>               testcase ref override (else derived from the CI input)
  --testcases-root <dir> where the testcase checkouts live (default $HOME)
  --dry-run              resolve the list and print the plan, launch nothing
  --                     everything after this is passed to ctp_run.sh verbatim
EOF
}

ARG_URL=""; ARG_BUILD=""; ARG_OUT=""; ARG_LIST=""; ARG_SUITE=""; ARG_PR=""
ARG_TCROOT="$HOME"; ARG_DRY=0; declare -a PASSTHRU=()
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --build) ARG_BUILD="${2:-}"; shift 2 ;;
    --out) ARG_OUT="${2:-}"; shift 2 ;;
    --failed-list) ARG_LIST="${2:-}"; shift 2 ;;
    --suite) ARG_SUITE="${2:-}"; shift 2 ;;
    --pr) ARG_PR="${2:-}"; shift 2 ;;
    --testcases-root) ARG_TCROOT="${2:-}"; shift 2 ;;
    --dry-run) ARG_DRY=1; shift ;;
    --) shift; PASSTHRU=( "$@" ); break ;;
    -*) die "unknown option: $1 (try --help)" ;;
    *) [ -z "$ARG_URL" ] || die "only one URL may be given"; ARG_URL="$1"; shift ;;
  esac
done

[ -x "$RUNNER" ] || die "runner not found/executable: $RUNNER"
ARG_BUILD="${ARG_BUILD:-${CUBRID:-$HOME/CUBRID}}"
ARG_OUT="${ARG_OUT:-$SELF_DIR/../../../../.git_ignored_dir/scratch/ctp-run-out}"

#####################################################################
# CI extraction
#####################################################################

# Echo "<suite>\t<case path>" lines for a CircleCI job. Unauthenticated: the
# project is public and both API versions answer without a token.
circle_failed() {
  local job="$1" url token page
  url="https://circleci.com/api/v2/project/gh/${GH_REPO}/${job}/tests"
  token=""
  while :; do
    page="$(curl -fsS "${url}${token:+?page-token=$token}")" || die "CircleCI API failed for job $job"
    printf '%s' "$page" | jq -r '.items[]? | select(.result != "success") | .name'
    token="$(printf '%s' "$page" | jq -r '.next_page_token // empty')"
    [ -n "$token" ] || break
  done
}

# Echo the PR number a CircleCI job ran for (pipeline branch is pull/<N>/head).
circle_pr() {
  local job="$1"
  curl -fsS "https://circleci.com/api/v1.1/project/github/${GH_REPO}/${job}" 2>/dev/null \
    | jq -r '.branch // empty' | sed -nE 's#^pull/([0-9]+)(/head)?$#\1#p'
}

# Echo the failing shell cases named in a gha-ci run's `collect` job log.
gha_failed() {
  local run="$1" job
  command -v gh >/dev/null 2>&1 || die "gh is required to read a GitHub Actions log"
  job="$(gh run view "$run" -R "$GH_REPO" --json jobs \
          --jq '.jobs[] | select(.name=="collect") | .databaseId' 2>/dev/null | head -1)"
  [ -n "$job" ] || die "run $run has no finished 'collect' job (still running, or the tests were skipped)"
  gh run view "$run" -R "$GH_REPO" --job "$job" --log 2>/dev/null \
    | grep -oP '\|\s*\d{2}\s*\|\s*`\K[^`]+' | LC_ALL=C sort -u
}

# Echo the tc/pr-<N> branch the collect job recorded for a gha-ci run.
gha_tc_branch() {
  local run="$1" job
  job="$(gh run view "$run" -R "$GH_REPO" --json jobs \
          --jq '.jobs[] | select(.name=="collect") | .databaseId' 2>/dev/null | head -1)"
  [ -n "$job" ] || return 0
  gh run view "$run" -R "$GH_REPO" --job "$job" --log 2>/dev/null \
    | grep -oP '\|\s*tc branch\s*\|\s*`?\K[^`|]+' | head -1 | tr -d ' '
}

#####################################################################
# Suite mapping. A CI case path starts with the scenario dir, which IS the suite
# key we need: sql/... -> sql, medium/... -> medium, shell/... -> shell,
# HA/shell/... -> ha_shell. The rest is the scenario-relative path.
#####################################################################
suite_of_case() {
  case "$1" in
    sql/*)      printf 'sql' ;;
    medium/*)   printf 'medium' ;;
    HA/shell/*) printf 'ha_shell' ;;
    shell/*)    printf 'shell' ;;
    *)          printf '' ;;
  esac
}
strip_suite() {
  case "$1" in
    HA/shell/*) printf '%s' "${1#HA/shell/}" ;;
    sql/*)      printf '%s' "${1#sql/}" ;;
    medium/*)   printf '%s' "${1#medium/}" ;;
    shell/*)    printf '%s' "${1#shell/}" ;;
    *)          printf '%s' "$1" ;;
  esac
}
tc_repo_of_suite() {
  case "$1" in
    sql|medium) printf 'cubrid-testcases' ;;
    shell)      printf 'cubrid-testcases-private-ex' ;;
    ha_shell)   printf 'cubrid-testcases-private' ;;
  esac
}

#####################################################################
# Resolve the input into WORK/<suite>.dirs files + PR number.
#####################################################################
WORK="$(mktemp -d "${TMPDIR:-$ARG_OUT}/.rerun.XXXXXX" 2>/dev/null || mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
RAW="$WORK/raw.txt"; : >"$RAW"
PR="$ARG_PR"; TCREF=""
CAP_NOTE=""

add_raw() {  # <suite-hint> ; reads case paths on stdin
  local hint="$1" line suite
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    line="${line#cubrid-testcases/}"; line="${line#cubrid-testcases-private-ex/}"
    line="${line#cubrid-testcases-private/}"
    suite="$(suite_of_case "$line")"
    [ -n "$suite" ] || suite="$hint"
    [ -n "$suite" ] || { warn "cannot tell which suite this case belongs to: $line"; continue; }
    printf '%s\t%s\n' "$suite" "$(strip_suite "$line")" >>"$RAW"
  done
}

from_circle_job() {
  local job="$1"
  info "CircleCI job $job: fetching test results (no token needed) ..."
  circle_failed "$job" | add_raw ""
  [ -n "$PR" ] || PR="$(circle_pr "$job" || true)"
}

from_gha_run() {
  local run="$1" n
  info "GitHub Actions run $run: reading the collect job's summary ..."
  gha_failed "$run" | add_raw "shell"
  n="$(awk -F'\t' '$1=="shell"' "$RAW" | wc -l)"
  if [ "$n" -ge 50 ]; then
    CAP_NOTE="the gha-ci summary table is capped at 50 rows upstream; only those $n case(s) are reproducible here. For the complete set use CI's own '/run rerun $run'."
    warn "$CAP_NOTE"
  fi
  if [ -z "$PR" ]; then
    local b; b="$(gha_tc_branch "$run" || true)"
    case "$b" in tc/pr-[0-9]*) PR="${b#tc/pr-}" ;; esac
  fi
}

from_pr() {
  local pr="$1" rollup line ctx state target
  PR="$pr"
  info "PR #$pr: looking for failed sql / medium / shell CI ..."
  command -v gh >/dev/null 2>&1 || die "gh is required to inspect a PR"
  rollup="$(gh pr view "$pr" -R "$GH_REPO" --json statusCheckRollup \
      --jq '.statusCheckRollup[] | select(.context? != null) | [.context, (.state // ""), (.targetUrl // "")] | @tsv' 2>/dev/null)" \
    || die "could not read PR #$pr status checks"
  local found=0
  while IFS=$'\t' read -r ctx state target; do
    case "$ctx" in
      "ci/circleci: test_sql"|"ci/circleci: test_medium")
        if [ "$state" = "FAILURE" ]; then
          found=1; from_circle_job "${target##*/}"
        else
          info "  $ctx: $state — nothing to reproduce."
        fi ;;
      "gha-ci: test_shell")
        if [ "$state" = "FAILURE" ]; then
          found=1; from_gha_run "${target##*/}"
        else
          info "  $ctx: $state — nothing to reproduce."
        fi ;;
    esac
  done <<<"$rollup"
  [ "$found" -eq 1 ] || info "no FAILED sql/medium/shell CI on PR #$pr."
}

if [ -n "$ARG_LIST" ]; then
  [ -r "$ARG_LIST" ] || die "--failed-list unreadable: $ARG_LIST"
  [ -n "$ARG_SUITE" ] || die "--failed-list requires --suite"
  add_raw "$ARG_SUITE" < "$ARG_LIST"
elif [ -n "$ARG_URL" ]; then
  case "$ARG_URL" in
    *github.com/*/pull/*)        from_pr "$(printf '%s' "$ARG_URL" | sed -nE 's#.*/pull/([0-9]+).*#\1#p')" ;;
    *github.com/*/actions/runs/*) from_gha_run "$(printf '%s' "$ARG_URL" | sed -nE 's#.*/actions/runs/([0-9]+).*#\1#p')" ;;
    *circleci.com/*/jobs/*)      from_circle_job "$(printf '%s' "$ARG_URL" | sed -nE 's#.*/jobs/([0-9]+).*#\1#p')" ;;
    *circleci.com/gh/*/[0-9]*)   from_circle_job "$(printf '%s' "$ARG_URL" | sed -nE 's#.*/([0-9]+)/?$#\1#p')" ;;
    *) die "unrecognized URL: $ARG_URL (expected a CUBRID/cubrid PR, a gha-ci run, or a CircleCI job)" ;;
  esac
else
  usage; die "give a CI URL or --failed-list"
fi

[ -s "$RAW" ] || { info "no failed cases to reproduce."; exit 0; }

# Case -> test directory (see GRANULARITY in the header).
awk -F'\t' '{ d=$2; sub(/\/cases\/[^\/]*$/, "", d); print $1 "\t" d }' "$RAW" \
  | LC_ALL=C sort -u > "$WORK/dirs.tsv"

info "reproducing $(wc -l < "$WORK/dirs.tsv") test dir(s) from $(wc -l < "$RAW") failed case(s):"
awk -F'\t' '{printf "  %-9s %s\n", $1, $2}' "$WORK/dirs.tsv"

#####################################################################
# Run each affected suite: one shard, subset = the failed dirs.
#####################################################################
rc=0
for suite in $(cut -f1 "$WORK/dirs.tsv" | LC_ALL=C sort -u); do
  repo="$(tc_repo_of_suite "$suite")"
  tcdir="$ARG_TCROOT/$repo"
  [ -d "$tcdir" ] || { warn "$suite: testcases checkout missing: $tcdir — skipped."; rc=1; continue; }

  declare -a only=()
  while IFS= read -r d; do only+=( --only "$d" ); done < <(awk -F'\t' -v s="$suite" '$1==s{print $2}' "$WORK/dirs.tsv")

  declare -a ref=()
  if [ -n "$PR" ]; then ref=( --pr "$PR" ); else
    warn "$suite: could not determine the PR -> falling back to --tc-ref develop. If the failures came from a PR's TC branch, pass --pr <N>."
    ref=( --tc-ref develop )
  fi

  out="$ARG_OUT/rerun-$(date -u +%Y%m%dT%H%M%SZ)-$$-$suite"
  info "=== $suite: ${#only[@]} subset arg(s), 1 shard -> $out"
  declare -a cmd=( "$RUNNER" --suite "$suite" --build "$ARG_BUILD" --testcases "$tcdir"
                   --out "$out" --shards 1 "${ref[@]}" "${only[@]}" )
  [ "$ARG_DRY" -eq 1 ] && cmd+=( --dry-run )
  [ "${#PASSTHRU[@]}" -gt 0 ] && cmd+=( "${PASSTHRU[@]}" )
  "${cmd[@]}" || rc=$?
done

[ -n "$CAP_NOTE" ] && warn "$CAP_NOTE"
exit "$rc"
