"""static_lint — '모양으로 정해지는' 결함 후보를 결정론으로 찾아 findings.auto.json 에 싣는다(H-15·H-33).

리뷰 LLM 의 주의에 맡겼다가 놓친 모양들(근거 사례):
  fp-range-outside        실수 값을 바깥 꼴 범위 검사로 거른다 — `OR_CHECK_{SHORT,INT,BIGINT,USHRT,UINT}_OVERFLOW (x)` 또는 같은 꼴의
                          `x > MAX || x < MIN`. NaN 은 모든 비교에 거짓이라 통과하고, 뒤따르는 정수 좁힘이 UB 다
                          (자가진화 #004 — CBRD-27660/PR#8253: CAST('nan' AS SMALLINT) = 0, NaN → TIME 깨진 값). PR 이 실수 범위 검사를
                          건드리면(추가 줄에 범위 매크로·is_floating_point·정수 한계 비교) 같은 파일의 남은 바깥 꼴을 `-siblings` 한 건으로 모은다.
  discarded-status        encode/decode/coerce/convert/conversion/cast 류 함수가 돌려준 상태(int·bool·*_STATUS)를 문장으로 버린다
                          (자가진화 #005 — CBRD-27770: tp_make_time_conversion 의 ER_TIME_CONVERSION 을 버려 음수 → TIME 이 깨진 값).
                          정의의 return 이 전부 성공 상수인 함수(늘 성공)와 입력이 전부 상수인 호출(고정값 만들기)은 세기만 한다.
  error-path-continues    er_set (ER_ERROR_SEVERITY|ER_FATAL_ERROR_SEVERITY, …) 뒤 흐름을 끊지 않는다(META-REVIEW PR#7937 → H-33):
                          use-after-fail(if 조건에서 NULL 로 확인한 자원을 블록을 나간 뒤 역참조 — 확인된 꼴, non-blocking) · continue(question)
                          · loop-end·fall-through(정밀도가 낮아 notes 에만).
  untracked-aligned-alloc posix_memalign·aligned_alloc·memalign·valloc·pvalloc(·mmap)을 직접 부른다 — 메모리 모니터가 추적하는
                          cub_aligned_alloc 이 아니다(META-REVIEW PR#7658 → H-33).
  join-shape-checklist    카디널리티·선택도 모델 함수가 바뀌면 조인 형상 체크리스트를 테스트 제안으로 싣는다 — preamble 의
                          「자동 finding」 절로 리뷰 단계에 주입된다(META-REVIEW PR#7937 → H-33). 파이프라인에서만.

쓰임
  (1) 파이프라인 노드 `static_lint`(full·review·design 의 context 단계, or_buf 뒤): codegraph 의 변경 함수만 본다.
      변경 줄의 적중은 지적(non-blocking 또는 question), 같은 함수의 바뀌지 않은 줄에 있는 fp-range-outside·discarded-status 는
      선재 결함(baseline same — 첫 줄에 "develop 동일" 표시) question 으로 낸다. error-path-continues·untracked-aligned-alloc 은
      변경 줄만(diff 한정). findings.auto.json 에 append(perf_claims 가 먼저 쓴다), 집계·건너뛴 수는 OUT/static_lint.json.
  (2) CLI(읽기 전용, 체크아웃 없음 — `git show <rev>:<path>`):
        python3 -m tools.harness.static_lint --rev upstream/develop [--paths src/object src/query] [--checkers fp-range-outside,…]
                                             [--repo ~/dev/sources/cubrid] [--json out.json]
      파일 전체를 훑는다 — 오탐률 측정, 수정 전후 비교(예: --rev 7e81221d3 vs e07b3b09a --paths src/object/object_domain_convert.cpp).
출력: finding.json 스키마 + `auto: true`·`checker`. 판정 불가(인자 타입·반환 타입을 모름)는 지적하지 않고 `unknown` 으로 센다 —
      침묵을 '깨끗함' 으로 읽지 않는다(or_buf 와 같은 원칙). 사이드이펙트: OUT 파일 쓰기뿐(외부 호출 없음, git 은 읽기만).
한계: 타입은 함수 안 선언·매개변수·파일 전역·헤더 구조체 필드·알려진 수학 함수·반환 타입 색인으로만 정한다(typedef·매크로
      펼침·오버로드·auto 는 모름). NaN·실패 상태가 그 호출에 실제로 오는지(도달성)는 보지 않는다 — 자가진화 #008: 게시 전에
      호출자·입력 경로를 확인한다. error-path-continues 는 직선 구간만 따라가고, 오류 블록을 나간 뒤 WINDOW 문장 안에
      return·goto·오류 변수 확인이 오면 정상으로 본다.
develop 전체 실측(2d925cb86, 2026-10-28, 1187파일 60초): fp-range-outside 13(인자 타입 13/13 맞음 — NaN 도달은 8곳 불가·5곳 미확인) ·
      discarded-status 57(늘 성공 42·상수 입력 24 제외 — 표본 12: 실제 2·가능 3·무해 7, 무해는 대개 검증된 입력) · error-path-continues use-after-fail 3(3/3 실제 결함) ·
      continue 6(대부분 의도된 재시도·수집) · untracked-aligned-alloc 2(둘 다 mmap 파일 매핑 — 오탐). 정렬 할당 직접 호출은 0.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

from .codegraph import TS_LIB, _normalize_split_headers

SRC_EXT = ('.c', '.cpp', '.cc', '.cxx', '.h', '.hpp', '.hh')
CHECKERS = ('fp-range-outside', 'discarded-status', 'error-path-continues', 'untracked-aligned-alloc', 'join-shape-checklist')
DIFF_ONLY = {'error-path-continues', 'untracked-aligned-alloc'}      # 선재분까지 내면 저장소 전체의 관행을 다시 리뷰하게 된다
# error-path-continues 의 '블록을 빠져나가 계속'·'반복 끝' 은 develop 표본(2026-10-28, 15건)에서 대부분 의도된 '기록하고 계속'(정리 후 반환·
# 일관성 검사기의 전수 수집)이었다 — 파이프라인에서는 지적으로 내지 않고 static_lint.json 의 notes 에만 남긴다
NOTE_ONLY = {'fall-through', 'loop-end'}

# ------------------------------------------------------------------ 파싱 (codegraph 와 같은 전처리)
_PARSERS = {}
# codegraph.TreeSitterProvider.parse_file 과 같은 전처리 — 세미콜론 없는 최상위 매크로 호출 줄을 같은 길이 공백으로
MACRO_LINE = re.compile(rb'(?m)^[A-Z_][A-Z0-9_]*\s*\(.*\)\s*$')


def _parser(lang):
    if lang not in _PARSERS:
        from tree_sitter import Language, Parser
        p = Parser()
        p.set_language(Language(TS_LIB, lang))
        _PARSERS[lang] = p
    return _PARSERS[lang]


def _errors(root) -> int:
    if not root.has_error:
        return 0
    n, stack = 0, [root]
    while stack:
        x = stack.pop()
        if x.type == 'ERROR' or x.is_missing:
            n += 1
        stack.extend(x.children)
    return n


def parse(src: bytes, rel: str):
    """반환 (tree, 전처리한 src). .c·.h 는 C 로 먼저, 오류가 남으면 C++ 로도 파싱해 오류가 적은 쪽(.c 도 C++ 로 컴파일되는 저장소)."""
    src = MACRO_LINE.sub(lambda m: b' ' * len(m.group(0)), src)
    src = _normalize_split_headers(src)
    langs = ['cpp'] if rel.endswith(('.cpp', '.hpp', '.cc', '.cxx', '.hh')) else ['c', 'cpp']
    best = None
    for lang in langs:
        t = _parser(lang).parse(src)
        e = _errors(t.root_node)
        if best is None or e < best[1]:
            best = (t, e)
        if e == 0:
            break
    return best[0], src


def _walk(node):
    stack = [node]
    while stack:
        n = stack.pop()
        yield n
        stack.extend(reversed(n.children))


# ------------------------------------------------------------------ 소스 접근 (rev 또는 작업 트리, 읽기 전용)
class GitSrc(object):
    """rev 가 None 이면 repo 작업 트리(파이프라인의 worktree), 아니면 git show/grep <rev> — 어느 쪽도 체크아웃하지 않는다."""

    def __init__(self, repo: str, rev: Optional[str] = None):
        self.repo, self.rev = repo, rev
        self._read, self._fields, self._ret = {}, None, {}

    def _git(self, *args) -> str:
        r = subprocess.run(['git', '-C', self.repo] + list(args), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return r.stdout.decode('utf-8', 'replace')

    def files(self, paths: List[str]) -> List[str]:
        out = self._git('ls-tree', '-r', '--name-only', self.rev, '--', *paths) if self.rev else self._git('ls-files', '--', *paths)
        return sorted(f for f in out.splitlines() if f.endswith(SRC_EXT))

    def read(self, rel: str) -> bytes:
        if rel not in self._read:
            if self.rev:
                r = subprocess.run(['git', '-C', self.repo, 'show', f'{self.rev}:{rel}'], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                self._read[rel] = r.stdout if r.returncode == 0 else b''
            else:
                try:
                    with open(os.path.join(self.repo, rel), 'rb') as f:
                        self._read[rel] = f.read()
                except OSError:
                    self._read[rel] = b''
        return self._read[rel]

    def grep(self, pattern: str, specs: List[str], before: int = 0) -> str:
        args = ['grep', '-n', '-E', '-I'] + (['-B', str(before)] if before else []) + [pattern]
        if self.rev:
            args.append(self.rev)
        return self._git(*args, '--', *specs)

    # 헤더 구조체 필드 이름 → 타입들. 같은 이름이 여러 구조체에 다른 타입으로 있으면 실수로 보지 않는다(보수적).
    # 정규식은 줄 하나에 선형으로 — 중첩 반복(쉼표 목록 전체를 한 식으로)은 맞지 않는 줄에서 지수 역추적이 난다(2026-10-28 실측)
    FIRST = re.compile(r'^\s*(?P<type>(?:(?:const|volatile|unsigned|signed|long|short|struct|enum|union)\s+)*[A-Za-z_]\w*)'
                       r'\s*(?P<ptr>\**)\s*(?P<name>[A-Za-z_]\w*)\s*(?P<arr>\[[^\]]*\])?\s*$')
    REST = re.compile(r'^\s*(?P<ptr>\**)\s*(?P<name>[A-Za-z_]\w*)\s*(?P<arr>\[[^\]]*\])?\s*$')
    NOT_TYPE = {'return', 'goto', 'else', 'case', 'typedef', 'break', 'continue', 'do', 'delete', 'throw'}

    def field_types(self) -> Dict[str, set]:
        if self._fields is None:
            self._fields = {}
            out = self.grep(r'^\s+[A-Za-z_][A-Za-z0-9_ ]*[ *]+[A-Za-z_]\w*\s*(\[[^]]*\])?\s*(,\s*\**\s*[A-Za-z_]\w*\s*)*;', ['src/*.h', 'src/*.hpp'])
            for ln in out.splitlines():
                body = ln.split(':', 3)[-1] if self.rev else ln.split(':', 2)[-1]
                body = re.split(r'/\*|//', body, maxsplit=1)[0].rstrip()
                if not body.endswith(';'):
                    continue
                parts = body[:-1].split(',')
                m = self.FIRST.match(parts[0])
                if not m or m.group('type').split()[-1] in self.NOT_TYPE:
                    continue
                ty = m.group('type')
                decls = [m] + [self.REST.match(x) for x in parts[1:]]
                for d in decls:
                    if d is not None:
                        self._fields.setdefault(d.group('name'), set()).add(ty + ('*' if d.group('ptr') or d.group('arr') else ''))
        return self._fields

    def definition(self, name: str):
        """하우스 스타일 정의(이름이 0열)의 (파일, 줄) — 하나일 때만. 여럿(static 동명·디버그 쌍)이면 None."""
        key = ('def', name)
        if key not in self._ret:
            hits = []
            for ln in self.grep('^' + name + r' ?\(', ['src/*.c', 'src/*.cpp', 'src/*.h', 'src/*.hpp']).splitlines():
                parts = ln.split(':', 3) if self.rev else ln.split(':', 2)
                parts = parts[1:] if self.rev else parts
                if len(parts) >= 3 and parts[1].isdigit() and not parts[2].rstrip().endswith(';'):
                    hits.append((parts[0], int(parts[1])))
            self._ret[key] = hits[0] if len(hits) == 1 else None
        return self._ret[key]

    def ctx(self, rel: str):
        """파일 하나의 FileCtx(캐시) — 다른 파일에 정의된 함수의 본문을 볼 때."""
        key = ('ctx', rel)
        if key not in self._ret:
            raw = self.read(rel)
            self._ret[key] = FileCtx(rel, raw, self) if raw else None
        return self._ret[key]

    def ret_type(self, name: str) -> Optional[str]:
        """함수 반환 타입(정규화) — 헤더 프로토타입과 하우스 스타일 정의(이름이 0열, 타입은 앞 줄). 둘 이상이 다르면 None."""
        if name in self._ret:
            return self._ret[name]
        found = set()
        proto = re.compile(r'^\s*(?:extern\s+)?(?:static\s+)?(?:inline\s+)?(?:STATIC_INLINE\s+)?(?P<type>(?:const\s+|unsigned\s+|signed\s+|struct\s+|enum\s+)*[A-Za-z_]\w*(?:\s+long|\s+int)?)\s*(?P<ptr>\**)\s*' + re.escape(name) + r'\s*\(')
        for ln in self.grep(r'\b' + name + r'\s*\(', ['src/*.h', 'src/*.hpp']).splitlines():
            body = ln.split(':', 3)[-1] if self.rev else ln.split(':', 2)[-1]
            m = proto.match(body)
            if m and m.group('type') not in ('return', 'if', 'while', 'else', 'case', 'sizeof'):
                found.add(norm_type(m.group('type')) + m.group('ptr'))
        if not found:      # 정의: `name (` 가 0열, 앞 줄이 반환 타입
            out = self.grep('^' + name + r' ?\(', ['src/*.c', 'src/*.cpp', 'src/*.h', 'src/*.hpp'], before=1)
            lines = out.splitlines()
            for i, ln in enumerate(lines):
                if re.search(r'[:-]\d+:' + re.escape(name) + r' ?\(', ln) and i > 0:
                    prev = re.split(r'-\d+-', lines[i - 1], maxsplit=1)[-1].strip()
                    if prev and not prev.endswith((';', '}', '{', ')', ',')) and not prev.startswith(('#', '/', '*')):
                        found.add(norm_type(prev))
        self._ret[name] = found.pop() if len(found) == 1 else None
        return self._ret[name]


def norm_type(t: str) -> str:
    t = re.sub(r'\b(static|extern|inline|STATIC_INLINE|const|volatile|register|virtual|constexpr|__inline__|__attribute__\s*\(\(.*?\)\))\b', ' ', t)
    return re.sub(r'\s+', ' ', t).strip()


FLOAT_T = re.compile(r'^(long )?(double|float)$')
INT_T = re.compile(r'^((unsigned|signed) )?((long long|long|short|char|int|unsigned|signed|bool|_Bool|size_t|ssize_t|time_t|off_t'
                   r'|u?int(8|16|32|64)_t|DB_BIGINT|DB_TIME|DB_DATE|DB_TIMESTAMP|INT(8|16|32|64)|UINT(8|16|32|64)|BIGINT|PAGEID|VOLID'
                   r'|LOG_PAGEID|MVCCID|TRANID|UINTPTR|INTPTR)( int)?)$')


def classify(t: str) -> Optional[str]:
    """정규화한 타입 → 'float' | 'int' | None. 포인터('*')는 값이 아니므로 None."""
    t = norm_type(t)
    if not t or '*' in t or '&' in t:
        return None
    if FLOAT_T.match(t):
        return 'float'
    if INT_T.match(t):
        return 'int'
    return None


# ------------------------------------------------------------------ 파일·함수 문맥
def text(src: bytes, n) -> str:
    return src[n.start_byte:n.end_byte].decode('utf-8', 'replace')


def _decl_name(n):
    """declarator 노드에서 (이름 노드, 포인터 깊이, 배열 여부). 함수 선언자(프로토타입·함수 포인터)면 (None, 0, False)."""
    ptr, arr = 0, False
    while n is not None:
        t = n.type
        if t in ('identifier', 'field_identifier'):
            return n, ptr, arr
        if t == 'function_declarator':
            return None, 0, False
        if t == 'pointer_declarator':
            ptr += 1
        elif t == 'array_declarator':
            arr = True
        nxt = n.child_by_field_name('declarator')
        if nxt is None:      # reference_declarator·parenthesized_declarator 는 declarator 필드가 없다
            nxt = next((c for c in n.named_children if c.type not in ('type_qualifier', 'number_literal', 'ms_pointer_modifier')), None)
        n = nxt
    return None, 0, False


class Fn(object):
    __slots__ = ('node', 'name', 'start', 'end', 'tparams', 'vars')

    def __init__(self, node, name, tparams):
        self.node, self.name, self.tparams = node, name, tparams
        self.start, self.end = node.start_point[0] + 1, node.end_point[0] + 1
        self.vars = {}


class FileCtx(object):
    """한 파일의 파싱 결과 + 타입 질의. gs 는 헤더 필드·반환 타입 색인(같은 rev)."""

    def __init__(self, rel: str, raw: bytes, gs: GitSrc):
        self.rel, self.gs = rel, gs
        self.tree, self.src = parse(raw, rel)
        self.globals, self.local_ret = {}, {}
        root = self.tree.root_node
        for c in root.named_children:
            if c.type == 'declaration':
                self._add_decl(c, self.globals)
        self.fns = []
        for n in _walk(root):
            if n.type == 'function_definition':
                name = self._fn_name(n)
                tps = set()
                if n.parent is not None and n.parent.type == 'template_declaration':
                    pl = n.parent.child_by_field_name('parameters')
                    for p in (pl.named_children if pl is not None else []):
                        if p.type in ('type_parameter_declaration', 'variadic_type_parameter_declaration', 'optional_type_parameter_declaration'):
                            ti = next((x for x in p.named_children if x.type == 'type_identifier'), None)
                            if ti is not None:
                                tps.add(text(self.src, ti))
                f = Fn(n, name, tps)
                self._collect_vars(f)
                self.fns.append(f)
                rt = n.child_by_field_name('type')
                decl = n.child_by_field_name('declarator')
                if rt is not None and name:
                    self.local_ret.setdefault(name.split('::')[-1], set()).add(
                        norm_type(text(self.src, rt)) + ('*' if decl is not None and decl.type == 'pointer_declarator' else ''))

    def _fn_name(self, n) -> str:
        d = n.child_by_field_name('declarator')
        while d is not None and d.type not in ('identifier', 'field_identifier', 'qualified_identifier', 'destructor_name', 'operator_name'):
            nx = d.child_by_field_name('declarator')
            d = nx if nx is not None else (d.named_children[0] if d.named_children else None)
        return text(self.src, d) if d is not None else '?'

    def _add_decl(self, decl, into, tparams=()):
        ty = decl.child_by_field_name('type')
        if ty is None:
            return
        tt = norm_type(text(self.src, ty))
        for d in decl.children_by_field_name('declarator'):
            if d.type == 'init_declarator':
                d = d.child_by_field_name('declarator')
            ident, ptr, arr = _decl_name(d)
            if ident is None:
                continue
            into.setdefault(text(self.src, ident), set()).add((tt, ptr, arr))

    def _collect_vars(self, f: Fn):
        decl = f.node.child_by_field_name('declarator')
        while decl is not None and decl.type != 'function_declarator':
            decl = decl.child_by_field_name('declarator')
        params = decl.child_by_field_name('parameters') if decl is not None else None
        for p in (params.named_children if params is not None else []):
            if p.type in ('parameter_declaration', 'optional_parameter_declaration'):
                self._add_decl(p, f.vars)
        body = f.node.child_by_field_name('body')
        for n in _walk(body) if body is not None else []:
            if n.type in ('declaration', 'condition_declaration', 'for_range_loop'):
                if n.type == 'declaration':
                    self._add_decl(n, f.vars)

    # ---------------------------------------------------------- 식의 '실수 여부'
    def var_kind(self, f: Fn, name: str) -> Optional[str]:
        ent = f.vars.get(name) or self.globals.get(name)
        if not ent:
            return None
        kinds = set()
        for tt, ptr, arr in ent:
            if ptr or arr:
                kinds.add(None)
            elif tt in f.tparams:
                kinds.add('tmpl')
            else:
                kinds.add(classify(tt))
        return kinds.pop() if len(kinds) == 1 else None

    def elem_kind(self, f: Fn, name: str) -> Optional[str]:
        """p[i]·*p 의 원소 타입 — 포인터·배열 한 단계."""
        ent = f.vars.get(name) or self.globals.get(name)
        if not ent:
            return None
        kinds = {classify(tt) if (ptr + (1 if arr else 0)) == 1 else None for tt, ptr, arr in ent}
        return kinds.pop() if len(kinds) == 1 else None

    def ret_kind(self, name: str) -> Optional[str]:
        if name in FP_FUNCS:
            return 'float'
        loc = self.local_ret.get(name)
        if loc and len(loc) == 1:
            return classify(next(iter(loc)))
        rt = self.gs.ret_type(name)
        return classify(rt) if rt else None

    def kind(self, f: Fn, n) -> Optional[str]:
        t = n.type
        if t == 'parenthesized_expression':
            inner = [c for c in n.named_children]
            return self.kind(f, inner[-1]) if inner else None
        if t == 'cast_expression':
            td = n.child_by_field_name('type')
            return classify(text(self.src, td)) if td is not None else None
        if t == 'number_literal':
            s = text(self.src, n).lower()
            if s.startswith('0x'):
                return 'int'
            return 'float' if re.search(r'[.e]|f$', s) else 'int'
        if t == 'identifier':
            return self.var_kind(f, text(self.src, n))
        if t == 'field_expression':
            fld = n.child_by_field_name('field')
            if fld is None:
                return None
            # 값으로 쓰인 필드이므로 포인터·배열로 선언된 같은 이름은 빼고 본다(`amount` 는 DB_MONETARY 의 double 과 다른 구조체의 포인터가 있다)
            types = {x for x in self.gs.field_types().get(text(self.src, fld), ()) if not x.endswith('*')}
            if not types:
                return None
            kinds = {classify(x) for x in types}
            return kinds.pop() if len(kinds) == 1 else None
        if t == 'call_expression':
            fn = n.child_by_field_name('function')
            if fn is None:
                return None
            name = text(self.src, fn).split('::')[-1].split('->')[-1].split('.')[-1]
            return self.ret_kind(name)
        if t == 'binary_expression':
            op = n.child_by_field_name('operator')
            op = text(self.src, op) if op is not None else ''
            if op not in ('+', '-', '*', '/', '%'):
                return 'int'                                # 비교·논리 연산의 결과는 정수
            ks = [self.kind(f, n.child_by_field_name('left')), self.kind(f, n.child_by_field_name('right'))]
            if 'float' in ks:
                return 'float'
            if 'tmpl' in ks:
                return 'tmpl'
            return 'int' if ks == ['int', 'int'] else None
        if t == 'unary_expression':
            op = n.child_by_field_name('operator')
            arg = n.child_by_field_name('argument')
            if op is not None and text(self.src, op) in ('-', '+') and arg is not None:
                return self.kind(f, arg)
            return 'int' if op is not None and text(self.src, op) in ('!', '~') else None
        if t == 'conditional_expression':
            ks = [self.kind(f, n.child_by_field_name('consequence')), self.kind(f, n.child_by_field_name('alternative'))]
            if 'float' in ks:
                return 'float'
            if 'tmpl' in ks:
                return 'tmpl'
            return 'int' if ks == ['int', 'int'] else None
        if t == 'subscript_expression':
            arg = n.child_by_field_name('argument')
            return self.elem_kind(f, text(self.src, arg)) if arg is not None and arg.type == 'identifier' else None
        if t == 'pointer_expression':
            arg = n.child_by_field_name('argument')
            op = text(self.src, n.children[0]) if n.children else ''
            if op == '*' and arg is not None and arg.type == 'identifier':
                return self.elem_kind(f, text(self.src, arg))
            return None
        return None


# 반환 타입이 실수인 C 표준 수학·변환 함수(헤더 색인 밖) + CUBRID 접근자
FP_FUNCS = {'floor', 'ceil', 'round', 'trunc', 'rint', 'nearbyint', 'fabs', 'sqrt', 'pow', 'fmod', 'exp', 'log', 'log10', 'log2',
            'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh', 'ldexp', 'frexp', 'modf', 'strtod', 'atof',
            'strtof', 'strtold', 'floorf', 'ceilf', 'roundf', 'truncf', 'fabsf', 'db_get_double', 'db_get_float'}

# ------------------------------------------------------------------ checker 1: fp-range-outside (#004)
OUTSIDE = {  # 매크로 → (안쪽 꼴에 쓸 MIN, MAX) — object_representation.h 의 바깥 꼴 정수 범위 매크로
    'OR_CHECK_SHORT_OVERFLOW': ('DB_INT16_MIN', 'DB_INT16_MAX'), 'OR_CHECK_INT_OVERFLOW': ('DB_INT32_MIN', 'DB_INT32_MAX'),
    'OR_CHECK_BIGINT_OVERFLOW': ('DB_BIGINT_MIN', 'DB_BIGINT_MAX'), 'OR_CHECK_USHRT_OVERFLOW': ('0', 'DB_UINT16_MAX'),
    'OR_CHECK_UINT_OVERFLOW': ('0', 'DB_UINT32_MAX')}
INT_LIMITS = {'DB_INT16_MAX', 'DB_INT16_MIN', 'DB_INT32_MAX', 'DB_INT32_MIN', 'DB_BIGINT_MAX', 'DB_BIGINT_MIN', 'DB_UINT16_MAX',
              'DB_UINT32_MAX', 'SHRT_MAX', 'SHRT_MIN', 'INT_MAX', 'INT_MIN', 'LONG_MAX', 'LONG_MIN', 'LLONG_MAX', 'LLONG_MIN',
              'USHRT_MAX', 'UINT_MAX', 'INT16_MAX', 'INT16_MIN', 'INT32_MAX', 'INT32_MIN', 'INT64_MAX', 'INT64_MIN', 'UINT16_MAX', 'UINT32_MAX'}


class Hit(object):
    __slots__ = ('checker', 'file', 'line', 'fn', 'detail', 'level', 'evidence', 'facts')

    def __init__(self, checker, file, line, fn, detail, level, evidence=None, facts=None):
        self.checker, self.file, self.line, self.fn, self.detail, self.level = checker, file, line, fn, detail, level
        self.evidence, self.facts = evidence or [], facts or {}

    def as_dict(self):
        return {'checker': self.checker, 'file': self.file, 'line': self.line, 'function': self.fn, 'level': self.level,
                'detail': self.detail, 'facts': self.facts}


def _is_constexpr_if(n):
    return n is not None and n.type == 'if_statement' and any(c.type == 'constexpr' for c in n.children)


def _cond_text(ctx, ifn) -> str:
    c = ifn.child_by_field_name('condition')
    return text(ctx.src, c) if c is not None else ''


def _tmpl_state(ctx: FileCtx, call, tparams) -> str:
    """템플릿 인자 T 가 이 호출 자리에서 실수일 수 있나: 'float'(실수 분기 안) | 'int'(가드됨) | 'maybe'."""
    fp = r'is_floating_point\s*<\s*(%s)\s*>' % '|'.join(re.escape(t) for t in tparams)
    integral = r'is_integral\s*<\s*(%s)\s*>' % '|'.join(re.escape(t) for t in tparams)
    child, a = call, call.parent
    while a is not None and a.type != 'function_definition':
        if _is_constexpr_if(a):
            cond = _cond_text(ctx, a)
            cons = a.child_by_field_name('consequence')
            in_cons = cons is not None and cons.start_byte <= call.start_byte < cons.end_byte
            neg = re.search(r'!\s*(std::)?' + fp, cond)
            if in_cons and (re.search(integral, cond) or neg):
                return 'int'
            if in_cons and re.search(fp, cond):
                return 'float'
            if not in_cons and re.search(fp, cond) and not neg:
                return 'int'                                 # else 가지
        if a.type == 'compound_statement':
            sib = child.prev_named_sibling
            while sib is not None:                           # 앞선 `if constexpr (is_floating_point<T>) { … return …; }` 가 실수를 걸렀다
                if _is_constexpr_if(sib) and re.search(fp, _cond_text(ctx, sib)) and not re.search(r'!\s*(std::)?' + fp, _cond_text(ctx, sib)):
                    cons = sib.child_by_field_name('consequence')
                    last = cons.named_children[-1] if cons is not None and cons.named_children else cons
                    if last is not None and last.type == 'return_statement' and sib.child_by_field_name('alternative') is None:
                        return 'int'
                sib = sib.prev_named_sibling
        child, a = a, a.parent
    return 'maybe'


def check_fp_range(ctx: FileCtx, f: Fn, stats) -> List[Hit]:
    out = []
    body = f.node.child_by_field_name('body')
    for n in _walk(body) if body is not None else []:
        macro, arg, limits = None, None, {}
        if n.type == 'call_expression':
            fn = n.child_by_field_name('function')
            name = text(ctx.src, fn) if fn is not None else ''
            if name in OUTSIDE:
                args = n.child_by_field_name('arguments')
                named = [c for c in args.named_children] if args is not None else []
                if named:
                    macro, arg = name, named[0]
        elif n.type == 'binary_expression' and n.child_by_field_name('operator') is not None and text(ctx.src, n.child_by_field_name('operator')) == '||':
            sides = [n.child_by_field_name('left'), n.child_by_field_name('right')]
            cmp = []
            for s in sides:
                while s is not None and s.type == 'parenthesized_expression' and s.named_children:
                    s = s.named_children[-1]
                if s is None or s.type != 'binary_expression':
                    break
                op = text(ctx.src, s.child_by_field_name('operator'))
                l, r = s.child_by_field_name('left'), s.child_by_field_name('right')
                if op in ('>', '>=', '<', '<=') and r is not None and text(ctx.src, r) in INT_LIMITS:
                    cmp.append((text(ctx.src, l), l, op, text(ctx.src, r)))
                elif op in ('>', '>=', '<', '<=') and l is not None and text(ctx.src, l) in INT_LIMITS:
                    cmp.append((text(ctx.src, r), r, {'<': '>', '<=': '>=', '>': '<', '>=': '<='}[op], text(ctx.src, l)))
            if len(cmp) == 2 and cmp[0][0] == cmp[1][0] and {cmp[0][2][0], cmp[1][2][0]} == {'>', '<'}:
                macro, arg = 'x > MAX || x < MIN', cmp[0][1]
                limits = {c[2][0]: c[3] for c in cmp}       # '>' 쪽이 MAX, '<' 쪽이 MIN
        if macro is None:
            continue
        k = ctx.kind(f, arg)
        line = n.start_point[0] + 1
        argt = text(ctx.src, arg)
        if k == 'tmpl':
            st = _tmpl_state(ctx, n, f.tparams)
            if st == 'int':
                continue
            k, why = 'float', ('템플릿 인자가 실수인 분기 안' if st == 'float' else '템플릿 인자 %s 가 실수로 인스턴스화될 수 있는데 is_floating_point 가드가 없다' % '/'.join(sorted(f.tparams)))
        else:
            why = ''
        if k == 'float':
            lo, hi = OUTSIDE.get(macro) or (limits.get('<', 'MIN'), limits.get('>', 'MAX'))
            out.append(Hit('fp-range-outside', ctx.rel, line, f.name, '%s (%s)%s' % (macro, argt, (' — ' + why) if why else ''),
                           'high', facts={'macro': macro, 'arg': argt, 'lo': lo, 'hi': hi, 'arg_note': why or _type_note(ctx, f, arg)}))
        elif k is None:
            stats['unknown'] += 1
    return out


def _type_note(ctx: FileCtx, f: Fn, arg) -> str:
    a = arg
    while a.type == 'parenthesized_expression' and a.named_children:
        a = a.named_children[-1]
    if a.type == 'identifier':
        ent = f.vars.get(text(ctx.src, a)) or ctx.globals.get(text(ctx.src, a))
        if ent:
            return '`%s` 는 %s 로 선언' % (text(ctx.src, a), '/'.join(sorted(t for t, _, _ in ent)))
    if a.type == 'call_expression':
        fn = a.child_by_field_name('function')
        return '`%s` 의 반환이 실수' % (text(ctx.src, fn) if fn is not None else '?')
    if a.type == 'field_expression':
        fld = a.child_by_field_name('field')
        return '필드 `%s` 가 실수(헤더 선언)' % (text(ctx.src, fld) if fld is not None else '?')
    return '식의 피연산자 중 실수가 있다'


# ------------------------------------------------------------------ checker 2: discarded-status (#005)
STATUS_NAME = re.compile(r'(^|_)(encode|decode|coerce|convert|conversion|cast)(_|$)')
STATUS_T = re.compile(r'^(int|bool|_Bool|TP_DOMAIN_STATUS|DB_DOMAIN_STATUS|\w+_STATUS|\w+_status)$')


OK_RETURNS = {'NO_ERROR', '0', 'true', 'DOMAIN_COMPATIBLE'}


def _always_ok(ctx: FileCtx, name: str) -> Optional[bool]:
    """정의의 모든 return 이 성공 상수(NO_ERROR·0·true·DOMAIN_COMPATIBLE)면 True — 버려도 잃는 것이 없다(db_datetime_decode 등).
    정의를 못 찾으면 None(판정 안 함)."""
    cand = [(ctx, f) for f in ctx.fns if f.name.split('::')[-1] == name]
    if not cand:
        loc = ctx.gs.definition(name)
        other = ctx.gs.ctx(loc[0]) if loc else None
        cand = [(other, f) for f in (other.fns if other else []) if f.name.split('::')[-1] == name and f.start <= loc[1] <= f.end]
    if len(cand) != 1:
        return None
    c, f = cand[0]
    rets = [n for n in _walk(f.node) if n.type == 'return_statement']
    vals = {re.sub(r'^return\s*|\s*;$|[()\s]', '', text(c.src, r)) for r in rets}
    return bool(rets) and vals <= OK_RETURNS


CONST_ARG = re.compile(r'^(-?\d[\w.]*|"[^"]*"|\'[^\']*\'|[A-Z][A-Z0-9_]*|NULL|true|false|strlen\s*\(\s*[A-Z][A-Z0-9_]*\s*\))$')
ERR_ARG = re.compile(r'^((date_|time_|tz_)?err(or)?(_p|_code)?|\w*error\w*)$')


def _const_inputs(ctx: FileCtx, call) -> bool:
    """첫·끝 인자의 &x(만들 값)와 error 류 출력 인자를 빼면 입력이 전부 상수(리터럴·대문자 매크로·strlen (상수))인가 — 그러면 실패할 수 없는
    호출로 본다(db_date_encode (&date, 12, 31, 1970)·numeric_coerce_string_to_num (DB_SERIAL_MAX, …, &e38) 같은 고정값 만들기). 가운데 &x 는
    입력을 포인터로 넘기는 것일 수 있어(db_timestamp_encode_ses (&date, &time, …)) 상수로 보지 않는다."""
    args = call.child_by_field_name('arguments')
    vals = [re.sub(r'\s+', ' ', text(ctx.src, a)).strip() for a in (args.named_children if args is not None else [])]
    ins = [v for i, v in enumerate(vals) if not (i in (0, len(vals) - 1) and v.startswith('&')) and not ERR_ARG.match(v)]
    return bool(ins) and all(CONST_ARG.match(v) for v in ins)


def check_discarded(ctx: FileCtx, f: Fn, stats, resolve=None) -> List[Hit]:
    """resolve(name) -> 반환 타입 문자열|None 을 주면 그것을 먼저 쓴다(파이프라인: codegraph 해석 결과).
    정의의 return 이 전부 성공 상수인 함수(늘 성공)는 버려도 결함이 아니므로 세기만 한다(stats['always_ok'])."""
    out = []
    body = f.node.child_by_field_name('body')
    for n in _walk(body) if body is not None else []:
        if n.type != 'call_expression' or n.parent is None or n.parent.type != 'expression_statement':
            continue
        fn = n.child_by_field_name('function')
        if fn is None or fn.type not in ('identifier', 'qualified_identifier'):
            continue
        name = text(ctx.src, fn).split('::')[-1]
        if not STATUS_NAME.search(name):
            continue
        rt = resolve(name) if resolve else None
        if rt is None:
            loc = ctx.local_ret.get(name)
            rt = next(iter(loc)) if loc and len(loc) == 1 else ctx.gs.ret_type(name)
        if rt is None:
            stats['unknown'] += 1
            continue
        rt = norm_type(rt)
        if rt == 'void' or '*' in rt or not STATUS_T.match(rt):
            continue
        if _always_ok(ctx, name):
            stats['always_ok'] = stats.get('always_ok', 0) + 1
            continue
        if _const_inputs(ctx, n):
            stats['const_args'] = stats.get('const_args', 0) + 1
            continue
        ft = f.node.child_by_field_name('type')
        out.append(Hit('discarded-status', ctx.rel, n.start_point[0] + 1, f.name, '%s → %s 를 버림' % (name, rt), 'high',
                       facts={'callee': name, 'rtype': rt, 'call': text(ctx.src, n)[:160],
                              'ftype': norm_type(text(ctx.src, ft)) if ft is not None else ''}))
    return out


# ------------------------------------------------------------------ checker 3: error-path-continues (META-REVIEW PR#7937)
ER_SET = {'er_set', 'er_set_with_oserror'}
ERR_SEV = {'ER_ERROR_SEVERITY', 'ER_FATAL_ERROR_SEVERITY'}
EXIT_STMT = {'return_statement', 'goto_statement', 'break_statement', 'throw_statement'}
NORETURN = re.compile(r'^\s*((abort|exit|_exit)\s*\(|assert_release\s*\(\s*(false|0)\s*\))')
LOOPS = {'for_statement', 'while_statement', 'do_statement', 'for_range_loop'}
BLOCKS = ('compound_statement', 'case_statement')


def _lhs_vars(ctx, stmt) -> set:
    out = set()
    for n in _walk(stmt):
        if n.type == 'assignment_expression':
            l = n.child_by_field_name('left')
            if l is not None:
                out |= set(re.findall(r'[A-Za-z_]\w*', text(ctx.src, l)))
    return out


def _next_stmt(n):
    s = n.next_named_sibling
    while s is not None and s.type == 'comment':
        s = s.next_named_sibling
    return s


def _idents(ctx, n) -> set:
    return set(re.findall(r'[A-Za-z_]\w*', text(ctx.src, n)))


# 흐름을 끊거나 오류를 확인하는 매크로(CHECK_PTR·CHECK_PARSE_ERR·ASSERT_ERROR_AND_SET·GOTO_EXIT_ON_ERROR …)는 안을 펼치지 않고 '확인' 으로 본다
CHECK_MACRO = re.compile(r'^\s*[A-Z0-9_]*(CHECK|ERR|EXIT|GOTO|RETURN|FAIL)[A-Z0-9_]*\s*[(;]')   # GOTO_EXIT_ON_ERROR; 처럼 인자 없는 꼴도
FREE_CALL = re.compile(r'^\s*(free_and_init|free|db_private_free_and_init|db_private_free|free_and_init|delete|pr_clear_value|db_value_clear)\b')
WINDOW = 4     # 오류 블록을 빠져나간 뒤 정리 문장(unlock·free·assert)을 몇 개까지 지나 return·goto 를 기다리나 — 표본에서 정리 문장은 3개 이하였다


MEM_FUNCS = r'(memcpy|memmove|memset|memcmp|strcpy|strncpy|strcat|strncat|strlen|strcmp|strncmp|sprintf|snprintf|strlcpy)'


def _derefs(t: str, x: str) -> bool:
    """문장 t 가 x 를 역참조하나: x->·x[·*x(x 가 이미 '*' 식이면 그 자체), 또는 메모리 함수(memcpy 등)에 x 를 넘긴다.
    NULL 을 그대로 대입·반환하거나 다른 함수에 넘기는 것은 세지 않는다(그 함수가 NULL 을 받을 수 있다)."""
    e = re.escape(x)
    if re.search(r'(?<![\w>.])' + e + r'\s*(->|\[)', t) or (not x.startswith('*') and re.search(r'(?<![\w*])\*\s*' + e + r'(?!\w)', t)):
        return True
    m = re.search(r'\b' + MEM_FUNCS + r'\s*\(', t)
    return bool(m) and re.search(r'(?<![\w>.])' + e + r'(?!\w)', t[m.end():]) is not None


def _null_checked(ctx: FileCtx, f: Fn, stmt) -> set:
    """er_set 을 then 쪽에 품은 가장 가까운 if 의 조건에서 '없음' 으로 확인한 포인터 식(X == NULL, NULL == X, !X(포인터 변수))."""
    a = stmt.parent
    while a is not None and a.type != 'function_definition':
        if a.type == 'if_statement':
            cons = a.child_by_field_name('consequence')
            if cons is None or not (cons.start_byte <= stmt.start_byte < cons.end_byte):
                return set()                           # else 쪽이면 조건은 '자원이 없음' 이 아니다
            cond = _cond_text(ctx, a)
            xs = set(re.findall(r'([*]?[A-Za-z_][\w.>\-\[\]]*?)\s*==\s*NULL\b', cond)) | set(re.findall(r'\bNULL\s*==\s*([*]?[A-Za-z_][\w.>\-\[\]]*)', cond))
            for x in re.findall(r'!\s*([A-Za-z_]\w*)\b(?!\s*[(.\->\[])', cond):
                ent = f.vars.get(x) or ctx.globals.get(x)
                if ent and all(ptr for _, ptr, _ in ent):
                    xs.add(x)
            return {x.replace('->', '->') for x in xs if x and x != 'NULL'}
        a = a.parent
    return set()


def _after_er_set(ctx: FileCtx, f: Fn, stmt, pre_assigned=()):
    """er_set 문장 뒤의 흐름을 직선 구간만 따라간다. 반환 (판정, 지나온 문장들).
    판정: ok — return·goto·break·throw·abort·확인 매크로·er_stack_pop 을 만나거나 함수 끝, 또는 오류 블록을 빠져나간 뒤
               WINDOW 문장 안에 방금 세운 오류 변수(er_set 앞뒤로 같은 블록에서 대입)·er_errid () 를 보는 문장이 온다
          use-after-fail — 빠져나간 뒤 if 조건에서 NULL 로 확인한 자원을 그대로 쓴다(할당 실패 뒤 memcpy 등 — 확인된 꼴)
          continue · loop-end(반복 본문 끝 → 다음 반복) · fall-through(빠져나간 뒤 오류를 보지 않고 다른 일을 계속한다)
          unknown — 오류 블록 안에서 분기·반복이 끼어 직선 구간이 아니다(판정하지 않는다).
    블록 끝을 넘어 바깥으로 올라가는 것은 문장을 실행하지 않으므로 세지 않는다(else-if 사슬·맨 블록·레이블은 투명)."""
    assigned, seq, cur, left, budget = set(pre_assigned), [], stmt, False, WINDOW
    nulls = _null_checked(ctx, f, stmt)
    while True:
        blk = cur.parent
        sib = _next_stmt(cur) if blk is not None and blk.type in BLOCKS else None
        if sib is None:
            if blk is None or blk.type == 'case_statement':
                return 'unknown', seq
            up = blk.parent if blk.type == 'compound_statement' else blk     # 중괄호 없는 단일 문장 본문이면 그 문장이 본문
            if up is None:
                return 'unknown', seq
            if up.type == 'function_definition':
                return 'ok', seq                       # 함수 끝 = 반환
            if up.type in LOOPS:
                return 'loop-end', seq
            if up.type in ('if_statement', 'else_clause'):
                cur, left = (up if up.type == 'if_statement' else up.parent), True
                if cur is None:
                    return 'unknown', seq
                continue
            if up.type in BLOCKS or up.type == 'labeled_statement':
                cur = blk if up.type in BLOCKS else up
                continue
            return 'unknown', seq
        st = sib
        if st.type == 'labeled_statement':            # 레이블은 투명 — 붙은 문장을 본다
            st = st.named_children[-1] if len(st.named_children) > 1 else None
            if st is None:
                cur = sib
                continue
        t = text(ctx.src, st)
        if st.type in EXIT_STMT:
            return 'ok', seq
        if st.type == 'continue_statement':
            return 'continue', seq
        if st.type == 'expression_statement' and (NORETURN.match(t) or CHECK_MACRO.match(t) or 'er_stack_pop' in t):
            return 'ok', seq
        if not left:                                   # 아직 오류 블록 안
            if st.type == 'expression_statement':
                assigned |= _lhs_vars(ctx, st)
                seq.append(t.strip()[:50])
                cur = sib
                continue
            if st.type == 'declaration':
                cur = sib
                continue
            return 'unknown', seq
        ids = _idents(ctx, st)
        if (assigned and ids & assigned) or 'er_errid' in ids:
            return 'ok', seq
        line1 = '↳ ' + t.strip().split('\n')[0][:50]
        used = [x for x in nulls if _derefs(t, x) and not FREE_CALL.match(t)]
        if used and st.type != 'if_statement':
            return 'use-after-fail', seq + [line1 + '  (`%s`)' % used[0]]
        if st.type not in ('expression_statement', 'declaration'):
            return 'fall-through', seq + [line1]
        seq.append(line1)
        assigned |= _lhs_vars(ctx, st)
        budget -= 1
        if budget == 0:
            return 'fall-through', seq
        cur = sib


ERR_VERDICT = {'use-after-fail': '블록을 빠져나가 NULL 로 확인한 자원을 그대로 쓴다', 'continue': 'continue',
               'loop-end': '반복 끝(다음 반복)', 'fall-through': '블록을 빠져나가 계속'}


def check_error_path(ctx: FileCtx, f: Fn, stats) -> List[Hit]:
    out = []
    body = f.node.child_by_field_name('body')
    for n in _walk(body) if body is not None else []:
        if n.type != 'call_expression':
            continue
        fn = n.child_by_field_name('function')
        if fn is None or text(ctx.src, fn) not in ER_SET:
            continue
        args = n.child_by_field_name('arguments')
        named = args.named_children if args is not None else []
        if not named or text(ctx.src, named[0]) not in ERR_SEV:
            continue                                   # 경고·알림 등급, 변수 등급은 보지 않는다
        if n.parent is None or n.parent.type != 'expression_statement':
            continue
        code = text(ctx.src, named[2]) if len(named) > 2 else '?'
        pre, p = set(), n.parent.prev_named_sibling   # 같은 블록에서 er_set 바로 앞에 세운 오류 변수(error = ER_…; er_set (…);)
        while p is not None and p.type in ('expression_statement', 'comment'):
            pre |= _lhs_vars(ctx, p)
            p = p.prev_named_sibling
        verdict, seq = _after_er_set(ctx, f, n.parent, pre)
        if verdict in ERR_VERDICT:
            out.append(Hit('error-path-continues', ctx.rel, n.start_point[0] + 1, f.name,
                           'er_set(%s, %s) 뒤 %s' % (text(ctx.src, named[0]), code, ERR_VERDICT[verdict]),
                           'high' if verdict == 'use-after-fail' else 'low',
                           facts={'severity': text(ctx.src, named[0]), 'code': code, 'after': seq[:6], 'verdict': verdict}))
        elif verdict == 'unknown':
            stats['unknown'] += 1
    return out


# ------------------------------------------------------------------ checker 4: untracked-aligned-alloc (META-REVIEW PR#7658)
ALIGNED = {'posix_memalign', 'aligned_alloc', 'memalign', 'valloc', 'pvalloc', '_aligned_malloc'}
MMAP = {'mmap', 'mmap64'}
ALLOC_OWNERS = ('src/base/memory_cwrapper.h', 'src/base/memory_wrapper.cpp', 'src/heaplayers/')   # 추적 래퍼 자신·서드파티 할당기


def check_aligned(ctx: FileCtx, f: Fn, stats) -> List[Hit]:
    if ctx.rel.startswith(ALLOC_OWNERS) or f.name == 'cub_aligned_alloc':
        return []
    out = []
    body = f.node.child_by_field_name('body')
    for n in _walk(body) if body is not None else []:
        if n.type != 'call_expression':
            continue
        fn = n.child_by_field_name('function')
        name = text(ctx.src, fn) if fn is not None else ''
        if name in ALIGNED or name in MMAP:
            out.append(Hit('untracked-aligned-alloc', ctx.rel, n.start_point[0] + 1, f.name, name, 'high' if name in ALIGNED else 'low',
                           facts={'callee': name}))
    return out


RUN = {'fp-range-outside': check_fp_range, 'discarded-status': check_discarded, 'error-path-continues': check_error_path,
       'untracked-aligned-alloc': check_aligned}


def scan_file(rel: str, raw: bytes, gs: GitSrc, checkers, stats, only_lines=None, resolve=None) -> List[Hit]:
    """한 파일의 함수들을 훑는다. only_lines(함수 시작 줄 집합)를 주면 그 함수만."""
    ctx = FileCtx(rel, raw, gs)
    hits = []
    for f in ctx.fns:
        if only_lines is not None and not any(f.start <= l <= f.end for l in only_lines):
            continue
        for c in checkers:
            if c not in RUN:
                continue
            st = stats.setdefault(c, {'hits': 0, 'unknown': 0})
            hs = RUN[c](ctx, f, st, resolve) if c == 'discarded-status' else RUN[c](ctx, f, st)
            st['hits'] += len(hs)
            hits += hs
    return hits


# ------------------------------------------------------------------ 파이프라인: finding 만들기
JOIN_MODEL = re.compile(r'^(planner_visit_node|.*(^|_)(selectivity|card|cardinality|ndv|ngroups|estimate|sel)(_|$).*)$')
JOIN_EXCLUDE = re.compile(r'^(bitset_|qo_rewrite_)')
JOIN_SHAPES = ('① 자식 1·부모 1', '② 자식 2·부모 1 이 같은 조인 스텝에서 연결', '③ 자식 1·부모 2', '④ 체인(A→B→C)',
               '⑤ self-ref(같은 테이블의 FK)', '⑥ semi·anti(EXISTS·NOT EXISTS·IN)', '⑦ outer(LEFT·RIGHT)')


HUNK = re.compile(r'^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@')


def _base_line(diff_text: str, file: str, head_line: int) -> Optional[int]:
    """unified diff 로 head 의 줄 번호 → 기준(merge-base) 줄 번호. 그 줄이 이 diff 에서 추가·변경됐으면 None.
    파일이 diff 에 없으면 그대로(바뀌지 않은 파일)."""
    in_file, seen, hunk, o, n, delta = False, False, False, 0, 0, 0
    for ln in diff_text.splitlines():
        if ln.startswith('diff --git'):
            if in_file:
                break
            continue
        if ln.startswith('+++ '):
            path = ln[6:] if ln.startswith('+++ b/') else ln[4:].strip()
            in_file = path == file
            seen = seen or in_file
            hunk = False
            continue
        if not in_file or ln.startswith('--- '):
            continue
        m = HUNK.match(ln)
        if m:
            if head_line < int(m.group(2)):
                return head_line - delta
            o, n, hunk = int(m.group(1)), int(m.group(2)), True
            continue
        if not hunk or ln.startswith('\\'):
            continue
        if ln.startswith('+'):
            if n == head_line:
                return None
            n += 1
        elif ln.startswith('-'):
            o += 1
        else:                                          # 문맥 줄(' ' 또는 빈 줄)
            if n == head_line:
                return o
            o += 1
            n += 1
        delta = n - o
    return head_line - delta if seen else head_line


def _anchor(changed_lines, start, end, want):
    """or_buf._anchor 와 같은 규칙: layer 코드는 anchor 가 diff ±3 안이어야 valid — 멀면 같은 함수의 가장 가까운 변경 줄로."""
    if any(abs(l - want) <= 3 for l in changed_lines):
        return want, '코드'
    inside = [l for l in changed_lines if start <= l <= end]
    if inside:
        return min(inside, key=lambda l: abs(l - want)), '코드'
    return want, '설계'


TEXT = {
    'fp-range-outside': dict(
        claim='`{arg}`(실수)를 바깥 꼴 범위 검사 `{macro}` 로 거른다 — NaN 이 이 검사를 통과해 정수 좁힘이 정의되지 않은 동작(UB)이 된다',
        why=('바깥 꼴(`x > MAX || x < MIN`)은 NaN 이면 두 비교가 모두 거짓이라 "범위 안" 으로 판정한다. 그 뒤 정수로 좁히는 캐스트는 '
             'C/C++ 에서 UB 라(x86 에서 INT_MIN 등) 오류 대신 0·깨진 값이 결과가 된다 — CBRD-27660 에서 CAST(\'nan\' AS SMALLINT) 가 0, '
             'NaN → TIME 이 깨진 값으로 나왔다. 경계 상수(DB_INT32_MAX·DB_BIGINT_MAX)도 실수로 바뀌며 반올림돼 경계값 하나가 빠져나갈 수 있다(자가진화 #004).'),
        proposal=('실수 인자는 안쪽 꼴로 검사한다 — NaN 이 모든 비교에 거짓이라 "범위 밖" 으로 떨어진다.\n\n```c\nif (! ({arg} >= {lo} && {arg} <= {hi}))\n'
                  '  {{\n    /* overflow */\n  }}\n```\n\nINTEGER·BIGINT 는 `OR_CHECK_INT_OVERFLOW_FROM_FP`·`OR_CHECK_BIGINT_OVERFLOW_FROM_FP`(정확한 2^31·2^63 경계, 안쪽 꼴)를 써도 된다. '
                  'NaN·±경계값 단위 케이스를 함께 둔다.')),
    'discarded-status': dict(
        claim='`{callee}` 가 돌려주는 {rtype} 상태를 버린다 — 실패해도 이 함수는 성공 쪽으로 진행한다',
        why=('`{callee}` 는 {rtype} 를 돌려준다. 이 호출은 문장으로 끝나 반환값을 보지 않으므로 실패(오류 코드·DOMAIN_* 상태)가 나도 출력 값이 '
             '채워지지 않은 채 다음 단계가 성공으로 이어 간다 — CBRD-27770 에서 tp_make_time_conversion 의 ER_TIME_CONVERSION 을 버려 '
             '음수 → TIME 이 깨진 값(12:00:0/ AM)으로 나왔다(자가진화 #005).'),
        proposal=('반환값을 받아 실패를 상태로 전파한다.\n\n```c\nif ({call} != {ok})\n  {{\n    {fail}\n  }}\n```\n\n'
                  '일부러 버리는 것이면 `(void) {callee} (…)` 로 의도를 드러내고 이유를 한 줄 주석으로 적는다.')),
    'error-path-continues': dict(
        claim='`er_set ({sev}, …, {code}, …)` 로 오류를 세운 뒤 {after} — 오류 경로가 흐름을 끊지 않는다',
        why=('er_set 은 오류를 기록만 하고 흐름을 바꾸지 않는다. 오류를 세운 뒤 계속 진행하면 이 함수가 끝에 성공(NO_ERROR)을 돌려주거나, 뒤의 er_set 이 '
             '첫 오류를 덮어써 호출자가 원인과 다른 오류를 받는다 — PR#7937 에서 ER_OUT_OF_VIRTUAL_MEMORY 뒤 continue 를 리뷰가 콜드패스로 보고 넘겼다. '
             '에러 경로에는 성능 규약(콜드패스 후순위)이 아니라 정확성 규약이 적용된다.'),
        proposal=('오류를 세웠으면 그 자리에서 빠져나간다.\n\n```c\ner_set ({sev}, ARG_FILE_LINE, {code}, …);\nerror = {code_or_errid};\ngoto exit_on_error;   /* 또는 return */\n```\n\n'
                  '일부러 건너뛰고 계속하는 것이면 오류 등급 er_set 대신 경고 등급(ER_WARNING_SEVERITY)으로 남기거나 er_clear () 로 상태를 지운다.')),
    'untracked-aligned-alloc': dict(
        claim='`{callee}` 를 직접 불러 받은 메모리를 메모리 모니터가 추적하지 않는다',
        why=('서버 모드의 메모리 모니터(cubrid memmon)는 cub_alloc·cub_aligned_alloc 이 기록한 블록만 센다. {callee} 로 직접 받은 블록은 합계에 안 잡혀 '
             '누수·과다 사용이 생겨도 memmon 으로 찾을 수 없다 — PR#7658 에서 슬롯 배열만 posix_memalign 을 직접 불러 모니터 사각이 생겼고, '
             '앞선 리뷰는 "래퍼는 operator new/delete 만 감싼다" 는 틀린 전제로 넘겼다.'),
        proposal=('정렬 할당은 `cub_aligned_alloc (alignment, size, __FILE__, __LINE__)`(src/base/memory_cwrapper.h)로 받고 free () 로 놓는다 — '
                  'SERVER_MODE 에서는 모니터가 기록하고 다른 모드에서는 posix_memalign 과 같다. 추적하지 않아야 할 이유가 있으면 그 이유를 주석으로 남긴다.')),
}


def _finding(h: Hit, layer: str, line: int, pre: bool, base_ev: Optional[str]) -> Dict:
    fx = h.facts
    T = TEXT[h.checker]
    if h.checker == 'fp-range-outside':
        kw = dict(arg=fx['arg'], macro=fx['macro'], lo=fx['lo'], hi=fx['hi'])
        ver = '인자 `%s` — %s. 범위 검사가 바깥 꼴(object_representation.h 의 `(i) > MAX || (i) < MIN`, 또는 같은 꼴의 인라인 비교)' % (fx['arg'], fx['arg_note'])
    elif h.checker == 'discarded-status':
        rt, ft = fx['rtype'], fx.get('ftype', '')
        ok = 'DOMAIN_COMPATIBLE' if 'STATUS' in rt.upper() else ('true' if rt in ('bool', '_Bool') else 'NO_ERROR')
        # 실패 때 돌려줄 값은 '감싼 함수' 의 반환 타입을 따른다(변환기는 TP_DOMAIN_STATUS, 일반 함수는 오류 코드)
        fail = ('return DOMAIN_ERROR;' if 'STATUS' in ft.upper() else 'return false;' if ft in ('bool', '_Bool')
                else 'goto exit_on_error;   /* 오류 상태를 남기고 정리 */' if ft == 'void' else 'return er_errid ();   /* 이 함수의 오류 규약대로 */')
        kw = dict(callee=fx['callee'], rtype=rt, call=fx['call'].rstrip(';'), ok=ok, fail=fail)
        ver = '호출 `%s` 가 expression_statement(반환값을 쓰지 않음), 반환 타입 %s(정의·프로토타입 색인)' % (fx['call'][:80], rt)
    elif h.checker == 'error-path-continues':
        after = {'use-after-fail': 'return·goto 없이 블록을 빠져나가 실패를 확인한 그 자원(%s)을 그대로 쓴다' % (fx['after'][-1].split('(`')[-1].rstrip('`)') if fx['after'] else '?'),
                 'continue': '`continue` 로 다음 반복을 계속한다', 'loop-end': '반복 본문 끝을 지나 다음 반복으로 넘어간다',
                 'fall-through': 'return·goto 없이 블록을 빠져나가 다음 문장으로 계속 진행한다'}[fx['verdict']]
        kw = dict(sev=fx['severity'], code=fx['code'], after=after, code_or_errid=fx['code'] if fx['code'].startswith('ER_') else 'er_errid ()')
        ver = 'er_set 다음 문장: %s → %s' % (' ; '.join(fx['after']) or '(없음)', fx['verdict'])
    else:
        kw = dict(callee=fx['callee'])
        ver = '호출 `%s` 는 추적 래퍼(cub_aligned_alloc)가 아니다' % fx['callee']
    claim = T['claim'].format(**kw)
    if pre:
        claim = '(이 줄은 이 PR 이 바꾸지 않았다 — 손댄 함수 안의 선재 결함 후보) ' + claim
    if line != h.line:
        claim += ' (%s:%d)' % (h.file, h.line)
    if h.checker in ('fp-range-outside', 'discarded-status', 'error-path-continues') and not pre and h.level == 'high':
        sev, cat = 'non-blocking', '버그 가능성'
    else:
        sev, cat = 'question', '확인 질문'
    f = {'id': 'LINT-%s-%s-%d' % (h.checker, re.sub(r'\W+', '_', h.fn), h.line), 'layer': layer, 'file': h.file, 'line': line,
         'claim': claim, 'why': T['why'].format(**kw), 'proposal': T['proposal'].format(**kw), 'category': cat, 'importance': '중간',
         'evidence': ['%s:%d' % (h.file, h.line)], 'severity': sev,
         'verification': {'method': 'static', 'result': '%s:%d — %s' % (h.file, h.line, ver)},
         'auto': True, 'checker': h.checker}
    if pre and base_ev:
        f['baseline'] = {'claim': 'same', 'evidence': [base_ev]}
        f['preexisting'] = True
    return f


# 이 PR 이 실수 범위 검사를 건드렸나 — 추가된 줄에 범위 매크로·is_floating_point·정수 한계 비교가 있으면 같은 파일 전체를 훑는다
FP_TOUCH = re.compile(r'OR_CHECK_\w*OVERFLOW|is_floating_point|[<>]=?\s*(DB_INT(16|32)|DB_BIGINT|DB_UINT(16|32))_(MAX|MIN)')


def _added(diff_text: str) -> Dict[str, List[Tuple[int, str]]]:
    out, cur, n = {}, None, 0
    for ln in diff_text.splitlines():
        if ln.startswith('+++ '):
            cur = ln[6:] if ln.startswith('+++ b/') else None
            continue
        m = HUNK.match(ln)
        if m:
            n = int(m.group(2))
            continue
        if cur is None or ln.startswith('--- '):
            continue
        if ln.startswith('+'):
            out.setdefault(cur, []).append((n, ln[1:]))
            n += 1
        elif not ln.startswith('-') and not ln.startswith('\\'):
            n += 1
    return out


def _sibling_finding(rel: str, sibs: List[Hit], changed_lines: List[int]) -> Dict:
    """같은 꼴이 남은 다른 자리를 한 지적으로 모은다(PR#8253: 첫 커밋은 SMALLINT 만 고쳤고 TIME 두 곳은 자기 리뷰가 따로 찾았다)."""
    line = min(changed_lines) if changed_lines else sibs[0].line
    lst = '\n'.join('- `%s:%d` `%s` — `%s (%s)`' % (h.file, h.line, h.fn, h.facts['macro'], h.facts['arg']) for h in sibs[:12])
    return {'id': 'LINT-fp-range-outside-siblings-%s' % re.sub(r'\W+', '_', os.path.basename(rel)), 'layer': '설계', 'file': rel, 'line': line,
            'claim': '이 PR 이 실수 범위 검사를 고쳤는데 같은 파일의 다른 %d곳은 아직 바깥 꼴(NaN 통과)이다 — %s'
                     % (len(sibs), ', '.join(sorted({'`%s`' % h.fn for h in sibs}))[:300]),
            'why': ('같은 계약(실수를 정수로 좁히기 전에 NaN·경계를 거른다)을 지켜야 하는 자리가 여러 개인데 일부만 고치면 남은 자리로 같은 증상이 난다 — '
                    'PR#8253 첫 커밋은 SMALLINT 만 고쳤고 DOUBLE·MONETARY → TIME 두 곳(NaN → 깨진 TIME)은 자기 리뷰가 따로 찾았다. '
                    '만드는 곳 하나가 아니라 같은 꼴이 있는 자리를 전수로 센다(META-REVIEW PR#7658 교훈, 자가진화 #004).'),
            'proposal': ('남은 자리마다 안쪽 꼴 `! (x >= MIN && x <= MAX)` 로 바꾸거나, 그 인자에 NaN 이 올 수 없는 이유(호출자·입력 토큰)를 '
                         'PR 본문이나 주석에 적는다(자가진화 #008 — 모양만 보고 "같은 결함" 이라 쓰지 않는다):\n\n' + lst),
            'category': '확인 질문', 'importance': '중간', 'severity': 'question',
            'evidence': ['%s:%d' % (h.file, h.line) for h in sibs[:8]],
            'verification': {'method': 'static', 'result': '파일 전체 훑기: 실수 인자를 받는 바깥 꼴 범위 검사 %d곳(변경 함수 밖)' % len(sibs)},
            'auto': True, 'checker': 'fp-range-outside'}


def _join_finding(fns, changed) -> Optional[Dict]:
    hit = [fn for fn in fns if fn.file.startswith('src/optimizer/') and JOIN_MODEL.match(fn.name) and not JOIN_EXCLUDE.match(fn.name)]
    if not hit:
        return None
    first = hit[0]
    ls = [l for l in changed.get(first.file, []) if first.start_line <= l <= first.end_line]
    line = ls[0] if ls else first.start_line
    names = ', '.join('`%s`' % fn.name for fn in hit[:6]) + (' 외 %d개' % (len(hit) - 6) if len(hit) > 6 else '')
    return {'id': 'LINT-join-shape-checklist', 'layer': '설계', 'file': first.file, 'line': line,
            'claim': '카디널리티·선택도 모델 함수(%s)가 바뀌었다 — 아래 조인 형상마다 추정치를 확인했나' % names,
            'why': ('추정 모델의 변경은 작성자가 본 형상에서만 맞고 다른 형상에서 과소·과대 추정이 될 수 있다 — PR#7937 에서 자식 2·부모 1 이 한 스텝에서 '
                    '연결되면 FK 바닥이 1/N² 로 떨어지는 것(card 10000 → 1)을 리뷰가 놓쳤다(작성자 프레임의 형상만 감사). 과소 추정은 NL 조인 폭주, '
                    '과대 추정은 불필요한 해시·정렬로 이어져 질의가 갑자기 느려진다.'),
            'proposal': ('형상마다 최소 테이블 재현 SQL 로 기준 빌드와 이 PR 의 card(`;plan detail` 의 cardinality)를 비교한다:\n\n'
                         + '\n'.join('- ' + s for s in JOIN_SHAPES) +
                         '\n\n과소·과대 추정 지적은 재현(verification.method=dynamic — 형상 SQL·두 빌드의 card)과 함께 낸다(sql-difftest §2-4 최소 테이블).'),
            'category': '테스트 제안', 'importance': '중간', 'severity': 'question',
            'evidence': ['%s:%d' % (fn.file, fn.start_line) for fn in hit[:6]],
            'verification': {'method': 'static', 'result': 'diff 가 바꾼 함수 중 추정 모델 이름(planner_visit_node·*selectivity*·*card*·*ndv*·*estimate*): ' + names},
            'auto': True, 'checker': 'join-shape-checklist'}


def analyze(repo_root: str, g, changed: Dict[str, List[int]], changed_fids: List[str], diff_text: str = '',
            checkers=CHECKERS, gs: Optional[GitSrc] = None) -> Tuple[List[Dict], Dict]:
    """파이프라인용: 변경 함수만 훑어 auto finding 과 집계를 돌려준다.
    gs 를 주지 않으면 repo_root(파이프라인의 worktree — git 작업 트리)의 파일을 읽는다. 시험·사후 점검은 GitSrc(엔진 리포, <head rev>)."""
    gs = gs or GitSrc(repo_root, None)
    stats = {c: {'hits': 0, 'unknown': 0, 'reported': 0} for c in checkers}
    fns = [x for x in (g.function(fid) for fid in changed_fids) if x is not None]
    byfile = {}
    for fn in fns:
        if fn.file.endswith(SRC_EXT):
            byfile.setdefault(fn.file, []).append(fn)

    _cache = {}

    def resolve(name):
        """codegraph 가 해석한 정의가 있으면 그 정의 머리의 반환 타입(이름 앞 텍스트)."""
        if name in _cache:
            return _cache[name]
        r = g.db.execute('SELECT fid FROM functions WHERE name=? ORDER BY fid', (name,)).fetchall()
        rt = None
        if len(r) == 1:
            node = g.function(r[0][0])
            lines = gs.read(node.file).decode('utf-8', 'replace').split('\n')[node.start_line - 1:node.start_line + 3]
            head = ' '.join(lines)
            head = head[:head.find(name)] if name in head else ''
            rt = norm_type(head) or None
        _cache[name] = rt
        return rt

    out, notes = [], []
    for rel, lst in sorted(byfile.items()):
        raw = gs.read(rel)
        if not raw:
            continue
        starts = {fn.start_line for fn in lst}
        hits = scan_file(rel, raw, gs, [c for c in checkers if c in RUN], stats, only_lines=starts, resolve=resolve)
        lines = set(changed.get(rel, []))
        for h in hits:
            on_changed = h.line in lines
            if h.checker in DIFF_ONLY and not on_changed:
                continue
            if h.checker == 'error-path-continues' and h.facts.get('verdict') in NOTE_ONLY:
                notes.append(h.as_dict())              # 정밀도가 낮다(develop 표본 15건 중 의도된 '기록하고 계속' 13건) — 지적 대신 참고로만
                continue
            fn = next((x for x in lst if x.start_line <= h.line <= x.end_line), None)
            line, layer = _anchor(sorted(lines), fn.start_line if fn else h.line, fn.end_line if fn else h.line, h.line)
            base = None
            if not on_changed and diff_text:
                bl = _base_line(diff_text, rel, h.line)
                base = '%s:%d' % (rel, bl) if bl else None
            out.append(_finding(h, layer, line, not on_changed, base))
            stats[h.checker]['reported'] += 1
    if 'fp-range-outside' in checkers and diff_text:
        for rel, adds in sorted(_added(diff_text).items()):
            if not rel.endswith(SRC_EXT) or not any(FP_TOUCH.search(t) for _, t in adds):
                continue
            raw = gs.read(rel)
            if not raw:
                continue
            own = [(fn.start_line, fn.end_line) for fn in byfile.get(rel, [])]
            sib_stats = {}
            sibs = [h for h in scan_file(rel, raw, gs, ['fp-range-outside'], sib_stats)
                    if not any(a <= h.line <= b for a, b in own)]
            if sibs:
                out.append(_sibling_finding(rel, sibs, sorted(changed.get(rel, []))))
                stats['fp-range-outside']['siblings'] = stats['fp-range-outside'].get('siblings', 0) + len(sibs)
    if 'join-shape-checklist' in checkers:
        jf = _join_finding(fns, changed)
        if jf:
            out.append(jf)
            stats['join-shape-checklist']['hits'] = stats['join-shape-checklist']['reported'] = 1
    stats['notes'] = notes
    return out, stats


def node(c, a):
    """pipeline 노드(context 단계, or_buf 뒤). findings.auto.json 은 perf_claims 가 먼저 썼으므로 덮어쓰지 않고 append 한다."""
    auto, stats = analyze(c.need('wt'), c.need('g'), c.need('changed'), c.need('changed_fids'), c.get('diff', ''),
                          tuple(a.get('checkers', CHECKERS)))
    p = os.path.join(c['out'], 'findings.auto.json')
    prev = []
    if os.path.isfile(p):
        with open(p, encoding='utf-8') as fp:
            prev = json.load(fp)
    prev = [f for f in prev if not str(f.get('id', '')).startswith('LINT-')]       # 같은 OUT 에서 다시 돌 때 중복 방지
    with open(p, 'w', encoding='utf-8') as fp:
        json.dump(prev + auto, fp, ensure_ascii=False, indent=1)
    with open(os.path.join(c['out'], 'static_lint.json'), 'w', encoding='utf-8') as fp:
        json.dump({'stats': stats, 'findings': [f['id'] for f in auto]}, fp, ensure_ascii=False, indent=1)
    # 판정 불가(unknown)를 함께 찍는다 — 0 건을 '깨끗함' 으로 읽지 않게
    c['log']('static_lint: %d auto findings · ' % len(auto) + ', '.join('%s %d/%d(판정 불가 %d)' % (k, v.get('reported', 0), v['hits'], v['unknown'])
                                                                     for k, v in stats.items() if isinstance(v, dict))
             + ' · 참고(notes) %d' % len(stats.get('notes', [])))
    return auto


# ------------------------------------------------------------------ CLI
def main(argv=None):
    ap = argparse.ArgumentParser(description='정적 린트 — 저장소 rev 를 체크아웃 없이 훑는다(오탐률·전후 비교용). 파이프라인에서는 노드 static_lint 로 돈다.',
                                 epilog=__doc__[__doc__.index('쓰임'):], formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--rev', default='upstream/develop', help='읽을 리비전(git show) — 작업 트리를 보려면 --worktree')
    ap.add_argument('--worktree', action='store_true', help='rev 대신 repo 작업 트리 파일을 읽는다')
    ap.add_argument('--paths', nargs='*', default=['src'])
    ap.add_argument('--checkers', default=','.join(c for c in CHECKERS if c in RUN))
    ap.add_argument('--json', default='', help='적중 전부를 JSON 으로 저장')
    ap.add_argument('--quiet', action='store_true', help='적중 줄을 찍지 않고 요약만')
    a = ap.parse_args(argv)
    gs = GitSrc(os.path.expanduser(a.repo), None if a.worktree else a.rev)
    checkers = [c.strip() for c in a.checkers.split(',') if c.strip()]
    stats, hits = {}, []
    files = gs.files(a.paths)
    for rel in files:
        if not rel.endswith(('.c', '.cpp', '.cc', '.cxx', '.h', '.hpp')):
            continue
        raw = gs.read(rel)
        if b'(' not in raw:
            continue
        hits += scan_file(rel, raw, gs, checkers, stats)
    if not a.quiet:
        for h in hits:
            print('%s:%d\t%s\t%s\t[%s] %s' % (h.file, h.line, h.fn, h.checker, h.level, h.detail))
    print('# %s %s — 파일 %d · ' % ('worktree' if a.worktree else a.rev, ' '.join(a.paths), len(files)) +
          ', '.join('%s 적중 %d(판정 불가 %d)' % (k, v['hits'], v['unknown']) for k, v in stats.items()), file=sys.stderr)
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as fp:
            json.dump({'rev': None if a.worktree else a.rev, 'paths': a.paths, 'stats': stats, 'hits': [h.as_dict() for h in hits]},
                      fp, ensure_ascii=False, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
