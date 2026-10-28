#!/usr/bin/env python3
"""source_facts_check — 소스 지식 문서의 file:line·심볼 인용이 기준 리비전에서 맞고, develop HEAD 에서도 그대로인지 본다(H-18, K-05).

왜: dev4-ai-source 의 staging·modules·스킬이 `object_domain_convert.cpp:2164` 처럼 줄 번호를 인용하는데, develop 이 움직이면
    그 줄은 다른 코드가 된다. LLM 이 그 인용을 믿고 쓰면 엉뚱한 줄을 근거로 지적한다. 낡음은 기계가 잡는다(하네스-작성-규칙 K-05).

사용(읽기 전용 — 엔진 리포는 git show/diff 만, 체크아웃 없음):
    python3 -m tools.harness.source_facts_check [문서·디렉터리 …] [--repo ~/dev/sources/cubrid] [--head upstream/develop]
                                                [--rev <기본 rev>] [--json out.json] [--md out.md] [--strict] [--show N]
    문서를 주지 않으면 ~/dev/docs/dev4-ai-source/{staging,modules}/*.md 와 이 리포 skills/*/SKILL.md.
기준 리비전: 항목(불릿·표 행·문단)마다 `**주장** (컨테이너, `rev`, 날짜)` 처럼 첫 괄호 안의 커밋 해시, 없으면 문서 머리의
    `기준 리비전 … `rev``·`> 기준: upstream/develop <rev>`, 그것도 없으면 --rev(기본 --head). 커밋으로 풀리지 않는 해시는 건너뛴다.
인용: `파일.c:123`·`dir/파일.cpp:10-20`·`:45`(같은 항목의 직전 파일)·`:1·:2`·`123 / :456` 이어 쓰기. 파일은 경로 끝이 맞는 것을 찾고
    여럿이면 문서의 모듈(src-query → src/query)·src/ 아래를 고른다. 심볼은 그 인용 바로 앞의 `이름`·`이름 ()` 과 짝지어
    "인용한 줄 ±3 안에 그 이름이 있나" 를 본다(줄 번호가 엉뚱한 곳을 가리키는지).
판정: ok(기준 rev 에서 맞고 HEAD 에서 같은 줄) · moved(내용은 그대로, HEAD 줄 번호가 바뀜 — 새 번호 제시) · changed(HEAD 에서 그 줄이
    바뀌거나 지워짐 — 다시 확인할 것) · gone(HEAD 에 파일이 없음) · error(기준 rev 에서 이미 틀림: 줄이 파일 밖·파일 없음·심볼이
    인용 줄 근처에 없음) · ambiguous(같은 이름 파일이 여럿).
출력: 문서마다 한 줄 요약 + error·changed 상위 --show 개, 전체는 --json/--md. 종료 코드: error 가 있으면 1(--strict 면 moved·changed·gone 도 1).
한계: 인용 문장의 '의미' 가 맞는지는 보지 않는다(줄과 심볼의 위치만). 줄 이동은 git diff 의 헝크로 따지므로 같은 내용이 다른 곳으로 옮겨진
    경우는 changed 로 나온다. 다른 리포(cubrid-testcases 등)의 경로는 error(파일 없음)로 보일 수 있다 — 문서에서 경로를 리포째 적는다.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

HOME = os.path.expanduser('~')
DEFAULT_DOCS = [os.path.join(HOME, 'dev/docs/dev4-ai-source/staging'), os.path.join(HOME, 'dev/docs/dev4-ai-source/modules'),
                os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'skills')]
EXT = r'(?:c|cpp|cc|cxx|h|hpp|hh|l|y|i|java|py|sh|g4|inc)'
FILE_LINE = re.compile(r'(?<![\w./-])(?P<path>(?:[\w.+-]+/)*[\w.+-]+\.' + EXT + r'):(?P<n>\d+(?:-\d+)?)(?P<more>(?:\s*(?:[·,/]|및)\s*:?\s*\d+(?:-\d+)?(?![\d.]))*)')
BARE = re.compile(r'(?:(?<=[(\s·,/])|^):(?P<n>\d+(?:-\d+)?)(?P<more>(?:\s*[·,/]\s*:?\s*\d+(?:-\d+)?)*)(?![\d:])')
MORE_NUM = re.compile(r'(\d+(?:-\d+)?)')
# 인용 바로 앞의 심볼 — 사이에는 공백과 '(' 또는 표 칸 구분 '|' 하나만 허용한다("`a`(f.c:1)"·"| `a` | f.c:1"·"`a` :1").
# 더 떨어진 심볼은 그 인용의 주어가 아닐 때가 많다("(`x`) · f.c:1" 의 x 는 앞 인용의 것) — 짝짓지 않는다
SYM_BEFORE = re.compile(r'(?:`([A-Za-z_][A-Za-z0-9_:~]*)(?:\s*\(\s*\))?`|\b([A-Za-z_][A-Za-z0-9_]{3,})\s*\(\s*\))\s*(?:\(|\|\s*)?$')
FILE_ANY = re.compile(r'(?<![\w./-])((?:[\w.+-]+/)*[\w.+-]+\.' + EXT + r')(?![\w])')
HEX = re.compile(r'`?\b([0-9a-f]{7,40})\b`?')
HEAD_REV = re.compile(r'기준(?:\s*리비전)?\s*[:：]?[^\n`]{0,40}?`?([0-9a-f]{7,40})`?')
CLAIM_PAREN = re.compile(r'^\s*(?:[-*]|\d+\.)\s+\*\*.+?\*\*\s*\(([^)]*)\)')
HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


class Repo(object):
    def __init__(self, path: str, head: str):
        self.path, self._show, self._diff, self._tree, self._ok = path, {}, {}, {}, {}
        self.head = self.commit(head) or head

    def _git(self, *a) -> Tuple[int, str]:
        r = subprocess.run(['git', '-C', self.path] + list(a), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return r.returncode, r.stdout.decode('utf-8', 'replace')

    def commit(self, rev: str) -> Optional[str]:
        if rev not in self._ok:
            rc, out = self._git('rev-parse', '--verify', '--quiet', rev + '^{commit}')
            self._ok[rev] = out.strip() if rc == 0 and out.strip() else None
        return self._ok[rev]

    def files(self, rev: str) -> List[str]:
        if rev not in self._tree:
            self._tree[rev] = self._git('ls-tree', '-r', '--name-only', rev)[1].split('\n')
        return self._tree[rev]

    def lines(self, rev: str, path: str) -> Optional[List[str]]:
        k = (rev, path)
        if k not in self._show:
            rc, out = self._git('show', '%s:%s' % (rev, path))
            self._show[k] = out.split('\n') if rc == 0 else None
        return self._show[k]

    def hunks(self, rev: str, path: str) -> List[Tuple[int, int, int, int]]:
        k = (rev, path)
        if k not in self._diff:
            out = self._git('diff', '-U0', '--no-color', rev, self.head, '--', path)[1]
            hs = []
            for ln in out.split('\n'):
                m = HUNK.match(ln)
                if m:
                    hs.append((int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1),
                               int(m.group(3)), int(m.group(4) if m.group(4) is not None else 1)))
            self._diff[k] = hs
        return self._diff[k]

    def map_line(self, rev: str, path: str, n: int) -> Optional[int]:
        """rev 의 줄 n → head 의 줄. 그 줄이 바뀌었거나 지워졌으면 None."""
        delta = 0
        for os_, oc, ns, nc in self.hunks(rev, path):
            if oc == 0:                                   # 순수 추가: os_ 줄 '뒤' 에 끼워 넣었다
                if n > os_:
                    delta += nc
                continue
            if n < os_:
                break
            if os_ <= n < os_ + oc:
                return None
            delta += nc - oc
        return n + delta

    def def_files(self, rev: str, sym: str) -> List[str]:
        """rev 에서 sym 의 정의가 있는 파일들(하우스 스타일: 이름이 0열 또는 2칸)."""
        k = ('def', rev, sym)
        if k not in self._show:
            name = sym.split('::')[-1]
            out = self._git('grep', '-l', '-E', r'^\s{0,2}([A-Za-z_][A-Za-z0-9_]*::)?' + re.escape(name) + r'(_debug|_release)?\s*\(', rev, '--', 'src')[1]
            self._show[k] = [l.split(':', 1)[-1] for l in out.split('\n') if l.strip()][:4]
        return self._show[k]

    def resolve(self, rev: str, cited: str, hint: str = '') -> Tuple[Optional[str], List[str]]:
        files = self.files(rev)
        c = cited.lstrip('./')
        cands = [f for f in files if f == c or f.endswith('/' + c)]
        if len(cands) > 1:
            pref = [f for f in cands if hint and f.startswith(hint)] or [f for f in cands if f.startswith('src/')]
            if len(pref) == 1:
                cands = pref
        return (cands[0] if len(cands) == 1 else None), cands


def _items(text: str) -> List[Tuple[int, str]]:
    """(시작 줄, 항목 텍스트) — 불릿·번호 목록·표 행·제목은 한 줄씩, 그 밖의 문단은 빈 줄까지 묶는다(이어지는 줄 포함)."""
    out, cur, start = [], [], 0
    for i, ln in enumerate(text.split('\n'), 1):
        new_item = re.match(r'^\s*([-*]|\d+\.)\s+|^\s*\||^#{1,6}\s', ln)
        if not ln.strip() or new_item:
            if cur:
                out.append((start, '\n'.join(cur)))
            cur, start = ([ln], i) if ln.strip() else ([], 0)
            continue
        if not cur:
            start = i
        cur.append(ln)
    if cur:
        out.append((start, '\n'.join(cur)))
    return out


def doc_rev(text: str, repo: Repo) -> Optional[str]:
    for ln in text.split('\n')[:12]:
        for m in HEAD_REV.finditer(ln):
            if repo.commit(m.group(1)):
                return m.group(1)
    return None


def item_rev(item: str, repo: Repo) -> Optional[str]:
    m = CLAIM_PAREN.match(item)
    if m:
        for h in HEX.finditer(m.group(1)):
            if repo.commit(h.group(1)):
                return h.group(1)
    return None


def module_hint(doc: str, item: str) -> str:
    m = re.search(r'\[src-(\w+)', item) or re.search(r'src-(\w+)\.md$', doc)
    return 'src/%s/' % m.group(1) if m else ''


def _nums(first: str, more: str) -> List[Tuple[int, int]]:
    out = []
    for tok in [first] + MORE_NUM.findall(more or ''):
        a, _, b = tok.partition('-')
        a, b = int(a), int(b or a)
        if b >= a and b - a < 400:
            out.append((a, b))
    return out


DEF_LINE = re.compile(r'^[A-Za-z_][\w:~<>]*\s*\(')        # 하우스 스타일 정의 머리(이름이 0열) — tree-sitter 가 없을 때의 대안


def _sym_re(sym: str):
    s_ = sym.split('::')[-1]
    # `_with_escapes` 처럼 앞 이름에 붙는 접미어 표기, `USE_IDX`(PT_HINT_USE_IDX) 처럼 접두를 줄인 대문자 매크로는 앞 단어 경계 없이 찾는다
    lead = r'' if s_.startswith('_') or (s_.isupper() and '_' in s_) else r'\b'
    return re.compile(lead + re.escape(s_) + r'\b')


_FN_RANGES = {}


def _fn_ranges(key, path: str, lines: List[str]):
    """(시작, 끝, 이름) 목록 — static_lint 와 같은 tree-sitter 파싱(codegraph 전처리). 문법 라이브러리가 없으면 None."""
    if key not in _FN_RANGES:
        try:
            from . import static_lint as SL
            ctx = SL.FileCtx(path, '\n'.join(lines).encode('utf-8'), SL.GitSrc('.', None))
            _FN_RANGES[key] = [(f.start, f.end, f.name) for f in ctx.fns]
        except Exception:
            _FN_RANGES[key] = None
    return _FN_RANGES[key]


def _sym_ok(lines: List[str], a: int, b: int, sym: str, key=None, path: str = '') -> bool:
    """인용 줄 ±3 에 심볼이 있거나, 인용 줄이 그 이름의 함수 본문 안이다. 함수 범위는 tree-sitter 로(네임스페이스 안 C++·
    디버그/릴리스 쌍 포함), 없으면 위로 올라가며 다른 정의·0열 '}' 를 만나기 전에 그 정의 머리를 만나는지 본다."""
    rx = _sym_re(sym)
    if rx.search('\n'.join(lines[max(0, a - 4):b + 3])):
        return True
    rng = _fn_ranges(key, path, lines) if key is not None and path.endswith(('.c', '.cpp', '.cc', '.h', '.hpp')) else None
    if rng is not None:
        short = sym.split('::')[-1]
        return any(st <= a <= en and re.sub(r'_(debug|release)$', '', nm.split('::')[-1]) == short for st, en, nm in rng)
    for i in range(min(a, len(lines)) - 1, max(-1, a - 1 - 3000), -1):
        ln = lines[i]
        if ln.startswith('}'):                         # 앞 함수의 끝
            return False
        if DEF_LINE.match(ln) or re.match(r'^[A-Za-z_][\w\s\*&:<>,]*\s\**[A-Za-z_][\w:~]*\s*\(', ln):
            name_part = re.sub(r'_(debug|release)\s*$', '', ln.split('(')[0].rstrip())   # pgbuf_ordered_fix_debug/_release 쌍
            return bool(rx.search(name_part))
    return False


def _at_rev(repo: Repo, rev: str, path: str, hint: str, a: int, b: int, sym: Optional[str]):
    """rev 에서 인용이 맞나. 반환 (resolved 경로|None, 실패 이유|None, 후보 파일들)."""
    resolved, cands = repo.resolve(rev, path, hint)
    if resolved is None:
        return None, ('같은 이름 파일 %d개: %s' % (len(cands), ', '.join(cands[:4]))) if len(cands) > 1 else '기준 rev 에 그 파일이 없다', cands
    lines = repo.lines(rev, resolved) or []
    if b > len(lines):
        return resolved, '기준 rev 에서 줄 %d 이 파일 밖(%d줄)' % (b, len(lines)), cands
    if sym and not _sym_ok(lines, a, b, sym, (rev, resolved), resolved):
        return resolved, '인용 줄 근처(±3)에도, 그 이름의 함수 본문에도 `%s` 가 없다 — 줄 번호가 다른 곳을 가리킨다' % sym.split('::')[-1], cands
    return resolved, None, cands


def _syms_before(seg: str) -> List[str]:
    """인용 앞 조각에서 심볼들(순서대로) — `이름`·`이름 ()`·이름 () . 커밋 해시·숫자·경로는 뺀다."""
    out = []
    for m in re.finditer(r'`([A-Za-z_][A-Za-z0-9_:~]*)(?:\s*\(\s*\))?`|\b([A-Za-z_][A-Za-z0-9_]{3,})\s*\(\s*\)', seg):
        s_ = m.group(1) or m.group(2)
        if s_ and not s_.isdigit() and s_ != 'src' and not re.fullmatch(r'[0-9a-f]{7,40}', s_):
            out.append(s_)
    return out


def check_doc(doc: str, repo: Repo, default_rev: str, rel: str) -> List[Dict]:
    """문서 하나의 인용을 판정한다. 인용마다 rev 후보를 [인용 바로 앞(같은 항목 200자 안)의 커밋 해시, 항목 rev, 문서 rev] 순으로
    시도해 처음 맞는 rev 를 기준으로 삼는다 — "PR#8253(`e07b3b09a` 머지)이 … :151" 처럼 문장이 rev 를 바꿔 말하는 경우와,
    "16eb8474d 이전엔 …" 처럼 서술로만 해시가 나오는 경우를 함께 다룬다. 어느 rev 에서도 안 맞으면 error(첫 후보의 이유)."""
    with open(doc, encoding='utf-8', errors='replace') as f:
        text = f.read()
    drev = doc_rev(text, repo) or default_rev
    res = []
    for start, item in _items(text):
        body = re.sub(r'https?://\S+', ' ', item)
        irev = item_rev(body, repo) or drev
        hint = module_hint(rel, body)
        spans = [(m.start(), m.group('path'), m.group('n'), m.group('more')) for m in FILE_LINE.finditer(body)]
        taken = [(m.start(), m.end()) for m in FILE_LINE.finditer(body)]
        for m in BARE.finditer(body):
            if not any(s <= m.start() < e for s, e in taken):
                spans.append((m.start(), None, m.group('n'), m.group('more')))
        mentions = [(m.start(), m.group(1)) for m in FILE_ANY.finditer(body)]   # 줄 없는 파일 언급도 "앞의 파일" 이 된다
        for pos, path, n, more in sorted(spans, key=lambda x: x[0]):
            implicit = path is None
            if implicit:
                prev = [f for p_, f in mentions if p_ < pos]
                if not prev:
                    continue
                path = prev[-1]
            sm = SYM_BEFORE.search(body[max(0, pos - 90):pos])
            sym = (sm.group(1) or sm.group(2)) if sm else None
            if sym and (sym.isdigit() or sym == 'src' or '.' in sym or re.fullmatch(r'[0-9a-f]{7,40}', sym)):
                sym = None
            nums = _nums(n, more)
            # 표의 "| `a` / `b` | file:1 / :2 |" 처럼 앞 칸의 심볼 여럿과 이 칸의 줄 여럿이 같은 수면 순서대로 맞춘다(표 행에서만)
            paired = None
            pre = body[:pos]
            if body.lstrip().startswith('|') and pre.count('|') >= 2 and len(nums) > 1:
                k2 = pre.rfind('|')
                prev_cell = pre[pre.rfind('|', 0, k2) + 1:k2]
                if not FILE_LINE.search(pre[k2 + 1:]):          # 이 칸의 첫 인용일 때만
                    many = _syms_before(prev_cell)
                    paired = many if len(many) == len(nums) else None
            near_rev = next((h.group(1) for h in reversed(list(HEX.finditer(body[max(0, pos - 200):pos]))) if repo.commit(h.group(1))), None)
            revs = [r for i, r in enumerate([near_rev, irev, drev]) if r and r not in [near_rev, irev, drev][:i]]
            for k, (a, b) in enumerate(nums):
                csym = paired[k] if paired else sym
                rec = {'doc': rel, 'doc_line': start + body[:pos].count('\n'), 'cited': '%s:%s' % (path, a if a == b else '%d-%d' % (a, b)),
                       'symbol': csym}
                if implicit:
                    rec['implicit_file'] = True
                paths = [path]
                if implicit and re.search(r'\.(h|hpp)$', path):          # 파일명 없는 `:N` 이 헤더 다음에 오면 같은 이름 .c/.cpp 도 본다
                    paths += [re.sub(r'\.(h|hpp)$', '.c', path), re.sub(r'\.(h|hpp)$', '.cpp', path)]
                first_err, used = None, None
                for rv in revs:
                    for pth in paths:
                        resolved, err, cands = _at_rev(repo, rv, pth, hint, a, b, csym)
                        if err is None:
                            used = (rv, resolved)
                            break
                        if first_err is None:
                            first_err = (rv, err, cands)
                    if used:
                        break
                if used is None and implicit and csym:            # 파일명 없는 `:N` — 심볼이 정의된 파일에서 맞으면 그 파일로 읽는다
                    for rv in revs:
                        for pth in repo.def_files(rv, csym):
                            resolved, err, _ = _at_rev(repo, rv, pth, '', a, b, csym)
                            if err is None:
                                used = (rv, resolved)
                                rec['inferred_file'] = resolved
                                break
                        if used:
                            break
                if used is None:
                    hres, herr, _ = _at_rev(repo, repo.head, path, hint, a, b, csym)
                    rv, err, cands = first_err
                    if herr is None:                                     # 기준 rev 로는 안 맞고 HEAD 로는 맞다 → 항목의 rev 가 없거나 틀렸다
                        rec.update(rev=rv[:12], path=hres, status='norev',
                                   why='기준 rev(%s)에서는 안 맞고 HEAD 에서 맞다 — 항목에 그 사실을 확인한 rev 를 적는다(K-02)' % rv[:9])
                        res.append(rec)
                        continue
                    rec.update(rev=rv[:12], status='ambiguous' if err.startswith('같은 이름') else 'error',
                               why=err + (' — 파일명 없는 `:%d` 를 앞에 나온 %s 로 읽었다(다른 파일이면 파일명을 적는다)' % (a, path) if implicit else ''))
                    res.append(rec)
                    continue
                rv, resolved = used
                rec.update(rev=rv[:12], path=resolved)
                if repo.lines(repo.head, resolved) is None:
                    rec.update(status='gone', why='HEAD 에 파일이 없다(옮기거나 지웠다)')
                else:
                    na, nb = repo.map_line(rv, resolved, a), repo.map_line(rv, resolved, b)
                    if na is None or nb is None:
                        rec.update(status='changed', why='HEAD 에서 그 줄이 바뀌었거나 지워졌다 — 사실을 다시 확인할 것')
                    elif (na, nb) == (a, b):
                        rec.update(status='ok')
                    else:
                        rec.update(status='moved', head='%s:%s' % (path, na if na == nb else '%d-%d' % (na, nb)),
                                   why='내용은 그대로, HEAD 줄 번호가 %+d' % (na - a))
                res.append(rec)
    return res


STATUS_ORDER = ('ok', 'moved', 'changed', 'gone', 'norev', 'ambiguous', 'error')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__[__doc__.index('사용'):],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('docs', nargs='*')
    ap.add_argument('--repo', default=os.path.join(HOME, 'dev/sources/cubrid'))
    ap.add_argument('--head', default='upstream/develop')
    ap.add_argument('--rev', default='', help='문서·항목에 기준 rev 가 없을 때 쓸 rev(기본 --head)')
    ap.add_argument('--json', default='')
    ap.add_argument('--md', default='')
    ap.add_argument('--strict', action='store_true', help='moved·changed·gone 도 실패로')
    ap.add_argument('--show', type=int, default=8, help='문서마다 찍을 error·changed 줄 수')
    a = ap.parse_args(argv)
    repo = Repo(os.path.expanduser(a.repo), a.head)
    default_rev = a.rev or a.head
    docs = []
    for d in a.docs or DEFAULT_DOCS:
        d = os.path.expanduser(d)
        if os.path.isdir(d):
            for dp, dns, fns in os.walk(d):
                dns[:] = [x for x in dns if not x.startswith('.')]
                docs += [os.path.join(dp, f) for f in sorted(fns) if f.endswith('.md')]
        elif os.path.isfile(d):
            docs.append(d)
    allres, bad, stale = [], 0, 0
    for doc in docs:
        rel = os.path.relpath(doc, os.path.join(HOME, 'dev/docs'))
        res = check_doc(doc, repo, default_rev, rel)
        if not res:
            continue
        allres += res
        cnt = {s: sum(1 for r in res if r['status'] == s) for s in STATUS_ORDER}
        bad += cnt['error']
        stale += cnt['moved'] + cnt['changed'] + cnt['gone'] + cnt['norev']
        print('%s — 인용 %d · 그대로 %d · 줄 이동 %d · 바뀜 %d · 파일 없음 %d · rev 없음(HEAD 에서만 맞음) %d · 모호 %d · 기준에서 틀림 %d' % (
            rel, len(res), cnt['ok'], cnt['moved'], cnt['changed'], cnt['gone'], cnt['norev'], cnt['ambiguous'], cnt['error']))
        shown = [r for r in res if r['status'] in ('error', 'changed', 'gone')][:a.show]
        for r in shown:
            print('    %-8s %s:%d `%s`%s @%s — %s' % (r['status'], r['doc'].split('/')[-1], r['doc_line'], r['cited'],
                                                  (' (%s)' % r['symbol']) if r.get('symbol') else '', r['rev'][:9], r.get('why', '')))
    print('# 합계 문서 %d · 인용 %d · 기준에서 틀림 %d · 낡음(이동·바뀜·없음) %d · HEAD %s' % (
        len(docs), len(allres), bad, stale, repo.head[:9]), file=sys.stderr)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as f:
            json.dump({'head': repo.head, 'results': allres}, f, ensure_ascii=False, indent=1)
    if a.md:
        rows = ['| 문서:줄 | 인용 | 심볼 | 기준 rev | 판정 | HEAD | 이유 |', '|---|---|---|---|---|---|---|']
        for r in allres:
            if r['status'] != 'ok':
                rows.append('| %s:%d | `%s` | %s | %s | %s | %s | %s |' % (r['doc'], r['doc_line'], r['cited'], r.get('symbol') or '',
                                                                       r['rev'][:9], r['status'], r.get('head', ''), r.get('why', '')))
        with open(a.md, 'w', encoding='utf-8') as f:
            f.write('# source_facts_check — HEAD %s\n\n' % repo.head[:9] + '\n'.join(rows) + '\n')
    return 1 if bad or (a.strict and stale) else 0


if __name__ == '__main__':
    sys.exit(main())
