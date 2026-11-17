#!/usr/bin/env python3
"""merge_check.py — 종합 단계가 지적을 합칠 때 반증 판단이 엇갈렸는데 한쪽으로 조용히 합쳤는지 본다(H-63 ③, 자가진화 #142).

  사용: python3 -m tools.harness.merge_check --out <OUT> [--findings <OUT>/findings.json] [--write]

2026-11-17 .51 평가 기준점: synthesizer 가 PR#8497 G1-01(px_scan.cpp:496, 반증 confirm 🟡 — 병렬 reset 도달 경로를
query_executor.c:11868 하나로 봄)을 G2-01(반증 confirm 🔴 — 안쪽 NL 스캔도 경로)에 합치면서 needs_judgment 를 0건으로 냈다.
세 PR 모두 0건 — 정의에 "엇갈리면 올린다" 를 적어도 LLM 이 지키지 않는다. 그래서 기계로 본다.

  1. 최종 findings 의 `merged_from`(합친 원래 id 목록)이 있으면 그 id 들의 refute 판단과 최종 id 의 것을 비교한다.
  2. 그룹 findings.<g>.json 에 있었는데 최종에 없고, `merged_from` 어디에도 없고, refute 가 drop 이 아닌 지적은 '말없이 사라짐'.
     이름(식별자) 겹침이 SIMILAR 이상인 최종 지적이 있으면 그쪽에 합친 것으로 보고 1 과 같이 비교한다(merged_from 을 안 쓴 합침).
  3. 비교: verdict 또는 importance_after 가 다르면 최종 지적에 needs_judgment·judgment_basis(양쪽 판단 요약)를 단다.

check(out, findings) -> (findings, report)   report = {'conflicts': [...], 'silent': [...], 'implicit': [...]}
"""
import argparse
import glob
import json
import os
import re

SIMILAR = 0.35   # 식별자 Jaccard — 평가 기준점 PR#8497 에서 G1-01→G2-01 0.42, G8-01→G4-01 0.56, 다른 지적과는 0.29 이하


def _load(path, key):
    raw = json.load(open(path, encoding='utf-8'))
    return raw[key] if isinstance(raw, dict) and key in raw else raw


def _toks(f):
    t = ' '.join(str(f.get(k) or '') for k in ('claim', 'why', 'proposal'))
    return set(re.findall(r'[A-Za-z_][A-Za-z0-9_]{3,}', t))


def _jac(a, b):
    return len(a & b) / len(a | b) if a | b else 0.0


def _brief(rid, r):
    return f"{rid}: {r.get('verdict')} {r.get('importance_after', '')} — {(r.get('reason') or '')[:160]}"


def check(out, findings):
    refutes = {}
    for p in sorted(glob.glob(os.path.join(out, 'refute.*.json'))):
        for r in _load(p, 'refutes'):
            if isinstance(r, dict) and r.get('id'): refutes[r['id']] = r
    groups = [f for p in sorted(glob.glob(os.path.join(out, 'findings.G*.json'))) for f in _load(p, 'findings') if isinstance(f, dict)]
    final_ids = {f.get('id') for f in findings}
    merged = {m for f in findings for m in (f.get('merged_from') or [])}
    rep = {'conflicts': [], 'silent': [], 'implicit': []}
    sources = {f.get('id'): list(f.get('merged_from') or []) for f in findings}
    for g in groups:
        gid = g.get('id')
        if gid in final_ids or gid in merged: continue
        if (refutes.get(gid) or {}).get('verdict') == 'drop': continue
        best = max(((_jac(_toks(g), _toks(f)), f.get('id')) for f in findings), default=(0.0, None))
        if best[0] >= SIMILAR:
            sources[best[1]].append(gid)
            rep['implicit'].append({'gone': gid, 'into': best[1], 'similarity': round(best[0], 2)})
        else:
            rep['silent'].append({'gone': gid, 'file': g.get('file'), 'line': g.get('line'), 'refute': (refutes.get(gid) or {}).get('verdict')})
    for f in findings:
        ids = [f.get('id')] + sources.get(f.get('id'), [])
        rs = [(i, refutes[i]) for i in ids if i in refutes]
        if len(rs) < 2: continue
        if len({r.get('verdict') for _, r in rs}) > 1 or len({r.get('importance_after') for _, r in rs}) > 1:
            f['needs_judgment'] = True
            f['judgment_basis'] = list(f.get('judgment_basis') or []) + ['합친 지적의 반증 판단이 엇갈린다(H-63 ③) — ' + ' | '.join(_brief(i, r) for i, r in rs)]
            rep['conflicts'].append({'id': f.get('id'), 'from': ids[1:], 'refutes': {i: [r.get('verdict'), r.get('importance_after')] for i, r in rs}})
    return findings, rep


def to_md(rep):
    md = []
    for c in rep['conflicts']:
        md.append(f"- ⚠ needs_judgment `{c['id']}` ← {c['from']}: 반증 판단 엇갈림 " + ', '.join(f'{i} {v} {imp}' for i, (v, imp) in c['refutes'].items()))
    for s in rep['implicit']:
        md.append(f"- · `{s['gone']}` 이 merged_from 없이 `{s['into']}` 에 합쳐진 것으로 봄(식별자 겹침 {s['similarity']})")
    for s in rep['silent']:
        md.append(f"- ⚠ 말없이 사라진 지적 `{s['gone']}` {s['file']}:{s['line']} (반증 {s['refute']}) — 합쳤으면 merged_from, 뺐으면 이유를 적는다")
    return '\n'.join(md)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--findings', default='')
    ap.add_argument('--write', action='store_true', help='needs_judgment 를 단 findings 를 같은 파일에 다시 쓴다')
    a = ap.parse_args()
    fp = a.findings or os.path.join(a.out, 'findings.json')
    raw = json.load(open(fp, encoding='utf-8'))
    fs = raw['findings'] if isinstance(raw, dict) and 'findings' in raw else raw
    fs, rep = check(a.out, fs)
    print(to_md(rep) or '합친 지적의 반증 엇갈림·말없이 사라진 지적 없음')
    if a.write:
        if isinstance(raw, dict) and 'findings' in raw: raw['findings'] = fs
        else: raw = fs
        json.dump(raw, open(fp, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)


if __name__ == '__main__':
    main()
