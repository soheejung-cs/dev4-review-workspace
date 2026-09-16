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

def build(repo: str, g: CodeGraph, diff_text: str, rules_dir: str, budget: int = 12000) -> ContextPack:
    changed = parse_unified_diff(diff_text)
    pack = ContextPack(budget=budget)
    seen_text: Set[str] = set()
    struct_refs: Set[str] = set(); axes: Set[str] = set(); sigs: List[str] = []
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
            facts = g.facts(fn.fid); pair = R.latch_pairing(g, fn.fid)
            for k, _, _ in facts: axes |= AXIS_BY_FACT.get(k, set())
            gates = R.paths_to_gates(g, fn.fid, ('latch_fix', 'lock_acquire', 'log_append', 'sysop_start'))
            entries = R.paths_from_entrypoints(g, fn.fid)
            fb = ['domain facts: ' + (', '.join(f'{k}:{c}@{l}' for k, c, l in facts) or 'none'),
                  'pairing (calls in this function only): ' + ', '.join(f'{k}={v:+d}' for k, v in pair.items() if v) or 'pairing: balanced/none',
                  'unresolved callees: ' + (', '.join(g.unresolved(fn.fid)[:12]) or 'none'),
                  'paths to gates: ' + ('; '.join(' -> '.join(p.nodes) + f' [{p.reason}{"" if p.complete else ", incomplete"}]' for p in gates) or 'none within 8 hops'),
                  'entry paths: ' + ('; '.join(' -> '.join(p.nodes) + f' [{p.reason}]' for p in entries) or 'none resolved')]
            pack.sections.append(Section(f'graph evidence {fn.fid}', '\n'.join('- ' + x for x in fb), 2))
            for m in re.finditer(r'\b([A-Z][A-Z0-9_]{3,})\s*\*?\s*\w', body): struct_refs.add(m.group(1))
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
    # 예산 적용: 우선순위 낮은 것부터 잘라낸다
    total = sum(s.tokens for s in pack.sections)
    for s in sorted([s for s in pack.sections], key=lambda s: (-s.priority, -s.tokens)):
        if total <= budget: break
        if s.priority <= 2: continue
        pack.sections.remove(s); pack.dropped.append(f'{s.title} ({s.tokens} tok)'); total -= s.tokens
    return pack
