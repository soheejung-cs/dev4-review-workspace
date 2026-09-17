#!/usr/bin/env python3
"""구현 게이트 — 워킹트리의 변경이 plan 을 지켰고 깨뜨린 게 없는지 결정론적으로 판정.
사용: python3 -m tools.harness.implement_gate --plan <out>/plan.json [--repo ~/dev/sources/cubrid] [--base <rev>] [--before <out>/codegraph.sqlite3]
검사: (1) 범위 — 바뀐 파일·함수가 plan.changes 안인가  (2) codestyle.sh  (3) -fsyntax-only (compile_commands 있으면)
      (4) 관문 짝 — 바뀐 함수의 latch/lock/alloc 카운트가 계획 없이 달라졌나 (before 그래프와 비교)
결과: <out>/gate.json + 표준출력 요약. 실패 = 코드를 고쳐 다시(재질의 1회 규칙은 스킬이 관리)."""
import argparse, json, os, subprocess, sys, tempfile, shutil
from . import codegraph as CG, reachability as R, adjudicate as AD, context_pack as CP

def sh(*a, cwd=None): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=cwd)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', required=True); ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--base', default='HEAD', help='비교 기준 rev (기본 HEAD: 미커밋 변경을 본다; 커밋했으면 upstream/develop 등)')
    ap.add_argument('--before', default='', help='plan 시점 codegraph.sqlite3 (기본: plan 과 같은 디렉터리)')
    a = ap.parse_args()
    plan = json.load(open(a.plan, encoding='utf-8')); out = os.path.dirname(os.path.abspath(a.plan))
    repo = a.repo; res = {'ok': True, 'checks': {}}
    def fail(k, msg): res['ok'] = False; res['checks'].setdefault(k, []).append(msg)

    # (1) 범위
    diff = sh('git', '-C', repo, 'diff', '--name-only', a.base).stdout.split() + sh('git', '-C', repo, 'ls-files', '--others', '--exclude-standard', 'src').stdout.split()
    changed_files = sorted({f for f in diff if f.endswith(('.c', '.cpp', '.h', '.hpp', '.cc'))})
    plan_files = {c['file'] for c in plan.get('changes', [])}; plan_fns = {(c['file'], c['function']) for c in plan.get('changes', [])}
    for f in changed_files:
        if f not in plan_files: fail('scope', f'plan 밖 파일 변경: {f}')
    for f in plan_files:
        if f not in changed_files: res['checks'].setdefault('scope-note', []).append(f'plan 에 있으나 변경 없음: {f}')
    diff_text = sh('git', '-C', repo, 'diff', '-U0', a.base, '--', *changed_files).stdout if changed_files else ''
    changed_lines = CP.parse_unified_diff(diff_text)
    # (2) codestyle — 원본을 복사해 스크립트를 돌리고 diff
    cs = os.path.join(repo, '.github', 'workflows', 'codestyle.sh')
    if os.path.isfile(cs):
        for f in changed_files:
            tmp = tempfile.mkdtemp(); dst = os.path.join(tmp, os.path.basename(f)); shutil.copy(os.path.join(repo, f), dst)
            sh('bash', cs, dst); d = sh('diff', '-q', os.path.join(repo, f), dst)
            if d.returncode: fail('codestyle', f'{f}: codestyle.sh 가 바꾼 줄 있음 → 스크립트 출력으로 교체')
            shutil.rmtree(tmp, ignore_errors=True)
    # (3) -fsyntax-only
    sc = AD.self_check_build(repo, [f for f in changed_files if f.endswith(('.c', '.cpp', '.cc'))])
    res['checks']['syntax'] = sc
    if sc.get('ran') and any(v not in ('ok', 'no-compile-command') for v in sc['results'].values()): fail('syntax', '컴파일 실패 — checks.syntax.results 참조')
    # (4) 관문 짝 전후 비교
    before_db = a.before or os.path.join(out, 'codegraph.sqlite3')
    scope = set(changed_files)
    for f in changed_files:
        d = os.path.dirname(f); dd = os.path.join(repo, d)
        if os.path.isdir(dd): scope |= {os.path.join(d, fn) for fn in os.listdir(dd) if fn.endswith(('.c', '.cpp', '.h', '.hpp'))}
    g_after = CG.CodeGraph.build(repo, sorted(scope), os.path.join(out, 'codegraph.after.sqlite3'), log=lambda m: None) if changed_files else None
    g_before = CG.CodeGraph(before_db) if os.path.isfile(before_db) else None
    touched = []
    if g_after:
        for f, lns in changed_lines.items():
            for fn in g_after.functions_in(f, lns):
                touched.append(fn.fid)
                if (fn.file, fn.name) not in plan_fns: fail('scope', f'plan 밖 함수 변경: {fn.fid}')
                pa = R.latch_pairing(g_after, fn.fid); pb = R.latch_pairing(g_before, fn.fid) if g_before and g_before.function(fn.fid) else {}
                delta = {k: pa.get(k, 0) - pb.get(k, 0) for k in set(pa) | set(pb) if pa.get(k, 0) != pb.get(k, 0)}
                unbalanced = {k: v for k, v in pa.items() if v}
                if unbalanced:
                    declared = next((c.get('gate_contract', '') for c in plan.get('changes', []) if c['file'] == fn.file and c['function'] == fn.name), '')
                    if not declared: fail('gates', f'{fn.fid}: 관문 카운트 불균형 {unbalanced} (변화 {delta}) 인데 plan.gate_contract 가 비어 있음')
                    else: res['checks'].setdefault('gates-declared', []).append(f'{fn.fid}: {unbalanced} — 계약: {declared[:120]}')
    res['changed_files'] = changed_files; res['touched_functions'] = touched
    json.dump(res, open(os.path.join(out, 'gate.json'), 'w'), ensure_ascii=False, indent=1)
    print(('GATE OK' if res['ok'] else 'GATE FAIL') + f' — files {len(changed_files)}, functions {len(touched)}')
    for k, v in res['checks'].items():
        if k == 'syntax': print(f'  syntax: {"ran" if v.get("ran") else "skipped: " + v.get("reason","")}' + (f' {v["results"]}' if v.get('ran') else '')); continue
        for m in v: print(f'  {k}: {m}')
    sys.exit(0 if res['ok'] else 1)

if __name__ == '__main__': main()
