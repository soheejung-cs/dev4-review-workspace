#!/usr/bin/env python3
"""post_review.py — findings(.adjudicated).json 을 PR 인라인 리뷰 코멘트로 게시한다 (review-response §게시 형태).

  사용: python3 -m tools.harness.post_review --pr <N> --findings <json> [--only A1,A3] [--post]
  기본 dry-run: 지적별 앵커(file:line)가 PR diff 의 헌크(RIGHT) 안에 있는지 검사하고 본문을 보여준다.
  --post 는 사용자 승인 뒤에만. 앵커가 diff 밖인 지적은 --fallback-issue 로 일반 코멘트 하나에 모은다.
  코멘트 첫 줄: `[코드 리뷰|설계 리뷰] [카테고리] 🔴/🟡/🟢`, 이어 claim · 왜 문제가 되는지 · 확인 결과 · 제안 · 근거.
"""
import argparse, json, re, subprocess, sys, tempfile, os

EMOJI = {'높음': '🔴', '중간': '🟡', '낮음': '🟢'}
LAYER = {'코드': '[코드 리뷰]', '설계': '[설계 리뷰]'}


def sh(cmd, inp=None):
    return subprocess.run(cmd, input=inp, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


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
    ap.add_argument('--draft', default='', help='comments.draft.md — `## N번 코멘트 — path:line (Aid…)` 절의 본문(--- 사이)을 그 id 의 본문으로 쓴다')
    a = ap.parse_args()
    data = json.load(open(a.findings))
    fs = data['findings'] if isinstance(data, dict) and 'findings' in data else data
    if a.only:
        keep = set(a.only.split(',')); fs = [f for f in fs if f.get('id') in keep]
    fs = [f for f in fs if f.get('status') != 'invalid']
    drafts = {}
    if a.draft:
        txt = open(a.draft).read()
        for m in re.finditer(r'^## (\d+)번 코멘트[^\n]*?\((A\d+|MEAS[^,)]*)[^\n]*\n---\n(.*?)\n---', txt, re.S | re.M):
            drafts[m.group(2)] = m.group(3).strip() + '\n'
        print(f"draft 본문 {len(drafts)}건 매핑", file=sys.stderr)
    ok = diff_lines(a.pr, a.repo)
    comments, orphans, preview = [], [], []
    for f in fs:
        path, line = f.get('file'), int(f.get('line') or 0)
        body = drafts.get(f['id']) or body_of(f)
        # 울타리 짝 검사 (review-response: 홀수면 거부)
        if body.count('```') % 2:
            print(f"✗ {f['id']} 울타리 홀수 — 게시 거부", file=sys.stderr); continue
        anchored = path in ok and line in ok[path]
        if not anchored and path in ok and ok[path]:
            near = min(ok[path], key=lambda x: abs(x - line))
            if abs(near - line) <= 3:
                print(f"↻ {f['id']} {path}:{line} → {near} (헌크 안 가장 가까운 줄)", file=sys.stderr)
                line, anchored = near, True
        preview.append(f"## {f['id']} — {path}:{line} {'' if anchored else '(diff 밖)'}\n\n{body}")
        if anchored:
            comments.append({'path': path, 'line': line, 'side': 'RIGHT', 'body': body})
        else:
            orphans.append((f['id'], path, line, body))
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
