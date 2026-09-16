#!/usr/bin/env python3
"""실패한 shell TC 가 이 PR(변경 파일)과 관련 있을 가능성 판정.
사용: tc_relevance.py (--pr <n> | --files a.c b.c ... | --diff <base>..<head>) --failed <case path 또는 이름> [...]
      [--index tests/shell-tc-index/index.json] [--engine ~/dev/sources/cubrid]
출력: 케이스마다 높음/중간/낮음 + 이유(겹친 태그, 이슈 키 일치, 경로 힌트). 근거가 없으면 '낮음(근거 없음)'.
"""
import argparse, json, os, re, subprocess, sys
def changed_files(a):
    if a.files: return a.files
    if a.pr:
        out = subprocess.run(['gh','pr','view',str(a.pr),'-R','CUBRID/cubrid','--json','files,title,body'],stdout=subprocess.PIPE,universal_newlines=True).stdout
        d = json.loads(out or '{}'); a.title = d.get('title','') + ' ' + (d.get('body') or '')
        return [f['path'] for f in d.get('files',[])]
    if a.diff: return subprocess.run(['git','-C',a.engine,'diff','--name-only',a.diff],stdout=subprocess.PIPE,universal_newlines=True).stdout.split()
    return []
def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--pr', type=int); ap.add_argument('--files', nargs='*'); ap.add_argument('--diff'); ap.add_argument('--failed', nargs='+', required=True)
    ap.add_argument('--index', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),'tests','shell-tc-index','index.json')); ap.add_argument('--engine', default=os.path.expanduser('~/dev/sources/cubrid'))
    a = ap.parse_args(); a.title = ''
    idx = json.load(open(a.index, encoding='utf-8')); files = changed_files(a)
    pr_tags, why = set(), {}
    for f in files:
        for rx, tags in idx['module_tags'].items():
            if re.search(rx, f): pr_tags.update(tags); [why.setdefault(t, f) for t in tags]
    pr_issues = set(m.upper() for m in re.findall(r'CBRD-\d+', a.title + ' ' + ' '.join(files), re.I))
    STOP = {'src','cubrid','class','catalog','network','statement','execute','object','query','storage','transaction','manager','server','client','common','system','parameter','header','include','cpp','hpp'}
    base_words = set(w for f in files for w in re.split(r'[/_.\-]', f.lower()) if len(w) > 5 and w not in STOP)
    print(f"변경 파일 {len(files)}개 → PR 태그 {sorted(pr_tags)}  이슈 {sorted(pr_issues) or '-'}\n")
    for want in a.failed:
        key = want.strip('/').split('/cases/')[0]
        cs = [c for c in idx['cases'] if c['path'].endswith(key) or key in c['path'] or os.path.basename(key) in c['path']]
        if not cs: print(f"?  {want}: shell 인덱스에 없음 — sql/medium(CTP) 케이스이거나 새 케이스(tc_index_build.py 재생성). shell 이 아니면 이 도구의 판정 대상이 아니다"); continue
        c = cs[0]; ov = sorted(set(c['tags']) & pr_tags); iss = sorted(set(c['issues']) & pr_issues)
        hint = sorted(w for w in base_words if w in c['path'].lower() or w in ' '.join(c['tags']))
        if iss or (len(ov) >= 2) : lvl = '높음'
        elif ov or hint: lvl = '중간'
        else: lvl = '낮음' + ('' if c['tags'] else '(근거 없음 — 케이스에 태그가 없다)')
        reasons = ([f"이슈 키 일치 {iss}"] if iss else []) + ([f"겹친 태그 {ov} (PR 쪽 근거: {', '.join(sorted({why[t] for t in ov}))})"] if ov else []) + ([f"경로 힌트 {hint}"] if hint else [])
        print(f"{lvl:<4} {c['path']}\n     케이스 태그 {c['tags']} · 이슈 {c['issues'] or '-'} · {c['comment']}\n     이유: {'; '.join(reasons) or 'PR 태그와 겹치는 것이 없음'}\n")
if __name__ == '__main__': main()
