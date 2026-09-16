"""ContextPack: LLM 에 넘길 컨텍스트를 토큰 예산 안에서 결정론적으로 조립한다.

우선순위(예산이 모자라면 뒤에서부터 잘린다):
 1. diff 의 변경 함수 본문(줄 번호 포함)          — 항상
 2. 그 함수의 도메인 사실(관문 호출)·짝 검사·경로   — 항상(작다)
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

def _rule_rows(rules_dir: str, axes: Set[str]) -> str:
    """rules/*.md 의 표 행 중 ID 접두어가 axes 에 있는 것만."""
    rows = []
    for fn in sorted(os.listdir(rules_dir)) if os.path.isdir(rules_dir) else []:
        if not fn.endswith('.md'): continue
        for l in open(os.path.join(rules_dir, fn), encoding='utf-8'):
            m = re.match(r'\|\s*([A-Z]{2,5})-\d+', l)
            if m and m.group(1) in axes: rows.append(l.rstrip())
    return '\n'.join(rows[:60])

AXIS_BY_FACT = {'latch_fix': {'PAR', 'COH', 'MEM'}, 'lock_acquire': {'PAR', 'CC'}, 'log_append': {'SER', 'SYS'}, 'alloc': {'ALLOC', 'MEM'}, 'sysop_start': {'CC'}}

def _invariants(rules_dir: str, repo: str, touched_dirs: Set[str]) -> str:
    """항상 들어가는 불변조건: 설계-리뷰-규칙 §2 INV 표 + 손대는 디렉터리 AGENTS.md 의 latch/lock 규칙 줄."""
    rows = []
    p = os.path.join(rules_dir, '설계-리뷰-규칙.md')
    if os.path.isfile(p):
        rows += [l.rstrip() for l in open(p, encoding='utf-8') if re.match(r'\|\s*INV-\d', l)]
    for d in sorted(touched_dirs):
        ag = os.path.join(repo, d, 'AGENTS.md')
        if os.path.isfile(ag):
            hits = [l.strip() for l in open(ag, encoding='utf-8', errors='replace') if re.search(r'latch|\bpage lock|\block order|lock(ing)? (order|protocol)|fix.*unfix|must not|never|deadlock', l, re.I) and not l.startswith('|') and not re.search(r'lockfree|lock_free|lock-free', l, re.I)]
            rows += [f'- ({d}/AGENTS.md) {h[:200]}' for h in hits[:8]]
    return '\n'.join(rows)

def _episodic(examples_dir: str, files: Set[str]) -> str:
    """과거 리뷰 결과(episodic memory): examples/episodic/PR-*.json 중 같은 파일을 건드린 항목."""
    d = os.path.join(examples_dir, 'episodic'); rows = []
    if not os.path.isdir(d): return ''
    import json
    for fn in sorted(os.listdir(d)):
        if not fn.endswith('.json'): continue
        for e in json.load(open(os.path.join(d, fn), encoding='utf-8')):
            if e.get('file') in files:
                rows.append(f"- [{e.get('outcome','open')}] PR#{e.get('pr')} {e['file']}:{e.get('line')} — {e.get('claim','')[:160]}")
    return '\n'.join(rows[:20])

def build(repo: str, g: CodeGraph, diff_text: str, rules_dir: str, budget: int = 12000, examples_dir: str = '') -> ContextPack:
    changed = parse_unified_diff(diff_text)
    pack = ContextPack(budget=budget)
    seen_text: Set[str] = set()
    struct_refs: Set[str] = set(); axes: Set[str] = set(); sigs: List[str] = []
    touched_dirs = {os.path.dirname(f) for f in changed}
    inv = _invariants(rules_dir, repo, touched_dirs)
    if inv: pack.sections.append(Section('invariants (always included)', inv, 2))
    for file in sorted(changed):
        if not file.endswith(('.c', '.cpp', '.h', '.hpp', '.cc')): continue
        for fn in g.functions_in(file, changed[file]):
            body = _slice(repo, fn.file, fn.start_line, fn.end_line)
            if body in seen_text: continue
            seen_text.add(body)
            if fn.end_line - fn.start_line > 400:   # 거대 함수: 변경 줄 ±40 만
                ls = [l for l in changed[file] if fn.start_line <= l <= fn.end_line]
                body = _slice(repo, fn.file, min(ls) - 40, max(ls) + 40) + f'\n      ... ({fn.end_line-fn.start_line} lines total, showing changed window)'
            pack.sections.append(Section(f'changed function {fn.fid} [{fn.start_line}-{fn.end_line}]', '```c\n' + body + '\n```', 1))
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
    rows = _rule_rows(rules_dir, axes or {'MEAS'})
    if rows: pack.sections.append(Section(f'rules for touched axes {sorted(axes)}', rows, 5))
    epi = _episodic(examples_dir, set(changed)) if examples_dir else ''
    if epi: pack.sections.append(Section('episodic memory (past findings on these files)', epi, 6))
    # 예산 적용: 우선순위 낮은 것부터 잘라낸다
    total = sum(s.tokens for s in pack.sections)
    for s in sorted([s for s in pack.sections], key=lambda s: (-s.priority, -s.tokens)):
        if total <= budget: break
        if s.priority <= 2: continue
        pack.sections.remove(s); pack.dropped.append(f'{s.title} ({s.tokens} tok)'); total -= s.tokens
    return pack


def batches(pack: ContextPack, budget: int) -> List[ContextPack]:
    """예산 초과 팩을 파일 단위 배치로 나눈다(Metis same-file batch split). 공유 절(invariants·rules·episodic)은 배치마다 들어간다.
    한 파일의 함수 묶음이 그 자체로 예산을 넘으면 함수 단위로 더 쪼갠다. 결정론: 파일명·함수 시작 순."""
    total = sum(s.tokens for s in pack.sections)
    if total <= budget: return [pack]
    shared = [s for s in pack.sections if s.priority >= 5 or s.title.startswith('invariants')]
    per_fn: Dict[str, List[Section]] = {}
    order: List[str] = []
    for s in pack.sections:
        if s in shared: continue
        m = re.match(r'(?:changed function|graph evidence) (\S+?)(?: \[|$)', s.title)   # 함수(fid) 단위 키
        key = m.group(1) if m else '_other'
        if key not in per_fn: per_fn[key] = []; order.append(key)
        per_fn[key].append(s)
    shared_tok = sum(s.tokens for s in shared)
    out: List[ContextPack] = []; cur: List[Section] = []; cur_tok = shared_tok; cur_files: Set[str] = set()
    def flush():
        if cur:
            b = ContextPack(budget=budget); b.sections = list(shared) + list(cur); out.append(b)
    for key in order:
        secs = per_fn[key]; tok = sum(s.tokens for s in secs)
        if tok + shared_tok > budget:
            for sec in secs:   # 한 함수가 혼자 예산을 넘는다: 본문을 앞뒤 절반씩 잘라 표시하고 LLM 에 알린다
                if sec.title.startswith('changed function') and sec.tokens > budget // 2:
                    keep = int(len(sec.body) * (budget // 2) / sec.tokens)
                    sec.body = sec.body[:keep] + f'\n      ... (truncated to fit batch budget: {sec.tokens} tok; ask for the rest by line range)\n```'
                    sec.tokens = est_tokens(sec.body)
            tok = sum(s.tokens for s in secs)
        if cur and (cur_tok + tok > budget or (cur_files and key not in cur_files and len(cur_files) >= 1 and cur_tok + tok > budget * 0.7)):
            flush(); cur = []; cur_tok = shared_tok; cur_files = set()
        cur += secs; cur_tok += tok; cur_files.add(key)
    flush()
    for i, b in enumerate(out): b.sections.insert(0, Section(f'batch {i+1}/{len(out)}', f'이 배치는 전체 diff 의 일부다. 다른 배치의 함수는 여기 없다 — 없는 것을 추측하지 말고 "다른 배치 참조" 로 적는다.', 0))
    return out
