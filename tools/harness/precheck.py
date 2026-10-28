"""precheck — 그룹 findings 에 판정기(adjudicate)의 결정론 검사만 먼저 돌려 반증 단계(harness-refuter)의 입력으로 준다(H-31).

refuter(LLM, effort high)가 판정기가 결정론으로 하는 검사 — anchor 존재·diff 안, 지어낸 함수명, 규칙 ID, 무검증 에러 우려,
why·proposal 길이, baseline·reach_trace 근거 해석 — 를 먼저 하느라 힘을 썼다. 이 도구가 그것을 기계로 하고 refuter 는 reasons 를
받아 '더 단순한 설명'(develop 선재·의도된 스펙 변경·이미 답한 스레드)과 제안의 예시 같은 의미 판단만 한다.

  python3 -m tools.harness.precheck --out OUT --findings OUT/findings.<g>.json [--repo ~/dev/sources/cubrid]
  산출: OUT/precheck.<g>.json — [{id, file, line, status, severity, reasons, passed, obligations | schema_errors}]
        (<g> 는 findings 파일 이름에서 딴다 — `--tag` 로 바꿀 수 있다). 표준출력에는 지적마다 한 줄.

`run full --findings` 와 다른 점(왜 그것을 그룹마다 돌리지 않나):
  - worktree·codegraph 를 새로 만들지 않는다 — `run full` 이 남긴 OUT/codegraph.sqlite3(읽기 전용으로 연다)·pr.diff·manifest.json 을 쓰고,
    head 파일은 엔진 리포에서 `git show <head>:<file>` 로 지적이 가리키는 것만 임시 디렉터리에 꺼낸다(실행마다 따로 — 그룹 병렬 가능).
  - findings.adjudicated.json·requery.json·report.md·episodic 을 쓰지 않는다(최종 판정은 종합 뒤 `run full --findings` 한 번).
  - repro 의무·self_check(컴파일)는 하지 않는다 — 종합 뒤 최종 판정의 몫.
사이드이펙트: OUT/precheck.<g>.json 하나만 쓴다. 네트워크(gh)를 쓰지 않는다.
"""
import argparse, json, os, re, shutil, sqlite3, subprocess, sys, tempfile
from urllib.parse import quote

from . import adjudicate as AD, codegraph as CG, context_pack as CP

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = re.compile(r'([^:\s]+):(\d+)')


def _read(p):
    with open(p, encoding='utf-8', errors='replace') as fp:
        return fp.read()


def _git(repo, *a, text=True):
    return subprocess.run(['git', '-C', repo, *a], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=text)


def referenced_files(findings):
    """지적이 head 에서 읽어야 하는 파일 — anchor·evidence·reach_trace 의 file:line 의 파일(pr-body 제외)."""
    out = set()
    for f in findings:
        refs = [f'{f.get("file")}:{f.get("line") or 0}'] + list(f.get('evidence') or []) + list(f.get('reach_trace') or [])
        for r in refs:
            m = REF.match(str(r))
            if m and m.group(1) not in ('pr-body', 'None'):
                out.add(m.group(1))
    return out


def materialize(repo, sha, files, dest):
    """head(sha) 의 파일을 dest 아래 같은 경로로 꺼낸다. 그 리비전에 없는 파일은 건너뛴다(판정기가 'anchor 없음' 으로 본다)."""
    n = 0
    for rel in sorted(files):
        if os.path.isabs(rel) or '..' in rel.split('/'):
            continue
        r = _git(repo, 'show', f'{sha}:{rel}', text=False)
        if r.returncode:
            continue
        p = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'wb') as fp:
            fp.write(r.stdout)
        n += 1
    return n


def base_rev_for(repo, sha):
    """기준 rev = head 와 upstream/develop(없으면 origin/develop) 의 merge-base — 판정기의 _base_rev 를 HEAD 대신 sha 로."""
    for ref in ('upstream/develop', 'origin/develop'):
        r = _git(repo, 'merge-base', sha, ref)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return ''


def open_graph(db_path):
    """OUT/codegraph.sqlite3 를 읽기 전용으로 — CodeGraph() 생성자는 스키마가 다르면 표를 지우므로 쓰지 않는다."""
    g = CG.CodeGraph.__new__(CG.CodeGraph)
    g.repo_root = ''
    g.db = sqlite3.connect('file:%s?mode=ro' % quote(os.path.abspath(db_path)), uri=True)
    v = g.db.execute("SELECT v FROM meta WHERE k='schema_version'").fetchone()
    if not v or v[0] != CG.CodeGraph.SCHEMA_VERSION:
        raise SystemExit(f'precheck: {db_path} 의 codegraph 스키마가 이 하네스와 다르다 — `run full --pr N` 을 다시 돌린다')
    return g


def run(out, findings_path, repo, tag=''):
    man_p = os.path.join(out, 'manifest.json')
    for need in (man_p, os.path.join(out, 'pr.diff'), os.path.join(out, 'codegraph.sqlite3')):
        if not os.path.isfile(need):
            raise SystemExit(f'precheck: {need} 가 없다 — 먼저 `python3 -m tools.harness.run full --pr <N>` 으로 OUT 을 만든다')
    sha = json.loads(_read(man_p))['head']
    if _git(repo, 'cat-file', '-e', f'{sha}^{{commit}}').returncode:
        raise SystemExit(f'precheck: {repo} 에 head {sha[:9]} 가 없다 — `run full` 을 돌린 리포를 --repo 로 준다')
    raw = json.loads(_read(findings_path))
    findings = raw['findings'] if isinstance(raw, dict) and 'findings' in raw else raw
    ok, bad = AD.validate_findings(findings, os.path.join(ROOT, 'harness', 'schemas', 'finding.json'))
    changed = CP.parse_unified_diff(_read(os.path.join(out, 'pr.diff')))
    rules_dir = os.path.join(ROOT, 'rules')
    rules_text = ''.join(_read(os.path.join(rules_dir, f)) for f in sorted(os.listdir(rules_dir)) if f.endswith('.md'))
    g = open_graph(os.path.join(out, 'codegraph.sqlite3'))
    tmp = tempfile.mkdtemp(prefix='precheck-')
    try:
        materialize(repo, sha, referenced_files(ok), tmp)
        res = AD.adjudicate(tmp, g, changed, ok, rules_text, git_repo=repo, base_rev=base_rev_for(repo, sha))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        g.db.close()
    rows = [{'id': r.get('id'), 'file': r.get('file'), 'line': r.get('line'), 'status': r['status'], 'severity': r.get('severity'),
             'reasons': r.get('reasons', []), 'passed': r.get('passed', []), 'obligations': r.get('obligations', {})} for r in res]
    rows += [{'id': b.get('id'), 'file': b.get('file'), 'line': b.get('line'), 'status': 'schema-fail', 'severity': b.get('severity'),
              'reasons': b['schema_errors'], 'passed': [], 'schema_errors': b['schema_errors']} for b in bad]
    if not tag:
        m = re.match(r'findings\.(.+)\.json$', os.path.basename(findings_path))
        tag = m.group(1) if m else 'all'
    dst = os.path.join(out, f'precheck.{tag}.json')
    with open(dst, 'w', encoding='utf-8') as fp:
        json.dump(rows, fp, ensure_ascii=False, indent=1)
    return dst, rows


def main():
    ap = argparse.ArgumentParser(description='그룹 findings 의 결정론 판정(adjudicate 의무)만 — refuter 입력 precheck.<g>.json')
    ap.add_argument('--out', required=True, help='run full 의 산출 디렉터리(OUT) — manifest.json·pr.diff·codegraph.sqlite3 가 있어야 한다')
    ap.add_argument('--findings', required=True, help='findings.<그룹>.json')
    ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'), help='엔진 리포(head·기준 rev 를 git 으로 읽는다)')
    ap.add_argument('--tag', default='', help='산출 이름 precheck.<tag>.json (기본: findings 파일 이름의 그룹)')
    a = ap.parse_args()
    dst, rows = run(a.out, a.findings, a.repo, a.tag)
    for r in rows:
        print(f"{r['status']:12s} {str(r.get('severity') or '')[:12]:12s} {r['id']} {r['file']}:{r['line']} {'; '.join(r['reasons'])}")
    n = {}
    for r in rows:
        n[r['status']] = n.get(r['status'], 0) + 1
    print(f"precheck: {len(rows)}건 " + ' · '.join(f'{k} {v}' for k, v in sorted(n.items())) + f' -> {dst}', file=sys.stderr)


if __name__ == '__main__':
    main()
