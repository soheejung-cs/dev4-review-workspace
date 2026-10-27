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
"""
import argparse, json, re, subprocess, sys, tempfile, os

EMOJI = {'높음': '🔴', '중간': '🟡', '낮음': '🟢'}
LAYER = {'코드': '[코드 리뷰]', '설계': '[설계 리뷰]'}
# 헌크 경계에서 한두 줄 어긋난 단일 줄 앵커만 살린다 — 더 멀면 다른 코드에 붙는다
SNAP_LINES = 3

SECTION = re.compile(r'^## (\d+)번 코멘트[^\n]*$', re.M)
ANCHOR = re.compile(r'`?([\w./+-]+\.\w+):(\d+)(?:-(\d+))?`?')
ID_IN_PARENS = re.compile(r'\(([^,()\s]+)')
RECOMMEND = re.compile(r'게시 권고:\s*(예|아니오)')
BODY_START = re.compile(r'^\[(코드|설계)')


def sh(cmd, inp=None):
    return subprocess.run(cmd, input=inp, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)


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
    a = ap.parse_args()
    data = json.load(open(a.findings))
    fs = data['findings'] if isinstance(data, dict) and 'findings' in data else data
    only = set(x for x in a.only.split(',') if x)
    if only:
        fs = [f for f in fs if f.get('id') in only]
    fs = [f for f in fs if f.get('status') != 'invalid']
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
        # 울타리 짝 검사 (review-response: 홀수면 거부)
        if body.count('```') % 2:
            print(f"✗ {f['id']} 울타리 홀수 — 게시 거부", file=sys.stderr); continue
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
        print('(dry-run) --post 로 게시', file=sys.stderr); return
    if comments:
        payload = {'event': 'COMMENT', 'body': '', 'comments': comments}
        with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as t:
            json.dump(payload, t, ensure_ascii=False); tp = t.name
        r = sh(['gh', 'api', '-X', 'POST', f'repos/{a.repo}/pulls/{a.pr}/reviews', '--input', tp])
        os.unlink(tp)
        if r.returncode:
            print('게시 실패:', r.stderr[:2000], file=sys.stderr); sys.exit(1)
        print('게시:', json.loads(r.stdout).get('html_url'))
    if orphans and a.fallback_issue:
        body = '\n\n---\n\n'.join(f"**{i}** (`{p}:{l}` — diff 밖이라 인라인 불가)\n\n{b}" for i, p, l, b in orphans)
        with tempfile.NamedTemporaryFile('w', suffix='.md', delete=False) as t:
            t.write(body); tp = t.name
        r = sh(['gh', 'api', '-X', 'POST', f'repos/{a.repo}/issues/{a.pr}/comments', '-F', f'body=@{tp}', '-q', '.html_url'])
        os.unlink(tp); print('일반 코멘트:', r.stdout.strip() or r.stderr[:500])


if __name__ == '__main__':
    main()
