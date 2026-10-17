#!/usr/bin/env python3
"""csql `;trace on` 출력에서 스캔 줄을 뽑아 커버리지·비용 표를 만든다 — structure.json 의 coverage_map·cost_census 재료.

사용: trace_scan.py <csql 출력>... [--json]
스캔마다: 대상 · 시간(heap/btree/lookup time) · fetch(페이지 fix) · 행 수 · fix/행 · 배치 여부(`batch pages: N (row path: M…)` /
`batch: no (사유)`). batch 로 돈 스캔은 지금 heap time·fetch 를 0 으로 찍는다(CBRD-27361 남은 일) — 그 스캔의 비용은 perf 로 본다.
trace 의 시간은 **중첩**이다(바깥 스캔 시간에 안쪽이 들어간다) — 시간은 비교용으로만, 비용은 fetch/행으로 본다.
CBRD-27361: Q3 customer PK 탐색 729만 번 = fetch 2,916만(탐색당 4) 을 이걸로 셌다.
"""
import json, re, sys

PAT = re.compile(r'SCAN \((?P<kind>table|index|temp)(?:: (?P<target>[^)]*))?\), \((?P<body>[^()]*(?:\([^()]*\)[^()]*)*)\)(?: \(lookup time: (?P<lt>\d+), rows: (?P<lrows>\d+)\))?')

def kv(body):
    d = {}
    for k, v in re.findall(r'([a-z ]+): ([0-9]+)', body):
        d[k.strip()] = int(v)
    m = re.search(r'batch pages: (\d+) \(row path: (\d+)(?:, last: ([^)]*))?\)', body)
    if m:
        d['batch'] = {'pages': int(m.group(1)), 'row_path': int(m.group(2)), 'last': m.group(3)}
    m = re.search(r'batch: no \((.*)\)', body)
    if m:
        d['batch'] = {'no': m.group(1)}
    return d

def scans(text):
    out = []
    for m in PAT.finditer(text):
        d = kv(m.group('body'))
        rec = {'kind': m.group('kind'), 'target': (m.group('target') or '').strip(), **d}
        if m.group('lt'):
            rec['lookup time'] = int(m.group('lt')); rec['lookup rows'] = int(m.group('lrows'))
        # 페이지 fix 비용: 키 하나(인덱스 탐색 한 번)당, 내준 행 하나당
        if d.get('fetch') and d.get('readkeys'):
            rec['fix_per_key'] = round(d['fetch'] / d['readkeys'], 2)
        if d.get('fetch') and d.get('rows'):
            rec['fix_per_row'] = round(d['fetch'] / d['rows'], 2)
        out.append(rec)
    return out

def main():
    files = [a for a in sys.argv[1:] if not a.startswith('--')]
    res = {f: scans(open(f, errors='replace').read()) for f in files}
    if '--json' in sys.argv:
        print(json.dumps(res, ensure_ascii=False, indent=1)); return
    for f, ss in res.items():
        print(f'== {f}')
        for s in ss:
            b = s.get('batch', {})
            bs = ('batch ' + str(b['pages']) + 'p' + (f" (row {b['row_path']})" if b.get('row_path') else '')) if 'pages' in b else ('batch: no — ' + b['no'] if 'no' in b else '')
            t = s.get('heap time', s.get('btree time', s.get('temp time', '')))
            print(f"  {s['kind']:5} {s['target'][:44]:44} time {t!s:>6} fetch {s.get('fetch', ''):>9} rows {s.get('rows', ''):>9} fix/key {s.get('fix_per_key', ''):>5} fix/row {s.get('fix_per_row', ''):>5}  {bs}")

if __name__ == '__main__':
    main()
