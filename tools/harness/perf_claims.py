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
    perf_claim = ln(r'빠르|빨라|느리|느려|개선|회귀|향상|단축|처리율|성능|붕괴|회복|병목|경합|\d+\s*배|speed|faster|slower|regress|wall[- ]?time|throughput|tps|latency|timeout')   # 한국어 활용형(빨라·느려)·성능 명사도 잡는다 (.52 2026-09-17)
    table = ln(r'^\|.*(median|중앙값|ms|초|s\b|배|%).*\|') or ln(r'\d+(\.\d+)?\s*(초|ms|s\b|sec|분\b|시간|배|%|건/초|rows?/s)')   # 표 또는 단위 붙은 수치 (한국어 단위 배·%·건/초·분·시간 포함)
    median = ln(r'median|중앙값')
    disp = ln(r'\bMAD\b|표준편차|stddev|σ|rep\s*별|편차')
    reps = ln(r'\d+\s*회|min-of-|median-of-|warm-?up|\d+\s*runs?|\d+\s*반복|반복\s*\d+')
    def ln_unless(pat, deferral):   # 같은 줄에 유보 표현("별도 실행합니다", "예정")이 있으면 근거로 치지 않는다 (.52 2026-09-17, PR#7900 본문)
        for i, l in enumerate(lines, 1):
            if re.search(pat, l, re.I) and not re.search(deferral, l): return i
        return 0
    ctp = ln_unless(r'\bCTP\b|/run all|test_sql|test_medium|test_shell|sql\s*/\s*medium|regression suite|회귀 테스트|회귀 검증', r'별도\s*실행|예정|아직|미실행|추후|나중에|돌릴')
    WHY = {'MEAS-01': '측정 없는 성능 주장은 머지 뒤 회귀가 나도 기준선이 없어 원인을 되짚을 수 없고, 리뷰어가 코드만 읽고 "빨라 보인다"에 동의하는 것은 근거가 아니다.',
           'MEAS-04': '1회 실행값은 캐시·호스트 부하 같은 기준선 오염과 개선을 구분할 수 없어, 개선이 잡음일 가능성을 배제하지 못한다.',
           'MEAS-05': '정확성이 성능보다 먼저다 — 회귀 테스트 근거가 없으면 빨라진 코드가 틀린 답을 내는지 아무도 확인하지 않은 상태로 머지된다.',
           'MEAS-07': '산포 없이는 0.9 배 개선과 1.1 배 회귀가 같은 잡음 폭 안일 수 있어, 표의 방향 자체를 믿을 수 없다.'}
    PROP = {'MEAS-01': '본문에 측정 표를 추가: 워크로드·계약(버퍼/병렬도/핀)·warmup·반복 수·median·MAD. 예: `| q1 | before 32.2s (MAD 0.3) | after 15.3s (MAD 0.2) | 0.47 |`',
            'MEAS-04': '반복 실행(최소 3회, median-of-N)으로 다시 재고 표에 N 을 적는다. 예: "warmup 1 + 5회 중앙값".',
            'MEAS-05': '`/run all` 결과 또는 CTP sql/medium 실행 결과(코어 0, NOK 분류)를 본문 Verification 에 한 줄로. 예: "CTP sql 통과, medium NOK 2건은 기존 답안 차이".',
            'MEAS-07': '표에 MAD(또는 표준편차/rep 별 값) 열을 추가한다. 예: `| q7 | 7.04 (MAD 0.05) | 7.75 (MAD 0.07) | 1.10 |` — 산포가 있어야 1.10 이 잡음 밖임이 보인다.'}
    def F(fid, rule, claim, ev):
        return {'id': fid, 'layer': '설계', 'file': anchor_file, 'line': anchor_line, 'claim': claim, 'why': WHY[rule], 'proposal': PROP[rule], 'category': '측정 요청', 'importance': '중간', 'evidence': [f'pr-body:{ev or 1}'], 'rule_ids': [rule], 'severity': 'non-blocking', 'auto': True}
    if perf_claim and not table: out.append(F('MEAS-01-auto', 'MEAS-01', '성능 주장은 있는데 측정 표(타이밍/프로파일)가 본문에 없다 — 측정 먼저', perf_claim))
    if median and not disp: out.append(F('MEAS-07-auto', 'MEAS-07', '중앙값만 있고 산포(MAD/표준편차/rep 별 값)가 없다 — 개선폭이 잡음 범위 안인지 판정 불가', median))
    if table and not reps: out.append(F('MEAS-04-auto', 'MEAS-04', '측정 표에 반복 횟수(min-of-N/median-of-N/warmup) 언급이 없다 — 1회 실행값이면 기준선 오염을 구분 못 한다', table))
    if perf_claim and not ctp: out.append(F('MEAS-05-auto', 'MEAS-05', '성능 변경인데 기능 회귀 테스트(CTP) 통과 근거가 본문에 없다', perf_claim))
    return out
