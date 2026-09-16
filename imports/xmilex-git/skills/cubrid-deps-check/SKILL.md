---
name: cubrid-deps-check
description: Diagnose whether a CUBRID workspace has the build, test, and per-skill dependencies the CUBRID tooling needs, printing a read-only [OK]/[MISS]/[WARN] report with echoed fix suggestions. Use when setting up a new machine or worker, before a build/test campaign, or when a CUBRID skill fails and you need to know which prerequisite is missing.
---

# CUBRID Dependency Check

Read-only probe of the prerequisites for building and testing CUBRID and for running the
bundled skills. It **diagnoses, never mutates**: it creates and modifies no files, and every
fix suggestion is a *printed string only* — nothing is executed.

## Usage

The target CUBRID checkout is **required** as the first argument (no cwd default — this tooling
repo is not a CUBRID checkout):

```bash
WORKSPACE="${1:?WORKSPACE required (pass the target CUBRID checkout)}"
.claude/skills/cubrid-deps-check/scripts/check.sh "$WORKSPACE"
```

Run it from this tooling repo's root, e.g. `… scripts/check.sh ~/dev/cubrid`.

## What it reports

One line per item, `[OK]` / `[MISS]` / `[WARN]`, followed by a `fix:` line for anything not OK,
ending with `Summary: N OK / M MISS / K WARN`.

- **MISS** — a build-blocking dependency is absent: the workspace is not a CUBRID source tree
  (`CMakePresets.json` + `CMakeLists.txt`), or `cmake` / `ninja` / `gcc` / `g++` / `just` is off PATH.
- **WARN** — an optional or per-skill dependency is absent: `~/CUBRID` runtime, CTP
  (`CTP_HOME`), the testcase repos, `cubrid-manual`, `podman`, `ssh`/`tmux`, `node`, `gh` (auth),
  `uv`, `cubrid-jira-search`, the prebuilt locale `.so`.
- **Hard-gated skills** are always annotated so you know what stays blocked:
  - `md-to-presentation` ABORTS without the frontend-design plugin **and** a Playwright MCP server.
  - `jira` HALTS without `uv` + `cubrid-jira-search`.

## Contract

- **Diagnostic only.** No file is created or modified; no scratch, no `mkdir`.
- **`[MISS]` is not a failure.** The script always exits `0` for a normal report (even with
  missing deps). A non-zero exit means the *check itself* failed (e.g. no `WORKSPACE` passed).
- **Suggestions are never run.** The `fix:` strings are informational; act on them yourself.
- **Idempotent.** Same environment ⇒ identical output (no timestamps, no randomness).

## How to use the result

1. Resolve every **MISS** first — a build cannot proceed without the core toolchain.
2. Resolve the **WARN**s that matter for the skill you intend to run (e.g. CTP for
   `podman` + the pinned CI image for `ctp-run`, the hard-gates for `md-to-presentation`).
3. Re-run to confirm. The report is safe to run as often as you like.
