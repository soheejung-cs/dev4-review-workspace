"""ContextPack: LLM 에 넘길 컨텍스트를 토큰 예산 안에서 결정론적으로 조립한다.

우선순위(예산이 모자라면 뒤에서부터 잘린다):
 1. diff 의 변경 함수 본문(줄 번호 포함)          — 항상
 2. 그 함수의 도메인 사실(관문 호출)·짝 검사·경로   — 항상(작다)
    + 불변조건(설계 §2 INV·BTL, 손댄 디렉터리 AGENTS.md 의 latch/lock 줄) — 항상, 배치마다
    + 이미 규명된 함정(dev4-ai-source `modules/<모듈>.md` §3, 손댄 모듈만) — 항상, 그 모듈의 함수가 든 배치에만(H-29)
 3. 변경 함수가 만지는 struct 정의(1 hop)          — 예산 내
 4. 직접 caller/callee 서명(본문 아님)            — 예산 내
 5. rules/ 에서 diff 가 건드린 축의 규칙 행만        — 예산 내
같은 텍스트(구조체·함수)는 한 번만 들어간다(중복 제거). 토큰 추정은 chars/3.2 (C 코드 평균).
"""
import os, re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple
from .codegraph import CodeGraph, FunctionNode
from . import reachability as R
from . import rule_ids as RI

def est_tokens(s: str) -> int: return int(len(s) / 3.2) + 1

@dataclass
class Section:
    title: str
    body: str
    priority: int
    tokens: int = 0
    def __post_init__(self): self.tokens = est_tokens(self.body)

@dataclass
class ContextPack:
    sections: List[Section] = field(default_factory=list)
    dropped: List[str] = field(default_factory=list)
    budget: int = 0
    def render(self) -> str:
        out = [f'<!-- context pack: {sum(s.tokens for s in self.sections)}/{self.budget} tokens, dropped {len(self.dropped)} -->']
        for s in self.sections: out.append(f'\n## {s.title}\n{s.body}')
        if self.dropped: out.append('\n## dropped (over budget)\n- ' + '\n- '.join(self.dropped))
        return '\n'.join(out)

def parse_unified_diff(diff_text: str) -> Dict[str, List[int]]:
    """새 파일 기준 '+'/context 줄 번호 집합(변경 지점 파악용)."""
    changed: Dict[str, List[int]] = {}
    f = None; n = None
    for l in diff_text.split('\n'):
        if l.startswith('+++ '): f = l[6:] if l.startswith('+++ b/') else l[4:]; continue
        m = re.match(r'@@ -\d+(?:,\d+)? \+(\d+)', l)
        if m: n = int(m.group(1)) - 1; continue
        if f is None or n is None: continue
        if l.startswith('-'): continue
        n += 1
        if l.startswith('+'): changed.setdefault(f, []).append(n)
    return changed

def _slice(repo: str, file: str, a: int, b: int) -> str:
    try: lines = open(os.path.join(repo, file), encoding='utf-8', errors='replace').read().split('\n')
    except OSError: return ''
    return '\n'.join(f'{i:6d}  {lines[i-1]}' for i in range(max(1, a), min(len(lines), b) + 1))

# 규칙 행 상한 — 행 평균 56 tok(2026-10-28 rules/ 213행 실측)이라 60행 ≈ 3.4k tok, 팩 예산(12k)의 1/3 이하
RULE_ROWS_CAP = 60

def _rule_rows(rules_dir: str, axes: Set[str], cap: int = RULE_ROWS_CAP) -> str:
    """rules/*.md 의 판정 행 중 축이 axes 에 드는 것만 — **ID 당 1행**(H-28).
    ID 꼴·축은 rule_ids.py 가 정한다(MEAS-R-26 → MEAS-R, A01 → A, PRIO-04.5 → PRIO). 행의 축 = ID 칸의 ID 들의 축 +
    문장이 'X 위반' 으로 가리키는 규칙의 축(17장 안티패턴 `A01 … ALLOC-01 위반` 은 ALLOC 축과 함께 들어온다).
    한 ID 는 처음 나온 행 하나만 싣는다 — 같은 ID 를 가진 뒤 행이 상한을 먹지 않게. ID 칸이 둘인 행(`BR-04/A59`)은 새 ID 가
    하나라도 있으면 싣는다. 상한을 넘겨 뺀 행이 있으면 마지막 줄에 수와 열 곳을 적는다(LLM 이 '없는 것' 을 알게)."""
    rows, seen, cut = [], set(), {}
    for fn in sorted(os.listdir(rules_dir)) if os.path.isdir(rules_dir) else []:
        if not fn.endswith('.md'): continue
        with open(os.path.join(rules_dir, fn), encoding='utf-8') as fp: lines = fp.readlines()
        for l in lines:
            ids = RI.row_ids(l)
            if not ids or all(i in seen for i in ids): continue
            if not (({RI.axis(i) for i in ids} | RI.violated_axes(l)) & axes): continue
            if len(rows) >= cap:
                cut[fn] = cut.get(fn, 0) + 1; continue
            seen.update(ids); rows.append(l.rstrip())
    if cut:
        rows.append(f'- (상한 {cap}행을 넘어 ' + ', '.join(f'rules/{fn} {n}행' for fn, n in sorted(cut.items()))
                    + f' 을 뺐다 — 필요하면 그 문서에서 축 {sorted(axes)} 의 표를 연다)')
    return '\n'.join(rows)

AXIS_BY_FACT = {'latch_fix': {'PAR', 'COH', 'MEM', 'BTL'}, 'lock_acquire': {'PAR', 'CC', 'BTL'},
                'log_append': {'SER', 'SYS'}, 'alloc': {'ALLOC', 'MEM'}, 'sysop_start': {'CC'}}
# 관문 사실로는 안 잡히는 축 — 손댄 디렉터리로 더한다. 옵티마이저(조인 순서·선택도·비용)는 성능 §6 의 플랜 증거 판정(MEAS-R)
AXIS_BY_DIR = {'src/optimizer': {'MEAS-R'}}

def _dir_axes(touched_dirs: Set[str]) -> Set[str]:
    out: Set[str] = set()
    for d in touched_dirs:
        for pre, ax in AXIS_BY_DIR.items():
            if d == pre or d.startswith(pre + '/'): out |= ax
    return out

def _invariants(rules_dir: str, repo: str, touched_dirs: Set[str]) -> str:
    """항상 들어가는 불변조건: 설계-리뷰-규칙 §2 INV 표 + 손대는 디렉터리 AGENTS.md 의 latch/lock 규칙 줄."""
    rows = []
    p = os.path.join(rules_dir, '설계-리뷰-규칙.md')
    if os.path.isfile(p):
        # BTL(B-tree 락킹)은 _rule_rows 가 아니라 여기로 승격한다 — 그쪽은 우선순위 5 라 예산이 모자라면
        # 통째로 잘리는데, 래치 커플링 판정은 잘리면 안 되는 축이다. 대신 무관한 PR 에 7행을 매번 넣지
        # 않도록 storage·transaction 을 건드릴 때만 넣는다.
        want_btl = any(d.startswith('src/storage') or d.startswith('src/transaction') for d in touched_dirs)
        pat = r'\|\s*(INV-\d|BTL-\d)' if want_btl else r'\|\s*INV-\d'
        rows += [l.rstrip() for l in open(p, encoding='utf-8') if re.match(pat, l)]
    for d in sorted(touched_dirs):
        ag = os.path.join(repo, d, 'AGENTS.md')
        if os.path.isfile(ag):
            hits = [l.strip() for l in open(ag, encoding='utf-8', errors='replace') if re.search(r'latch|\bpage lock|\block order|lock(ing)? (order|protocol)|fix.*unfix|must not|never|deadlock', l, re.I) and not l.startswith('|') and not re.search(r'lockfree|lock_free|lock-free', l, re.I)]
            rows += [f'- ({d}/AGENTS.md) {h[:200]}' for h in hits[:8]]
    return '\n'.join(rows)

PITFALLS_TITLE = 'known pitfalls'
# 모듈 하나의 §3 상한 — 2026-10-28 가장 긴 src-storage §3(76줄)을 해소됨 접기 뒤 다 싣는 크기. 넘으면 줄 단위로 자르고 끝에 적는다
PITFALLS_CAP_TOKENS = 3000

def source_notes_root() -> str:
    """소스 분석 지식 리포(dev4-ai-source) 위치 — DEV4_SOURCE_NOTES, 없으면 각 컨테이너의 clone 자리(그 리포 README)."""
    return os.path.expanduser(os.environ.get('DEV4_SOURCE_NOTES') or '~/dev/docs/dev4-ai-source')

def _module_of(d: str) -> Tuple[str, str]:
    """손댄 디렉터리 → (모듈 뿌리, 노트 파일). src/query/parallel → (src/query, src-query.md), pl_engine/x → (pl_engine, pl_engine.md)."""
    parts = [p for p in d.split('/') if p]
    if not parts: return '', ''
    if parts[0] == 'src' and len(parts) > 1: return 'src/' + parts[1], f'src-{parts[1]}.md'
    return parts[0], f'{parts[0]}.md'

def _note_section3(text: str) -> List[str]:
    """노트의 `## 3.` 절 본문(다음 `## ` 머리 전까지). 절 머리 아래 안내 인용문(`> …`)은 뺀다."""
    out, on = [], False
    for l in text.split('\n'):
        if l.startswith('## '):
            if on: break
            on = l.startswith('## 3.')
            continue
        if on and not l.startswith('>'): out.append(l)
    while out and not out[0].strip(): out.pop(0)
    while out and not out[-1].strip(): out.pop()
    return out

def _fold_resolved(lines: List[str]) -> List[str]:
    """`- **[해소됨 …]**` 항목은 첫 줄(200자)만 — 회귀 대조에는 무엇이었나만 있으면 되고, 전문은 미해결 항목에 예산을 쓴다."""
    out, skip = [], False
    for l in lines:
        if l.startswith('- '):
            skip = l.startswith('- **[해소됨')
            out.append(l[:200] + (' …' if skip and len(l) > 200 else '')); continue
        if skip and (l.startswith(' ') or l.startswith('\t')): continue   # 해소됨 항목의 이어지는 줄
        skip = False
        out.append(l)
    return out

def _known_pitfalls(touched_dirs: Set[str], notes_root: str = '', cap: int = PITFALLS_CAP_TOKENS) -> List['Section']:
    """손댄 디렉터리의 모듈 노트 §3(예비 이슈 — 미수정 버그·의심·구조적 한계)을 모듈마다 절 하나로(H-29).
    리뷰가 이미 규명된 함정을 다시 밟거나 미수정 상태를 정상으로 가정하지 않게 — 예전에는 LLM 이 노트를 열어야 했다.
    노트는 읽기만 한다(pull 하지 않는다 — 어느 판을 읽었는지는 manifest 의 source_notes_git)."""
    root = notes_root or source_notes_root()
    mods: Dict[str, Set[str]] = {}
    for d in touched_dirs:
        mroot, fn = _module_of(d)
        if fn: mods.setdefault(fn, set()).add(mroot)
    out = []
    for fn in sorted(mods):
        p = os.path.join(root, 'modules', fn)
        if not os.path.isfile(p): continue
        with open(p, encoding='utf-8', errors='replace') as fp: body = _fold_resolved(_note_section3(fp.read()))
        if not body: continue
        kept, tok = [], 0
        for i, l in enumerate(body):
            if tok + est_tokens(l) > cap:
                kept.append(f'… ({len(body) - i}줄 더 — 상한 {cap} tok. modules/{fn} §3 을 연다)'); break
            kept.append(l); tok += est_tokens(l)
        mroot = sorted(mods[fn])[0]
        out.append(Section(f'{PITFALLS_TITLE} {mroot} (dev4-ai-source modules/{fn} §3)', '\n'.join(kept), 2))
    return out

def _pitfall_root(title: str) -> str:
    """known pitfalls 절 제목의 모듈 뿌리(src/query 등)."""
    return title[len(PITFALLS_TITLE):].split()[0] if title.startswith(PITFALLS_TITLE) else ''

def _episodic(examples_dir: str, files: Set[str]) -> str:
    """과거 리뷰 결과(episodic memory): examples/episodic/PR-*.json 중 같은 파일을 건드린 항목."""
    d = os.path.join(examples_dir, 'episodic'); rows = []
    if not os.path.isdir(d): return ''
    import json
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.json'): continue
        for e in json.load(open(os.path.join(d, fn), encoding='utf-8')):
            if e.get('file') in files:
                tag = e.get('outcome', 'open') if e.get('source') != 'local' else f"local {e.get('status')}/{e.get('severity')}"
                rows.append(f"- [{tag}] PR#{e.get('pr')} {e['file']}:{e.get('line')} — {e.get('claim','')[:160]}")
    return '\n'.join(rows[:20])

def build(repo: str, g: CodeGraph, diff_text: str, rules_dir: str, budget: int = 12000, examples_dir: str = '',
          changed: Dict[str, List[int]] = None, label: str = 'changed function', notes_root: Optional[str] = '') -> ContextPack:
    """changed: 미리 계산한 {file: [lines]} (구현 팩처럼 diff 가 없을 때). 없으면 diff_text 를 파싱한다.
    notes_root: 모듈 노트 리포(빈 문자열 = source_notes_root(), None = 함정 절을 싣지 않음)."""
    changed = changed if changed is not None else parse_unified_diff(diff_text)
    pack = ContextPack(budget=budget)
    seen_text: Set[str] = set()
    struct_refs: Set[str] = set(); axes: Set[str] = set(); sigs: List[str] = []
    touched_dirs = {os.path.dirname(f) for f in changed}
    inv = _invariants(rules_dir, repo, touched_dirs)
    if inv: pack.sections.append(Section('invariants (always included)', inv, 2))
    if notes_root is not None:
        pack.sections += _known_pitfalls(touched_dirs, notes_root)
    for file in sorted(changed):
        if not file.endswith(('.c', '.cpp', '.h', '.hpp', '.cc')): continue
        for fn in g.functions_in(file, changed[file]):
            body = _slice(repo, fn.file, fn.start_line, fn.end_line)
            if body in seen_text: continue
            seen_text.add(body)
            if fn.end_line - fn.start_line > 400:   # 거대 함수: 변경 줄 ±40 만
                ls = [l for l in changed[file] if fn.start_line <= l <= fn.end_line]
                body = _slice(repo, fn.file, min(ls) - 40, max(ls) + 40) + f'\n      ... ({fn.end_line-fn.start_line} lines total, showing changed window)'
            pack.sections.append(Section(f'{label} {fn.fid} [{fn.start_line}-{fn.end_line}]', '```c\n' + body + '\n```', 1))
            facts = g.facts(fn.fid); pair = R.latch_pairing(g, fn.fid); byvar = R.latch_pairing_by_var(g, fn.fid)
            for k, _, _ in facts: axes |= AXIS_BY_FACT.get(k, set())
            gates = R.paths_to_gates(g, fn.fid, ('latch_fix', 'lock_acquire', 'log_append', 'sysop_start'))
            entries = R.paths_from_entrypoints(g, fn.fid)
            fb = ['domain facts: ' + (', '.join(f'{k}:{c}@{l}' for k, c, l in facts) or 'none'),
                  'pairing (calls in this function only): ' + (', '.join(f'{k}={v:+d}' for k, v in pair.items() if v) or 'balanced/none'),
                  'pairing by variable (opened but never closed by name, ownership-transfer excluded): ' + (', '.join(f'{k}:{v}' for k, v in byvar.items()) or 'none'),
                  'unresolved callees: ' + (', '.join(g.unresolved(fn.fid)[:12]) or 'none'),
                  'paths to gates: ' + ('; '.join(' -> '.join(p.nodes) + f' [{p.reason}{"" if p.complete else ", incomplete"}]' for p in gates) or 'none within 8 hops'),
                  'entry paths: ' + ('; '.join(' -> '.join(p.nodes) + f' [{p.reason}]' for p in entries) or 'none resolved')]
            pack.sections.append(Section(f'graph evidence {fn.fid}', '\n'.join('- ' + x for x in fb), 2))
            struct_refs |= set(g.types(fn.fid))           # tree-sitter type_identifier: 실제 참조 타입
            for c in g.callers(fn.fid)[:8] + g.callees(fn.fid)[:8]:
                f2 = g.function(c)
                if f2: sigs.append(f'{f2.fid}{f2.params}  [{f2.file}:{f2.start_line}]')
    for name in sorted(struct_refs):
        for sname, sfile, a, b in g.struct(name)[:1]:
            if b - a > 120: b = a + 120
            pack.sections.append(Section(f'struct {sname} [{sfile}:{a}-{b}]', '```c\n' + _slice(repo, sfile, a, b) + '\n```', 3))
    if sigs: pack.sections.append(Section('direct callers/callees (signatures)', '\n'.join('- ' + s for s in sorted(set(sigs))), 4))
    axes = (axes or {'MEAS'}) | _dir_axes(touched_dirs)
    rows = _rule_rows(rules_dir, axes)
    if rows: pack.sections.append(Section(f'rules for touched axes {sorted(axes)}', rows, 5))
    epi = _episodic(examples_dir, set(changed)) if examples_dir else ''
    if epi:
        # 메모리는 작다(대개 수백 토큰). 예산 컷에서 맨 먼저 잘리던 것을(우선순위 6) 작을 때는 잘리지 않는 급(2)으로 —
        # 2026-09-30: 8029 팩에서 2줄짜리 절이 매번 dropped 로 갔다
        sec = Section('episodic memory (past findings on these files)', epi, 6)
        if sec.tokens <= 600: sec.priority = 2
        pack.sections.append(sec)
    # 예산 적용: 우선순위 낮은 것부터 잘라낸다
    total = sum(s.tokens for s in pack.sections)
    for s in sorted([s for s in pack.sections], key=lambda s: (-s.priority, -s.tokens)):
        if total <= budget: break
        if s.priority <= 2: continue
        pack.sections.remove(s); pack.dropped.append(f'{s.title} ({s.tokens} tok)'); total -= s.tokens
    return pack


def batches(pack: ContextPack, budget: int) -> List[ContextPack]:
    """예산 초과 팩을 파일 단위 배치로 나눈다(Metis same-file batch split). 공유 절(invariants, 우선순위 5 이상 — rules·큰 episodic)은 배치마다 들어간다.
    모듈 함정 절(known pitfalls, H-29)은 그 모듈의 함수가 든 배치에만 들어간다(배치마다 모든 모듈의 §3 을 싣지 않게).
    한 파일의 함수 묶음이 그 자체로 예산을 넘으면 함수 단위로 더 쪼갠다. 결정론: 파일명·함수 시작 순."""
    total = sum(s.tokens for s in pack.sections)
    if total <= budget: return [pack]
    scoped = [s for s in pack.sections if s.title.startswith(PITFALLS_TITLE)]
    shared = [s for s in pack.sections if (s.priority >= 5 or s.title.startswith('invariants')) and s not in scoped]
    per_fn: Dict[str, List[Section]] = {}
    order: List[str] = []
    for s in pack.sections:
        if s in shared or s in scoped: continue
        m = re.match(r'(?:changed function|graph evidence) (\S+?)(?: \[|$)', s.title)   # 함수(fid) 단위 키
        key = m.group(1) if m else '_other'
        if key not in per_fn: per_fn[key] = []; order.append(key)
        per_fn[key].append(s)
    def scoped_for(secs) -> List[Section]:   # 이 절들이 든 배치에 붙을 함정 절 — 함수 절 제목의 파일로 모듈을 정한다
        roots = {_module_of(os.path.dirname(m.group(1)))[0]
                 for s in secs for m in [re.match(r'(?:changed function|candidate function|graph evidence) ([^:\s]+):', s.title)] if m}
        return [s for s in scoped if _pitfall_root(s.title) in roots]
    def need(secs, have_secs) -> int:   # secs 를 이 배치에 넣으면 새로 붙는 함정 절의 토큰
        have = {id(s) for s in scoped_for(have_secs)}
        return sum(s.tokens for s in scoped_for(list(have_secs) + list(secs)) if id(s) not in have)
    shared_tok = sum(s.tokens for s in shared)
    out: List[ContextPack] = []; cur: List[Section] = []; cur_tok = shared_tok; cur_files: Set[str] = set()
    def flush():
        if cur:
            b = ContextPack(budget=budget); b.sections = list(shared) + scoped_for(cur) + list(cur); out.append(b)
    for key in order:
        secs = per_fn[key]; tok = sum(s.tokens for s in secs)
        if tok + shared_tok + need(secs, []) > budget:
            for sec in secs:   # 한 함수가 혼자 예산을 넘는다: 본문을 앞뒤 절반씩 잘라 표시하고 LLM 에 알린다
                if sec.title.startswith('changed function') and sec.tokens > budget // 2:
                    keep = int(len(sec.body) * (budget // 2) / sec.tokens)
                    sec.body = sec.body[:keep] + f'\n      ... (truncated to fit batch budget: {sec.tokens} tok; ask for the rest by line range)\n```'
                    sec.tokens = est_tokens(sec.body)
            tok = sum(s.tokens for s in secs)
        add = need(secs, cur)
        if cur and (cur_tok + tok + add > budget or (cur_files and key not in cur_files and len(cur_files) >= 1 and cur_tok + tok + add > budget * 0.7)):
            flush(); cur = []; cur_tok = shared_tok; cur_files = set()
            add = need(secs, cur)
        cur += secs; cur_tok += tok + add; cur_files.add(key)
    flush()
    for i, b in enumerate(out): b.sections.insert(0, Section(f'batch {i+1}/{len(out)}', f'이 배치는 전체 diff 의 일부다. 다른 배치의 함수는 여기 없다 — 없는 것을 추측하지 말고 "다른 배치 참조" 로 적는다.', 0))
    return out
