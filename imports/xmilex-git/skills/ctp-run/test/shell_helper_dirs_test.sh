#!/usr/bin/env bash
# shell_helper_dirs_test.sh — the shell-suite shard copy must ship helper dirs
# (common/, commonMethod/) that cases source by relative path, and nothing else.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SKILL="$(cd "$HERE/.." && pwd)"
ORCH="$SKILL/scripts/ctp_run.sh"
REPO="$(git -C "$SKILL" rev-parse --show-toplevel)"
SCRATCH_BASE="$REPO/.git_ignored_dir/scratch/ctp-run-selftest"
mkdir -p "$SCRATCH_BASE"
SCRATCH="$(mktemp -d "$SCRATCH_BASE/helpers.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT
PASS=0; FAIL=0
ok () { PASS=$((PASS + 1)); printf '[PASS] %s\n' "$*"; }
bad () { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$*" >&2; }

# shellcheck disable=SC1090
. <(sed -n '/^shell_helper_dirs() {/,/^}/p' "$ORCH")
declare -F shell_helper_dirs >/dev/null || { bad "shell_helper_dirs missing in ctp_run.sh"; exit 1; }

scn="$SCRATCH/shell"
mkdir -p "$scn/_10_x/iso/common" "$scn/_10_x/iso/_01/test01/cases" "$scn/_10_x/iso/_01/test02/cases" \
         "$scn/_20_y/case_a/cases" "$scn/_20_y/case_a/src/util" "$scn/_30_z/grp/commonMethod/sub" \
         "$scn/_30_z/grp/c1/cases" "$scn/config" "$scn/_40_empty"
touch "$scn/_10_x/iso/common/lib.sh" "$scn/_10_x/iso/_01/test01/cases/test01.sh" \
      "$scn/_20_y/case_a/cases/a.sh" "$scn/_20_y/case_a/src/util/x.c" \
      "$scn/_30_z/grp/commonMethod/m.sh" "$scn/_30_z/grp/commonMethod/sub/n.sh" \
      "$scn/_30_z/grp/c1/cases/c1.sh" "$scn/config/exclude.list"

got="$(shell_helper_dirs "$scn" | LC_ALL=C sort | tr '\n' ' ')"
want="_10_x/iso/common _30_z/grp/commonMethod _40_empty "
if [ "$got" = "$want" ]; then
  ok "helper dirs = outermost non-case dirs only (got: $got)"
else
  bad "helper dirs mismatch: got '$got' want '$want'"
fi
case " $got " in
  *" _20_y/case_a/src "*|*" _20_y/case_a/src/util "*) bad "a dir inside a case dir was reported as a helper" ;;
  *) ok "dirs inside a case dir are not helpers" ;;
esac
case " $got " in
  *" _30_z/grp/commonMethod/sub "*) bad "a nested helper dir was reported separately" ;;
  *) ok "only the outermost helper dir is reported" ;;
esac
case " $got " in
  *" config "*) bad "config/ reported as a helper" ;;
  *) ok "config/ is excluded" ;;
esac
printf 'RESULT: %d PASS / %d FAIL\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
