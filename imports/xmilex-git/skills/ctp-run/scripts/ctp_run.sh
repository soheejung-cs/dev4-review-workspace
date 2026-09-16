#!/usr/bin/env bash
#
# ctp_run.sh — the CUBRID CTP runner: one suite, N isolated podman shards.
#
# Suites: sql | medium | shell | ha_shell. Each shard is one rootless-podman
# container running the real `ctp.sh <category>` over a private, pristine copy of
# the scenario tree / install / CTP conf. A shard's slice of the suite is
# MATERIALIZED (only its own cases exist in its scenario copy), so CTP needs no
# giant exclusion list; results are merged into one pass/fail summary.
#
# The container image and its entrypoint are the CI ones: cubridci/cubridci at
# tag test_rl8.10, with this skill's entrypoint.sh (only two HA additions)
# bind-mounted over /entrypoint.sh. The image is never rebuilt locally.
#
# Isolation is the whole point: CTP's teardown runs `pkill cub` / kills every
# process of the running user, so a host-side run destroys other sessions'
# servers. Inside podman that kill only reaches the shard's own namespace.
#
# Port isolation is by namespace, NOT by reassignment: every shard reuses the SAME
# ports/SHM IDs from the CTP conf; podman's net + IPC + mount namespaces keep them
# from colliding.
#
# See README.md for the design rationale and the manual podman e2e QA steps.
#
# Copyright (c) 2024 CUBRID test-infra. Apache-2.0.

set -euo pipefail

#####################################################################
# Constants — container-internal mount targets (NOT host paths).
#####################################################################
# These are dictated by the cubridci entrypoint, not chosen by us: it runs CTP with
# HOME=$WORKDIR and the stock confs resolve ${HOME}/<tc repo> and ${CTP_HOME}, so the
# mounts must land exactly here for an unmodified CTP conf to be correct.
readonly C_WORKDIR="/home"
readonly C_CUBRID="/home/CUBRID"
readonly C_CTP="/home/cubrid-testtools/CTP"
C_DB="/home/CUBRID_DB" # HA resolves this below to match CTP cleanup paths.
readonly C_REPORT="/home/reports"
C_SCN=""          # scenario root inside the container; set by resolve_suite
C_TCREPO=""       # testcases repo mount point inside the container

# The CI test image, pinned by digest. Rocky Linux 8.10 / glibc 2.28, so an install
# built on this host runs unchanged when mounted in. CUBRID itself is injected,
# not built here. Pinned because the tag moves: an upgrade must be a
# deliberate edit of this line, never a silent `podman pull`.
readonly DEFAULT_IMAGE="docker.io/cubridci/cubridci:test_rl8.10"
readonly DEFAULT_IMAGE_DIGEST="sha256:79a344fc7664af48cd13b4b01bc54c059dd92ac2be812f2e4d9802234e06fb7e"

# One id per invocation: every container / network of this run carries it, so
# concurrent runs (different sessions, different suites) never share a name and
# cleanup can never take down someone else's container.
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
# CTP reaches the HA slave by password (jsch); it is a throwaway inside a private
# network namespace that exists only for this run.
HA_NODE_PASSWORD="${HA_NODE_PASSWORD:-ctprun}"

SELF="$(basename "$0")"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
readonly CGROUPNS="private"   # rootless on cgroup-v1 hosts fails with the default ns

#####################################################################
# Tiny helpers
#####################################################################
info()  { printf '[ctp-run] %s\n' "$*"; }
warn()  { printf '[ctp-run] WARN: %s\n' "$*" >&2; }
err()   { printf '[ctp-run] ERROR: %s\n' "$*" >&2; }
die()   { err "$*"; exit 1; }

usage() {
  cat <<'EOF'
ctp_run.sh — run one CUBRID CTP suite in isolated podman containers.

  ctp_run.sh --suite <sql|medium|shell|ha_shell> --build <install> \
             --testcases <repo checkout> [--ctp <CTP_HOME>] [--out <dir>] [options]

WHAT / WHY
  Every CTP run happens inside a container because CTP's own teardown kills every
  cub_* process of the running user (pkill cub / kill -9 by uid). On the host that
  destroys other sessions' servers; in a namespace it reaches only the shard.

  The image is the CI one (cubridci/cubridci:test_rl8.10, digest-pinned) with this
  skill's entrypoint fork bind-mounted over /entrypoint.sh. The CUBRID install is
  mounted in, never built here.

SUITES
  sql        shardable, default 7 shards (bulk + measured-time split)
  shell      shardable, default 7 shards (per test dir, count-balanced)
  medium     always 1 shard  (one mdb dataset, mutated in place)
  ha_shell   always 1 shard  (a shard is a master+slave container pair)

WHOLE SUITE vs SUBSET
  no --only        the whole suite, split across the suite's default shard count
  --only <path>    a subset: scenario-relative dir (repeatable). Sharding still
                   applies, so a big subset can be parallel too.

TESTCASE REF (never silently 'develop')
  --tc-ref <ref>   explicit branch/tag/sha        (wins over everything)
  --pr <N>         engine PR -> tc/pr-<N>
  --workspace <d>  infer the PR from that checkout's branch via gh
  With none of the three this refuses to run. cubrid-testcases and
  cubrid-testcases-private-ex carry tc/pr-<N>; a missing branch falls back to
  develop and says so in the provenance line. cubrid-testcases-private has no
  tc/pr-<N> convention and is always develop.
  The ref is materialized as a git worktree, so the host checkout's branch and
  its uncommitted edits are never touched.

OPTIONS
  --suite <s>            sql | medium | shell | ha_shell        (default sql)
  --build <dir>          CUBRID install to test (required for a real run)
  --testcases <dir>      testcases repo checkout for this suite (required)
  --ctp <dir>            CTP_HOME to copy per shard      (default ~/cubrid-testtools/CTP)
  --out <dir>            run/output directory
  --only <relpath>       scenario-relative subset path (repeatable)
  --shards <N>           override the shard count (refused for medium/ha_shell)
  --tc-ref / --pr / --workspace   see TESTCASE REF
  --testcases-as-is      use --testcases verbatim, skipping ref resolution
  --worktree-root <dir>  where testcase worktrees live
  --conf <file>          cubrid.conf whose [<suite>/cubrid.conf] section CTP applies
  --exclude <file>       host exclusion file replacing the default; empty = none
  --image <ref>          container image override
  --env K=V              extra env into every container (repeatable)
  --by-category|--by-dir|--by-case   split unit (default: per suite)
  --weights <f>|--no-weights         time-balance source
  --colocate <f>|--no-colocate       order-sensitivity registry
  --overlay              mount the install via overlay instead of copying it
  --keep                 do not remove the containers afterwards
  --abort-on-core        stop every shard on the first real core dump (default ON)
  --no-abort-on-core     opt out
  --no-webconsole        skip the sql/medium webconsole merge
  --merge-only <dir>     merge a finished run's dir into the webconsole and exit
  --label <text>         label for the merged run
  --locale-dir <dir>     prebuilt libcubrid_all_locales.so to inject
  --dry-run              plan + validate the split, launch nothing
  -h, --help             this text

OUTPUT
  <out>/provenance.txt|tsv   install / image / CTP / testcases ref+sha of this run
  <out>/plan.tsv, assignment.tsv, units.tsv
  <out>/shard_N/{console.log,exclusions.txt,assigned_cases.txt,reports/,cores/}
  <out>/failed.list          failing cases, in the shape --only takes
EOF
}

#####################################################################
# Argument parsing
#####################################################################
ARG_BUILD=""
ARG_TC=""
ARG_SUITE="sql"        # CTP suite: sql | medium | shell | ha_shell
ARG_TCREF=""           # explicit testcases ref (branch/tag/sha); "" = derive from --pr/--workspace
ARG_PR=""              # engine PR number -> testcases ref tc/pr-<N>
ARG_WS=""              # CUBRID source checkout, used to infer the PR when --pr/--tc-ref are absent
ARG_CONF=""            # host cubrid.conf whose [suite/cubrid.conf] params CTP should apply
ARG_EXCLUDE=""         # host exclusion file; set-but-empty disables exclusions
ARG_EXCLUDE_SET=0
ARG_TC_ASIS=0          # 1 = use --testcases verbatim, no ref resolution / worktree
ARG_WT_ROOT=""         # where testcase worktrees live (default: <out>/../tc-worktrees)
ARG_SHARDS=""
ARG_CTP="${HOME}/cubrid-testtools/CTP"
ARG_IMAGE="${DEFAULT_IMAGE%:*}@$DEFAULT_IMAGE_DIGEST"
ARG_OUT="./ctp-run-out"
ARG_OVERLAY=0
ARG_UNIT="auto"       # split-unit mode: auto (per-suite default) | category (top-level _* "bulk") | dir | case
declare -a ARG_ONLY=()   # scenario-relative subset prefixes ("" = whole suite)
ARG_KEEP=0
ARG_WEIGHTS="auto"   # "auto" = bundled baseline_weights.tsv (time-based) | <path> | "none" (count)
ARG_LOCALE_DIR=""
ARG_WEBCONSOLE=1
ARG_COLOCATE="auto"   # "auto" = bundled colocate.tsv if present; a path = that file; "" = disabled
ARG_ABORT_ON_CORE=1   # default ON (2026-09-03): stop every shard as soon as a core dump / disk-floor breach is seen; --no-abort-on-core opts out
ARG_MERGE_ONLY=""     # path to a finished --out dir to merge into webconsole, then exit
ARG_LABEL=""          # human tag for the merged run (webconsole 'machine' field)
ARG_DRYRUN=0
ARG_VALIDATE_ONLY=0
VO_ASSIGN=""
VO_SQL=""
declare -a ARG_ENV=()   # repeatable --env NAME=VALUE, passed through to every shard container

parse_args() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --build)       ARG_BUILD="${2:-}"; shift 2 ;;
      --testcases)   ARG_TC="${2:-}"; shift 2 ;;
      --suite)       ARG_SUITE="${2:-}"; shift 2 ;;
      --tc-ref)      ARG_TCREF="${2:-}"; shift 2 ;;
      --pr)          ARG_PR="${2:-}"; shift 2 ;;
      --workspace)   ARG_WS="${2:-}"; shift 2 ;;
      --conf)        ARG_CONF="${2:-}"; shift 2 ;;
      --exclude)
        [ "$#" -ge 2 ] || die "--exclude requires a host file path or an empty argument"
        ARG_EXCLUDE="$2"; ARG_EXCLUDE_SET=1; shift 2 ;;
      --testcases-as-is) ARG_TC_ASIS=1; shift ;;
      --worktree-root)   ARG_WT_ROOT="${2:-}"; shift 2 ;;
      --shards)      ARG_SHARDS="${2:-}"; shift 2 ;;
      --ctp)         ARG_CTP="${2:-}"; shift 2 ;;
      --image)       ARG_IMAGE="${2:-}"; shift 2 ;;
      --out)         ARG_OUT="${2:-}"; shift 2 ;;
      --overlay)     ARG_OVERLAY=1; shift ;;
      --only)        ARG_ONLY+=( "${2:-}" ); shift 2 ;;
      --by-category) ARG_UNIT="category"; shift ;;
      --by-dir)      ARG_UNIT="dir"; shift ;;
      --by-case)     ARG_UNIT="case"; shift ;;
      --keep)        ARG_KEEP=1; shift ;;
      --weights)     ARG_WEIGHTS="${2:-}"; shift 2 ;;
      --no-weights)  ARG_WEIGHTS="none"; shift ;;
      --locale-dir)  ARG_LOCALE_DIR="${2:-}"; shift 2 ;;
      --colocate)    ARG_COLOCATE="${2:-}"; shift 2 ;;
      --no-colocate) ARG_COLOCATE=""; shift ;;
      --abort-on-core) ARG_ABORT_ON_CORE=1; shift ;;
      --no-abort-on-core) ARG_ABORT_ON_CORE=0; shift ;;
      --no-webconsole) ARG_WEBCONSOLE=0; shift ;;
      --merge-only)  ARG_MERGE_ONLY="${2:-}"; shift 2 ;;
      --label)       ARG_LABEL="${2:-}"; shift 2 ;;
      --env)
        case "${2:-}" in
          [A-Za-z_]*=*) : ;;
          *) usage; die "--env expects NAME=VALUE (got: '${2:-}')" ;;
        esac
        # Scope must reach the host planner before it materializes each shard.
        case "${2%%=*}" in
          CTP_SCENARIO|CTP_EXCLUDE|SHELL_SCENARIO|HA_SCENARIO|MEMORY_SCENARIO)
            die "${2%%=*} is retired; use --only for scope or --exclude for exclusions" ;;
          TEST_SCENARIO|TEST_EXCLUDE)
            die "${2%%=*} is managed by the runner; use --only or --exclude so the split matches the run" ;;
        esac
        ARG_ENV+=("$2"); shift 2 ;;
      --dry-run)     ARG_DRYRUN=1; shift ;;
      # Hidden self-test seam: run ONLY the offline split-validator against the
      # given assignment.tsv + .sql list (used by run_tests.sh on synthetic data).
      --validate-only) ARG_VALIDATE_ONLY=1; VO_ASSIGN="${2:-}"; VO_SQL="${3:-}"; shift 3 ;;
      -h|--help)     usage; exit 0 ;;
      *)             usage; die "unknown argument: $1" ;;
    esac
  done

  if [ "$ARG_VALIDATE_ONLY" -eq 1 ]; then
    [ -r "$VO_ASSIGN" ] || die "--validate-only: assignment file unreadable: $VO_ASSIGN"
    [ -r "$VO_SQL" ]    || die "--validate-only: sql-list file unreadable: $VO_SQL"
    return 0
  fi

  # --merge-only needs only a finished out dir + a CTP_HOME to merge into (no build/testcases).
  if [ -n "$ARG_MERGE_ONLY" ]; then
    [ -d "$ARG_MERGE_ONLY" ] || die "--merge-only dir does not exist: $ARG_MERGE_ONLY"
    [ -d "$ARG_CTP" ]        || die "--merge-only: --ctp dir does not exist: $ARG_CTP"
    [ -d "$ARG_CTP/sql" ]    || die "--merge-only: $ARG_CTP has no sql/ (need a CTP_HOME with webconsole)"
    return 0
  fi

  if [ "$ARG_EXCLUDE_SET" -eq 1 ] && [ -n "$ARG_EXCLUDE" ]; then
    [ -f "$ARG_EXCLUDE" ] && [ -r "$ARG_EXCLUDE" ] \
      || die "--exclude file unreadable: $ARG_EXCLUDE"
    ARG_EXCLUDE="$(readlink -f "$ARG_EXCLUDE")"
  fi

  resolve_suite
  # Both MUST be absolute before anything uses them. `git worktree add` resolves a
  # relative path against the REPOSITORY, not the invocation cwd, so a relative
  # --out silently created the worktree inside the testcases checkout while every
  # later check looked for it under the cwd. mkdir -p first: readlink -f of a
  # not-yet-existing path is fine, but the out dir is created here anyway.
  mkdir -p "$ARG_OUT" 2>/dev/null || die "cannot create --out dir: $ARG_OUT"
  ARG_OUT="$(readlink -f "$ARG_OUT")"
  [ -n "$ARG_WT_ROOT" ] || ARG_WT_ROOT="$(dirname "$ARG_OUT")/tc-worktrees"
  mkdir -p "$ARG_WT_ROOT" 2>/dev/null || die "cannot create worktree root: $ARG_WT_ROOT"
  ARG_WT_ROOT="$(readlink -f "$ARG_WT_ROOT")"
  [ -n "$ARG_TC" ]  || { usage; die "--testcases is required"; }
  [ -d "$ARG_TC" ]  || die "--testcases dir does not exist: $ARG_TC"
  SCN="$ARG_TC/$SUITE_SUBPATH"
  [ -d "$SCN" ]     || die "scenario dir not found: $SCN (expected <testcases>/$SUITE_SUBPATH)"
  [ -d "$ARG_CTP" ] || die "--ctp dir does not exist: $ARG_CTP"
  if [ -n "$ARG_CONF" ]; then
    [ -r "$ARG_CONF" ] || die "--conf file unreadable: $ARG_CONF"
    ARG_CONF="$(readlink -f "$ARG_CONF")"
  fi

  if [ "$ARG_DRYRUN" -eq 0 ]; then
    [ -n "$ARG_BUILD" ] || { usage; die "--build is required for a real run (omit only with --dry-run)"; }
    [ -d "$ARG_BUILD" ] || die "--build dir does not exist: $ARG_BUILD"
    # Resolve symlinks: a CUBRID install is often a symlink (e.g. ~/CUBRID -> .../CUBRID-11.5.x).
    # cp -a of a symlink would create a dangling link in the shard workdir, so copy the target.
    ARG_BUILD="$(readlink -f "$ARG_BUILD")"
    [ -d "$ARG_BUILD" ] || die "--build resolved to a non-directory: $ARG_BUILD"
  fi
  # Normalise SCN to an absolute path with no trailing slash (matches scenarioRootPath in F4).
  SCN="$(cd "$SCN" && pwd)"
}

#####################################################################
# Suite table. Each suite fixes: which testcases repo holds it, where under that
# repo the scenario root is, which ctp.sh category runs it, what a "case" file
# looks like, the default split unit, whether it may be sharded at all, and where
# its base exclusion list lives.
#
# Sharding policy (decided 2026-09-04, workspace#157 grilling):
#   sql      shardable, default 7 (bulk + measured-time split)
#   shell    shardable, default 7 (per test-dir, count-balanced until timings exist)
#   medium   NEVER sharded — one mdb is loaded from a single data_file tarball and
#            the cases mutate it in place, so two shards would race one dataset.
#   ha_shell NEVER sharded — a shard is a PAIR of containers (master+slave) and the
#            suite is only ever run one bucket at a time.
#####################################################################
SUITE_TCREPO=""; SUITE_SUBPATH=""; SUITE_CAT=""; SUITE_EXT=""
SUITE_UNIT_DEFAULT=""; SUITE_SHARDABLE=0; SUITE_STYLE=""; SUITE_HA=0
SUITE_CONF=""     # source conf for host engine-parameter merging, before the runtime copy
resolve_suite() {
  case "$ARG_SUITE" in
    sql)
      SUITE_TCREPO="cubrid-testcases";            SUITE_SUBPATH="sql"
      SUITE_CAT="sql";      SUITE_EXT="sql";      SUITE_UNIT_DEFAULT="category"
      SUITE_SHARDABLE=1;    SUITE_STYLE="sqlresult";  SUITE_CONF="conf/sql.conf" ;;
    medium)
      SUITE_TCREPO="cubrid-testcases";            SUITE_SUBPATH="medium"
      SUITE_CAT="medium";   SUITE_EXT="sql";      SUITE_UNIT_DEFAULT="dir"
      SUITE_SHARDABLE=0;    SUITE_STYLE="sqlresult";  SUITE_CONF="conf/medium_dev.conf" ;;
    shell)
      SUITE_TCREPO="cubrid-testcases-private-ex"; SUITE_SUBPATH="shell"
      SUITE_CAT="shell";    SUITE_EXT="sh";       SUITE_UNIT_DEFAULT="dir"
      SUITE_SHARDABLE=1;    SUITE_STYLE="status";     SUITE_CONF="conf/shell_ci.conf" ;;
    ha_shell)
      SUITE_TCREPO="cubrid-testcases-private";    SUITE_SUBPATH="HA/shell"
      SUITE_CAT="ha_shell"; SUITE_EXT="sh";       SUITE_UNIT_DEFAULT="dir"
      SUITE_SHARDABLE=0;    SUITE_STYLE="status";     SUITE_CONF="conf/ha_shell_ci.conf"; SUITE_HA=1
      # HA cleanup removes $CUBRID/databases/<db>*. Keep copylog files there too,
      # or a later case reuses replication logs belonging to its previous DB.
      C_DB="$C_CUBRID/databases" ;;
    *) usage; die "--suite must be one of: sql medium shell ha_shell (got: '$ARG_SUITE')" ;;
  esac
  C_TCREPO="$C_WORKDIR/$SUITE_TCREPO"
  C_SCN="$C_TCREPO/$SUITE_SUBPATH"
  # NOT a one-line test: as the last statement of the function its exit status
  # would become the function's, so an explicit --by-* (auto already replaced)
  # would make resolve_suite return 1 and, under set -e, kill the run silently.
  if [ "$ARG_UNIT" = "auto" ]; then
    ARG_UNIT="$SUITE_UNIT_DEFAULT"
  fi
}

#####################################################################
# Which testcases ref this run verifies.
#
# The default must NEVER be "whatever the host checkout happens to have on
# HEAD": that is how a PR gets validated against develop testcases and the
# result is quietly meaningless (and how a second session's `git checkout`
# changes another session's run mid-flight). So: explicit --tc-ref wins, else
# --pr, else the PR inferred from --workspace's branch; with none of the three
# this refuses to run.
#
# CI's own rule is mirrored here: cubrid-testcases and cubrid-testcases-private-ex
# carry tc/pr-<N> branches, and a missing one falls back to develop (recorded in
# the provenance line, never silent). cubrid-testcases-private has no tc/pr-<N>
# convention at all, so it is always develop.
#####################################################################
TC_REF=""; TC_REF_SRC=""; TC_SHA=""; TC_WORKTREE=""
resolve_tc_ref() {
  local repo="$SUITE_TCREPO" want=""
  if [ -n "$ARG_TCREF" ]; then
    want="$ARG_TCREF"; TC_REF_SRC="--tc-ref"
  elif [ "$repo" = "cubrid-testcases-private" ]; then
    want="develop";    TC_REF_SRC="always-develop (repo has no tc/pr-N convention)"
  else
    local pr="$ARG_PR"
    if [ -z "$pr" ] && [ -n "$ARG_WS" ]; then
      pr="$(infer_pr_from_workspace "$ARG_WS")" || pr=""
      [ -n "$pr" ] && TC_REF_SRC="inferred from --workspace branch (PR #$pr)"
    else
      [ -n "$pr" ] && TC_REF_SRC="--pr $pr"
    fi
    [ -n "$pr" ] || die "no testcases ref: pass --tc-ref <ref> or --pr <engine PR>, or --workspace <cubrid checkout> to infer it. Refusing to silently run develop testcases."
    want="tc/pr-$pr"
  fi
  TC_REF="$want"
}

# Echo the engine PR number whose head is the workspace's current branch, or fail.
infer_pr_from_workspace() {
  local ws="$1" br
  [ -d "$ws/.git" ] || return 1
  br="$(git -C "$ws" rev-parse --abbrev-ref HEAD 2>/dev/null)" || return 1
  [ -n "$br" ] && [ "$br" != "HEAD" ] || return 1
  case "$br" in develop|master|main) return 1 ;; esac
  command -v gh >/dev/null 2>&1 || return 1
  gh pr list --repo CUBRID/cubrid --head "$br" --state all --limit 1 \
     --json number --jq '.[0].number' 2>/dev/null | grep -E '^[0-9]+$'
}

# Materialize TC_REF as a worktree we own, so the host checkout's branch and its
# uncommitted edits are never touched (and two sessions on different refs cannot
# fight over one working tree). Reused across runs; refreshed to the remote tip.
materialize_tc_worktree() {
  local repo_dir="$1" ref="$2" wt_root="$3"
  local safe; safe="$(printf '%s' "$ref" | tr -c 'A-Za-z0-9._-' '_')"
  local wt="$wt_root/$(basename "$repo_dir")/$safe"
  git -C "$repo_dir" rev-parse --git-dir >/dev/null 2>&1 \
    || die "not a git checkout: $repo_dir"

  if ! git -C "$repo_dir" fetch --quiet origin "$ref" 2>/dev/null; then
    if [ "$ref" != "develop" ]; then
      warn "$(basename "$repo_dir"): ref '$ref' not on origin -> falling back to develop."
      TC_REF="develop"; TC_REF_SRC="$TC_REF_SRC + fallback (no $ref on origin)"
      materialize_tc_worktree "$repo_dir" develop "$wt_root"
      return
    fi
    die "$(basename "$repo_dir"): cannot fetch origin develop"
  fi
  TC_SHA="$(git -C "$repo_dir" rev-parse FETCH_HEAD)"

  mkdir -p "$(dirname "$wt")"
  if [ -d "$wt/.git" ] || [ -f "$wt/.git" ]; then
    git -C "$wt" reset --quiet --hard "$TC_SHA"
    git -C "$wt" clean -qfd
  else
    rm -rf "$wt"
    git -C "$repo_dir" worktree add --quiet --detach "$wt" "$TC_SHA" \
      || die "could not create worktree $wt at $TC_SHA"
  fi
  TC_WORKTREE="$wt"
  info "testcases: $(basename "$repo_dir") @ $TC_REF ($(printf '%.12s' "$TC_SHA")) -> $wt"
}

PROVENANCE=""
build_provenance() {
  PROVENANCE="$(printf 'install=%s image=%s ctp=%s testcases=%s@%s(%.12s) suite=%s shards=%s ref-src=%s' \
    "${ARG_BUILD:-<none>}" "$ARG_IMAGE" "$(ctp_revision)" \
    "$SUITE_TCREPO" "$TC_REF" "${TC_SHA:-unknown}" "$ARG_SUITE" "$NSHARDS" "${TC_REF_SRC:-n/a}")"
}
ctp_revision() {
  git -C "$ARG_CTP" rev-parse --short HEAD 2>/dev/null || echo "unknown"
}
write_provenance() {
  printf '%s\n' "$PROVENANCE" > "$OUT/provenance.txt"
  {
    printf 'suite\t%s\n' "$ARG_SUITE"
    printf 'install\t%s\n' "${ARG_BUILD:-}"
    printf 'image\t%s\n' "$ARG_IMAGE"
    printf 'image_digest\t%s\n' "$(podman image inspect "$ARG_IMAGE" --format '{{.Digest}}' 2>/dev/null || echo unknown)"
    printf 'ctp\t%s\t%s\n' "$ARG_CTP" "$(ctp_revision)"
    printf 'testcases_repo\t%s\n' "$SUITE_TCREPO"
    printf 'testcases_ref\t%s\n' "$TC_REF"
    printf 'testcases_sha\t%s\n' "${TC_SHA:-}"
    printf 'testcases_ref_source\t%s\n' "${TC_REF_SRC:-}"
    printf 'shards\t%s\n' "$NSHARDS"
    printf 'conf\t%s\n' "${ARG_CONF:-<CTP default>}"
    if [ "$ARG_EXCLUDE_SET" -eq 1 ]; then
      printf 'exclude\t%s\n' "${ARG_EXCLUDE:-<none>}"
    else
      printf 'exclude\t%s\n' '<suite default>'
    fi
    printf 'started\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$OUT/provenance.tsv"
  info "provenance: $PROVENANCE"
}

#####################################################################
# Host preflight — podman must exist for a real run. Runs BEFORE any
# work dirs are created, so a missing podman leaves nothing behind.
#####################################################################
host_preflight() {
  if ! command -v podman >/dev/null 2>&1; then
    err "podman is not installed or not on PATH."
    err "This tool launches one rootless podman container per shard; it cannot run without podman."
    err "Install podman (rootless) and retry, or use --dry-run to validate the split logic only."
    exit 3
  fi
  if ! podman image exists "$ARG_IMAGE" 2>/dev/null; then
    info "image '$ARG_IMAGE' not present locally; pulling ..."
    podman pull "$ARG_IMAGE" || die "could not obtain image '$ARG_IMAGE' (pull failed)."
  fi
  # The default reference includes its digest. --image is an explicit override;
  # provenance records the resolved digest for either path.
  # Rough disk headroom check: N working copies of build+scenario+CTP.
  local avail_kb
  avail_kb="$(df -Pk "$(dirname "$ARG_OUT")" 2>/dev/null | awk 'NR==2{print $4}')"
  [ -n "${avail_kb:-}" ] && info "disk available at out parent: $((avail_kb/1024/1024)) GB"
}

#####################################################################
# Resource sizing — pick N when --shards not given.
#   DEFAULT = 7 (DEFAULT_SHARDS): the workload-optimal count for the default
#   bulk(_*) + measured-time split on the CUBRID sql suite. The slowest shard is
#   bounded by the heaviest single bulk (~323s here, _05_plcsql), so the knee is
#   N* = ceil(total_time / heaviest_bulk) = ceil(2204/323) = 7 — at 7 shards every
#   shard is full (~315-324s) and adding more shards only leaves them idle (no
#   wall-clock gain) while costing extra build copies / RAM. (CircleCI uses 10 only
#   because it splits by name, not time.) Memory-guarded: capped down with a warning
#   if free RAM can't hold 7 shards, to avoid OOM; pass --shards to override.
#####################################################################
readonly DEFAULT_SHARDS=7
PER_SHARD_GB=3
shard_refusal_reason() {
  case "$ARG_SUITE" in
    medium)   printf 'one mdb dataset is loaded from a single data_file tarball and the cases mutate it in place' ;;
    ha_shell) printf 'a shard is a master+slave container pair and the suite is run one bucket at a time' ;;
    *)        printf 'suite policy' ;;
  esac
}
choose_shards() {
  if [ "$SUITE_SHARDABLE" -eq 0 ]; then
    if [ -n "$ARG_SHARDS" ] && [ "$ARG_SHARDS" != "1" ]; then
      die "suite '$ARG_SUITE' cannot be sharded (--shards $ARG_SHARDS refused): $(shard_refusal_reason)"
    fi
    NSHARDS=1
    info "shard count: 1 (suite '$ARG_SUITE' is never sharded: $(shard_refusal_reason))"
    return
  fi
  if [ -n "$ARG_SHARDS" ]; then
    case "$ARG_SHARDS" in (*[!0-9]*|"") die "--shards must be a positive integer";; esac
    [ "$ARG_SHARDS" -ge 1 ] || die "--shards must be >= 1"
    NSHARDS="$ARG_SHARDS"
    info "shard count: $NSHARDS (from --shards)"
    return
  fi
  # A subset defaults to ONE shard. Every shard costs a full copy of the install
  # (~1GB) and a container, which is worth it for a 17k-case suite and absurd for
  # the handful of dirs a subset usually is. Parallelism is still one flag away
  # (--shards N) for a genuinely large subset.
  if [ "${#ARG_ONLY[@]}" -gt 0 ]; then
    NSHARDS=1
    info "shard count: 1 (subset run; pass --shards N to split a large subset)"
    return
  fi
  NSHARDS=$DEFAULT_SHARDS
  local free_gb mem_cap
  free_gb="$(free -g 2>/dev/null | awk '/^Mem:/{print $7}')"; [ -z "${free_gb:-}" ] && free_gb=$(( DEFAULT_SHARDS * PER_SHARD_GB ))
  mem_cap=$(( free_gb / PER_SHARD_GB )); [ "$mem_cap" -lt 1 ] && mem_cap=1
  if [ "$mem_cap" -lt "$NSHARDS" ]; then
    warn "default $DEFAULT_SHARDS shards needs ~$(( DEFAULT_SHARDS * PER_SHARD_GB ))GB; only ${free_gb}GB free -> capping to $mem_cap (override with --shards)."
    NSHARDS=$mem_cap
  fi
  info "shard count: $NSHARDS (default; workload-optimal for the bulk + measured-time split)"
}

#####################################################################
# Order-sensitivity registry (colocate.tsv). Produces two WORK files consumed by
# unit discovery / routing / balancing:
#   keepwhole.txt : one cases-dir per line -> routed at cases-dir granularity in
#                   EVERY mode (so --by-case never splits these), kept whole.
#   gids.txt      : "<cases-dir>\t<group-id>" -> dirs sharing a group-id are pinned
#                   to the SAME shard by the balancer.
# Both files always exist (possibly empty) so the awk getline never trips. A
# registered dir absent from the scenario is warned about, not fatal.
#####################################################################
COLO_KEEPWHOLE=""
COLO_GIDS=""
resolve_colocate() {
  COLO_KEEPWHOLE="$WORK/keepwhole.txt"
  COLO_GIDS="$WORK/gids.txt"
  : >"$COLO_KEEPWHOLE"; : >"$COLO_GIDS"
  local src=""
  case "$ARG_COLOCATE" in
    "")   info "colocate: disabled (--no-colocate)."; return 0 ;;
    auto) local bundled="$SELF_DIR/../colocate.tsv"
          # bundled registry lists sql/ cases dirs only; meaningless for another suite
          [ "$ARG_SUITE" = "sql" ] && [ -r "$bundled" ] && src="$bundled" ;;
    *)    [ -r "$ARG_COLOCATE" ] || die "--colocate file not readable: $ARG_COLOCATE"; src="$ARG_COLOCATE" ;;
  esac
  if [ -z "$src" ]; then
    info "colocate: no registry found (bundled colocate.tsv absent); no constraints."
    return 0
  fi
  # Each non-comment line is a group; tokens are cases-dir relpaths (trailing '/'
  # tolerated). Emit "<dir>\tG<lineno>"; line number is a stable per-group id.
  awk '
    /^[[:space:]]*#/ { next }
    { gsub(/\r/,""); n=split($0,t,/[[:space:]]+/)
      for (i=1;i<=n;i++) if (t[i]!="") { d=t[i]; sub(/\/+$/,"",d); print d "\tG" NR }
    }
  ' "$src" > "$COLO_GIDS"
  cut -f1 "$COLO_GIDS" > "$COLO_KEEPWHOLE"
  local missing=0 d
  while IFS= read -r d; do
    [ -n "$d" ] || continue
    [ -d "$SCN/$d" ] || { warn "colocate: registered dir not in scenario (ignored): $d"; missing=$((missing+1)); }
  done < "$COLO_KEEPWHOLE"
  local ndirs ngroups
  ndirs=$(grep -c . "$COLO_KEEPWHOLE" 2>/dev/null || echo 0)
  ngroups=$(cut -f2 "$COLO_GIDS" 2>/dev/null | LC_ALL=C sort -u | grep -c . || echo 0)
  info "colocate: $ndirs dir(s) in $ngroups group(s) from $(basename "$src") — keep-whole applies to --by-case only; multi-dir groups pinned to one shard$([ "$missing" -gt 0 ] && echo "; $missing missing")."
}

#####################################################################
# Resolve the effective weights table (measured per-case seconds for time balancing).
#   auto (default) -> bundled baseline_weights.tsv if present, else count.
#   none           -> count-based (--no-weights).
#   <path>         -> that file (must be readable).
#####################################################################
WEIGHTS_FILE=""
resolve_weights() {
  case "$ARG_WEIGHTS" in
    none|"") WEIGHTS_FILE=""; info "weights: count-based (--no-weights)." ;;
    auto)
      local bundled="$SELF_DIR/../baseline_weights.tsv"
      if [ "$ARG_SUITE" != "sql" ]; then
        WEIGHTS_FILE=""; info "weights: bundled table is sql-only; count-based for suite '$ARG_SUITE'."
      elif [ -r "$bundled" ]; then
        WEIGHTS_FILE="$bundled"
        info "weights: time-based from bundled $(basename "$bundled") ($(awk -F'\t' '{s+=$2}END{printf "%.0f", s}' "$bundled" 2>/dev/null)s over $(wc -l < "$bundled") cases)."
      else
        WEIGHTS_FILE=""; info "weights: no bundled table; count-based."
      fi ;;
    *)
      [ -r "$ARG_WEIGHTS" ] || die "--weights file not readable: $ARG_WEIGHTS"
      WEIGHTS_FILE="$ARG_WEIGHTS"; info "weights: time-based from $ARG_WEIGHTS." ;;
  esac
}

#####################################################################
# Unit discovery.
#   DEFAULT unit = top-level _* directory ("bulk"). This is exactly CircleCI's sql
#   split unit (.circleci/config.yml globs "cubrid-testcases/sql/_*" and ships each
#   match WHOLE to one node). A bulk is INDIVISIBLE — it is never split across shards
#   — so every test inside a _* dir stays co-located in canonical order, exactly as
#   CI groups them. That co-location is what keeps the suite green: finer splits move
#   tests apart and expose cross-test / shared-DB interference (one shard runs ONE
#   database for all its cases, and not every test self-isolates), producing failures
#   that depend on which tests share a shard. Bulk grouping mirrors CI and avoids it.
#   Balance bulks by measured time with --weights (a bulk's weight = sum of its cases'
#   seconds); without weights, by case count. Tradeoff: the heaviest single bulk is a
#   hard floor on the slowest shard, since a bulk cannot be split.
#   --by-dir:      unit = OUTERMOST cases dir (~1157 units; finer => better balance, but
#                  co-locates fewer related tests, so may expose isolation failures).
#   --by-case:     unit = the .sql itself (finest; NOT order-safe; opt in only when the
#                  targeted cases are known independent).
#   colocate.tsv:  applies ONLY to --by-case (keeps listed cases dirs whole / co-located);
#                  in dir/category the cases dir / bulk is already atomic.
# Emits to stdout: "<unit_relpath>\t<weight>" (scenario-relative, no leading '/').
# Honors base exclusions (F4): a .sql removed by the base list is not counted.
#####################################################################
UNITS_FILE=""      # tmp: unit \t sqlcount
SQL_LIST=""        # tmp: surviving .sql relpaths (post base exclusion)
SQL_ALL=""         # tmp: all .sql relpaths
BASE_FILE=""       # the original CTP exclusions.txt (verbatim base list)

# The base exclusion list CTP itself will apply. It must be the SAME file the
# container's conf points at, or our surviving-case count (and therefore the
# split invariant) disagrees with what CTP actually runs.
suite_base_exclusion_file() {
  case "$ARG_SUITE" in
    sql|medium) printf '%s' "$ARG_CTP/conf/exclusions.txt" ;;
    shell|ha_shell) printf '%s' "$SCN/config/daily_regression_test_excluded_list_linux.conf" ;;
  esac
}

discover_units() {
  BASE_FILE="$(suite_base_exclusion_file)"
  if [ "$ARG_EXCLUDE_SET" -eq 1 ]; then
    BASE_FILE="${ARG_EXCLUDE:-/dev/null}"
  fi
  [ -r "$BASE_FILE" ] || die "exclusion list unreadable: $BASE_FILE (use --exclude '' for no exclusions)"
  # Snapshot the exact list used for planning; every shard receives these bytes.
  cp -f "$BASE_FILE" "$WORK/exclusions.txt"
  BASE_FILE="$WORK/exclusions.txt"
  SQL_ALL="$WORK/sql_all.txt"
  SQL_LIST="$WORK/sql_surviving.txt"
  UNITS_FILE="$WORK/units.tsv"

  # All .sql under a cases dir, as scenario-relative paths with NO leading '/'.
  # CTP's run.sh appends a trailing '/' to the scenario root before computing
  # caseRelativePath = caseFile.substring(rootLen), so the relative path it
  # matches against exclusions has NO leading slash. We must use the same
  # convention or the substring (containPath) match never fires.
  find "$SCN" -type f -name "*.${SUITE_EXT}" -path '*/cases/*' \
    | sed "s#^${SCN}/##" | LC_ALL=C sort > "$SQL_ALL"
  local total; total=$(wc -l < "$SQL_ALL")
  [ "$total" -gt 0 ] || die "no .${SUITE_EXT} case found under $SCN"

  # Subset (--only): keep only cases under the requested scenario-relative dirs.
  # This is how a partial run is expressed — one shard over a filtered case pool —
  # so the whole-suite path and the subset path share every downstream step.
  if [ "${#ARG_ONLY[@]}" -gt 0 ]; then
    local sel="$WORK/only.txt" pfx
    : >"$sel"
    for pfx in "${ARG_ONLY[@]}"; do
      pfx="${pfx#/}"; pfx="${pfx%/}"
      [ -n "$pfx" ] || continue
      [ -e "$SCN/$pfx" ] || die "--only path not in the scenario: $pfx (under $SCN)"
      awk -v p="$pfx/" 'index($0,p)==1' "$SQL_ALL" >> "$sel"
      awk -v p="$pfx" '$0==p' "$SQL_ALL" >> "$sel"
    done
    LC_ALL=C sort -u "$sel" -o "$sel"
    [ -s "$sel" ] || die "--only selected no cases (checked ${#ARG_ONLY[@]} path(s) under $SCN)"
    info "subset: ${#ARG_ONLY[@]} path(s) -> $(wc -l < "$sel") of $total case(s)"
    mv -f "$sel" "$SQL_ALL"
    total=$(wc -l < "$SQL_ALL")
  fi

  # Apply base exclusions (F4 containPath) to get the surviving pool.
  apply_base_exclusions "$SQL_ALL" "$SQL_LIST"

  GLOBAL_SQL=$total
  SURVIVING_SQL=$(wc -l < "$SQL_LIST")
  BASE_EXCLUDED=$(( GLOBAL_SQL - SURVIVING_SQL ))
  [ "$SURVIVING_SQL" -gt 0 ] || die "exclusions removed every selected case; nothing to run"

  # Build "<unit>\t<weight>": aggregate the surviving .sql by their unit key and sum
  # weights (measured seconds from --weights, else 1 per .sql; an all-zero unit is
  # clamped to weight 1 so it is still schedulable). The unit key (see unitkey()) is
  # the cases dir by default, the top-level _* dir with --by-category, or the .sql
  # itself with --by-case.
  local wfile="${WEIGHTS_FILE:-/dev/null}"
  awk -F'\t' -v mode="$ARG_UNIT" -v wf="$wfile" -v kw="$COLO_KEEPWHOLE" '
    function unitkey(p,   u, c) {
      # category (DEFAULT): top-level _* "bulk" — atomic, never split across shards.
      if (mode=="category") { u=p; sub(/\/.*/,"",u); return u }
      # outermost cases dir of p (also the keep-whole lookup key).
      c=p; if (match(c,/\/cases\//)) c=substr(c,1,RSTART+RLENGTH-2)
      # keep-whole registry applies ONLY to --by-case (the only mode finer than a cases dir);
      # in dir/category the cases dir / bulk is already whole, so the registry must not pull a
      # cases dir out of its atomic bulk.
      if (mode=="case") return (c in KW) ? c : p
      return c                         # dir: outermost cases dir
    }
    BEGIN {
      while ((getline l < kw) > 0) if (l!="") KW[l]=1
      while ((getline l < wf) > 0) { m=split(l,a,"\t"); if (m>=2) w[a[1]]=a[2]+0 }
    }
    { p=$0; wt=(p in w)?w[p]:1; agg[unitkey(p)]+=wt }
    END { for (u in agg) printf "%s\t%d\n", u, (agg[u]<1?1:agg[u]) }
  ' "$SQL_LIST" | LC_ALL=C sort > "$UNITS_FILE"
}

# shell_helper_dirs <scenario_root>
# Print (scenario-relative, one per line) every directory that is neither a case
# dir (has a cases/ beneath it), nor inside one, nor config/ — i.e. the shared
# helper dirs (common/, commonMethod/) shell cases source by relative path. Only
# the outermost such dir is printed; rsync -r ships its contents.
shell_helper_dirs() {
  local scn="$1"
  local cases all
  cases="$(cd "$scn" && find . -type d -name cases -printf '%h\n' | sed 's#^\./##' | LC_ALL=C sort -u)"
  all="$(cd "$scn" && find . -mindepth 1 -type d ! -name cases ! -path '*/cases/*' ! -path './config' ! -path './config/*' \
          | sed 's#^\./##' | LC_ALL=C sort)"
  awk 'NR==FNR { c[$0]=1; next }
       {
         d=$0
         for (k in c) {
           if (k==d || index(k, d "/")==1 || index(d, k "/")==1) next
         }
         # skip a dir whose ancestor was already printed (rsync -r covers it)
         for (h in out) if (index(d, h "/")==1) next
         out[d]=1; print d
       }' <(printf '%s\n' "$cases") <(printf '%s\n' "$all")
}

# apply_base_exclusions <in_sql_list> <out_surviving_list>
# Uses SQL containPath or shell raw-substring matching for the selected suite.
apply_base_exclusions() {
  local in="$1" out="$2"
  # getLineList keeps every non-blank, trimmed line (comments included; they match nothing).
  awk 'NF{ gsub(/^[ \t]+|[ \t]+$/,""); if(length) print }' "$BASE_FILE" > "$WORK/base_entries.txt" 2>/dev/null || :
  [ -s "$WORK/base_entries.txt" ] || : >"$WORK/base_entries.txt"

  # Which root the exclusion entries are relative to is NOT the same for every
  # suite. sql/medium list cases relative to the scenario root (_13_issues/...),
  # but the shell lists are written relative to the REPO root and so carry the
  # scenario dir itself as their first segment (shell/_06_issues/...). Matching
  # scenario-relative paths against those entries silently excludes nothing —
  # every entry misses — so for those suites we prefix the paths, filter, then
  # strip the prefix back off.
  local pfx=""
  [ "$SUITE_STYLE" = "status" ] && pfx="$SUITE_SUBPATH/"
  if [ -z "$pfx" ]; then
    contain_path_filter "$WORK/base_entries.txt" "$in" "$out"
    return
  fi
  awk -v p="$pfx" '{print p $0}' "$in" > "$WORK/base_prefixed_in.txt"
  # The shell runner matches raw substrings, including an individual .sh name;
  # SQL's directory-suffix rule would turn x.sh into x.sh/ and miss that case.
  awk -v entries="$WORK/base_entries.txt" '
    BEGIN { while ((getline e < entries) > 0) if (e!="") ent[++n]=e }
    { for (i=1;i<=n;i++) if (index($0,ent[i])>0) next; print }
  ' "$WORK/base_prefixed_in.txt" > "$WORK/base_prefixed_out.txt"
  awk -v p="$pfx" 'index($0,p)==1 { print substr($0, length(p)+1); next } { print }' \
    "$WORK/base_prefixed_out.txt" > "$out"
}

# contain_path_filter <entries_file> <paths_file> <surviving_out>
# Writes to <surviving_out> the paths NOT matched by ANY entry, using F4 semantics:
#   entry e -> e2 = (e ends with '/' or '.sql') ? e : e+'/' ; match if index(path,e2)>0
contain_path_filter() {
  local entries="$1" paths="$2" out="$3"
  # Read entries in BEGIN via getline (NOT the NR==FNR idiom): when the entries
  # file is EMPTY, NR==FNR stays true for the paths file too, so every path would
  # be misread as an exclusion entry and nothing would survive. getline keeps the
  # two inputs cleanly separate and yields n=0 (exclude nothing) on an empty file.
  awk -v entries="$entries" '
    BEGIN {
      n=0
      while ((getline e < entries) > 0) {
        if (e=="") continue
        if (substr(e,length(e))!="/" && substr(e,length(e)-3)!=".sql") e=e"/"
        ent[++n]=e
      }
    }
    {
      p=$0; excluded=0
      for (i=1;i<=n;i++) { if (index(p,ent[i])>0) { excluded=1; break } }
      if (!excluded) print p
    }
  ' "$paths" > "$out"
}

#####################################################################
# Greedy LPT balancing — assign units to shards, respecting co-locate groups.
#   A "group" is a co-locate group from the registry (its member units must share
#   a shard) or, for every other unit, the unit itself. Groups are packed weight
#   DESC (tie-break: group-id ASC, deterministic) into the least-loaded shard;
#   then each member unit inherits its group's shard. With no registry every group
#   is a singleton, so this reduces exactly to per-unit LPT.
# Produces: ASSIGN ("unit \t shard"), SHARD_LOAD[i] totals.
#####################################################################
ASSIGN_FILE=""
declare -a SHARD_LOAD
balance_units() {
  ASSIGN_FILE="$WORK/assignment.tsv"
  : >"$ASSIGN_FILE"
  local i
  for (( i=0; i<NSHARDS; i++ )); do SHARD_LOAD[i]=0; done

  # (1) unit -> group-id (default = the unit) and per-group summed weight.
  awk -F'\t' -v gidf="$COLO_GIDS" -v ugf="$WORK/unit_gid.tsv" -v gwf="$WORK/group_w.tsv" '
    BEGIN { while ((getline l < gidf) > 0) { m=split(l,a,"\t"); if (m>=2) gid[a[1]]=a[2] } }
    { u=$1; w=$2+0; g=(u in gid)?gid[u]:u; gw[g]+=w; print u "\t" g > ugf }
    END { for (g in gw) printf "%s\t%d\n", g, gw[g] > gwf }
  ' "$UNITS_FILE"

  # (2) LPT over groups (weight desc, group-id asc) -> group -> shard + #LOAD.
  LC_ALL=C sort -t"$(printf '\t')" -k2,2nr -k1,1 "$WORK/group_w.tsv" \
    | awk -F'\t' -v n="$NSHARDS" '
        BEGIN { for (i=0;i<n;i++) load[i]=0 }
        { g=$1; w=$2+0; best=0; for (i=1;i<n;i++) if (load[i]<load[best]) best=i; load[best]+=w; print g "\t" best }
        END { for (i=0;i<n;i++) printf "#LOAD\t%d\t%d\n", i, load[i] }
      ' > "$WORK/group_shard_raw.tsv"
  grep -v '^#LOAD' "$WORK/group_shard_raw.tsv" > "$WORK/group_shard.tsv"

  # (3) join unit -> group -> shard.
  awk -F'\t' '
    FNR==NR { gs[$1]=$2; next }            # group -> shard
    { print $1 "\t" gs[$2] }               # unit  -> shard
  ' "$WORK/group_shard.tsv" "$WORK/unit_gid.tsv" > "$ASSIGN_FILE"

  # capture loads
  while IFS=$'\t' read -r tag idx load; do
    [ "$tag" = "#LOAD" ] && SHARD_LOAD[idx]=$load
  done < <(grep '^#LOAD' "$WORK/group_shard_raw.tsv")
}

#####################################################################
# Expand the unit->shard assignment into per-shard .sql lists (the files each
# shard will MATERIALIZE) and per-shard .sql counts. Each surviving .sql is
# routed to exactly one shard via its unit, so this is an exact partition.
# Also writes a base-only exclusions.txt per shard (belt-and-suspenders: the
# assigned .sql are materialized, the rest are simply never copied).
#####################################################################
declare -a SHARD_NSQL
expand_shard_sets() {
  local i
  rm -f "$WORK/unmapped.txt"
  for (( i=0; i<NSHARDS; i++ )); do
    : >"$WORK/shard_${i}.sql.txt"
    SHARD_NSQL[i]=0
    cp -f "$BASE_FILE" "$WORK/shard_${i}.exclusions.txt"
  done
  awk -F'\t' -v mode="$ARG_UNIT" -v work="$WORK" -v kw="$COLO_KEEPWHOLE" '
    function unitkey(p,   u, c) {
      if (mode=="category") { u=p; sub(/\/.*/,"",u); return u }
      c=p; if (match(c,/\/cases\//)) c=substr(c,1,RSTART+RLENGTH-2)
      if (mode=="case") return (c in KW) ? c : p
      return c
    }
    BEGIN { while ((getline l < kw) > 0) if (l!="") KW[l]=1 }
    FNR==NR { sh[$1]=$2; next }                      # ASSIGN: unit -> shard
    {
      p=$0; u=unitkey(p); s=sh[u]
      if (s=="") { print p > (work"/unmapped.txt"); next }
      print p > (work"/shard_" s ".sql.txt")
      cnt[s]++
    }
    END { for (s in cnt) print s"\t"cnt[s] }
  ' "$ASSIGN_FILE" "$SQL_LIST" > "$WORK/shard_nsql.txt"
  while IFS=$'\t' read -r s c; do SHARD_NSQL[$s]=$c; done < "$WORK/shard_nsql.txt"
}

#####################################################################
# Split validation (offline; needs only assignment.tsv + the surviving .sql list,
# so it doubles as the --validate-only self-test seam).
#
# A unit U (a dir path) CLAIMS a .sql P iff P==U (per-case units) or P starts with
# U"/" (dir/category units). A clean split requires every surviving .sql to be
# claimed by EXACTLY ONE shard:
#   * 0 claiming units            -> ORPHAN: no shard would run it.
#   * units on >1 distinct shard  -> AMBIGUOUS: a nested cases dir landed on a
#                                    different shard than its outer dir, so the .sql
#                                    would be materialized in more than one shard.
# Proving this for every .sql proves the unit set is prefix-free (order-safe split
# units never overlap) AND covers the pool exactly once. Aborts (exit 4) otherwise.
#####################################################################
validate_split() {
  local nsql; nsql="$(wc -l < "$SQL_LIST")"
  : "${SURVIVING_SQL:=$nsql}"
  info "validating split over $nsql surviving .sql (each must be claimed by exactly one shard) ..."

  # Exact-key routing guard (real/dry-run path only; --validate-only has no WORK
  # tree): a .sql whose unit key is absent from the assignment was routed nowhere.
  if [ -n "${WORK:-}" ] && [ -s "$WORK/unmapped.txt" ]; then
    err "split validation FAILED: $(wc -l < "$WORK/unmapped.txt") surviving .sql not routed to any shard. Head:"
    sed -n '1,20p' "$WORK/unmapped.txt" >&2
    exit 4
  fi

  # Containment proof, independent of how routing was computed. For each .sql, walk
  # its path prefixes (and itself) and match against the assigned units.
  if awk -F'\t' '
        FNR==NR { U[$1]=$2; next }                   # ASSIGN: unit -> shard
        {
          p=$0; nclaim=0; sset=""
          if (p in U) { nclaim++; sset="<" U[p] ">" }
          L=length(p)
          for (k=1;k<=L;k++) {
            if (substr(p,k,1)=="/") {
              cand=substr(p,1,k-1)
              if (cand in U) {
                nclaim++; tag="<" U[cand] ">"
                if (index(sset,tag)==0) sset=sset tag
              }
            }
          }
          if (nclaim==0) { printf "ORPHAN (no assigned unit claims): %s\n", p > "/dev/stderr"; bad++; next }
          ndist=gsub(/>/,">",sset)                    # one ">" per distinct shard tag
          if (ndist>1) { printf "ORPHAN/AMBIGUOUS (claimed by %d units across shards %s): %s\n", nclaim, sset, p > "/dev/stderr"; bad++ }
        }
        END { exit (bad>0)?1:0 }
      ' "$ASSIGN_FILE" "$SQL_LIST"; then
    info "split valid: all $nsql surviving .sql alive in exactly one shard."
  else
    err "split validation FAILED: orphaned or cross-shard-ambiguous .sql (see ORPHAN lines above)."
    exit 4
  fi
}

#####################################################################
# Upstream composes the runtime CTP conf inside the container and applies
# TEST_SCENARIO / TEST_EXCLUDE there. Repo-relative mounts also preserve the
# medium data_file path; namespace isolation preserves ports and SHM IDs.
#
# Optional --conf: a cubrid.conf whose server parameters this run should use.
# WHERE they have to go differs by suite, because who starts the server differs:
#
#   sql / medium : CTP starts the server and writes its conf from the
#                  [<suite>/cubrid.conf] SECTION of its own suite conf. So the
#                  parameters are merged into that section of the shard's CTP
#                  copy (keys present there are replaced, new keys appended).
#                  Dropping a cubrid.conf next to it would do nothing at all.
#   shell / HA   : the cases start servers themselves (init.sh), from the
#                  install's own $CUBRID/conf/cubrid.conf. So the file replaces
#                  the one in the shard's install copy.
install_suite_conf() {
  local d="$1"
  [ -n "$ARG_CONF" ] || return 0

  if [ "$SUITE_STYLE" = "status" ]; then
    if [ "$ARG_OVERLAY" -eq 1 ]; then
      die "--conf with --overlay is not supported for suite '$ARG_SUITE': the install is not copied, so its conf cannot be replaced per shard."
    fi
    cp -f "$ARG_CONF" "$d/CUBRID/conf/cubrid.conf"
    info "conf: $(basename "$ARG_CONF") -> shard install conf/cubrid.conf"
    return
  fi

  local target="$d/CTP/$SUITE_CONF" section="[${SUITE_CAT}/cubrid.conf]"
  [ -f "$target" ] || die "--conf: cannot find the shard's CTP conf to merge into: $target"
  awk -v userconf="$ARG_CONF" -v section="$section" '
    function flushnew() {
      for (k in u) if (!(k in done)) { print k "=" u[k]; done[k]=1 }
    }
    BEGIN {
      while ((getline l < userconf) > 0) {
        if (l ~ /^[ \t]*[#;]/ || l ~ /^[ \t]*$/ || l ~ /^[ \t]*\[/) continue
        if (match(l, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=/)) {
          k = l; sub(/=.*/, "", k); gsub(/[ \t]/, "", k)
          v = l; sub(/^[^=]*=/, "", v); gsub(/^[ \t]+|[ \t]+$/, "", v)
          u[k] = v
        }
      }
    }
    $0 == section { insec = 1; print; next }
    /^[ \t]*\[/ && insec { flushnew(); insec = 0; print; next }
    insec && match($0, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=/) {
      k = $0; sub(/=.*/, "", k); gsub(/[ \t]/, "", k)
      if (k in u) { print k "=" u[k]; done[k] = 1; next }
      print; next
    }
    { print }
    END { if (insec) flushnew() }
  ' "$target" > "$target.merged" && mv -f "$target.merged" "$target"
  info "conf: merged $(basename "$ARG_CONF") into $SUITE_CONF ${section}"
}

#####################################################################
# Emit the plan artifacts into --out (always; used by --dry-run and real runs).
#####################################################################
emit_plan() {
  mkdir -p "$OUT"
  cp -f "$ASSIGN_FILE" "$OUT/assignment.tsv"
  cp -f "$UNITS_FILE"  "$OUT/units.tsv"
  local i
  : >"$OUT/plan.tsv"
  printf '# shard\tsql_count\tweight\n' >>"$OUT/plan.tsv"
  for (( i=0; i<NSHARDS; i++ )); do
    printf '%d\t%d\t%d\n' "$i" "${SHARD_NSQL[i]:-0}" "${SHARD_LOAD[i]:-0}" >>"$OUT/plan.tsv"
    mkdir -p "$OUT/shard_${i}"
    cp -f "$WORK/shard_${i}.exclusions.txt" "$OUT/shard_${i}/exclusions.txt"
    cp -f "$WORK/shard_${i}.sql.txt" "$OUT/shard_${i}/assigned_cases.txt" 2>/dev/null || :
  done
  info "plan written to $OUT (assignment.tsv, units.tsv, plan.tsv, shard_*/{exclusions.txt,assigned_cases.txt})"
}

print_plan_summary() {
  info "split summary:"
  printf '  global .sql:        %d\n' "$GLOBAL_SQL"
  printf '  base-excluded .sql: %d\n' "$BASE_EXCLUDED"
  printf '  surviving .sql:     %d\n' "$SURVIVING_SQL"
  local modelabel
  case "$ARG_UNIT" in
    dir)      modelabel="cases-dir" ;;
    category) modelabel="bulk(_*)" ;;
    case)     modelabel="per-case" ;;
    *)        modelabel="$ARG_UNIT" ;;
  esac
  printf '  units:              %d  (mode: %s, weight: %s)\n' "$(wc -l < "$UNITS_FILE")" \
    "$modelabel" \
    "$([ -n "$WEIGHTS_FILE" ] && echo measured-time || echo count)"
  # awk line/group counts (always exit 0 and emit exactly one integer, even on an
  # empty file — unlike `grep -c`, which prints 0 AND exits 1, doubling under `||`).
  local kwn=0 grpn=0
  [ -r "$COLO_KEEPWHOLE" ] && kwn=$(awk 'END{print NR+0}' "$COLO_KEEPWHOLE" 2>/dev/null)
  [ -r "$COLO_GIDS" ] && grpn=$(awk -F'\t' '{c[$2]++} END{m=0; for(g in c) if(c[g]>1) m++; print m+0}' "$COLO_GIDS" 2>/dev/null)
  printf '  colocate:           %d dir(s), %d multi-dir group(s) pinned  (keep-whole active only with --by-case)\n' "${kwn:-0}" "${grpn:-0}"
  printf '  shards:             %d\n' "$NSHARDS"
  printf '  env passthrough (fixed): CUBRID CTP_HOME CUBRID_DATABASES WORKDIR TEST_REPORT TZ LC_ALL TEST_SCENARIO TEST_EXCLUDE\n'
  printf '  env passthrough (--env, %d): %s\n' "${#ARG_ENV[@]}" "${ARG_ENV[*]:-<none>}"
  printf '  %-7s %-10s %s\n' "shard" "sql" "weight$([ -n "$WEIGHTS_FILE" ] && echo '(s)')"
  local i
  for (( i=0; i<NSHARDS; i++ )); do
    printf '  %-7d %-10d %d\n' "$i" "${SHARD_NSQL[i]:-0}" "${SHARD_LOAD[i]:-0}"
  done
}

#####################################################################
# Per-shard working-copy construction (host side).
#####################################################################
prepare_shell_locale_conf() {
  [ "$SUITE_STYLE" = status ] || return 0
  local conf="$1/conf/cubrid_locales.txt" all="$1/conf/cubrid_locales.all.txt"
  [ -r "$conf" ] && [ -r "$all" ] || return 0
  cmp -s "$conf" "$all" || return 0
  grep -qEv '^[[:space:]]*(#|$)' "$conf" || return 0

  # CTP resets generated locale libraries before each shell/HA case. A build
  # previously used for SQL can still enable the full sample list, making that
  # deletion fatal even to en_US utilities. Reset only this recognizable preset;
  # independently authored locale configs and the source install stay untouched.
  [ "$ARG_OVERLAY" -eq 0 ] \
    || die "shell/HA needs a private locale-config reset for this build; omit --overlay"
  awk '/^[[:space:]]*(#|$)/' "$conf" > "$conf.ctprun"
  mv -f "$conf.ctprun" "$conf"
  info "locale: reset inherited all-locales preset in the $ARG_SUITE install copy (CTP deletes generated libraries)."
}

build_shard_workdir() {
  local i="$1" d="$OUT/shard_${i}"
  mkdir -p "$d"
  info "shard $i: building working copies under $d ..."

  # build -> CUBRID (writable copy absorbs CTP's conf rewrites; F3)
  if [ "$ARG_OVERLAY" -eq 1 ]; then
    prepare_shell_locale_conf "$ARG_BUILD"
    info "shard $i: --overlay set; build mounted via podman :O overlay (no copy)."
  else
    cp -a "$ARG_BUILD" "$d/CUBRID"
    prepare_shell_locale_conf "$d/CUBRID"
    # Locale speedup (D6): CTP keeps need_make_locale=yes, but make_locale is the
    # single slowest startup step (~60-90s of gcc, repeated in EVERY shard). Ship a
    # prebuilt libcubrid_all_locales.so + an early-exit make_locale.sh so CTP's
    # make_locale finds the .so already present and returns immediately instead of
    # recompiling. The locale is thus compiled at most ONCE (offline), not per shard.
    if [ -n "$LOCALE_SO" ]; then
      cp -f "$LOCALE_SO" "$d/CUBRID/lib/libcubrid_all_locales.so"
      cp -f "$LOCALE_SCRIPT" "$d/CUBRID/bin/make_locale.sh"
      chmod +x "$d/CUBRID/bin/make_locale.sh" 2>/dev/null || :
    fi
  fi

  # testcases -> private scenario copy holding ONLY this shard's slice.
  #
  # sql/medium: two passes over case FILES. Pass A takes the tree, answers and
  # everything except the test .sql; pass B adds back exactly the .sql assigned
  # here. CTP then finds precisely this shard's cases, with no giant exclusion
  # list and no O(n^2) path matching.
  #
  # shell/ha_shell: whole test DIRECTORIES, because a shell case is not one file
  # — its dir also carries .answer files, helper .sh, .c and .java sources the
  # case compiles at run time. A file-level split would leave those behind.
  #
  # The copy is laid out repo-relative (scenario under <repo>/<subpath>) so the
  # stock CTP confs, which resolve ${HOME}/<repo>/..., are correct unmodified.
  # The host worktree is never written to.
  local scn_dst="$d/testcases/$SUITE_SUBPATH"
  mkdir -p "$scn_dst"
  if [ "$SUITE_EXT" = "sh" ]; then
    sed 's#/cases/[^/]*$##' "$WORK/shard_${i}.sql.txt" | LC_ALL=C sort -u > "$WORK/shard_${i}.dirs.txt"
    # config/ holds the exclusion list CTP reads; ship every non-case top file too.
    # -r is NOT redundant with -a here: --files-from turns recursion OFF, so
    # without it rsync creates the listed DIRECTORIES and copies none of their
    # contents — a shard that runs zero cases and reports a green empty pass.
    ( cd "$SCN" && rsync -a -r --exclude='*.result' --exclude='*.log' \
        --files-from="$WORK/shard_${i}.dirs.txt" ./ "$scn_dst/" )
    # Helper dirs (common/, commonMethod/): shared libraries a case sources by a
    # relative path (`. ../../../common/lib_csql_io.sh`). They hold no cases/, so
    # the dir list above never names them and every case that sources one failed
    # locally with a fake "No such file" (wf228, 2026-09-10). Ship all of them;
    # they are a handful of tiny dirs.
    shell_helper_dirs "$SCN" > "$WORK/shard_${i}.helpers.txt"
    if [ -s "$WORK/shard_${i}.helpers.txt" ]; then
      ( cd "$SCN" && rsync -a -r --exclude='*.result' --exclude='*.log' \
          --files-from="$WORK/shard_${i}.helpers.txt" ./ "$scn_dst/" )
      info "shard $i: $(wc -l < "$WORK/shard_${i}.helpers.txt") helper dir(s) shipped alongside the cases."
    fi
    [ -d "$SCN/config" ] && rsync -a "$SCN/config" "$scn_dst/" || :
    find "$SCN" -maxdepth 1 -type f -exec cp -f {} "$scn_dst/" \; 2>/dev/null || :
  elif command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude='*.sql' --exclude='*.result' --exclude='*.log' "$SCN/" "$scn_dst/"
    # (a file list, so recursion is moot here — unlike the dir list above.)
    rsync -a --files-from="$WORK/shard_${i}.sql.txt" "$SCN/" "$scn_dst/"
  else
    cp -a "$SCN/." "$scn_dst/"
    find "$scn_dst" -type f \( -name '*.result' -o -name '*.log' \) -delete
    ( cd "$scn_dst" && find . -type f -name '*.sql' -path '*/cases/*' | sed 's#^\./##' \
        | grep -vxF -f "$WORK/shard_${i}.sql.txt" | tr '\n' '\0' | xargs -0 -r rm -f )
  fi

  # Materialization is the one step whose failure looks like success: a shard whose
  # scenario copy came out empty runs zero cases and reports a clean pass. (It has
  # happened: --files-from turns rsync's recursion off, so a directory list copied
  # the dirs and none of their contents.) So count what actually landed and refuse
  # to launch on a shortfall.
  local want got
  want="$(wc -l < "$WORK/shard_${i}.sql.txt")"
  got="$(find "$scn_dst" -type f -name "*.${SUITE_EXT}" -path '*/cases/*' 2>/dev/null | wc -l)"
  if [ "$got" -lt "$want" ]; then
    die "shard $i: materialized $got of $want assigned case file(s) under $scn_dst — refusing to run a partially-copied scenario (a shard that silently runs nothing looks like a pass)."
  fi
  info "shard $i: $got case file(s) materialized (assigned $want)."

  # CTP -> per-shard copy (drop stale run artifacts), then wire conf + exclusions
  mkdir -p "$d/CTP"
  if command -v rsync >/dev/null 2>&1; then
    # The host CTP_HOME accumulates every past run's results, for every suite.
    # Copying them into the shard makes this run's own result scan pick up
    # someone else's months-old failures (seen: a June shell failure surfacing in
    # a September sql run's failed.list). Keep the DIRECTORIES (CTP writes into
    # them) and drop their contents.
    rsync -a \
      --exclude='.output_*.log' --exclude='.script_cont_*' \
      --exclude='sql/result/*' --exclude='sql/log/*' \
      --exclude='result/*' --exclude='log/*' \
      "$ARG_CTP/" "$d/CTP/"
  else
    cp -a "$ARG_CTP/." "$d/CTP/"
    rm -rf "$d/CTP"/.output_*.log "$d/CTP"/.script_cont_* "$d/CTP"/sql/result/* "$d/CTP"/sql/log/* \
           "$d/CTP"/result/* "$d/CTP"/log/* 2>/dev/null || :
  fi
  cp -f "$WORK/shard_${i}.exclusions.txt" "$d/CTP/conf/ctprun_exclusions.txt"
  install_suite_conf "$d"

  # fresh per-shard CUBRID_DATABASES + report dir
  mkdir -p "$d/CUBRID_DB"
  : >"$d/CUBRID_DB/databases.txt"
  mkdir -p "$d/out" "$d/reports"

  # HA runs a second container (the slave node) which needs its OWN install and
  # databases dir — see shard_mounts.
  if [ "$SUITE_HA" -eq 1 ] && [ "$ARG_OVERLAY" -eq 0 ]; then
    info "shard $i: copying a second install for the HA slave node ..."
    cp -a "$d/CUBRID" "$d/CUBRID.slave"
    mkdir -p "$d/CUBRID_DB.slave"
    : >"$d/CUBRID_DB.slave/databases.txt"
  fi
}

#####################################################################
# Decide how cub_server core dumps can be captured out of each shard.
# The kernel's core_pattern is a single global knob (not namespaced), but the
# path is resolved in the crashing process's mount-ns, so:
#   - absolute path  -> bind-mount the shard's cores/ over that dir (cores land on
#                       the host automatically). [CORE_MODE=path]
#   - relative       -> cores land in the server's cwd (under the mounted CUBRID*/
#                       CTP copies); collect them after the run.          [relative]
#   - pipe (|handler)-> the host's coredump handler gets them; we cannot capture
#                       per-shard, only warn.                              [pipe]
#####################################################################
CORE_MODE="none"
CORE_DIR=""
setup_core_capture() {
  local pat; pat="$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
  case "$pat" in
    \|*) CORE_MODE="pipe"
         err "WARNING: core_pattern pipes to a handler ('$pat'); per-shard cores cannot be"
         err "         captured. Cores (if any) go to the host coredump store." ;;
    /*)  CORE_MODE="path"; CORE_DIR="$(dirname "$pat")"
         info "core capture: bind-mounting each shard's cores/ over '$CORE_DIR' (pattern '$pat')." ;;
    ?*)  CORE_MODE="relative"
         info "core capture: core_pattern is relative ('$pat'); will collect cores from shard copies." ;;
    *)   CORE_MODE="none"
         err "WARNING: core_pattern is empty; cores may be disabled on this host." ;;
  esac
}

#####################################################################
# Launch one shard container (detached, NOT --rm).
#####################################################################
SHM_SIZE="2g"

# Common podman arguments for any container of this run.
#   no --network=host / no published ports : each shard's localhost is private, so
#     every shard can reuse the conf's fixed ports without colliding.
#   --ipc=private  : private SysV SHM space, same reason for the fixed SHM ids.
#   --ulimit core=-1 : never truncate a real cub_server core.
shard_common_args() {
  printf '%s\n' --ipc=private --cgroupns="$CGROUPNS" --shm-size="$SHM_SIZE" --ulimit core=-1
}

# Mounts for one container of one shard. $1 = shard dir, $2 = node role
# (master|slave). CTP and the testcases are shared between the pair (the slave
# only needs to read the same cases), but the INSTALL and CUBRID_DATABASES must
# NOT be: each node runs its own server, rewrites its own conf, and creates its
# own database volumes there. Sharing them makes the pair overwrite each other's
# conf and databases.txt — which is why the old two-container HA setup kept a
# per-node copy of the install too.
shard_mounts() {
  local d="$1" role="${2:-master}"
  local inst="$d/CUBRID" dbs="$d/CUBRID_DB"
  if [ "$role" = "slave" ]; then inst="$d/CUBRID.slave"; dbs="$d/CUBRID_DB.slave"; fi
  local -a m=(
    -v "$d/CTP:${C_CTP}:rw"
    -v "$d/testcases:${C_TCREPO}:rw"
    -v "$d/reports:${C_REPORT}:rw"
    # The image ships its own /entrypoint.sh; bind this skill's fork over it so the
    # runner is always the one in this checkout and the image is never rebuilt.
    -v "$SELF_DIR/entrypoint.sh:/entrypoint.sh:ro"
  )
  if [ "$ARG_OVERLAY" -eq 1 ]; then
    # An overlay mount is per-container copy-on-write, so the pair does not share
    # writes even though both point at the same lower dir.
    m+=( -v "$ARG_BUILD:${C_CUBRID}:O" )
  else
    m+=( -v "$inst:${C_CUBRID}:rw" )
  fi
  # HA nests the database mount under the install; mount the parent first.
  m+=( -v "$dbs:${C_DB}:rw" )
  # Core capture: core_pattern is a global kernel knob but is resolved in the
  # crashing process's mount-ns, so for an absolute pattern we bind the shard's
  # cores/ over that directory and cores land on the host, outside $CUBRID and
  # $CTP_HOME where CTP's clean_log_cores cannot delete them.
  [ "$CORE_MODE" = "path" ] && m+=( -v "$d/cores:${CORE_DIR}:rw" )
  printf '%s\n' "${m[@]}"
}

# Env shared by master and slave.
shard_envs() {
  local -a e=(
    -e "CUBRID=${C_CUBRID}" -e "CTP_HOME=${C_CTP}" -e "CUBRID_DATABASES=${C_DB}"
    -e "WORKDIR=${C_WORKDIR}" -e "TEST_REPORT=${C_REPORT}"
    -e "TZ=Asia/Seoul" -e "LC_ALL=en_US"
    # Upstream options operate on the private, already partitioned scenario.
    -e "TEST_SCENARIO=${C_SCN}"
  )
  if [ "$ARG_EXCLUDE_SET" -eq 1 ] && [ -z "$ARG_EXCLUDE" ]; then
    e+=( -e "TEST_EXCLUDE=" )
  else
    e+=( -e "TEST_EXCLUDE=${C_CTP}/conf/ctprun_exclusions.txt" )
  fi
  local kv
  if [ "${#ARG_ENV[@]}" -gt 0 ]; then
    for kv in "${ARG_ENV[@]}"; do e+=( -e "$kv" ); done
  fi
  printf '%s\n' "${e[@]}"
}

# HA needs two containers with DIFFERENT hostnames on one network: CTP derives
# ha_node_list from `hostname` over ssh, so a shared UTS namespace cannot host a
# pair. Names carry the run id, so concurrent sessions never collide and nothing
# survives the run.
HA_NET=""
declare -a HA_SLAVE_NAMES
launch_ha_slave() {
  local i="$1" d="$OUT/shard_${i}"
  local slave="ctprun_${RUN_ID}_${i}_slave"
  HA_SLAVE_NAMES[i]="$slave"
  HA_NET="ctprun_${RUN_ID}_${i}_net"
  podman network exists "$HA_NET" 2>/dev/null || podman network create "$HA_NET" >/dev/null
  info "shard $i: launching HA slave $slave on network $HA_NET ..."
  local -a args=(); mapfile -t args < <(shard_common_args)
  local -a mounts=(); mapfile -t mounts < <(shard_mounts "$d" slave)
  local -a envs=(); mapfile -t envs < <(shard_envs)
  podman run -d --name "$slave" --hostname haslave \
    --network "$HA_NET" \
    "${args[@]}" "${envs[@]}" \
    -e "TEST_SUITE=${SUITE_CAT}" -e "HA_NODE_PASSWORD=${HA_NODE_PASSWORD}" \
    "${mounts[@]}" \
    "$ARG_IMAGE" node >/dev/null
}

launch_shard() {
  local i="$1" d="$OUT/shard_${i}" name="ctprun_${RUN_ID}_${i}"
  SHARD_NAMES[i]="$name"
  mkdir -p "$d/cores"

  local -a args=(); mapfile -t args < <(shard_common_args)
  local -a mounts=(); mapfile -t mounts < <(shard_mounts "$d" master)
  local -a envs=(); mapfile -t envs < <(shard_envs)
  local -a net=()

  if [ "$SUITE_HA" -eq 1 ]; then
    launch_ha_slave "$i"
    net=( --network "$HA_NET" --hostname hamaster )
    envs+=( -e "HA_SLAVE_HOST=haslave" -e "HA_NODE_PASSWORD=${HA_NODE_PASSWORD}" )
  fi

  info "shard $i: launching container $name (image $ARG_IMAGE, ctp.sh ${SUITE_CAT}) ..."
  podman run -d --name "$name" \
    "${net[@]}" \
    "${args[@]}" \
    "${envs[@]}" \
    "${mounts[@]}" \
    "$ARG_IMAGE" test "$SUITE_CAT" >/dev/null
}

#####################################################################
# --abort-on-core watchdog: while shards run, poll the shard working copies
# (bind-mounted host dirs, so cores are visible live even in relative
# core_pattern mode) and the free disk at --out. On the first REAL core dump
# (file(1)-verified, not just a core.* name) or a disk-floor breach, stop ALL
# shard containers so a crash-looping server cannot fill the disk with cores
# (2026-08-31 incident: 1.1T of cores on /home). The abort reason is left in
# $OUT/.abort_reason for aggregate() to report; collection still runs.
#####################################################################
CORE_POLL_SECS=5   # 2026-09-03 #199: a crash-looping shard produced 13 cores (16GB) inside one 30s poll; 5s bounds it to ~2 per shard
DISK_FLOOR_GB=30
WATCHDOG_PID=""
start_core_watchdog() {
  rm -f "$OUT/.abort_reason"
  (
    while :; do
      sleep "$CORE_POLL_SECS"
      reason=""
      avail_gb="$(df -BG --output=avail "$OUT" 2>/dev/null | tail -1 | tr -dc '0-9')"
      if [ -n "$avail_gb" ] && [ "$avail_gb" -lt "$DISK_FLOOR_GB" ]; then
        reason="disk floor breached: ${avail_gb}GB available < ${DISK_FLOOR_GB}GB"
      else
        # core_pattern names have no whitespace (core.%e.%p.%h.%t), so word
        # splitting the find output is safe here.
        for f in $(find "$OUT"/shard_*/CUBRID "$OUT"/shard_*/CUBRID_DB "$OUT"/shard_*/CTP "$OUT"/shard_*/cores \
                     -type f -name 'core.*' 2>/dev/null); do
          if file -b "$f" 2>/dev/null | grep -q 'core file'; then reason="core dump detected: $f"; break; fi
        done
      fi
      if [ -n "$reason" ]; then
        printf '%s\n' "$reason" > "$OUT/.abort_reason"
        echo "[ctp-run] ABORT-ON-CORE: $reason — stopping all shard containers." >&2
        # kill, not stop: every second of a crash-looping server is another ~2.4GB core
        for n in "${SHARD_NAMES[@]}"; do podman kill "$n" >/dev/null 2>&1 || podman stop -t 2 "$n" >/dev/null 2>&1 || :; done
        exit 0
      fi
    done
  ) &
  WATCHDOG_PID=$!
  info "abort-on-core watchdog started (pid $WATCHDOG_PID, poll ${CORE_POLL_SECS}s, disk floor ${DISK_FLOOR_GB}GB)."
}
stop_core_watchdog() {
  if [ -n "$WATCHDOG_PID" ]; then kill "$WATCHDOG_PID" 2>/dev/null || :; wait "$WATCHDOG_PID" 2>/dev/null || :; fi
  WATCHDOG_PID=""
}

#####################################################################
# Wait, collect, aggregate.
#####################################################################
declare -a SHARD_NAMES
declare -a SHARD_RC
wait_shards() {
  local i
  for (( i=0; i<NSHARDS; i++ )); do
    local name="${SHARD_NAMES[i]}" d="$OUT/shard_${i}"
    info "waiting on shard $i ($name) ..."
    SHARD_RC[i]="$(podman wait "$name" 2>/dev/null || echo 255)"
    podman logs "$name" > "$d/console.log" 2>&1 || :
    if [ "$SUITE_HA" -eq 1 ] && [ -n "${HA_SLAVE_NAMES[i]:-}" ]; then
      podman logs "${HA_SLAVE_NAMES[i]}" > "$d/console.slave.log" 2>&1 || :
      podman stop -t 5 "${HA_SLAVE_NAMES[i]}" >/dev/null 2>&1 || :
    fi
    info "shard $i finished rc=${SHARD_RC[i]}"
  done
}

# Preserve artifacts BEFORE cleanup; then rm (unless --keep).
declare -a SHARD_CORES
collect_shards() {
  local i
  for (( i=0; i<NSHARDS; i++ )); do
    local d="$OUT/shard_${i}"
    cp -f "$WORK/shard_${i}.exclusions.txt" "$d/exclusions.txt" 2>/dev/null || :
    # CTP summary/logs already live in the per-shard CTP copy on the host; which
    # subtree holds them depends on the runner CTP used (sql/ vs result/<cat>/).
    [ -d "$d/CTP/sql/result" ] && cp -a "$d/CTP/sql/result" "$d/out/" 2>/dev/null || :
    [ -d "$d/CTP/sql/log" ]    && cp -a "$d/CTP/sql/log"    "$d/out/" 2>/dev/null || :
    [ -d "$d/CTP/result" ]     && cp -a "$d/CTP/result"     "$d/out/" 2>/dev/null || :
    # Core dumps. path mode: already on the host in $d/cores via the bind-mount.
    # relative mode: cores landed in the server cwd inside the mounted CUBRID*/CTP
    # copies; sweep them into $d/cores before they are lost. (CTP only deletes cores
    # under $CUBRID/$CTP_HOME, so sweep those plus CUBRID_DB.)
    mkdir -p "$d/cores"
    if [ "$CORE_MODE" = "relative" ]; then
      find "$d/CUBRID" "$d/CUBRID_DB" "$d/CTP" -type f -name 'core*' 2>/dev/null \
        -exec sh -c 'f="$1"; [ "$(file -b "$f" 2>/dev/null | grep -c core)" -gt 0 ] && mv "$f" "$2/" || :' _ {} "$d/cores" \; 2>/dev/null || :
    fi
    SHARD_CORES[i]=$(find "$d/cores" -type f -name 'core*' 2>/dev/null | wc -l)
    [ "${SHARD_CORES[i]}" -gt 0 ] && err "shard $i: ${SHARD_CORES[i]} core dump(s) preserved in $d/cores"
  done
  cp -f "$ASSIGN_FILE" "$OUT/assignment.tsv" 2>/dev/null || :
  if [ "$ARG_KEEP" -eq 0 ]; then
    for (( i=0; i<NSHARDS; i++ )); do
      podman rm -f "${SHARD_NAMES[i]}" >/dev/null 2>&1 || :
      [ -n "${HA_SLAVE_NAMES[i]:-}" ] && podman rm -f "${HA_SLAVE_NAMES[i]}" >/dev/null 2>&1 || :
    done
    [ -n "$HA_NET" ] && podman network rm "$HA_NET" >/dev/null 2>&1 || :
  else
    info "--keep set: leaving containers (and any HA network) in place."
  fi
}

# Parse "Fail/Success/Total" from a shard's CTP output. Echoes "fail success total".
# status-style suites (shell, ha_shell) do not print a Fail/Success/Total block;
# CTP writes test_status.data with the counters, exactly as the CI runner's
# judge_status reads them. Echoes "fail success total".
parse_shard_result_status() {
  local d="$1" f="" t="" sd
  sd="$(find "$d/CTP" "$d/out" -name test_status.data -type f 2>/dev/null | head -1)"
  if [ -z "$sd" ]; then
    # No counter file at all: fall back to the JUnit report the entrypoint collected.
    local xml; xml="$(find "$d/reports" -name '*.xml' -type f 2>/dev/null | head -1)"
    if [ -n "$xml" ]; then
      t="$(grep -o 'tests="[0-9]*"' "$xml" | head -1 | tr -dc '0-9')"
      f="$(grep -o 'failures="[0-9]*"' "$xml" | head -1 | tr -dc '0-9')"
      printf '%d %d %d\n' "${f:-0}" "$(( ${t:-0} - ${f:-0} ))" "${t:-0}"
      return
    fi
    echo "0 0 0"; return
  fi
  f="$(sed -nE 's/^[[:space:]]*total_fail_case_count[[:space:]]*[:=][[:space:]]*([0-9]+).*/\1/p' "$sd" | tail -1)"
  t="$(sed -nE 's/^[[:space:]]*total_executed_case_count[[:space:]]*[:=][[:space:]]*([0-9]+).*/\1/p' "$sd" | tail -1)"
  f="${f:-0}"; t="${t:-0}"
  printf '%d %d %d\n' "$f" "$(( t - f ))" "$t"
}

parse_shard_result() {
  local d="$1" src=""
  if [ "$SUITE_STYLE" = "status" ]; then parse_shard_result_status "$d"; return; fi
  # CTP writes a summary; search console + any result summary files.
  for cand in "$d/console.log" "$d"/CTP/sql/result/*summary* "$d"/out/result/*summary*; do
    [ -f "$cand" ] && src="$cand $src"
  done
  [ -n "$src" ] || { echo "0 0 0"; return; }
  # CTP prints its summary as capitalized "Key:Value" with no space, e.g.
  #   Fail:10
  #   Success:129
  #   Total:139
  # Split on ':'/'=', lowercase+trim the key, and pull the digits from the value.
  # Last matching block wins (console.log is parsed last, so it is authoritative).
  # shellcheck disable=SC2086
  awk -F'[:=]' '
    { key=tolower($1); gsub(/[ \t]/,"",key); val=$2; gsub(/[^0-9]/,"",val) }
    key=="fail"    && val!="" { f=val }
    key=="success" && val!="" { s=val }
    key=="total"   && val!="" { t=val }
    END { printf "%d %d %d\n", f+0, s+0, t+0 }
  ' $src 2>/dev/null
}

# Write $OUT/failed.list — one scenario-relative case path per line, the exact
# shape --only accepts, so a failed run can be re-run with:
#   ctp_run.sh --suite <s> ... $(sed "s/^/--only /" failed.list)
# CTP co-locates a failed case's artifacts in its result tree, which is what makes
# the list recoverable without parsing console output.
emit_failed_list() {
  local i out="$OUT/failed.list"
  : >"$out"
  for (( i=0; i<NSHARDS; i++ )); do
    local d="$OUT/shard_${i}"
    if [ "$SUITE_STYLE" = "status" ]; then
      # shell/HA: the entrypoint collects one JUnit report per shard, whose
      # <testcase> entries name the cases and carry <failure> for the bad ones.
      find "$d/reports" -name '*.xml' -type f 2>/dev/null | while read -r x; do
        # The attribute is ` name="…"`, and it MUST be matched with its leading
        # boundary: `classname="…"` contains `name="` as a substring, so a naive
        # /name="[^"]*"/ picks up the classname instead — which for the shell
        # runner is a GitHub blob URL, not a path anything local can use.
        awk '
          /<testcase/ {
            pend=""
            if (match($0, /[ \t]name="[^"]*"/))
              pend=substr($0, RSTART+7, RLENGTH-8)
          }
          /<(failure|error)[ >]/ { if (pend!="") { print pend; pend="" } }
        ' "$x"
      done >> "$out"
    else
      # sql/medium: CTP writes NO per-case JUnit here (the entrypoint's collector
      # finds nothing and says so). What it does write is the schedule dir's
      # summary.xml, one <scenario> block per case with its <case> path and
      # <result>. Read that from the shard's own CTP copy — this run's data only.
      find "$d/CTP" -name 'summary.xml' -type f 2>/dev/null | while read -r x; do
        awk '
          match($0, /<case>[^<]*<\/case>/) { c=substr($0, RSTART+6, RLENGTH-13) }
          /<result>[ \t]*fail[ \t]*<\/result>/ { if (c!="") { print c; c="" } }
        ' "$x"
      done >> "$out"
    fi
  done
  # CTP names a case relative to the testcases REPO (sql/_13_issues/…,
  # shell/_21_xa/…), while --only takes scenario-relative paths. Normalize so
  # this file can be fed straight back in.
  if [ -s "$out" ]; then
    awk -v p="$SUITE_SUBPATH/" 'index($0,p)==1 { print substr($0, length(p)+1); next } { print }' \
      "$out" > "$out.norm" && mv -f "$out.norm" "$out"
  fi
  LC_ALL=C sort -u "$out" -o "$out" 2>/dev/null || :
  if [ -s "$out" ]; then
    info "failed cases ($(wc -l < "$out")) listed in $out"
  else
    rm -f "$out"
  fi
}
aggregate() {
  local i tot_fail=0 tot_succ=0 tot_total=0 tot_cores=0 any_crash=0
  info "==================== AGGREGATE ===================="
  printf '  %-7s %-8s %-8s %-8s %-8s %-8s %s\n' "shard" "rc" "fail" "success" "total" "cores" "expected"
  for (( i=0; i<NSHARDS; i++ )); do
    local d="$OUT/shard_${i}" f s t exp cores
    read -r f s t <<<"$(parse_shard_result "$d")"
    exp="${SHARD_NSQL[i]:-0}"
    cores="${SHARD_CORES[i]:-0}"
    tot_fail=$((tot_fail+f)); tot_succ=$((tot_succ+s)); tot_total=$((tot_total+t)); tot_cores=$((tot_cores+cores))
    # CTP exits non-zero merely because cases failed, so rc alone does not mean a
    # crash. Only call it CRASHED when the shard produced no counters at all —
    # that is the shape of a container/CTP startup failure, and it is the case
    # that must not be mistaken for "0 failures".
    local note=""
    if [ "${SHARD_RC[i]}" != "0" ] && [ "$t" -eq 0 ]; then
      note="CRASHED"; any_crash=1
    elif [ "${SHARD_RC[i]}" != "0" ] && [ "$f" -eq 0 ]; then
      note="rc!=0,NO-FAILURES-PARSED"; any_crash=1
    fi
    [ "$cores" -gt 0 ] && note="${note:+$note,}CORE"
    [ "$t" != "$exp" ] && note="${note:+$note,}TOTAL!=EXPECTED"
    printf '  %-7d %-8s %-8d %-8d %-8d %-8d %s %s\n' "$i" "${SHARD_RC[i]}" "$f" "$s" "$t" "$cores" "$exp" "$note"
  done
  printf '  %-7s %-8s %-8d %-8d %-8d %-8d %d\n' "ALL" "-" "$tot_fail" "$tot_succ" "$tot_total" "$tot_cores" "$SURVIVING_SQL"

  # Split invariant: Sigma CTP Total == surviving == global - base_excluded.
  # Hard for sql/medium (a mismatch means the split lost or duplicated cases).
  # Advisory for shell/ha_shell: CTP additionally drops cases by macro
  # (testcase_exclude_by_macro=LINUX_NOT_SUPPORTED) and by its own skip rules, so
  # executed < assigned is normal there and must not fail an otherwise-green run.
  local fail=0
  if [ "$tot_total" -ne "$SURVIVING_SQL" ]; then
    if [ "$SUITE_STYLE" = "status" ]; then
      warn "executed=$tot_total != assigned=$SURVIVING_SQL (macro/skip exclusions; not a split error)"
      [ "$tot_total" -eq 0 ] && { err "INVARIANT VIOLATED: nothing executed at all"; fail=1; }
    else
      err "INVARIANT VIOLATED: sum(CTP Total)=$tot_total != surviving=$SURVIVING_SQL"
      fail=1
    fi
  fi
  if [ "$SURVIVING_SQL" -ne "$(( GLOBAL_SQL - BASE_EXCLUDED ))" ] && [ "$SUITE_STYLE" != "status" ]; then
    err "INVARIANT VIOLATED: surviving=$SURVIVING_SQL != global-base=$(( GLOBAL_SQL - BASE_EXCLUDED ))"
    fail=1
  fi
  if [ -s "$OUT/.abort_reason" ]; then
    err "run ABORTED by --abort-on-core watchdog: $(cat "$OUT/.abort_reason")"
    err "shard results below are PARTIAL (containers were stopped mid-run)."
    fail=1
  fi
  [ "$any_crash" -ne 0 ] && { err "one or more shards crashed."; fail=1; }
  [ "$tot_cores" -ne 0 ] && { err "$tot_cores core dump(s) captured (see shard_*/cores/)."; fail=1; }
  [ "$tot_fail" -ne 0 ] && { err "$tot_fail test failure(s) across all shards."; fail=1; }

  emit_failed_list
  if [ "$fail" -ne 0 ]; then
    err "RESULT: FAILED"
    return 1
  fi
  info "RESULT: PASSED (fail=0, total=$tot_total over $NSHARDS shards)"
  return 0
}

#####################################################################
# Merge the per-shard CTP result dirs into ONE schedule dir under
# $CTP_HOME/sql/result so `ctp.sh webconsole start` shows the whole parallel run
# as a single entry whose failures are all browsable (D7).
#
# CTP makes each schedule dir self-contained for FAILED cases: it co-locates the
# failing <case>.sql/.result/.answer inside the schedule's category tree, and the
# webconsole's failure view simply walks the schedule dir for *.sql. So merging is
# a UNION of every shard's schedule sql/ subtree plus one summed main.info at the
# root (findAllTestResults stops at the first main.info, so only the merged run is
# listed; per-shard main.info are intentionally NOT copied).
#####################################################################
merge_results() {
  [ "$ARG_WEBCONSOLE" -eq 1 ] || { info "webconsole merge skipped (--no-webconsole)."; return 0; }
  # The webconsole model is the sql runner's schedule dir layout; the shell runner
  # writes elsewhere and has no such view, so there is nothing to merge for it.
  if [ "$SUITE_STYLE" = "status" ]; then
    info "webconsole merge skipped (suite '$ARG_SUITE' has no sql/result schedule layout)."
    return 0
  fi
  [ "$NSHARDS" -gt 0 ] || return 0
  local resroot="$ARG_CTP/sql/result"
  if [ ! -d "$ARG_CTP/sql" ]; then
    warn "webconsole merge: $ARG_CTP/sql not found; skipping."
    return 0
  fi

  # Build/version label + a representative cubrid_rel from any shard's main.info.
  local ver="" rel="" os="linux" i src sched
  for (( i=0; i<NSHARDS; i++ )); do
    local mi; mi="$(find "$OUT/shard_${i}/CTP/sql/result" -name main.info 2>/dev/null | head -1)"
    if [ -n "$mi" ]; then
      ver="$(awk -F: '/^build:/{print $2; exit}' "$mi")"
      rel="$(grep -m1 '^cubrid_rel:' "$mi" | cut -d: -f2-)"
      os="$(awk -F: '/^os:/{print $2; exit}' "$mi")"
      break
    fi
  done
  [ -n "$ver" ] || ver="unknown"

  local y m stamp lbl lblsan
  lbl="${ARG_LABEL:-$(basename "$OUT")}"
  lblsan="$(printf '%s' "$lbl" | tr -c 'A-Za-z0-9._-' '_')"
  y="y$(date +%Y)"; m="m$(date +%-m)"; stamp="$(date +%s)"
  sched="schedule_${os}_${ARG_SUITE}_64bit_parallel_${stamp}_${lblsan}_${ver}"
  local dest="$resroot/$y/$m/$sched"
  mkdir -p "$dest/sql"

  # Union every shard's schedule sql/ subtree (carries each shard's failed-case
  # artifacts + summary files). Distinct failures live in distinct category leaves,
  # so the union is lossless for the failure view.
  local sum_succ=0 sum_fail=0 sum_total=0 sum_exec=0 sum_time=0
  for (( i=0; i<NSHARDS; i++ )); do
    src="$(find "$OUT/shard_${i}/CTP/sql/result" -type d -name 'schedule_*' 2>/dev/null | head -1)"
    [ -n "$src" ] || { warn "webconsole merge: shard $i has no schedule dir; skipping it."; continue; }
    [ -d "$src/sql" ] && cp -an "$src/sql/." "$dest/sql/" 2>/dev/null || :
    local mi="$src/main.info"
    if [ -r "$mi" ]; then
      sum_succ=$(( sum_succ + $(awk -F: '/^success:/{print $2+0; exit}' "$mi") ))
      sum_fail=$(( sum_fail + $(awk -F: '/^fail:/{print $2+0; exit}' "$mi") ))
      sum_total=$(( sum_total + $(awk -F: '/^total:/{print $2+0; exit}' "$mi") ))
      sum_exec=$(( sum_exec + $(awk -F: '/^execute_case:/{print $2+0; exit}' "$mi") ))
      sum_time=$(( sum_time + $(awk -F: '/^totalTime:/{print $2+0; exit}' "$mi") ))
    fi
  done

  # One summed main.info at the merged root -> appears as a single run in the list.
  {
    printf 'build:%s\n' "$ver"
    printf 'version:64bit\n'
    printf 'os:%s\n' "$os"
    printf 'category:%s\n' "$ARG_SUITE"
    printf 'elapse_time:%d\n' "$sum_time"
    printf 'success:%d\n' "$sum_succ"
    printf 'fail:%d\n' "$sum_fail"
    printf 'total:%d\n' "$sum_total"
    printf 'execute_case:%d\n' "$sum_exec"
    printf 'totalTime:%d\n' "$sum_time"
    printf 'end_time:%s\n' "$(date '+%Y-%m-%d %H:%M:%S')"
    printf 'result_path:%s\n' "$dest"
    printf '%s\n' "${rel:+cubrid_rel:$rel}"
    printf 'user:\n'
    printf 'machine:ctp-run/%s(%d shards)\n' "$lbl" "$NSHARDS"
  } > "$dest/main.info"

  info "webconsole: merged $NSHARDS shards -> $dest (success=$sum_succ fail=$sum_fail total=$sum_total)"
  info "webconsole: view with  CTP_HOME=$ARG_CTP $ARG_CTP/bin/ctp.sh webconsole start  (then open http://<host>:8888 )"
}

#####################################################################
# Resolve the prebuilt locale lib + early-exit make_locale.sh to inject per shard
# (D6). Source priority: (1) --locale-dir, (2) a libcubrid_all_locales.so already
# in the build tree. The early-exit make_locale.sh is taken from --locale-dir if
# present, else the skill-bundled copy under locale/. The 18MB .so itself is NOT
# bundled in the skill; without it shards compile the locale themselves (slow).
#####################################################################
LOCALE_SO=""
LOCALE_SCRIPT=""
resolve_locale() {
  local bundled="$SELF_DIR/../locale/make_locale.sh"
  if [ -n "$ARG_LOCALE_DIR" ]; then
    [ -d "$ARG_LOCALE_DIR" ] || die "--locale-dir does not exist: $ARG_LOCALE_DIR"
    [ -r "$ARG_LOCALE_DIR/libcubrid_all_locales.so" ] \
      || die "--locale-dir has no libcubrid_all_locales.so: $ARG_LOCALE_DIR"
    LOCALE_SO="$(cd "$ARG_LOCALE_DIR" && pwd)/libcubrid_all_locales.so"
    if [ -r "$ARG_LOCALE_DIR/make_locale.sh" ]; then
      LOCALE_SCRIPT="$(cd "$ARG_LOCALE_DIR" && pwd)/make_locale.sh"
    else
      LOCALE_SCRIPT="$bundled"
    fi
  elif [ -r "$ARG_BUILD/lib/libcubrid_all_locales.so" ]; then
    LOCALE_SO="$ARG_BUILD/lib/libcubrid_all_locales.so"
    LOCALE_SCRIPT="$bundled"
  fi
  if [ -n "$LOCALE_SO" ]; then
    [ -r "$LOCALE_SCRIPT" ] || die "early-exit make_locale.sh missing: $LOCALE_SCRIPT"
    info "locale: injecting prebuilt $(basename "$LOCALE_SO") + early-exit make_locale.sh per shard (skips per-shard compile)."
  else
    info "locale: no prebuilt locale lib; shards will run make_locale (slow). Pass --locale-dir <dir with libcubrid_all_locales.so> to skip it."
  fi
}

#####################################################################
# Main
#####################################################################
WORK=""
OUT=""
cleanup() {
  stop_core_watchdog 2>/dev/null || :
  [ -n "${WORK:-}" ] && rm -rf "$WORK" 2>/dev/null || :
}

main() {
  parse_args "$@"

  # Hidden self-test seam: validate a supplied split and exit.
  if [ "$ARG_VALIDATE_ONLY" -eq 1 ]; then
    WORK="$(mktemp -d)"
    trap cleanup EXIT
    ASSIGN_FILE="$VO_ASSIGN"
    SQL_LIST="$VO_SQL"
    SURVIVING_SQL="$(wc -l < "$SQL_LIST")"
    validate_split
    return $?
  fi

  # --merge-only: merge a finished run's out dir into webconsole and exit (no podman).
  if [ -n "$ARG_MERGE_ONLY" ]; then
    OUT="$(cd "$ARG_MERGE_ONLY" && pwd)"
    NSHARDS="$(find "$OUT" -maxdepth 1 -type d -name 'shard_*' 2>/dev/null | wc -l)"
    [ "$NSHARDS" -ge 1 ] || die "--merge-only: no shard_* dirs under $OUT"
    ARG_WEBCONSOLE=1
    info "merge-only: merging $NSHARDS shard result dir(s) from $OUT into $ARG_CTP/sql/result ..."
    merge_results
    return 0
  fi

  # podman preflight FIRST (real runs only) — before any work dirs exist.
  if [ "$ARG_DRYRUN" -eq 0 ]; then
    host_preflight
  fi

  choose_shards

  OUT="$ARG_OUT"
  mkdir -p "$OUT"
  OUT="$(cd "$OUT" && pwd)"
  WORK="$(mktemp -d "${OUT}/.work.XXXXXX")"
  trap cleanup EXIT

  # Which testcases ref, materialized as a worktree we own (host checkout untouched).
  if [ "$ARG_TC_ASIS" -eq 1 ]; then
    info "testcases: --testcases-as-is -> using $ARG_TC verbatim (ref resolution skipped)"
    TC_REF="as-is:$ARG_TC"; TC_REF_SRC="--testcases-as-is"
    TC_SHA="$(git -C "$ARG_TC" rev-parse HEAD 2>/dev/null || echo unknown)"
  else
    resolve_tc_ref
    materialize_tc_worktree "$ARG_TC" "$TC_REF" "$ARG_WT_ROOT"
    SCN="$TC_WORKTREE/$SUITE_SUBPATH"
    [ -d "$SCN" ] || die "scenario dir missing in the worktree: $SCN"
    SCN="$(cd "$SCN" && pwd)"
  fi
  build_provenance
  write_provenance

  resolve_colocate
  resolve_weights
  info "discovering units under $SCN ..."
  discover_units
  balance_units
  expand_shard_sets
  validate_split
  emit_plan
  print_plan_summary

  if [ "$ARG_DRYRUN" -eq 1 ]; then
    info "--dry-run: plan + validation complete; not launching containers."
    return 0
  fi

  setup_core_capture
  resolve_locale
  declare -ga SHARD_NAMES SHARD_RC
  local i
  for (( i=0; i<NSHARDS; i++ )); do build_shard_workdir "$i"; done
  for (( i=0; i<NSHARDS; i++ )); do launch_shard "$i"; done
  [ "$ARG_ABORT_ON_CORE" -eq 1 ] && start_core_watchdog
  wait_shards
  stop_core_watchdog
  collect_shards
  local agg_rc=0
  aggregate || agg_rc=$?
  merge_results || :
  return "$agg_rc"
}

main "$@"
