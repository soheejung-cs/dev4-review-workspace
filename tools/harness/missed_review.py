#!/usr/bin/env python3
"""놓친 지적 대조 — 다른 리뷰어(사람·봇)가 PR 에 단 지적을 우리 리뷰(하네스 findings·우리가 연 스레드)와 대조해 '우리가 놓친 것' 후보를 가른다.
사용: python3 -m tools.harness.missed_review --pr 8242 [--repo CUBRID/cubrid] [--me <login>] [--out <dir>] [--window 15]
입력: review_threads.collect(스레드 전체), REST pulls/<n>/reviews(본문 있는 리뷰), ~/dev/utils/harness-out/<PR>/<sha>/ 마다
      findings.adjudicated.json → findings.json → findings.G*.json(있는 것 하나), manifest.json 의 changed_functions·head, codegraph.sqlite3.
산출: <out>/missed.json · <out>/missed.md (기본 out = ~/dev/utils/harness-out/missed/<PR>). 다른 리뷰어의 지적마다
      - 대응: '근처'(같은 파일 ±window 줄에 우리 finding·스레드) / '같은 심볼'(본문의 함수·식별자가 우리 지적에 있음) / '놓침 후보'
      - 팩: 그 줄이 우리가 리뷰한 head 의 변경 함수 안이었나('변경 함수' / '팩 파일, 변경 함수 밖' / '팩 밖' / '하네스 산출 없음')
      - 시점: 지적 시점 커밋이 우리가 리뷰한 head 와 같은가(다르면 그 사이 고쳐졌을 수 있다 — 반성 대상인지 LLM 이 가른다)
      놓침 후보마다 반성 칸(②타당성 ③왜 놓쳤나 ④승격)을 비워 둔다.
계기: 2026-11-03 사용자 지시 "다른 사람 리뷰 보면서 타당해 보이면 우리는 왜 리뷰에서 그런 지적을 못했는지 반성하는 피드백 루프도 하네스에 넣어 줘".
판단(타당성·원인)은 LLM 이 missed.md 를 채워 references/놓친지적-반성.md 에 append 하고 /self-evolution 으로 승격한다 — 이 도구는 대조만 한다(T-10).
한계: 줄 번호는 지적 시점 커밋 기준이라 우리 head 와 다르면 '근처'·'팩' 판정이 몇 줄 어긋날 수 있다(시점 칸을 같이 본다). 일반 리뷰 본문은 줄이 없어 심볼로만 대조한다.
사이드이펙트: 읽기만 한다(gh api, 로컬 파일). 산출 파일 두 개만 쓴다."""
import argparse, glob, json, os, re, sqlite3, subprocess, sys, time

from tools.harness import review_threads

# 같은 함수 안의 다른 줄을 짚는 일이 흔해 15줄 — 더 넓히면 다른 지적을 '다룸' 으로 잘못 묶는다(PR#8242 스레드 7건으로 잼)
DEFAULT_WINDOW = 15
# 리뷰 봇 — 사람과 같은 대조 대상이지만 표에서 구분한다
BOTS = ('greptile-apps', 'chatgpt-codex-connector', 'coderabbitai', 'github-actions', 'copilot-pull-request-reviewer')
HOUT = os.path.expanduser('~/dev/utils/harness-out')
# 본문에서 대조에 쓸 심볼: 함수 호출꼴·식별자(밑줄·대문자 상수), 흔한 낱말은 뺀다
SYM = re.compile(r'`([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(\s*\))?`|\b([a-z][a-z0-9]*_[a-z0-9_]+|[A-Z][A-Z0-9]*_[A-Z0-9_]+)\b')
STOP = {'row_count', 'max_rows', 'pt_node', 'db_value', 'no_error', 'er_failed'}


def symbols(text):
    out = set()
    for m in SYM.finditer(text or ''):
        s = (m.group(1) or m.group(2) or '').lower()
        if len(s) > 3 and s not in STOP:
            out.add(s)
    return out


def _ours(pr):
    """우리 리뷰 산출 — head 마다 findings(파일·줄·텍스트)·변경 함수·codegraph 경로."""
    runs = []
    for d in sorted(glob.glob(os.path.join(HOUT, str(pr), '*/'))):
        man = os.path.join(d, 'manifest.json')
        if not os.path.exists(man):
            continue
        m = json.load(open(man))
        fpath = next((p for p in (os.path.join(d, 'findings.adjudicated.json'), os.path.join(d, 'findings.json')) if os.path.exists(p)), None)
        flist = []
        for p in ([fpath] if fpath else sorted(glob.glob(os.path.join(d, 'findings.G*.json')))):
            j = json.load(open(p))
            flist += j['findings'] if isinstance(j, dict) and 'findings' in j else (j if isinstance(j, list) else [])
        runs.append({'dir': d, 'head': m.get('head', ''), 'changed': set(m.get('changed_functions') or []),
                     'codegraph': os.path.join(d, 'codegraph.sqlite3'),
                     'findings': [{'id': f.get('id'), 'file': f.get('file'), 'line': f.get('line') or 0,
                                   'text': ' '.join(str(f.get(k) or '') for k in ('claim', 'why', 'proposal', 'title'))} for f in flist]})
    return runs


def _function_at(codegraph, path, line):
    if not line or not os.path.exists(codegraph):
        return None
    c = sqlite3.connect(codegraph)
    r = c.execute('select name from functions where file = ? and start_line <= ? and end_line >= ? order by start_line desc limit 1',
                  (path, line, line)).fetchone()
    c.close()
    return r[0] if r else None


def pack_status(run, path, line):
    if run is None:
        return '하네스 산출 없음', None
    fn = _function_at(run['codegraph'], path, line)
    if fn and f'{path}:{fn}' in run['changed']:
        return '변경 함수', fn
    if any(c.startswith(path + ':') for c in run['changed']):
        return '팩 파일, 변경 함수 밖', fn
    return '팩 밖', fn


def match(item, run, mine, window=DEFAULT_WINDOW):
    """다른 리뷰어 지적 하나 → (대응, 근거). 순수 함수 — tests 가 가짜 입력으로 부른다.
    줄 없는 리뷰 본문에 심볼이 하나도 없으면 '요약'("의견 반영돼 approve" 처럼 인라인을 가리키는 말 — PR#8242 에서 2건이 놓침 후보로 섞였다)."""
    if item['kind'] == 'review' and not symbols(item['body']):
        return '요약', []
    near = []
    for f in (run['findings'] if run else []):
        if item['path'] and f['file'] == item['path'] and item['line'] and abs((f['line'] or 0) - item['line']) <= window:
            near.append(f'finding {f["id"]} ({f["file"]}:{f["line"]})')
    for t in mine:
        if item['path'] and t['path'] == item['path'] and item['line'] and abs((t['line'] or 0) - item['line']) <= window:
            near.append(f'우리 스레드 {t["url"]}')
    if near:
        return '근처', near
    sy = symbols(item['body'])
    shared = []
    for f in (run['findings'] if run else []):
        common = sy & symbols(f['text'])
        if common:
            shared.append(f'finding {f["id"]}: {", ".join(sorted(common)[:4])}')
    for t in mine:
        common = sy & symbols(t['first_body'])
        if common:
            shared.append(f'우리 스레드 {t["url"]}: {", ".join(sorted(common)[:4])}')
    return ('같은 심볼', shared) if shared else ('놓침 후보', [])


def collect(pr, repo, me):
    data = review_threads.collect(pr, repo)
    author = data['author']
    r = subprocess.run(['gh', 'api', f'repos/{repo}/pulls/{pr}/reviews', '--paginate'], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    reviews = json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip().startswith('[') else []
    others, mine = [], []
    for t in data['threads']:
        (mine if t['opened_by'] == me else others).append(t)
    others = [t for t in others if t['opened_by'] != author]
    items = [{'kind': 'thread', 'by': t['opened_by'], 'bot': t['opened_by'] in BOTS, 'path': t['path'], 'line': t['line'], 'url': t['url'],
              'at': t['first_at'], 'commit': t['original_commit'], 'resolved': t['resolved'], 'body': t['first_body']} for t in others]
    for v in reviews:
        login = (v.get('user') or {}).get('login', '').replace('[bot]', '')
        if login in (me, author) or not (v.get('body') or '').strip():
            continue
        items.append({'kind': 'review', 'by': login, 'bot': login in BOTS, 'path': '', 'line': 0, 'url': v.get('html_url', ''),
                      'at': (v.get('submitted_at') or '')[:10], 'commit': v.get('commit_id', ''), 'resolved': None, 'body': v['body']})
    return data, items, mine


def to_md(res):
    L = [f'# PR #{res["pr"]} 놓친 지적 대조 (우리 = {res["me"]}, {res["collected"]})', '',
         f'우리 리뷰 head: {", ".join(r["head"][:9] for r in res["runs"]) or "없음(하네스 산출 없음)"} · 다른 리뷰어 지적 {len(res["items"])}건 — '
         f'놓침 후보 {sum(1 for i in res["items"] if i["match"] == "놓침 후보")} · 같은 심볼 {sum(1 for i in res["items"] if i["match"] == "같은 심볼")} · 근처 {sum(1 for i in res["items"] if i["match"] == "근처")} · 요약 {sum(1 for i in res["items"] if i["match"] == "요약")}', '',
         '대조는 기계적이다 — "근처"·"같은 심볼" 도 같은 지적인지는 읽어서 확인하고, "놓침 후보" 는 타당한 것만 반성 칸을 채워 `references/놓친지적-반성.md` 에 append 한다(harness-review 절차 10).', '',
         '| # | 누가 | 어디 | 시점 커밋 | 우리 head 와 | 팩 | 대응 | 요지 |', '|---|---|---|---|---|---|---|---|']
    for n, i in enumerate(res['items'], 1):
        where = f'{i["path"]}:{i["line"]}' if i['path'] else '(리뷰 본문)'
        gist = ' '.join(re.sub(r'<[^>]+>', ' ', i['body']).split())[:90].replace('|', '/')
        L.append(f'| {n} | {i["by"]}{" (봇)" if i["bot"] else ""} | {where} | `{(i["commit"] or "")[:9]}` | {i["timing"]} | {i["pack"]} | {i["match"]} | {gist} |')
    for n, i in enumerate(res['items'], 1):
        if i['match'] != '놓침 후보':
            continue
        L += ['', f'## {n}. {i["by"]} — {i["path"] or "리뷰 본문"}{":" + str(i["line"]) if i["line"] else ""} ({i["at"]}) {i["url"]}',
              f'- ① 지적: {" ".join(re.sub(r"<[^>]+>", " ", i["body"]).split())[:400]}',
              f'- 기계 판정: 팩 = {i["pack"]}{" (" + i["function"] + ")" if i.get("function") else ""} · 시점 = {i["timing"]}',
              '- ② 타당성: (확인 방법·결과 — 타당하지 않으면 그 근거만 적고 끝)',
              '- ③ 왜 놓쳤나: [팩 밖 | 팩 안·판정 누락 | 규칙 없음 | 동적 확인 부족 | 리뷰 뒤 바뀐 코드 | 범위·스펙 판단 차이] + 한 줄',
              '- ④ 승격: 자가진화 규칙 #NNN / 대기열 H-NN / 팩·프롬프트 변경 / 승격 안 함(이유)']
    return '\n'.join(L) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--pr', type=int, required=True)
    ap.add_argument('--repo', default='CUBRID/cubrid')
    ap.add_argument('--me', default='', help='우리 GitHub 로그인(기본: gh api user)')
    ap.add_argument('--out', default='')
    ap.add_argument('--window', type=int, default=DEFAULT_WINDOW)
    a = ap.parse_args()
    me = a.me or subprocess.run(['gh', 'api', 'user', '-q', '.login'], stdout=subprocess.PIPE, universal_newlines=True).stdout.strip()
    data, items, mine = collect(a.pr, a.repo, me)
    runs = _ours(a.pr)
    run = runs[-1] if runs else None
    if run:
        # 가장 최근 head 의 산출을 쓴다 — 여러 head 를 리뷰했으면 각 head 의 findings 를 합쳐 대조한다
        run = dict(run, findings=[f for r in runs for f in r['findings']])
    for i in items:
        i['match'], i['evidence'] = match(i, run, mine, a.window)
        i['pack'], i['function'] = pack_status(run, i['path'], i['line']) if i['path'] else ('(줄 없음)', None)
        heads = {r['head'] for r in runs}
        i['timing'] = '같음' if i['commit'] in heads else ('하네스 산출 없음' if not heads else '다름(그 사이 바뀌었을 수 있음)')
    res = {'pr': a.pr, 'repo': a.repo, 'me': me, 'author': data['author'], 'collected': time.strftime('%Y-%m-%dT%H:%M:%S'),
           'runs': [{'dir': r['dir'], 'head': r['head'], 'n_findings': len(r['findings'])} for r in runs], 'items': items}
    out = a.out or os.path.join(HOUT, 'missed', str(a.pr))
    os.makedirs(out, exist_ok=True)
    json.dump(res, open(os.path.join(out, 'missed.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(out, 'missed.md'), 'w').write(to_md(res))
    n_miss = sum(1 for i in items if i['match'] == '놓침 후보')
    print(f'[missed] PR #{a.pr}: 다른 리뷰어 지적 {len(items)}건 — 놓침 후보 {n_miss} · 같은 심볼 '
          f'{sum(1 for i in items if i["match"] == "같은 심볼")} · 근처 {sum(1 for i in items if i["match"] == "근처")} -> {out}/missed.md')
    if not runs:
        print('  ⚠ 이 PR 의 하네스 산출(~/dev/utils/harness-out/<PR>/)이 없다 — 대응·팩 칸은 비교 없이 "놓침 후보"·"하네스 산출 없음" 으로 나온다', file=sys.stderr)


if __name__ == '__main__':
    main()
