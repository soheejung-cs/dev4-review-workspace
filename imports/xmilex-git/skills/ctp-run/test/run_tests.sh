#!/usr/bin/env bash
#
# run_tests.sh — static + logic self-tests for ctp-run. Runs WITHOUT podman.
#
# Drives the orchestrator's pure-logic paths (--dry-run / --validate-only) against
# the REAL testtools + testcases trees on this machine and asserts every property
# from SPEC.md "Verification". Real container launch is out of scope here (no
# podman); see README.md "Manual e2e QA".
#
# Usage:
#   bash run_tests.sh [--testcases <root>] [--ctp <CTP_HOME>]
# Defaults: --testcases $HOME/cubrid-testcases  --ctp $HOME/cubrid-testtools/CTP

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SKILL="$(cd "$HERE/.." && pwd)"
ORCH="$SKILL/scripts/ctp_run.sh"
ENTRY="$SKILL/scripts/entrypoint.sh"

TC="${HOME}/cubrid-testcases"
CTP="${HOME}/cubrid-testtools/CTP"
while [ $# -gt 0 ]; do
  case "$1" in
    --testcases) TC="$2"; shift 2 ;;
    --ctp)       CTP="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
SCN="$TC/sql"

# Keep all scratch on disk-backed storage (NOT /tmp).
REPO="$(git -C "$SKILL" rev-parse --show-toplevel)"
mkdir -p "$REPO/.git_ignored_dir/scratch/ctp-selftests"
SCRATCH="$(mktemp -d "$REPO/.git_ignored_dir/scratch/ctp-selftests/run.XXXXXX")"
export TMPDIR="$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

PASS=0
FAILED=0
note() { printf '  - %s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '[PASS] %s\n' "$*"; }
bad()  { FAILED=$((FAILED+1)); printf '[FAIL] %s\n' "$*" >&2; }
require_files() {
  [ -r "$ORCH" ]  || { echo "missing orchestrator: $ORCH" >&2; exit 2; }
  [ -d "$SCN" ]   || { echo "missing scenario tree: $SCN" >&2; exit 2; }
  [ -r "$CTP/conf/sql.conf" ] || { echo "missing CTP template: $CTP/conf/sql.conf" >&2; exit 2; }
}
require_files

echo "=================================================================="
echo " ctp-run self-tests"
echo "   orchestrator : $ORCH"
echo "   testcases    : $SCN"
echo "   CTP_HOME     : $CTP"
echo "=================================================================="

#-------------------------------------------------------------------
# (a) Static lint — bash -n on every script (+ shellcheck if available)
#-------------------------------------------------------------------
echo; echo "## (a) static lint"
if bash -n "$ORCH"; then ok "bash -n clean: ctp_run.sh"; else bad "bash -n FAILED: ctp_run.sh"; fi
if bash -n "$ENTRY"; then ok "bash -n clean: entrypoint.sh"; else bad "bash -n FAILED: entrypoint.sh"; fi
if command -v shellcheck >/dev/null 2>&1; then
  if shellcheck -S warning "$ORCH" "$ENTRY"; then ok "shellcheck clean"; else bad "shellcheck reported issues"; fi
else
  note "shellcheck absent — bash -n only"
fi

#-------------------------------------------------------------------
# Helper: run a dry-run for a given N, capture stdout + artifacts.
#-------------------------------------------------------------------
dryrun() { # <N> <outdir>  -> writes log to <outdir>.log
  # --no-weights so the structural / invariant / count-balance checks are stable and
  # independent of the bundled time table (auto-weights is covered separately in (j)).
  local n="$1" out="$2"
  rm -rf "$out"
  bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards "$n" --no-weights --out "$out" >"$out.log" 2>&1
  return $?
}

#-------------------------------------------------------------------
# (b) Leaf/unit discovery == independent direct find
#-------------------------------------------------------------------
echo; echo "## (b) unit discovery"
OUT10="$SCRATCH/out10"
if dryrun 10 "$OUT10"; then
  units_plan="$(wc -l < "$OUT10/units.tsv")"
  # DEFAULT unit = top-level _* "bulk" (CI's sql unit). Recompute independently:
  # the first path component of every */cases/*.sql.
  units_find="$(find "$SCN" -type f -name '*.sql' -path '*/cases/*' \
                  | sed "s#^${SCN}/##" | sed -E 's#/.*##' | sort -u | wc -l)"
  glob_bulks="$(ls -d "$SCN"/_*/ 2>/dev/null | wc -l)"
  note "bulk(_*) units (plan)=$units_plan  (direct find)=$units_find  ; sql/_* dirs=$glob_bulks"
  if [ "$units_plan" -eq "$units_find" ]; then ok "unit discovery matches direct find ($units_plan bulks)"; else bad "unit discovery $units_plan != find $units_find"; fi
  if [ "$units_plan" -ge 20 ] && [ "$units_plan" -le 60 ]; then ok "bulk count in expected ~35 range ($units_plan)"; else bad "bulk count $units_plan out of range"; fi
else
  bad "dry-run N=10 failed"; cat "$OUT10.log" >&2
fi

#-------------------------------------------------------------------
# (c) Partition correctness for N in {1,4,10}: disjoint + union == all leaves
#-------------------------------------------------------------------
echo; echo "## (c) partition correctness N in {1,4,10}"
ALL_UNITS="$SCRATCH/all_units.txt"
awk -F'\t' '{print $1}' "$OUT10/units.tsv" | sort > "$ALL_UNITS"
total_units="$(wc -l < "$ALL_UNITS")"
for N in 1 4 10; do
  OUT="$SCRATCH/out$N"
  if ! dryrun "$N" "$OUT"; then bad "dry-run N=$N failed"; cat "$OUT.log" >&2; continue; fi
  asg="$OUT/assignment.tsv"
  rows="$(wc -l < "$asg")"
  uniq_units="$(awk -F'\t' '{print $1}' "$asg" | sort -u | wc -l)"
  union_match="$(awk -F'\t' '{print $1}' "$asg" | sort | comm -3 - "$ALL_UNITS" | wc -l)"
  # disjoint: no unit assigned to >1 shard  => rows == uniq_units
  # union   : assigned set == all units      => union_match == 0 AND uniq_units == total_units
  # shard range: every shard idx in [0,N)
  badidx="$(awk -F'\t' -v n="$N" '$2<0 || $2>=n' "$asg" | wc -l)"
  if [ "$rows" -eq "$uniq_units" ] && [ "$uniq_units" -eq "$total_units" ] && [ "$union_match" -eq 0 ] && [ "$badidx" -eq 0 ]; then
    ok "N=$N: leaf-sets disjoint & union==all leaves ($uniq_units units, shards in range)"
  else
    bad "N=$N: rows=$rows uniq=$uniq_units total=$total_units union_diff=$union_match badidx=$badidx"
  fi
done

#-------------------------------------------------------------------
# (d) Offline split-validator passes on the real tree (already runs inside dry-run)
#     + synthetic ambiguous fixture must be FLAGGED (proves not a no-op)
#-------------------------------------------------------------------
echo; echo "## (d) split-validator"
if grep -q "split valid: all .* surviving .sql alive in exactly one shard" "$OUT10.log"; then
  ok "validator passes on real tree (0 duplicates, 0 orphans)"
else
  bad "validator did not confirm clean split on real tree"; grep -i 'validat\|orphan\|duplicate' "$OUT10.log" >&2
fi
# Synthetic fixture: an inner .sql under a NESTED cases dir, with the outer and
# inner units assigned to DIFFERENT shards -> ambiguous substring match -> orphan.
FX_ASG="$SCRATCH/fx_assign.tsv"; FX_SQL="$SCRATCH/fx_sql.txt"
printf '/a/cases\t0\n/a/cases/sub/cases\t1\n' > "$FX_ASG"
printf '/a/cases/x.sql\n/a/cases/sub/cases/y.sql\n' > "$FX_SQL"
if bash "$ORCH" --validate-only "$FX_ASG" "$FX_SQL" >"$SCRATCH/fx.log" 2>&1; then
  bad "validator did NOT flag the ambiguous fixture (false negative)"; cat "$SCRATCH/fx.log" >&2
else
  if grep -q 'ORPHAN' "$SCRATCH/fx.log"; then ok "validator FLAGS ambiguous substring fixture (orphan, non-zero exit)"; else bad "validator exited non-zero but without an ORPHAN diagnosis"; fi
fi
# Control: a clean disjoint fixture must PASS.
printf '/a/cases\t0\n/b/cases\t1\n' > "$SCRATCH/fxok_a.tsv"
printf '/a/cases/x.sql\n/b/cases/y.sql\n' > "$SCRATCH/fxok_s.txt"
if bash "$ORCH" --validate-only "$SCRATCH/fxok_a.tsv" "$SCRATCH/fxok_s.txt" >/dev/null 2>&1; then
  ok "validator passes a clean disjoint fixture (control)"
else
  bad "validator wrongly flagged a clean disjoint fixture"
fi

#-------------------------------------------------------------------
# (e) Invariant: sum surviving(shard) == global - base_excluded
#-------------------------------------------------------------------
echo; echo "## (e) split invariant"
g="$(awk '/global \.sql:/{print $NF}'        "$OUT10.log")"
b="$(awk '/base-excluded \.sql:/{print $NF}' "$OUT10.log")"
s="$(awk '/surviving \.sql:/{print $NF}'     "$OUT10.log")"
sum_shard="$(awk -F'\t' 'NR>1{s+=$2} END{print s+0}' "$OUT10/plan.tsv")"
sum_units="$(awk -F'\t' '{s+=$2} END{print s+0}' "$OUT10/units.tsv")"
note "global=$g base_excluded=$b surviving=$s ; sum(shard sql)=$sum_shard sum(unit sql)=$sum_units"
if [ "$s" -eq "$((g - b))" ] && [ "$sum_shard" -eq "$s" ] && [ "$sum_units" -eq "$s" ]; then
  ok "invariant holds: surviving==global-base==Σshard==Σunit ($s)"
else
  bad "invariant violated: g=$g b=$b s=$s Σshard=$sum_shard Σunit=$sum_units"
fi

#-------------------------------------------------------------------
# (f) Balance sanity (--by-dir: fine units balance well) + bulk atomicity (default)
#-------------------------------------------------------------------
echo; echo "## (f) balance (--by-dir, N=10) + bulk atomicity (default)"
# Balance is measured with --by-dir: the default bulk(_*) unit is intentionally coarse
# (a bulk is atomic, so the heaviest bulk bounds the slowest shard); fine units show the
# greedy-LPT quality. Real bulk runs balance by TIME via --weights.
OUTF="$SCRATCH/outf_bydir"
bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 10 --by-dir --no-weights --out "$OUTF" >"$OUTF.log" 2>&1
read -r mx mean ratio_ok < <(awk -F'\t' '
  NR>1 { c[n++]=$2; sum+=$2; if($2>mx) mx=$2 }
  END { mean=sum/n; printf "%d %.1f %d\n", mx, mean, (mx <= 1.5*mean) }
' "$OUTF/plan.tsv")
note "by-dir max-shard=$mx mean=$mean  (max <= 1.5*mean ? $ratio_ok)"
if [ "$ratio_ok" -eq 1 ]; then ok "balanced (--by-dir): max-shard $mx <= 1.5 x mean $mean"; else bad "imbalanced (--by-dir): max-shard $mx > 1.5 x mean $mean"; fi
# Bulk atomicity: in the default plan, each top-level _* bulk lands on exactly ONE shard.
splitn="$(awk 'FNR==1{ match(FILENAME,/shard_([0-9]+)/,m); sid=m[1] }
               { b=$0; sub(/\/.*/,"",b); seen[b"\t"sid]=1; bulks[b]=1 }
               END{ for(k in seen){split(k,a,"\t"); c[a[1]]++} s=0; for(b in bulks) if(c[b]>1) s++; print s+0 }' \
             "$OUT10"/shard_*/assigned_cases.txt 2>/dev/null)"
if [ "${splitn:-1}" -eq 0 ]; then ok "bulk atomicity: no _* bulk split across shards"; else bad "bulk atomicity: $splitn bulk(s) split across shards"; fi

#-------------------------------------------------------------------
# (g) Container contract. The per-shard CTP conf is no longer generated on the
# host — the entrypoint fork composes it inside the container, where the paths
# are the container's. So what has to hold is the fork's contract itself.
#-------------------------------------------------------------------
echo; echo "## (g) container contract (entrypoint fork + mount layout)"
cfg_ok=1
grep -q 'apply_scope' "$ENTRY" || { cfg_ok=0; note "entrypoint lost upstream apply_scope"; }
grep -q 'testcase_update_yn=false' "$ENTRY" || { cfg_ok=0; note "entrypoint fork no longer forces testcase_update_yn=false (CTP would git-pull the testcases and switch to develop)"; }
grep -q 'mount a testcases tree there' "$ENTRY" || { cfg_ok=0; note "entrypoint lost upstream mount guidance"; }
grep -q 'TEST_SCENARIO' "$ORCH" || { cfg_ok=0; note "orchestrator does not pin TEST_SCENARIO"; }
# The scenario mount must be repo-relative, so the STOCK CTP confs (which resolve
# ${HOME}/<repo>/...) are correct with no rewriting — including medium's data_file.
grep -q 'C_SCN="$C_TCREPO/$SUITE_SUBPATH"' "$ORCH" || { cfg_ok=0; note "scenario mount is not repo-relative"; }
grep -q 'readonly C_CTP="/home/cubrid-testtools/CTP"' "$ORCH" || { cfg_ok=0; note "CTP mount does not match the image's CTP_HOME"; }
grep -qE 'DEFAULT_IMAGE_DIGEST="sha256:[0-9a-f]{64}"' "$ORCH" || { cfg_ok=0; note "image is not digest-pinned"; }
if [ "$cfg_ok" -eq 1 ]; then
  ok "container contract: overrides present, testcase auto-update forced off, mounts match the image, image digest-pinned"
else
  bad "container contract check failed"
fi

# (g2) Suite policy: medium and ha_shell must refuse to shard.
for s_ in medium ha_shell; do
  case "$s_" in
    medium)   tcdir="$TC" ;;
    ha_shell) tcdir="$HOME/cubrid-testcases-private" ;;
  esac
  if [ ! -d "$tcdir" ]; then note "$s_: no testcases checkout, skipped"; continue; fi
  if bash "$ORCH" --suite "$s_" --shards 3 --dry-run --testcases "$tcdir" --testcases-as-is \
        --ctp "$CTP" --out "$SCRATCH/refuse_$s_" >"$SCRATCH/refuse_$s_.log" 2>&1; then
    bad "$s_: --shards 3 was accepted; it must be refused"
  else
    grep -q 'cannot be sharded' "$SCRATCH/refuse_$s_.log" \
      && ok "$s_: --shards 3 refused with the reason" \
      || bad "$s_: failed, but not with the shard-refusal reason"
  fi
done

# (g4) --conf must land where the suite's server actually reads it: merged into
# the [<cat>/cubrid.conf] SECTION of CTP's own conf for sql/medium (CTP writes the
# server conf from there), and over the install's conf for shell/HA (the cases
# start their own servers). A file merely dropped beside CTP's conf does nothing.
CM="$SCRATCH/confmerge"
rm -rf "$CM"; mkdir -p "$CM/CTP/conf" "$CM/CUBRID/conf"
if [ -r "$CTP/conf/sql.conf" ]; then
  cp "$CTP/conf/sql.conf" "$CM/CTP/conf/"
  printf 'cubrid_port_id=1755\nnew_param_xyz=42\n' > "$CM/user.conf"
  ( set -e
    info(){ :; }; die(){ echo "DIE: $*" >&2; exit 1; }
    ARG_CONF="$CM/user.conf"; ARG_OVERLAY=0
    SUITE_STYLE=sqlresult; SUITE_CONF=conf/sql.conf; SUITE_CAT=sql; ARG_SUITE=sql
    eval "$(sed -n '/^install_suite_conf() {/,/^}/p' "$ORCH")"
    install_suite_conf "$CM" ) >/dev/null 2>&1
  sec="$(sed -n '/^\[sql\/cubrid.conf\]/,/^\[sql\/cubrid_ha/p' "$CM/CTP/conf/sql.conf")"
  ok1=0; printf '%s' "$sec" | grep -qx 'cubrid_port_id=1755' && ok1=1
  ok2=0; printf '%s' "$sec" | grep -qx 'new_param_xyz=42' && ok2=1
  ok3=1; printf '%s' "$sec" | grep -qx 'cubrid_port_id=1822' && ok3=0
  if [ "$ok1$ok2$ok3" = "111" ]; then
    ok "--conf: existing key replaced in [sql/cubrid.conf], new key appended, stale value gone"
  else
    bad "--conf merge wrong (replaced=$ok1 appended=$ok2 stale-removed=$ok3)"
  fi
else
  note "--conf test skipped: $CTP/conf/sql.conf not readable"
fi

# (g5) failed.list extraction, both result styles. This has been wrong twice:
# once reading `classname="…"` (a GitHub blob URL) because it contains `name="`,
# and once producing nothing at all for sql, whose CTP writes no per-case JUnit.
FL="$SCRATCH/faillist"; rm -rf "$FL"; mkdir -p "$FL/shard_0/reports" "$FL/shard_0/CTP"
cat > "$FL/shard_0/reports/test-shell.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<testsuites>
  <testsuite name="shell" tests="2" skipped="0">
    <testcase classname="https://github.com/CUBRID/cubrid-testcases-private-ex/blob/develop/shell/_21_xa/_02_x/cases/_02_x.sh" name="shell/_21_xa/_02_x/cases/_02_x.sh" time="1.0">
      <failure message="Test failed" type="TestFailure">boom</failure>
    </testcase>
    <testcase classname="https://github.com/x/blob/develop/shell/_21_xa/_03_ok/cases/_03_ok.sh" name="shell/_21_xa/_03_ok/cases/_03_ok.sh" time="1.0"/>
  </testsuite>
</testsuites>
XML
cat > "$FL/shard_0/CTP/summary.xml" <<'XML'
<results>
  <scenario><case>sql/_13_issues/_24_2h/cases/bad.sql</case><result>fail</result></scenario>
  <scenario><case>sql/_13_issues/_24_2h/cases/good.sql</case><result>success</result></scenario>
</results>
XML
run_fl() {   # $1 = style, $2 = subpath
  ( set -e
    info(){ :; }; OUT="$FL"; NSHARDS=1; SUITE_STYLE="$1"; SUITE_SUBPATH="$2"
    eval "$(sed -n '/^emit_failed_list() {/,/^}/p' "$ORCH")"
    emit_failed_list ) >/dev/null 2>&1
  cat "$FL/failed.list" 2>/dev/null
}
got_sh="$(run_fl status shell)"; rm -f "$FL/failed.list"
got_sql="$(run_fl sqlresult sql)"; rm -f "$FL/failed.list"
fl_ok=1
[ "$got_sh" = "_21_xa/_02_x/cases/_02_x.sh" ] || { fl_ok=0; note "shell style got: '$got_sh'"; }
[ "$got_sql" = "_13_issues/_24_2h/cases/bad.sql" ] || { fl_ok=0; note "sql style got: '$got_sql'"; }
if [ "$fl_ok" -eq 1 ]; then
  ok "failed.list: only failing cases, scenario-relative, for both result styles (no classname URL, no empty sql list)"
else
  bad "failed.list extraction wrong"
fi

# (g3) A run with no testcase ref at all must refuse rather than default to develop.
if bash "$ORCH" --suite sql --dry-run --testcases "$TC" --ctp "$CTP" \
      --out "$SCRATCH/noref" >"$SCRATCH/noref.log" 2>&1; then
  bad "no --tc-ref/--pr/--workspace was accepted; it must refuse"
else
  grep -q 'Refusing to silently run develop testcases' "$SCRATCH/noref.log" \
    && ok "missing testcase ref refused (no silent develop)" \
    || bad "failed without the missing-ref reason"
fi

#-------------------------------------------------------------------
# (h) podman-missing preflight: clear error, non-zero, no partial work dirs
#-------------------------------------------------------------------
echo; echo "## (h) podman-missing preflight"
if command -v podman >/dev/null 2>&1; then
  note "podman IS present on this host — skipping the absence assertion (environment-dependent)."
  ok "podman-missing path: not applicable here (podman present)"
else
  PM_OUT="$SCRATCH/pm_out_should_not_exist"
  rm -rf "$PM_OUT"
  # Use an existing dir as a stand-in build so arg-validation passes and we reach
  # the podman preflight (which must fail fast, before any work dirs are made).
  bash "$ORCH" --build "$CTP" --testcases "$TC" --testcases-as-is --ctp "$CTP" --out "$PM_OUT" >"$SCRATCH/pm.log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ] && grep -qi 'podman is not installed' "$SCRATCH/pm.log" && [ ! -e "$PM_OUT" ]; then
    ok "podman-missing: clear error, exit=$rc, no work dir left behind"
  else
    bad "podman-missing path wrong (rc=$rc, out-exists=$([ -e "$PM_OUT" ] && echo yes || echo no))"; cat "$SCRATCH/pm.log" >&2
  fi
fi

#-------------------------------------------------------------------
# (i) colocate registry: keep-whole (survives --by-case) + co-locate (one shard)
#-------------------------------------------------------------------
echo; echo "## (i) colocate registry"
COLO="$SKILL/colocate.tsv"
if [ -r "$COLO" ]; then
  # (i1) keep-whole: a registered dir stays ONE unit even under --by-case.
  reg_dir="$(awk '!/^[[:space:]]*#/ && NF {print $1; exit}' "$COLO")"
  OUTBC="$SCRATCH/outbc"; rm -rf "$OUTBC"
  bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 10 --by-case --out "$OUTBC" >"$OUTBC.log" 2>&1
  asg="$OUTBC/assignment.tsv"
  asunit="$(awk -F'\t' -v d="$reg_dir" '$1==d' "$asg" | wc -l)"
  undersplit="$(awk -F'\t' -v d="$reg_dir/" 'index($1,d)==1' "$asg" | wc -l)"
  if [ "$asunit" -eq 1 ] && [ "$undersplit" -eq 0 ]; then
    ok "keep-whole: registered dir is one unit under --by-case ($reg_dir)"
  else
    bad "keep-whole failed: as-unit=$asunit per-case-under=$undersplit for $reg_dir"
  fi
  # (i2) co-locate: two dirs grouped on one registry line must share a shard.
  d1="$(awk -F'\t' 'NR==1{print $1}' "$OUT10/units.tsv")"
  d2="$(awk -F'\t' 'NR==2{print $1}' "$OUT10/units.tsv")"
  GRP="$SCRATCH/grp.tsv"; printf '%s %s\n' "$d1" "$d2" > "$GRP"
  OUTG="$SCRATCH/outg"; rm -rf "$OUTG"
  bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 10 --colocate "$GRP" --out "$OUTG" >"$OUTG.log" 2>&1
  s1="$(awk -F'\t' -v d="$d1" '$1==d{print $2}' "$OUTG/assignment.tsv")"
  s2="$(awk -F'\t' -v d="$d2" '$1==d{print $2}' "$OUTG/assignment.tsv")"
  if [ -n "$s1" ] && [ "$s1" = "$s2" ]; then
    ok "co-locate: grouped dirs share shard $s1"
  else
    bad "co-locate failed: shards differ ($d1->$s1, $d2->$s2)"
  fi
  # (i3) --no-colocate ignores the registry: --by-case is pure per-.sql again.
  OUTN="$SCRATCH/outn"; rm -rf "$OUTN"
  bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 10 --by-case --no-colocate --out "$OUTN" >"$OUTN.log" 2>&1; rcN=$?
  un="$(wc -l < "$OUTN/units.tsv" 2>/dev/null || echo -1)"
  if [ "$rcN" -eq 0 ] && [ "$un" -eq "$s" ]; then
    ok "--no-colocate: dry-run rc=0 and --by-case units == surviving .sql ($un, registry ignored)"
  else
    bad "--no-colocate failed: rc=$rcN by-case units=$un != surviving=$s"
  fi
else
  note "no bundled colocate.tsv — skipping registry checks"
fi

#-------------------------------------------------------------------
# (j) auto-weights: default run (no --weights) loads the bundled time table
#-------------------------------------------------------------------
echo; echo "## (j) auto-weights (bundled time table, default)"
BW="$SKILL/baseline_weights.tsv"
if [ -r "$BW" ]; then
  OUTW="$SCRATCH/outw"; rm -rf "$OUTW"
  bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 10 --out "$OUTW" >"$OUTW.log" 2>&1; rcW=$?
  # default should report time-based from the bundled table, and unit weights should sum
  # to the measured seconds (~ table total), NOT the case count (17420).
  bw_total="$(awk -F'\t' '{s+=$2} END{print s+0}' "$BW")"
  uw_sum="$(awk -F'\t' '{s+=$2} END{print s+0}' "$OUTW/units.tsv" 2>/dev/null)"
  if [ "$rcW" -eq 0 ] && grep -q 'weights: time-based from bundled' "$OUTW.log" \
       && [ "$uw_sum" -gt 0 ] && [ "$uw_sum" -lt 5000 ] && [ "$uw_sum" -ne "$s" ]; then
    ok "auto-weights: default is time-based (Σunit-weight=${uw_sum}s ~ table ${bw_total}s, != count $s)"
  else
    bad "auto-weights failed: rc=$rcW Σunit-weight=$uw_sum table=$bw_total count=$s"; grep -i weights "$OUTW.log" >&2
  fi
else
  note "no bundled baseline_weights.tsv — skipping auto-weights check"
fi

#-------------------------------------------------------------------
# (k) default shard count = 7 (workload-optimal; no --shards given)
#-------------------------------------------------------------------
echo; echo "## (k) default shard count"
OUTK="$SCRATCH/outk"; rm -rf "$OUTK"
bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --out "$OUTK" >"$OUTK.log" 2>&1
def_n="$(awk -F'\t' 'NR>1{n++} END{print n+0}' "$OUTK/plan.tsv" 2>/dev/null)"
if [ "$def_n" -eq 7 ]; then
  ok "default shard count = 7 (no --shards)"
else
  note "default shards=$def_n (7 unless RAM-capped on this host)"
  grep -q 'capping to' "$OUTK.log" && ok "default 7 capped by RAM guard (expected on low-memory host)" || bad "default shards=$def_n, expected 7"
fi

#-------------------------------------------------------------------
# (l) --env passthrough (#108): parsed values reach the launch-plan summary
#     (mechanical proof without podman; real container/environ check is
#     manual e2e QA — see README.md).
#-------------------------------------------------------------------
echo; echo "## (l) --env passthrough"
OUTE="$SCRATCH/oute"; rm -rf "$OUTE"
bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 3 --no-weights \
  --env CUBRID_WM_SORT_NEW=1 --env CUBRID_WM_SCAN_NEW=1 --out "$OUTE" >"$OUTE.log" 2>&1
if grep -qF 'env passthrough (--env, 2): CUBRID_WM_SORT_NEW=1 CUBRID_WM_SCAN_NEW=1' "$OUTE.log"; then
  ok "--env: both values reach the launch-plan summary verbatim, in order"
else
  bad "--env: passthrough summary missing or wrong"; grep -i 'env passthrough' "$OUTE.log" >&2
fi
# default (no --env) must still report the fixed 5 and an explicit empty list.
OUTE0="$SCRATCH/oute0"; rm -rf "$OUTE0"
bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --shards 3 --no-weights --out "$OUTE0" >"$OUTE0.log" 2>&1
if grep -qF 'env passthrough (--env, 0): <none>' "$OUTE0.log"; then
  ok "--env: default (no flag) reports an explicit empty list (no regression)"
else
  bad "--env: default-case summary missing or wrong"; grep -i 'env passthrough' "$OUTE0.log" >&2
fi
# malformed NAME=VALUE must be rejected before any work dir is created.
OUTEBAD="$SCRATCH/outebad_should_not_exist"; rm -rf "$OUTEBAD"
if bash "$ORCH" --dry-run --testcases "$TC" --testcases-as-is --ctp "$CTP" --env NOEQUALSSIGN --out "$OUTEBAD" >"$SCRATCH/outebad.log" 2>&1; then
  bad "--env: malformed NAME=VALUE was NOT rejected"
else
  if grep -qi 'expects NAME=VALUE' "$SCRATCH/outebad.log" && [ ! -e "$OUTEBAD" ]; then
    ok "--env: malformed value rejected before any work dir is created"
  else
    bad "--env: malformed value rejection wrong (missing message or work dir leaked)"; cat "$SCRATCH/outebad.log" >&2
  fi
fi

if bash "$HERE/image_contract_test.sh"; then
  ok "upstream image scope, source pinning and host exclusion planning"
else
  bad "upstream image scope, source pinning and host exclusion planning"
fi

if bash "$HERE/locale_staging_test.sh"; then
  ok "shell/HA locale staging survives CTP library reset"
else
  bad "shell/HA locale staging survives CTP library reset"
fi

# HA setup regression: exercise the entrypoint functions with isolated fixtures.
if bash "$HERE/ha_shell_test.sh"; then
  ok "HA csql port and slave broker setup regression"
else
  bad "HA csql port and slave broker setup regression"
fi

#-------------------------------------------------------------------
# Summary
#-------------------------------------------------------------------
echo
echo "=================================================================="
if [ "$FAILED" -eq 0 ]; then
  echo "ALL TESTS PASSED ($PASS checks)"
  echo "=================================================================="
  exit 0
else
  echo "TESTS FAILED: $FAILED failed, $PASS passed"
  echo "=================================================================="
  exit 1
fi
