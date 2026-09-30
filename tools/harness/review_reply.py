#!/usr/bin/env python3
"""리뷰 답글 게시 — 답글 초안(replies.md 의 `## T<idx> — …` 절, `> ` 인용 본문)을 review_threads 의 스레드에 붙인다.
기본은 **dry-run**(무엇을 어디에 붙일지 표로), `--post` 는 사용자 승인 뒤에만(review-response 규약: 사람이 연 스레드는 resolve 하지 않는다).
사용: python3 -m tools.harness.review_reply --pr 7658 --draft <replies.md> [--threads <threads.json>] [--post] [--only T26,T27]"""
import argparse, json, os, re, subprocess, sys
from . import review_threads as RT

def parse_draft(path):
    txt = open(path, encoding='utf-8').read(); out = {}
    for m in re.finditer(r'^## (T\d+) —[^\n]*\n(.*?)(?=^## |\Z)', txt, re.S | re.M):
        body = '\n'.join(l[2:] if l.startswith('> ') else l[1:] if l.startswith('>') else l for l in m.group(2).strip().splitlines() if l.startswith('>') or not l.strip())
        out[m.group(1)] = body.strip()
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--draft', required=True); ap.add_argument('--threads', default=''); ap.add_argument('--only', default='')
    ap.add_argument('--post', action='store_true', help='실제 게시 (사용자 승인 뒤에만)')
    a = ap.parse_args()
    data = json.load(open(a.threads, encoding='utf-8')) if a.threads else RT.collect(a.pr, a.repo)
    byidx = {f"T{t['idx']}": t for t in data['threads']}
    drafts = parse_draft(a.draft); only = {x.strip() for x in a.only.split(',') if x.strip()}
    print(f"# PR #{a.pr} 답글 {'게시' if a.post else 'dry-run'} — 초안 {len(drafts)}건, 스레드 {len(byidx)}건 (head {data['head'][:9]})"); print()
    print('| 스레드 | 파일:줄 | 상태 | 답글 첫 줄 | 글자 |'); print('|---|---|---|---|---:|')
    posted = 0
    for tid, body in drafts.items():
        if only and tid not in only: continue
        t = byidx.get(tid)
        if not t: print(f'| {tid} | — | ⚠ 스레드 없음(번호가 바뀌었나?) | | |'); continue
        st = t['class'] + (' · resolved' if t['resolved'] else '')
        print(f"| {tid} | {t['path']}:{t['line']} | {st} | {body.splitlines()[0][:70] if body else '(비어 있음)'} | {len(body)} |")
        if a.post and body:
            q = 'mutation($id:ID!,$b:String!){addPullRequestReviewThreadReply(input:{pullRequestReviewThreadId:$id, body:$b}){comment{url}}}'
            r = subprocess.run(['gh', 'api', 'graphql', '-f', f'query={q}', '-f', f"id={t['id']}", '-f', f'b={body}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            if r.returncode: print(f'  ⚠ {tid} 게시 실패: {r.stderr.strip()[:200]}')
            else: posted += 1; print(f"  ✓ {tid} -> {json.loads(r.stdout)['data']['addPullRequestReviewThreadReply']['comment']['url']}")
    if a.post: print(f'\n게시 {posted}건. 사람이 연 스레드는 resolve 하지 않았다.')
    else: print('\n(dry-run) 게시하려면 사용자 승인 뒤 --post')

if __name__ == '__main__': main()
