#!/usr/bin/env python3
"""구현 게이트 — 워킹트리의 변경이 plan 을 지켰고 깨뜨린 게 없는지 결정론적으로 판정.
사용: python3 -m tools.harness.implement_gate --plan <out>/plan.json [--repo ~/dev/sources/cubrid] [--base <rev>] [--before <out>/codegraph.sqlite3]
검사: (1) 범위 — 바뀐 파일·함수가 plan.changes 안인가  (2) codestyle.sh  (3) -fsyntax-only (compile_commands 있으면)
      (4) 관문 짝 — 바뀐 함수의 latch/lock/alloc 카운트가 계획 없이 달라졌나 (before 그래프와 비교)
      (5) 성능 작업이면 structure.json  (6) 수명 있는 상태 — diff 가 구조체에 포인터 필드를 더했거나 plan.lifetime_state 가 있으면
          plan.release_sites 에 그 상태를 놓는 자리 전수의 판정(released/cannot-attach/not-applicable)이 있어야 하고, 그래프가 뽑은
          해제 경로 후보(free·decache·retire·clear·destroy… 이름 + 그 구조체 타입을 쓰거나 새 필드를 만지는 함수) 하나하나가
          release_sites 에 나와야 한다(META-REVIEW PR#7658 — '만드는 곳' 이 아니라 '놓는 곳' 을 전수로, H-33)
결과: <out>/gate.json + 표준출력 요약. 실패 = 코드를 고쳐 다시(재질의 1회 규칙은 스킬이 관리)."""
import argparse, sys, json, os, subprocess, sys, tempfile, shutil
import re as _re
from . import codegraph as CG, reachability as R, adjudicate as AD, context_pack as CP
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def sh(*a, cwd=None): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=cwd)

# ---- (6) 수명 있는 상태의 해제 경로 전수 (META-REVIEW PR#7658 → H-33) ----
# 놓는 자리 이름 — 같은 계약(이 상태를 놓는다)을 지켜야 하는 함수들. 'final' 은 *_final (xcache_final 등)
RELEASE_NAME = _re.compile(r'(^|_)(free|decache|retire|clear|destroy|finalize|final|cleanup|dealloc|release|reset|drop|uninit)(_|$)')
PTR_MEMBER = _re.compile(r'^\s*(?:(?:const|struct|unsigned|signed|volatile|class)\s+)*[A-Za-z_][\w:<>]*\s*\*+\s*(?:const\s+)?([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;')


_LINES = {}


def _lines(repo, file):
    key = os.path.join(repo, file)
    if key not in _LINES:
        try:
            with open(key, encoding='utf-8', errors='replace') as fp:
                _LINES[key] = fp.read().split('\n')
        except OSError:
            _LINES[key] = []
    return _LINES[key]


def _struct_aliases(repo, file, sname, end_line):
    """구조체 태그 + typedef 이름(`} NAME;`·`typedef struct tag NAME;`) — 함수의 타입 참조(fn_types)와 맞춰 볼 이름들."""
    names = {sname}
    lines = _lines(repo, file)
    if not lines:
        return names
    m = _re.match(r'\s*}\s*([A-Za-z_]\w*)\s*;', lines[end_line - 1]) if 0 < end_line <= len(lines) else None
    if m:
        names.add(m.group(1))
    for ln in lines:
        m = _re.match(r'\s*typedef\s+(?:struct|class)\s+' + _re.escape(sname) + r'\s+([A-Za-z_]\w*)\s*;', ln)
        if m:
            names.add(m.group(1))
    return names


def release_check(plan, repo, g, added):
    """반환 (실패 메시지들, 참고 메시지들, 해제 경로 후보 fid 들). added = {file: [(new_line, text)]}(static_lint._added).
    상태 찾기: diff 가 구조체 본문 안에 더한 원시 포인터 필드 + plan.lifetime_state('구조체.필드'). 없으면 검사하지 않는다."""
    states = []                                      # (구조체 이름, 필드, 파일, 구조체 끝 줄)
    for f, rows in added.items():
        sts = g.db.execute('SELECT name, start_line, end_line FROM structs WHERE file=?', (f,)).fetchall()
        for n, t in rows:
            m = PTR_MEMBER.match(t)
            inside = [x for x in sts if x[1] < n < x[2]]
            if m and inside:
                sname, _, end = min(inside, key=lambda x: x[2] - x[1])
                states.append((sname, m.group(1), f, end))
    for d in plan.get('lifetime_state') or []:
        sname, _, field = d.partition('.')
        for name, f, a, b in g.db.execute('SELECT name, file, start_line, end_line FROM structs WHERE name=?', (sname,)).fetchall()[:1]:
            states.append((name, field, f, b))
        if not g.db.execute('SELECT 1 FROM structs WHERE name=?', (sname,)).fetchone():
            states.append((sname, field, '', 0))      # 그래프 범위 밖 구조체 — 필드 이름으로만 찾는다
    if not states:
        return [], [], []
    _LINES.clear()                                   # 게이트 한 번 = 작업 트리 한 상태
    sites = plan.get('release_sites') or []
    fails, notes = [], []
    desc = ', '.join(sorted({'%s.%s' % (s, fld) for s, fld, _, _ in states}))
    if not sites:
        fails.append('수명 있는 상태(%s)를 더했는데 plan.release_sites 가 비었다 — 그 상태를 놓는 자리(free·decache·retire·clear·destroy 호출부)를 '
                     '전수로 적고 각각 released(여기서 놓는다)·cannot-attach(이 경로엔 붙을 수 없다)·not-applicable 로 판정한다(META-REVIEW PR#7658, H-33)' % desc)
    cands = {}
    for sname, field, f, end in states:
        aliases = _struct_aliases(repo, f, sname, end) if f else {sname}
        pat = _re.compile(r'(->|\.)\s*' + _re.escape(field) + r'\b')
        for fid, name, file, a, b in g.db.execute('SELECT fid, name, file, start_line, end_line FROM functions ORDER BY fid'):
            if not RELEASE_NAME.search(name.split('::')[-1]):
                continue
            uses_type = bool(set(g.types(fid)) & aliases)
            body = '\n'.join(_lines(repo, file)[a - 1:b])
            touches = bool(pat.search(body))
            if uses_type or touches:
                prev = cands.get(fid, (name, False))
                cands[fid] = (name, prev[1] or touches)
    for fid, (name, touches) in sorted(cands.items()):
        short = name.split('::')[-1]
        if not any(short in str(s.get('site', '')) for s in sites):
            fails.append('해제 경로 후보 %s 에 release_sites 판정이 없다 — %s' % (fid, '새 필드를 이미 다룬다(놓는가 확인)' if touches
                                                                           else '이 구조체를 다루는데 새 필드는 안 다룬다(여기서 새는가?)'))
    for s in sites:
        if s.get('verdict') == 'released' and not s.get('why'):
            notes.append('release_sites %s: released 인데 why(어느 줄에서 놓나) 가 비었다' % s.get('site'))
    if cands:
        notes.append('해제 경로 후보 %d개(변경 파일과 같은 디렉터리 범위 — 그 밖의 놓는 자리는 plan 에 직접 적는다): %s'
                     % (len(cands), ', '.join(sorted(n for n, _ in cands.values()))[:400]))
    return fails, notes, sorted(cands)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--plan', required=True); ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--commits', default='', help='<base>..<head>: 커밋마다 홀로 컴파일되는지도 본다(compile_check) — 나눈 커밋 필수')
    ap.add_argument('--base', default='HEAD', help='비교 기준 rev (기본 HEAD: 미커밋 변경을 본다; 커밋했으면 upstream/develop 등)')
    ap.add_argument('--before', default='', help='plan 시점 codegraph.sqlite3 (기본: plan 과 같은 디렉터리)')
    a = ap.parse_args()
    plan = json.load(open(a.plan, encoding='utf-8')); out = os.path.dirname(os.path.abspath(a.plan))
    repo = a.repo; res = {'ok': True, 'checks': {}}
    def fail(k, msg): res['ok'] = False; res['checks'].setdefault(k, []).append(msg)

    # (1) 범위
    diff = sh('git', '-C', repo, 'diff', '--name-only', a.base).stdout.split() + sh('git', '-C', repo, 'ls-files', '--others', '--exclude-standard', 'src').stdout.split()
    changed_files = sorted({f for f in diff if f.endswith(('.c', '.cpp', '.h', '.hpp', '.cc'))})
    plan_files = {c['file'] for c in plan.get('changes', [])}
    # plan 의 function 칸은 사람이 "a / b / c" 나 "(선언 3곳: a, b)" 처럼 여러 이름을 적기도 한다 — 식별자 토큰 전부를 인정한다 (2026-09-30)
    import re as _re
    plan_fns = set()
    for c in plan.get('changes', []):
        toks = [t for t in _re.findall(r'[A-Za-z_][A-Za-z0-9_]*', c['function']) if len(t) > 2]; prev = None
        for t in toks:
            if t.startswith('_') and prev:   # "heap_attr_readval_int / _bigint / _short" 식 축약: 앞 이름의 마지막 조각을 바꾼다
                t = prev.rsplit('_', 1)[0] + t
            plan_fns.add((c['file'], t)); prev = t
    plan_wild = {c['file'] for c in plan.get('changes', []) if '*' in c['function']}   # "file: *" 는 그 파일 전체 허용
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
    if sc.get('ran') and any(v not in ('ok', 'no-compile-command', 'no-ninja-target') for v in sc['results'].values()): fail('syntax-fail', '컴파일 실패 — checks.syntax.results 참조')   # 'syntax' 는 결과 dict 자리라 다른 키로 (2026-10-16 AttributeError)
    res['skipped'] = [] if sc.get('ran') else ['syntax: ' + sc.get('reason', '')]
    if a.commits:
        # 커밋 단위 컴파일: 헝크로 나눈 커밋이 홀로 컴파일되지 않던 사고(2026-09-30) — 게이트가 직접 돈다
        cc = subprocess.run([sys.executable, '-m', 'tools.harness.compile_check', '--repo', repo, '--commits', a.commits], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=ROOT)
        res['checks']['commits'] = [l for l in cc.stdout.splitlines() if l.strip()]
        if cc.returncode: fail('commits', f'커밋 단위 컴파일 실패 ({a.commits}) — checks.commits 참조')
    if sc.get('ran'): res['skipped'] += [f'syntax({f}): {v}' for f, v in sc['results'].items() if v in ('no-compile-command', 'no-ninja-target')]
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
                if (fn.file, fn.name) not in plan_fns and fn.file not in plan_wild: fail('scope', f'plan 밖 함수 변경: {fn.fid}')
                pa = R.latch_pairing(g_after, fn.fid); pb = R.latch_pairing(g_before, fn.fid) if g_before and g_before.function(fn.fid) else {}
                delta = {k: pa.get(k, 0) - pb.get(k, 0) for k in set(pa) | set(pb) if pa.get(k, 0) != pb.get(k, 0)}
                unbalanced = {k: v for k, v in pa.items() if v}
                if unbalanced:
                    declared = next((c.get('gate_contract', '') for c in plan.get('changes', []) if c['file'] == fn.file and fn.name in c['function']), '')
                    if declared: res['checks'].setdefault('gates-declared', []).append(f'{fn.fid}: {unbalanced} — 계약: {declared[:120]}')
                    elif delta and g_before is not None and g_before.function(fn.fid): fail('gates', f'{fn.fid}: 이 변경이 관문 카운트를 바꿈 {delta} (현재 불균형 {unbalanced}) 인데 plan.gate_contract 가 비어 있음')
                    else: res['checks'].setdefault('gates-note', []).append(f'{fn.fid}: 불균형 {unbalanced} 은 변경 전에도 같음(또는 before 그래프 없음) — 빌더의 소유권 이전이면 정상, 아니면 선재 결함 후보')
    # (5) 구조 먼저 — 성능 작업이면 structure.json (harness/구조우선-성능작업.md)
    perf_like = any(c.get('rule_ids') for c in plan.get('changes', []))
    if plan.get('perf') or perf_like:
        from . import structure_check as SC
        sp = plan.get('structure') or os.path.join(out, 'structure.json')
        errs, notes = SC.check(os.path.expanduser(sp), 'gate')
        for e in errs: fail('structure', e)
        for n in notes: res['checks'].setdefault('structure-note', []).append(n)
        if perf_like and not plan.get('perf'): res['checks'].setdefault('structure-note', []).append('changes 에 성능 rule_ids 가 있어 성능 작업으로 보고 검사했다 — plan.perf=true 로 적는다')
    # (6) 수명 있는 상태 — 해제 경로 전수 (META-REVIEW PR#7658 → H-33)
    if g_after is not None:
        from .static_lint import _added
        rf, rn, rc = release_check(plan, repo, g_after, _added(diff_text))
        for m in rf: fail('lifetime', m)
        for m in rn: res['checks'].setdefault('lifetime-note', []).append(m)
        if rc: res['lifetime_candidates'] = rc
    res['changed_files'] = changed_files; res['touched_functions'] = touched
    json.dump(res, open(os.path.join(out, 'gate.json'), 'w'), ensure_ascii=False, indent=1)
    verdict = 'GATE FAIL' if not res['ok'] else ('GATE OK' if not res.get('skipped') else 'GATE OK* (건너뛴 검사 있음 — 컴파일을 확인한 것이 아니다)')
    print(verdict + f' — files {len(changed_files)}, functions {len(touched)}')
    for m in res.get('skipped', []): print(f'  skipped: {m}')
    for k, v in res['checks'].items():
        if k == 'syntax': print(f'  syntax: {"ran" if v.get("ran") else "skipped: " + v.get("reason","")}' + (f' {v["results"]}' if v.get('ran') else '')); continue
        for m in v: print(f'  {k}: {m}')
    sys.exit(0 if res['ok'] else 1)

if __name__ == '__main__': main()
