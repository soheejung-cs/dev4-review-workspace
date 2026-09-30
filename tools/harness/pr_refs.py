#!/usr/bin/env python3
"""PR 본문이 기대는 외부 사실을 확인한다 — 참조한 PR(#nnnn)의 상태, 언급한 커밋 해시가 이 브랜치에 있는지, JIRA 키 목록.
사용: python3 -m tools.harness.pr_refs --pr 7658 [--repo CUBRID/cubrid] [--src ~/dev/sources/cubrid] [--branch <ref>]
2026-09-30: PR#7658 이 "CBRD-27408(#7915) 규칙을 포트했다" 고 적었는데 #7915 는 미머지로 닫혀 있었다 — 본문의 참조를 한 번 훑으면 잡히는 어긋남."""
import argparse, json, os, re, subprocess, sys

def sh(*a): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True).stdout

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--src', default=os.path.expanduser('~/dev/sources/cubrid')); ap.add_argument('--branch', default='', help='커밋 존재를 볼 ref (기본: PR head)')
    a = ap.parse_args()
    meta = json.loads(sh('gh', 'pr', 'view', str(a.pr), '-R', a.repo, '--json', 'body,headRefOid,headRefName,number'))
    body = meta['body'] or ''; head = a.branch or meta['headRefOid']
    prs = sorted({int(x) for x in re.findall(r'(?<![\w/])#(\d{4,5})\b', body) if int(x) != a.pr})
    hashes = sorted({h for h in re.findall(r'`?\b([0-9a-f]{9,10})\b`?', body) if not h.isdigit()})
    jiras = sorted(set(re.findall(r'\bCBRD-\d{4,5}\b', body)))
    rows = []
    for n in prs:
        j = sh('gh', 'pr', 'view', str(n), '-R', a.repo, '--json', 'state,mergedAt,closedAt,title')
        try: d = json.loads(j)
        except Exception: rows.append((f'#{n}', '조회 실패', '')); continue
        st = d['state'] + ('' if d['state'] != 'CLOSED' else ' (미머지)')
        rows.append((f'#{n}', st, (d.get('mergedAt') or d.get('closedAt') or '')[:10] + ' ' + d['title'][:60]))
    print(f'# PR #{a.pr} 본문 참조 확인 (head {head[:9]})'); print()
    print('| 참조 PR | 상태 | 비고 |'); print('|---|---|---|')
    for r in rows: print(f'| {r[0]} | {r[1]} | {r[2]} |')
    warn = [r for r in rows if '미머지' in r[1]]
    print()
    print('| 커밋 해시 | head 에 있나 |'); print('|---|---|')
    missing_h = []
    for h in hashes:
        exists = subprocess.run(['git', '-C', a.src, 'merge-base', '--is-ancestor', h, head], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
        known = subprocess.run(['git', '-C', a.src, 'cat-file', '-e', h + '^{commit}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
        st = '있음' if exists else ('저장소엔 있지만 head 조상 아님 (리베이스/force-push?)' if known else '로컬 저장소에 없음 (fetch 필요?)')
        if not exists: missing_h.append(h)
        print(f'| `{h}` | {st} |')
    print(); print('JIRA: ' + ', '.join(jiras))
    if warn: print(); print('⚠ 본문이 기대는 PR 중 미머지로 닫힌 것: ' + ', '.join(r[0] for r in warn) + ' — 그 규칙을 이 PR 이 "포트했다" 면 develop 과 어긋난다')
    if missing_h: print('⚠ head 에 없는 커밋 해시: ' + ', '.join(missing_h))

if __name__ == '__main__': main()
