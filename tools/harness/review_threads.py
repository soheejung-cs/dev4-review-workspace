#!/usr/bin/env python3
"""리뷰 스레드 수집 — PR 의 미해결 스레드를 뽑아 '대응 필요'(리뷰어가 마지막) / 'resolve 대기'(작성자가 마지막) 로 가른다.
사용: python3 -m tools.harness.review_threads --pr 7658 [--repo CUBRID/cubrid] [--out <dir>] [--author <login>]
산출: <out>/threads.json (구조), <out>/threads.md (읽기용). implement_pack --pr N 이 threads.json 을 후보 file:line 으로 쓴다.
2026-09-30: 3차 리뷰 25건을 GraphQL 로 손으로 가르던 일을 도구로."""
import argparse, json, os, subprocess, sys, time

Q = '''query($o:String!,$r:String!,$n:Int!){repository(owner:$o,name:$r){pullRequest(number:$n){headRefOid author{login}
 reviewThreads(first:100){nodes{id isResolved isOutdated path line originalLine comments(first:30){nodes{author{login} createdAt body url}}}}}}}'''

def collect(pr: int, repo: str = 'CUBRID/cubrid', author: str = None):
    o, r = repo.split('/')
    out = subprocess.run(['gh', 'api', 'graphql', '-f', f'query={Q}', '-f', f'o={o}', '-f', f'r={r}', '-F', f'n={pr}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
    if out.returncode: sys.exit(f'gh api 실패: {out.stderr.strip()[:300]}')
    d = json.loads(out.stdout)['data']['repository']['pullRequest']
    author = author or d['author']['login']
    threads = []
    for i, t in enumerate(d['reviewThreads']['nodes']):
        c = t['comments']['nodes']
        if not c: continue
        last = c[-1]['author']['login']
        cls = 'resolved' if t['isResolved'] else ('대응 필요' if last != author else 'resolve 대기')
        threads.append({'idx': i, 'id': t['id'], 'path': t['path'], 'line': t['line'] or t['originalLine'], 'outdated': t['isOutdated'], 'resolved': t['isResolved'],
                        'class': cls, 'opened_by': c[0]['author']['login'], 'last_by': last, 'n_comments': len(c), 'first_at': c[0]['createdAt'][:10],
                        'last_at': c[-1]['createdAt'][:10], 'url': c[0]['url'], 'first_body': c[0]['body'], 'last_body': c[-1]['body']})
    return {'pr': pr, 'repo': repo, 'head': d['headRefOid'], 'author': author, 'collected': time.strftime('%Y-%m-%dT%H:%M:%S'), 'threads': threads}

def to_md(data):
    md = [f"# PR #{data['pr']} 리뷰 스레드 (head {data['head'][:9]}, {data['collected']}, 작성자 {data['author']})", '']
    for cls in ('대응 필요', 'resolve 대기', 'resolved'):
        ts = [t for t in data['threads'] if t['class'] == cls]
        md.append(f'## {cls} — {len(ts)}건'); md.append('')
        for t in ts:
            md.append(f"### T{t['idx']} {t['path']}:{t['line']} — {t['opened_by']} {t['first_at']}, 마지막 {t['last_by']} {t['last_at']}, 댓글 {t['n_comments']}{' (outdated)' if t['outdated'] else ''}")
            md.append(t['url']); md.append('')
            body = t['first_body'] if cls == '대응 필요' or cls == 'resolved' else t['last_body']
            md += ['> ' + l for l in body.strip().splitlines()[:40]]; md.append('')
    return '\n'.join(md)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default='CUBRID/cubrid'); ap.add_argument('--author', default=None)
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    out = a.out or os.path.expanduser(f'~/dev/utils/harness-out/threads/{a.pr}'); os.makedirs(out, exist_ok=True)
    data = collect(a.pr, a.repo, a.author)
    json.dump(data, open(os.path.join(out, 'threads.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(out, 'threads.md'), 'w', encoding='utf-8').write(to_md(data))
    n = {c: sum(1 for t in data['threads'] if t['class'] == c) for c in ('대응 필요', 'resolve 대기', 'resolved')}
    print(f"[threads] PR #{a.pr} head {data['head'][:9]}: 대응 필요 {n['대응 필요']} · resolve 대기 {n['resolve 대기']} · resolved {n['resolved']} -> {out}")
    for t in data['threads']:
        if t['class'] == '대응 필요': print(f"  T{t['idx']} {t['path']}:{t['line']} {t['opened_by']} {t['first_at']}: {t['first_body'].strip().splitlines()[0][:100]}")

if __name__ == '__main__': main()
