"""Adjudicator: LLM(또는 사람)이 낸 finding 을 결정론적 규칙으로 판정한다 — Metis triage 의 'obligation + deterministic gate' 미러.

finding 형식(JSON): {id, layer:'설계'|'코드', file, line, claim, evidence:[file:line...], rule_ids:[...], severity}
판정: valid | invalid | inconclusive. 과신 금지 — 의무 증거가 없으면 valid 로 올리지 않는다.
"""
import os, re, subprocess
from typing import Dict, List
from .codegraph import CodeGraph

def _line_exists(repo: str, file: str, line: int) -> bool:
    p = os.path.join(repo, file)
    if not os.path.isfile(p): return False
    with open(p, 'rb') as f:
        return sum(1 for _ in f) >= line

def _in_diff(changed: Dict[str, List[int]], file: str, line: int, slack: int = 3) -> bool:
    return any(abs(l - line) <= slack for l in changed.get(file, []))

def adjudicate(repo: str, g: CodeGraph, changed: Dict[str, List[int]], findings: List[Dict], rules_text: str) -> List[Dict]:
    out = []
    rule_ids = set(re.findall(r'\b([A-Z]{2,5}-\d{2})\b', rules_text))
    for f in findings:
        obligations = {'anchor_exists': False, 'anchor_in_diff': False, 'evidence_resolves': False, 'rule_known': False, 'graph_supports': None}
        status = 'inconclusive'; reasons = []
        file, line = f.get('file'), int(f.get('line') or 0)
        if file and line and _line_exists(repo, file, line): obligations['anchor_exists'] = True
        else: reasons.append('anchor file:line 이 저장소에 없다')
        if obligations['anchor_exists'] and _in_diff(changed, file, line): obligations['anchor_in_diff'] = True
        elif f.get('layer') == '코드': reasons.append('anchor 가 diff 밖(±3줄) — 이 PR 이 도입한 결함인지 증명 안 됨')
        ev = f.get('evidence') or []
        ok = 0
        for e in ev:
            m = re.match(r'([^:\s]+):(\d+)', e)
            if m and _line_exists(repo, m.group(1), int(m.group(2))): ok += 1
        obligations['evidence_resolves'] = bool(ev) and ok == len(ev)
        if not obligations['evidence_resolves']: reasons.append(f'evidence {ok}/{len(ev)} 해석됨')
        rids = set(f.get('rule_ids') or [])
        obligations['rule_known'] = bool(rids) and rids <= rule_ids
        if rids and not obligations['rule_known']: reasons.append(f'모르는 규칙 ID {sorted(rids - rule_ids)}')
        # 그래프 검증: 짝 불균형/경로 주장을 사실과 대조
        claim = (f.get('claim') or '')
        if file and line:
            fns = g.functions_in(file, [line])
            if fns:
                from . import reachability as R
                pair = R.latch_pairing(g, fns[0].fid)
                if re.search(r'unfix|누락|leak|누수|짝', claim):
                    obligations['graph_supports'] = any(v > 0 for v in pair.values())
                    if obligations['graph_supports'] is False: reasons.append(f'그래프 상 관문 짝은 균형({pair}) — 주장과 모순(조건 분기 확인 필요)')
        # 결정론적 게이트
        if obligations['anchor_exists'] and obligations['evidence_resolves'] and obligations['graph_supports'] is not False and (f.get('layer') != '코드' or obligations['anchor_in_diff']):
            status = 'valid'
        elif obligations['graph_supports'] is False or not obligations['anchor_exists']:
            status = 'invalid' if not obligations['anchor_exists'] else 'inconclusive'
        out.append(dict(f, status=status, obligations=obligations, reasons=reasons))
    return out

def self_check_build(repo: str, files: List[str], timeout: int = 900) -> Dict:
    """자기 보정 루프의 정적 단계: 변경 파일만 컴파일(빌드 디렉터리의 compile_commands.json 사용). 실패 = finding 이 아니라 하네스가 멈춘다."""
    cc = os.path.join(repo, '..', '..', 'build', 'build_x86_64_release', 'compile_commands.json')
    if not os.path.isfile(cc): return {'ran': False, 'reason': 'compile_commands.json 없음'}
    import json
    cmds = {os.path.relpath(e['file'], repo): e for e in json.load(open(cc)) if e['file'].startswith(repo)}
    res = {}
    for f in files:
        e = cmds.get(f)
        if not e: res[f] = 'no-compile-command'; continue
        cmd = e.get('command') or ' '.join(e.get('arguments', []))
        cmd = re.sub(r'\s-o\s+\S+', ' -o /dev/null', cmd) + ' -fsyntax-only'
        r = subprocess.run(cmd, shell=True, cwd=e['directory'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=timeout)
        res[f] = 'ok' if r.returncode == 0 else r.stderr[-800:]
    return {'ran': True, 'results': res}
