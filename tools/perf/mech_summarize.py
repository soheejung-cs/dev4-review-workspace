#!/usr/bin/env python3
"""mech_bench.sh 라운드들을 요약한다: 모양·모드별 median±MAD, NEW/REF, 판정, 출력 대조, 라운드 일관성.

사용: mech_summarize.py --ref <dir>[,<dir>] --new <dir>[,<dir>] [--shapes shapes.txt] [--md]
- 판정: |차| > 2×MAD 이고 3% 넘을 때만 빨라짐/느려짐.
- 출력 대조: 첫 결과 블록의 행을 **정렬해** 비교한다 — 병렬 gather 는 실행마다 행 순서가 달라진다(CBRD-27361 F14).
- 라운드 일관성: 같은 팔의 라운드끼리 median 비가 3% 넘게 다르면 표시한다 — 다른 컨테이너 부하가 라운드 끝을 탄 흔적
  (CBRD-27361: develop 한 라운드 8%). 그때는 **가장 불리한 짝**(NEW 느린 라운드 ÷ REF 빠른 라운드)으로도 판정이 서는지 본다.
"""
import argparse, math, os, re, statistics as st

def load(d):
    out = {}
    for l in open(os.path.join(d, 'times.tsv')):
        c = l.rstrip('\n').split('\t')
        if len(c) >= 3:
            v = [float(x) for x in re.findall(r'\d+\.\d+', c[2])]
            if v:
                out[(c[0], c[1])] = v
    return out

def rows(d, i, m):
    p = os.path.join(d, f'{i}.{m}.out')
    if not os.path.exists(p):
        return None
    s = re.sub(r'\([0-9.]+ sec\)', '', open(p, errors='replace').read())
    b = re.split(r'=== <Result of SELECT Command in Line \d+> ===', s)
    return sorted(b[1].strip().splitlines()) if len(b) > 1 else None

def ms(v):
    m = st.median(v); return m, st.median([abs(x - m) for x in v])

def gmean(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else float('nan')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', required=True); ap.add_argument('--new', required=True)
    ap.add_argument('--shapes', default=''); ap.add_argument('--md', action='store_true')
    a = ap.parse_args()
    refs, news = a.ref.split(','), a.new.split(',')
    R = [load(d) for d in refs]; N = [load(d) for d in news]
    label = {}
    if a.shapes:
        for l in open(a.shapes):
            if '|' in l:
                n = l.split('|')[0]; label[n.split(' ')[0]] = n.split(' ', 1)[1] if ' ' in n else ''
    keys = sorted(set(R[0]) & set(N[0]))
    ratios = {'ser': [], 'par': []}; worst = {'ser': [], 'par': []}; warn = []
    print('| 모양 | 모드 | REF (s) | NEW (s) | NEW÷REF | 판정 | 출력 |' if a.md else '')
    if a.md:
        print('|---|---|---|---|---|---|---|')
    for k in keys:
        rv = sum((x[k][1:] for x in R if k in x), []); nv = sum((x[k][1:] for x in N if k in x), [])   # 첫 값은 예열
        (r, rm), (n, nm) = ms(rv), ms(nv)
        q = n / r; v = '차이 없음'
        if abs(n - r) > 2 * max(rm, nm) and abs(q - 1) > 0.03:
            v = '빨라짐' if q < 1 else '느려짐'
        ratios[k[1]].append(q)
        rr = [st.median(x[k][1:]) for x in R if k in x]; nr = [st.median(x[k][1:]) for x in N if k in x]
        worst[k[1]].append(max(nr) / min(rr))
        for side, ms_ in (('REF', rr), ('NEW', nr)):
            if len(ms_) > 1 and max(ms_) / min(ms_) > 1.03:
                warn.append(f'{k[0]} {k[1]} {side} 라운드 간 {max(ms_) / min(ms_):.2f}배')
        a0, b0 = rows(refs[0], *k), rows(news[0], *k)
        out = '?' if a0 is None or b0 is None else ('동일' if a0 == b0 else '다름')
        name = f"{k[0]} {label.get(k[0], '')}".strip()
        if a.md:
            print(f'| {name} | {k[1]} | {r:.2f} ±{rm:.2f} | {n:.2f} ±{nm:.2f} | {q:.2f} | {v} | {out} |')
        else:
            print(f'{name:40} {k[1]}  REF {r:7.2f}±{rm:.2f}  NEW {n:7.2f}±{nm:.2f}  {q:.2f}  {v}  출력 {out}')
    for m in ('ser', 'par'):
        if ratios[m]:
            print(f'{m}: 기하평균 NEW/REF {gmean(ratios[m]):.2f} (가장 불리한 짝 {gmean(worst[m]):.2f}, 최악 {max(worst[m]):.2f})')
    for w in warn:
        print('라운드 불일치:', w)

if __name__ == '__main__':
    main()
