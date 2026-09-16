#!/usr/bin/env python3
"""review-to-do 보드 생성기 — roster.json 의 tracked_github 가 assignee 인 open·non-draft PR 을 추적한다.
   출력: <out_dir>/index.html (자립 HTML, charset utf-8) + <out_dir>/board.json
   사용: review_board_gen.py [out_dir]   (기본 ~/dev/utils/review-board/site)
   의존: gh CLI(인증됨). 이 컨테이너의 리뷰 문서(claude-workspace/projects/CBRD-*, dev4-review-workspace/examples|reviews)를 링크한다.
"""
import json, os, re, subprocess, sys, datetime, html
from urllib.parse import quote

HERE = os.path.dirname(os.path.abspath(__file__))
ROSTER = json.load(open(os.path.join(HERE, 'roster.json'), encoding='utf-8'))
TRACKED = ROSTER.get('tracked_github', [])
REPOS = ROSTER.get('repos', ['CUBRID/cubrid'])
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/dev/utils/review-board/site')
DOCS = [os.path.expanduser('~/dev/docs/claude-workspace/projects'),
        os.path.expanduser('~/dev/docs/dev4-review-workspace/examples'),
        os.path.expanduser('~/dev/docs/dev4-review-workspace/reviews')]
DOCS_HTTP_BASE = 'http://192.168.6.51:8827/docs/'   # site/docs -> ~/dev/docs 심링크; 컨테이너 주소로 접근 (사용자 요청 2026-09-16)
BOTS = {'greptile-apps', 'chatgpt-codex-connector', 'github-actions', 'cubrid-submodule-bot'}

def sh(*a):
    return subprocess.check_output(a, universal_newlines=True)

def list_prs(repo):
    js = sh('gh', 'pr', 'list', '--repo', repo, '--state', 'open', '--limit', '300', '--json',
            'number,title,url,isDraft,assignees,author,createdAt,updatedAt,headRefName,statusCheckRollup')
    out = []
    for p in json.loads(js):
        if p.get('isDraft'):
            continue
        ass = [a['login'] for a in p.get('assignees', [])]
        hit = [a for a in ass if a in TRACKED]
        if not hit:
            continue
        p['repo'] = repo; p['tracked'] = hit; p['assignee_logins'] = ass
        out.append(p)
    return out

def details(repo, num):
    owner, name = repo.split('/')
    q = '''{repository(owner:"%s",name:"%s"){pullRequest(number:%d){
      reviewThreads(first:100){totalCount nodes{isResolved isOutdated comments(first:1){nodes{author{login} path}}}}
      reviewRequests(first:30){nodes{requestedReviewer{... on User{login} ... on Team{name}}}}
      latestReviews(first:30){nodes{author{login} state submittedAt}}
      reviewDecision mergeable
    }}}''' % (owner, name, num)
    d = json.loads(sh('gh', 'api', 'graphql', '-f', 'query=' + q))['data']['repository']['pullRequest']
    threads = d['reviewThreads']['nodes']
    unresolved = [t for t in threads if not t['isResolved']]
    by_author = {}
    for t in unresolved:
        c = (t['comments']['nodes'] or [{}])[0]
        a = (c.get('author') or {}).get('login', '?')
        by_author[a] = by_author.get(a, 0) + 1
    req = []
    for n in d['reviewRequests']['nodes']:
        r = n.get('requestedReviewer') or {}
        req.append(r.get('login') or r.get('name') or '?')
    latest = [(n['author']['login'], n['state'], n['submittedAt'][:10]) for n in d['latestReviews']['nodes'] if n.get('author')]
    return {'threads_total': d['reviewThreads']['totalCount'], 'unresolved': len(unresolved),
            'unresolved_by': by_author, 'requested': req, 'latest_reviews': latest,
            'decision': d.get('reviewDecision') or '-', 'mergeable': d.get('mergeable') or '-'}

def ci_summary(rollup):
    if not rollup:
        return '-'
    fail = [c for c in rollup if (c.get('conclusion') or c.get('state') or '').upper() in ('FAILURE', 'ERROR', 'TIMED_OUT')]
    pend = [c for c in rollup if (c.get('conclusion') or c.get('state') or '').upper() in ('PENDING', 'IN_PROGRESS', 'QUEUED', 'EXPECTED')]
    fail = [c for c in fail if 'Check TC PRs' not in (c.get('name') or c.get('context') or '')]  # 규약: Check TC PRs 는 항상 무시
    if fail:
        return 'FAIL ' + ', '.join((c.get('name') or c.get('context') or '?') for c in fail[:3])
    if pend:
        return 'pending %d' % len(pend)
    return 'ok'

def jira_key(title):
    m = re.search(r'\b(CBRD|APIS|CUBRIDQA)-\d+', title)
    return m.group(0) if m else ''

def local_docs(key, num):
    found = []
    for base in DOCS:
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            rel = os.path.relpath(root, os.path.expanduser('~/dev/docs'))
            if key and key in root and root.count('/') - base.count('/') == 1:
                found.append(rel + '/'); dirs[:] = []; continue
            for f in files:
                if (key and key in f) or ('PR%d' % num in f) or ('pr%d' % num in f) or ('%d' % num in f and 'pr' in f.lower()):
                    found.append(os.path.join(rel, f))
    return sorted(set(found))

def tc_prs(num):
    out = []
    for repo in ('CUBRID/cubrid-testcases', 'CUBRID/cubrid-testcases-private-ex'):
        try:
            js = sh('gh', 'pr', 'list', '--repo', repo, '--state', 'open', '--search', 'cubrid#%d in:title' % num, '--json', 'number,url,isDraft')
            for p in json.loads(js):
                out.append((repo.split('/')[1], p['number'], p['url'], p['isDraft']))
        except subprocess.CalledProcessError:
            pass
    return out

def main():
    prs = []
    for repo in REPOS:
        prs += list_prs(repo)
    rows, tcs = [], []
    for p in prs:
        d = details(p['repo'], p['number'])
        key = jira_key(p['title'])
        rec = {'repo': p['repo'], 'number': p['number'], 'title': p['title'], 'url': p['url'], 'author': p['author']['login'],
               'assignees': p['assignee_logins'], 'tracked': p['tracked'], 'created': p['createdAt'][:10], 'updated': p['updatedAt'][:10],
               'jira': key, 'docs': local_docs(key, p['number']), 'linked_tc': [], **d}
        if p['repo'] == 'CUBRID/cubrid':
            rows.append(rec)
        else:
            m = re.search(r'cubrid#(\d+)', p['title'])
            rec['engine_pr'] = int(m.group(1)) if m else None
            tcs.append(rec)
    # 연동: JIRA 키가 같거나 제목이 cubrid#<n> 을 가리키는 TC PR 을 엔진 PR 하위에 붙인다 (사용자 지시 2026-09-16)
    for t in tcs:
        home = next((r for r in rows if (t['engine_pr'] and r['number'] == t['engine_pr']) or (t['jira'] and t['jira'] == r['jira'])), None)
        if home is not None:
            home['linked_tc'].append(t); t['linked'] = True
        else:
            t['linked'] = False
    orphans = [t for t in tcs if not t.get('linked')]
    rows.sort(key=lambda r: (r['tracked'][0], -r['unresolved'], r['updated']))
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M KST')
    os.makedirs(OUT, exist_ok=True)
    json.dump({'generated': now, 'tracked': TRACKED, 'prs': rows, 'unlinked_tc_prs': orphans}, open(os.path.join(OUT, 'board.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(render(rows, now, orphans))
    print('%s review board: %d PRs (tracked %d ids) -> %s/index.html' % (now, len(rows), len(TRACKED), OUT))

def esc(s):
    return html.escape(str(s))

def render(rows, now, orphans=()):
    by = {}
    for r in rows:
        by.setdefault(r['tracked'][0], []).append(r)
    tot_unres = sum(r['unresolved'] for r in rows)
    tot_wait = sum(1 for r in rows if r['requested'])
    css = """
    :root{--bg:#eef0f3;--surface:#fff;--ink:#161a20;--muted:#6b7684;--line:#d5dae1;--gate:#14706b;--warn:#a85b16;--bad:#a3262b;--soft:#f6f7f9}
    @media (prefers-color-scheme:dark){:root{--bg:#0e1218;--surface:#161b23;--ink:#e7ebf0;--muted:#8a95a3;--line:#2a323c;--gate:#46b3a8;--warn:#d98b45;--bad:#e06c70;--soft:#1b2128}}
    body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.55 "IBM Plex Sans KR",system-ui,sans-serif}
    .wrap{max-width:1180px;margin:0 auto;padding:28px 20px}
    h1{font-size:24px;margin:0 0 4px;letter-spacing:-.02em} h2{font-size:17px;margin:28px 0 10px;display:flex;gap:10px;align-items:baseline}
    .meta{color:var(--muted);font-family:"IBM Plex Mono",monospace;font-size:12px}
    .facts{display:flex;gap:26px;margin:14px 0 6px;flex-wrap:wrap}.fact b{font-family:"IBM Plex Mono",monospace;font-size:22px}.fact span{display:block;color:var(--muted);font-size:12px}
    table{border-collapse:collapse;width:100%;background:var(--surface);border:1px solid var(--line);border-radius:8px;overflow:hidden;font-size:13.5px}
    th,td{padding:8px 10px;border-bottom:1px solid var(--line);vertical-align:top;text-align:left}th{background:var(--soft);font-family:"IBM Plex Mono",monospace;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);white-space:nowrap}
    tr:last-child td{border-bottom:0} .n{font-family:"IBM Plex Mono",monospace;white-space:nowrap} .warn{color:var(--warn);font-weight:600}.bad{color:var(--bad);font-weight:600}.ok{color:var(--gate);font-weight:600}
    .pill{display:inline-block;font-family:"IBM Plex Mono",monospace;font-size:11px;padding:1px 7px;border-radius:999px;border:1px solid var(--line);margin:1px 2px 1px 0;white-space:nowrap}
    .pill.APPROVED{color:var(--gate);border-color:var(--gate)}.pill.CHANGES_REQUESTED{color:var(--bad);border-color:var(--bad)}.pill.req{color:var(--warn);border-color:var(--warn)}
    a{color:inherit} .docs a{display:block;font-size:12px;font-family:"IBM Plex Mono",monospace;color:var(--gate)} .muted{color:var(--muted)}
    """
    h = ['<!doctype html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>review-to-do</title><style>%s</style></head><body><div class="wrap">' % css]
    h.append('<h1>review-to-do</h1><div class="meta">스냅샷 %s · assignee ∈ {%s} · open · non-draft · 미해결 스레드는 리뷰 코멘트 기준 · CI 는 보지 않음(사용자 지시) · TC PR 은 JIRA 키/cubrid#n 으로 엔진 PR 하위에 연동</div>' % (now, ', '.join(TRACKED)))
    h.append('<div class="facts"><div class="fact"><b>%d</b><span>추적 PR</span></div><div class="fact"><b class="%s">%d</b><span>미해결 스레드 합</span></div><div class="fact"><b>%d</b><span>리뷰어 응답 대기 PR</span></div></div>' % (len(rows), 'warn' if tot_unres else 'ok', tot_unres, tot_wait))
    for who in TRACKED:
        rs = by.get(who, [])
        h.append('<h2>@%s <span class="meta">%d PR</span></h2>' % (esc(who), len(rs)))
        if not rs:
            h.append('<div class="muted">추적 대상 없음 (open·non-draft·assignee 기준)</div>'); continue
        h.append('<table><tr><th>PR</th><th>제목 / JIRA</th><th>미해결</th><th>리뷰어</th><th>갱신</th><th>연동된 테스트케이스</th><th>리뷰 문서(이 컨테이너)</th></tr>')
        for r in rs:
            unres = ('<span class="%s">%d</span> / %d' % ('bad' if r['unresolved'] else 'ok', r['unresolved'], r['threads_total']))
            if r['unresolved_by']:
                unres += '<div class="muted" style="font-size:11px">' + ', '.join('%s %d' % (esc(a), n) for a, n in sorted(r['unresolved_by'].items(), key=lambda x: -x[1])) + '</div>'
            rev = ''.join('<span class="pill %s">%s %s</span>' % (esc(st), esc(a), {'APPROVED': '✓', 'CHANGES_REQUESTED': '✗', 'COMMENTED': '…', 'DISMISSED': '–'}.get(st, st)) for a, st, _ in r['latest_reviews'] if a not in BOTS)
            rev += ''.join('<span class="pill req">%s 대기</span>' % esc(a) for a in r['requested'])
            rev += '<div class="muted" style="font-size:11px">decision: %s</div>' % esc(r['decision'])
            tcl = ''.join('<div><a href="%s">%s#%d</a> <span class="muted">미해결 %d/%d%s</span></div>' % (esc(t['url']), esc(t['repo'].split('/')[1].replace('cubrid-testcases','tc')), t['number'], t['unresolved'], t['threads_total'], (' · ' + ','.join(esc(a) + ' 대기' for a in t['requested'])) if t['requested'] else '') for t in r['linked_tc']) or '<span class="muted">없음</span>'
            docs = ''.join('<a href="%s%s">%s</a>' % (DOCS_HTTP_BASE, quote(d), esc(d)) for d in r['docs']) or '<span class="muted">없음</span>'
            others = [a for a in r['assignees'] if a != r['tracked'][0]]
            h.append('<tr><td class="n"><a href="%s">%s#%d</a>%s</td><td>%s<div class="meta">%s · by %s%s</div></td><td class="n">%s</td><td>%s</td><td class="n">%s<div class="muted" style="font-size:11px">생성 %s</div></td><td class="docs" style="font-size:12px">%s</td><td class="docs">%s</td></tr>' % (
                esc(r['url']), esc(r['repo'].split('/')[1]), r['number'], '<div class="muted" style="font-size:11px">%s</div>' % esc(r['tracked'][0]) if len(rs) and who != r['tracked'][0] else '',
                esc(r['title']), esc(r['jira'] or '-'), esc(r['author']), (' · assignees +' + ','.join(others)) if others else '',
                unres, rev, esc(r['updated']), esc(r['created']), tcl, docs))
        h.append('</table>')
    if orphans:
        h.append('<h2>연동 대상 없는 TC PR <span class="meta">%d</span></h2><table><tr><th>PR</th><th>제목</th><th>미해결</th><th>리뷰어 대기</th></tr>' % len(orphans))
        for t in orphans:
            h.append('<tr><td class="n"><a href="%s">%s#%d</a></td><td>%s</td><td class="n">%d/%d</td><td>%s</td></tr>' % (esc(t['url']), esc(t['repo'].split('/')[1]), t['number'], esc(t['title']), t['unresolved'], t['threads_total'], esc(','.join(t['requested'])) or '-'))
        h.append('</table>')
    h.append('<p class="meta" style="margin-top:26px">생성기 dev4-review-workspace/tools/review_board_gen.py · 갱신 규약 skills/review-board/SKILL.md · 추적 ID 는 tools/roster.json</p></div></body></html>')
    return '\n'.join(h)

if __name__ == '__main__':
    main()
