"""Reachability: 변경된 함수에서 도메인 관문(latch/lock/log/sysop)과 진입점까지의 호출 경로를 BFS 로 뽑는다.

Metis 의 원칙: 경로는 CodeGraph 의 결정론적 증거이고, 그래프가 불완전(미해석 호출)하면 '경로 없음'을
단정하지 않는다(fail-open). max_path_length 로 보고 경로를 묶는다.
"""
from collections import deque
from dataclasses import dataclass
from typing import Dict, List, Optional, Set, Tuple
from .codegraph import CodeGraph

@dataclass
class Path:
    nodes: List[str]
    reason: str        # 어느 관문/진입점에 닿았나
    complete: bool     # 경로 위 미해석 호출이 없었나

ENTRYPOINT_HINTS = ('xqmgr_', 'xlocator_', 'xbtree_', 'xstats_', 'xlogtb_', 'sbtree_', 'slocator_', 'net_server_', 'qexec_execute_query', 'do_', 'xtran_')

def _fact_kinds(g: CodeGraph, fid: str) -> Set[str]:
    return {k for k, _, _ in g.facts(fid)}

def paths_to_gates(g: CodeGraph, start: str, kinds: Tuple[str, ...], max_len: int = 8, max_paths: int = 6) -> List[Path]:
    """start 에서 아래로(callee 방향) 내려가며 kinds 관문을 직접 부르는 함수까지의 경로."""
    out: List[Path] = []
    seen: Set[str] = {start}
    q = deque([([start], True)])
    while q and len(out) < max_paths:
        path, complete = q.popleft()
        cur = path[-1]
        hit = _fact_kinds(g, cur) & set(kinds)
        if hit and len(path) > 1 or (hit and cur == start):
            out.append(Path(list(path), '+'.join(sorted(hit)), complete))
            if cur != start: continue
        if len(path) >= max_len: continue
        if g.unresolved(cur): complete = False
        for nxt in g.callees(cur):
            if nxt not in seen:
                seen.add(nxt); q.append((path + [nxt], complete))
    return out

def paths_from_entrypoints(g: CodeGraph, target: str, max_len: int = 8, max_paths: int = 6) -> List[Path]:
    """target 에서 위로(caller 방향) 올라가며 서버 진입점(x*/s*/net_server_)까지의 경로 — '어느 요청이 이 코드를 태우나'."""
    out: List[Path] = []
    seen: Set[str] = {target}
    q = deque([([target], True)])
    while q and len(out) < max_paths:
        path, complete = q.popleft()
        cur = path[-1]
        name = cur.split(':', 1)[1]
        if len(path) > 1 and name.startswith(ENTRYPOINT_HINTS):
            out.append(Path(list(reversed(path)), 'entrypoint', complete)); continue
        if len(path) >= max_len: continue
        callers = g.callers(cur)
        if not callers and len(path) > 1:
            out.append(Path(list(reversed(path)), 'root(no caller resolved)', complete)); continue
        for prv in callers:
            if prv not in seen:
                seen.add(prv); q.append((path + [prv], complete))
    return out

def latch_pairing(g: CodeGraph, fid: str) -> Dict[str, int]:
    """관문 짝 검사(결정론적 증거): fix/unfix, lock/unlock, sysop start/end, alloc/free 의 호출 수 차."""
    facts = g.facts(fid)
    c = lambda k: sum(1 for kind, _, _ in facts if kind == k)
    return {'latch_fix-unfix': c('latch_fix') - c('latch_unfix'), 'lock_acquire-release': c('lock_acquire') - c('lock_release'),
            'sysop_start-end': c('sysop_start') - c('sysop_end'), 'alloc-free': c('alloc') - c('free')}
