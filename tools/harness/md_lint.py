#!/usr/bin/env python3
"""게시 본문(GitHub 마크다운) 교정·검사 — 답글 도구(review_reply)와 인라인 게시 도구(post_review)가 같이 쓴다(H-32).

둘이 따로 검사하던 때 post_review 는 울타리 홀수만 거부해, 문장 뒤에 붙은 울타리(```suggestion 이 열리지 않음)·
" / " 로 이어 붙은 절·긴 문단이 그대로 나갈 수 있었다. 검사 함수는 이 모듈 하나에만 둔다.

  sanitize_markdown(body) -> (body, fixes, fatal)   문장 뒤 울타리를 줄 맨 앞으로 분리, 울타리 홀수는 치명(게시 거부)
  readability(body)       -> (body, fixes, warns)   울타리 밖 산문의 절 경계·①②③ 열거 복원, 문단·중요도별 길이 경고
  tone(body)              -> (warns, fatal)          말투(H-64 ⑦): 내부 ID 거부, 첫 문장·꼬리표·지은 말·단어 수 경고
  count_warnings(marks)   -> warns                   PR 당 코멘트 수(🔴🟡🟢 목록)
  check(body)             -> (body, fixes, warns, fatal)   위 셋을 차례로
  report(tag, body, out)  -> (body, fatal)          check 결과를 ↻(교정)·⚠(경고)·✗(거부) 줄로 out 에 쓴다

사이드이펙트 없음(문자열만 다룬다). 사용 예: python3 -m tools.harness.md_lint <본문.md> — 파일 하나를 검사해 출력만 한다.
"""
import re
import sys

# ── 게시 전 마크다운 교정 ──────────────────────────────────────────────
# 실제로 깨진 사례(2026-10-06, PR#8034 T?): 초안이 "제안: ```suggestion" 처럼
# 울타리를 문장 뒤 같은 줄에 붙였다. GitHub 은 줄 맨 앞에서 시작하는 울타리만
# 코드블록으로 읽으므로, 블록이 열리지 않은 채 본문이 마크다운으로 렌더됐고
# "* domain here …" 줄이 불릿 목록으로 바뀌어 제안 문구가 통째로 망가졌다.
FENCE_TAIL = re.compile(r'^(?P<pre>.*\S)[ \t]*(?P<fence>`{3,}|~{3,})(?P<info>[A-Za-z0-9_+-]*)[ \t]*$')
FENCE_LINE = re.compile(r'^[ \t]*(`{3,}|~{3,})')


def sanitize_markdown(body):
    """게시 전 교정. 반환 (고친 본문, 고친 내역, 치명적 문제 목록)."""
    fixes, fatal, out = [], [], []
    for ln in body.split('\n'):
        m = FENCE_TAIL.match(ln)
        if m and not FENCE_LINE.match(ln):
            indent = ln[:len(ln) - len(ln.lstrip())]
            out.append(m.group('pre').rstrip())
            out.append(indent + m.group('fence') + m.group('info'))
            fixes.append(f"울타리를 줄 맨 앞으로 분리: {ln.strip()[:60]}")
        else:
            out.append(ln)
    body = '\n'.join(out)
    n = sum(1 for ln in body.split('\n') if FENCE_LINE.match(ln))
    if n % 2:
        fatal.append(f'코드 울타리가 홀수개({n}) — 열고 닫지 않았다. 게시하면 뒤 본문이 통째로 코드블록이 된다')
    return body, fixes, fatal


# ── 가독성 교정 ────────────────────────────────────────────────────────
# 실측(2026-10-06 PR#8034): 한 지적의 여러 절(왜·반증 보강·완결성 비평·별건 확인)이
# " / " 로 이어 붙고 ①②③ 열거가 줄바꿈 없이 들어가 3,000자 한 덩어리가 됐다.
# 내용은 맞는데 읽을 수가 없다 — 절 경계와 열거만 복원해도 크게 달라진다.
SECTION = r'왜 문제가 되는지|왜 문제인가|반증[^\s:：]*|보강 근거[^\s:：]*|완결성[^\s:：]*|확인 결과[^\s:：]*|검증|제안|별건[^\s:：]*|참고'
SEC_SPLIT = re.compile(r'[ \t]+/[ \t]+(?=(?:' + SECTION + r'))')
ENUM = re.compile(r'(?<=[^\n\s])[ \t]+(?=[①②③④⑤⑥⑦⑧⑨])')
LONG_PARA = 600          # 한 문단이 이보다 길면 읽기 어렵다(review-response 「게시 형식」)
BUDGET = {'🟢': 1200, '🟡': 2500, '🔴': 4000}   # 중요도별 한 코멘트 길이(같은 절)


def _fmt_prose(seg):
    seg = SEC_SPLIT.sub('\n\n', seg)      # " / 반증 …:" → 새 절
    seg = ENUM.sub('\n- ', seg)           # ①②③ 을 각자 줄의 목록으로
    return seg


def readability(body):
    """울타리 밖 산문만 손본다. 반환 (고친 본문, 내역, 경고)."""
    fixes, warns, out, infence = [], [], [], False
    for ln in body.split('\n'):
        if FENCE_LINE.match(ln): infence = not infence; out.append(ln); continue
        out.append(ln if infence else _fmt_prose(ln))
    body2 = '\n'.join(out)
    if body2 != body:
        if SEC_SPLIT.search(body): fixes.append('" / " 로 이어 붙은 절을 문단으로 분리')
        if ENUM.search(body):      fixes.append('①②③ 열거를 목록으로 분리')
    for para in body2.split('\n\n'):
        if not para.startswith('```') and len(para) > LONG_PARA:
            warns.append(f'문단 하나가 {len(para)}자 — {LONG_PARA}자 넘으면 읽기 어렵다. 절로 쪼갤 것: "{para[:40]}…"')
    mark = next((m for m in BUDGET if m in body2.split('\n')[0]), None)
    if mark and len(body2) > BUDGET[mark]:
        warns.append(f'{mark} 지적인데 {len(body2)}자 — 권장 {BUDGET[mark]}자. 중요도에 견주어 길다(같은 근거를 두 번 적지 않았나?)')
    return body2, fixes, warns


# ── 말투 검사 (H-64 ⑦, 정본 references/글쓰기-말투.md) ─────────────────
# 실측(2026-11-17 .51 PR#8375): 결론이 조건 셋 뒤, "왜 문제가 되는지:" 꼬리표, 분석하며 지은 말("닿는 조건"), 600단어·PR 당 14건.
# 기준값은 episodic 의 지난 코멘트 289건에서 쟀다(첫 문장 중앙값 153자·p80 215자, 꼬리표로 연 코멘트 54건, PR 당 p80 10건).
# 내부 ID 만 거부하고 나머지는 경고 — 문장은 사람이 고친다.
INTERNAL_ID = re.compile(r'(?<![0-9A-Za-z_-])(?:G\d+-F?\d+[a-z0-9-]*|H-\d{2,3}|R-\d{2}|#0\d\d)(?![0-9A-Za-z_])')
SECTION_SIGN = re.compile(r'§\s?\d')
EXTERNAL_DOC = re.compile(r'(Reference|SQL|ISO|RFC|표준|매뉴얼|manual|Manual|스펙)[^§\n]{0,30}$')   # 외부 문서의 절 번호는 내부 ID 가 아니다(Oracle SQL Reference §8)
TAG_LINE = re.compile(r'^\s*(?:\*\*)?(?:왜 문제가 되는지|왜 문제인가|닿는 조건|확인 결과[^:：\n]{0,12}|반증[^:：\n]{0,8}|보강 근거)(?:\*\*)?\s*[:：]', re.M)
COINED = ('닿는 조건', '파생 절', '사용자 범위', '방어로')   # 글쓰기-말투.md §1-3·§2 — 분석하며 지은 이름
FIRST_SENT = 150      # 첫 문장 글자 수 경고선(지난 코멘트 중앙값)
WORDS = 250           # 울타리·<details> 밖 본문 단어 수 경고선(다섯 문장 안팎 + 재현, §1-5)


def _prose(body):
    """울타리·<details> 블록을 뺀 산문."""
    t = re.sub(r'^[ \t]*(`{3,}|~{3,}).*?^[ \t]*\1[ \t]*$', '', body, flags=re.S | re.M)
    return re.sub(r'<details>.*?</details>', '', t, flags=re.S)


def first_sentence(body):
    """첫 줄의 `[층] [카테고리] 이모지` 꼬리표 줄을 건너뛴 첫 문장."""
    lines = [ln.strip() for ln in _prose(body).split('\n') if ln.strip()]
    lines = [ln for ln in lines if not re.match(r'^\[[^\]]+\]\s*(\[[^\]]+\])?\s*\S{0,2}$', ln)]
    if not lines: return ''
    m = re.search(r'.+?(?:[.?!](?=\s|$)|$)', lines[0])
    return m.group(0) if m else lines[0]


def tone(body):
    """반환 (경고, 치명). 내부 ID 는 치명(밖으로 나가는 글에 쓰지 않는다), 나머지는 경고."""
    warns, fatal = [], []
    prose = _prose(body)
    bare = re.sub(r'`[^`\n]*`', '', prose)
    ids = sorted(set(m.group(0) for m in INTERNAL_ID.finditer(bare))
                 | {m.group(0) for m in SECTION_SIGN.finditer(bare) if not EXTERNAL_DOC.search(bare[max(0, m.start() - 40):m.start()])})
    if ids: fatal.append(f'내부 식별자 {ids} — 밖으로 나가는 글엔 쓰지 않는다(글쓰기-말투 §2)')
    fs = first_sentence(body)
    if len(fs) > FIRST_SENT:
        warns.append(f'첫 문장이 {len(fs)}자(경고선 {FIRST_SENT}) — 결론(관찰할 수 있는 결과)부터, 조건은 뒤로: "{fs[:40]}…"')
    tags = TAG_LINE.findall(prose)
    if tags: warns.append(f'꼬리표로 연 문단 {len(tags)}개({", ".join(t.strip()[:12] for t in tags[:3])}) — 문단 첫 문장이 그 역할을 하게')
    coined = [w for w in COINED if w in prose]
    if coined: warns.append(f'분석하며 지은 말 {coined} — 코드·SQL·독자가 쓰는 말로 바꾼다')
    n = len(prose.split())
    if n > WORDS: warns.append(f'본문 {n}단어(경고선 {WORDS}) — 다섯 문장 안팎 + 재현, 긴 인과 사슬은 <details> 로')
    return warns, fatal


def count_warnings(marks):
    """PR 하나에 올릴 코멘트들의 중요도 이모지 목록 → 경고(결정 3: 🔴 하나씩 · 🟡 8건 넘으면 묶음 · 🟢 한 코멘트)."""
    w = []
    if marks.count('🟡') > 8: w.append(f'🟡 {marks.count("🟡")}건 — 8건 넘으면 같은 파일·같은 원인끼리 묶는다')
    if marks.count('🟢') > 1: w.append(f'🟢 {marks.count("🟢")}건 — 🟢 는 한 코멘트로 모은다')
    if len(marks) > 10: w.append(f'코멘트 {len(marks)}건(지난 리뷰 p80 10건) — 많으면 독자는 전부를 기계 글로 본다')
    return w


def check(body):
    """sanitize_markdown → readability → tone. 반환 (고친 본문, 교정 내역, 경고, 치명)."""
    body, fixes, fatal = sanitize_markdown(body)
    body, rfix, warns = readability(body)
    tw, tf = tone(body)
    return body, fixes + rfix, warns + tw, fatal + tf


def report(tag, body, out=sys.stderr):
    """check 결과를 review_reply 와 같은 기호로 찍는다: ↻ 교정 · ⚠ 경고 · ✗ 거부. 반환 (고친 본문, 치명 목록)."""
    body, fixes, warns, fatal = check(body)
    for f in fixes: print(f'  ↻ {tag} 교정: {f}', file=out)
    for w in warns: print(f'  ⚠ {tag} {w}', file=out)
    for f in fatal: print(f'  ✗ {tag} {f} — 게시하지 않는다. 초안을 고치고 다시 돌릴 것', file=out)
    return body, fatal


if __name__ == '__main__':
    if len(sys.argv) != 2 or sys.argv[1] in ('-h', '--help'):
        print(__doc__); sys.exit(0 if len(sys.argv) == 2 else 2)
    _b, _fatal = report('본문', open(sys.argv[1], encoding='utf-8').read(), sys.stdout)
    sys.exit(1 if _fatal else 0)
