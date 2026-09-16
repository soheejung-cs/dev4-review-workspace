# ctp-run — runtime and design notes

The user-facing workflow is [SKILL.md](SKILL.md). The design decision is
[ADR 0017](../../../docs/adr/0017-ctp-runner-on-cubridci-image.md).
All commands below run from the standalone tooling repository, not an engine checkout.

## Image update (2026-09-11)

The default image is `docker.io/cubridci/cubridci:test_rl8.10`, pulled and run by
full digest from `scripts/ctp_run.sh`. The September update uses
`sha256:79a344fc7664af48cd13b4b01bc54c059dd92ac2be812f2e4d9802234e06fb7e`.
The image's entrypoint matches upstream commit
`452b7be0cee601b9efd59a1d9c9faf89886d255f`.

- [#120](https://github.com/CUBRID/cubridci/pull/120): prepare the node account's DB directory and ownership.
- [#121](https://github.com/CUBRID/cubridci/pull/121): disable shell/rqg testcase updates and print start/end provenance.
- [#123](https://github.com/CUBRID/cubridci/pull/123): common `TEST_SCENARIO` / `TEST_EXCLUDE` and fresh runtime confs.

`scripts/entrypoint.sh` is that upstream version plus **only two HA additions**
described below. It is bind-mounted over `/entrypoint.sh`; the image is never
built locally. A host-built CUBRID install is copied into each shard. There is
no local `Containerfile` to build.

`--image <ref>` deliberately overrides the pinned default. Changing only the
image to an older version does not roll back the entrypoint or option contract;
restore those together. Old image layers are retained for rollback.

## Scope and exclusion options

```bash
# Select the testcase ref explicitly. Each example selects one directory.
TC_REF=develop just ctp sql _01_object/_01_type/_004_integer

# Replace the default exclusions with a host file.
TC_REF=develop EXCLUDE="$PWD/my-exclusions.txt" just ctp sql _01_object/_01_type/_004_integer

# Explicitly disable exclusions, including the host planner's defaults.
TC_REF=develop EXCLUDE='' just ctp sql _01_object/_01_type/_004_integer

# Inspect a plan without launching CTP.
TC_REF=develop CTP_ARGS='--dry-run' just ctp sql _01_object/_01_type/_004_integer
```

The runner uses `--only <scenario-relative-dir>` for the selected directories and
`--exclude <host-file>` for the list. It applies both before planning and copying
the shard. Each container receives `TEST_SCENARIO` naming the materialized suite
root, plus `TEST_EXCLUDE` naming the staged list (`conf/ctprun_exclusions.txt`),
or an empty value for explicit exclusion disabling. An unreadable list fails
before container startup.

| List entry | Meaning |
|---|---|
| SQL/medium: `_01_object/.../cases/example.sql` | path relative to the suite root; omit `sql/` or `medium/` |
| shell: `shell/_06_issues/.../cases/example.sh` | substring including the category prefix |
| ha_shell: `HA/shell/.../cases/example.sh` | substring including the HA category prefix |

A custom list replaces, rather than augments, the default. To add exclusions,
prepare a host file containing both the default entries and the additions.
The SQL scenario remains the suite root inside each shard, so subset selection
does not change the root against which SQL exclusions are interpreted.

`--env TEST_SCENARIO` and `--env TEST_EXCLUDE` are rejected: a container-only
change would bypass the host plan and its executed-count checks. Use the host
options above. The retired `CTP_SCENARIO`, `CTP_EXCLUDE`, `SHELL_SCENARIO`,
`HA_SCENARIO`, `MEMORY_SCENARIO` names also fail, including empty values.
Other `--env NAME=VALUE` values still pass to every container, e.g.:

```bash
TC_REF=develop CTP_ARGS='--env CUBRID_WM_SORT_NEW=1' just ctp sql _01_object/_01_type/_004_integer
```

The image itself has a broader category API than this runner. In upstream,
`jdbc` accepts neither scope option and `sql_by_cci` accepts only `TEST_SCENARIO`.
See [Running part of a category](https://github.com/CUBRID/cubridci/blob/452b7be0cee601b9efd59a1d9c9faf89886d255f/README.md#running-part-of-a-category)
for the other runners' exclusion semantics.

## Runtime configuration and provenance

Upstream copies `sql.conf`, `medium_dev.conf`, and `shell_ci.conf` to
`sql_runtime.conf`, `medium_runtime.conf`, and `shell_runtime.conf` respectively.
For HA it derives `ha_shell_ci.conf` from `shell_ci.conf`. Scope and source pinning
change those generated files. `CONF=<cubrid.conf>` still merges engine parameters
into the shard's source conf before upstream copies it; the host CTP is not edited.

Look at `[scope]` and `[pin]` in `shard_N/console.log` and the generated conf to
verify the effective values. Upstream prints `[provenance]` before and after CTP.
Because shard copies do not include `.git`, git fields there can be `unknown`;
the host `provenance.txt` / `provenance.tsv` records the selected source ref/SHA,
CTP revision, install, image digest and exclusion option.

## Shell/HA locale baseline

CTP removes generated locale libraries before each shell/HA case. A build already
used for SQL can carry `cubrid_locales.txt` identical to `cubrid_locales.all.txt`;
its enabled locales then require the deleted library even for ordinary utilities.
The runner recognizes only that exact preset and clears its active entries in the
private install copy, before CTP snapshots it. Built-in locales remain available.
SQL/medium keep their full preset, and custom locale configurations are preserved.
Locale-specific cases can still generate their own libraries. `--locale-dir` supplies
a library and optional build script, not a locale configuration file.

For that inherited preset, `--overlay` fails with a request to omit the flag;
the shared source install is never normalized in place. `test/locale_staging_test.sh`
exercises real shard construction followed by CTP-style deletion, including both
HA install copies and preservation of SQL/medium/custom settings.

## HA remote csql setup

`just ctp ha_shell ...` derives the thin-csql broker port from CTP's
`default.broker2.BROKER_PORT` and supplies it to both nodes' non-login SSH
environments. Missing, duplicate, or invalid ports stop preparation. For folded
clients, the runner patches only its private CTP helper copy to start the slave
broker after HA configuration upload and heartbeat startup. These two additions
remain local until the engine parameter and CTP support reach upstream.

The pair has separate install and database copies. Mount each node's DB directory
**over `$CUBRID/databases`**; an external container path is incompatible with HA
scripts and cases that directly access that location. The upstream ownership fix
continues to apply, but ownership alone cannot make a different path work.

## Isolation and splitting

CTP teardown kills processes by user/name. Every CTP execution therefore goes
through `just ctp` / `just ctp-rerun` in private rootless-podman containers.
Network, IPC, mount and cgroup namespaces are private; the common flags include
`--cgroupns=private` for this Rocky 8 host. Raw podman probes without that flag
can fail even when the runner works.

SQL splits by top-level category by default, with measured per-case weights
(`baseline_weights.tsv`) and greedy LPT balancing. `--by-dir` and `--by-case`
provide finer opt-ins; `colocate.tsv` keeps registered case directories together.
Shell splits by test directory and copies shared helper directories alongside
it. SQL/medium copy only assigned case files; shell/HA copy whole test directories
with their answer files, helpers and source files.

Default whole-suite concurrency is 7 for SQL/shell, 1 for subsets. Medium and
HA always use one shard: medium mutates a shared dataset, and an HA shard is
already a master/slave pair. The offline validator proves each planned case is
assigned exactly once. SQL/medium executed totals must match the plan; shell/HA
may skip cases by macro, but a run executing nothing is rejected.

The selected exclusion list is snapshotted for planning and staged unchanged
in every shard. SQL uses CTP's `containPath` rules; shell/HA use raw substring
matching, including individual `.sh` entries. No complement-of-shard list is
needed because unassigned test files/directories are not materialized.

SQL/medium results can be merged into the host CTP webconsole result tree.
`--no-webconsole` skips that merge. `--merge-only <finished-run-dir>` performs
only the merge, without starting containers. Timing weights can be refreshed
with `scripts/harvest_weights.sh` from a completed run.

## Verification

```bash
bash .agents/skills/ctp-run/test/run_tests.sh
```

This includes split invariants, the image scope contract, runtime conf isolation,
unset/empty/custom exclusion planning, invalid-option rejection, and the two HA
hooks. `test/image_contract_test.sh`, `test/locale_staging_test.sh` and
`test/ha_shell_test.sh` use fixtures and
never execute CTP. Scratch stays under `.git_ignored_dir/scratch/`.

For an image update, also run small real SQL, shell and HA subsets via `just ctp`.
Compare a SQL run with `EXCLUDE=''` to one with a custom list: excluded cases must
be absent, actual totals must match the plan, and the source conf must remain
unchanged by the entrypoint. For HA, verify both nodes and query results as well
as the generated config and DB ownership. Inspect only containers owned by that
run; keep other tasks' containers and host servers untouched.
