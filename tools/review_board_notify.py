#!/usr/bin/env python3
"""review-to-do 보드 변화 알림 — 추적 PR 에 새 코멘트·답글·리뷰·리뷰 요청·스레드 해결이 생기면 해당자에게 Teams 채널 알림.
   발신자는 사용자가 아니라 채널 Webhook(워크플로) — 규약: memory rules/메일-발신-금지 의 '리뷰 보드 봇 예외'.
   사용: review_board_notify.py [--dry]      (--dry 는 보내지 않고 로그만, 상태는 갱신하지 않는다)
   웹훅 URL: ~/.config/review-board/teams_webhook (chmod 600) 또는 환경변수 REVIEW_BOARD_TEAMS_WEBHOOK. 없으면 드라이런(로그만, 상태 갱신).
   상태: ~/dev/utils/review-board/state/notify_state.json (site/ 밖 — 웹으로 노출하지 않는다)
   첫 실행은 기준선만 잡고 알리지 않는다. 새로 잡힌 PR 은 리뷰 요청만 알린다.
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


def events(key, d, old, first_seen):
    """[(수신자 login, 한 줄, url)] — old 에 없던 것만."""
    out = []
    assignees = [a['login'] for a in d['assignees']['nodes']]
    oc, orr, oi, oq, ot = set(old['c']), set(old['r']), set(old['i']), set(old['q']), set(old['t'])
    cur_q = [n['requestedReviewer']['login'] for n in d['reviewRequests']['nodes'] if n.get('requestedReviewer')]
    for who in cur_q:
        if who not in oq and human(who):
            out.append((who, '리뷰 요청을 받았습니다', None))
    if first_seen:
        return out
    for t in d['reviewThreads']['nodes']:
        cs = t['comments']['nodes']
        for i, c in enumerate(cs):
            a = (c.get('author') or {}).get('login')
            if c['id'] in oc or not human(a):
                continue
            prior = {(p.get('author') or {}).get('login') for p in cs[:i]}
            if i == 0:
                rec, what = set(assignees), '새 리뷰 코멘트를'
            else:
                rec, what = set(assignees) | prior, '답글을'
            for who in sorted(r for r in rec if r and r != a and human(r)):
                out.append((who, '%s님이 %s 남겼습니다: “%s”' % (G_name(a), what, snip(c['body'])), c['url']))
        if t['isResolved'] and t['id'] not in ot:
            rb = (t.get('resolvedBy') or {}).get('login')
            first = (cs[0].get('author') or {}).get('login') if cs else None
            if human(first) and first != rb:
                out.append((first, '남기신 스레드를 %s님이 해결 처리했습니다' % G_name(rb or '?'), cs[0]['url']))
    for r in d['reviews']['nodes']:
        a = (r.get('author') or {}).get('login')
        if r['id'] in orr or not human(a):
            continue
        if r['state'] == 'COMMENTED' and not (r.get('body') or '').strip():
            continue                                    # 인라인 코멘트의 껍데기 — 코멘트 이벤트가 따로 있다
        label = {'APPROVED': '승인했습니다', 'CHANGES_REQUESTED': '변경을 요청했습니다', 'COMMENTED': '리뷰 의견을 남겼습니다',
                 'DISMISSED': '리뷰가 기각됐습니다'}.get(r['state'], r['state'])
        for who in assignees:
            if who != a and human(who):
                out.append((who, '%s님이 %s' % (G_name(a), label) + (': “%s”' % snip(r['body']) if (r.get('body') or '').strip() else ''), r['url']))
    for c in d['comments']['nodes']:
        a = (c.get('author') or {}).get('login')
        if c['id'] in oi or not human(a):
            continue
        for who in assignees:
            if who != a and human(who):
                out.append((who, '%s님이 PR 대화에 글을 남겼습니다: “%s”' % (G_name(a), snip(c['body'])), c['url']))
    return out


def G_name(login):
    return (TEAMS.get(login) or {}).get('name') or login


def card(items):
    """items: {login: [(PR 제목줄, 한 줄, url)]} → Teams Adaptive Card payload."""
    body, ents = [{'type': 'TextBlock', 'text': '리뷰 보드 알림', 'weight': 'Bolder', 'size': 'Medium'}], []
    for who in sorted(items):
        t = TEAMS.get(who) or {}
        if t.get('email'):
            tag = '<at>%s</at>' % G_name(who)
            ents.append({'type': 'mention', 'text': tag, 'mentioned': {'id': t['email'], 'name': G_name(who)}})
        else:
            tag = '**%s**' % G_name(who)
        lines = items[who][:MAX_LINES_PER_PERSON]
        more = len(items[who]) - len(lines)
        md = ['%s' % tag]
        for head, line, url in lines:
            md.append('- %s — %s%s' % (head, line, (' ([보기](%s))' % url) if url else ''))
        if more > 0:
            md.append('- … 외 %d건' % more)
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
        if old_all is None:
            continue
        head = '#%d %s' % (p['number'], snip(p['title'], 40))
        if p['repo'] != 'CUBRID/cubrid':
            head = '[TC] ' + head
        for who, line, url in events(key, d, old_all.get(key) or {'c': [], 'r': [], 'i': [], 'q': [], 't': []}, key not in old_all):
            items.setdefault(who, []).append((head, line, url or p['url']))
    n = sum(len(v) for v in items.values())
    if old_all is None:
        log('baseline: %d PRs, no notifications' % len(new_all))
    elif n:
        payload = card(items)
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
                for head, line, _ in items[who]:
                    log('   -> %s | %s — %s' % (who, head, line))
    else:
        log('no changes (%d PRs)' % len(new_all))
    if not DRY:
        json.dump(new_all, open(STATE + '.tmp', 'w', encoding='utf-8'))
        os.replace(STATE + '.tmp', STATE)


if __name__ == '__main__':
    main()
