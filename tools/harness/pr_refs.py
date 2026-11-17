#!/usr/bin/env python3
"""PR 본문이 기대는 외부 사실을 확인한다 — 참조한 PR(#nnnn)의 상태, 언급한 커밋 해시가 이 브랜치에 있는지, JIRA 키·본문, TC 주장.
사용: python3 -m tools.harness.pr_refs --pr 7658 [--repo CUBRID/cubrid] [--src ~/dev/sources/cubrid] [--branch <ref>] [--offline]
2026-09-30: PR#7658 이 "CBRD-27408(#7915) 규칙을 포트했다" 고 적었는데 #7915 는 미머지로 닫혀 있었다 — 본문의 참조를 한 번 훑으면 잡히는 어긋남.

H-117(2026-11-17) — PR 번호·해시만 보던 것을 넓혔다:
  ① 경계: `\\b` 는 한글 조사(CBRD-27579가)를 단어로 이어 붙여 키를 못 뽑았다(PR#8497) → 영숫자 lookaround.
  ② JIRA 키마다 그 키를 단 PR 과 머지 여부. 전제 문장("~가 막는다·먼저·위에서·의존") 속 키가 머지되지 않았으면 경고(PR#8497).
  ③ 이 PR 의 JIRA 본문의 `{{이름}}`·백틱 식별자를 head 에서 `git grep -w` — 없으면 문서 제안 후보(PR#6466, 함수 이름 3개가 코드에 없었다).
  ④ PR 본문이 TC·단위 테스트에 넣었다고 한 문장의 백틱 SQL 조각을 tc/pr-N(두 TC 리포)·head 의 테스트 경로에서 찾는다(PR#8478·TC#4127).
네트워크(gh·JIRA)가 실패하면 그 항목만 '조회 실패' 로 남기고 나머지는 계속한다."""
import argparse, json, os, re, subprocess, sys

def sh(*a): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True).stdout

NB = r'(?<![0-9A-Za-z])'          # 앞 경계 — 한글 조사가 붙어도 키를 뽑는다(\b 는 한글을 단어 문자로 본다)
NA = r'(?![0-9A-Za-z])'           # 뒤 경계
JIRA_RE = re.compile(NB + r'CBRD-\d{4,5}' + NA)
PREREQ = re.compile(r'막[는아힌혀을고습]|전제|선행|먼저|위에서|위에 쌓|의존|기대(는|어|고)|이후에|머지(된|되어야|돼야)|depends|requires|blocked|on top of', re.I)
TC_SENT = re.compile(r'TC|테스트\s*케이스|답안|test ?case|단위\s*테스트|unit ?test', re.I)
JIRA = os.environ.get('JIRA_BASE', 'https://jira.cubrid.org')
TC_REPOS = ('CUBRID/cubrid-testcases', 'CUBRID/cubrid-testcases-private-ex')


def sentences(text):
    return [x.strip() for x in re.split(r'(?<=[.!?。])\s+|\n+', text) if x.strip()]


def jira_prs(key, repo, run=sh):
    """그 키를 제목·본문에 단 PR 들 [(번호, 상태)] — 상태는 MERGED/OPEN/CLOSED. 조회 실패면 None."""
    try: rows = json.loads(run('gh', 'pr', 'list', '-R', repo, '--search', key, '--state', 'all', '--json', 'number,state', '--limit', '20') or 'null')
    except Exception: return None
    return None if rows is None else [(r['number'], r['state']) for r in rows]


def jira_description(key, run=sh):
    """JIRA 본문(wiki markup). 조회 실패면 None."""
    try: d = json.loads(run('curl', '-snL', '--max-time', '20', f'{JIRA}/rest/api/2/issue/{key}?fields=description') or 'null')
    except Exception: return None
    return None if not isinstance(d, dict) or 'fields' not in d else (d['fields'].get('description') or '')


def code_names(text):
    """JIRA 본문의 {{이름}}·백틱 안의 식별자 — 코드 이름처럼 보이는 것(밑줄이 있거나 () 가 붙음)만."""
    raw = [x for x in re.findall(r'\{\{([^}\n]*)\}\}', text) + re.findall(r'`([^`\n]*)`', text) if 3 <= len(x) <= 80]
    out = set()
    for r in raw:
        if '/' in r or '=' in r: continue           # 경로(sql/_13_issues/…)·설정(키=값)은 코드 이름이 아니다
        for m in re.finditer(r'([A-Za-z_][A-Za-z0-9_]{3,})\s*(\(\s*\))?', r):
            n = m.group(1)
            if n.startswith('_') or n.endswith('_') or re.match(r'(?i)(cbrd_\d+|bug_bts_\w+|answer_\w+|\w+_\d{3})$', n): continue   # TC 이름·환경변수(MALLOC_CHECK_)
            if '_' in n or m.group(2): out.add(n)
    return sorted(out)


def missing_in_head(names, head, src, rc_of=None):
    """head 트리에 단어로 없는 이름. git grep 의 1(없음)만 셈 — 128(ref 없음 등)은 판단하지 않는다."""
    rc_of = rc_of or (lambda n: subprocess.run(['git', '-C', src, 'grep', '-q', '-w', n, head, '--'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode)
    return [n for n in names if rc_of(n) == 1]


def tc_claims(body):
    """TC·단위 테스트에 넣었다는 문장의 백틱 조각(공백 포함·8자 이상 — SQL 조각) [(문장 앞부분, 조각)]."""
    out = []
    for ln in body.split('\n'):   # 문장으로 먼저 자르면 코드 조각 안의 '.' 에서 끊겨 백틱 짝이 어긋난다 — 줄 단위로 짝을 맞춘다
        if TC_SENT.search(re.sub(r'`[^`]*`', '', ln)):
            out += [(ln.strip()[:60], f) for f in re.findall(r'`([^`\n]*)`', ln) if 8 <= len(f) <= 200 and ' ' in f.strip()]   # 짧은 조각(`-0`)도 짝으로 먹어야 그 뒤 짝이 안 어긋난다
    return out


def tc_vague(body):
    """TC·단위 테스트에 '더했다·넣었다' 고만 하고 조각(백틱)도 파일도 없는 문장 — 기계로 대조할 수 없으니 손으로 볼 목록(PR#8478 '그 꼴은 TC 에도 더했다')."""
    return [snt[:80] for snt in sentences(body) if TC_SENT.search(snt) and re.search(r'더했|넣었|추가했|반영했|added', snt) and '`' not in snt and not re.search(r'\w+\.(sql|sh|cpp|c)\b', snt)]


def tc_text(pr, run=sh):
    """두 TC 리포의 tc/pr-N 이 develop 에 더한 patch 를 이어 붙인 것. (텍스트, 찾은 브랜치 수)."""
    t, n = '', 0
    for r in TC_REPOS:
        try: d = json.loads(run('gh', 'api', f'repos/{r}/compare/develop...tc/pr-{pr}') or '{}')
        except Exception: continue
        if isinstance(d, dict) and 'files' in d:
            n += 1; t += '\n'.join(f.get('patch') or '' for f in d['files'])
    return t, n


def _norm(x): return re.sub(r'\s+', ' ', x).strip().lower()


def check(pr: int, body: str, head: str, repo: str = 'CUBRID/cubrid', src: str = None, jira_key: str = '', offline: bool = False) -> dict:
    """본문 참조 확인 결과: {'prs': [(ref, state, note)], 'hashes': [(hash, state, ok)], 'jiras', 'jira_rows', 'jira_names_missing', 'tc_missing', 'warnings'}"""
    src = src or os.path.expanduser('~/dev/sources/cubrid')
    prs = sorted({int(x) for x in re.findall(r'(?<![\w/])#(\d{4,5})' + NA, body) if int(x) != pr})
    hashes = sorted({h for h in re.findall(NB + r'([0-9a-f]{9,10})' + NA, body) if not h.isdigit()})
    jiras = sorted(set(JIRA_RE.findall(body)))
    rows = []
    for n in ([] if offline else prs):
        j = sh('gh', 'pr', 'view', str(n), '-R', repo, '--json', 'state,mergedAt,closedAt,title')
        try: d = json.loads(j)
        except Exception: rows.append((f'#{n}', '조회 실패', '')); continue
        st = d['state'] + ('' if d['state'] != 'CLOSED' else ' (미머지)')
        rows.append((f'#{n}', st, (d.get('mergedAt') or d.get('closedAt') or '')[:10] + ' ' + d['title'][:60]))
    hrows = []
    for h in hashes:
        exists = subprocess.run(['git', '-C', src, 'merge-base', '--is-ancestor', h, head], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
        known = subprocess.run(['git', '-C', src, 'cat-file', '-e', h + '^{commit}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
        hrows.append((h, '있음' if exists else ('저장소엔 있지만 head 조상 아님 (리베이스/force-push?)' if known else '로컬 저장소에 없음 (fetch 필요?)'), exists))
    warnings = []
    warn = [r for r in rows if '미머지' in r[1]]
    if warn: warnings.append('본문이 기대는 PR 중 미머지로 닫힌 것: ' + ', '.join(r[0] for r in warn) + ' — 그 규칙을 이 PR 이 "포트했다" 면 develop 과 어긋난다')
    missing_h = [h for h, _, ok in hrows if not ok]
    if missing_h: warnings.append('head 에 없는 커밋 해시: ' + ', '.join(missing_h))
    # ② JIRA 키 — 전제 문장 속 키가 머지되지 않았나
    own = jira_key or ''
    prereq = {k for snt in sentences(body) if PREREQ.search(snt) for k in JIRA_RE.findall(snt) if k != own}
    jrows = []
    for k in jiras[:10]:
        st = None if offline else jira_prs(k, repo)
        jrows.append((k, '조회 안 함' if offline else '조회 실패' if st is None else (', '.join(f'#{n} {s}' for n, s in st) or 'PR 없음'), k in prereq))
        if k in prereq and st is not None and not any(s == 'MERGED' for _, s in st):
            warnings.append(f'본문이 전제로 기대는 {k} 가 develop 에 없다(PR ' + (', '.join(f'#{n} {s}' for n, s in st) or '없음')
                            + ') — 그 이슈가 막아 준다는 주장은 지금 develop 에서 성립하지 않는다')
    # ③ 이 PR 의 JIRA 본문 식별자가 head 에 있나
    names_missing = []
    if own and not offline:
        desc = jira_description(own)
        if desc is None: warnings.append(f'{own} JIRA 본문 조회 실패 — 식별자 대조를 못 했다(netrc?)')
        elif desc and subprocess.run(['git', '-C', src, 'cat-file', '-e', head + '^{commit}'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode:
            warnings.append(f'head {head[:9]} 가 {src} 에 없어 {own} JIRA 본문 식별자 대조를 못 했다 — fetch 하거나 --branch 로 로컬 ref 를')
        elif desc:
            names_missing = missing_in_head(code_names(desc), head, src)
            if names_missing: warnings.append(f'{own} JIRA 본문의 식별자 중 head 에 없는 것: {names_missing} — 이름이 바뀌었으면 JIRA 본문 갱신 제안(문서 제안 후보)')
    # ④ TC·단위 테스트에 넣었다는 SQL 조각이 실제로 있나
    tc_missing = []
    claims = tc_claims(body)
    if claims and not offline:
        hay, nbr = tc_text(pr)
        hay = _norm(hay)
        if nbr == 0:
            warnings.append(f'tc/pr-{pr} 브랜치를 두 TC 리포에서 못 찾아 TC 조각 {len(claims)}개를 대조하지 못했다(머지 뒤 지워졌나?)')
            claims = []
        for snt, frag in claims:
            if _norm(frag) in hay: continue
            rc = subprocess.run(['git', '-C', src, 'grep', '-q', '-F', '-i', frag, head, '--', '*unit*', '*test*'], stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode
            if rc == 1: tc_missing.append((snt, frag))
        if tc_missing:
            warnings.append(f'본문이 TC·단위 테스트에 넣었다는 조각이 tc/pr-{pr}(찾은 TC 브랜치 {nbr}개)·head 의 테스트 경로에 없다: '
                            + '; '.join(f'`{f[:50]}`' for _, f in tc_missing))
    vague = tc_vague(body)
    if vague: warnings.append('TC·단위 테스트에 더했다는데 조각·파일이 없는 문장(손으로 tc/pr-%d 를 볼 것): ' % pr + ' / '.join(f'"{v}"' for v in vague))
    return {'pr': pr, 'head': head, 'prs': rows, 'hashes': hrows, 'jiras': jiras, 'jira_rows': jrows,
            'jira_names_missing': names_missing, 'tc_missing': tc_missing, 'warnings': warnings}

def to_md(r: dict) -> str:
    md = [f"# PR #{r['pr']} 본문 참조 확인 (head {r['head'][:9]})", '', '| 참조 PR | 상태 | 비고 |', '|---|---|---|']
    md += [f'| {a} | {b} | {c} |' for a, b, c in r['prs']]
    md += ['', '| 커밋 해시 | head 에 있나 |', '|---|---|'] + [f'| `{h}` | {st} |' for h, st, _ in r['hashes']]
    md += ['', '| JIRA | 그 키의 PR | 전제 문장 |', '|---|---|---|'] + [f"| {k} | {st} | {'예' if pre else ''} |" for k, st, pre in r.get('jira_rows', [])]
    md += [''] + ['⚠ ' + w for w in r['warnings']]
    return '\n'.join(md)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--src', default=os.path.expanduser('~/dev/sources/cubrid')); ap.add_argument('--branch', default='', help='커밋 존재를 볼 ref (기본: PR head)')
    ap.add_argument('--offline', action='store_true', help='gh·JIRA 조회 없이 본문 파싱과 로컬 git 만')
    a = ap.parse_args()
    meta = json.loads(sh('gh', 'pr', 'view', str(a.pr), '-R', a.repo, '--json', 'body,headRefOid,headRefName,number,title'))
    m = JIRA_RE.search(meta.get('title') or '') or JIRA_RE.search(meta.get('body') or '')
    print(to_md(check(a.pr, meta['body'] or '', a.branch or meta['headRefOid'], a.repo, a.src, m.group(0) if m else '', a.offline)))

if __name__ == '__main__': main()
