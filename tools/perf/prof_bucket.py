#!/usr/bin/env python3
"""perf 자기 시간을 엔진 비용 축으로 묶는다 — structure.json 의 time_map 을 만든다.

사용: prof_bucket.py <perf.data | perf report --stdio 의 flat 출력> [...] [--json]
축(먼저 맞는 것): 페이지 버퍼(fix·래치·LRU) / B-tree / 힙 레코드(fetch·MVCC·복사) / 술어·값(eval·DB_VALUE) /
리스트·튜플(qfile) / 집계·정렬·해시 / 메모리 할당 / 기타.
CBRD-27361(2026-10-18): 조인 질의의 85%가 앞의 셋이라는 걸 이걸로 처음 봤다 — 식 batch 가 조인을 못 줄이는 이유.
"""
import json, os, re, subprocess, sys

BUCKETS = [
    ('page_buffer', '페이지 버퍼 fix/unfix·래치·LRU', r'^(pgbuf_|__pthread_mutex|pthread_|lf_|\[k\]|0xffff)'),
    ('btree', 'B-tree 탐색', r'^(btree_|mr_index_|bt_)'),
    ('heap', '힙 레코드 읽기(fetch·MVCC·복사)', r'^(spage_|heap_|or_mvcc|__memmove|__memcpy|mvcc_|logtb_is_interrupted|or_get_)'),
    ('eval', '술어·값 처리(eval·fetch·DB_VALUE)', r'^(expr_|eb_|eval_|fetch_|pr_|tp_|db_|numeric_|float_|julian|mr_|lang_|qstr_|intl_|scan_)'),
    ('list', '리스트·튜플(qfile)', r'^(qfile_|qdata_generate|qdata_copy|qexec_end_one|qexec_next)'),
    ('agg_sort_hash', '집계·정렬·해시', r'^(qexec_hash|qexec_gby|qdata_|mht_|sort_|qexec_groupby|qexec_sort|hjoin)'),
    ('alloc', '메모리 할당', r'^(malloc|free|_int_|cfree|db_private|tc_|je_|operator new|operator delete|realloc|calloc)'),
]

def flat_lines(path):
    if path.endswith('.data'):
        perf = os.path.expanduser('~/tools/bin/perf')
        env = dict(os.environ, LD_LIBRARY_PATH=os.path.expanduser('~/tools/lib'))
        out = subprocess.run([perf, 'report', '-i', path, '--no-children', '--sort', 'sym', '--stdio', '-g', 'none'],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True, env=env).stdout
        return out.splitlines()
    return open(path).read().splitlines()

def bucketize(lines):
    tot = {k: 0.0 for k, _, _ in BUCKETS}; tot['other'] = 0.0; top = []
    for l in lines:
        m = re.match(r'\s+([0-9.]+)%\s+\[([.k])\]\s+(\S+)', l)
        if not m:
            continue
        p, kind, sym = float(m.group(1)), m.group(2), m.group(3)
        if kind == 'k':
            sym = '[k]'
        top.append((p, sym))
        for k, _, r in BUCKETS:
            if re.search(r, sym):
                tot[k] += p; break
        else:
            tot['other'] += p
    return tot, sorted(top, reverse=True)[:8]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    as_json = '--json' in sys.argv
    res = {}
    for a in args:
        tot, top = bucketize(flat_lines(a))
        res[a] = {'buckets': {k: round(v, 1) for k, v in tot.items()}, 'top': [[s, p] for p, s in top]}
        if not as_json:
            label = {k: l for k, l, _ in BUCKETS}; label['other'] = '기타'
            print(f'== {a}')
            print('   ' + ' | '.join(f'{label[k]} {v:.0f}%' for k, v in tot.items()))
            print('   top: ' + ', '.join(f'{s} {p:.1f}%' for p, s in top[:5]))
    if as_json:
        print(json.dumps(res, ensure_ascii=False, indent=1))

if __name__ == '__main__':
    main()
