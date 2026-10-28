#!/usr/bin/env python3
"""post_review.py — findings(.adjudicated).json 을 PR 인라인 리뷰 코멘트로 게시한다 (review-response §게시 형태).

  사용: python3 -m tools.harness.post_review --pr <N> --findings <json> [--draft comments.draft.md] [--only A1,A3] [--post]
  기본 dry-run: 지적별 앵커(file:line)가 PR diff 의 헌크(RIGHT) 안에 있는지 검사하고 본문을 보여준다.
  --post 는 사용자 승인 뒤에만 — **GitHub 에 리뷰를 게시한다(외부 게시)**. 앵커가 diff 밖인 지적은 --fallback-issue 로 일반 코멘트 하나에 모은다.
  코멘트 첫 줄: `[코드 리뷰|설계 리뷰] [카테고리] 🔴/🟡/🟢`, 이어 claim · 왜 문제가 되는지 · 확인 결과 · 제안 · 근거.

  --draft (사용자가 승인한 초안, harness-synthesizer 가 쓴다) 형식 — 이 docstring 이 정본이다:
      ## <N>번 코멘트 — <아무 제목>
      앵커: `path:line[-end]` (<finding id>) · 게시 권고: 예|아니오
      (빈 줄)
      [코드 리뷰|설계 리뷰] [카테고리] 이모지      ← 여기부터 게시 본문
      …
      ---                                          ← 절 끝(코드 울타리 밖의 `---` 한 줄) 또는 다음 `## ` 제목
  앵커·id 는 제목 줄에 있어도 된다(`## 1번 코멘트 — `path:line` (g1-01, …)`). 앵커가 있으면 **초안의 앵커**로 게시한다(사용자가 본 자리).
  초안이 없는 지적이 하나라도 있으면 --post 를 거부한다 — 승인하지 않은 자동 본문이 나가지 않게(2026-10-27, 매핑 0건인데
  자동 본문으로 게시될 뻔한 결함). 자동 본문을 승인했으면 --allow-auto-body. `게시 권고: 아니오` 절은 --only 로 지목해야 게시한다.

  제출 방식(H-43 — 2026-10-27 PR#8196: 인라인 90건 한 리뷰는 422 'Line could not be resolved', 파일별로 나누면 통과했고,
  36건 묶음 POST 가 'Server Error' 를 내고도 리뷰를 만들어 재시도가 36건을 중복 게시했다):
    - 인라인 코멘트를 **파일 묶음**(파일이 처음 나온 순서, 한 파일이 --per-review 를 넘으면 그 수씩)으로 나눠 리뷰 하나씩 **순서대로** 제출한다.
    - 제출 전에 PR 의 인라인 코멘트를 GET 해 같은 (파일, 본문)이 이미 있는 것은 뺀다 — 중간에 멈춘 게시를 다시 실행해도 중복이 안 생긴다.
      dry-run 도 이 목록을 읽어 '이미 게시됨' 수와 제출 계획(리뷰 수·파일별 건수)을 보인다.
    - 4xx(422 등)는 아무것도 만들지 않으므로 그 묶음만 실패로 적고 다음 묶음으로 간다.
    - 5xx·타임아웃·응답 없음은 결과를 모른다 — **다시 보내지 않고** GET 으로 그 묶음의 본문이 생겼는지 확인한다(5초 간격 3번).
      생겼으면 성공으로 치고 이어 가고, 안 보이면 멈춘다(남은 묶음은 같은 명령을 다시 실행하면 이미 있는 것을 건너뛰고 이어서 올린다).
    - --fallback-issue 의 일반 코멘트도 같다(같은 본문이 이미 있으면 건너뜀, 5xx 뒤 재시도 없음).
"""
import argparse, json, re, subprocess, sys, tempfile, time, os
from . import md_lint as ML   # 게시 전 교정·검사 — review_reply 와 같은 함수(H-32)

EMOJI = {'높음': '🔴', '중간': '🟡', '낮음': '🟢'}
LAYER = {'코드': '[코드 리뷰]', '설계': '[설계 리뷰]'}
# 헌크 경계에서 한두 줄 어긋난 단일 줄 앵커만 살린다 — 더 멀면 다른 코드에 붙는다
SNAP_LINES = 3
# 리뷰 하나에 싣는 인라인 상한 — 파일별로 나눈 뒤에도 한 파일이 많으면 자른다. 2026-10-27 PR#8196: 파일별 리뷰 13개 중 가장 큰
# 것(query_executor.c 29건)은 통과했고 36건 묶음은 5xx 를 냈다 — 그 사이
MAX_PER_REVIEW = 30
POST_TIMEOUT = 120                   # 초 — 리뷰 POST 한 번. 넘으면 '결과를 모름' 으로 보고 GET 으로 확인한다(다시 보내지 않는다)
CONFIRM_TRIES, CONFIRM_WAIT = 3, 5   # 5xx·타임아웃 뒤 생성 여부 GET 확인 횟수·간격(초) — 서버가 응답을 버린 뒤 늦게 쓰는 경우를 기다린다

SECTION = re.compile(r'^## (\d+)번 코멘트[^\n]*$', re.M)
ANCHOR = re.compile(r'`?([\w./+-]+\.\w+):(\d+)(?:-(\d+))?`?')
ID_IN_PARENS = re.compile(r'\(([^,()\s]+)')
RECOMMEND = re.compile(r'게시 권고:\s*(예|아니오)')
BODY_START = re.compile(r'^\[(코드|설계)')


def sh(cmd, inp=None, timeout=None):
    return subprocess.run(cmd, input=inp, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=timeout)


def diff_lines(pr, repo):
    """file -> set(new-file line numbers inside hunks)  (RIGHT side 로 코멘트 가능한 줄)"""
    out = sh(['gh', 'pr', 'diff', str(pr), '-R', repo]).stdout
    ok, cur, n = {}, None, 0
    for ln in out.splitlines():
        if ln.startswith('+++ '):
            cur = ln[4:].strip()
            cur = cur[2:] if cur.startswith('b/') else cur
            ok.setdefault(cur, set())
        elif ln.startswith('@@'):
            m = re.match(r'@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@', ln)
            n = int(m.group(1))
        elif cur is not None and ln.startswith('-'):
            pass
        elif cur is not None and (ln.startswith('+') or ln.startswith(' ')):
            ok[cur].add(n); n += 1
        elif cur is not None and ln.startswith('\\'):
            pass
    return ok


def parse_drafts(txt, known_ids):
    """comments.draft.md → {id: {n, body, path, start, line, recommend}} 와 id 를 못 정한 절 번호 목록.
    id 는 제목 줄·`앵커:` 줄의 괄호 첫 토큰 중 findings 에 있는 것을 고른다(없으면 첫 토큰)."""
    heads = list(SECTION.finditer(txt))
    out, unknown = {}, []
    for i, h in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(txt)
        nxt = re.search(r'^## ', txt[h.end():end], re.M)          # '## 참고 …' 같은 다른 절에서 끊는다
        lines = txt[h.end():h.end() + nxt.start() if nxt else end].splitlines()
        meta = [h.group(0)]
        body, fence, started = [], False, False
        for ln in lines:
            if not started:
                if BODY_START.match(ln):
                    started = True
                else:
                    meta.append(ln); continue
            if ln.startswith('```'):
                fence = not fence
            if ln.strip() == '---' and not fence:
                break
            body.append(ln)
        meta_txt = '\n'.join(m for m in meta if m.startswith('## ') or m.lstrip('*').startswith('앵커'))
        cands = ID_IN_PARENS.findall(meta_txt)
        fid = next((c for c in cands if c in known_ids), cands[0] if cands else None)
        if not fid or not body:
            unknown.append(h.group(1)); continue
        a = ANCHOR.search(meta_txt)
        rec = RECOMMEND.search(meta_txt)
        d = {'n': h.group(1), 'body': '\n'.join(body).strip() + '\n',
             'recommend': rec.group(1) if rec else '예'}
        if a:
            s, e = int(a.group(2)), int(a.group(3) or a.group(2))
            d.update(path=a.group(1), start=s, line=e)
        out[fid] = d
    return out, unknown


def body_of(f):
    emoji = EMOJI.get(f.get('importance') or '', '🟡')
    head = f"{LAYER.get(f.get('layer'), '[코드 리뷰]')} [{f.get('category', '확인 질문')}] {emoji}"
    parts = [head, '', f.get('claim', '').strip(), '', '왜 문제가 되는지: ' + f.get('why', '').strip()]
    v = f.get('verification') or {}
    if v:
        m = v.get('method', ''); r = v.get('result', '') or v.get('summary', '')
        if m or r:
            parts += ['', f"확인 결과({m}): {r}".rstrip()]
    prop = f.get('proposal', '').strip()
    if prop:
        parts += ['', '제안:', '', prop]
    ev = f.get('evidence') or []
    rid = f.get('rule_ids') or []
    tail = []
    if ev: tail.append('근거: ' + ', '.join(f'`{e}`' for e in ev))
    if rid: tail.append('규칙: ' + ', '.join(rid))
    if tail: parts += [''] + tail
    return '\n'.join(parts).strip() + '\n'


# ── 제출(H-43): 파일 묶음 · 이미 있는 본문 건너뜀 · 5xx/타임아웃 뒤 재시도 금지 ─────────────────────────
def _norm(s):
    return (s or '').replace('\r\n', '\n').strip()


def _gh_rows(args):
    """`gh api --paginate … -q '.[] | {…}'` 의 줄마다 JSON 한 개(GET). 실패하면 None."""
    r = sh(['gh', 'api', '--paginate', *args])
    if r.returncode:
        return None
    return [json.loads(ln) for ln in r.stdout.splitlines() if ln.strip()]


def inline_keys(pr, repo):
    """PR 에 이미 있는 인라인 코멘트의 (파일, 본문) 집합 — GET pulls/<n>/comments. 읽지 못하면 None."""
    rows = _gh_rows([f'repos/{repo}/pulls/{pr}/comments', '-q', '.[] | {path, body}'])
    return None if rows is None else {(r.get('path'), _norm(r.get('body'))) for r in rows}


def issue_bodies(pr, repo):
    """PR 에 이미 있는 일반 코멘트 본문 집합 — GET issues/<n>/comments. 읽지 못하면 None."""
    rows = _gh_rows([f'repos/{repo}/issues/{pr}/comments', '-q', '.[] | {body}'])
    return None if rows is None else {_norm(r.get('body')) for r in rows}


def _ckey(c):
    return (c['path'], _norm(c['body']))


def batches_by_file(comments, cap=MAX_PER_REVIEW):
    """인라인 코멘트를 파일 묶음으로 — 파일이 처음 나온 순서, 한 파일이 cap 을 넘으면 cap 씩 자른다."""
    order, by = [], {}
    for c in comments:
        if c['path'] not in by:
            by[c['path']] = []
            order.append(c['path'])
        by[c['path']].append(c)
    return [by[p][i:i + cap] for p in order for i in range(0, len(by[p]), cap)]


def post_once(args, timeout=POST_TIMEOUT):
    """gh api POST 한 번 → ('ok', 표준출력) | ('client', 메시지) | ('unknown', 메시지). 어떤 경우에도 다시 보내지 않는다.
    client = 4xx(서버가 거부 — 아무것도 만들지 않았다). unknown = 5xx·타임아웃·HTTP 코드 없는 실패(연결 끊김) — 만들었을 수 있다."""
    try:
        r = sh(args, timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'unknown', f'{timeout}s 타임아웃'
    if r.returncode == 0:
        return 'ok', r.stdout
    m = re.search(r'HTTP (\d{3})', r.stderr or '')
    msg = ' '.join(x for x in (r.stderr.strip(), r.stdout.strip()[:300]) if x)
    return ('client' if m and 400 <= int(m.group(1)) < 500 else 'unknown'), msg


def confirm_created(fetch, keys, tries=CONFIRM_TRIES, wait=CONFIRM_WAIT):
    """결과를 모르는 POST 뒤: fetch() 집합에 keys 중 하나라도 생겼나(리뷰는 통째로 만들어진다). 생긴 수, 끝내 없으면 0."""
    for _ in range(tries):
        time.sleep(wait)
        have = fetch()
        n = sum(1 for k in keys if have is not None and k in have)
        if n:
            return n
    return 0


def post_inline(pr, repo, comments, cap=MAX_PER_REVIEW, out=sys.stderr):
    """파일 묶음 리뷰를 순서대로 제출한다. 반환 (리뷰 URL 목록, 실패 메시지 목록, 멈췄으면 남은 묶음 목록)."""
    have = inline_keys(pr, repo)
    if have is None:
        return [], ['PR 인라인 코멘트 목록(GET pulls/<n>/comments)을 읽지 못했다 — 중복 여부를 모르니 게시하지 않는다(인증·네트워크 확인 후 다시)'], batches_by_file(comments, cap)
    todo = [c for c in comments if _ckey(c) not in have]
    if len(todo) < len(comments):
        print(f'· 이미 PR 에 같은 본문이 있는 인라인 {len(comments) - len(todo)}건은 건너뜀(다시 실행해도 중복 없음)', file=out)
    groups = batches_by_file(todo, cap)
    urls, errors = [], []
    for i, g in enumerate(groups):
        label = f"[{i + 1}/{len(groups)}] {g[0]['path']} {len(g)}건"
        with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as t:
            json.dump({'event': 'COMMENT', 'body': '', 'comments': g}, t, ensure_ascii=False)
            tp = t.name
        try:
            st, res = post_once(['gh', 'api', '-X', 'POST', f'repos/{repo}/pulls/{pr}/reviews', '--input', tp])
        finally:
            os.unlink(tp)
        if st == 'ok':
            url = json.loads(res).get('html_url')
            urls.append(url)
            print(f'게시 {label}: {url}', file=out)
            continue
        if st == 'client':
            errors.append(f'{label}: {res[:500]}')
            print(f'✗ {label} 거부(아무것도 만들어지지 않았다 — 다음 묶음으로): {res[:500]}', file=out)
            continue
        print(f'⚠ {label} 결과를 모름({res[:200]}) — 다시 보내지 않고 GET 으로 생성 여부를 확인한다', file=out)
        n = confirm_created(lambda: inline_keys(pr, repo), [_ckey(c) for c in g])
        if n:
            urls.append(f'(결과를 모르던 제출 — {n}/{len(g)}건 생성 확인)')
            print(f'↻ {label}: 리뷰가 만들어져 있었다({n}/{len(g)}건 확인) — 재시도하지 않고 이어 간다', file=out)
            continue
        errors.append(f'{label}: 결과를 모름 — 생성 확인 안 됨')
        print(f'✗ {label}: {CONFIRM_TRIES}번 확인해도 안 보인다 — 여기서 멈춘다. 같은 명령을 다시 실행하면 이미 있는 것은 건너뛰고 '
              f'이어서 올린다', file=out)
        return urls, errors, groups[i:]
    return urls, errors, []


def post_issue_comment(pr, repo, body, out=sys.stderr):
    """일반 코멘트 하나 — 같은 본문이 이미 있으면 건너뛰고, 5xx·타임아웃 뒤에는 재시도 없이 GET 으로 확인한다. 성공이면 True."""
    have = issue_bodies(pr, repo)
    if have is None:
        print('✗ 일반 코멘트 목록(GET issues/<n>/comments)을 읽지 못해 게시하지 않는다(중복 여부를 모름)', file=out)
        return False
    if _norm(body) in have:
        print('· 같은 본문의 일반 코멘트가 이미 있다 — 건너뜀', file=out)
        return True
    with tempfile.NamedTemporaryFile('w', suffix='.md', delete=False) as t:
        t.write(body)
        tp = t.name
    try:
        st, res = post_once(['gh', 'api', '-X', 'POST', f'repos/{repo}/issues/{pr}/comments', '-F', f'body=@{tp}', '-q', '.html_url'])
    finally:
        os.unlink(tp)
    if st == 'ok':
        print('일반 코멘트:', res.strip())
        return True
    if st == 'unknown' and confirm_created(lambda: issue_bodies(pr, repo), [_norm(body)]):
        print('↻ 일반 코멘트: 결과를 몰랐지만 만들어져 있었다 — 재시도하지 않음', file=out)
        return True
    print(f'✗ 일반 코멘트 게시 실패({st}): {res[:500]}', file=out)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', required=True, type=int)
    ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--findings', required=True)
    ap.add_argument('--only', default='')
    ap.add_argument('--post', action='store_true')
    ap.add_argument('--fallback-issue', action='store_true', help='앵커가 diff 밖인 지적을 일반 코멘트 하나로')
    ap.add_argument('--out', default='', help='본문 미리보기 md 저장')
    ap.add_argument('--draft', default='', help='comments.draft.md — 형식은 이 파일 docstring. 초안이 없는 지적이 있으면 --post 거부')
    ap.add_argument('--allow-auto-body', action='store_true', help='초안이 없는 지적을 자동 본문으로 게시하는 것을 승인했을 때만')
    ap.add_argument('--per-review', type=int, default=MAX_PER_REVIEW,
                    help=f'리뷰 하나에 싣는 인라인 상한(기본 {MAX_PER_REVIEW}) — 파일별로 나눈 뒤 한 파일이 이보다 많으면 자른다')
    a = ap.parse_args()
    data = json.load(open(a.findings))
    fs = data['findings'] if isinstance(data, dict) and 'findings' in data else data
    only = set(x for x in a.only.split(',') if x)
    if only:
        fs = [f for f in fs if f.get('id') in only]
    fs = [f for f in fs if f.get('status') not in ('invalid', 'withheld')]
    # 주석 제안은 지금 주석이 코드와 다를 때(mismatch)·새 API 의 쓰임을 제한할 때(new_api_contract)만 게시한다
    # (사용자 지시 2026-10-27) — 꼭 달아야 하면 --only 로 지목한다
    held = [f['id'] for f in fs if f.get('category') == '주석 제안'
            and f.get('comment_basis') not in ('mismatch', 'new_api_contract') and f.get('id') not in only]
    if held:
        print(f"· 주석 제안 기준 미달로 게시하지 않음: {held} — comment_basis 가 mismatch·new_api_contract 가 아니다"
              f"(꼭 달려면 --only 로 지목)", file=sys.stderr)
        fs = [f for f in fs if f['id'] not in held]
    drafts = {}
    if a.draft:
        drafts, unknown = parse_drafts(open(a.draft).read(), {f.get('id') for f in fs})
        missing = [f['id'] for f in fs if f['id'] not in drafts]
        print(f"draft 본문 {len(drafts)}건 매핑 / 지적 {len(fs)}건", file=sys.stderr)
        if unknown:
            print(f"⚠ id·본문을 못 읽은 초안 절: {unknown} — 앵커 줄에 `(지적 id)`, 본문 첫 줄이 `[코드`·`[설계` 인지 본다", file=sys.stderr)
        extra = sorted(set(drafts) - {f['id'] for f in fs})
        if extra:
            print(f"· findings 에 없는 초안 절(게시 안 함): {extra}", file=sys.stderr)
        if missing and not a.allow_auto_body:
            print(f"✗ 초안이 없는 지적 {missing} — 승인하지 않은 자동 본문이 나간다. 초안에 그 절을 넣거나, --only 로 빼거나, "
                  f"자동 본문을 승인했으면 --allow-auto-body", file=sys.stderr)
            if a.post:
                sys.exit(2)
    elif a.post:
        print('· 초안 없이 자동 본문(body_of)으로 게시한다', file=sys.stderr)
    ok = diff_lines(a.pr, a.repo)
    comments, orphans, preview = [], [], []
    for f in fs:
        d = drafts.get(f['id'])
        if d and d['recommend'] == '아니오' and f['id'] not in only:
            print(f"· {f['id']} 게시 권고 아니오 — 건너뜀(--only 로 지목하면 게시)", file=sys.stderr); continue
        path, line = f.get('file'), int(f.get('line') or 0)
        start = line
        if d and d.get('path'):
            if (d['path'], d['line']) != (path, line):
                print(f"↻ {f['id']} 앵커를 초안 기준으로: {path}:{line} → {d['path']}:{d['line']}", file=sys.stderr)
            path, start, line = d['path'], d['start'], d['line']
        body = d['body'] if d else body_of(f)
        # 문장 뒤 울타리 분리·가독성 경고·울타리 홀수 거부 — review_reply 와 같은 검사(md_lint, H-32)
        body, fatal = ML.report(f['id'], body, sys.stderr)
        if fatal: continue
        hunk = ok.get(path, set())
        anchored = line in hunk and start in hunk
        if not anchored and start == line and hunk:
            near = min(hunk, key=lambda x: abs(x - line))
            if abs(near - line) <= SNAP_LINES:
                print(f"↻ {f['id']} {path}:{line} → {near} (헌크 안 가장 가까운 줄)", file=sys.stderr)
                start = line = near; anchored = True
        span = f"{start}-{line}" if start != line else f"{line}"
        preview.append(f"## {f['id']} — {path}:{span} {'' if anchored else '(diff 밖)'}\n\n{body}")
        if anchored:
            c = {'path': path, 'line': line, 'side': 'RIGHT', 'body': body}
            if start != line:
                c.update(start_line=start, start_side='RIGHT')
            comments.append(c)
        else:
            orphans.append((f['id'], path, span, body))
    if a.out:
        open(a.out, 'w').write('\n\n'.join(preview))
    print(f"앵커 OK {len(comments)}건 / diff 밖 {len(orphans)}건: {[o[0] for o in orphans]}", file=sys.stderr)
    if not a.post:
        if comments:   # 제출 계획 — 이미 PR 에 있는 본문은 빼고(GET), 파일 묶음으로
            have = inline_keys(a.pr, a.repo)
            todo = comments if have is None else [c for c in comments if _ckey(c) not in have]
            groups = batches_by_file(todo, a.per_review)
            print(f"제출 계획: 리뷰 {len(groups)}개(파일 묶음, 묶음당 최대 {a.per_review}건) — "
                  + (', '.join(f"{g[0]['path']}({len(g)})" for g in groups) or '없음')
                  + ('' if have is None else f" · 이미 게시된 같은 본문 {len(comments) - len(todo)}건은 건너뜀")
                  + (' · ⚠ PR 인라인 코멘트 목록을 못 읽음(게시하면 거부된다)' if have is None else ''), file=sys.stderr)
        print('(dry-run) --post 로 게시', file=sys.stderr); return
    failed = False
    if comments:
        urls, errors, left = post_inline(a.pr, a.repo, comments, a.per_review)
        print(f"인라인: 리뷰 {len(urls)}개 게시" + (f" · 실패 {len(errors)}" if errors else '') + (f" · 남은 묶음 {len(left)}" if left else ''), file=sys.stderr)
        failed = bool(errors or left)
        if left:   # 결과를 모르는 제출에서 멈췄다 — 이어서 하는 것은 사람이 상태를 본 뒤 같은 명령 재실행으로
            print('· 멈췄으므로 일반 코멘트(--fallback-issue)도 올리지 않는다', file=sys.stderr)
            sys.exit(1)
    if orphans and a.fallback_issue:
        body = '\n\n---\n\n'.join(f"**{i}** (`{p}:{l}` — diff 밖이라 인라인 불가)\n\n{b}" for i, p, l, b in orphans)
        failed = (not post_issue_comment(a.pr, a.repo, body)) or failed
    if failed:
        sys.exit(1)


if __name__ == '__main__':
    main()
