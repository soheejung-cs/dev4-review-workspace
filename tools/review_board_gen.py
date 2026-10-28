#!/usr/bin/env python3
"""review-to-do 보드 생성기 — roster.json 의 tracked_github 가 assignee 인(없으면 작성자인) open·non-draft PR 을 추적한다.
   출력: <out_dir>/index.html (자립 HTML, charset utf-8) + <out_dir>/board.json
   사용: review_board_gen.py [out_dir]   (기본 ~/dev/utils/review-board/site)
   의존: gh CLI(인증됨). 이 컨테이너의 리뷰 문서(claude-workspace/projects/CBRD-*, dev4-review-workspace/examples|reviews)를 링크한다.
   사이드이펙트: gh 로 읽기만 하고 out_dir 의 두 파일만 쓴다. 갱신·호스팅·보드 읽는 법은 skills/review-board/SKILL.md.

UI 명세 — 이 docstring 이 정본이다(2026-10-28 스킬에서 옮김, H-40). 생성기를 고치면 여기도 고친다.

[추적 대상] (list_prs)
  assignee 에 tracked_github 가 있는 open·non-draft PR. assignee 에 추적 ID 가 없으면 작성자로도 잡고(사용자 지시 2026-10-14 —
  assignee 를 비운 PR 이 빠지던 것) 그 행에는 `작성자 기준(assignee 없음)`, board.json `tracked_via: author`. 머지된 것·draft 는
  싣지 않는다 — 기준은 JIRA 가 아니라 GitHub(사용자 지시 2026-09-16).

[PR 행] (details·agent_reviewed·local_docs·tc_prs)
  - 미해결 리뷰 스레드 수 / 전체 스레드 수, 미해결 스레드를 연 사람별 개수.
  - 리뷰어 필: 최신 리뷰 상태(✓ APPROVED · ✗ CHANGES_REQUESTED · … COMMENTED · – DISMISSED, 봇 제외)와 두 갈래의 남은 일 —
    `미착수`(요청됐고 리뷰 이력 0) / `승인 전`(리뷰했으나 APPROVED 아님). GitHub 은 리뷰를 제출하는 순간 그 사람을 reviewRequests 에서
    빼므로, 요청 목록만 보면 코멘트 한 번 남기고 승인하지 않은 리뷰어가 사라진다(2026-09-21). 재요청되면 다시 `미착수`.
    그 아래 reviewDecision.
  - CI 는 싣지 않는다(머리말 "CI 는 보지 않음(사용자 지시)") — ci_summary() 는 남아 있지만 render 가 쓰지 않는다.
  - 연동된 TC PR: JIRA 키(또는 제목의 `cubrid#<n>`)가 같은 cubrid-testcases / -private-ex PR 을 엔진 PR 하위에 — 미해결 수·미착수 리뷰어.
  - 갱신·생성일.

[머리 통계·사람별 표] (사용자 지시 2026-10-06)
  머리 통계는 '미완료 PR' 하나 — 미완료 = 머지 가능이 아닌 것 = 미해결 스레드가 남았거나 승인자가 0. `18 / 전체 21` 꼴.
  사람별 표는 사람마다 `내 PR`(assignee — 작성자가 움직일 차례) / `내가 리뷰어`(미착수·승인 전 — 내 차례) / `머지 대기`(승인 끝·미해결 0, 회색).

[툴바] (사용자 지시 2026-10-06, 클라이언트 JS — 재생성 없이 즉시 바뀐다)
  - 「사람으로 모아 보기 — 담당자 + 리뷰어」 버튼 줄: 사람을 누르면 그 사람이 리뷰어(미착수·승인 전·승인자)이거나 담당자(assignee)인
    행만 남기고 빈 섹션은 접는다. 담당자 포함은 이현욱(hyunuk.lee) 요청 2026-10-07 — 리뷰어만 걸면 자기가 담당인 PR 이 사라졌다.
    판정은 행의 `data-rev`(리뷰어 전원)·`data-pend`(미착수·승인 전)·`data-own`(담당자) 속성.
  - `승인 전·미착수만`: 아직 응답이 남은 PR 만(`data-pend` 가 비지 않은 행).
  - 오른쪽 `내 전용 열 보기` 체크: 아래 내 전용 열을 켠다. 선택은 localStorage `showAgent` 에 남는다.
  - `붉은색 = …` 문구(#redHint)가 지금의 붉은 행 기준을 말한다.

[붉은 행 = 고른 사람이 움직일 차례] (사용자 지시 2026-10-07, paint())
  왼쪽 테두리 3px + 옅은 배경 + PR 번호를 붉게(`tr.unres`). 기준은 툴바에서 고른 사람과의 관계로 바뀐다(행의 data-own/data-rev/data-appr/data-unres):
    고른 사람이 담당자(assignee)         → 머지 전이면 붉다(이 보드는 open PR 만 싣으므로 사실상 늘 붉다)
    고른 사람이 리뷰어                   → 그 사람이 아직 승인하지 않았으면 붉다(남이 승인했어도)
    아무도 안 고름(전체)                 → 중립 기준: resolve 안 한 코멘트가 남았으면 붉다
    승인 끝·미해결 0 인데 머지 안 됨     → 회색(`tr.ready`) — 사라지지 않고 남아 "머지만 남았다" 를 보인다(사람별 표의 `머지 대기`)

[내 전용 열 — 기본 숨김] (`.col-mine`, 사용자 지시 2026-10-06 "헷갈린다" + 2026-10-07 "리뷰 문서는 나만 보게")
  - `에이전트 리뷰`: roster.json agent_github 로그인(또는 본문이 agent_marker 정규식에 맞는 것)이 남긴 리뷰·인라인·이슈 코멘트 건수와
    마지막 날짜(본문 없는 빈 리뷰 껍데기는 세지 않는다). 팀 공유 계정으로 돌리면 agent_github 만 그 계정으로 바꾼다 — 지금은 사용자 본인
    계정이라 본인 코멘트도 함께 세어진다(공유 계정 전환 전 한계).
  - `리뷰 문서(내부망)`: 이 컨테이너의 DOCS 에서 JIRA 키·PR 번호로 찾은 것, DOCS_HTTP_BASE(`site/docs → ~/dev/docs` 심링크) 링크.
  - 헤드리스 리뷰 실행 표(render_headless, <out>/headless/cost.jsonl)도 같은 토글 뒤에 숨는다.

[연동 대상 없는 TC PR 절]
  엔진 PR 에 붙지 못한 TC PR. 본문 행과 같은 data-* 속성을 붙여 툴바의 사람 필터가 여기에도 걸린다(사용자 지시 2026-10-13).
"""
import sys, json, os, re, subprocess, datetime, html
from urllib.parse import quote

HERE = os.path.dirname(os.path.abspath(__file__))
ROSTER = json.load(open(os.path.join(HERE, 'roster.json'), encoding='utf-8'))
TRACKED = ROSTER.get('tracked_github', [])
AGENT = set(ROSTER.get('agent_github', []))
AGENT_MARK = re.compile(ROSTER.get('agent_marker') or r'Claude-Session:')
REPOS = ROSTER.get('repos', ['CUBRID/cubrid'])
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/dev/utils/review-board/site')
# 산출물 디렉터리는 사람마다 다르다 — 이 리포는 팀이 공유하므로 개인 경로를 하드코딩하지 않는다.
# DEV4_RECORDS_ROOT > roster.json 의 records_root > 기본값(이 리포를 만든 사람의 배치) 순.
RECORDS_ROOT = os.path.expanduser(
    os.environ.get('DEV4_RECORDS_ROOT') or ROSTER.get('records_root') or '~/dev/docs/claude-workspace')
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = [os.path.join(RECORDS_ROOT, 'projects'),
        os.path.join(HERE, 'examples'),
        os.path.join(HERE, 'reviews')]
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
        via = 'assignee'
        # assignee 에 추적 ID 가 없으면 작성자(author)로도 잡는다 — assignee 를 비워 둔 PR 이 보드에서 빠지던 것(사용자 지시 2026-10-14).
        # open·non-draft 조건은 같다. 행에는 '작성자 기준' 을 표시한다.
        if not hit and (p.get('author') or {}).get('login') in TRACKED:
            hit = [p['author']['login']]; via = 'author'
        if not hit:
            continue
        p['repo'] = repo; p['tracked'] = hit; p['assignee_logins'] = ass; p['tracked_via'] = via
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
    # GitHub 은 리뷰를 제출하는 순간 그 사람을 reviewRequests 에서 뺀다. 그래서 requested 만 보면
    # 코멘트 한 번 남기고 승인하지 않은 리뷰어가 보드에서 사라진다. 두 상태를 갈라서 둘 다 남긴다:
    #   requested        = 요청됐고 리뷰 이력 0 (미착수)
    #   pending_approval = 리뷰는 했으나 APPROVED 아님 (승인 전)
    # 재요청되면 requested 에 다시 들어오므로 그쪽을 우선한다.
    approvers = [a for a, st, _ in latest if st == 'APPROVED' and a not in BOTS]
    pending = [a for a, st, _ in latest if st != 'APPROVED' and a not in BOTS and a not in req]
    return {'threads_total': d['reviewThreads']['totalCount'], 'unresolved': len(unresolved),
            'unresolved_by': by_author, 'requested': req, 'latest_reviews': latest,
            'approvers': approvers, 'pending_approval': pending,
            'decision': d.get('reviewDecision') or '-', 'mergeable': d.get('mergeable') or '-'}

def agent_reviewed(repo, num):
    """이 에이전트(agent_github 로그인 또는 본문 마커)가 남긴 리뷰/코멘트: (건수, 마지막 날짜, 종류 집합)"""
    n, last, kinds = 0, '', set()
    for kind, ep in (('review', 'pulls/%d/reviews'), ('inline', 'pulls/%d/comments'), ('comment', 'issues/%d/comments')):
        try:
            items = json.loads(sh('gh', 'api', 'repos/%s/%s' % (repo, ep % num), '--paginate'))
        except subprocess.CalledProcessError:
            continue
        for it in items:
            login = ((it.get('user') or {}).get('login') or '')
            body = it.get('body') or ''
            if login in AGENT or AGENT_MARK.search(body):
                if kind == 'review' and not body and it.get('state') in (None, 'COMMENTED', 'PENDING'):
                    continue  # 본문 없는 빈 리뷰 껍데기는 세지 않음
                n += 1; kinds.add(kind)
                d = (it.get('submitted_at') or it.get('updated_at') or it.get('created_at') or '')[:10]
                if d > last: last = d
    return n, last, sorted(kinds)

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
               'assignees': p['assignee_logins'], 'tracked': p['tracked'], 'tracked_via': p.get('tracked_via', 'assignee'), 'created': p['createdAt'][:10], 'updated': p['updatedAt'][:10],
               'jira': key, 'docs': local_docs(key, p['number']), 'linked_tc': [], **d}
        rec['agent_n'], rec['agent_last'], rec['agent_kinds'] = agent_reviewed(p['repo'], p['number'])
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
    dump = lambda: json.dump({'generated': now, 'tracked': TRACKED, 'prs': rows, 'unlinked_tc_prs': orphans}, open(os.path.join(OUT, 'board.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    dump()
    open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(render(rows, now, orphans))
    print('%s review board: %d PRs (tracked %d ids) -> %s/index.html' % (now, len(rows), len(TRACKED), OUT))

def esc(s):
    return html.escape(str(s))

def render_headless(limit=20):
    """헤드리스 리뷰(tools/headless_review.sh) 실행 기록·비용 — <out>/headless/cost.jsonl 을 읽는다 (FinOps, 2026-10-13).
       내 전용 열(col-mine)처럼 토글 뒤에 숨긴다 — 팀원에겐 소음이다."""
    f = os.path.join(OUT, 'headless', 'cost.jsonl')
    if not os.path.exists(f):
        return ''
    rows = [json.loads(l) for l in open(f, encoding='utf-8') if l.strip()]
    if not rows:
        return ''
    tot = sum(r.get('cost_usd') or 0 for r in rows)
    h = ['<section class="col-mine" style="display:none"><h2>헤드리스 리뷰 실행 <span class="meta">%d회 · 누적 $%.2f · tools/headless_review.sh</span></h2>' % (len(rows), tot)]
    h.append('<table><tr><th>시각</th><th>PR</th><th>결과</th><th>비용</th><th>턴</th><th>시간</th><th>모델별 토큰(in/out/cache-read)</th><th>마지막 메시지</th></tr>')
    for r in reversed(rows[-limit:]):
        st = '<span class="bad">실패 rc=%s</span>' % r.get('rc') if r.get('error') or r.get('rc') else '<span class="ok">완료</span>'
        models = '<br>'.join('<span class="muted">%s</span> %s/%s/%s' % (esc(m.replace('claude-', '')), u.get('inputTokens', '-'), u.get('outputTokens', '-'), u.get('cacheReadInputTokens', '-')) for m, u in (r.get('models') or {}).items())
        h.append('<tr><td class="n">%s</td><td class="n"><a href="https://github.com/CUBRID/cubrid/pull/%d">#%d</a> <a class="muted" href="headless/%s" style="font-size:11px">json</a></td><td>%s</td><td class="n">$%.3f</td><td class="n">%s</td><td class="n">%ss</td><td style="font-size:12px">%s</td><td class="muted" style="font-size:11px">%s</td></tr>' % (
            esc(r.get('ts', '')), r['pr'], r['pr'], esc(r.get('json', '')), st, r.get('cost_usd') or 0, r.get('turns'), r.get('duration_s'), models, esc((r.get('result_tail') or '')[-160:])))
    h.append('</table></section>')
    return '\n'.join(h)

TOOLBAR_JS = """<script>
(function () {
  var body = document.body;
  // 에이전트 리뷰 열 — 기본 숨김, 체크해야 보인다. 선택은 브라우저에 기억된다.
  var chk = document.getElementById('agentToggle');
  try { if (localStorage.getItem('showAgent') === '1') { body.classList.add('show-mine'); chk.checked = true; } } catch (e) {}
  chk.addEventListener('change', function () {
    body.classList.toggle('show-mine', chk.checked);
    try { localStorage.setItem('showAgent', chk.checked ? '1' : '0'); } catch (e) {}
  });
  // 리뷰어 필터 — 그 사람이 리뷰어(미착수·승인 전·승인자)인 행만 남기고, 빈 섹션은 접는다.
  var btns = [].slice.call(document.querySelectorAll('[data-rev-filter]'));
  // 붉은 표시는 고른 사람 기준이다 (사용자 지시 2026-10-07):
  //   내가 담당자  → 머지 전이면 붉다 (이 보드는 open PR 만 싣는다 = 늘 붉다)
  //   내가 리뷰어  → 내가 아직 승인하지 않았으면 붉다
  //   아무도 안 고름 → 중립 기준(미해결 스레드가 남았으면 붉다)
  function paint(who) {
    [].forEach.call(document.querySelectorAll('tr[data-rev]'), function (tr) {
      var red;
      if (!who) {
        red = tr.dataset.unres === '1';
      } else if ((tr.getAttribute('data-own') || '').split(',').indexOf(who) >= 0) {
        red = true;
      } else if ((tr.getAttribute('data-rev') || '').split(',').indexOf(who) >= 0) {
        red = (tr.getAttribute('data-appr') || '').split(',').indexOf(who) < 0;
      } else {
        red = false;
      }
      tr.classList.toggle('unres', red);
      // 붉지 않으면서 머지 대기면 회색 — 승인 다 받고 멈춰 있는 PR 이 눈에서 사라지지 않게
      tr.classList.toggle('ready', !red && tr.dataset.ready === '1');
    });
    var hint = document.getElementById('redHint');
    if (hint) hint.textContent = who
      ? ('붉은색 = ' + who + ' 가 움직일 차례 (내 PR: 머지 전 · 내가 리뷰어: 내가 승인 안 함) · 회색 = 승인 끝, 머지 대기')
      : '붉은색 = resolve 안 한 코멘트가 남음 · 회색 = 승인 끝, 머지 대기';
  }
  function apply(who, pendOnly) {
    [].forEach.call(document.querySelectorAll('tr[data-rev]'), function (tr) {
      var rev = (tr.getAttribute(pendOnly ? 'data-pend' : 'data-rev') || '').split(',');
      var own = (tr.getAttribute('data-own') || '').split(',');
      // 그 사람이 리뷰어이거나 담당자면 남긴다 (이현욱 요청 2026-10-07 — 담당자 PR 이 필터에서 사라지던 것)
      var hit = !who ? (pendOnly ? rev.filter(Boolean).length > 0 : true)
                     : (rev.indexOf(who) >= 0 || (!pendOnly && own.indexOf(who) >= 0));
      tr.classList.toggle('hidden', !hit);
    });
    paint(who);
    [].forEach.call(document.querySelectorAll('section.who'), function (sec) {
      var vis = sec.querySelectorAll('tr[data-rev]:not(.hidden)').length;
      sec.classList.toggle('hidden', vis === 0);
      var m = sec.querySelector('h2 .meta');
      if (m && !m.dataset.all) m.dataset.all = m.textContent;
      if (m) m.textContent = (who || pendOnly) ? (vis + ' PR (필터)') : m.dataset.all;
    });
  }
  btns.forEach(function (b) {
    b.addEventListener('click', function () {
      btns.forEach(function (x) { x.classList.remove('on'); });
      b.classList.add('on');
      apply(b.getAttribute('data-rev-filter'), b.hasAttribute('data-pend-only'));
    });
  });
})();
</script>"""

def render(rows, now, orphans=()):
    by = {}
    for r in rows:
        by.setdefault(r['tracked'][0], []).append(r)
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
    .pill.APPROVED{color:var(--gate);border-color:var(--gate)}.pill.CHANGES_REQUESTED{color:var(--bad);border-color:var(--bad)}.pill.req{color:var(--warn);border-color:var(--warn)}.pill.pend{color:var(--muted);border-color:var(--muted)}
    a{color:inherit} .docs a{display:block;font-size:12px;font-family:"IBM Plex Mono",monospace;color:var(--gate)} .muted{color:var(--muted)}
    /* 에이전트 리뷰 열은 켜지 않는 한 보이지 않는다 (사용자 지시 2026-10-06: 헷갈린다) */
    /* 내 전용 열(에이전트 리뷰 · 리뷰 문서)은 켜지 않는 한 보이지 않는다 */
    .col-mine{display:none} body.show-mine .col-mine{display:table-cell} body.show-mine section.col-mine{display:block!important}
    /* resolve 안 한 코멘트가 남은 PR 은 행을 붉게 (사용자 지시 2026-10-07) */
    tr.unres td:first-child{box-shadow:inset 3px 0 0 var(--bad)}
    tr.unres td{background:color-mix(in srgb, var(--bad) 6%, transparent)}
    tr.unres .n a{color:var(--bad);font-weight:600}
    /* 승인 다 받았는데 머지 안 한 PR — 사라지게 하지 말고 회색 (사용자 지시 2026-10-07) */
    tr.ready td{background:color-mix(in srgb, var(--muted) 7%, transparent);color:var(--muted)}
    tr.ready td:first-child{box-shadow:inset 3px 0 0 var(--muted)} tr.ready .n a{color:var(--muted)}
    .bar{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:16px 0 4px;padding:10px 12px;background:var(--surface);border:1px solid var(--line);border-radius:8px}
    .bar .lbl{font-family:"IBM Plex Mono",monospace;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}
    .btn{font:inherit;font-size:12px;font-family:"IBM Plex Mono",monospace;padding:3px 10px;border-radius:999px;border:1px solid var(--line);background:var(--soft);color:var(--ink);cursor:pointer}
    .btn:hover{border-color:var(--gate)} .btn.on{background:var(--gate);border-color:var(--gate);color:#fff}
    .hidden{display:none}
    """
    h = ['<!doctype html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>review-to-do</title><style>%s</style></head><body><div class="wrap">' % css]
    h.append('<h1>review-to-do</h1><div class="meta">스냅샷 %s · assignee(없으면 작성자) ∈ {%s} · open · non-draft · 미해결 스레드는 리뷰 코멘트 기준 · CI 는 보지 않음(사용자 지시) · TC PR 은 JIRA 키/cubrid#n 으로 엔진 PR 하위에 연동</div>' % (now, ', '.join(TRACKED)))
    # 머리 통계는 '미완료 PR 건수' 하나만 (사용자 지시 2026-10-06)
    # 미완료 = 머지 가능이 아닌 것 = 미해결 스레드가 남았거나 승인자가 0
    tot_open = sum(1 for r in rows if r['unresolved'] or not r.get('approvers'))
    h.append('<div class="facts"><div class="fact"><b class="%s">%d</b><span>미완료 PR <span class="muted">/ 전체 %d</span></span></div></div>' % ('bad' if tot_open else 'ok', tot_open, len(rows)))
    # 툴바: 리뷰어 필터 + 에이전트 열 토글 (사용자 지시 2026-10-06)
    h.append('<div class="bar"><span class="lbl">사람으로 모아 보기 <span class="muted" style="text-transform:none;letter-spacing:0">담당자 + 리뷰어</span></span>'
             + '<button class="btn on" data-rev-filter="">전체</button>'
             + ''.join('<button class="btn" data-rev-filter="%s">%s</button>' % (esc(c), esc(c)) for c in TRACKED)
             + '<button class="btn" data-rev-filter="" data-pend-only="1">승인 전·미착수만</button>'
             + '<span id="redHint" class="muted" style="font-size:11px">붉은색 = resolve 안 한 코멘트가 남음</span><span style="flex:1"></span>'
             + '<label class="lbl" style="cursor:pointer"><input type="checkbox" id="agentToggle"> 내 전용 열 보기 <span class="muted" style="text-transform:none;letter-spacing:0">에이전트 리뷰 · 리뷰 문서</span></label>'
             + '</div>')
    h.append('<div class="meta" style="margin:6px 0 2px">누구 차례인가 — <b>미해결 &gt; 0</b>: 작성자 · <b>미해결 0 + 승인자 0</b>: 리뷰어(승인 필요, "승인만 남은 PR") · <b>미해결 0 + 승인자 ≥ 1</b>: 머지 가능. 리뷰어 필 — <span class="pill req">미착수</span> 요청됐고 리뷰 이력 없음 · <span class="pill pend">승인 전</span> 리뷰했으나 승인 안 함(GitHub 은 리뷰 제출 시 요청 목록에서 빼므로 이 상태가 따로 필요하다)</div>')
    # 사람별 미완료 PR (사용자 지시 2026-10-06: 업무 부하표 대신 이것)
    # 미완료 = 머지 가능이 아닌 것 — 미해결 스레드가 남았거나 승인자가 0.
    # 본인이 assignee 인 것(=작성자가 움직일 차례)과 본인이 리뷰어인 것(=내가 움직일 차례)을 나눈다.
    people = list(TRACKED)
    mine = {c: {'own': [], 'rev': [], 'ready': []} for c in people}
    for r in rows:
        done = (not r['unresolved']) and bool(r.get('approvers'))   # 승인 끝·미해결 0 = 머지 대기
        for c in r.get('tracked', []):
            if c in mine:
                k = 'ready' if done else 'own'
                if r['number'] not in mine[c][k]: mine[c][k].append(r['number'])
        if done:
            continue                                     # 리뷰어 몫은 남지 않았다
        for c in set(r.get('requested', []) + r.get('pending_approval', [])):
            if c in mine and r['number'] not in mine[c]['rev']: mine[c]['rev'].append(r['number'])
    tot = {c: len(v['own']) + len(v['rev']) for c, v in mine.items()}
    mx = max(tot.values() or [1])
    h.append('<h2>미완료 PR <span class="meta">내 PR(작성자 차례) / 내가 리뷰어(내 차례) / 머지 대기(승인 끝, 회색)</span></h2>')
    h.append('<table><tr><th>사람</th><th>내 PR</th><th>내가 리뷰어</th><th>머지 대기</th><th>합계</th><th></th></tr>')
    fmt = lambda xs: '<span class="muted" style="font-size:11px">%s</span>' % esc(' '.join('#%d' % x for x in sorted(xs))) if xs else ''
    for c in sorted(people, key=lambda c: -tot[c]):
        v = mine[c]; bar = '<div style="height:8px;border-radius:4px;background:var(--gate);width:%d%%;opacity:.7"></div>' % (100 * tot[c] // mx if mx else 0)
        h.append('<tr><td class="n">%s</td><td class="n">%d %s</td><td class="n">%d %s</td><td class="n muted">%d %s</td><td class="n"><b class="%s">%d</b></td><td style="width:180px">%s</td></tr>' % (
            esc(c), len(v['own']), fmt(v['own']), len(v['rev']), fmt(v['rev']), len(v['ready']), fmt(v['ready']), 'bad' if tot[c] >= mx and mx >= 6 else '', tot[c], bar))
    h.append('</table>')
    for who in TRACKED:
        rs = by.get(who, [])
        h.append('<section class="who"><h2>@%s <span class="meta">%d PR</span></h2>' % (esc(who), len(rs)))
        if not rs:
            h.append('<div class="muted">추적 대상 없음 (open·non-draft·assignee 또는 작성자 기준)</div></section>'); continue
        h.append('<table><tr><th>PR</th><th>제목 / JIRA</th><th>미해결</th><th>리뷰어</th><th class="col-mine">에이전트 리뷰</th><th>갱신</th><th>연동된 테스트케이스</th><th class="col-mine">리뷰 문서(내부망)</th></tr>')
        for r in rs:
            unres = ('<span class="%s">%d</span> / %d' % ('bad' if r['unresolved'] else 'ok', r['unresolved'], r['threads_total']))
            if r['unresolved_by']:
                unres += '<div class="muted" style="font-size:11px">' + ', '.join('%s %d' % (esc(a), n) for a, n in sorted(r['unresolved_by'].items(), key=lambda x: -x[1])) + '</div>'
            rev = ''.join('<span class="pill %s">%s %s</span>' % (esc(st), esc(a), {'APPROVED': '✓', 'CHANGES_REQUESTED': '✗', 'COMMENTED': '…', 'DISMISSED': '–'}.get(st, st)) for a, st, _ in r['latest_reviews'] if a not in BOTS)
            rev += ''.join('<span class="pill req">%s 미착수</span>' % esc(a) for a in r['requested'])
            rev += ''.join('<span class="pill pend">%s 승인 전</span>' % esc(a) for a in r.get('pending_approval', []))
            rev += '<div class="muted" style="font-size:11px">decision: %s</div>' % esc(r['decision'])
            ag = ('<span class="ok">✓ %s</span><div class="muted" style="font-size:11px">%d건 · %s</div>' % (esc(r['agent_last']), r['agent_n'], esc('/'.join(r['agent_kinds'])))) if r['agent_n'] else '<span class="warn">아직</span>'
            tcl = ''.join('<div><a href="%s">%s#%d</a> <span class="muted">미해결 %d/%d%s</span></div>' % (esc(t['url']), esc(t['repo'].split('/')[1].replace('cubrid-testcases','tc')), t['number'], t['unresolved'], t['threads_total'], (' · ' + ','.join(esc(a) + ' 미착수' for a in t['requested'])) if t['requested'] else '') for t in r['linked_tc']) or '<span class="muted">없음</span>'
            docs = ''.join('<a href="%s%s">%s</a>' % (DOCS_HTTP_BASE, quote(d), esc(d)) for d in r['docs']) or '<span class="muted">없음</span>'
            others = [a for a in r['assignees'] if a != r['tracked'][0]]
            revs = sorted(set(r.get('requested', []) + r.get('pending_approval', []) + r.get('approvers', [])))
            pend_revs = sorted(set(r.get('requested', []) + r.get('pending_approval', [])))
            owns = sorted(set(r.get('assignees', []) + r.get('tracked', [])))   # 담당자 (이현욱 요청 2026-10-07)
            apprs = sorted(set(r.get('approvers', [])))                          # 승인한 사람
            ready = (not r['unresolved']) and bool(apprs)                         # 승인 끝·미해결 0 = 머지 대기
            h.append('<tr class="%s" data-unres="%s" data-ready="%s" data-rev="%s" data-pend="%s" data-own="%s" data-appr="%s"><td class="n"><a href="%s">%s#%d</a>%s</td><td>%s<div class="meta">%s · by %s%s</div></td><td class="n">%s</td><td>%s</td><td class="n col-mine">%s</td><td class="n">%s<div class="muted" style="font-size:11px">생성 %s</div></td><td class="docs" style="font-size:12px">%s</td><td class="docs col-mine">%s</td></tr>' % (
                ('unres' if r['unresolved'] else ('ready' if ready else '')), '1' if r['unresolved'] else '0', '1' if ready else '0', esc(','.join(revs)), esc(','.join(pend_revs)), esc(','.join(owns)), esc(','.join(apprs)),
                esc(r['url']), esc(r['repo'].split('/')[1]), r['number'], '<div class="muted" style="font-size:11px">%s</div>' % esc(r['tracked'][0]) if len(rs) and who != r['tracked'][0] else '',
                esc(r['title']), esc(r['jira'] or '-'), esc(r['author']), ((' · assignees +' + ','.join(others)) if others else '') + (' · <span class="warn">작성자 기준(assignee 없음)</span>' if r.get('tracked_via') == 'author' else ''),
                unres, rev, ag, esc(r['updated']), esc(r['created']), tcl, docs))
        h.append('</table></section>')
    if orphans:
        # 본문 PR 행과 같은 data-own/data-rev 속성을 붙여 툴바의 사람 필터가 여기에도 걸리게 한다 — 사람을 클릭하면
        # 그 사람이 담당자(assignee)이거나 리뷰어인 TC PR 만 남고, 0건이면 절이 접힌다 (사용자 지시 2026-10-13).
        h.append('<section class="who"><h2>연동 대상 없는 TC PR <span class="meta">%d</span></h2><table><tr><th>PR</th><th>제목</th><th>담당자</th><th>미해결</th><th>리뷰어</th></tr>' % len(orphans))
        for t in orphans:
            revs = sorted(set(t.get('requested', []) + t.get('pending_approval', []) + t.get('approvers', [])))
            pend_revs = sorted(set(t.get('requested', []) + t.get('pending_approval', [])))
            owns = sorted(set(t.get('assignees', []) + t.get('tracked', [])))
            apprs = sorted(set(t.get('approvers', [])))
            ready = (not t['unresolved']) and bool(apprs)
            rev = ''.join('<span class="pill req">%s 미착수</span>' % esc(a) for a in t['requested'])
            rev += ''.join('<span class="pill pend">%s 승인 전</span>' % esc(a) for a in t.get('pending_approval', []))
            rev += ''.join('<span class="pill APPROVED">%s ✓</span>' % esc(a) for a in apprs)
            h.append('<tr class="%s" data-unres="%s" data-ready="%s" data-rev="%s" data-pend="%s" data-own="%s" data-appr="%s"><td class="n"><a href="%s">%s#%d</a></td><td>%s</td><td>%s</td><td class="n">%d/%d</td><td>%s</td></tr>' % (
                ('unres' if t['unresolved'] else ('ready' if ready else '')), '1' if t['unresolved'] else '0', '1' if ready else '0', esc(','.join(revs)), esc(','.join(pend_revs)), esc(','.join(owns)), esc(','.join(apprs)),
                esc(t['url']), esc(t['repo'].split('/')[1]), t['number'], esc(t['title']), esc(', '.join(owns)), t['unresolved'], t['threads_total'], rev or '-'))
        h.append('</table></section>')
    h.append(render_headless())
    h.append(TOOLBAR_JS)
    h.append('<p class="meta" style="margin-top:26px">생성기 dev4-review-workspace/tools/review_board_gen.py · 갱신 규약 skills/review-board/SKILL.md · 추적 ID 는 tools/roster.json</p></div></body></html>')
    return '\n'.join(h)

if __name__ == '__main__':
    main()
