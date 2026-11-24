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
  추적 밖 PR: assignee·작성자가 추적 ID 가 아니어도 추적 ID 가 리뷰어인 open·non-draft PR 은 맨 아래 [추적 밖 PR 절]에 따로 싣는다
  (사용자 지시 2026-11-24 "팀원거 아니어도 밑에 따로 보여야" — hgryoo 의 cubrid#8624 에 soheejung-cs 가 리뷰어로 요청돼 있는데 보드에 없었다).
  리뷰어 = reviewRequests 의 사람 + latestReviews 를 남긴 사람. 팀 단위 요청은 사람으로 풀지 않는다(지금 리포의 요청은 사람 단위).

[PR 행] (details·agent_reviewed·local_docs·tc_prs)
  - 미해결 리뷰 스레드 수 / 전체 스레드 수, 미해결 스레드를 연 사람별 개수.
  - 리뷰어 필: 최신 리뷰 상태(✓ APPROVED · ✗ CHANGES_REQUESTED · … COMMENTED · – DISMISSED, 봇 제외)와 두 갈래의 남은 일 —
    `미착수`(요청됐고 리뷰 이력 0) / `승인 전`(리뷰했으나 APPROVED 아님). GitHub 은 리뷰를 제출하는 순간 그 사람을 reviewRequests 에서
    빼므로, 요청 목록만 보면 코멘트 한 번 남기고 승인하지 않은 리뷰어가 사라진다(2026-09-21). 재요청되면 다시 `미착수`.
    그 아래 reviewDecision.
  - TC 신호등(사용자 지시 2026-11-10 "agent-to-do 보드에 있는 테스트 결과 보는 걸 review-to-do 에도" — 2026-09 의 "CI 는 보지 않음" 을
    대체): 엔진 PR 행의 `TC 테스트` 열에 헤드 커밋의 `gha-ci: test_sql|medium|shell|sql_by_cci`(표시 `cci`, 2026-11-24 사용자 지시로 다시 싣는다)
    를 초록(통과)·노랑(도는 중)·빨강(실패)·회색(런 없음)으로.
    헤드에 결과가 없으면 앞 커밋 5개의 가장 최근 결과, 런이 큐에 있으면 노랑. 링크는 도는 런·앞 커밋 결과면 그 런, 아니면 PR checks.
    그 밖의 체크(code-style·cppcheck·Check TC PRs …)는 싣지 않는다 — ci_summary() 는 남아 있지만 render 가 쓰지 않는다.
  - 연동된 TC PR: JIRA 키(또는 제목의 `cubrid#<n>`)가 같은 cubrid-testcases / -private-ex PR 을 엔진 PR 하위에 — 미해결 수·미착수 리뷰어.
  - 갱신·생성일.

[머리 통계·사람별 표] (사용자 지시 2026-10-06)
  머리 통계는 '미완료 PR' 하나 — 미완료 = 머지 가능이 아닌 것 = 미해결 스레드가 남았거나 승인자가 0. `18 / 전체 21` 꼴.
  사람별 표는 사람마다 `내 PR`(assignee — 작성자가 움직일 차례) / `내가 리뷰어`(미착수·승인 전 — 내 차례) / `머지 대기`(승인 끝·미해결 0, 회색).
  `내가 리뷰어` 는 추적 밖 PR 도 센다(2026-11-24). 머리 통계 '미완료 PR' 은 추적 PR 만 센다.

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
  추적 밖 TC PR(작성자는 추적 밖, 추적 ID 가 리뷰어)도 붙을 엔진 PR 이 없으면 여기에 온다.

[추적 밖 PR 절] (사용자 지시 2026-11-24, pr_row — 사람별 절과 같은 행)
  추적 ID 가 리뷰어인 추적 밖 엔진 PR 중 그 추적 ID 가 아직 응답할 몫(미착수·승인 전)이 있는 것 — 추적 ID 가 승인을 마친 것은 싣지 않는다.
  열·data-* 속성·붉은 행 판정이 사람별 절과 같아 툴바에서 사람을 고르면 그 사람이 리뷰어인 행만 남는다.
  연동된 TC PR: 추적 TC PR 을 먼저 추적 행에, 못 붙으면 이 행에 붙이고, 이 행의 TC PR 은 작성자도 대개 추적 밖이라 list_prs 가 이미 받은
  open·non-draft 목록(ALL_OPEN)에서 JIRA 키·`cubrid#<n>` 으로 더 찾는다(스레드·리뷰어만 읽는다 — record(light=True)).
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

def sh_retry(*a, tries=3):
    """목록 질의용 — GitHub GraphQL 의 일시 실패(시간 초과·502)에 보드 갱신 한 번이 통째로 죽지 않게 두 번 더 묻는다."""
    for k in range(tries):
        try:
            return sh(*a)
        except subprocess.CalledProcessError:
            if k == tries - 1:
                raise
            import time
            time.sleep(5 * (k + 1))

ALL_OPEN = {}   # repo -> open·non-draft PR 원본 — 추적 밖 엔진 PR 의 TC PR 을 찾을 때 gh 에 다시 묻지 않는다

def list_prs(repo):
    """(추적 PR, 추적 밖 PR). 추적 밖 = assignee·작성자는 추적 ID 가 아니지만 추적 ID 가 리뷰어(요청됨·리뷰함)인 것."""
    js = sh_retry('gh', 'pr', 'list', '--repo', repo, '--state', 'open', '--limit', '300', '--json',
                  'number,title,url,isDraft,assignees,author,createdAt,updatedAt,headRefName,statusCheckRollup')
    # 리뷰어 필드는 따로 묻는다 — 한 질의에 statusCheckRollup 과 같이 실으면 cubrid(open 196)에서 GraphQL 이 실패했다(2026-11-24, 재시도로는 통과)
    revjs = sh_retry('gh', 'pr', 'list', '--repo', repo, '--state', 'open', '--limit', '300', '--json', 'number,reviewRequests,latestReviews')
    revs_of = {q['number']: q for q in json.loads(revjs)}
    out, outside = [], []
    ALL_OPEN[repo] = []
    for p in json.loads(js):
        p.update({k: v for k, v in revs_of.get(p['number'], {}).items() if k != 'number'})
        if p.get('isDraft'):
            continue
        ass = [a['login'] for a in p.get('assignees', [])]
        p['repo'] = repo; p['assignee_logins'] = ass
        ALL_OPEN[repo].append(p)
        hit = [a for a in ass if a in TRACKED]
        via = 'assignee'
        # assignee 에 추적 ID 가 없으면 작성자(author)로도 잡는다 — assignee 를 비워 둔 PR 이 보드에서 빠지던 것(사용자 지시 2026-10-14).
        # open·non-draft 조건은 같다. 행에는 '작성자 기준' 을 표시한다.
        if not hit and (p.get('author') or {}).get('login') in TRACKED:
            hit = [p['author']['login']]; via = 'author'
        if hit:
            p['tracked'] = hit; p['tracked_via'] = via
            out.append(p)
            continue
        # 추적 밖이라도 추적 ID 가 리뷰어면 따로 싣는다(사용자 지시 2026-11-24 "팀원거 아니어도 밑에 따로 보여야").
        # 리뷰어 = 요청된 사람(팀 요청은 풀지 않는다) + 최신 리뷰를 남긴 사람 — details() 의 미착수·승인 전과 같은 범위.
        revs = {(r or {}).get('login') for r in (p.get('reviewRequests') or [])}
        revs |= {((r or {}).get('author') or {}).get('login') for r in (p.get('latestReviews') or [])}
        rhit = [c for c in TRACKED if c in revs]
        if rhit:
            p['tracked'] = []; p['tracked_via'] = 'reviewer'; p['tracked_reviewers'] = rhit
            outside.append(p)
    return out, outside

def details(repo, num):
    owner, name = repo.split('/')
    q = '''{repository(owner:"%s",name:"%s"){pullRequest(number:%d){
      reviewThreads(first:100){totalCount nodes{isResolved isOutdated comments(first:1){nodes{author{login} path}}}}
      reviewRequests(first:30){nodes{requestedReviewer{... on User{login} ... on Team{name}}}}
      latestReviews(first:30){nodes{author{login} state submittedAt}}
      reviewDecision mergeable
      commits(last:6){nodes{commit{oid status{contexts{context state targetUrl}}}}}
    }}}''' % (owner, name, num)
    d = json.loads(sh('gh', 'api', 'graphql', '-f', 'query=' + q))['data']['repository']['pullRequest']
    # TC 신호등(사용자 지시 2026-11-10, agent-todo 보드의 것을 여기에도): 헤드 커밋의 `gha-ci: test_sql|medium|shell` 상태.
    # 헤드에 없으면(런을 건 뒤 push) 앞 커밋 5개 중 가장 최근 결과를 쓴다 — 같은 쿼리로 받아 PR 마다 호출을 늘리지 않는다
    ci, ci_stale, ci_runs = {}, None, []   # ci_runs: 보이는 상태를 단 런 id 들(ci_lights 가 큐의 새 런과 견준다, board.json 에 실리므로 list)
    commits = [n['commit'] for n in ((d.get('commits') or {}).get('nodes') or [])]
    for k, c in enumerate(reversed(commits)):
        xs = [(m.group(1), x) for x in ((c.get('status') or {}).get('contexts') or [])
              for m in [GHA_CTX.match(x.get('context') or '')] if m]
        if xs:
            ci, ci_stale = {s: (x.get('state') or '').upper() for s, x in xs}, (c['oid'] if k else None)
            ci_runs = sorted({run_id(x.get('targetUrl')) for _, x in xs} - {None})
            break
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
            'decision': d.get('reviewDecision') or '-', 'mergeable': d.get('mergeable') or '-',
            'ci': ci, 'ci_stale': ci_stale, 'ci_runs': ci_runs}

# sql_by_cci 도 싣는다(사용자 지시 2026-11-24 "cci 결과도 보드에 다시 표시해 주는 것으로 바꿔 줘 이제 봐도 됨" — 11-11 의 'CCI 보지 마' 를 푼다)
GHA_CTX = re.compile(r'gha-ci: test_(sql|medium|shell|sql_by_cci)$')

# gha-ci 는 `/run` 코멘트(issue_comment)로 develop 위에서 돌아 PR 브랜치로는 런을 못 찾는다 — 런 제목 "PR #N · …" 으로
# 묶는다. 코멘트마다 생기는 skipped 런은 뺀다. 보드 생성마다 한 번만 읽는다(agent-board-gen.py 와 같은 규칙).
_GHA_RUNS = None
_LAST_SKIP = {}      # PR 번호 → 가장 최근 skipped 런의 생성 시각(코멘트마다 런이 하나씩 생기고 /run 이 아니면 skipped)
_WINDOW_START = ''   # 읽은 런 목록(최근 300런)의 가장 이른 생성 시각 — 그보다 앞 코멘트는 런을 만들었는지 알 수 없다
def gha_run(num):
    global _GHA_RUNS, _WINDOW_START
    if _GHA_RUNS is None:
        _GHA_RUNS = {}
        try:
            runs = json.loads(sh('gh', 'run', 'list', '--repo', 'CUBRID/cubrid', '--workflow', 'gha-ci.yml', '--limit', '300',
                                 '--json', 'databaseId,status,conclusion,displayTitle,createdAt'))
        except Exception:
            runs = []
        _WINDOW_START = min((r.get('createdAt') or '' for r in runs), default='')
        for r in runs:   # 최신순
            m = re.match(r'PR #(\d+) ', r.get('displayTitle') or '')
            if m and r.get('conclusion') != 'skipped':
                _GHA_RUNS.setdefault(int(m.group(1)), r)
            elif m:
                _LAST_SKIP.setdefault(int(m.group(1)), r.get('createdAt') or '')
    return _GHA_RUNS.get(num)

def run_id(url):
    m = re.search(r'/actions/runs/(\d+)', url or '')
    return int(m.group(1)) if m else None

def kst(ts):
    try:
        return (datetime.datetime.strptime(ts, '%Y-%m-%dT%H:%M:%SZ') + datetime.timedelta(hours=9)).strftime('%m-%d %H:%M')
    except Exception:
        return ts

# 마지막 실행 런 뒤에 단 `/run …` 코멘트가 런을 만들지 못했으면(skipped) (시각, 코멘트 url, 사유)를 낸다(agent-board-gen.py 와
# 같은 규칙). gha-ci 의 gate 는 startsWith(body, '/run') 이고 작성자가 OWNER·MEMBER·COLLABORATOR 여야 한다(gha-ci.yml L191-192)
# — 웹 화면에서 줄바꿈으로 시작한 '/run all' 은 아무 알림 없이 건너뛰어지고, 보드는 앞 런의 결과를 계속 보여 준다(사용자 2026-11-24
# "run all을 돌렸으면 새걸로 나와야하는데 왜 옛날 기록이 보일까?" — #8587 17:56 코멘트). 코멘트는 마지막 skipped 런이 마지막
# 실행 런보다 새로울 때만 읽는다(코멘트마다 런이 생기므로 그렇지 않으면 그 뒤 코멘트가 없다).
_RUN_SKIPPED = {}
def run_skipped(num):
    if num in _RUN_SKIPPED:
        return _RUN_SKIPPED[num]
    _RUN_SKIPPED[num] = None
    since = (gha_run(num) or {}).get('createdAt') or ''
    last_skip = _LAST_SKIP.get(num) or ''
    if not last_skip or last_skip <= since:
        return None
    q = 'repos/CUBRID/cubrid/issues/%d/comments?per_page=100' % num + ('&since=' + since if since else '')
    try:
        rows = sh('gh', 'api', '--paginate', q, '--jq',
                  '.[] | select((.body // "") | test("^\\\\s*/run")) | '
                  '[.created_at, .html_url, ((.body // "") | startswith("/run")), .author_association] | @tsv')
    except Exception:
        return None
    cands = [l.split('\t') for l in rows.splitlines() if l.count('\t') == 3]
    cands = [c for c in cands if c[0] > since]
    if not cands:
        return None
    at, url, starts, assoc = cands[-1]
    if last_skip < at:   # 그 코멘트 뒤에 생긴 skipped 런이 없다 — 막 달아 런이 아직 안 생겼다
        return None
    if not _WINDOW_START or at < _WINDOW_START:   # 목록보다 앞 코멘트 — 그것이 만든 런이 목록 밖일 수 있다
        return None
    if starts != 'true':
        why = '본문이 /run 으로 시작하지 않는다(앞에 줄바꿈·공백) — 첫 글자부터 /run 으로 다시 단다'
    elif assoc not in ('OWNER', 'MEMBER', 'COLLABORATOR'):
        why = '작성자 권한(%s)이 gate 조건(OWNER·MEMBER·COLLABORATOR)에 없다' % assoc
    else:
        why = 'gate 가 건너뛰었다(원인 미상 — 런 목록을 볼 것)'
    _RUN_SKIPPED[num] = (at, url, '%s 의 /run 코멘트가 런을 만들지 못했다: %s. 신호등은 그 앞 런의 결과다' % (kst(at), why))
    return _RUN_SKIPPED[num]

# 통과 초록, 도는 중 노랑, 실패 빨강, 런 없음 회색 — 앞 커밋 결과를 써도 '이전 head' 표시는 달지 않는다(agent-todo 보드와 같게)
CI_LIGHT = {'SUCCESS': ('ok', '통과'), 'PENDING': ('run', '도는 중'), 'EXPECTED': ('run', '도는 중'),
            'FAILURE': ('bad', '실패'), 'ERROR': ('bad', '실패')}

def ci_lights(r):
    run = gha_run(r['number'])
    waiting = run is not None and run.get('status') != 'completed'
    # 큐의 새 런이 아직 상태를 달지 않았는데 보이는 결과가 그보다 앞 런의 것이면(develop 을 머지해 헤드가 바뀌어 앞 커밋에서
    # 빌려 왔거나, 같은 헤드에 /run 을 다시 달았다) 옛 결과 대신 '도는 중'으로 보인다(사용자 2026-11-24 "왜 옛날 기록이 보일까?").
    ci = {} if waiting and run['databaseId'] not in r.get('ci_runs', ()) else r['ci']
    skip = run_skipped(r['number'])
    out = ''
    for s in ('sql', 'medium', 'shell', 'sql_by_cci'):
        st = ci.get(s, '')
        if not st and waiting:
            st = 'PENDING'   # 런이 큐에 있어 아직 pending 상태를 달지 않았다
        cls, word = CI_LIGHT.get(st, ('none', '런 없음'))
        out += '<span class="light light-%s" title="%s: %s"><i></i>%s</span>' % (cls, s, word, 'cci' if s == 'sql_by_cci' else s)
    if skip:   # 런을 못 만든 /run 코멘트 — 신호등은 그 앞 런의 결과이므로 경고를 붙이고 링크는 그 코멘트로
        out += '<span class="light light-skip" title="%s"><i></i>/run 안 돎</span>' % esc(skip[2])
    href = (skip[1] if skip
            else 'https://github.com/CUBRID/cubrid/actions/runs/%d' % run['databaseId'] if run and (r['ci_stale'] or waiting)
            else 'https://github.com/CUBRID/cubrid/pull/%d/checks' % r['number'])
    return '<a class="lights" href="%s" target="_blank" rel="noopener">%s</a>' % (esc(href), out)

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

def record(p, light=False):
    """board.json 의 PR 레코드. light=True 는 스레드·리뷰어만 읽는다 — 추적 밖 엔진 PR 에 붙는 TC PR(행에는 미해결 수·미착수만 쓴다)."""
    d = details(p['repo'], p['number'])
    key = jira_key(p['title'])
    rec = {'repo': p['repo'], 'number': p['number'], 'title': p['title'], 'url': p['url'], 'author': p['author']['login'],
           'assignees': p['assignee_logins'], 'tracked': p.get('tracked', []), 'tracked_via': p.get('tracked_via', 'assignee'), 'created': p['createdAt'][:10], 'updated': p['updatedAt'][:10],
           'jira': key, 'docs': [] if light else local_docs(key, p['number']), 'linked_tc': [], **d}
    if p.get('tracked_reviewers'):
        rec['tracked_reviewers'] = p['tracked_reviewers']
    rec['agent_n'], rec['agent_last'], rec['agent_kinds'] = (0, '', []) if light else agent_reviewed(p['repo'], p['number'])
    if p['repo'] != 'CUBRID/cubrid':
        m = re.search(r'cubrid#(\d+)', p['title'])
        rec['engine_pr'] = int(m.group(1)) if m else None
    return rec

def tc_home(t, homes):
    """TC PR 이 붙을 엔진 PR 행 — JIRA 키가 같거나 제목이 cubrid#<n> 을 가리키는 것 (사용자 지시 2026-09-16)"""
    return next((r for r in homes if (t.get('engine_pr') and r['number'] == t['engine_pr']) or (t['jira'] and t['jira'] == r['jira'])), None)

def main():
    prs, outs = [], []
    for repo in REPOS:
        mine, other = list_prs(repo)
        prs += mine; outs += other
    recs = [record(p) for p in prs]
    rows = [r for r in recs if r['repo'] == 'CUBRID/cubrid']
    tcs = [r for r in recs if r['repo'] != 'CUBRID/cubrid']
    # 추적 밖 PR 은 추적 ID 가 아직 응답할 몫(미착수·승인 전)이 있는 것만 싣는다 — 추적 ID 가 이미 승인했으면 그 사람의 할 일이 아니고,
    # 작성자도 추적 밖이라 머지 대기로 남길 까닭이 없다(첫 생성에서 shparkcubrid 가 승인만 한 PR 7개가 절을 채웠다, 2026-11-24)
    orecs = []
    for p in outs:
        rec = record(p, light=True)
        rec['tracked_reviewers'] = [c for c in TRACKED if c in set(rec['requested'] + rec['pending_approval'])]
        if not rec['tracked_reviewers']:
            continue
        if rec['repo'] == 'CUBRID/cubrid':   # 엔진 행은 사람별 절의 행처럼 리뷰 문서·에이전트 리뷰 열도 채운다
            rec['docs'] = local_docs(rec['jira'], rec['number'])
            rec['agent_n'], rec['agent_last'], rec['agent_kinds'] = agent_reviewed(rec['repo'], rec['number'])
        orecs.append(rec)
    outside = [r for r in orecs if r['repo'] == 'CUBRID/cubrid']
    tcs += [r for r in orecs if r['repo'] != 'CUBRID/cubrid']
    # 연동: 추적 행에 먼저, 못 붙으면 추적 밖 행에
    for t in tcs:
        home = tc_home(t, rows) or tc_home(t, outside)
        if home is not None:
            home['linked_tc'].append(t); t['linked'] = True
        else:
            t['linked'] = False
    # 추적 밖 엔진 PR 의 TC PR 은 작성자도 대개 추적 밖이라 위 목록에 없다 — 이미 받은 open·non-draft 목록에서 더 찾는다
    seen = {(t['repo'], t['number']) for t in tcs}
    for repo in REPOS:
        if repo == 'CUBRID/cubrid' or not outside:
            continue
        for p in ALL_OPEN.get(repo, []):
            if (repo, p['number']) in seen:
                continue
            m = re.search(r'cubrid#(\d+)', p['title'])
            probe = {'engine_pr': int(m.group(1)) if m else None, 'jira': jira_key(p['title'])}
            home = tc_home(probe, outside)
            if home is not None:
                t = record(p, light=True); t['linked'] = True
                home['linked_tc'].append(t)
    orphans = [t for t in tcs if not t.get('linked')]
    rows.sort(key=lambda r: (r['tracked'][0], -r['unresolved'], r['updated']))
    outside.sort(key=lambda r: (-r['unresolved'], r['updated']))
    now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M KST')
    os.makedirs(OUT, exist_ok=True)
    dump = lambda: json.dump({'generated': now, 'tracked': TRACKED, 'prs': rows, 'unlinked_tc_prs': orphans, 'outside_prs': outside}, open(os.path.join(OUT, 'board.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    dump()
    open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(render(rows, now, orphans, outside))
    print('%s review board: %d PRs + %d outside (tracked %d ids) -> %s/index.html' % (now, len(rows), len(outside), len(TRACKED), OUT))

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

PR_TABLE_HEAD = ('<table><tr><th>PR</th><th>제목 / JIRA</th><th>미해결</th><th>리뷰어</th><th class="col-mine">에이전트 리뷰</th><th>갱신</th>'
                 '<th>TC 테스트</th><th>연동된 테스트케이스</th><th class="col-mine">리뷰 문서(내부망)</th></tr>')

def pr_row(r):
    """엔진 PR 행 하나 — 사람별 절과 추적 밖 절이 같이 쓴다. data-* 속성으로 툴바의 사람 필터·붉은 행 판정(paint)이 걸린다."""
    owner = r['tracked'][0] if r['tracked'] else None
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
    others = [a for a in r['assignees'] if a != owner]
    revs = sorted(set(r.get('requested', []) + r.get('pending_approval', []) + r.get('approvers', [])))
    pend_revs = sorted(set(r.get('requested', []) + r.get('pending_approval', [])))
    owns = sorted(set(r.get('assignees', []) + r.get('tracked', [])))   # 담당자 (이현욱 요청 2026-10-07)
    apprs = sorted(set(r.get('approvers', [])))                          # 승인한 사람
    ready = (not r['unresolved']) and bool(apprs)                         # 승인 끝·미해결 0 = 머지 대기
    via = ' · <span class="warn">작성자 기준(assignee 없음)</span>' if r.get('tracked_via') == 'author' else ''
    if r.get('tracked_via') == 'reviewer':
        via = ' · <span class="muted">추적 밖 — 리뷰어 %s</span>' % esc(', '.join(r.get('tracked_reviewers', [])))
    return '<tr class="%s" data-unres="%s" data-ready="%s" data-rev="%s" data-pend="%s" data-own="%s" data-appr="%s"><td class="n"><a href="%s">%s#%d</a></td><td>%s<div class="meta">%s · by %s%s</div></td><td class="n">%s</td><td>%s</td><td class="n col-mine">%s</td><td class="n">%s<div class="muted" style="font-size:11px">생성 %s</div></td><td>%s</td><td class="docs" style="font-size:12px">%s</td><td class="docs col-mine">%s</td></tr>' % (
        ('unres' if r['unresolved'] else ('ready' if ready else '')), '1' if r['unresolved'] else '0', '1' if ready else '0', esc(','.join(revs)), esc(','.join(pend_revs)), esc(','.join(owns)), esc(','.join(apprs)),
        esc(r['url']), esc(r['repo'].split('/')[1]), r['number'],
        esc(r['title']), esc(r['jira'] or '-'), esc(r['author']), ((' · assignees +' + ','.join(others)) if others else '') + via,
        unres, rev, ag, esc(r['updated']), esc(r['created']), ci_lights(r), tcl, docs)

def render(rows, now, orphans=(), outside=()):
    by = {}
    for r in rows:
        by.setdefault(r['tracked'][0], []).append(r)
    css = """
    :root{--bg:#eef0f3;--surface:#fff;--ink:#161a20;--muted:#6b7684;--line:#d5dae1;--gate:#14706b;--warn:#a85b16;--bad:#a3262b;--soft:#f6f7f9;--sig-ok:#2FA35B;--sig-run:#E3B505;--sig-bad:#D0453A;--sig-none:#C9D0C6}
    @media (prefers-color-scheme:dark){:root{--bg:#0e1218;--surface:#161b23;--ink:#e7ebf0;--muted:#8a95a3;--line:#2a323c;--gate:#46b3a8;--warn:#d98b45;--bad:#e06c70;--soft:#1b2128;--sig-ok:#5CB878;--sig-run:#F2C94C;--sig-bad:#E07B6E;--sig-none:#4A5249}}
    /* TC 신호등 (agent-todo 보드의 agent-board-style.css 와 같은 꼴) */
    .lights{display:inline-flex;gap:8px;text-decoration:none;color:var(--muted)}.light{display:inline-flex;align-items:center;gap:4px;font-size:11px;font-family:"IBM Plex Mono",monospace}
    .light i{width:9px;height:9px;border-radius:50%;background:var(--sig-none)}.light-ok i{background:var(--sig-ok)}.light-run i{background:var(--sig-run)}.light-bad i{background:var(--sig-bad)}.light-skip{color:var(--sig-bad);font-weight:600}.light-skip i{background:var(--sig-bad);border-radius:2px}
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
    h.append('<h1>review-to-do</h1><div class="meta">스냅샷 %s · assignee(없으면 작성자) ∈ {%s} · open · non-draft · 미해결 스레드는 리뷰 코멘트 기준 · TC 테스트 = gha-ci test_sql·medium·shell·sql_by_cci(cci)(헤드 커밋) · TC PR 은 JIRA 키/cubrid#n 으로 엔진 PR 하위에 연동 · 추적 밖이라도 추적 ID 가 리뷰어인 PR 은 맨 아래 따로</div>' % (now, ', '.join(TRACKED)))
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
    for r in list(rows) + list(outside):   # 추적 밖 PR 은 tracked 가 비어 '내가 리뷰어' 에만 든다(2026-11-24)
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
        h.append(PR_TABLE_HEAD)
        for r in rs:
            h.append(pr_row(r))
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
    if outside:
        # 추적 밖 PR — 작성자·담당자가 추적 ID 가 아니어도 추적 ID 가 리뷰어인 것 (사용자 지시 2026-11-24 "팀원거 아니어도 밑에 따로")
        h.append('<section class="who"><h2>추적 밖 PR — 추적 ID 가 리뷰어 <span class="meta">%d PR</span></h2>' % len(outside))
        h.append('<div class="meta" style="margin:-4px 0 8px">작성자·담당자는 추적 ID 가 아니지만 추적 ID 가 리뷰어(요청됨·리뷰함)인 open·non-draft PR · 머리의 미완료 PR 수에는 넣지 않고 사람별 표의 내가 리뷰어에는 넣는다</div>')
        h.append(PR_TABLE_HEAD)
        for r in outside:
            h.append(pr_row(r))
        h.append('</table></section>')
    h.append(render_headless())
    h.append(TOOLBAR_JS)
    h.append('<p class="meta" style="margin-top:26px">생성기 dev4-review-workspace/tools/review_board_gen.py · 갱신 규약 skills/review-board/SKILL.md · 추적 ID 는 tools/roster.json</p></div></body></html>')
    return '\n'.join(h)

if __name__ == '__main__':
    main()
