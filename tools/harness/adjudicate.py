"""Adjudicator: LLM(또는 사람)이 낸 finding 을 결정론적 규칙으로 판정한다 — Metis triage 의 'obligation + deterministic gate' 미러.

finding 형식(JSON): {id, layer:'설계'|'코드', file, line, claim, evidence:[file:line...], rule_ids:[...], severity}
판정: valid | invalid | inconclusive. 과신 금지 — 의무 증거가 없으면 valid 로 올리지 않는다.
"""
import os, re, subprocess
from typing import Dict, List, Tuple
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
        obligations = {'anchor_exists': False, 'anchor_in_diff': False, 'evidence_resolves': False, 'rule_known': False, 'graph_supports': None, 'why_present': False, 'proposal_present': False, 'verified_if_error': True}
        status = 'inconclusive'; reasons = []
        file, line = f.get('file'), int(f.get('line') or 0)
        claim = (f.get('claim') or '')
        why = (f.get('why') or '').strip()
        obligations['why_present'] = len(why) >= 40 and not why.startswith(f.get('claim', '')[:20])
        if not obligations['why_present']: reasons.append('why(왜 문제가 되는지) 가 없거나 claim 반복 — 결과·영향·근거를 적어야 한다')
        prop = (f.get('proposal') or '').strip()
        obligations['proposal_present'] = len(prop) >= 20
        if not obligations['proposal_present']: reasons.append('proposal(제안+예시) 없음 — 무엇을 어떻게 고치라는지와 예시를 적어야 한다')
        ERROR_WORDS = r'데드락|deadlock|크래시|crash|SIGSEGV|누수|leak|오답|wrong|silently|UB|undefined|경합|race|double free|use-after-free|미초기화|uninitializ|무한|hang'
        if re.search(ERROR_WORDS, claim + ' ' + (f.get('why') or ''), re.I):
            v = f.get('verification') or {}
            obligations['verified_if_error'] = v.get('method') in ('static', 'dynamic') and bool((v.get('result') or '').strip())
            if not obligations['verified_if_error']:
                reasons.append('에러 우려인데 verification(static/dynamic 확인 결과) 없음 → severity 를 question 으로 내림')
                f['severity'] = 'question'
        if file and line and _line_exists(repo, file, line): obligations['anchor_exists'] = True
        else: reasons.append('anchor file:line 이 저장소에 없다')
        if obligations['anchor_exists'] and _in_diff(changed, file, line): obligations['anchor_in_diff'] = True
        elif f.get('layer') == '코드': reasons.append('anchor 가 diff 밖(±3줄) — 이 PR 이 도입한 결함인지 증명 안 됨')
        ev = f.get('evidence') or []
        ok = 0
        for e in ev:
            m = re.match(r'([^:\s]+):(\d+)', e)
            if m and (m.group(1) == 'pr-body' or _line_exists(repo, m.group(1), int(m.group(2)))): ok += 1
        obligations['evidence_resolves'] = bool(ev) and ok == len(ev)
        if not obligations['evidence_resolves']: reasons.append(f'evidence {ok}/{len(ev)} 해석됨')
        rids = set(f.get('rule_ids') or [])
        obligations['rule_known'] = bool(rids) and rids <= rule_ids
        if rids and not obligations['rule_known']: reasons.append(f'모르는 규칙 ID {sorted(rids - rule_ids)}')
        # 그래프 검증: 짝 불균형/경로 주장을 사실과 대조
        if file and line:
            fns = g.functions_in(file, [line])
            if fns:
                from . import reachability as R
                pair = R.latch_pairing(g, fns[0].fid)
                if re.search(r'unfix|누락|leak|누수|짝', claim):
                    obligations['graph_supports'] = any(v > 0 for v in pair.values())
                    if obligations['graph_supports'] is False: reasons.append(f'그래프 상 관문 짝은 균형({pair}) — 주장과 모순(조건 분기 확인 필요)')
        # 결정론적 게이트
        if obligations['why_present'] and obligations['proposal_present'] and obligations['anchor_exists'] and obligations['evidence_resolves'] and obligations['graph_supports'] is not False and (f.get('layer') != '코드' or obligations['anchor_in_diff']):
            status = 'valid'
        elif obligations['graph_supports'] is False or not obligations['anchor_exists']:
            status = 'invalid' if not obligations['anchor_exists'] else 'inconclusive'
        out.append(fill_importance(dict(f, status=status, obligations=obligations, reasons=reasons)))
    return out

def self_check_build(repo: str, files: List[str], timeout: int = 900) -> Dict:
    """자기 보정 루프의 정적 단계: 변경 파일만 컴파일(빌드 디렉터리의 compile_commands.json 사용). 실패 = finding 이 아니라 하네스가 멈춘다."""
    cc = os.environ.get('HARNESS_COMPILE_COMMANDS', os.path.expanduser('~/dev/build/build_x86_64_release/compile_commands.json'))
    if not os.path.isfile(cc): return {'ran': False, 'reason': f'compile_commands.json 없음 ({cc})'}
    import json
    src_root = os.path.expanduser('~/dev/sources/cubrid')   # compile_commands 는 원본 소스 경로 기준; 워크트리 파일로 치환해 컴파일한다
    cmds = {os.path.relpath(e['file'], src_root): e for e in json.load(open(cc)) if e['file'].startswith(src_root)}
    res = {}
    for f in files:
        e = cmds.get(f)
        if not e: res[f] = 'no-compile-command'; continue
        cmd = e.get('command') or ' '.join(e.get('arguments', []))
        cmd = re.sub(r'\s-o\s+\S+', ' -o /dev/null', cmd).replace(e['file'], os.path.join(repo, f)) + ' -fsyntax-only'
        r = subprocess.run(cmd, shell=True, cwd=e['directory'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=timeout)
        res[f] = 'ok' if r.returncode == 0 else r.stderr[-800:]
    return {'ran': True, 'results': res}

# ---- 자기 보정 루프 ----
import json, hashlib, glob

def validate_findings(findings: List[Dict], schema_path: str) -> Tuple[List[Dict], List[Dict]]:
    """finding.json 스키마 검증. 통과/실패로 나누고 실패 이유를 붙인다(재질의 입력)."""
    import jsonschema
    schema = json.load(open(schema_path, encoding='utf-8'))
    ok, bad = [], []
    for f in findings:
        errs = sorted(jsonschema.Draft7Validator(schema).iter_errors(f), key=lambda e: list(e.path))
        if errs: bad.append(dict(f, schema_errors=[f'{"/".join(map(str, e.path)) or "<root>"}: {e.message}' for e in errs]))
        else: ok.append(f)
    return ok, bad

def dedup(findings: List[Dict]) -> List[Dict]:
    """배치 간 중복 제거(Metis finding_dedup 의 결정론 부분): 같은 file:line(±2) + claim 앞 40자."""
    seen = {}; out = []
    for f in sorted(findings, key=lambda x: (x.get('file', ''), int(x.get('line') or 0), x.get('claim', ''))):
        key = (f.get('file'), int(f.get('line') or 0) // 3, (f.get('claim') or '')[:40])
        if key in seen: continue
        seen[key] = True; out.append(f)
    return out

def repro_obligation(repo_docs: str, findings: List[Dict], jira: str = '') -> List[Dict]:
    """valid + blocking 인 코드 finding 은 재현 스크립트(projects/<JIRA>/repro/*) 또는 graph_supports=True 가 있어야 blocking 을 유지한다.
    없으면 non-blocking 으로 강등하고 이유를 남긴다 — '재현 없이 결함이라 하지 않는다'."""
    repros = glob.glob(os.path.join(repo_docs, 'projects', jira or '*', 'repro', '*')) if repo_docs else []
    for f in findings:
        if f.get('status') == 'valid' and f.get('severity') == 'blocking' and f.get('layer') == '코드':
            if not repros and not f.get('obligations', {}).get('graph_supports'):
                f['severity'] = 'non-blocking'; f.setdefault('reasons', []).append('재현 스크립트도 그래프 증거도 없어 blocking 을 유지할 수 없음 → non-blocking 강등')
    return findings

def requery(bad: List[Dict], adjudicated: List[Dict], out_path: str) -> int:
    """LLM 재질의 입력: 스키마 실패 + inconclusive 를 이유와 함께 한 파일로. 러너가 이 파일이 비어 있지 않으면 1회 재질의한다."""
    items = [{'id': b.get('id'), 'why': b['schema_errors'], 'finding': b} for b in bad]
    items += [{'id': a.get('id'), 'why': a.get('reasons'), 'finding': {k: v for k, v in a.items() if k not in ('obligations', 'reasons', 'status')}} for a in adjudicated if a.get('status') == 'inconclusive']
    json.dump(items, open(out_path, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    return len(items)


# ---- 중요도·헤더 ----
IMPORTANCE_EMOJI = {'높음': '🔴', '중간': '🟡', '낮음': '🟢'}
LOW_CATEGORIES = {'주석 제안', '문서 제안'}

def fill_importance(f: Dict) -> Dict:
    """비어 있으면 severity·category 로 채운다: blocking → 높음, 주석/문서 → 낮음, 그 외 → 중간. 버그 가능성은 verification 이 있고 사실로 확인됐을 때만 높음."""
    if f.get('importance'): return f
    cat = f.get('category', ''); sev = f.get('severity', '')
    if sev == 'blocking': f['importance'] = '높음'
    elif cat in LOW_CATEGORIES: f['importance'] = '낮음'
    elif cat == '버그 가능성' and (f.get('verification') or {}).get('method') in ('static', 'dynamic') and re.search(r'확인|재현|reproduc', (f.get('verification') or {}).get('result', '')): f['importance'] = '높음' if sev != 'question' else '중간'
    else: f['importance'] = '중간'
    return f

def comment_header(f: Dict) -> str:
    """게시 코멘트 첫 줄: [층] [카테고리] 이모지 — 예) [코드 리뷰] [주석 제안] 🟢"""
    layer = '설계 리뷰' if f.get('layer') == '설계' else '코드 리뷰'
    return f"[{layer}] [{f.get('category', '확인 질문')}] {IMPORTANCE_EMOJI.get(f.get('importance', '중간'), '🟡')}"
