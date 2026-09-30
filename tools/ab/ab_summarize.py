#!/usr/bin/env python3
"""ab_time.sh 로그 → 질의별 median±MAD, NEW/REF. 사용: ab_summarize.py <log>  (ERR 샘플은 세고 표에 표시)"""
import re, sys, statistics as st
rows = {}; errs = {}
for l in open(sys.argv[1]):
    m = re.match(r'(\S+) rep=\d+ REF=(\S+) NEW=(\S+)', l)
    if not m: continue
    q, a, b = m.groups()
    if a == 'ERR' or b == 'ERR': errs[q] = errs.get(q, 0) + 1; continue
    rows.setdefault(q, []).append((float(a), float(b)))
def mad(v): m = st.median(v); return st.median([abs(x - m) for x in v])
print('| 질의 | n | REF median (±MAD) | NEW median (±MAD) | NEW/REF |'); print('|---|---:|---:|---:|---:|')
for q, v in rows.items():
    A = [x for x, _ in v]; B = [y for _, y in v]
    print(f'| {q} | {len(v)} | {st.median(A):.3f} (±{mad(A):.3f}) | {st.median(B):.3f} (±{mad(B):.3f}) | {st.median(B)/st.median(A):.2f} |')
for q, n in errs.items(): print(f'| {q} | — | ERR ×{n} (csql 오류 — 로그 확인) | | |')
