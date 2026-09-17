"""CodeGraph: Tree-sitter 로 C/C++ 함수·호출·구조체·락 호출을 뽑아 SQLite 에 결정론적으로 저장한다.

Metis 의 CodeGraph 계약을 따른다 — 심볼, 호출, 소스 위치, 언어 파생 사실(락/latch 호출)만 담고
LLM 추론은 담지 않는다. 같은 입력(repo fingerprint + 파일 목록)이면 같은 그래프(행 순서까지)가 나온다.
"""
import hashlib, json, os, sqlite3, subprocess, sys
from dataclasses import dataclass, asdict, field
from typing import Dict, Iterable, List, Optional, Tuple

TS_LIB = os.path.expanduser('~/dev/utils/tree-sitter/langs.so')

@dataclass(frozen=True)
class FunctionNode:
    fid: str          # file:name  (static 함수는 파일 단위로 유일)
    name: str
    file: str
    start_line: int
    end_line: int
    params: str
    is_static: bool

@dataclass(frozen=True)
class CallSite:
    caller: str       # fid
    callee: str       # 이름(미해석) — 해석은 resolve() 에서
    line: int
    args: str

@dataclass(frozen=True)
class DomainFact:
    """언어 파생 사실: 락/latch/로그 관문 호출. 사전설계 13장 P3 의 '관문' 목록이 프로파일이다.
    var: 관문이 다루는 변수 — fix/alloc 은 대입의 좌변, unfix/free 는 (thread_p 다음) 첫 인자. 변수 단위 짝 검사에 쓴다."""
    fid: str
    kind: str         # latch_fix | latch_unfix | lock_acquire | lock_release | log_append | sysop_start | sysop_end | alloc | free
    callee: str
    line: int
    var: str = ''

# 사전설계문서 13장(관문)과 성능규칙집 MEM/ALLOC 축에서 뽑은 DB 도메인 프로파일
DOMAIN_PROFILE = {
    'latch_fix': ('pgbuf_fix', 'pgbuf_fix_with_retry', 'pgbuf_ordered_fix', 'pgbuf_fix_debug', 'btree_fix_root_with_info'),
    'latch_unfix': ('pgbuf_unfix', 'pgbuf_unfix_and_init', 'pgbuf_ordered_unfix', 'pgbuf_unfix_debug'),
    'lock_acquire': ('lock_object', 'lock_scan', 'lock_subclass', 'lock_object_wait_msecs', 'lock_hold_object_instant'),
    'lock_release': ('lock_unlock_object', 'lock_unlock_all', 'lock_unlock_scan'),
    'log_append': ('log_append_undoredo_data', 'log_append_redo_data', 'log_append_undo_data', 'log_append_postpone'),
    'sysop_start': ('log_sysop_start',),
    'sysop_end': ('log_sysop_commit', 'log_sysop_abort', 'log_sysop_attach_to_outer', 'log_sysop_end_logical_undo'),
    'alloc': ('malloc', 'calloc', 'realloc', 'db_private_alloc', 'db_private_realloc', 'posix_memalign'),
    'free': ('free', 'free_and_init', 'db_private_free', 'db_private_free_and_init'),
}
_KIND_BY_CALLEE = {c: k for k, cs in DOMAIN_PROFILE.items() for c in cs}

def repo_fingerprint(repo: str) -> str:
    head = subprocess.run(['git', '-C', repo, 'rev-parse', 'HEAD'], stdout=subprocess.PIPE, universal_newlines=True).stdout.strip()
    dirty = subprocess.run(['git', '-C', repo, 'status', '--porcelain', '--untracked-files=no'], stdout=subprocess.PIPE, universal_newlines=True).stdout
    return hashlib.sha1((head + dirty).encode()).hexdigest()

class TreeSitterProvider:
    """언어 소유 제공자. 실패(문법 라이브러리 없음)면 CtagsProvider 로 폴백한다."""
    def __init__(self):
        from tree_sitter import Language, Parser
        self._parsers = {}
        for lang in ('c', 'cpp'):
            p = Parser(); p.set_language(Language(TS_LIB, lang)); self._parsers[lang] = p

    def parse_file(self, repo: str, rel: str) -> Tuple[List[FunctionNode], List[CallSite], List[DomainFact], List[dict]]:
        lang = 'cpp' if rel.endswith(('.cpp', '.hpp', '.cc', '.cxx')) else 'c'
        src = open(os.path.join(repo, rel), 'rb').read()
        tree = self._parsers[lang].parse(src)
        funcs, calls, facts, structs, types = [], [], [], [], []

        def text(n): return src[n.start_byte:n.end_byte].decode('utf-8', 'replace')

        def declarator_name(node):
            # function_declarator -> declarator (identifier | qualified_identifier | pointer_declarator ...)
            d = node
            while d is not None and d.type not in ('identifier', 'field_identifier', 'qualified_identifier', 'destructor_name', 'operator_name'):
                d = d.child_by_field_name('declarator') if d.child_by_field_name('declarator') is not None else (d.children[0] if d.children else None)
            return text(d) if d is not None else None

        def walk(node, cur_fid):
            t = node.type
            if t == 'function_definition':
                decl = node.child_by_field_name('declarator')
                params = ''
                fd = decl
                while fd is not None and fd.type != 'function_declarator':
                    fd = fd.child_by_field_name('declarator')
                if fd is not None:
                    pl = fd.child_by_field_name('parameters'); params = text(pl) if pl is not None else ''
                name = declarator_name(fd if fd is not None else decl) or '?'
                is_static = any(text(c) == 'static' for c in node.children if c.type == 'storage_class_specifier')
                fid = f'{rel}:{name}'
                funcs.append(FunctionNode(fid, name, rel, node.start_point[0] + 1, node.end_point[0] + 1, params, is_static))
                cur_fid = fid
            elif t == 'call_expression' and cur_fid is not None:
                fn = node.child_by_field_name('function'); args = node.child_by_field_name('arguments')
                callee = text(fn) if fn is not None else '?'
                callee = callee.split('->')[-1].split('.')[-1].split('::')[-1]
                line = node.start_point[0] + 1
                calls.append(CallSite(cur_fid, callee, line, text(args)[:120] if args is not None else ''))
                k = _KIND_BY_CALLEE.get(callee)
                if k:
                    var = ''
                    if k in ('latch_fix', 'alloc', 'lock_acquire'):
                        par = node.parent
                        while par is not None and par.type not in ('assignment_expression', 'init_declarator', 'function_definition', 'expression_statement'):
                            par = par.parent
                        if par is not None and par.type in ('assignment_expression', 'init_declarator'):
                            lhs = par.child_by_field_name('left') if par.type == 'assignment_expression' else par.child_by_field_name('declarator')
                            var = text(lhs).replace('*', '').strip() if lhs is not None else ''
                    elif k in ('latch_unfix', 'free', 'lock_release') and args is not None:
                        ids = [text(c) for c in args.children if c.type not in ('(', ')', ',')]
                        ids = [i for i in ids if i not in ('thread_p', 'thread_ref', '&thread_ref')]
                        var = ids[0].replace('&', '').replace('*', '').strip() if ids else ''
                    facts.append(DomainFact(cur_fid, k, callee, line, var))
            elif t == 'type_identifier' and cur_fid is not None:
                types.append((cur_fid, text(node)))
            elif t in ('struct_specifier', 'class_specifier') and node.child_by_field_name('body') is not None:
                nm = node.child_by_field_name('name')
                if nm is not None:
                    structs.append({'name': text(nm), 'file': rel, 'start_line': node.start_point[0] + 1, 'end_line': node.end_point[0] + 1})
            for c in node.children:
                walk(c, cur_fid)
        walk(tree.root_node, None)
        return funcs, calls, facts, structs, sorted(set(types))

class CtagsProvider:
    """폴백: 함수 정의만(호출 없음). 그래프는 '불완전' 표지가 붙고 reachability 는 fail-open 으로 돈다."""
    def parse_file(self, repo, rel):
        out = subprocess.run(['ctags', '-x', '--c-kinds=f', '--c++-kinds=f', os.path.join(repo, rel)], stdout=subprocess.PIPE, universal_newlines=True).stdout
        funcs = []
        for ln in out.splitlines():
            # ctags -x: "<name> <kind> <line> <file> ..." -- but a C++ `operator ==` splits the name
            # in two, so take the first purely numeric field after the kind as the line number
            parts = ln.split()
            try:
                k = parts.index('function')
                line_no = int(parts[k + 1])
            except (ValueError, IndexError):
                continue
            name = ' '.join(parts[:k])
            funcs.append(FunctionNode(f'{rel}:{name}', name, rel, line_no, line_no, '', False))
        return funcs, [], [], [], []

class CodeGraph:
    SCHEMA_VERSION = '2'
    def __init__(self, db_path: str, repo_root: str = ''):
        self.repo_root = repo_root
        self.db = sqlite3.connect(db_path)
        # 스키마 버전이 다르면 통째로 버린다(캐시일 뿐이고, 부분 마이그레이션은 결정론을 해친다)
        try:
            v = self.db.execute("SELECT v FROM meta WHERE k='schema_version'").fetchone()
        except sqlite3.OperationalError: v = None
        if v is None or v[0] != self.SCHEMA_VERSION:
            for t in ('functions', 'calls', 'facts', 'structs', 'fn_types', 'meta'): self.db.execute(f'DROP TABLE IF EXISTS {t}')
        self.db.executescript('''
        CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
        CREATE TABLE IF NOT EXISTS functions(fid TEXT PRIMARY KEY, name TEXT, file TEXT, start_line INT, end_line INT, params TEXT, is_static INT);
        CREATE TABLE IF NOT EXISTS calls(caller TEXT, callee TEXT, line INT, args TEXT, resolved TEXT);
        CREATE TABLE IF NOT EXISTS facts(fid TEXT, kind TEXT, callee TEXT, line INT, var TEXT);
        CREATE TABLE IF NOT EXISTS fn_types(fid TEXT, type TEXT);
        CREATE TABLE IF NOT EXISTS structs(name TEXT, file TEXT, start_line INT, end_line INT);
        CREATE INDEX IF NOT EXISTS ix_calls_caller ON calls(caller); CREATE INDEX IF NOT EXISTS ix_calls_resolved ON calls(resolved);
        CREATE INDEX IF NOT EXISTS ix_fn_name ON functions(name);''')

    @classmethod
    def build(cls, repo: str, files: Iterable[str], db_path: str, provider=None, log=print) -> 'CodeGraph':
        files = sorted(set(files))  # 결정론: 입력 순서 무관
        fp = repo_fingerprint(repo)
        key = hashlib.sha1(('v2|' + fp + '\n'.join(files)).encode()).hexdigest()
        g = cls(db_path, repo)
        cur = g.db.execute("SELECT v FROM meta WHERE k='input_fingerprint'").fetchone()
        if cur and cur[0] == key:
            log(f'codegraph: cache hit ({len(files)} files)'); return g
        if provider is None:
            try: provider = TreeSitterProvider(); complete = True
            except Exception as e: log(f'codegraph: tree-sitter unavailable ({e}); ctags fallback'); provider = CtagsProvider(); complete = False
        else: complete = not isinstance(provider, CtagsProvider)
        with g.db:
            for t in ('functions', 'calls', 'facts', 'structs', 'fn_types', 'meta'): g.db.execute(f'DELETE FROM {t}')
            for rel in files:
                if not os.path.isfile(os.path.join(repo, rel)): continue
                fs, cs, fa, st, ty = provider.parse_file(repo, rel)
                g.db.executemany('INSERT OR REPLACE INTO functions VALUES(?,?,?,?,?,?,?)', [(f.fid, f.name, f.file, f.start_line, f.end_line, f.params, int(f.is_static)) for f in fs])
                g.db.executemany('INSERT INTO calls VALUES(?,?,?,?,NULL)', [(c.caller, c.callee, c.line, c.args) for c in cs])
                g.db.executemany('INSERT INTO facts VALUES(?,?,?,?,?)', [(x.fid, x.kind, x.callee, x.line, x.var) for x in fa])
                g.db.executemany('INSERT INTO fn_types VALUES(?,?)', ty)
                g.db.executemany('INSERT INTO structs VALUES(?,?,?,?)', [(s['name'], s['file'], s['start_line'], s['end_line']) for s in st])
            g._resolve()
            g.db.executemany('INSERT OR REPLACE INTO meta VALUES(?,?)', [('input_fingerprint', key), ('repo_fingerprint', fp), ('complete', '1' if complete else '0'), ('n_files', str(len(files))), ('schema_version', cls.SCHEMA_VERSION)])
        log(f'codegraph: built {g.count("functions")} functions, {g.count("calls")} calls, {g.count("facts")} domain facts from {len(files)} files')
        return g

    def _resolve(self):
        """호출 해석: 같은 파일의 static 함수 우선, 다음 전역 유일 이름. 모호(동명 다수)는 미해석으로 남긴다(fail-open 근거)."""
        by_name: Dict[str, List[Tuple[str, str, int]]] = {}
        for fid, name, file, st in self.db.execute('SELECT fid,name,file,is_static FROM functions'):
            by_name.setdefault(name, []).append((fid, file, st))
        rows = self.db.execute('SELECT rowid, caller, callee FROM calls').fetchall()
        upd = []
        for rowid, caller, callee in rows:
            cands = by_name.get(callee, [])
            cfile = caller.split(':', 1)[0]
            same = [c for c in cands if c[1] == cfile]
            if len(same) == 1: upd.append((same[0][0], rowid)); continue
            glob = [c for c in cands if not c[2]]
            if len(glob) == 1: upd.append((glob[0][0], rowid))
        self.db.executemany('UPDATE calls SET resolved=? WHERE rowid=?', upd)

    def count(self, table): return self.db.execute(f'SELECT COUNT(*) FROM {table}').fetchone()[0]
    def complete(self): r = self.db.execute("SELECT v FROM meta WHERE k='complete'").fetchone(); return bool(r and r[0] == '1')
    def functions_in(self, file: str, lines: Iterable[int]) -> List[FunctionNode]:
        out = []
        for ln in sorted(set(lines)):
            r = self.db.execute('SELECT fid,name,file,start_line,end_line,params,is_static FROM functions WHERE file=? AND start_line<=? AND end_line>=? ORDER BY start_line', (file, ln, ln)).fetchone()
            if r and (not out or out[-1].fid != r[0]): out.append(FunctionNode(*r[:6], bool(r[6])))
        return out
    def callers(self, fid): return [r[0] for r in self.db.execute('SELECT DISTINCT caller FROM calls WHERE resolved=? ORDER BY caller', (fid,))]
    def callees(self, fid): return [r[0] for r in self.db.execute('SELECT DISTINCT resolved FROM calls WHERE caller=? AND resolved IS NOT NULL ORDER BY resolved', (fid,))]
    def unresolved(self, fid): return [r[0] for r in self.db.execute('SELECT DISTINCT callee FROM calls WHERE caller=? AND resolved IS NULL ORDER BY callee', (fid,))]
    def facts(self, fid): return self.db.execute('SELECT kind,callee,line FROM facts WHERE fid=? ORDER BY line', (fid,)).fetchall()
    def facts_v(self, fid): return self.db.execute('SELECT kind,callee,line,var FROM facts WHERE fid=? ORDER BY line', (fid,)).fetchall()
    def types(self, fid): return [r[0] for r in self.db.execute('SELECT DISTINCT type FROM fn_types WHERE fid=? ORDER BY type', (fid,))]
    def function(self, fid):
        r = self.db.execute('SELECT fid,name,file,start_line,end_line,params,is_static FROM functions WHERE fid=?', (fid,)).fetchone()
        return FunctionNode(*r[:6], bool(r[6])) if r else None
    def struct(self, name): return self.db.execute('SELECT name,file,start_line,end_line FROM structs WHERE name=? ORDER BY file', (name,)).fetchall()
