"""or-buf-undersized: 고정 요청/응답 버퍼(OR_ALIGNED_BUF)의 선언 크기와 or_pack_*/or_unpack_* 체인의
실제 배치를 산술로 비교한다. `or_pack_int64`·`or_pack_double`·`or_pack_ptr`(+unpack 짝)은 쓰기 전에
`PTR_ALIGN` 을 하므로 `OR_INT_SIZE + OR_INT64_SIZE * n` 같은 단순 합은 정렬 패딩만큼 모자라다(PR#7899).
LLM 판단이 아니라 산술이므로 매 리뷰에서 결정론으로 돈다.

모델은 LP64 하나다. 64비트에서 `OR_ALIGNED_BUF(size)` 는 `char buf[size]`(여유 0)이고
`OR_ALIGNED_BUF_START` 는 정렬 연산 없이 8정렬 주소를 준다(object_representation.h:1020-1030) —
그래서 오프셋 0 에서 정렬 올림만 누적하면 배치가 정확히 재현된다. 32비트는 `+ MAX_ALIGNMENT` 여유가 있어
같은 결함이 가려지므로 판정하지 않고 finding 문구에만 적는다.

오탐 0 이 최우선이다. 조금이라도 모르면 그 버퍼는 통째로 버린다(poison).
"""
import os
import re
from typing import Dict, List, Optional, Tuple

MAX_ALIGNMENT = 8
PTR_ALIGNMENT = 8          # LP64. 32비트(4)는 판정 대상이 아니다

# object_representation_constants.h / object_representation.h (64비트)
CONST = {
    'OR_BYTE_SIZE': 1, 'OR_SHORT_SIZE': 2, 'OR_INT_SIZE': 4, 'OR_INT64_SIZE': 8, 'OR_BIGINT_SIZE': 8,
    'OR_FLOAT_SIZE': 4, 'OR_DOUBLE_SIZE': 8, 'OR_PTR_SIZE': 8, 'OR_BIGINT_ALIGNED_SIZE': 16,
    'OR_DOUBLE_ALIGNED_SIZE': 16, 'OR_PTR_ALIGNED_SIZE': 16, 'OR_OID_SIZE': 8, 'OR_VPID_SIZE': 6,
    'OR_HFID_SIZE': 12, 'OR_BTID_SIZE': 10, 'OR_BTID_ALIGNED_SIZE': 12, 'OR_EHID_SIZE': 12,
    'OR_LOG_LSA_SIZE': 10, 'OR_LOG_LSA_ALIGNED_SIZE': 12, 'OR_TIME_SIZE': 4, 'OR_UTIME_SIZE': 4,
    'OR_DATE_SIZE': 4, 'OR_DATETIME_SIZE': 8, 'OR_MONETARY_SIZE': 12, 'OR_SHA1_SIZE': 20,
    'OR_MVCCID_SIZE': 8, 'MAX_ALIGNMENT': 8, 'INT_ALIGNMENT': 4, 'DOUBLE_ALIGNMENT': 8,
}

# 쓰기 전에 PTR_ALIGN 하는 커서형 패커 — object_representation.c 전수 확인(이게 전부다)
ALIGN = {'or_pack_int64': MAX_ALIGNMENT, 'or_unpack_int64': MAX_ALIGNMENT,
         'or_pack_double': MAX_ALIGNMENT, 'or_unpack_double': MAX_ALIGNMENT,
         'or_pack_ptr': PTR_ALIGNMENT, 'or_unpack_ptr': PTR_ALIGNMENT}

# 전진 바이트가 상수인 패커만. 여기 없는 or_(un)pack_* 를 만나면 그 체인은 거기서 끊는다.
ADVANCE = {
    'or_pack_int': 4, 'or_unpack_int': 4, 'or_pack_short': 4, 'or_unpack_short': 4,
    'or_pack_errcode': 4, 'or_unpack_errcode': 4, 'or_pack_lock': 4, 'or_unpack_lock': 4,
    'or_pack_float': 4, 'or_unpack_float': 4, 'or_pack_int64': 8, 'or_unpack_int64': 8,
    'or_pack_double': 8, 'or_unpack_double': 8, 'or_pack_ptr': 8, 'or_unpack_ptr': 8,
    'or_pack_oid': 8, 'or_unpack_oid': 8, 'or_pack_hfid': 12, 'or_unpack_hfid': 12,
    'or_pack_ehid': 12, 'or_unpack_ehid': 12, 'or_pack_btid': 12, 'or_unpack_btid': 12,
    'or_pack_log_lsa': 12, 'or_unpack_log_lsa': 12, 'or_pack_mvccid': 8, 'or_unpack_mvccid': 8,
    'or_pack_sha1': 20, 'or_unpack_sha1': 20,
}
# ENABLE_UNUSED_FUNCTION 가드 — 기본 빌드에 없다. 만나면 판정을 포기한다(경고 대상 아님).
GUARDED = {'or_pack_bigint', 'or_unpack_bigint', 'or_pack_time', 'or_unpack_time', 'or_pack_utime',
           'or_unpack_utime', 'or_pack_date', 'or_unpack_date', 'or_pack_monetary', 'or_unpack_monetary',
           'or_pack_string_array', 'or_unpack_string_array', 'or_pack_db_value_array', 'or_unpack_db_value_array'}

# 버퍼/커서를 받아도 그 버퍼에 쓰지 않는 소비자. 이 목록 밖의 호출이 커서를 만지면 포기한다.
READONLY = re.compile(r'^(OR_ALIGNED_BUF_SIZE|OR_ALIGNED_BUF_START|sizeof|assert'
                      r'|net_client_request\w*|net_client_send_data\w*'
                      r'|css_send_data_to_client\w*|css_send_reply_and_\w*data_to_client\w*)$')
KEYWORD = {'if', 'else', 'for', 'while', 'do', 'switch', 'return', 'sizeof', 'case'}

DECL = re.compile(r'\bOR_ALIGNED_BUF\s*\(([^();]*)\)\s*(\w+)\s*;')
START = re.compile(r'(\w+)\s*=\s*OR_ALIGNED_BUF_START\s*\(\s*(\w+)\s*\)')
STEP = re.compile(r'(\w+)\s*=\s*(or_(?:un)?pack_\w+)\s*\(\s*(\w+)\s*[,)]')
VOIDSTEP = re.compile(r'^\s*(?:\(\s*void\s*\*?\s*\)\s*)?(or_(?:un)?pack_\w+)\s*\(\s*(\w+)\s*[,)]')
ANYPACK = re.compile(r'\bor_(?:un)?pack_\w+')
CALL = re.compile(r'\b(\w+)\s*\(')
ALIAS = re.compile(r'^\s*(?:\w[\w\s\*]*?\*\s*)?(\w+)\s*=\s*(\w+)\s*;\s*$')
LOOPY = re.compile(r'\b(for|while|do|goto)\b|^\s*\w+\s*:\s*$')
BREAK = re.compile(r'[{}]|^\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b'
                   r'|\b(if|else|for|while|do|switch|case|return|goto)\b|^\s*\w+\s*:\s*$')
NETREQ = re.compile(r'\bnet_client_request\w*\s*\(')
SENDCLI = re.compile(r'\bcss_send_data_to_client\w*\s*\(([^;]*)\)\s*;', re.S)
PARAM = re.compile(r'\bchar\s*\*\s*(\w+)')


# ------------------------------------------------------------------ 소스 다루기
def strip_noise(src: str) -> str:
    """주석·문자열 리터럴을 공백으로 바꾼다(줄 수·열 위치 보존)."""
    out = list(src); i, n, mode = 0, len(src), None
    while i < n:
        c = src[i]; nxt = src[i + 1] if i + 1 < n else ''
        if mode is None:
            if c == '/' and nxt == '*': mode = 'c'; out[i] = out[i + 1] = ' '; i += 2; continue
            if c == '/' and nxt == '/': mode = 'l'; out[i] = out[i + 1] = ' '; i += 2; continue
            if c in '"\'': mode = c; i += 1; continue
        elif mode == 'c':
            if c == '*' and nxt == '/': mode = None; out[i] = out[i + 1] = ' '; i += 2; continue
            if c != '\n': out[i] = ' '
        elif mode == 'l':
            if c == '\n': mode = None
            else: out[i] = ' '
        else:
            if c == '\\': i += 2; continue
            if c == mode: mode = None
            elif c != '\n': out[i] = ' '
        i += 1
    return ''.join(out)


def function_body(repo_root: str, fn) -> str:
    """reachability.latch_pairing_by_var 와 같은 좌표(1-based, 양끝 포함)로 함수 본문만 자른다."""
    try:
        with open(os.path.join(repo_root, fn.file), encoding='utf-8', errors='replace') as f:
            return '\n'.join(f.read().split('\n')[fn.start_line - 1:fn.end_line])
    except Exception:
        return ''


def eval_size(expr: str) -> Optional[int]:
    """`OR_INT_SIZE + OR_INT64_SIZE * 2` 같은 상수식만 계산한다. 모르는 이름·매크로·sizeof 면 None."""
    e = expr.strip()
    if not e or 'sizeof' in e: return None
    e = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(CONST[m.group(0)]) if m.group(0) in CONST else '?', e)
    if '?' in e or not re.match(r'^[\d\s+\-*()]+$', e): return None
    try:
        v = eval(e, {'__builtins__': {}}, {})        # 토큰을 숫자·연산자로 제한한 뒤에만 계산한다
    except Exception:
        return None
    return v if isinstance(v, int) and 0 < v < (1 << 20) else None


def _align(off: int, boundary: int) -> int:
    return (off + boundary - 1) & ~(boundary - 1)


# ------------------------------------------------------------ 체인 시뮬레이션
class Chain(object):
    """버퍼 하나의 패킹 체인. need = 이 체인이 실제로 차지하는 바이트."""
    __slots__ = ('buf', 'declared', 'size_expr', 'decl_i', 'start_i', 'last_i', 'depth',
                 'need', 'over_i', 'steps', 'dead', 'poison', 'why')

    def __init__(self, buf, declared, size_expr, decl_i, start_i, depth):
        self.buf, self.declared, self.size_expr = buf, declared, size_expr
        self.decl_i, self.start_i, self.last_i, self.depth = decl_i, start_i, start_i, depth
        self.need, self.over_i, self.steps = 0, None, []
        self.dead = self.poison = False
        self.why = ''


def scan_body(src: str, seed: Optional[Tuple[str, str]] = None) -> Dict[str, Chain]:
    """함수 본문 하나에서 OR_ALIGNED_BUF 체인을 뽑아 오프셋을 누적한다.

    seed: (커서이름, 가상버퍼이름) — 서버 핸들러의 `request` 파라미터처럼 선언이 없는 커서를 0 에서 시작시킨다.
          declared 가 None 이므로 단독 finding 은 만들지 않고 쌍 대조에만 쓴다.
    반환: {버퍼이름: Chain}. poison 이면 호출자는 그 버퍼를 버려야 한다.
    """
    lines = strip_noise(src).split('\n')
    depth, d = [], 0
    for l in lines:
        depth.append(d); d += l.count('{') - l.count('}')

    decls = {}                                          # buf -> (식, 값, 줄 인덱스)
    dup = set()                                         # 같은 이름이 값 다르게 두 번 선언된 버퍼
    for i, l in enumerate(lines):
        for m in DECL.finditer(l):
            name, expr = m.group(2), m.group(1).strip()
            val = eval_size(expr)
            if name in decls and decls[name][1] != val:
                # #if 가지마다 다른 크기로 선언한 것. 전처리기 상태를 모르므로 어느 가지의 체인인지
                # 알 수 없다 — 2013년 boot_check_db_consistency 가 이 모양이었고, 마지막 선언이
                # 이기게 두면 blocking 오탐이 난다(반증 2026-09-21).
                dup.add(name)
            decls[name] = (expr, val, i)

    chains = {}                                         # buf -> Chain
    track = {}                                          # 커서 -> (buf, offset)

    def poison(buf, why=''):
        c = chains.get(buf)
        if c is not None:
            c.poison = True
            if not c.why: c.why = why

    def touched(line):
        hit = set()
        for cur, (buf, _) in track.items():
            if re.search(r'\b%s\b' % re.escape(cur), line): hit.add(buf)
        for buf in chains:
            if re.search(r'\b%s\b' % re.escape(buf), line): hit.add(buf)
        return hit

    def step(c, i, name, off):
        """패커 한 번의 [begin, end) 를 계산해 체인에 기록. 못 세면 None."""
        if name in GUARDED or name not in ADVANCE:
            c.dead = True; return None                  # 가변·미지 패커 → 여기서 끊는다
        if c.steps and (depth[i] != c.depth or any(BREAK.search(x) for x in lines[c.last_i + 1:i])):
            c.dead = True; return None                  # 조건 분기·전처리기·라벨이 끼면 끊는다
        if not c.steps: c.depth = depth[i]
        beg = _align(off, ALIGN[name]) if name in ALIGN else off
        end = beg + ADVANCE[name]
        c.steps.append({'idx': i, 'name': name, 'begin': beg, 'end': end})
        c.last_i, c.need = i, max(c.need, end)
        if c.declared is not None and end > c.declared and c.over_i is None: c.over_i = i
        return end

    if seed:
        cur, buf = seed
        chains[buf] = Chain(buf, None, '', 0, 0, depth[0] if depth else 0)
        track[cur] = (buf, 0)

    for i, l in enumerate(lines):
        for m in START.finditer(l):                                     # ① 체인 시작
            cur, buf = m.group(1), m.group(2)
            if buf not in decls: continue
            if buf in dup: continue                                     # #if 로 크기가 갈린 선언 — 판정 불가
            if buf in chains: poison(buf, 'restart'); continue           # 같은 버퍼를 다시 씀 — 포기
            expr, declared, di = decls[buf]
            chains[buf] = Chain(buf, declared, expr, di, i, depth[i])
            track[cur] = (buf, 0)

        packs = ANYPACK.findall(l)
        if packs:                                                       # ② 패커 호출
            if len(packs) != 1:
                for b in touched(l): poison(b, 'multi-pack-line')
                continue
            m, v = STEP.search(l), VOIDSTEP.match(l)
            if m:                                                       # dst = or_pack_x (src, ...)
                dst, name, srcv = m.group(1), m.group(2), m.group(3)
                rest = l[m.end(3):]
                if srcv not in track:                                   # 추적 밖 버퍼의 체인 — 대상만 놓아준다
                    for b in touched(rest): poison(b, 'cursor-as-argument')
                    track.pop(dst, None); continue
                buf, off = track[srcv]; c = chains[buf]
                if c.poison or c.dead: track.pop(dst, None); continue
                end = step(c, i, name, off)
                if end is None: track.pop(dst, None); continue
                track[dst] = (buf, end); continue
            if v:                                                       # (void) or_pack_x (src, ...) — 반환값 버림
                name, srcv = v.group(1), v.group(2)
                if srcv not in track: continue
                buf, off = track[srcv]; c = chains[buf]
                if not (c.poison or c.dead): step(c, i, name, off)      # 쓰기 범위만 세고 커서는 전진하지 않는다
                continue
            for b in touched(l): poison(b, 'unparsed-pack-line')
            continue

        for m in CALL.finditer(l):                                      # ③ 낯선 호출이 커서를 만지면 포기
            name = m.group(1)
            if name == 'OR_ALIGNED_BUF' or name in KEYWORD or READONLY.match(name): continue
            if name.startswith(('or_pack_', 'or_unpack_')): continue
            for b in touched(l): poison(b, 'foreign-call:' + name)

        a = ALIAS.match(l)                                              # ④ 단순 별칭
        if a and a.group(2) in track and a.group(1) != a.group(2):
            track[a.group(1)] = track[a.group(2)]

    for c in chains.values():                                           # ⑤ 루프·점프가 범위에 걸리면 포기
        if c.steps and any(LOOPY.search(x) for x in lines[c.start_i:c.last_i + 1]):
            c.poison = True
            if not c.why: c.why = 'loop/goto'
    return chains


# ----------------------------------------------------------- 서버·클라이언트 쌍
def net_request_map(repo_root: str, rel: str = 'src/communication/network_sr.c') -> Dict[str, str]:
    """`net_Requests[NET_SERVER_X]` → `processing_function = s...;` 순서대로 읽어 상수 → 핸들러 이름."""
    out = {}
    try:
        txt = strip_noise(open(os.path.join(repo_root, rel), encoding='utf-8', errors='replace').read())
    except Exception:
        return out
    cur = None
    for l in txt.split('\n'):
        m = re.search(r'net_Requests\s*\[\s*(NET_SERVER_\w+)\s*\]', l)
        if m: cur = m.group(1)
        m2 = re.search(r'processing_function\s*=\s*(\w+)\s*;', l)
        if m2 and cur: out[cur] = m2.group(1); cur = None
    return out


def _balanced(s: str, open_pos: int) -> str:
    depth, j = 0, open_pos
    while j < len(s):
        if s[j] == '(': depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0: return s[open_pos + 1:j]
        j += 1
    return ''


def cl_request(src: str) -> Optional[Tuple[str, List[str]]]:
    """클라이언트 함수의 net_client_request 호출 → (NET_SERVER_상수, [OR_ALIGNED_BUF_SIZE 인자 순서대로])."""
    s = strip_noise(src)
    m = NETREQ.search(s)
    if not m: return None
    args = _balanced(s, m.end() - 1)
    c = re.search(r'\b(NET_SERVER_\w+)\b', args)
    if not c: return None
    return c.group(1), re.findall(r'OR_ALIGNED_BUF_SIZE\s*\(\s*(\w+)\s*\)', args)


def sr_reply_buf(src: str) -> Optional[str]:
    """서버 핸들러가 css_send_data_to_client 에 넘기는 응답 버퍼 이름(하나로 확정될 때만)."""
    bufs = {b for m in SENDCLI.finditer(strip_noise(src))
            for b in re.findall(r'OR_ALIGNED_BUF_SIZE\s*\(\s*(\w+)\s*\)', m.group(1))}
    return bufs.pop() if len(bufs) == 1 else None


# --------------------------------------------------------------- finding 만들기
WHY_OVER = ('64비트에서 OR_ALIGNED_BUF(size) 는 char buf[size] 라 여유 바이트가 없고 START 는 8정렬이라, '
            '{cause}{need}B 가 선언한 {declared}B 를 {over}B 넘어선다 — '
            '스택의 이웃 변수를 덮어쓰고(디버그 빌드는 PTR_ALIGN 안의 memset 이 페이로드보다 먼저 넘어간다), '
            '송신 길이는 OR_ALIGNED_BUF_SIZE 가 선언값 {declared}B 를 그대로 주므로 마지막 필드가 잘려 나간다. '
            '32비트 빌드는 +MAX_ALIGNMENT 여유에 가려 그대로 통과하므로 64비트에서만 드러난다.')
PROP_OVER = ('둘 중 하나. (a) **필드 순서를 바꿔 패딩을 없앤다** — 8정렬 필드(or_pack_int64/double/ptr)를 앞에, '
             '4바이트 필드를 뒤에. 이러면 선언식을 그대로 두고도 맞는다. '
             '(b) 선언식에 패딩을 **정확히 {need}B 만큼** 드러낸다(예: `OR_INT_SIZE + MAX_ALIGNMENT + ...`). '
             '⚠ 넉넉하게 키우지 마라 — `OR_ALIGNED_BUF_SIZE` 가 선언값을 **그대로 송신 길이로** 쓰므로, '
             '필요보다 큰 선언은 초기화되지 않은 스택 바이트를 회선으로 내보낸다'
             '(develop 의 sqmgr_execute_query 가 memset 으로 막고 있는 그 패턴이다). '
             '서버와 클라이언트 선언을 함께 고쳐야 한다 — 한쪽만 고치면 송수신 길이가 어긋난다.')
WHY_PAIR = ('같은 요청 {req} 의 고정 버퍼 크기가 서버 {a}B, 클라이언트 {b}B 로 다르다 — '
            'css_send_data_to_client 는 서버 선언값만큼 보내고 net_client_request 는 클라이언트 선언값만큼 받으므로 '
            '작은 쪽 기준으로 뒤쪽 필드가 잘리거나 읽히지 않은 바이트가 남아 필드가 밀려 해석될 수 있다. '
            '한쪽만 고친 변경에서는 컴파일도 테스트도 이 차이를 잡아 주지 않는다.')
PROP_PAIR = ('두 선언을 같은 상수식으로 맞춘다 — 서버 `{sf}` ({sfile}) 와 클라이언트 `{cf}` ({cfile}) 의 '
             '버퍼 선언·필드 순서·필드 개수를 나란히 두고 대조하고, 가능하면 공용 매크로 한 곳으로 뺀다.')


def _fix_expr(steps: List[dict]) -> Tuple[str, int]:
    """정렬을 포함한 보수적 선언식 제안 — 정렬 패커는 ALIGNED 상수로 잡는다."""
    n64 = sum(1 for s in steps if s['name'] in ('or_pack_int64', 'or_unpack_int64'))
    ndb = sum(1 for s in steps if s['name'] in ('or_pack_double', 'or_unpack_double'))
    npt = sum(1 for s in steps if s['name'] in ('or_pack_ptr', 'or_unpack_ptr'))
    rest = sum(s['end'] - s['begin'] for s in steps if s['name'] not in ALIGN)
    parts = []
    if rest: parts.append('OR_INT_SIZE * %d' % (rest // 4) if rest % 4 == 0 else '%d' % rest)
    for n, nm in ((n64, 'OR_BIGINT_ALIGNED_SIZE'), (ndb, 'OR_DOUBLE_ALIGNED_SIZE'), (npt, 'OR_PTR_ALIGNED_SIZE')):
        if n: parts.append(nm + (' * %d' % n if n > 1 else ''))
    return ' + '.join(parts), rest + 16 * (n64 + ndb + npt)


def _anchor(changed_lines: List[int], fn, want: int) -> Tuple[int, str]:
    """layer='코드' 는 anchor 가 diff ±3 안이어야 valid 다(adjudicate.py:42-43, 64).
    넘친 줄이 diff 밖이면 같은 함수의 가장 가까운 변경 줄로 옮기고, 그것도 없으면 '설계' 로 낸다."""
    if any(abs(l - want) <= 3 for l in changed_lines): return want, '코드'
    inside = [l for l in changed_lines if fn.start_line <= l <= fn.end_line]
    if inside: return min(inside, key=lambda l: abs(l - want)), '코드'
    return want, '설계'


def _finding(fid, layer, file, line, claim, why, proposal, evidence, verification, severity='non-blocking'):
    """키 구성은 perf_claims.F() 와 같게 두고 checker 만 덧붙인다(finding.json 은 additionalProperties 를 막지 않는다)."""
    return {'id': fid, 'layer': layer, 'file': file, 'line': line, 'claim': claim, 'why': why,
            'proposal': proposal, 'category': '버그 가능성', 'importance': '높음', 'evidence': evidence,
            'rule_ids': ['SER-05'], 'severity': severity, 'verification': verification,
            'auto': True, 'checker': 'or-buf-undersized'}


def analyze(repo_root: str, g, changed: Dict[str, List[int]], changed_fids: List[str]) -> Tuple[List[Dict], int]:
    """변경된 함수의 OR_ALIGNED_BUF 체인을 시뮬레이션해 auto finding 을 만든다. 확신이 없으면 아무것도 내지 않는다."""
    out, seen, cl_side = [], set(), {}
    skipped = 0                                         # 판정 불가로 버린 체인 — 침묵을 '깨끗함'으로 읽지 않기 위해 센다
    for fid in changed_fids:
        fn = g.function(fid)
        if fn is None or not fn.file.endswith(('.c', '.cpp', '.cc')): continue
        src = function_body(repo_root, fn)
        if 'OR_ALIGNED_BUF' not in src: continue
        chains = scan_body(src)
        lines = changed.get(fn.file, [])
        for buf, c in sorted(chains.items()):
            if c.poison or c.declared is None or not c.steps:
                skipped += 1; continue
            if c.over_i is None: continue                    # 초과 없음 = 정상 (건너뛴 것이 아니다)
            line, layer = _anchor(lines, fn, fn.start_line + c.over_i)
            if (fn.file, line, buf) in seen: continue
            seen.add((fn.file, line, buf))
            who = ', '.join(sorted({s['name'] for s in c.steps if s['name'] in ALIGN}))
            # 정렬 패커가 하나도 없으면 초과 원인은 패딩이 아니라 '필드가 선언식보다 많다' 이다.
            # 둘을 같은 문장으로 쓰면 verification 줄(패딩 없는 배치)과 모순돼 신뢰를 잃는다.
            cause = ('정렬하는 패커(%s) 앞의 패딩까지 더한 ' % who) if who else '패킹 체인이 쓰는 '
            layout = ' → '.join('%s[%d:%d]' % (re.sub(r'^or_(un)?pack_', '', s['name']), s['begin'], s['end'])
                                for s in c.steps)
            out.append(_finding(
                'SER-05-orbuf-%s-%s-%d' % (fn.name, buf, line), layer, fn.file, line,
                '%s 는 OR_ALIGNED_BUF (%s) = %dB 로 선언됐는데 이 함수의 패킹 체인은 %dB 를 쓴다 — %s'
                % (buf, c.size_expr, c.declared, c.need,
                   '정렬하는 패커 앞의 패딩이 선언식에 들어 있지 않다(64비트)' if who
                   else '선언식이 실제 패킹하는 필드보다 작다(64비트)'),
                WHY_OVER.format(cause=cause, need=c.need, declared=c.declared, over=c.need - c.declared),
                PROP_OVER.format(need=c.need),
                ['%s:%d' % (fn.file, fn.start_line + c.decl_i)]
                + ['%s:%d' % (fn.file, fn.start_line + s['idx']) for s in c.steps],
                {'method': 'static',
                 'result': '오프셋 시뮬레이션(LP64, START 8정렬): %s → 필요 %dB > 선언 %dB' % (layout, c.need, c.declared)},
                'blocking'))
        if fn.file.endswith('network_interface_cl.c'):
            cc = cl_request(src)
            if cc and len(cc[1]) >= 2: cl_side[cc[0]] = (fn, chains, cc[1][0], cc[1][1])

    if not cl_side: return out, skipped
    reqmap = net_request_map(repo_root)
    for req, (cfn, cchains, creq_buf, crep_buf) in sorted(cl_side.items()):
        sname = reqmap.get(req)
        if not sname: continue
        row = g.db.execute('SELECT fid FROM functions WHERE name=? ORDER BY fid', (sname,)).fetchone()
        if not row: continue
        sfn = g.function(row[0]); ssrc = function_body(repo_root, sfn)
        if not ssrc: continue
        sbuf = sr_reply_buf(ssrc)
        if not sbuf: continue
        pm = PARAM.search(sfn.params or '')
        schains = scan_body(ssrc, seed=(pm.group(1), '@request') if pm else None)
        sc, cc = schains.get(sbuf), cchains.get(crep_buf)
        line, layer = _anchor(changed.get(cfn.file, []), cfn,
                              cfn.start_line + (cc.decl_i if cc else 0))
        if (sc and cc and not sc.poison and not cc.poison and sc.declared is not None
                and cc.declared is not None and sc.declared != cc.declared):
            out.append(_finding(
                'SER-05-pair-reply-%s' % req, layer, cfn.file, line,
                '%s 의 응답 버퍼 선언이 서버 %s (%dB) 와 클라이언트 %s (%dB) 에서 다르다'
                % (req, sfn.name, sc.declared, cfn.name, cc.declared),
                WHY_PAIR.format(req=req, a=sc.declared, b=cc.declared),
                PROP_PAIR.format(sf=sfn.name, sfile=sfn.file, cf=cfn.name, cfile=cfn.file),
                ['%s:%d' % (cfn.file, cfn.start_line + cc.decl_i), '%s:%d' % (sfn.file, sfn.start_line + sc.decl_i)],
                {'method': 'static', 'result': '선언식 대조: 서버 `%s` = %dB vs 클라이언트 `%s` = %dB'
                                               % (sc.size_expr, sc.declared, cc.size_expr, cc.declared)}))
        sq, cq = schains.get('@request'), cchains.get(creq_buf)
        if (sq and cq and not sq.poison and not cq.poison and not sq.dead and cq.declared is not None
                and sq.need > cq.declared):
            out.append(_finding(
                'SER-05-pair-request-%s' % req, layer, cfn.file, line,
                '%s 의 요청을 서버 %s 는 %dB 까지 읽는데 클라이언트 %s 가 보내는 요청 버퍼는 %dB 다'
                % (req, sfn.name, sq.need, cfn.name, cq.declared),
                WHY_PAIR.format(req=req, a=sq.need, b=cq.declared),
                PROP_PAIR.format(sf=sfn.name, sfile=sfn.file, cf=cfn.name, cfile=cfn.file),
                ['%s:%d' % (cfn.file, cfn.start_line + cq.decl_i),
                 '%s:%d' % (sfn.file, sfn.start_line + sq.steps[-1]['idx'])]
                if sq.steps else ['%s:%d' % (cfn.file, cfn.start_line + cq.decl_i)],
                {'method': 'static', 'result': '서버 언패킹 %dB vs 클라이언트 요청 선언 `%s` = %dB'
                                               % (sq.need, cq.size_expr, cq.declared)}))
    return out, skipped
