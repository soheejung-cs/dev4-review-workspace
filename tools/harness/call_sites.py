"""call_sites — 바뀐 판정·플래그 함수의 호출 자리 전부와 그 자리의 패스를 표로 낸다(H-177, 2026-11-13).

왜: PR#8489 는 `pt_check_union_is_foldable ()` 의 판정을 바꿨는데, 그 판정을 부르는 자리 둘(semantic_check.c 의미 검사·type_checking.c
    타입 계산)과 이름 해석의 donot_fold 타입 계산이 표시보다 먼저 접는 '패스 순서 구멍' 이 있었다 — 팩이 호출 자리를 싣지 않아 독립 판정이
    소스를 읽어 찾았다. 판정 함수를 바꾸면 그 판정을 소비하는 자리 전부가 리뷰 범위다.
대상 함수: 바뀐 줄(추가·삭제)에 조건식(if·while·switch·?:·논리·비교 return)이 있거나, 이름이 판정꼴(is·can·check·need·should·has·allow·
    ok·valid·possible·foldable·eligible 조각)인 바뀐 함수.
호출 자리: `git grep -n -w <이름>` 에서 들여쓴 줄의 `<이름> (` — 정의·선언(줄 맨 앞)·#define 은 뺀다. 그래프 범위(바뀐 디렉터리) 밖도 본다.
    둘러싼 함수는 위로 올라가 첫 '줄 맨 앞 이름 (' 줄(GNU 스타일 정의)로 정한다 — 매크로 안 호출·K&R 정의는 틀릴 수 있다.
패스: 파일 경로로 정한다(PASS 표). 실행 때 다시 타입을 계산하는 자리(예: 실행기의 pt_* 호출)는 파일로 구분되지 않는다 — 표의 함수 이름을 본다.
쓰임: pipeline 의 review_request 머리(preamble.md)와 implement_pack 의 머리에 section () 을 싣는다. 사이드이펙트 없음(git grep·파일 읽기만).
"""
import os
import re
import subprocess
from typing import Dict, List, Optional, Tuple

COND = re.compile(r'\bif\s*\(|\belse\s+if\b|\bwhile\s*\(|\bswitch\s*\(|\?[^:;]*:|\breturn\b[^;]*(&&|\|\||==|!=|<=|>=|[^-]>|<)')
PRED_NAME = re.compile(r'(^|_)(is|can|check|need|needs|should|has|allow|allows|ok|valid|possible|foldable|eligible)(_|$)')
HUNK = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@')
DEF_LINE = re.compile(r'^(?:[A-Za-z_][\w\s\*]*?[\s\*])?([A-Za-z_]\w*)\s*\([^;]*$')
KEYWORDS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'case', 'do', 'else'}
SRC_EXT = ('.c', '.cpp', '.cc', '.h', '.hpp', '.y', '.i')
# 파일 → 패스. 앞에서부터 처음 맞는 것(구체적인 파일을 디렉터리보다 먼저)
PASS = [('src/parser/csql_grammar.y', '구문 분석'), ('src/parser/name_resolution.c', '이름 해석'),
        ('src/parser/semantic_check.c', '의미 검사'), ('src/parser/type_checking.c', '타입 계산'),
        ('src/parser/view_transform.c', '뷰 변환'), ('src/parser/xasl_generation.c', 'XASL 생성'),
        ('src/parser/parse_tree_cl.c', '트리 출력·복사'), ('src/parser/', '파서'),
        ('src/optimizer/query_rewrite.c', '질의 재작성'), ('src/optimizer/', '최적화'),
        ('src/query/execute_schema.c', 'DDL 실행(클라이언트)'), ('src/query/execute_statement.c', '문장 실행(클라이언트)'),
        ('src/query/query_executor.c', '실행기(서버)'), ('src/query/scan_manager.c', '스캔(서버)'),
        ('src/query/expr_', '식 컴파일·배치(서버)'), ('src/query/', '질의 처리'), ('src/compat/', '클라이언트 API'),
        ('src/object/', '객체·스키마'), ('src/storage/', '저장소(서버)'), ('src/transaction/', '트랜잭션(서버)')]


def pass_of(path: str) -> str:
    return next((p for k, p in PASS if path.startswith(k)), '기타')


def _hunks(diff_text: str):
    """[(file, new_start, removed_texts, [(new_line, added_text)])]"""
    out, cur, h, n = [], None, None, 0
    for ln in diff_text.splitlines():
        if ln.startswith('+++ '):
            cur = ln[6:] if ln.startswith('+++ b/') else None
            continue
        if ln.startswith('--- ') or cur is None:
            continue
        m = HUNK.match(ln)
        if m:
            n = int(m.group(2))
            h = (cur, n, [], [])
            out.append(h)
            continue
        if h is None:
            continue
        if ln.startswith('-'):
            h[2].append(ln[1:])
        elif ln.startswith('+'):
            h[3].append((n, ln[1:]))
            n += 1
        elif not ln.startswith('\\'):
            n += 1
    return out


def predicate_functions(diff_text: str, fns) -> List[Tuple[str, str, int]]:
    """바뀐 함수 중 판정·플래그 함수 — fns 는 (file, name, start, end) 목록. [(file, name, start)]"""
    touched = {}
    for f, start, rem, add in _hunks(diff_text):
        for (ff, name, s, e) in fns:
            if ff != f:
                continue
            lines = [t for ln, t in add if s <= ln <= e] + (rem if s <= start <= e else [])
            if lines or (s <= start <= e):
                touched.setdefault((ff, name, s), []).extend(lines)
    out = []
    for (f, name, s), lines in sorted(touched.items()):
        if PRED_NAME.search(name) or any(COND.search(t) for t in lines):
            out.append((f, name, s))
    return out


def _read(repo: str, path: str, rev: Optional[str], cache: Dict[str, List[str]]) -> List[str]:
    if path not in cache:
        if rev:
            r = subprocess.run(['git', '-C', repo, 'show', '%s:%s' % (rev, path)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            cache[path] = r.stdout.decode('utf-8', 'replace').split('\n')
        else:
            try:
                with open(os.path.join(repo, path), encoding='utf-8', errors='replace') as fp:
                    cache[path] = fp.read().split('\n')
            except OSError:
                cache[path] = []
    return cache[path]


def enclosing_function(lines: List[str], line: int) -> Optional[str]:
    for i in range(line - 2, -1, -1):
        t = lines[i]
        if not t or t[0] in ' \t#{}/*':
            continue
        m = DEF_LINE.match(t)
        if m and m.group(1) not in KEYWORDS:
            return m.group(1)
    return None


def call_sites(repo: str, name: str, rev: Optional[str] = None, limit: int = 200) -> List[Dict]:
    args = ['git', '-C', repo, 'grep', '-n', '-w', '-e', name]
    args += ([rev] if rev else []) + ['--', 'src']
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    call = re.compile(r'\b%s\s*\(' % re.escape(name))
    out, cache = [], {}
    for row in r.stdout.decode('utf-8', 'replace').splitlines():
        if rev and row.startswith(rev + ':'):
            row = row[len(rev) + 1:]
        try:
            path, ln, text = row.split(':', 2)
        except ValueError:
            continue
        if not path.endswith(SRC_EXT) or not text[:1] in (' ', '\t') or not call.search(text):
            continue                                    # 정의·선언(줄 맨 앞)·#define 은 들여쓰지 않는다
        stripped = text.strip()
        if stripped.startswith(('*', '//', '/*')):
            continue
        out.append({'file': path, 'line': int(ln), 'caller': enclosing_function(_read(repo, path, rev, cache), int(ln)) or '?',
                    'pass': pass_of(path), 'text': stripped[:100]})
        if len(out) >= limit:
            break
    return out


def render(func: Tuple[str, str, int], sites: List[Dict], max_rows: int = 30) -> str:
    f, name, s = func
    rows = ['### `%s` (%s:%d) — 호출 %d곳' % (name, f, s, len(sites)), '', '| 호출 자리 | 둘러싼 함수 | 패스 | 줄 |', '|---|---|---|---|']
    for x in sites[:max_rows]:
        rows.append('| `%s:%d` | `%s` | %s | `%s` |' % (x['file'], x['line'], x['caller'], x['pass'], x['text'].replace('|', '\\|')))
    if len(sites) > max_rows:
        rows.append('| … %d곳 더 | | | |' % (len(sites) - max_rows))
    return '\n'.join(rows)


def section(repo: str, funcs: List[Tuple[str, str, int]], rev: Optional[str] = None, max_funcs: int = 8) -> str:
    """판정 함수들의 호출 자리 절(마크다운). 대상이 없으면 빈 문자열."""
    if not funcs:
        return ''
    parts = ['## 바뀐 판정·플래그 함수의 호출 자리 (H-177)', '',
             '판정을 바꾸면 그 판정을 부르는 자리 전부가 범위다 — 패스(이름 해석·의미 검사·타입 계산·실행)마다 바뀐 판정이 '
             '표시·접기보다 먼저 쓰이는 순서 구멍이 없는지 본다(PR#8489 의 의미 검사·타입 계산 두 자리). 그래프 범위 밖 호출자도 싣는다.', '']
    for fn in funcs[:max_funcs]:
        parts += [render(fn, call_sites(repo, fn[1], rev)), '']
    if len(funcs) > max_funcs:
        parts.append('- 판정 함수 %d개 더(생략): %s' % (len(funcs) - max_funcs, ', '.join('`%s`' % x[1] for x in funcs[max_funcs:])))
    return '\n'.join(parts) + '\n'
