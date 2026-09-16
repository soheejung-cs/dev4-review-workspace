"""CLI: python3 -m tools.harness.run {review|design} --pr N [--repo ~/dev/sources/cubrid] [--budget 12000] [--out out/]

격리·결정론:
 - 소스는 PR 헤드를 `git worktree` 로 임시 체크아웃해 읽는다(작업 트리 오염 없음). 끝나면 지운다(RAII: try/finally).
 - 외부 호출은 gh(PR diff/메타)만. LLM 호출은 이 CLI 안에 없다 — 산출물(context pack, arch.json, findings 스키마)을 만들고
   판정(adjudicate) 은 LLM 산출 findings.json 을 입력으로 다시 이 CLI 로 돈다. 그래서 같은 입력이면 같은 출력이다.
 - 산출물은 out/<pr>/<head-sha>/ 에 쓴다. 헤드가 바뀌면 새 디렉터리.
"""
import argparse, json, os, shutil, subprocess, sys, time
from . import codegraph as CG, context_pack as CP, arch_infer as AI, adjudicate as AD

def sh(*a, **k): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, **k)

class Worktree:
    """PR 헤드의 읽기 전용 스냅샷. with 블록을 벗어나면 반드시 제거된다(예외 포함)."""
    def __init__(self, repo, sha, base):
        self.repo, self.sha, self.path = repo, sha, os.path.join(base, f'wt-{sha[:9]}')
    def __enter__(self):
        if not os.path.isdir(self.path):
            r = sh('git', '-C', self.repo, 'worktree', 'add', '--detach', self.path, self.sha)
            if r.returncode: sh('git', '-C', self.repo, 'fetch', '-q', 'origin', self.sha); r = sh('git', '-C', self.repo, 'worktree', 'add', '--detach', self.path, self.sha)
            if r.returncode: raise RuntimeError(r.stderr)
        return self.path
    def __exit__(self, *exc):
        sh('git', '-C', self.repo, 'worktree', 'remove', '--force', self.path); return False

def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument('mode', choices=['review', 'design', 'adjudicate'])
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--budget', type=int, default=12000); ap.add_argument('--out', default=os.path.expanduser('~/dev/utils/harness-out'))
    ap.add_argument('--findings', help='adjudicate: LLM 이 낸 findings.json'); ap.add_argument('--scope', default='changed+1', help="codegraph 범위: changed | changed+1 | dir")
    a = ap.parse_args(argv); t0 = time.time()
    meta = json.loads(sh('gh', 'pr', 'view', str(a.pr), '-R', 'CUBRID/cubrid', '--json', 'headRefOid,title,files,author').stdout)
    sha = meta['headRefOid']; out = os.path.join(a.out, str(a.pr), sha[:9]); os.makedirs(out, exist_ok=True)
    diff = sh('gh', 'pr', 'diff', str(a.pr), '-R', 'CUBRID/cubrid').stdout
    open(os.path.join(out, 'pr.diff'), 'w').write(diff)
    changed = CP.parse_unified_diff(diff)
    log = lambda m: print(f'[{time.time()-t0:6.1f}s] {m}', file=sys.stderr)
    rules_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'rules')
    rules_text = ''.join(open(os.path.join(rules_dir, f), encoding='utf-8').read() for f in os.listdir(rules_dir) if f.endswith('.md'))
    with Worktree(a.repo, sha, a.out) as wt:
        files = set(f for f in changed if f.endswith(('.c', '.cpp', '.h', '.hpp', '.cc')))
        if a.scope in ('changed+1', 'dir'):   # 변경 파일과 같은 디렉터리의 소스(호출 해석용) — 수백만 줄을 다 파싱하지 않는다
            for f in list(files):
                d = os.path.dirname(f)
                for fn in os.listdir(os.path.join(wt, d)) if os.path.isdir(os.path.join(wt, d)) else []:
                    if fn.endswith(('.c', '.cpp', '.h', '.hpp')): files.add(os.path.join(d, fn))
        g = CG.CodeGraph.build(wt, files, os.path.join(out, 'codegraph.sqlite3'), log=log)
        changed_fids = sorted({fn.fid for f, ls in changed.items() for fn in g.functions_in(f, ls)})
        json.dump({'pr': a.pr, 'head': sha, 'title': meta['title'], 'author': meta['author']['login'], 'files': sorted(changed), 'changed_functions': changed_fids, 'codegraph_complete': g.complete(), 'scope': a.scope, 'n_files_parsed': len(files)}, open(os.path.join(out, 'manifest.json'), 'w'), ensure_ascii=False, indent=1)
        if a.mode in ('review', 'design'):
            pack = CP.build(wt, g, diff, rules_dir, a.budget)
            open(os.path.join(out, 'context_pack.md'), 'w', encoding='utf-8').write(pack.render()); log(f'context pack: {sum(s.tokens for s in pack.sections)} tokens, {len(pack.sections)} sections, dropped {len(pack.dropped)}')
        if a.mode == 'design':
            arch = AI.infer(g, changed_fids); risks = AI.detect_risks(arch, g, changed_fids)
            json.dump({'architecture': arch, 'risks': risks}, open(os.path.join(out, 'arch.json'), 'w'), ensure_ascii=False, indent=1)
            open(os.path.join(out, 'arch.mmd'), 'w').write(AI.to_mermaid(arch, risks)); json.dump(AI.to_excalidraw(arch), open(os.path.join(out, 'arch.excalidraw'), 'w'))
            log(f'arch: {len(arch["components"])} layers, {len(arch["connections"])} edges, touched {arch["touched_layers"]}, risks {len(risks)}')
        if a.mode == 'adjudicate':
            findings = json.load(open(a.findings))
            res = AD.adjudicate(wt, g, changed, findings, rules_text)
            json.dump(res, open(os.path.join(out, 'findings.adjudicated.json'), 'w'), ensure_ascii=False, indent=1)
            for r in res: log(f'{r["status"]:12s} {r.get("id")} {r.get("file")}:{r.get("line")} {"; ".join(r["reasons"])}')
    log(f'done -> {out}'); print(out)

if __name__ == '__main__': main()
