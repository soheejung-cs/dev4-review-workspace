"""YAML 러너: harness/pipelines/*.yaml 이 실행을 소유한다(Metis execution-graph 원칙).
- 노드의 impl 은 레지스트리 이름이어야 하고, 모르는 이름·빠진 입력은 로딩 시점에 실패한다.
- manifest.json 에 결정론 기록: pr/head, harness git sha, pipeline yaml sha, rules sha, skills(prompt) sha, codegraph fingerprint, model_id(환경 HARNESS_MODEL).
- 외부 호출은 gh 만. LLM 호출은 없고, LLM 산출물(findings.json)은 adjudicate 파이프라인의 입력이다.
"""
import hashlib, json, os, re, subprocess, sys, time
import yaml
from . import codegraph as CG, context_pack as CP, arch_infer as AI, adjudicate as AD, perf_claims as PC, episodic as EP, or_buf as OB, pr_refs as PR
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
    if c.get('local_base'):
        # 로컬 head(미push 커밋) 리뷰: PR 의 diff 대신 base...sha 의 git diff
        c['diff'] = sh('git', '-C', c['repo'], 'diff', f"{c['local_base']}...{c['sha']}").stdout
    else:
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
    inc = AI.include_edges(c.need('wt'), c.need('diff')); arch['include_edges'] = inc
    risks = sorted(risks + AI.include_risks(inc) + AI.mode_guard_changes(c['diff']), key=lambda r: ({'critical': 0, 'high': 1, 'medium': 2, 'low': 3}[r['severity']], r['kind'], r['component']))
    if inc: c['log'](f"arch: new includes {len(inc)} ({sum(1 for e in inc if e['cross_layer'])} cross-layer)")
    json.dump({'architecture': arch, 'risks': risks}, open(os.path.join(c['out'], 'arch.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(c['out'], 'arch.mmd'), 'w').write(AI.to_mermaid(arch, risks)); json.dump(AI.to_excalidraw(arch), open(os.path.join(c['out'], 'arch.excalidraw'), 'w'))
    c['log'](f'arch: {len(arch["components"])} layers, {len(arch["connections"])} edges, touched {arch["touched_layers"]}, risks {len(risks)}'); c['risks'] = risks; return arch
def n_perf_claims(c, a):
    body = c['meta'].get('body') or ''; fids = c.need('changed_fids')
    fn = c['g'].function(fids[0]) if fids else None
    auto = PC.analyze(body, fn.file if fn else (sorted(c['changed'])[0] if c['changed'] else 'PR'), fn.start_line if fn else 1)
    json.dump(auto, open(os.path.join(c['out'], 'findings.auto.json'), 'w'), ensure_ascii=False, indent=1); c['log'](f'perf claims: {len(auto)} auto findings'); return auto
def n_or_buf(c, a):
    """OR_ALIGNED_BUF 선언 크기 vs or_(un)pack_* 체인의 실제 배치. 산술이라 LLM 없이 결정론으로 돈다(PR#7899).
    findings.auto.json 은 perf_claims 가 이미 썼으므로 **덮어쓰지 않고 append** 한다."""
    auto, skipped = OB.analyze(c.need('wt'), c.need('g'), c.need('changed'), c.need('changed_fids'))
    p = os.path.join(c['out'], 'findings.auto.json')
    prev = json.load(open(p, encoding='utf-8')) if os.path.isfile(p) else []
    json.dump(prev + auto, open(p, 'w'), ensure_ascii=False, indent=1)
    # 건너뛴 체인을 반드시 보고한다 — 이 검사기는 모르면 조용히 넘어가므로, 침묵을 '깨끗함'으로
    # 읽으면 안 된다(반증 2026-09-21: 같은 결함의 흔한 변형 12가지를 놓친다).
    c['log'](f'or-buf: {len(auto)} auto findings · 판정 불가로 건너뛴 체인 {skipped}개')
    c['or_buf_skipped'] = skipped
    return auto
def n_validate(c, a):
    findings = json.load(open(c['findings_path'], encoding='utf-8'))
    if os.path.isfile(os.path.join(c['out'], 'findings.auto.json')) and a.get('merge_auto', True): findings += json.load(open(os.path.join(c['out'], 'findings.auto.json')))
    ok, bad = AD.validate_findings(AD.dedup(findings), os.path.join(ROOT, 'harness', 'schemas', 'finding.json'))
    c['findings_ok'], c['findings_bad'] = ok, bad; c['log'](f'validate: {len(ok)} ok, {len(bad)} schema failures'); return ok
def n_gate(c, a):
    rules_dir = os.path.join(ROOT, 'rules'); rules_text = ''.join(open(os.path.join(rules_dir, f), encoding='utf-8').read() for f in os.listdir(rules_dir) if f.endswith('.md'))
    res = AD.adjudicate(c.need('wt'), c.need('g'), c.need('changed'), c.need('findings_ok'), rules_text)
    docs = AD.records_root(ROOT)   # 팀 리포가 개인 경로를 알지 않도록 env/roster 에서 해석한다
    if not os.path.isdir(docs):
        c['log'](f'repro 의무: 산출물 디렉터리 없음({docs}) — 평가 건너뜀(blocking 유지). DEV4_RECORDS_ROOT 또는 roster.json records_root 설정')
    res = AD.repro_obligation(docs, res, c['meta'].get('jira', ''))
    json.dump(res, open(os.path.join(c['out'], 'findings.adjudicated.json'), 'w'), ensure_ascii=False, indent=1)
    n = AD.requery(c.get('findings_bad', []), res, os.path.join(c['out'], 'requery.json'))
    for r in res: c['log'](f'{r["status"]:12s} {r.get("severity","")[:12]:12s} {r.get("id")} {r.get("file")}:{r.get("line")} {"; ".join(r.get("reasons", []))}')
    c['log'](f'requery items: {n} (schema failures + inconclusive)'); c['adjudicated'] = res; return res
def n_self_check(c, a):
    files = sorted({f for f in c.need('changed') if f.endswith(('.c', '.cpp'))})
    if not any(r['status'] == 'valid' for r in c.get('adjudicated', [])): c['log']('self_check: skipped (no valid finding)'); return None
    r = AD.self_check_build(c.need('wt'), files); c['log'](f'self_check: {r}'); json.dump(r, open(os.path.join(c['out'], 'self_check.json'), 'w')); return r
def n_episodic(c, a):
    p = EP.collect(c['pr'], a.get('reviewer', 'soheejung-cs'), os.path.join(ROOT, 'examples', 'episodic'), local=c.get('adjudicated'))
    n = len(json.load(open(p, encoding='utf-8'))); c['log'](f'episodic: {n} items -> {p}'); return p
def n_pr_refs(c, a):
    """본문이 기대는 외부 사실 — 참조 PR 상태(미머지로 닫힌 것을 "포트했다" 고 적었나), 언급한 커밋 해시가 head 에 있나."""
    r = PR.check(c['pr'], c['meta'].get('body') or '', c['meta']['headRefOid'], 'CUBRID/cubrid', c.get('wt') or os.path.expanduser('~/dev/sources/cubrid'))
    open(os.path.join(c['out'], 'pr_refs.md'), 'w', encoding='utf-8').write(PR.to_md(r)); json.dump(r, open(os.path.join(c['out'], 'pr_refs.json'), 'w'), ensure_ascii=False, indent=1)
    c['pr_refs'] = r; c['log'](f"pr_refs: {len(r['prs'])} PR refs, {len(r['hashes'])} hashes, warnings {len(r['warnings'])}" + (' — ' + ' | '.join(r['warnings']) if r['warnings'] else '')); return r


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

_PACK_RE = re.compile(r'^context_pack\.batch(\d+)\.md$')

def _pack_batches(out_dir):
    """context_pack.batch<N>.md 를 **N 의 수치 순**으로 돌려준다 [(N, filename), ...].
    파일명 문자열 정렬(batch1, batch10, batch100 …)은 배치가 10개를 넘는 순간 번호를 어긋나게 한다
    — review_request.batch8 안에 context_pack.batch105 가 들어가던 버그(2026-10-02, PR#8022 154배치에서 발견)."""
    items = [(int(m.group(1)), f) for f in os.listdir(out_dir) for m in [_PACK_RE.match(f)] if m]
    if items: return sorted(items)
    return [(0, 'context_pack.md')] if os.path.isfile(os.path.join(out_dir, 'context_pack.md')) else []

def _pack_files(path):
    """팩 안 '## changed function <file>:<fn>' 에서 파일별 변경 함수 수."""
    cnt = {}
    for l in open(path, encoding='utf-8', errors='replace'):
        if l.startswith('## changed function '):
            f = l[len('## changed function '):].split(':', 1)[0].strip()
            cnt[f] = cnt.get(f, 0) + 1
    return cnt

def _batch_index(out_dir, batches, max_group=12, chunk=4):
    """배치 색인 + 그룹 계획. 팬아웃 오케스트레이터가 '어느 에이전트에 어느 배치' 를 **팩 내용 기준**으로 정하고,
    세션 한도에 걸리지 않게 몇 개씩 끊어 돌릴지 정하는 데 쓴다(2026-10-02: 15개 동시 × 10배치로 한도 3회 소진)."""
    per = [(n, f, _pack_files(os.path.join(out_dir, f))) for n, f in batches]
    groups, cur = [], []
    def primary(c): return max(c.items(), key=lambda kv: (kv[1], kv[0]))[0] if c else ''
    for n, f, c in per:
        if cur and (primary(c) != primary(cur[-1][2]) or len(cur) >= max_group):
            groups.append(cur); cur = []
        cur.append((n, f, c))
    if cur: groups.append(cur)
    gj = []
    for i, g in enumerate(groups):
        agg = {}
        for _, _, c in g:
            for k, v in c.items(): agg[k] = agg.get(k, 0) + v
        gj.append({'code': f'G{i+1}', 'batches': [n for n, _, _ in g], 'primary': primary(agg),
                   'files': dict(sorted(agg.items(), key=lambda kv: -kv[1]))})
    json.dump({'n_batches': len(per), 'chunk_hint': chunk, 'max_group': max_group, 'groups': gj},
              open(os.path.join(out_dir, 'batch_index.json'), 'w'), ensure_ascii=False, indent=1)
    md = ['# 배치 색인 — 어느 배치가 어느 파일을 담고 있나', '',
          f'배치 {len(per)}개 · 제안 그룹 {len(gj)}개 · 동시 실행 제안 **{chunk}개씩**.', '',
          '## 읽는 법 (팬아웃할 때)',
          '- 공통 지시·의무 항목·finding 스키마는 **`preamble.md` 에 한 번만** 있다. 에이전트는 그걸 한 번 읽고,',
          '  배정된 `review_request.batch<N>.md`(= 팩) 만 읽는다. 배치 파일에 지시문을 매번 싣지 않는다.',
          '- `review_request.batch<N>.md` 의 N 은 `context_pack.batch<N>.md` 의 N 과 **같다**(수치 정렬).',
          f'- 한 번에 {chunk}개 그룹씩 끊어 돌린다 — 그래야 중간에 한도에 걸려도 끝난 그룹의 결과가 남는다.', '',
          '## 그룹 제안', '', '| 그룹 | 배치 | 주 파일 | 파일(변경 함수 수) |', '|---|---|---|---|']
    for g in gj:
        bs = g['batches']; rng = f"{bs[0]}-{bs[-1]}" if len(bs) > 2 and bs == list(range(bs[0], bs[-1]+1)) else ','.join(map(str, bs))
        md.append(f"| {g['code']} | {rng} | `{g['primary']}` | " + ', '.join(f"{k}({v})" for k, v in list(g['files'].items())[:6]) + ' |')
    md += ['', '## 배치별', '', '| 배치 | 파일(변경 함수 수) |', '|---|---|']
    for n, _, c in per:
        md.append(f"| {n} | " + (', '.join(f"{k}({v})" for k, v in sorted(c.items(), key=lambda kv: -kv[1])) or '(없음)') + ' |')
    open(os.path.join(out_dir, 'batch_index.md'), 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    return gj

def n_review_request(c, a):
    """LLM 에 줄 입력. 공통 지시(preamble.md)는 **한 번만** 쓰고, 배치 파일에는 팩만 담는다.
    배치가 하나면 종전처럼 자립 파일(review_request.md)로 쓴다."""
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
3-3. 모든 지적에 `category`(버그 가능성 / 성능 검토 / 설계 판정 / 주석 제안 / 문서 제안 / 테스트 제안 / 측정 요청 / 확인 질문)를 붙이고 `importance`(높음 🔴 / 중간 🟡 / 낮음 🟢)를 판단한다 — 주석·문서 제안은 낮음, 확인된 버그·머지 차단은 높음. 게시 첫 줄은 `[층] [카테고리] 이모지`.
3-5. **`function_notes` — 배정 팩의 `## changed function` 전부에 쓴다(결함이 없어도).** 항목마다 `file`·`function`·`line`(팩이 적은 함수 시작 줄 그대로)·`role`(누가 부르고 무엇을 돌려주나, 동어반복 금지)·`before`(develop 의 같은 함수를 **실제로 읽고** 쓴다; 신설이면 "develop 에 없음")·`after`·`changed`(시그니처/분기/자료구조/삭제 같은 성격 + 가능하면 ±줄 수)·`risk`(none|watch|defect). 이것이 보고서 **§2 변경 지도**와 한국어 주석 리뷰 PR 의 원천이다 — 이게 비면 보고서가 '지적 목록'이 되어 읽는 사람이 변경을 이해하지 못한다(사용자 지적 2026-10-06: PR#8022 보고서가 89파일 중 24개만 언급, 함수명 3종).
3-6. **develop 과의 관계를 말하는 지적은 `baseline` 을 채운다** — "develop 은 X 였다 / 이 PR 이 바꿨다 / 하나만 고쳤다 / 회귀" 는 전부 기준 rev(merge-base) 의 **같은 함수를 실제로 읽은** `file:line` 과 `claim`(same·differs·new) 이 있어야 valid 다. `same` 이면 선재 결함 — 첫 줄에 "(develop 동일 — 별도 이슈 후보)" 가 붙고 blocking 으로 올라가지 않는다. (PR#8022 2026-10-06: 추출된 셀 함수의 비대칭·날짜 오류 코드·IMPLICIT JSON 검사 — 기준 코드를 읽지 않은 지적 3건을 작성자가 기준 줄 번호로 반박했다. 이동·추출 PR 은 **옮긴 코드의 원본 자리**를 먼저 찾는다.)
3-7. **"이 분기에 닿는다 / 사용자에게 보인다" 는 `reach_trace` 를 채운다** — 분기 하나가 아니라 그 조건을 만드는 쪽(표 생성·등록·호출자가 미리 벗기거나 거르는 전처리)까지 읽은 `file:line` 2곳 이상. 호출자가 하나뿐인 새 helper 는 **호출자가 넘기기 전에 무엇을 정규화했는지**부터 본다. (PR#8022: `-1383` 분기가 "사용자에게 보이는 경로" 라 했으나 부팅 때 만드는 표가 값을 가질 수 있는 타입을 전부 덮었고, IMPLICIT 검사는 호출자가 JSON 을 먼저 벗긴 뒤였다.)
3-4. 산출은 `findings.json`(스키마 아래) 하나. `layer` 는 '코드'|'설계', `evidence` 는 `file:line`(팩 안의 줄) 또는 `pr-body:N`, 성능 지적은 `rule_ids` 필수. 설계 지적은 `arch_edge` 인용.
4. 자동 finding(MEAS)이 있으면 그대로 두고 필요하면 `claim` 만 보강한다. 게시 문장은 사람처럼, 첫 줄에 층 표기 — 게시는 판정(adjudicate) 뒤 사용자 승인 후.

## 의무 항목 (매 리뷰 확인)
{mand}

## 자동 finding (perf_claims)
```json
{json.dumps(auto, ensure_ascii=False, indent=1)}
```

## 본문 참조 확인 (pr_refs)
{chr(10).join('- ⚠ ' + w for w in (c.get('pr_refs') or {}).get('warnings', [])) or '- 참조 PR·커밋 해시 모두 정상 (또는 미실행)'}

## 아키텍처 유추 (arch.json 요약)
""" + (('- touched layers: ' + ', '.join(arch['architecture']['touched_layers']) + '\n- edges: ' + '; '.join(f"{e['source']}->{e['target']}({e['calls']})" for e in arch['architecture']['connections'][:30]) + '\n- risks: ' + ('\n  - '.join(f"[{r['severity']}] {r['kind']} {r['component']}: {r['issue']}" for r in arch['risks']) or 'none')) if arch else '- (design infer 미실행)') + f"""

## finding 스키마
```json
{schema}
```
"""
    batches = _pack_batches(c['out'])
    if not batches: c['log']('review_request: no context pack'); return []
    open(os.path.join(c['out'], 'preamble.md'), 'w', encoding='utf-8').write(head)
    written = ['preamble.md']
    title = f"PR #{c['pr']} {c['meta']['title']} ({c['meta']['headRefOid'][:9]}, {c['meta']['jira']})"
    if len(batches) == 1:
        n, b = batches[0]
        body = open(os.path.join(c['out'], b), encoding='utf-8').read()
        open(os.path.join(c['out'], 'review_request.md'), 'w', encoding='utf-8').write(head + f"\n## 컨텍스트 팩 ({b})\n" + body)
        written.append('review_request.md')
    else:
        for n, b in batches:
            files = _pack_files(os.path.join(c['out'], b))
            thin = (f"# review_request — {title} — batch {n}/{len(batches)}\n\n"
                    f"공통 지시·의무 항목·finding 스키마는 이 디렉터리의 **`preamble.md`** 에 한 번만 있다 — 먼저 한 번 읽고, "
                    f"배치 파일은 팩만 읽는다(여러 배치를 맡아도 지시문은 한 번).\n"
                    f"이 배치의 파일: " + (', '.join(f'`{k}`({v})' for k, v in sorted(files.items(), key=lambda kv: -kv[1])) or '(없음)') +
                    f"\n배치 전체 지도: `batch_index.md`\n\n## 컨텍스트 팩 ({b})\n")
            open(os.path.join(c['out'], f'review_request.batch{n}.md'), 'w', encoding='utf-8').write(
                thin + open(os.path.join(c['out'], b), encoding='utf-8').read())
            written.append(f'review_request.batch{n}.md')
    gj = _batch_index(c['out'], batches)
    written += ['batch_index.md', 'batch_index.json']
    c['log'](f'review_request: {len(batches)} batch file(s) + preamble.md + batch_index (그룹 제안 {len(gj)}개)')
    return written

def n_report(c, a):
    """findings.adjudicated.json → report.md (skills/code-review §5 형식의 골격: TL;DR·[설계 리뷰]·[코드 리뷰]·돌릴 것)."""
    res = c.get('adjudicated') or []
    def fmt(r):
        v = r.get('verification') or {}
        return (f"- {AD.comment_header(r)} `{r.get('file')}:{r.get('line')}` — {r.get('claim')}\n  - 왜 문제인가: {r.get('why', '(없음)')}\n  - 제안: {r.get('proposal', '(없음)')}"
                + (f"\n  - 검증: [{v.get('method')}] {v.get('result')}" + (f" ({v.get('artifact')})" if v.get('artifact') else '') if v else '')
                + f"\n  - status **{r.get('status')}**, {r.get('severity')}, rules {r.get('rule_ids') or '-'}" + (f"; {'; '.join(r.get('reasons'))}" if r.get('reasons') else '')
                + (f"\n  - 통과: {', '.join(r.get('passed'))}" if r.get('passed') else ''))
    ORDER = {'높음': 0, '중간': 1, '낮음': 2}
    res = sorted(res, key=lambda r: ORDER.get(r.get('importance', '중간'), 1))
    valid = [r for r in res if r['status'] == 'valid']; inc = [r for r in res if r['status'] != 'valid']
    blocking = [r for r in valid if r.get('severity') == 'blocking']
    tl = 'Blocking' if blocking else ('Non-blocking' if valid else '작성자 확인 필요')
    md = [f"# PR #{c['pr']} 리뷰 보고서 (하네스 생성 골격)", '', f"**PR:** https://github.com/CUBRID/cubrid/pull/{c['pr']}  **HEAD:** `{c['meta']['headRefOid'][:9]}`  **JIRA:** {c['meta']['jira']}", '',
          f"> **TL;DR** ({tl}): valid {len(valid)} (blocking {len(blocking)}) · inconclusive/invalid {len(inc)} · requery {sum(1 for r in inc if r['status']=='inconclusive')}", '', '## Findings', '', '### [설계 리뷰]', '']
    md += [fmt(r) for r in valid if r.get('layer') == '설계'] or ['없음']; md += ['', '### [코드 리뷰]', '']
    md += [fmt(r) for r in valid if r.get('layer') == '코드'] or ['없음']; md += ['', '### 판정 보류 (requery.json)', '']
    md += [fmt(r) for r in inc] or ['없음']
    # §2 변경 지도 — function_notes 가 있으면 파일→함수 표로 전수 수록한다.
    # 없으면 "왜 없는지"를 적는다: 비어 있으면 보고서가 지적 목록이 되어 읽는 사람이 변경을 이해 못 한다.
    notes = []
    for nm in ('function_notes.json', 'notes.json'):
        np_ = os.path.join(c['out'], nm)
        if os.path.isfile(np_):
            raw = json.load(open(np_, encoding='utf-8'))
            notes = raw['function_notes'] if isinstance(raw, dict) else raw
            break
    if notes:
        byf = {}
        for n in notes: byf.setdefault(n['file'], []).append(n)
        RISK = {'defect': '🔴', 'watch': '🟡', 'none': '·'}
        cm = ['', '## 변경 지도 (파일 → 함수)', '',
              f"변경 함수 {len(notes)}개 / {len(byf)}파일. `risk` 는 · 문제없음 확인 · 🟡 계약이 암묵적 · 🔴 지적 있음.", '']
        for f in sorted(byf):
            cm += [f'### `{f}` ({len(byf[f])}개)', '', '| 함수 | 줄 | | develop 에서는 | 이 PR 에서는 | 바뀐 것 |', '|---|---|---|---|---|---|']
            for n in sorted(byf[f], key=lambda x: int(x.get('line') or 0)):
                cm.append('| `{}` | {} | {} | {} | {} | {} |'.format(
                    n.get('function'), n.get('line'), RISK.get(n.get('risk'), '·'),
                    (n.get('before') or '').replace('|', '\\|').replace('\n', ' '),
                    (n.get('after') or '').replace('|', '\\|').replace('\n', ' '),
                    (n.get('changed') or '').replace('|', '\\|').replace('\n', ' ')))
            cm += ['', '역할: ' + ' / '.join(f"`{n['function']}` {n.get('role','')}" for n in byf[f][:6]), '']
        md = md[:md.index('## Findings')] + cm + md[md.index('## Findings'):]
    else:
        md.insert(md.index('## Findings'), '> ⚠ `function_notes` 가 없어 **변경 지도가 비었다** — 이 보고서만으로는 변경을 이해할 수 없다. 리뷰 단계가 배정 팩의 changed function 전수에 notes 를 써야 한다(review_output.json 스키마).\n')
    arch_p = os.path.join(c['out'], 'arch.json')
    if os.path.isfile(arch_p):
        arch = json.load(open(arch_p, encoding='utf-8')); md += ['', '## 아키텍처 리스크 (결정론 탐지)', ''] + [f"- [{r['severity']}] {r['kind']} `{r['component']}` — {r['issue']}" for r in arch['risks']] or ['없음']
    pr_p = os.path.join(c['out'], 'pr_refs.json')
    if os.path.isfile(pr_p):
        rr = json.load(open(pr_p, encoding='utf-8'))
        md += ['', '## 본문 참조 확인 (pr_refs)', ''] + ([f'- ⚠ {w}' for w in rr['warnings']] or ['- 참조 PR·커밋 해시 모두 정상']) + [f"- 참조 PR: " + ', '.join(f'{a} {b}' for a, b, _ in rr['prs'])] if rr['prs'] else []
    md += ['', '## 돌릴 것 (제안 — 요청자 확인 후)', '', f"- 변경 계층 {json.load(open(arch_p))['architecture']['touched_layers'] if os.path.isfile(arch_p) else '?'} → review-testing 매트릭스로 CTP/동시성/JOB/TPC-H 제안", '', f"_manifest: harness {sh('git','-C',ROOT,'rev-parse','--short','HEAD').stdout.strip()}, model {os.environ.get('HARNESS_MODEL','unset')}_"]
    open(os.path.join(c['out'], 'report.md'), 'w', encoding='utf-8').write('\n'.join(md)); c['log']('report: report.md'); return md

REGISTRY = {'review_request': n_review_request, 'report': n_report, 'worktree': n_worktree, 'diff_map': n_diff_map, 'codegraph': n_codegraph, 'pack': n_pack, 'arch': n_arch, 'perf_claims': n_perf_claims, 'or_buf': n_or_buf,
            'validate': n_validate, 'gate': n_gate, 'self_check': n_self_check, 'episodic': n_episodic, 'pr_refs': n_pr_refs}

def load(path):
    y = yaml.safe_load(open(path, encoding='utf-8'))
    for st, nodes in y['stages'].items():
        if not isinstance(nodes, dict): raise SystemExit(f'pipeline: stage {st} must be a mapping of nodes')
        for name, node in nodes.items():
            if node is not None and not isinstance(node, dict): raise SystemExit(f'pipeline: node {st}.{name} must be a mapping')
            impl = (node or {}).get('impl', name)
            if impl not in REGISTRY: raise SystemExit(f'pipeline {os.path.basename(path)}: stage {st} node {name}: unknown impl {impl!r} (registry: {sorted(REGISTRY)})')
    return y

def run(pipeline_path, pr, out_base, repo, findings_path=None, model=None, local_sha=None, local_base=None):
    """local_sha: PR head 대신 이 로컬 리비전을 리뷰한다(미push 자기 리뷰). local_base(기본 upstream/develop)...local_sha 의 diff."""
    t0 = time.time(); y = load(pipeline_path)
    log = lambda m: print(f'[{time.time()-t0:6.1f}s] {m}', file=sys.stderr)
    meta = json.loads(sh('gh', 'pr', 'view', str(pr), '-R', 'CUBRID/cubrid', '--json', 'headRefOid,title,files,author,body').stdout)
    import re; m = re.search(r'CBRD-\d+', meta['title'] + ' ' + (meta.get('body') or '')); meta['jira'] = m.group(0) if m else ''
    sha = meta['headRefOid']
    if local_sha:
        sha = sh('git', '-C', repo, 'rev-parse', local_sha).stdout.strip(); meta['headRefOid'] = sha
        local_base = local_base or 'upstream/develop'; log(f'local head {sha[:9]} (diff vs {local_base}) — PR head 가 아닌 미push 리비전을 리뷰한다')
    out = os.path.join(out_base, str(pr), sha[:9]); os.makedirs(out, exist_ok=True)
    c = Ctx(pr=pr, out=out, log=log, meta=meta, findings_path=findings_path, repo=repo, sha=sha, local_base=local_base if local_sha else None, _wt_cm=Worktree(repo, sha, out_base))
    manifest = {'pipeline': y['harness'], 'pipeline_sha': _sha([pipeline_path]), 'pr': pr, 'head': sha, 'local_head': bool(local_sha), 'diff_base': local_base if local_sha else 'pr', 'title': meta['title'], 'author': meta['author']['login'], 'jira': meta['jira'],
                'harness_git': sh('git', '-C', ROOT, 'rev-parse', '--short', 'HEAD').stdout.strip(), 'rules_sha': _sha([os.path.join(ROOT, 'rules', f) for f in os.listdir(os.path.join(ROOT, 'rules'))]),
                'skills_sha': _sha([os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(ROOT, 'skills')) for f in fs if f == 'SKILL.md']),
                'model_id': model or os.environ.get('HARNESS_MODEL', 'unset'), 'started': time.strftime('%Y-%m-%dT%H:%M:%S'), 'stages': {}}
    try:
        for st, nodes in y['stages'].items():
            for name, node in (nodes or {}).items():
                node = node or {}; impl = node.get('impl', name)
                if node.get('when') == 'findings' and not findings_path: log(f'{st}.{name}: skipped (no --findings)'); continue
                REGISTRY[impl](c, node); manifest['stages'].setdefault(st, []).append(name)
        if 'g' in c: manifest.update(codegraph_fingerprint=c['g'].db.execute("SELECT v FROM meta WHERE k='input_fingerprint'").fetchone()[0], codegraph_complete=c['g'].complete(), codegraph_parse_quality=c['g'].parse_quality(), n_files_parsed=c.get('files_parsed'), changed_functions=c.get('changed_fids'), n_batches=c.get('n_batches'))
    finally:
        if 'wt' in c: c['_wt_cm'].__exit__(None, None, None)
        manifest['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S'); json.dump(manifest, open(os.path.join(out, 'manifest.json'), 'w'), ensure_ascii=False, indent=1)
    log(f'done -> {out}'); print(out); return out
