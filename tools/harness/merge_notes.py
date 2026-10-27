#!/usr/bin/env python3
"""merge_notes.py — 그룹별 function_notes.<g>.json 을 OUT/function_notes.json 하나로 합치고, 지적(findings)을 함수에 붙인다.

  사용: python3 -m tools.harness.merge_notes --out <OUT> [--findings <OUT>/findings.json]

판정 에이전트가 변경 지도를 쓰지 않고 harness-notes 가 병렬로 쓰는 A안(사용자 결정 2026-10-27)에서 harness-synthesizer 단계 2-1 을
기계로 한다: 같은 (file, function, line) 은 하나로, 각 지적은 같은 파일에서 지적 줄 이하로 시작하는 가장 가까운 함수에 붙여
`finding_ids` 에 넣고 그 함수의 `risk` 를 `defect` 로 올린다(withheld 지적은 붙이기만 하고 risk 는 올리지 않는다).
결과는 report 의 §변경 지도와 annotate(한국어 주석 리뷰 PR)가 읽는다.
"""
import argparse
import glob
import json
import os
import re


def first_line(v):
    """줄 번호 — 노트 에이전트가 '6622-6658' 같은 범위나 문자열로 쓰기도 해서 첫 정수를 쓴다."""
    m = re.search(r'\d+', str(v or ''))
    return int(m.group()) if m else 0


def load_list(path, key):
    raw = json.load(open(path, encoding='utf-8'))
    return raw[key] if isinstance(raw, dict) and key in raw else raw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, help='하네스 산출 디렉터리(OUT)')
    ap.add_argument('--findings', default='', help='지적 파일(기본 OUT/findings.json)')
    a = ap.parse_args()

    notes, seen, groups = [], set(), sorted(glob.glob(os.path.join(a.out, 'function_notes.*.json')))
    for p in groups:
        for n in load_list(p, 'function_notes'):
            k = (n.get('file'), n.get('function'), first_line(n.get('line')))
            if k in seen:
                continue
            seen.add(k)
            n.setdefault('finding_ids', [])
            notes.append(n)

    fpath = a.findings or os.path.join(a.out, 'findings.json')
    attached, unplaced = 0, []
    if os.path.isfile(fpath):
        by_file = {}
        for n in notes:
            by_file.setdefault(n.get('file'), []).append(n)
        for f in load_list(fpath, 'findings'):
            line = first_line(f.get('line'))
            cands = [n for n in by_file.get(f.get('file'), []) if first_line(n.get('line')) <= line]
            if not cands:
                unplaced.append(f.get('id'))
                continue
            n = max(cands, key=lambda x: first_line(x.get('line')))
            if f.get('id') not in n['finding_ids']:
                n['finding_ids'].append(f.get('id'))
                attached += 1
            if f.get('status') != 'withheld':
                n['risk'] = 'defect'

    out = os.path.join(a.out, 'function_notes.json')
    json.dump(notes, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print(f'function_notes {len(notes)} (그룹 파일 {len(groups)}개) · 지적 붙임 {attached} · 함수를 못 찾은 지적 {unplaced} -> {out}')


if __name__ == '__main__':
    main()
