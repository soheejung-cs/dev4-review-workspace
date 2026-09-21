#!/usr/bin/env python3
"""review_recommend.py — PR 에 맞는 리뷰어를 추천한다 (/review-recommend).

  python3 tools/review_recommend.py <PR번호> [--repo CUBRID/cubrid] [--months 18] [--refresh] [--json] [--pool a,b,c]

세 축을 합친다 (skills/review-recommend/SKILL.md 에 계산식):
  1. 관심도  — 머지된 PR 인덱스(작성자·리뷰어·모듈만, 파일 목록은 저장하지 않음)와 대상 PR 의 모듈 겹침.
               ~/dev/utils/review-recommend/merged_index.json — **한 번만 수집, 자동 갱신 없음**(사용자 지시 2026-09-17).
               다시 받으려면 --refresh.
  2. 난이도  — 변경 규모·파일/모듈 수·어려운 영역·제목 키워드 → 간단(1명) / 보통(2명) / 어려움(3명).
               2명 이상이면 한 자리는 확률(learner_prob)로 '학습 슬롯' — 그 모듈 이해가 낮은 사람(성능 개선팀 학습 목적).
  3. 부하    — review-to-do 보드(board.json, 10분 주기)에서 후보별 대기 요청·진행 중 리뷰 수. 보드가 낡았으면 GitHub 직접 조회.
추천만 한다 — reviewer 지정(gh pr edit --add-reviewer) 은 사람이 한다.
python 3.6 호환(이 컨테이너 기본).
"""
import argparse, datetime, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROSTER = os.path.join(HERE, 'roster.json')
CACHE_DIR = os.path.expanduser('~/dev/utils/review-recommend')
INDEX = os.path.join(CACHE_DIR, 'merged_index.json')
BOARD_JSON = os.path.expanduser('~/dev/utils/review-board/site/board.json')
BOTS = ('greptile-apps', 'cubrid-submodule-bot', 'github-actions', 'dependabot')

DEFAULT_CFG = {
    # 난이도 점수를 올리는 디렉터리(접두)·파일 키워드·제목 키워드
    'hard_dirs': ['src/storage', 'src/transaction', 'src/query', 'src/optimizer', 'src/thread', 'src/connection',
                  'src/communication', 'src/replication', 'src/parser'],
    'hard_files': ['btree', 'heap', 'log_', 'lock_', 'mvcc', 'page_buffer', 'pgbuf', 'xasl', 'scan', 'vacuum'],
    'hard_title': ['refactor', 'parallel', 'lock', 'mvcc', 'deadlock', 'latch', 'crash', 'concurren', 'recovery',
                   'race', 'barrier', 'atomic', 'volatile', '동시', '교착', '경쟁'],
    'easy_title': ['typo', 'backport', 'doc', 'comment', 'message', 'rename', '오타', '주석', '메시지'],
    # 종합 점수 가중치와 부하 공평성
    'w_interest': 0.6, 'w_load': 0.4,
    'load_pending': 1.0, 'load_in_progress': 0.5,
    'fair_margin': 2.0,         # 부하가 평균 + margin 이상이면 후보에서 뒤로 뺀다(대안이 있을 때)
    'recency_half_life_months': 12.0,
    'w_author': 1.0, 'w_reviewer': 0.6,
    'learner_prob': 0.5,        # 리뷰어 2명 이상일 때 마지막 자리를 학습 슬롯으로 돌릴 확률(PR 번호 시드 → 재실행 동일)
    'tiers': [(2, '간단', 1), (5, '보통', 2), (999, '어려움', 3)],   # 난이도 점수 상한, 이름, 리뷰어 수
}


def d_iso(s):
    return datetime.datetime.strptime(s[:10], '%Y-%m-%d').date()


def sh(*a):
    return subprocess.check_output(a, universal_newlines=True)


def gql(query, **vars):
    cmd = ['gh', 'api', 'graphql', '-f', 'query=' + query]
    for k, v in vars.items():
        if v is not None:
            cmd += ['-f', '%s=%s' % (k, v)]
    return json.loads(sh(*cmd))


def load_roster():
    r = json.load(open(ROSTER, encoding='utf-8'))
    cfg = dict(DEFAULT_CFG)
    cfg.update(r.get('recommend', {}))
    return r, cfg


def is_bot(login):
    return not login or login.endswith('[bot]') or login in BOTS


# ---------------------------------------------------------------- 1. 머지 PR 인덱스
INDEX_Q = '''query($q:String!, $after:String){ search(query:$q, type:ISSUE, first:50, after:$after){
  issueCount pageInfo{hasNextPage endCursor}
  nodes{ ... on PullRequest { number title mergedAt additions deletions changedFiles author{login}
    files(first:100){nodes{path}} reviews(first:30){nodes{author{login} state}} } } } }'''


def month_windows(since, until):
    d = since
    while d < until:
        nxt = (d.replace(day=1) + datetime.timedelta(days=32)).replace(day=1)
        yield d, min(nxt - datetime.timedelta(days=1), until)
        d = nxt


def fetch_merged(repo, since, until, log=sys.stderr):
    out = {}
    for a, b in month_windows(since, until):
        q = 'repo:%s is:pr is:merged merged:%s..%s' % (repo, a.isoformat(), b.isoformat())
        after = None
        while True:
            d = gql(INDEX_Q, q=q, after=after)['data']['search']
            for n in d['nodes']:
                if not n or 'number' not in n:
                    continue
                revs = {}
                for r in n['reviews']['nodes']:
                    a_ = (r.get('author') or {}).get('login')
                    if not is_bot(a_):
                        revs[a_] = r['state']
                out[str(n['number'])] = {
                    'title': n['title'][:60], 'merged': n['mergedAt'][:10],
                    'author': (n.get('author') or {}).get('login') or '?',
                    'mods': sorted(set(dir_of(f['path']) for f in n['files']['nodes'])), 'reviewers': revs}
            if d['issueCount'] > 1000:
                print('warn: window %s..%s has %d > 1000 results (search cap)' % (a, b, d['issueCount']), file=log)
            if not d['pageInfo']['hasNextPage']:
                break
            after = d['pageInfo']['endCursor']
    print('  index: %d merged PRs' % len(out), file=log)
    return out


def load_index(repo, months, refresh):
    """캐시가 있으면 그대로 쓴다 — 증분·자동 갱신 없음. 없거나 --refresh 일 때만 수집."""
    if not refresh and os.path.exists(INDEX):
        idx = json.load(open(INDEX, encoding='utf-8'))
        if idx.get('repo') == repo:
            return idx
    today = datetime.date.today()
    since = today - datetime.timedelta(days=int(months * 30.4))
    print('building merged-PR index %s since %s (once) …' % (repo, since), file=sys.stderr)
    prs = fetch_merged(repo, since, today)
    idx = {'repo': repo, 'since': since.isoformat(), 'generated': today.isoformat(), 'prs': prs}
    os.makedirs(CACHE_DIR, exist_ok=True)
    json.dump(idx, open(INDEX, 'w', encoding='utf-8'), ensure_ascii=False)
    return idx


# ---------------------------------------------------------------- 대상 PR
PR_Q = '''query($owner:String!,$name:String!){repository(owner:$owner,name:$name){pullRequest(number:%d){
  number title isDraft additions deletions changedFiles author{login} labels(first:20){nodes{name}}
  files(first:100){nodes{path additions deletions}}
  reviewRequests(first:30){nodes{requestedReviewer{... on User{login}}}}
  latestReviews(first:30){nodes{author{login} state}} }}}'''


def fetch_pr(repo, num):
    owner, name = repo.split('/')
    d = gql(PR_Q % num, owner=owner, name=name)
    p = d['data']['repository']['pullRequest']
    return {
        'number': p['number'], 'title': p['title'], 'draft': p['isDraft'],
        'author': (p.get('author') or {}).get('login') or '?',
        'add': p['additions'], 'del': p['deletions'], 'nfiles': p['changedFiles'],
        'labels': [l['name'] for l in p['labels']['nodes']],
        'files': [f['path'] for f in p['files']['nodes']],
        'requested': [(n.get('requestedReviewer') or {}).get('login') for n in p['reviewRequests']['nodes']],
        'reviewed': {(n.get('author') or {}).get('login'): n['state'] for n in p['latestReviews']['nodes'] if n.get('author')},
    }


def dir_of(path, depth=2):
    parts = path.split('/')
    return '/'.join(parts[:depth]) if len(parts) > depth else '/'.join(parts[:-1]) or '.'


# ---------------------------------------------------------------- 2. 난이도
def _kw_hits(text, keys):
    """제목 키워드 매칭. ASCII 키워드는 **단어 경계**로 본다 — 부분문자열로 보면
    `lock` 이 `CLOCK_MONOTONIC`·`string block search` 에, `comment` 가 "post a PR comment" 에 걸린다.
    실측(2026-09-21, 머지 PR 100건): 제목 키워드 ±1 하나가 tier 를 바꾼 PR 이 8건이었다.
    한글 키워드는 단어 경계 개념이 없으므로 부분 일치를 유지한다."""
    hits = []
    for k in keys:
        if all(ord(ch) < 128 for ch in k):   # 3.6 호환 (str.isascii 는 3.7+)
            if re.search(r'(?<![a-z0-9])%s(?![a-z0-9])' % re.escape(k), text): hits.append(k)
        elif k in text:
            hits.append(k)
    return hits

def _hard_file_hits(path, keys):
    """파일 키워드. `log_`·`lock_` 처럼 밑줄로 끝나는 키는 **basename 접두**로만 본다 —
    부분문자열이면 `catalog_class.c` 가 `log_` 에 걸린다(실측). 나머지는 토큰 경계."""
    b = os.path.basename(path).lower()
    out = []
    for k in keys:
        if k.endswith('_'):
            if b.startswith(k): out.append(k)
        elif re.search(r'(?:^|[_.\-])%s(?:[_.\-]|$)' % re.escape(k), b):
            out.append(k)
    return out

def difficulty(pr, cfg):
    size = pr['add'] + pr['del']
    dirs = set(dir_of(f) for f in pr['files'])
    pts, why = 0, []
    s = 0 if size < 50 else 1 if size < 200 else 2 if size < 600 else 3
    pts += s; why.append('변경 %d줄(+%d/−%d) → %d' % (size, pr['add'], pr['del'], s))
    f = 0 if pr['nfiles'] < 3 else 1 if pr['nfiles'] < 8 else 2
    pts += f; why.append('파일 %d개 → %d' % (pr['nfiles'], f))
    d = 0 if len(dirs) <= 1 else 1 if len(dirs) == 2 else 2
    pts += d; why.append('디렉터리 %d개 → %d' % (len(dirs), d))
    hard_hit = sorted(set(dd for dd in dirs for h in cfg['hard_dirs'] if dd.startswith(h)))
    hf = sorted(set(os.path.basename(fp) for fp in pr['files'] if _hard_file_hits(fp, cfg['hard_files'])))
    if hard_hit or hf:
        pts += 2; why.append('어려운 영역 %s → 2' % ', '.join((hard_hit + hf)[:4]))
    t = pr['title'].lower()
    hk = _kw_hits(t, cfg['hard_title'])
    if hk:
        pts += 1; why.append('제목 키워드(+1): ' + ', '.join(hk))
    ek = _kw_hits(t, cfg['easy_title'])
    if ek:
        pts -= 1; why.append('제목 키워드(−1): ' + ', '.join(ek))
    pts = max(0, pts)
    for cap, name, n in cfg['tiers']:
        if pts <= cap:
            return {'points': pts, 'tier': name, 'n_reviewers': n, 'why': why, 'dirs': sorted(dirs)}
    return {'points': pts, 'tier': '어려움', 'n_reviewers': 3, 'why': why, 'dirs': sorted(dirs)}


# ---------------------------------------------------------------- 1. 관심도
def interest(pr, idx, pool, cfg):
    tmods = set(dir_of(f) for f in pr['files'])
    today = datetime.date.today()
    hl = cfg['recency_half_life_months']
    res = {c: {'score': 0.0, 'authored': 0, 'reviewed': 0, 'mods_hit': set(), 'top': []} for c in pool}
    for num, m in idx['prs'].items():
        hit = set(m['mods']) & tmods
        if not hit:
            continue
        sim = len(hit) / float(len(tmods) or 1)
        age_m = (today - d_iso(m['merged'])).days / 30.4
        w_t = 0.5 ** (age_m / hl)
        for c in pool:
            if m['author'] == c:
                role = cfg['w_author']; res[c]['authored'] += 1
            elif c in m['reviewers']:
                role = cfg['w_reviewer']; res[c]['reviewed'] += 1
            else:
                continue
            gain = role * sim * w_t
            res[c]['score'] += gain
            res[c]['mods_hit'] |= hit
            res[c]['top'].append((gain, int(num), m['title'], 'author' if m['author'] == c else 'review'))
    for c in pool:
        res[c]['top'] = sorted(res[c]['top'], reverse=True)[:2]
        res[c]['mods_hit'] = sorted(res[c]['mods_hit'])
    return res


# ---------------------------------------------------------------- 3. 부하
def load_from_board(pool, cfg):
    if not os.path.exists(BOARD_JSON):
        return None, 'board.json 없음'
    b = json.load(open(BOARD_JSON, encoding='utf-8'))
    gen = datetime.datetime.strptime(b['generated'][:16], '%Y-%m-%d %H:%M')
    age_min = (datetime.datetime.now() - gen).total_seconds() / 60
    if age_min > 90:
        return None, 'board.json 이 %d분 전 — 데몬 확인 필요' % age_min
    prs = []
    def walk(p):
        prs.append(p)
        for t in p.get('linked_tc', []):
            walk(t)
    for p in b['prs']:
        walk(p)
    for p in b.get('unlinked_tc_prs', []):
        walk(p)
    load = {c: {'pending': [], 'in_progress': [], 'own_open': 0} for c in pool}
    for p in prs:
        for c in pool:
            if p['author'] == c:
                load[c]['own_open'] += 1; continue
            key = '%s#%d' % (p['repo'].split('/')[-1], p['number'])
            if c in p.get('requested', []):
                load[c]['pending'].append(key)
            elif any(r[0] == c and r[1] != 'APPROVED' for r in p.get('latest_reviews', [])):
                load[c]['in_progress'].append(key)
    for c in pool:
        load[c]['score'] = cfg['load_pending'] * len(load[c]['pending']) + cfg['load_in_progress'] * len(load[c]['in_progress'])
    return load, 'board.json %s (%d개 PR)' % (b['generated'], len(prs))


LOAD_Q = '''query($q:String!){ search(query:$q, type:ISSUE, first:100){ nodes{ ... on PullRequest {
  number repository{name} author{login} reviewRequests(first:30){nodes{requestedReviewer{... on User{login}}}}
  latestReviews(first:30){nodes{author{login} state}} } } } }'''


def load_from_github(pool, repos, cfg):
    load = {c: {'pending': [], 'in_progress': [], 'own_open': 0} for c in pool}
    for repo in repos:
        d = gql(LOAD_Q, q='repo:%s is:pr is:open draft:false' % repo)['data']['search']['nodes']
        for p in d:
            if not p:
                continue
            author = (p.get('author') or {}).get('login')
            req = [(n.get('requestedReviewer') or {}).get('login') for n in p['reviewRequests']['nodes']]
            lat = [((n.get('author') or {}).get('login'), n['state']) for n in p['latestReviews']['nodes']]
            key = '%s#%d' % (p['repository']['name'], p['number'])
            for c in pool:
                if author == c:
                    load[c]['own_open'] += 1; continue
                if c in req:
                    load[c]['pending'].append(key)
                elif any(a == c and s != 'APPROVED' for a, s in lat):
                    load[c]['in_progress'].append(key)
    for c in pool:
        load[c]['score'] = cfg['load_pending'] * len(load[c]['pending']) + cfg['load_in_progress'] * len(load[c]['in_progress'])
    return load, 'GitHub 직접 조회(open·non-draft, %s)' % ', '.join(repos)


# ---------------------------------------------------------------- 종합
def recommend(pr, diff, inter, load, pool, cfg):
    cands = [c for c in pool if c != pr['author']]
    max_i = max([inter[c]['score'] for c in cands] + [1e-9])
    loads = [load[c]['score'] for c in cands]
    mean_l = sum(loads) / float(len(loads) or 1)
    max_l = max(loads + [1e-9])
    rows = []
    for c in cands:
        i_n = inter[c]['score'] / max_i
        l_n = load[c]['score'] / max_l if max_l > 0 else 0.0
        total = cfg['w_interest'] * i_n + cfg['w_load'] * (1.0 - l_n)
        over = load[c]['score'] >= mean_l + cfg['fair_margin']
        rows.append({'login': c, 'interest': inter[c]['score'], 'interest_n': i_n, 'load': load[c]['score'],
                     'load_n': l_n, 'total': total, 'overloaded': over,
                     'already': 'requested' if c in pr['requested'] else ('reviewed:' + pr['reviewed'][c] if c in pr['reviewed'] else '')})
    rows.sort(key=lambda r: (-r['total'], r['load'], r['login']))
    picks, deferred = [], []
    n_expert = diff['n_reviewers']
    learner = None
    if diff['n_reviewers'] >= 2 and len(cands) >= 3:
        import random
        rng = random.Random(pr['number'])          # 같은 PR 이면 같은 답
        if rng.random() < cfg['learner_prob']:
            # 학습 슬롯: 관심도 하위 절반 중 부하가 가장 낮은 사람(동률이면 관심도 더 낮은 쪽)
            by_i = sorted(rows, key=lambda r: r['interest'])
            low = by_i[:max(1, len(by_i) // 2)]
            low = [r for r in low if not r['overloaded']] or low
            learner = sorted(low, key=lambda r: (r['load'], r['interest'], r['login']))[0]
            n_expert = diff['n_reviewers'] - 1
    for r in rows:
        if learner is not None and r['login'] == learner['login']:
            continue
        if r['overloaded'] and len([x for x in rows if not x['overloaded'] and (learner is None or x['login'] != learner['login'])]) >= n_expert:
            deferred.append(r); continue
        picks.append(r)
        if len(picks) >= n_expert:
            break
    if learner is not None:
        learner = dict(learner); learner['learner'] = True
        picks.append(learner)
    return rows, picks, deferred, mean_l


def fmt_report(repo, pr, diff, inter, load, load_src, rows, picks, deferred, mean_l, idx, cfg):
    o = []
    o.append('# 리뷰어 추천 — %s#%d' % (repo, pr['number']))
    o.append('**%s** · 작성자 %s · +%d/−%d · 파일 %d%s' % (pr['title'], pr['author'], pr['add'], pr['del'], pr['nfiles'],
                                                     ' · draft' if pr['draft'] else ''))
    cur = [x for x in pr['requested'] if x] + ['%s(%s)' % kv for kv in pr['reviewed'].items() if not is_bot(kv[0])]
    if cur:
        o.append('현재 리뷰어: ' + ', '.join(cur))
    o.append('')
    o.append('## 난이도 **%s** (%d점) → 리뷰어 **%d명**' % (diff['tier'], diff['points'], diff['n_reviewers']))
    o.append('- ' + ' · '.join(diff['why']))
    o.append('- 모듈: ' + ', '.join(diff['dirs']))
    o.append('')
    o.append('## 추천: **%s**' % ', '.join('@' + p['login'] + (' (학습 슬롯)' if p.get('learner') else '') for p in picks))
    for p in picks:
        i = inter[p['login']]; l = load[p['login']]
        top = '; '.join('#%d %s(%s)' % (n, t[:36], role) for _, n, t, role in i['top'])
        tag = '학습 슬롯 — 이 모듈 이해가 낮은 사람에게 배정(learner_prob %.1f)' % cfg['learner_prob'] if p.get('learner') else '전문'
        o.append('- **%s** [%s] 관심도 %.2f (작성 %d·리뷰 %d, 겹친 모듈 %s%s) · 부하 %.1f (대기 %d·진행 %d)'
                 % (p['login'], tag, i['score'], i['authored'], i['reviewed'], ', '.join(i['mods_hit']) or '-',
                    '; 대표 ' + top if top else '', p['load'], len(l['pending']), len(l['in_progress'])))
    if deferred:
        o.append('- 부하로 뒤로 뺀 후보(평균 %.1f + %.0f 이상): %s' % (mean_l, cfg['fair_margin'],
                 ', '.join('%s(%.1f)' % (d['login'], d['load']) for d in deferred)))
    o.append('')
    o.append('| 후보 | 관심도 | 작성/리뷰 | 부하 | 대기 PR | 종합 | 현재 |')
    o.append('|---|---|---|---|---|---|---|')
    for r in rows:
        i = inter[r['login']]; l = load[r['login']]
        o.append('| %s | %.2f | %d/%d | %.1f%s | %s | %.2f | %s |' % (
            r['login'], r['interest'], i['authored'], i['reviewed'], r['load'], ' ⚠' if r['overloaded'] else '',
            ', '.join(l['pending'][:3]) + (' …' if len(l['pending']) > 3 else ''), r['total'], r['already'] or '-'))
    o.append('')
    o.append('_인덱스: 머지 PR %d건(%s~%s, 갱신 안 함; --refresh) · 부하: %s · 종합 = %.1f·관심 + %.1f·(1−부하)_'
             % (len(idx['prs']), idx['since'], idx['generated'], load_src, cfg['w_interest'], cfg['w_load']))
    o.append('지정은 사람이: `gh pr edit %d -R %s --add-reviewer %s`' % (pr['number'], repo, ','.join(p['login'] for p in picks)))
    return '\n'.join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('pr', type=int)
    ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--months', type=float, default=18)
    ap.add_argument('--refresh', action='store_true', help='머지 PR 인덱스 재구축')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--pool', help='후보 로그인 콤마 목록(기본 roster tracked_github)')
    a = ap.parse_args()
    roster, cfg = load_roster()
    pool = a.pool.split(',') if a.pool else list(roster.get('reviewer_pool') or roster['tracked_github'])
    pr = fetch_pr(a.repo, a.pr)
    idx = load_index(a.repo, a.months, a.refresh)
    diff = difficulty(pr, cfg)
    inter = interest(pr, idx, pool, cfg)
    load, load_src = load_from_board(pool, cfg)
    if load is None:
        print('load: %s → GitHub 조회' % load_src, file=sys.stderr)
        load, load_src = load_from_github(pool, roster.get('repos', [a.repo]), cfg)
    rows, picks, deferred, mean_l = recommend(pr, diff, inter, load, pool, cfg)
    if a.json:
        print(json.dumps({'pr': pr, 'difficulty': diff, 'picks': [p['login'] for p in picks], 'rows': rows,
                          'interest': inter, 'load': load, 'load_src': load_src}, ensure_ascii=False, indent=1, default=str))
    else:
        print(fmt_report(a.repo, pr, diff, inter, load, load_src, rows, picks, deferred, mean_l, idx, cfg))


if __name__ == '__main__':
    main()
