"""아키텍처 유추 + 리스크 탐지 (Azure Architecture Review Agent 의 파이프라인을 코드 그래프에 적용).

Azure 샘플은 텍스트에서 components/connections 를 유추하고 템플릿 리스크(SPOF·공유 DB·fan-in)를 돈다.
여기서는 유추의 입력이 CodeGraph 다: 컴포넌트 = 사전설계문서 3장의 계층([A]~[I]), 연결 = 계층 간 해석된 호출.
리스크 탐지는 규칙 기반(결정론)이고 LLM 은 '설명'만 한다.
"""
import json
from collections import Counter, defaultdict
from typing import Dict, List, Tuple
from .codegraph import CodeGraph

LAYERS = [  # (id, dir prefix, 표시 이름)  — 사전설계문서 §3
    ('A', 'src/broker/', 'broker/CAS'), ('B', 'src/parser/', 'parser'), ('B', 'src/optimizer/', 'optimizer'), ('B', 'src/xasl/', 'xasl'),
    ('C', 'src/object/', 'object'), ('C', 'src/compat/', 'compat'), ('D', 'src/connection/', 'connection'), ('D', 'src/communication/', 'communication'),
    ('E', 'src/query/', 'query'), ('E', 'src/thread/', 'thread'), ('E', 'src/session/', 'session'), ('F', 'src/transaction/', 'transaction'),
    ('G', 'src/storage/', 'storage'), ('H', 'src/sp/', 'sp'), ('H', 'src/method/', 'method'), ('I', 'src/base/', 'base'), ('I', 'src/executables/', 'executables'),
]
# 사전설계 §4: 허용 의존(아래 방향) + 공식 사이클 4개
ALLOWED_CYCLES = {frozenset({'storage', 'transaction'}), frozenset({'query', 'storage'}), frozenset({'parser', 'optimizer'})}
HUB = 'object'
DOWNWARD_OK = {'base', 'compat'}  # 누구나 부를 수 있는 바닥
# [D] 통신 계층은 요청 디스패처다: x*/s* 서버 함수를 부르는 간선은 3파일 계약(사전설계 §3 [D]) 의 일부이지 사이클이 아니다
DISPATCH_OK = {('communication', 'storage'), ('communication', 'query'), ('communication', 'transaction'), ('communication', 'object')}

def layer_of(path: str) -> Tuple[str, str]:
    for lid, pfx, name in LAYERS:
        if path.startswith(pfx): return lid, name
    return ('?', path.split('/')[1] if path.count('/') >= 1 else path)

def infer(g: CodeGraph, changed_fids: List[str]) -> Dict:
    comps: Dict[str, Dict] = {}
    conns: Counter = Counter()
    samples: Dict[Tuple[str, str], List[str]] = defaultdict(list)
    for caller, resolved in g.db.execute('SELECT caller, resolved FROM calls WHERE resolved IS NOT NULL'):
        a = layer_of(caller.split(':', 1)[0])[1]; b = layer_of(resolved.split(':', 1)[0])[1]
        for n in (a, b): comps.setdefault(n, {'id': n, 'name': n, 'type': 'layer', 'functions': 0})
        if a != b:
            conns[(a, b)] += 1
            if len(samples[(a, b)]) < 3: samples[(a, b)].append(f'{caller} -> {resolved}')
    for (file,), in [(r,) for r in g.db.execute('SELECT file FROM functions')]:
        n = layer_of(file)[1]; comps.setdefault(n, {'id': n, 'name': n, 'type': 'layer', 'functions': 0}); comps[n]['functions'] += 1
    connections = [{'source': a, 'target': b, 'calls': c, 'samples': samples[(a, b)]} for (a, b), c in sorted(conns.items())]
    # 변경이 닿은 계층
    touched = sorted({layer_of(f.split(':', 1)[0])[1] for f in changed_fids})
    return {'components': sorted(comps.values(), key=lambda c: c['id']), 'connections': connections, 'touched_layers': touched}

def detect_risks(arch: Dict, g: CodeGraph, changed_fids: List[str]) -> List[Dict]:
    risks = []
    edges = {(c['source'], c['target']): c for c in arch['connections']}
    # 이 PR 이 도입한 계층 간선만 리스크로 올린다 — 변경 함수가 caller 인 해석 호출. 저장소에 원래 있던 사이클은 리뷰 대상이 아니다(context 로만 arch.json 에 남는다).
    changed = set(changed_fids); new_edges = {}
    for caller, resolved in g.db.execute('SELECT caller, resolved FROM calls WHERE resolved IS NOT NULL'):
        if caller not in changed: continue
        a = layer_of(caller.split(':', 1)[0])[1]; b = layer_of(resolved.split(':', 1)[0])[1]
        if a != b:
            e = new_edges.setdefault((a, b), {'source': a, 'target': b, 'calls': 0, 'samples': []}); e['calls'] += 1
            if len(e['samples']) < 3: e['samples'].append(f'{caller} -> {resolved}')
    arch['changed_edges'] = sorted(new_edges.values(), key=lambda e: (e['source'], e['target']))
    # 1) 계층 위반: 변경 함수가 만든 간선이 공식 사이클 밖의 역방향을 닫거나, 바닥(base/compat)이 위를 부르는 것
    for (a, b), c in new_edges.items():
        if a in DOWNWARD_OK or b in DOWNWARD_OK or HUB in (a, b) or (a, b) in DISPATCH_OK or (b, a) in DISPATCH_OK: continue
        if (b, a) in edges and frozenset({a, b}) not in ALLOWED_CYCLES:
            risks.append({'kind': 'layer-cycle', 'severity': 'high', 'component': f'{a} <-> {b}', 'issue': f'공식 사이클(사전설계 §4) 밖의 양방향 의존 ({c["calls"]}/{edges[(b,a)]["calls"]} calls)', 'evidence': c['samples'][:2] + edges[(b, a)]['samples'][:1], 'rule': '설계-리뷰-규칙 §3'})
        if a in DOWNWARD_OK and b not in DOWNWARD_OK and b != HUB:
            risks.append({'kind': 'layer-inversion', 'severity': 'high', 'component': f'{a} -> {b}', 'issue': '바닥 계층이 상위를 호출', 'evidence': c['samples'][:2], 'rule': '설계-리뷰-규칙 §3'})
    # 2) 변경 함수 단위: 관문 짝 불균형(latch/lock/sysop/alloc) — 데드락·누수 후보
    from . import reachability as R
    for fid in changed_fids:
        pair = R.latch_pairing(g, fid)
        for k, v in pair.items():
            if v > 0:
                risks.append({'kind': 'gate-imbalance', 'severity': 'medium', 'component': fid, 'issue': f'{k} = {v:+d} (이 함수 안의 호출 수 차; 에러 경로 unfix/unlock 누락 후보 — 조건 분기·호출자 위임이면 오탐)', 'evidence': [f'{fid}:{l} {c}' for kd, c, l in g.facts(fid) if kd.split("_")[0] == k.split("_")[0]][:6], 'rule': 'src-storage §2 pgbuf 짝 규약 / 설계-리뷰-규칙 §4 P3'})
        # 3) 락 획득 순서: 한 함수 안에서 latch 를 잡은 채 lock_object 를 부르면(래치 → 락) 데드락 등급 A 자원 순서 위반 후보
        kinds = [(l, k) for k, _, l in g.facts(fid)]
        held = 0
        for l, k in sorted(kinds):
            if k == 'latch_fix': held += 1
            elif k == 'latch_unfix': held = max(0, held - 1)
            elif k == 'lock_acquire' and held > 0:
                risks.append({'kind': 'latch-then-lock', 'severity': 'high', 'component': fid, 'issue': f'페이지 래치를 든 채 트랜잭션 락 요청(line {l}) — 래치 대기 vs 락 대기 교착 후보(AGENTS.md storage: latch 는 물리, lock 은 논리)', 'evidence': [f'{fid}:{l}'], 'rule': '설계-리뷰-규칙 §4 A등급'}); break
    # 4) SPOF 유사: 변경 함수가 fan-in 이 큰(>=20 callers) 공유 함수면 파급 등급 표시
    for fid in changed_fids:
        n = len(g.callers(fid))
        if n >= 20: risks.append({'kind': 'hot-shared', 'severity': 'medium', 'component': fid, 'issue': f'callers {n} — 변경 파급이 넓다(핫패스면 성능 규칙 §2 의무 항목)', 'evidence': g.callers(fid)[:5], 'rule': '성능-리뷰-규칙 §2'})
    return sorted(risks, key=lambda r: ({'critical': 0, 'high': 1, 'medium': 2, 'low': 3}[r['severity']], r['kind'], r['component']))

def to_mermaid(arch: Dict, risks: List[Dict]) -> str:
    bad = {r['component'] for r in risks if r['kind'] in ('layer-cycle', 'layer-inversion')}
    lines = ['flowchart LR']
    for c in arch['components']:
        tag = ':::touched' if c['name'] in arch['touched_layers'] else ''
        lines.append(f'  {c["id"]}["{c["name"]} ({c["functions"]} fn)"]{tag}')
    for e in arch['connections']:
        style = '-.->' if f'{e["source"]} <-> {e["target"]}' in bad or f'{e["target"]} <-> {e["source"]}' in bad else '-->'
        lines.append(f'  {e["source"]} {style}|{e["calls"]}| {e["target"]}')
    lines.append('  classDef touched fill:#fde68a,stroke:#b45309;')
    return '\n'.join(lines)

def to_excalidraw(arch: Dict) -> Dict:
    """Azure 샘플의 _layout/_rect/_arrow 를 최소로 미러한 Excalidraw 요소 JSON."""
    els = []; pos = {}
    for i, c in enumerate(arch['components']):
        x, y = 40 + (i % 4) * 260, 40 + (i // 4) * 160
        pos[c['id']] = (x, y)
        col = '#fde68a' if c['name'] in arch['touched_layers'] else '#e0f2fe'
        els.append({'type': 'rectangle', 'id': f'r-{c["id"]}', 'x': x, 'y': y, 'width': 200, 'height': 80, 'backgroundColor': col, 'strokeColor': '#1e293b', 'roundness': {'type': 3}})
        els.append({'type': 'text', 'id': f't-{c["id"]}', 'x': x + 12, 'y': y + 28, 'width': 176, 'height': 24, 'text': f'{c["name"]} ({c["functions"]})', 'fontSize': 16, 'fontFamily': 1})
    for e in arch['connections']:
        (sx, sy), (tx, ty) = pos[e['source']], pos[e['target']]
        els.append({'type': 'arrow', 'id': f'a-{e["source"]}-{e["target"]}', 'x': sx + 200, 'y': sy + 40, 'width': tx - sx - 200, 'height': ty - sy, 'points': [[0, 0], [tx - sx - 200, ty - sy]], 'strokeColor': '#334155', 'label': {'text': str(e['calls'])}})
    return {'type': 'excalidraw', 'version': 2, 'source': 'dev4-harness', 'elements': els, 'appState': {'viewBackgroundColor': '#ffffff'}}
