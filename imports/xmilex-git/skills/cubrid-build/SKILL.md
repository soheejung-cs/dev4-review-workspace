---
name: cubrid-build
description: CUBRID build and test workflow using the portable justfile. The CUBRID source directory is passed explicitly via WORKSPACE (the justfile lives in this standalone tooling repo, not inside the checkout). Use when building, compiling, installing, or testing CUBRID source code in any CUBRID worktree or source directory.
---

# CUBRID Build & Test

Build, install, and test CUBRID via this repo's portable `justfile` (drives the target
checkout's tracked `CMakePresets.json`; installs to a per-mode versioned dir and repoints `~/CUBRID`).

## Workspace (required)

This tooling repo is **not** a CUBRID checkout, so the build recipes cannot use the current
directory. Pass the CUBRID source directory explicitly via `WORKSPACE` (no cwd default):

```bash
WORKSPACE=/path/to/cubrid just build          # env var
just workspace=/path/to/cubrid build          # just variable
```

Run `just` from **this tooling repo's root** (where the `justfile` and `.claude/locale/` live).
Source-touching recipes — `build`, `configure`, `rebuild`, `ctest`, `deploy` — operate on
`$WORKSPACE` and fail fast if it is unset or is not a CUBRID checkout (no `CMakePresets.json`).
The `use`, `conf`, and `install-locale` recipes act on the install tree / `$CUBRID` and need no
`WORKSPACE`.

## When to Use

- User says "build", "compile", "install", "test", "빌드", "테스트"
- After code edits, to verify compilation or run tests
- Switching the active build between debug and release
- Any time you have a CUBRID source tree/worktree to build (contains `src/storage/`, `src/parser/`, `CMakePresets.json`) — pass it as `WORKSPACE`

## Prerequisites

- A CUBRID source tree/worktree (with `CMakePresets.json` at its root) passed via `WORKSPACE`;
  run `just` from this tooling repo (which holds the `justfile` + `.claude/locale/`)
- Toolchain: `just`, `cmake` (>=3.21), `ninja`, `gcc`/`g++` (8+)
- `$CUBRID` set to the runtime dir (e.g. `~/CUBRID`); `$HOME` set
- First build auto-inits the `cubrid-cci` submodule (needs network once)
- No `$PRESET_MODE` / `$CUBRID_BUILD_DIR` / direnv needed — the justfile is self-contained

## Build Modes

- **debug** — `CMAKE_BUILD_TYPE=Debug`, assertions on. Use for correctness / stability / crash work.
- **release** — `RelWithDebInfo`, optimized. Use for performance measurement.

Each mode installs to its OWN dir `~/<mode>/CUBRID-<version>` (default version `11.5.develop`,
override with `CUBRID_VERSION`) and repoints `~/CUBRID` -> that dir. debug and release never
clobber each other; switch the active one with `just use <mode>` (no rebuild).

## Commands

### Build + install (default: debug)
Prefix source-touching recipes with `WORKSPACE=<cubrid-src>` (shown once here; required on every
`build`/`configure`/`rebuild`/`ctest`/`deploy` call):
```bash
WORKSPACE=~/dev/cubrid just build          # debug
WORKSPACE=~/dev/cubrid just build release  # release
WORKSPACE=~/dev/cubrid just debug          # alias for: just build debug
WORKSPACE=~/dev/cubrid just release        # alias for: just build release
WORKSPACE=~/dev/cubrid just build debug 11.5.x  # explicit version label
```
Builds `build_preset_<mode>/` and installs to `~/<mode>/CUBRID-<version>`, then points
`~/CUBRID` there. **Use this to verify edits compile.** Never call `cmake --build` directly.

### Switch active install (no rebuild)
```bash
just use release         # repoint ~/CUBRID -> ~/release/CUBRID-<version>
just use debug
```

### Fresh rebuild (wipes the build tree first)
```bash
WORKSPACE=~/dev/cubrid just rebuild          # debug
WORKSPACE=~/dev/cubrid just rebuild release
```

### Configure only
```bash
WORKSPACE=~/dev/cubrid just configure        # debug
WORKSPACE=~/dev/cubrid just configure release
```

### Apply campaign test conf (idempotent)
```bash
just conf                # server=demodb, thread_worker_timeout_seconds=4, double_write_buffer_size=0
```

### Locale files (auto-handled by build)
`just build` / `just rebuild` automatically copy the prebuilt locale files from
`.claude/locale/` into the install (`libcubrid_all_locales.so` -> `lib/`,
`make_locale.sh` -> `bin/`) — the all-locales lib is needed for CTP execution and this
avoids the slow `make_locale` rebuild. To (re)copy manually into the current `$CUBRID`
(or a given dir):
```bash
just install-locale          # into $CUBRID
just install-locale <dir>
```

### Full local refresh
```bash
WORKSPACE=~/dev/cubrid just deploy           # stop server (if any) -> build debug -> conf
WORKSPACE=~/dev/cubrid just deploy release
```

### Tests
```bash
WORKSPACE=~/dev/cubrid just ctest            # ctest (unit + sql-level) against build_preset_debug
WORKSPACE=~/dev/cubrid just ctest release
```

## Typical Workflow

1. Edit code
2. `WORKSPACE=<src> just build` (or `... just build release`) — verify it compiles + installs
3. `WORKSPACE=<src> just ctest` — run unit + sql-level tests
4. Switch modes anytime with `just use <mode>` (no rebuild; no `WORKSPACE` needed)

## Important

- **Always use `just`**, never raw `cmake --build` / `ctest`.
- Run long builds with `run_in_background`.
- `just build` / `rebuild` / `deploy` **REPOINT `~/CUBRID`**. To preserve the current target,
  note `readlink ~/CUBRID` first, or build with a throwaway `CUBRID_VERSION=<label>` and restore after.
- If a build fails, read the error output carefully before attempting fixes.
- `just --list` shows all available recipes.
