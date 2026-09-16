"""YAML 러너: harness/pipelines/*.yaml 이 실행을 소유한다(Metis execution-graph 원칙).
- 노드의 impl 은 레지스트리 이름이어야 하고, 모르는 이름·빠진 입력은 로딩 시점에 실패한다.
- manifest.json 에 결정론 기록: pr/head, harness git sha, pipeline yaml sha, rules sha, skills(prompt) sha, codegraph fingerprint, model_id(환경 HARNESS_MODEL).
- 외부 호출은 gh 만. LLM 호출은 없고, LLM 산출물(findings.json)은 adjudicate 파이프라인의 입력이다.
"""
import hashlib, json, os, subprocess, sys, time
import yaml
from . import codegraph as CG, context_pack as CP, arch_infer as AI, adjudicate as AD, perf_claims as PC, episodic as EP
from .run_support import Worktree, sh

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
def _sha(paths):
    h = hashlib.sha1()
    for p in sorted(paths):
        if os.path.isfile(p): h.update(open(p, 'rb').read())
    return h.hexdigest()[:12]

class Ctx(dict):
    """단계 간 값 전달. 노드 출력은 이름으로 넣고, 입력은 이름으로 꺼낸다(없으면 실패)."""
    def need(self, k):
        if k not in self: raise KeyError(f'pipeline: input {k!r} not produced by an earlier node')
        return self[k]

def n_worktree(c, a): c['wt'] = c['_wt_cm'].__enter__(); return c['wt']
def n_diff_map(c, a):
    c['diff'] = sh('gh', 'pr', 'diff', str(c['pr']), '-R', 'CUBRID/cubrid').stdout
    open(os.path.join(c['out'], 'pr.diff'), 'w').write(c['diff']); c['changed'] = CP.parse_unified_diff(c['diff']); return c['changed']
def n_codegraph(c, a):
    wt = c.need('wt'); changed = c.need('changed')
    files = {f for f in changed if f.endswith(('.c', '.cpp', '.h', '.hpp', '.cc'))}
    if a.get('scope', 'changed+1') != 'changed':
        for f in list(files):
            d = os.path.dirname(f); dd = os.path.join(wt, d)
            if os.path.isdir(dd): files |= {os.path.join(d, fn) for fn in os.listdir(dd) if fn.endswith(('.c', '.cpp', '.h', '.hpp'))}
    g = CG.CodeGraph.build(wt, files, os.path.join(c['out'], 'codegraph.sqlite3'), log=c['log'])
    c['g'] = g; c['files_parsed'] = len(files)
    c['changed_fids'] = sorted({fn.fid for f, ls in changed.items() for fn in g.functions_in(f, ls)}); return g
def n_pack(c, a):
    pack = CP.build(c.need('wt'), c.need('g'), c.need('diff'), os.path.join(ROOT, 'rules'), a.get('budget_tokens', 12000), os.path.join(ROOT, 'examples'))
    bs = CP.batches(pack, a.get('budget_tokens', 12000))
    for i, b in enumerate(bs):
        name = 'context_pack.md' if len(bs) == 1 else f'context_pack.batch{i+1}.md'
        open(os.path.join(c['out'], name), 'w', encoding='utf-8').write(b.render())
    c['log'](f'context pack: {sum(s.tokens for s in pack.sections)} tokens → {len(bs)} batch(es), dropped {len(pack.dropped)}'); c['n_batches'] = len(bs); return bs
def n_arch(c, a):
    arch = AI.infer(c.need('g'), c.need('changed_fids')); risks = AI.detect_risks(arch, c['g'], c['changed_fids'])
    json.dump({'architecture': arch, 'risks': risks}, open(os.path.join(c['out'], 'arch.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(c['out'], 'arch.mmd'), 'w').write(AI.to_mermaid(arch, risks)); json.dump(AI.to_excalidraw(arch), open(os.path.join(c['out'], 'arch.excalidraw'), 'w'))
    c['log'](f'arch: {len(arch["components"])} layers, {len(arch["connections"])} edges, touched {arch["touched_layers"]}, risks {len(risks)}'); c['risks'] = risks; return arch
def n_perf_claims(c, a):
    body = c['meta'].get('body') or ''; fids = c.need('changed_fids')
    fn = c['g'].function(fids[0]) if fids else None
    auto = PC.analyze(body, fn.file if fn else (sorted(c['changed'])[0] if c['changed'] else 'PR'), fn.start_line if fn else 1)
    json.dump(auto, open(os.path.join(c['out'], 'findings.auto.json'), 'w'), ensure_ascii=False, indent=1); c['log'](f'perf claims: {len(auto)} auto findings'); return auto
def n_validate(c, a):
    findings = json.load(open(c['findings_path'], encoding='utf-8'))
    if os.path.isfile(os.path.join(c['out'], 'findings.auto.json')) and a.get('merge_auto', True): findings += json.load(open(os.path.join(c['out'], 'findings.auto.json')))
    ok, bad = AD.validate_findings(AD.dedup(findings), os.path.join(ROOT, 'harness', 'schemas', 'finding.json'))
    c['findings_ok'], c['findings_bad'] = ok, bad; c['log'](f'validate: {len(ok)} ok, {len(bad)} schema failures'); return ok
def n_gate(c, a):
    rules_dir = os.path.join(ROOT, 'rules'); rules_text = ''.join(open(os.path.join(rules_dir, f), encoding='utf-8').read() for f in os.listdir(rules_dir) if f.endswith('.md'))
    res = AD.adjudicate(c.need('wt'), c.need('g'), c.need('changed'), c.need('findings_ok'), rules_text)
    res = AD.repro_obligation(os.path.expanduser('~/dev/docs/claude-workspace'), res, c['meta'].get('jira', ''))
    json.dump(res, open(os.path.join(c['out'], 'findings.adjudicated.json'), 'w'), ensure_ascii=False, indent=1)
    n = AD.requery(c.get('findings_bad', []), res, os.path.join(c['out'], 'requery.json'))
    for r in res: c['log'](f'{r["status"]:12s} {r.get("severity","")[:12]:12s} {r.get("id")} {r.get("file")}:{r.get("line")} {"; ".join(r.get("reasons", []))}')
    c['log'](f'requery items: {n} (schema failures + inconclusive)'); c['adjudicated'] = res; return res
def n_self_check(c, a):
    files = sorted({f for f in c.need('changed') if f.endswith(('.c', '.cpp'))})
    if not any(r['status'] == 'valid' for r in c.get('adjudicated', [])): c['log']('self_check: skipped (no valid finding)'); return None
    r = AD.self_check_build(c.need('wt'), files); c['log'](f'self_check: {r}'); json.dump(r, open(os.path.join(c['out'], 'self_check.json'), 'w')); return r
def n_episodic(c, a):
    p = EP.collect(c['pr'], a.get('reviewer', 'soheejung-cs'), os.path.join(ROOT, 'examples', 'episodic')); c['log'](f'episodic: {p}'); return p

REGISTRY = {'worktree': n_worktree, 'diff_map': n_diff_map, 'codegraph': n_codegraph, 'pack': n_pack, 'arch': n_arch, 'perf_claims': n_perf_claims,
            'validate': n_validate, 'gate': n_gate, 'self_check': n_self_check, 'episodic': n_episodic}

def load(path):
    y = yaml.safe_load(open(path, encoding='utf-8'))
    for st, nodes in y['stages'].items():
        if not isinstance(nodes, dict): raise SystemExit(f'pipeline: stage {st} must be a mapping of nodes')
        for name, node in nodes.items():
            if node is not None and not isinstance(node, dict): raise SystemExit(f'pipeline: node {st}.{name} must be a mapping')
            impl = (node or {}).get('impl', name)
            if impl not in REGISTRY: raise SystemExit(f'pipeline {os.path.basename(path)}: stage {st} node {name}: unknown impl {impl!r} (registry: {sorted(REGISTRY)})')
    return y

def run(pipeline_path, pr, out_base, repo, findings_path=None, model=None):
    t0 = time.time(); y = load(pipeline_path)
    log = lambda m: print(f'[{time.time()-t0:6.1f}s] {m}', file=sys.stderr)
    meta = json.loads(sh('gh', 'pr', 'view', str(pr), '-R', 'CUBRID/cubrid', '--json', 'headRefOid,title,files,author,body').stdout)
    import re; m = re.search(r'CBRD-\d+', meta['title'] + ' ' + (meta.get('body') or '')); meta['jira'] = m.group(0) if m else ''
    sha = meta['headRefOid']; out = os.path.join(out_base, str(pr), sha[:9]); os.makedirs(out, exist_ok=True)
    c = Ctx(pr=pr, out=out, log=log, meta=meta, findings_path=findings_path, _wt_cm=Worktree(repo, sha, out_base))
    manifest = {'pipeline': y['harness'], 'pipeline_sha': _sha([pipeline_path]), 'pr': pr, 'head': sha, 'title': meta['title'], 'author': meta['author']['login'], 'jira': meta['jira'],
                'harness_git': sh('git', '-C', ROOT, 'rev-parse', '--short', 'HEAD').stdout.strip(), 'rules_sha': _sha([os.path.join(ROOT, 'rules', f) for f in os.listdir(os.path.join(ROOT, 'rules'))]),
                'skills_sha': _sha([os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, 'skills')) for f in fs if f == 'SKILL.md']),
                'model_id': model or os.environ.get('HARNESS_MODEL', 'unset'), 'started': time.strftime('%Y-%m-%dT%H:%M:%S'), 'stages': {}}
    try:
        for st, nodes in y['stages'].items():
            for name, node in (nodes or {}).items():
                node = node or {}; impl = node.get('impl', name)
                if node.get('when') == 'findings' and not findings_path: log(f'{st}.{name}: skipped (no --findings)'); continue
                REGISTRY[impl](c, node); manifest['stages'].setdefault(st, []).append(name)
        if 'g' in c: manifest.update(codegraph_fingerprint=c['g'].db.execute("SELECT v FROM meta WHERE k='input_fingerprint'").fetchone()[0], codegraph_complete=c['g'].complete(), n_files_parsed=c.get('files_parsed'), changed_functions=c.get('changed_fids'), n_batches=c.get('n_batches'))
    finally:
        if 'wt' in c: c['_wt_cm'].__exit__(None, None, None)
        manifest['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S'); json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), ensure_ascii=False, indent=1)
    log(f'done -> {out}'); print(out); return out
