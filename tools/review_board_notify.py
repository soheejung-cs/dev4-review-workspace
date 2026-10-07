#!/usr/bin/env python3
"""review-to-do 보드 변화 알림 — 추적 PR 에 새 코멘트·답글·리뷰·리뷰 요청·스레드 해결이 생기면 해당자에게 Teams 채널 알림.
   발신자는 사용자가 아니라 채널 Webhook(워크플로) — 규약: memory rules/메일-발신-금지 의 '리뷰 보드 봇 예외'.
   사용: review_board_notify.py [--dry]      (--dry 는 보내지 않고 로그만, 상태는 갱신하지 않는다)
   웹훅 URL: ~/.config/review-board/teams_webhook (chmod 600) 또는 환경변수 REVIEW_BOARD_TEAMS_WEBHOOK. 없으면 드라이런(로그만, 상태 갱신).
   상태: ~/dev/utils/review-board/state/notify_state.json (site/ 밖 — 웹으로 노출하지 않는다)
   첫 실행은 기준선만 잡고 알리지 않는다. 알리는 것은 4종류(KINDS) — roster.json 의 notify_events 로 줄일 수 있다.
"""
import sys, os, re, json, datetime, subprocess, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import review_board_gen as G

STATE_DIR = os.path.expanduser('~/dev/utils/review-board/state')
STATE = os.path.join(STATE_DIR, 'notify_state.json')
LOG = os.path.join(STATE_DIR, 'notify.log')
HOOK_FILE = os.path.expanduser('~/.config/review-board/teams_webhook')
TEAMS = G.ROSTER.get('teams', {})          # {github_login: {"name": "표시이름", "email": "UPN"}}
MAX_LINES_PER_PERSON = 8
DRY = '--dry' in sys.argv
SIM = '--sim' in sys.argv        # 현재 상태 전부를 '새 변화'로 보고 보낸다(시험 전송 — 상태는 건드리지 않는다)
DIGEST = '--digest' in sys.argv  # 하루 한 번 정리(리뷰 안 한 PR · 머지 안 한 PR) — 상태와 무관

Q = '''query($o:String!,$n:String!,$num:Int!){repository(owner:$o,name:$n){pullRequest(number:$num){
  assignees(first:10){nodes{login}}
  reviewRequests(first:30){nodes{requestedReviewer{... on User{login}}}}
  reviewThreads(first:100){nodes{id isResolved resolvedBy{login}
    comments(first:50){nodes{id author{login} createdAt body url}}}}
  reviews(last:50){nodes{id author{login} state body url}}
  comments(last:30){nodes{id author{login} body url}}
}}}'''


def log(msg):
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write('%s %s\n' % (datetime.datetime.now().strftime('%F %T'), msg))


def fetch(repo, num):
    o, n = repo.split('/')
    js = G.sh('gh', 'api', 'graphql', '-f', 'query=' + Q, '-F', 'o=' + o, '-F', 'n=' + n, '-F', 'num=%d' % num)
    return json.loads(js)['data']['repository']['pullRequest']


def human(login):
    return bool(login) and login in G.TRACKED


def snip(body, n=90):
    s = re.sub(r'\s+', ' ', (body or '')).strip()
    return s if len(s) <= n else s[:n] + '…'


def snapshot(d):
    """PR 의 현재 상태를 '본 것' 집합으로 줄인다."""
    s = {'c': [], 'r': [], 'i': [], 'q': [], 't': []}
    for t in d['reviewThreads']['nodes']:
        s['c'] += [c['id'] for c in t['comments']['nodes']]
        if t['isResolved']:
            s['t'].append(t['id'])
    s['r'] = [r['id'] for r in d['reviews']['nodes']]
    s['i'] = [c['id'] for c in d['comments']['nodes']]
    s['q'] = sorted(n['requestedReviewer']['login'] for n in d['reviewRequests']['nodes'] if n.get('requestedReviewer'))
    return s


KINDS = [   # (코드, 카드에 쓰는 문구) — 사용자 지정 문구 (2026-10-07)
    ('review_request', '새로 리뷰해야 할 것이 추가되었습니다.'),
    ('new_pr', '새 PR 이 게시되었습니다.'),
    ('reply_resolve', '리뷰한 코멘트에 답글이 달렸습니다. Resolve 를 부탁드려요.'),
    ('reply', '리뷰에 답글이 게시되었습니다.'),
]
KIND_TEXT = dict(KINDS)
ENABLED = set(G.ROSTER.get('notify_events') or [k for k, _ in KINDS])


def events(key, d, old, first_seen):
    """[(수신자 login, 종류, 상세 한 줄, url)] — old 에 없던 것만."""
    out = []
    assignees = [a['login'] for a in d['assignees']['nodes']]
    oc, oq, ot = set(old['c']), set(old['q']), set(old['t'])
    cur_q = [n['requestedReviewer']['login'] for n in d['reviewRequests']['nodes'] if n.get('requestedReviewer')]
    for who in cur_q:
        if human(who) and (first_seen or who not in oq):
            out.append((who, 'new_pr' if first_seen else 'review_request', '', None))
    if first_seen:
        return out
    for t in d['reviewThreads']['nodes']:
        if t['isResolved']:
            continue                                   # 이미 해결된 스레드의 답글은 Resolve 요청 대상이 아니다
        cs = t['comments']['nodes']
        opener = (cs[0].get('author') or {}).get('login') if cs else None
        for i, c in enumerate(cs):
            a = (c.get('author') or {}).get('login')
            if i == 0 or c['id'] in oc or not human(a):
                continue                               # 첫 글(새 코멘트)은 알리지 않는다 — 답글만
            detail = '%s님: “%s”' % (G_name(a), snip(c['body']))
            if opener and opener != a and human(opener):
                out.append((opener, 'reply_resolve', detail, c['url']))   # 내가 연 스레드에 남이 답했다 → Resolve 요청
            prior = {(p.get('author') or {}).get('login') for p in cs[:i]} | set(assignees)
            for who in sorted(r for r in prior if r and r != a and r != opener and human(r)):
                out.append((who, 'reply', detail, c['url']))
            if a == opener:                            # 스레드를 연 리뷰어가 다시 답했다 → 작성자(담당자)에게
                for who in assignees:
                    if who != a and human(who):
                        out.append((who, 'reply', detail, c['url']))
    seen, uniq = set(), []
    for e in out:                                  # 같은 사람·종류·링크는 한 번만
        if e[1] in ENABLED and (e[0], e[1], e[3]) not in seen:
            seen.add((e[0], e[1], e[3])); uniq.append(e)
    return uniq


def G_name(login):
    return (TEAMS.get(login) or {}).get('name') or login


BOARD_URL = 'http://192.168.6.51:8827/'


def card(items, title='리뷰 보드 알림'):
    """items: {login: [(PR 제목줄, 종류, 상세, url, PR url)]} → 사람마다 종류별 'N건' 한 줄만 담은 Adaptive Card."""
    body = [{'type': 'TextBlock', 'text': '%s — [%s](%s)' % (title, BOARD_URL, BOARD_URL), 'weight': 'Bolder', 'wrap': True}]
    ents = []
    for who in sorted(items):
        t = TEAMS.get(who) or {}
        if t.get('email'):
            tag = '<at>%s</at>' % G_name(who)
            ents.append({'type': 'mention', 'text': tag, 'mentioned': {'id': t['email'], 'name': G_name(who)}})
        else:
            tag = '**%s**' % G_name(who)
        md = [tag]
        for kind, text in KINDS:
            rows = [r for r in items[who] if r[1] == kind]
            if not rows:
                continue
            prs = []                                    # PR 별로 묶고 순서 유지
            for head, _k, _d, _u, pr_url in rows:
                num = head.split('#', 1)[1].split()[0]
                tc = head.startswith('[TC]')
                for e in prs:
                    if e[0] == pr_url:
                        e[2] += 1; break
                else:
                    prs.append([pr_url, ('TC#' if tc else '#') + num, 1])
            links = ' · '.join('[%s](%s)' % (n, u) + ('×%d' % c if c > 1 else '') for u, n, c in prs)
            md.append('- %s **%d건** (%s)' % (text, len(rows), links))
        body.append({'type': 'TextBlock', 'text': '\n'.join(md), 'wrap': True})
    content = {'$schema': 'http://adaptivecards.io/schemas/adaptive-card.json', 'type': 'AdaptiveCard', 'version': '1.4', 'body': body}
    if ents:
        content['msteams'] = {'entities': ents}
    return {'type': 'message', 'attachments': [{'contentType': 'application/vnd.microsoft.card.adaptive', 'content': content}]}


def digest_card():
    """board.json(보드 갱신 데몬이 10분마다 쓴다)에서 사람마다 '리뷰하지 않은 PR' · '머지하지 않은 PR' 링크 모음."""
    b = json.load(open(os.path.expanduser('~/dev/utils/review-board/site/board.json'), encoding='utf-8'))
    prs = b['prs']
    def lk(r):
        return '[#%d](%s) %s' % (r['number'], r['url'], snip(r['title'], 50))
    body = [{'type': 'TextBlock', 'text': '리뷰 보드 일일 정리 — [%s](%s)' % (BOARD_URL, BOARD_URL), 'weight': 'Bolder', 'wrap': True}]
    ents = []
    for who in sorted(G.TRACKED):
        unreviewed = [r for r in prs if who in r['requested']]                       # 요청됐고 리뷰 이력 0
        unmerged = [r for r in prs if who in r['assignees']]                         # 내 PR 중 머지 전(보드 대상은 open·non-draft)
        if not unreviewed and not unmerged:
            continue
        t = TEAMS.get(who) or {}
        if t.get('email'):
            tag = '<at>%s</at>' % G_name(who)
            ents.append({'type': 'mention', 'text': tag, 'mentioned': {'id': t['email'], 'name': G_name(who)}})
        else:
            tag = '**%s**' % G_name(who)
        md = [tag]
        if unreviewed:
            md.append('- 리뷰하지 않은 PR **%d건**' % len(unreviewed))
            md += ['  - ' + lk(r) for r in unreviewed]
        if unmerged:
            md.append('- 머지하지 않은 PR **%d건**' % len(unmerged))
            for r in unmerged:
                st = '미해결 %d' % r['unresolved'] if r['unresolved'] else ('승인 %d' % len(r['approvers']) if r['approvers'] else '승인 대기')
                md.append('  - %s — %s' % (lk(r), st))
        body.append({'type': 'TextBlock', 'text': '\n'.join(md), 'wrap': True})
    content = {'$schema': 'http://adaptivecards.io/schemas/adaptive-card.json', 'type': 'AdaptiveCard', 'version': '1.4', 'body': body}
    if ents:
        content['msteams'] = {'entities': ents}
    return {'type': 'message', 'attachments': [{'contentType': 'application/vnd.microsoft.card.adaptive', 'content': content}]}


def webhook():
    u = os.environ.get('REVIEW_BOARD_TEAMS_WEBHOOK')
    if not u and os.path.exists(HOOK_FILE):
        u = open(HOOK_FILE).read().strip()
    return u or None


def post(url, payload):
    req = urllib.request.Request(url, json.dumps(payload).encode('utf-8'), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status


def main():
    os.makedirs(STATE_DIR, exist_ok=True)
    if DIGEST:
        payload, url = digest_card(), webhook()
        if DRY or not url:
            print(json.dumps(payload, ensure_ascii=False, indent=1)); return
        log('digest sent HTTP %s' % post(url, payload)); return
    old_all = json.load(open(STATE, encoding='utf-8')) if os.path.exists(STATE) else None
    prs = []
    for repo in G.REPOS:
        prs += G.list_prs(repo)
    new_all, items = {}, {}
    for p in prs:
        key = '%s#%d' % (p['repo'], p['number'])
        try:
            d = fetch(p['repo'], p['number'])
        except Exception as e:                      # 한 PR 의 조회 실패가 전체를 막지 않게 — 그 PR 은 이전 상태를 유지
            log('fetch failed %s: %s' % (key, e))
            if old_all and key in old_all:
                new_all[key] = old_all[key]
            continue
        snap = snapshot(d)
        new_all[key] = snap
        if old_all is None and not SIM:
            continue
        head = '#%d %s' % (p['number'], snip(p['title'], 40))
        if p['repo'] != 'CUBRID/cubrid':
            head = '[TC] ' + head
        empty = {'c': [], 'r': [], 'i': [], 'q': [], 't': []}
        evs = events(key, d, old_all.get(key) or empty, key not in old_all)
        if SIM:                                    # 현재 상태 전부를 새 변화로: 새 PR + 리뷰 요청 + 답글류
            evs += events(key, d, empty, True) + events(key, d, empty, False)
        seen_e = set()
        for who, kind, detail, url in evs:
            if (who, kind, url) in seen_e:
                continue
            seen_e.add((who, kind, url))
            items.setdefault(who, []).append((head, kind, detail, url or p['url'], p['url']))
    n = sum(len(v) for v in items.values())
    if old_all is None and not SIM:
        log('baseline: %d PRs, no notifications' % len(new_all))
    elif n:
        payload = card(items, '리뷰 보드 알림 [시험 전송: 현재 상태 전체를 새 변화로 간주]' if SIM else '리뷰 보드 알림')
        url = webhook()
        summary = '; '.join('%s×%d' % (w, len(v)) for w, v in sorted(items.items()))
        if DRY:
            print(json.dumps(payload, ensure_ascii=False, indent=1))
            log('DRY-RUN (state not advanced) %d events: %s' % (n, summary))
            return
        if url:
            try:
                st = post(url, payload)
            except Exception as e:
                log('post failed (state not advanced, will retry next cycle): %s' % e)
                return
            log('sent HTTP %s, %d events: %s' % (st, n, summary))
        else:
            log('NO WEBHOOK — would send %d events: %s' % (n, summary))
            for who in sorted(items):
                for head, kind, detail, _u, _p in items[who]:
                    log('   -> %s | %s | %s %s' % (who, kind, head, detail))
    else:
        log('no changes (%d PRs)' % len(new_all))
    if not DRY and not SIM:
        json.dump(new_all, open(STATE + '.tmp', 'w', encoding='utf-8'))
        os.replace(STATE + '.tmp', STATE)


if __name__ == '__main__':
    main()
