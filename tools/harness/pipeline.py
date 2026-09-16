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


def _mandatory(rules_dir):
    """두 규칙 문서의 '의무 항목' 절(성능 §2, 설계 §1·§2)만 뽑는다 — LLM 이 매 리뷰에서 반드시 확인할 것."""
    out = []
    for fn, secs in (('성능-리뷰-규칙.md', ('## 2.',)), ('설계-리뷰-규칙.md', ('## 0.', '## 1.', '## 2.'))):
        p = os.path.join(rules_dir, fn)
        if not os.path.isfile(p): continue
        txt = open(p, encoding='utf-8').read(); parts = txt.split('\n## ')
        for part in parts[1:]:
            if any(('## ' + part).startswith(sec) for sec in secs): out.append(f'### [{fn}] ' + part.strip()[:6000])
    return '\n\n'.join(out)

def n_review_request(c, a):
    """LLM 에 줄 단일 입력. 배치가 여럿이면 배치마다 하나. 지시는 스킬 두 개의 산출물 절을 요약한 고정 문구."""
    rules_dir = os.path.join(ROOT, 'rules'); mand = _mandatory(rules_dir)
    arch_p = os.path.join(c['out'], 'arch.json'); arch = json.load(open(arch_p, encoding='utf-8')) if os.path.isfile(arch_p) else None
    auto_p = os.path.join(c['out'], 'findings.auto.json'); auto = json.load(open(auto_p, encoding='utf-8')) if os.path.isfile(auto_p) else []
    schema = open(os.path.join(ROOT, 'harness', 'schemas', 'finding.json'), encoding='utf-8').read()
    head = f"""# review_request — PR #{c['pr']} {c['meta']['title']} ({c['meta']['headRefOid'][:9]}, {c['meta']['jira']})

## 지시 (리뷰용 + 설계용을 한 번에)
1. 아래 컨텍스트 팩만 읽는다. 팩에 없는 코드는 "없음"으로 적고 추측하지 않는다(다른 배치 참조 가능).
2. 지적은 두 층으로 낸다 — **[코드 리뷰]**: 정확성·관문 짝·에러 경로·핫패스 비용(성능 규칙 ID 인용). **[설계 리뷰]**: 아래 불변조건·계층 간선(arch.json)·공유 자원 계약(설계 규칙 §4)·대안표·결정 요청 Q.
3. 모든 지적에 **`why`(왜 문제가 되는지)** 를 쓴다 — 이대로 두면 누가/무엇이 어떻게 되는지(오답·정지·회복 불가·비결정성·발견 가능성), 근거(수치·규칙 ID·스펙 절). why 없는 지적은 판정에서 거절된다.
3-1. 모든 지적에 **`proposal`(제안 + 예시)** 를 쓴다 — 주석 관련이면 그대로 붙일 수 있는 확정 문구(게시 시 ```suggestion 블록), 코드면 스케치, 문서면 문구, 테스트면 TC 시나리오. "고쳐 달라"만 있는 지적은 판정에서 거절된다.
3-2. **에러 우려**(데드락·크래시·누수·오답·UB·경합) 지적은 `verification` 을 채운다 — 하네스/이 세션이 무엇을 어떻게 확인했나(`static`: 관련 함수를 직접 읽어 순서·초기화 등을 확정 / `dynamic`: 빌드·재현 스크립트 실행 결과). 확인 없이 우려만 있으면 severity 를 `question` 으로 내린다.
3-3. 산출은 `findings.json`(스키마 아래) 하나. `layer` 는 '코드'|'설계', `evidence` 는 `file:line`(팩 안의 줄) 또는 `pr-body:N`, 성능 지적은 `rule_ids` 필수. 설계 지적은 `arch_edge` 인용.
4. 자동 finding(MEAS)이 있으면 그대로 두고 필요하면 `claim` 만 보강한다. 게시 문장은 사람처럼, 첫 줄에 층 표기 — 게시는 판정(adjudicate) 뒤 사용자 승인 후.

## 의무 항목 (매 리뷰 확인)
{mand}

## 자동 finding (perf_claims)
```json
{json.dumps(auto, ensure_ascii=False, indent=1)}
```

## 아키텍처 유추 (arch.json 요약)
""" + (('- touched layers: ' + ', '.join(arch['architecture']['touched_layers']) + '\n- edges: ' + '; '.join(f"{e['source']}->{e['target']}({e['calls']})" for e in arch['architecture']['connections'][:30]) + '\n- risks: ' + ('\n  - '.join(f"[{r['severity']}] {r['kind']} {r['component']}: {r['issue']}" for r in arch['risks']) or 'none')) if arch else '- (design infer 미실행)') + f"""

## finding 스키마
```json
{schema}
```
"""
    packs = sorted(f for f in os.listdir(c['out']) if f.startswith('context_pack') and f.endswith('.md'))
    batches = [f for f in packs if '.batch' in f] or ['context_pack.md']
    written = []
    for i, b in enumerate(batches):
        name = 'review_request.md' if len(batches) == 1 else f'review_request.batch{i+1}.md'
        body = open(os.path.join(c['out'], b), encoding='utf-8').read()
        open(os.path.join(c['out'], name), 'w', encoding='utf-8').write(head + f"\n## 컨텍스트 팩 ({b})\n" + body); written.append(name)
    c['log'](f'review_request: {len(written)} file(s) → {written}'); return written

def n_report(c, a):
    """findings.adjudicated.json → report.md (skills/code-review §5 형식의 골격: TL;DR·[설계 리뷰]·[코드 리뷰]·돌릴 것)."""
    res = c.get('adjudicated') or []
    def fmt(r):
        v = r.get('verification') or {}
        return (f"- `{r.get('file')}:{r.get('line')}` — {r.get('claim')}\n  - 왜 문제인가: {r.get('why', '(없음)')}\n  - 제안: {r.get('proposal', '(없음)')}"
                + (f"\n  - 검증: [{v.get('method')}] {v.get('result')}" + (f" ({v.get('artifact')})" if v.get('artifact') else '') if v else '')
                + f"\n  - status **{r.get('status')}**, {r.get('severity')}, rules {r.get('rule_ids') or '-'}" + (f"; {'; '.join(r.get('reasons'))}" if r.get('reasons') else ''))
    valid = [r for r in res if r['status'] == 'valid']; inc = [r for r in res if r['status'] != 'valid']
    blocking = [r for r in valid if r.get('severity') == 'blocking']
    tl = 'Blocking' if blocking else ('Non-blocking' if valid else '작성자 확인 필요')
    md = [f"# PR #{c['pr']} 리뷰 보고서 (하네스 생성 골격)", '', f"**PR:** https://github.com/CUBRID/cubrid/pull/{c['pr']}  **HEAD:** `{c['meta']['headRefOid'][:9]}`  **JIRA:** {c['meta']['jira']}", '',
          f"> **TL;DR** ({tl}): valid {len(valid)} (blocking {len(blocking)}) · inconclusive/invalid {len(inc)} · requery {sum(1 for r in inc if r['status']=='inconclusive')}", '', '## Findings', '', '### [설계 리뷰]', '']
    md += [fmt(r) for r in valid if r.get('layer') == '설계'] or ['없음']; md += ['', '### [코드 리뷰]', '']
    md += [fmt(r) for r in valid if r.get('layer') == '코드'] or ['없음']; md += ['', '### 판정 보류 (requery.json)', '']
    md += [fmt(r) for r in inc] or ['없음']
    arch_p = os.path.join(c['out'], 'arch.json')
    if os.path.isfile(arch_p):
        arch = json.load(open(arch_p, encoding='utf-8')); md += ['', '## 아키텍처 리스크 (결정론 탐지)', ''] + [f"- [{r['severity']}] {r['kind']} `{r['component']}` — {r['issue']}" for r in arch['risks']] or ['없음']
    md += ['', '## 돌릴 것 (제안 — 요청자 확인 후)', '', f"- 변경 계층 {json.load(open(arch_p))['architecture']['touched_layers'] if os.path.isfile(arch_p) else '?'} → review-testing 매트릭스로 CTP/동시성/JOB/TPC-H 제안", '', f"_manifest: harness {sh('git','-C',ROOT,'rev-parse','--short','HEAD').stdout.strip()}, model {os.environ.get('HARNESS_MODEL','unset')}_"]
    open(os.path.join(c['out'], 'report.md'), 'w', encoding='utf-8').write('\n'.join(md)); c['log']('report: report.md'); return md

REGISTRY = {'review_request': n_review_request, 'report': n_report, 'worktree': n_worktree, 'diff_map': n_diff_map, 'codegraph': n_codegraph, 'pack': n_pack, 'arch': n_arch, 'perf_claims': n_perf_claims,
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
