---
name: ctp-run
description: >-
  Run any CUBRID CTP suite — sql, medium, shell, HA/shell — inside isolated
  rootless-podman containers: the whole suite in parallel shards, an arbitrary
  subset in one shard, or exactly the cases that failed in CI. Use when asked to
  run CTP / a regression suite / shell tests / medium / HA tests, to "run ctp in
  parallel", "shard the sql suite", "reproduce the CI failures locally",
  "ctp 병렬 실행", "shell TC 돌려", "CI 실패분 재현", or "딸깍 ctp". Host-side CTP is
  forbidden (its teardown pkills every cub_* of this user), so this is the only
  way CTP may run. Requires podman; without it use --dry-run to validate a split.
---

# ctp-run — the CTP runner

One entry point for every CTP suite. Two commands cover everything:

```bash
just ctp <sql|medium|shell|ha_shell>              # whole suite
just ctp <suite> <DIR> [<DIR> ...]                # subset (scenario-relative dirs)
just ctp-rerun <PR | CircleCI job | gha-ci run URL>   # exactly what failed in CI
```

Env knobs: `PR=<n>` / `TC_REF=<ref>` (testcase ref), `SHARDS=<n>`, `BUILD=<install>`,
`CONF=<file>`, `EXCLUDE=<file>`, `NO_ABORT_ON_CORE=1`, `CTP_ARGS="…"`, `TESTCASES_ROOT=<dir>`.

## Non-negotiables

**CTP only ever runs in a container.** CTP's teardown runs `pkill cub` and
`kill -9` over `ps -u $USER`, so a host-side run kills every `cub_master` /
`cub_server` / `cub_broker` of this user regardless of ports — the port registry
cannot protect against it (2026-08-28 incident). Never invoke `ctp.sh` on the
host, and never add a host-side recipe back.

**The testcase ref is never implicit.** Pass `PR=`, `TC_REF=`, or let the runner
infer the PR from `WORKSPACE`'s branch. With none of the three it refuses rather
than run develop testcases against a PR build. Refs are materialized as git
worktrees, so host checkouts (and their uncommitted edits) are never touched.

**medium and ha_shell are never sharded.** medium loads one mdb from a single
`data_file` tarball and its cases mutate it in place; an ha_shell shard is a
master+slave container pair. `SHARDS>1` is refused with the reason.

## Layout

| path | what |
|---|---|
| `scripts/ctp_run.sh` | the runner: split, mount, launch, aggregate, merge |
| `scripts/ctp_rerun.sh` | CI failure extraction → subset run |
| `scripts/entrypoint.sh` | current upstream entrypoint plus two local HA hooks, bind-mounted into the container |
| `scripts/harvest_weights.sh` | turn a finished run into a timing table |
| `baseline_weights.tsv` | measured per-case seconds (sql), for time-balanced splits |
| `colocate.tsv` | order-sensitive dirs that must stay on one shard |
| `test/run_tests.sh` | split, image-contract and HA regression checks; no CTP execution |
| `test/image_contract_test.sh` | runtime conf isolation, scope migration and exclusion planning fixtures |
| `test/locale_staging_test.sh` | real shard staging followed by CTP-style locale deletion |

The image is `cubridci/cubridci:test_rl8.10`, digest-pinned in `ctp_run.sh`. It is
never built locally; the CUBRID install is mounted in from the host (`just build`
output). The image includes gcc for locale and testcase helper compilation. Our entrypoint fork is bind-mounted over
`/entrypoint.sh`, so changing the runner never means rebuilding an image.
The default pull/run reference includes the full digest; `--image` explicitly
overrides it. Since the 2026-09-11 update, scope, testcase source pinning, DB
permissions and start/end provenance come from upstream (PRs #120, #121, #123).
Only the HA thin-csql port and slave broker startup remain local. See
`docs/adr/0017-ctp-runner-on-cubridci-image.md` for the migration decision.

## Suites

| suite | testcases repo | scenario | whole-suite shards | result style |
|---|---|---|---|---|
| `sql` | cubrid-testcases | `sql/` | 7 (bulk + measured time) | schedule `summary.xml` |
| `medium` | cubrid-testcases | `medium/` | always 1 | schedule `summary.xml` |
| `shell` | cubrid-testcases-private-ex | `shell/` | 7 (per test dir) | `test_status.data` + JUnit |
| `ha_shell` | cubrid-testcases-private | `HA/shell/` | always 1 (2 containers) | `test_status.data` + JUnit |

A **subset defaults to 1 shard** whatever the suite: each shard costs a full copy
of the install and a container, which pays for itself over 17k cases and not over
a handful of dirs. `SHARDS=N` splits a big subset anyway.

`sql`/`medium` split by case FILE (two-pass materialization); `shell`/`ha_shell`
split by test DIRECTORY, because a shell case's directory also carries the
`.answer` files and the `.c`/`.java`/helper `.sh` it compiles at run time.

An `ha_shell` shard is two containers: the master runs CTP, the slave runs the
image's `node` mode (sshd + a `qa` account CTP logs into). Each gets its OWN copy
of the install and of `CUBRID_DATABASES` — they run separate servers and rewrite
their own conf, so sharing those would make the pair clobber each other.
Inside each HA node, mount `CUBRID_DATABASES` at `$CUBRID/databases`: CTP cleans
that path between cases, including replication logs tied to the previous DB.

## Scope and exclusions

Use `just ctp <suite> <DIR>...` / `--only` to select scenario-relative directories.
The host partitions those cases before launch and sets upstream `TEST_SCENARIO`
to each shard's materialized scenario root. This keeps SQL exclusion paths relative
to the suite root even when only one directory is selected.

`EXCLUDE=<host-file> just ctp ...` (`--exclude <host-file>` in the runner) replaces
the default list **before planning** and stages the same list for upstream
`TEST_EXCLUDE`. Unset uses the suite default; `EXCLUDE=''` / `--exclude ''` disables
exclusions. Missing files fail before launch. SQL/medium entries are relative to
`sql/` or `medium/` without that prefix; shell entries include `shell/`, and HA
entries include `HA/shell/`. Entries use the target runner's substring semantics.

Use these host options instead of `--env TEST_SCENARIO=...` / `--env TEST_EXCLUDE=...`:
a container-only override would disagree with the host's planned case count.
Those `--env` names are rejected with guidance. The retired `CTP_SCENARIO`,
`CTP_EXCLUDE`, `SHELL_SCENARIO`, `HA_SCENARIO`, `MEMORY_SCENARIO` names also fail,
even when empty; do not use them in `CTP_ARGS`.

Upstream writes SQL, medium and shell overrides to `conf/<suite>_runtime.conf`.
HA derives `conf/ha_shell_ci.conf` from `shell_ci.conf`. Inspect these generated
files, not just the stock conf, when diagnosing scope. `CONF=` engine parameters
are still merged into the shard's stock conf before upstream copies it.

## Shell/HA locale baseline

CTP deletes generated locale libraries before every shell/HA case. If the build's
`cubrid_locales.txt` exactly matches its full sample `cubrid_locales.all.txt`, the
runner clears the active entries **in the private install copy only**, before CTP
snapshots it. Built-in locales then work after the deletion; cases that need extra
locales still call `do_make_locale` themselves. SQL/medium and independently
customized locale configs are unchanged. With that inherited preset, omit
`--overlay`: normalization requires a writable install copy and refuses to edit
the shared lower directory.

## Output

Every run writes, under its `--out` dir:

- `provenance.txt` / `provenance.tsv` — install, image digest, CTP revision,
  testcases repo@ref(sha), how the ref was chosen. Read this first when a result
  surprises you.
- `plan.tsv`, `assignment.tsv`, `units.tsv` — the split.
- `shard_N/` — `console.log` (the container's own log; primary evidence),
  `assigned_cases.txt`, `exclusions.txt`, `reports/` (JUnit), `cores/`, and the
  per-shard `CUBRID` / `CTP` / `testcases` / `CUBRID_DB` copies.
- `failed.list` — failing cases in the exact shape `--only` accepts.

## Reproducing CI failures

`ctp-rerun` takes a PR URL and finds the failed suites itself, or a single job URL.

- **CircleCI** (sql, medium): the public API needs no token; failing case paths
  come straight from `/api/v2/project/gh/CUBRID/cubrid/<job>/tests`.
- **GitHub Actions** (shell): the run uploads no artifacts — the complete
  `failed.list` exists only on the self-hosted runner's storage. What is readable
  is the `collect` job's log summary, which upstream caps at 50 rows. Past that
  cap only the first 50 are reproducible here; the rest need CI's `/run rerun <id>`.

It always uses 1 shard and re-runs whole test DIRECTORIES, not single cases: sql
cases in one directory share state, so a case run without its directory-mates
fails for reasons that have nothing to do with the change.

## Diagnosing a bad run

1. `provenance.txt` — right install? right testcase ref?
2. `shard_N/console.log` — the container's stdout, including the entrypoint's
   `[conf]`/`[scope]`/`[pin]`/`[provenance]` lines. Check the final `[scope]`
   line and runtime conf. Upstream may report `unknown` git metadata because
   shard copies omit `.git`; the host `provenance.tsv` records the source refs.
3. `shard_N/CTP/conf/<suite>*.conf` — the conf the entrypoint actually composed,
   preserved on the host.
4. `shard_N/cores/` — real core dumps. By default the first one stops every
   shard (`--no-abort-on-core` opts out); a crash-looping server once wrote 1.1T
   of cores.

Run `bash .agents/skills/ctp-run/test/run_tests.sh` from the tooling repo after
touching the runner. The fixtures cover split invariants, runtime conf isolation,
unset/empty/custom exclusions and retained HA hooks. For an image update, also
run small real SQL/shell/HA subsets through `just ctp` and compare executed
counts with the plan; fixtures alone do not validate the image.
