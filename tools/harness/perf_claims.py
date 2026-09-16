"""PR 본문의 성능 주장을 결정론적으로 검사해 MEAS 규칙 finding(layer=설계)을 만든다.
검사: 표에 중앙값/median 이 있는데 MAD·표준편차·rep 별 값이 없다(MEAS-07) · 반복 횟수 언급 없음(MEAS-04) · 회귀 테스트(CTP) 근거 없음(MEAS-05) ·
'빠르다/느리다/개선' 주장에 수치 표가 전혀 없음(MEAS-01). anchor 는 첫 변경 함수의 시작 줄, evidence 는 pr-body:<줄>."""
import re
from typing import Dict, List

def analyze(body: str, anchor_file: str, anchor_line: int) -> List[Dict]:
    lines = body.split('\n'); out = []
    def ln(pat):
        for i, l in enumerate(lines, 1):
            if re.search(pat, l, re.I): return i
        return 0
    perf_claim = ln(r'빠르|느리|개선|회귀|speed|faster|slower|regress|wall[- ]?time|throughput|tps')
    table = ln(r'^\|.*(median|중앙값|ms|초|s\b).*\|') or ln(r'\d+(\.\d+)?\s*(초|ms|s\b|sec)')   # 표 또는 단위 붙은 수치
    median = ln(r'median|중앙값')
    disp = ln(r'\bMAD\b|표준편차|stddev|σ|rep\s*별|편차')
    reps = ln(r'\d+\s*회|min-of-|median-of-|warm-?up|\d+\s*runs?')
    ctp = ln(r'\bCTP\b|/run all|test_sql|test_shell|sql\s*/\s*medium|regression suite')
    def F(fid, rule, claim, ev):
        return {'id': fid, 'layer': '설계', 'file': anchor_file, 'line': anchor_line, 'claim': claim, 'evidence': [f'pr-body:{ev or 1}'], 'rule_ids': [rule], 'severity': 'non-blocking', 'auto': True}
    if perf_claim and not table: out.append(F('MEAS-01-auto', 'MEAS-01', '성능 주장은 있는데 측정 표(타이밍/프로파일)가 본문에 없다 — 측정 먼저', perf_claim))
    if median and not disp: out.append(F('MEAS-07-auto', 'MEAS-07', '중앙값만 있고 산포(MAD/표준편차/rep 별 값)가 없다 — 개선폭이 잡음 범위 안인지 판정 불가', median))
    if table and not reps: out.append(F('MEAS-04-auto', 'MEAS-04', '측정 표에 반복 횟수(min-of-N/median-of-N/warmup) 언급이 없다 — 1회 실행값이면 기준선 오염을 구분 못 한다', table))
    if perf_claim and not ctp: out.append(F('MEAS-05-auto', 'MEAS-05', '성능 변경인데 기능 회귀 테스트(CTP) 통과 근거가 본문에 없다', perf_claim))
    return out
