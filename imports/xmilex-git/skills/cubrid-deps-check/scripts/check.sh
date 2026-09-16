#!/usr/bin/env bash
# cubrid-deps-check — DIAGNOSTIC ONLY.
# Probes the build/test/skill prerequisites for a CUBRID workspace and prints a
# [OK]/[MISS]/[WARN] report plus a Summary line. It creates/modifies NO files,
# never executes any fix suggestion (suggestions are printed strings only), and
# exits 0 even when items are MISS. Non-zero exit ONLY on an internal failure.
set -u

# Hard-require the target CUBRID checkout as the first argument (no cwd default).
WORKSPACE="${1:?WORKSPACE required (pass the target CUBRID checkout)}"

# Tool-repo root (where the bundled locale lives), resolved from this script's
# canonical location: .agents/skills/cubrid-deps-check/scripts/check.sh -> 4 up.
self="$(readlink -f "$0" 2>/dev/null || echo "$0")"
REPO="$(cd "$(dirname "$self")/../../../.." 2>/dev/null && pwd || echo "")"

ok=0; miss=0; warn=0
emit() { # $1=OK|MISS|WARN  $2=message  $3=suggestion(optional, PRINTED not run)
  printf '[%s] %s\n' "$1" "$2"
  [ -n "${3:-}" ] && printf '       fix: %s\n' "$3"
  case "$1" in OK) ok=$((ok+1));; MISS) miss=$((miss+1));; WARN) warn=$((warn+1));; esac
}
have() { command -v "$1" >/dev/null 2>&1; }

echo "cubrid-deps-check — workspace: $WORKSPACE"
echo

# --- CUBRID source tree (the workspace itself) ---
if [ -f "$WORKSPACE/CMakePresets.json" ] && [ -f "$WORKSPACE/CMakeLists.txt" ]; then
  emit OK "CUBRID source tree ($WORKSPACE: CMakePresets.json + CMakeLists.txt)"
else
  emit MISS "CUBRID source tree at $WORKSPACE" "pass a CUBRID checkout; expected CMakePresets.json + CMakeLists.txt at its root"
fi

# --- Core build toolchain (MISS = blocks a build) ---
for c in cmake ninja gcc g++ just; do
  if have "$c"; then emit OK "build toolchain: $c"
  else emit MISS "build toolchain: $c not on PATH" "install $c (cmake>=3.21, ninja, gcc/g++ 8+, just)"; fi
done

# --- Runtime install (informational) ---
if [ -n "${CUBRID:-}" ] && [ -d "${CUBRID:-/nonexistent}" ]; then emit OK "runtime install (\$CUBRID=$CUBRID)"
elif [ -d "$HOME/CUBRID" ]; then emit OK "runtime install (~/CUBRID)"
else emit WARN "runtime install not found (\$CUBRID / ~/CUBRID)" "build & install it: WORKSPACE=$WORKSPACE just build"; fi

# --- CTP test platform (powers ctp-run) ---
ctp="${CTP_HOME:-$HOME/cubrid-testtools/CTP}"
if [ -x "$ctp/bin/ctp.sh" ]; then emit OK "CTP ($ctp/bin/ctp.sh)"
else emit WARN "CTP not found at $ctp/bin/ctp.sh" "clone cubrid-testtools and set CTP_HOME"; fi

# --- Test case repos (informational) ---
sqlcases="${TESTCASES_SQL:-$HOME/cubrid-testcases}"
[ -d "$sqlcases/sql" ] && emit OK "sql testcases ($sqlcases/sql)" \
  || emit WARN "sql testcases not found ($sqlcases/sql)" "clone cubrid-testcases or set TESTCASES_SQL"
shcases="${TESTCASES:-$HOME/cubrid-testcases-private-ex}"
[ -d "$shcases/shell" ] && emit OK "shell testcases ($shcases/shell)" \
  || emit WARN "shell testcases not found ($shcases/shell)" "clone cubrid-testcases-private-ex or set TESTCASES"

# --- CUBRID manual (powers cubrid-manual) ---
man="${CUBRID_MANUAL:-$HOME/cubrid-manual}"
if [ -d "$man/en" ] && [ -d "$man/ko" ]; then emit OK "cubrid-manual ($man/{en,ko})"
else emit WARN "cubrid-manual en/ko not found ($man)" "clone cubrid-manual or set CUBRID_MANUAL"; fi

# --- Per-skill tools (informational) ---
have podman && emit OK "podman (ctp-run)" || emit WARN "podman not found (ctp-run)" "install rootless podman — without it CTP cannot run at all (host-side CTP pkills every cub_* of this user); only ctp_run.sh --dry-run works"
# The runner pulls this on first use, so a missing image is informational, not a
# failure — but on an offline host it is the difference between working and not.
if have podman; then
  if podman image exists docker.io/cubridci/cubridci:test_rl8.10 2>/dev/null; then
    emit OK "CTP runner image (cubridci/cubridci:test_rl8.10)"
  else
    emit WARN "CTP runner image not pulled (cubridci/cubridci:test_rl8.10)" "podman pull docker.io/cubridci/cubridci:test_rl8.10  (the runner does this on first use; pre-pull for offline hosts)"
  fi
fi
have ssh   && emit OK "ssh (remote-claude/remote-codex)" || emit WARN "ssh not found" "install openssh client"
have tmux  && emit OK "tmux (remote-claude/remote-codex)" || emit WARN "tmux not found" "install tmux"
have node  && emit OK "node (remote-claude preseed.js)"   || emit WARN "node not found (remote-claude)" "install node"
if have gh; then
  if gh auth status >/dev/null 2>&1; then emit OK "gh authenticated (cubrid-pr-*)"
  else emit WARN "gh present but not authenticated" "authenticate the GitHub CLI (gh auth login)"; fi
else emit WARN "gh not found (cubrid-pr-create/review)" "install the GitHub CLI"; fi
have uv && emit OK "uv (jira)" || emit WARN "uv not found (jira)" "install uv (astral.sh)"
have cubrid-jira-search && emit OK "cubrid-jira-search (jira)" || emit WARN "cubrid-jira-search not on PATH (jira)" "install/link the cubrid-jira-search helper"

# --- Bundled locale lib (informational) ---
if [ -n "$REPO" ] && [ -f "$REPO/.claude/locale/libcubrid_all_locales.so" ]; then
  emit OK "prebuilt locale lib (.claude/locale/libcubrid_all_locales.so)"
else
  emit WARN "prebuilt locale lib missing (.claude/locale/libcubrid_all_locales.so)" "regenerate with .claude/locale/make_locale.sh"
fi

# --- Hard-gated skills (cannot be auto-verified from a shell; annotate) ---
emit WARN "frontend-design plugin (md-to-presentation)" "md-to-presentation ABORTS (hard-gate) without the frontend-design plugin"
emit WARN "Playwright MCP server (md-to-presentation)" "md-to-presentation ABORTS (hard-gate) without a Playwright MCP server"
emit WARN "jira hard-gate" "jira HALTS without uv + cubrid-jira-search"

echo
printf 'Summary: %d OK / %d MISS / %d WARN\n' "$ok" "$miss" "$warn"

# Diagnostic-only: MISS/WARN never cause a non-zero exit.
exit 0
