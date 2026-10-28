#!/usr/bin/env python3
"""리뷰 스레드 수집 — PR 의 미해결 스레드를 뽑아 '대응 필요'(리뷰어가 마지막) / 'resolve 대기'(작성자가 마지막) 로 가른다.
사용: python3 -m tools.harness.review_threads --pr 7658 [--repo CUBRID/cubrid] [--out <dir>] [--author <login>] [--repo-dir ~/dev/sources/cubrid]
산출: <out>/threads.json (구조), <out>/threads.md (읽기용). implement_pack --pr N 이 threads.json 을 후보 file:line 으로 쓴다.
2026-09-30: 3차 리뷰 25건을 GraphQL 로 손으로 가르던 일을 도구로.

지적 시점 커밋(H-30): 스레드마다 첫 코멘트의 originalCommit(리뷰어가 본 커밋)을 `original_commit` 으로 적고, 지금 head 와 다르면
`stale: true` — threads.md 머리줄에 `⚠ 지적 시점 <커밋> ≠ head` 로 보인다. 엔진 리포(--repo-dir)에 두 커밋이 다 있으면 그 사이에
스레드의 파일이 바뀌었는지(`path_changed_since`)·커밋 수(`commits_since`)도 적는다(없으면 null — 이 도구는 fetch 하지 않는다).
이미 고친 지적을 다시 고친 일이 두 번 있었다(2026-09-30 PR#7957, 2026-10-22 PR#8196) — 답하기 전에 이 표시부터 본다(review-response §1).
사이드이펙트: 읽기만 한다(gh api graphql 질의, git 읽기). 산출 파일 두 개만 쓴다."""
import argparse, json, os, subprocess, sys, time

# reviewThreads is paged (100 at most per page): a PR past 100 threads (PR#8196 had 198, 2026-10-27) lost the rest,
# its newest -- the ones a reviewer had just opened -- among them
Q = '''query($o:String!,$r:String!,$n:Int!,$c:String){repository(owner:$o,name:$r){pullRequest(number:$n){headRefOid author{login}
 reviewThreads(first:100, after:$c){pageInfo{hasNextPage endCursor} nodes{id isResolved isOutdated path line originalLine comments(first:30){nodes{author{login} createdAt body url originalCommit{oid}}}}}}}}'''


def _git_ok(repo_dir, *a):
    if not repo_dir or not os.path.isdir(repo_dir):
        return None
    return subprocess.run(['git', '-C', repo_dir, *a], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, universal_newlines=True)


def since(repo_dir, orig, head, path):
    """(path_changed_since, commits_since) — 두 커밋이 로컬에 있을 때만, 아니면 (None, None)."""
    if not orig or not head or orig == head:
        return (False, 0) if orig and orig == head else (None, None)
    for c in (orig, head):
        r = _git_ok(repo_dir, 'cat-file', '-e', f'{c}^{{commit}}')
        if r is None or r.returncode:
            return None, None
    d = _git_ok(repo_dir, 'diff', '--quiet', orig, head, '--', path)
    n = _git_ok(repo_dir, 'rev-list', '--count', f'{orig}..{head}')
    changed = None if d is None or d.returncode not in (0, 1) else d.returncode == 1
    count = int(n.stdout.strip()) if n is not None and n.returncode == 0 and n.stdout.strip().isdigit() else None
    return changed, count


def classify(d, author=None, repo_dir=''):
    """GraphQL pullRequest 노드 → threads.json 의 threads 목록(수집과 분리 — 가짜 응답으로 시험할 수 있게)."""
    author = author or d['author']['login']
    head = d['headRefOid']
    threads = []
    for i, t in enumerate(d['reviewThreads']['nodes']):
        c = t['comments']['nodes']
        if not c: continue
        last = c[-1]['author']['login']
        cls = 'resolved' if t['isResolved'] else ('대응 필요' if last != author else 'resolve 대기')
        orig = ((c[0].get('originalCommit') or {}).get('oid')) or ''
        changed, count = since(repo_dir, orig, head, t['path']) if orig and orig != head and not t['isResolved'] else (None, None)
        threads.append({'idx': i, 'id': t['id'], 'path': t['path'], 'line': t['line'] or t['originalLine'], 'outdated': t['isOutdated'], 'resolved': t['isResolved'],
                        'class': cls, 'opened_by': c[0]['author']['login'], 'last_by': last, 'n_comments': len(c), 'first_at': c[0]['createdAt'][:10],
                        'last_at': c[-1]['createdAt'][:10], 'url': c[0]['url'], 'first_body': c[0]['body'], 'last_body': c[-1]['body'],
                        'original_commit': orig, 'stale': bool(orig) and orig != head, 'path_changed_since': changed, 'commits_since': count})
    return author, threads


def collect(pr: int, repo: str = 'CUBRID/cubrid', author: str = None, repo_dir: str = ''):
    o, r = repo.split('/')
    nodes, cur = [], None
    while True:
        args = ['gh', 'api', 'graphql', '-f', f'query={Q}', '-f', f'o={o}', '-f', f'r={r}', '-F', f'n={pr}'] + (['-f', f'c={cur}'] if cur else [])
        out = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        if out.returncode: sys.exit(f'gh api 실패: {out.stderr.strip()[:300]}')
        d = json.loads(out.stdout)['data']['repository']['pullRequest']
        nodes += d['reviewThreads']['nodes']
        page = d['reviewThreads']['pageInfo']
        if not page['hasNextPage']: break
        cur = page['endCursor']
    d['reviewThreads']['nodes'] = nodes
    if repo_dir == '':
        repo_dir = os.path.expanduser('~/dev/sources/cubrid')
    author, threads = classify(d, author, repo_dir)
    return {'pr': pr, 'repo': repo, 'head': d['headRefOid'], 'author': author, 'collected': time.strftime('%Y-%m-%dT%H:%M:%S'), 'threads': threads}


def stale_note(t):
    """'⚠ 지적 시점 abc1234 ≠ head …' — 지적 시점이 head 와 같거나 모르면 ''."""
    if not t.get('stale'):
        return ''
    s = f" ⚠ 지적 시점 `{t['original_commit'][:9]}` ≠ head"
    if t.get('path_changed_since') is True:
        s += f" — 그 뒤 이 파일이 바뀜({t.get('commits_since')}커밋, 이미 고쳤을 수 있다)"
    elif t.get('path_changed_since') is False:
        s += f" — 그 뒤 이 파일은 그대로({t.get('commits_since')}커밋)"
    return s


def to_md(data):
    md = [f"# PR #{data['pr']} 리뷰 스레드 (head {data['head'][:9]}, {data['collected']}, 작성자 {data['author']})", '']
    st = [t for t in data['threads'] if t.get('stale') and t['class'] != 'resolved']
    if st:
        md += [f"> ⚠ 미해결 {len(st)}건이 지금 head 와 다른 커밋을 보고 단 지적이다 — 답하기 전에 항목마다 \"아직 있나 / 어느 커밋이 바꿨나\" 를 대조한다"
               f"(review-response §1).", '']
    for cls in ('대응 필요', 'resolve 대기', 'resolved'):
        ts = [t for t in data['threads'] if t['class'] == cls]
        md.append(f'## {cls} — {len(ts)}건'); md.append('')
        for t in ts:
            md.append(f"### T{t['idx']} {t['path']}:{t['line']} — {t['opened_by']} {t['first_at']}, 마지막 {t['last_by']} {t['last_at']}, 댓글 {t['n_comments']}{' (outdated)' if t['outdated'] else ''}{stale_note(t) if cls != 'resolved' else ''}")
            md.append(t['url']); md.append('')
            body = t['first_body'] if cls == '대응 필요' or cls == 'resolved' else t['last_body']
            md += ['> ' + l for l in body.strip().splitlines()[:40]]; md.append('')
    return '\n'.join(md)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default='CUBRID/cubrid'); ap.add_argument('--author', default=None)
    ap.add_argument('--out', default='')
    ap.add_argument('--repo-dir', default='', help='지적 시점 커밋과 head 사이 파일 변경을 볼 엔진 리포(기본 ~/dev/sources/cubrid, 커밋이 없으면 건너뜀)')
    a = ap.parse_args()
    out = a.out or os.path.expanduser(f'~/dev/utils/harness-out/threads/{a.pr}'); os.makedirs(out, exist_ok=True)
    data = collect(a.pr, a.repo, a.author, a.repo_dir)
    json.dump(data, open(os.path.join(out, 'threads.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(out, 'threads.md'), 'w', encoding='utf-8').write(to_md(data))
    n = {c: sum(1 for t in data['threads'] if t['class'] == c) for c in ('대응 필요', 'resolve 대기', 'resolved')}
    ns = sum(1 for t in data['threads'] if t['stale'] and t['class'] != 'resolved')
    print(f"[threads] PR #{a.pr} head {data['head'][:9]}: 대응 필요 {n['대응 필요']} · resolve 대기 {n['resolve 대기']} · resolved {n['resolved']}"
          + (f" · ⚠ 지적 시점 ≠ head {ns}" if ns else '') + f" -> {out}")
    for t in data['threads']:
        if t['class'] == '대응 필요': print(f"  T{t['idx']} {t['path']}:{t['line']} {t['opened_by']} {t['first_at']}{stale_note(t)}: {t['first_body'].strip().splitlines()[0][:100]}")

if __name__ == '__main__': main()
