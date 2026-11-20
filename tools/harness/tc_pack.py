#!/usr/bin/env python3
"""엔진 PR 의 TC PR(두 TC 리포의 tc/pr-<N>)을 리뷰 입력으로 모은다 — OUT/tc_pack.md · tc_pack.json.

사용자 지시 2026-11-20 "리뷰할때 tc 변경 사항 있으면 같이 리뷰하는거 잊지 말고 하네스에 넣어놔".
계기: PR#8613 리뷰(게시 5988001118)가 cubrid-testcases#4303(cbrd_28141, 14 케이스)을 보지 않고 끝났다 —
run full 이 엔진 diff 만 팩으로 만들고, 스킬 절차에도 TC PR 단계가 없었다.

  python3 -m tools.harness.tc_pack --pr 8613 [--out OUT]   # 단독 실행(OUT 기본: ~/dev/utils/harness-out/<pr>/<head9>)

산출:
  - 리포마다 TC PR 번호·상태·draft·제목·본문, 바뀐 파일(+/-), 파일별 diff(파일당 MAX_FILE_LINES 줄, 전체 MAX_TOTAL_LINES 줄까지 — 넘으면 잘림 표시)
  - 결정론 점검(체크리스트의 기계로 볼 수 있는 몫): 제목·본문이 봇 기본값인가, 본문 첫 줄 JIRA 링크, 바뀐 파일 0 인 Draft(리뷰 대상 아님),
    shell TC 면 private-ex 룰북 경로, 답안만 바뀐 TC 면 '답안 정렬' 꼴(TC-PR-본문.md) 요구
판단(케이스가 엔진 변경을 실제로 지키나, 수정 전 NOK·수정 후 OK 근거, 답안 변경이 의도를 해치지 않나)은 리뷰 단계가 한다 — 스킬 harness-review 절차 2-2.
네트워크(gh)가 실패하면 그 리포만 '조회 실패' 로 남긴다."""
import argparse, json, os, re, subprocess

TC_REPOS = ('CUBRID/cubrid-testcases', 'CUBRID/cubrid-testcases-private-ex')
BOT_TITLE = re.compile(r'Draft: TC changes for PR', re.I)
JIRA_LINE = re.compile(r'https?://jira\.cubrid\.org/browse/CBRD-\d+', re.I)   # 첫 줄 어디든(`- Issue: http…` 꼴도 받는다)
MAX_FILE_LINES = 160
MAX_TOTAL_LINES = 4000


def sh(*a, timeout=120):
    try:
        p = subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, timeout=timeout)
        return p.returncode, p.stdout
    except subprocess.TimeoutExpired:
        return 124, ''


def tc_prs(pr, repo, run=sh):
    rc, out = run('gh', 'pr', 'list', '-R', repo, '--head', f'tc/pr-{pr}', '--state', 'all',
                  '--json', 'number,state,isDraft,title,body,changedFiles,files,url')
    if rc != 0:
        return None
    return json.loads(out or '[]')


def split_diff(text):
    """diff 를 파일별 [(path, lines)] 로"""
    files, cur, buf = [], None, []
    for ln in text.splitlines():
        m = re.match(r'^diff --git a/(\S+) b/(\S+)', ln)
        if m:
            if cur:
                files.append((cur, buf))
            cur, buf = m.group(2), [ln]
        elif cur:
            buf.append(ln)
    if cur:
        files.append((cur, buf))
    return files


def checks(p, repo):
    """기계로 볼 수 있는 점검 — [(수준, 문장)]"""
    out = []
    files = [f['path'] for f in p.get('files') or []]
    if p['changedFiles'] == 0:
        out.append(('정보', '바뀐 파일 0 — 리뷰 대상 아님(머지 때 사용자가 닫는다)'))
        return out
    if BOT_TITLE.search(p['title'] or ''):
        out.append(('지적 후보', '제목이 봇 기본값(`Draft: TC changes for PR …`) — 무엇을 더하거나 바꾸는지로(TC-PR-본문.md)'))
    body = (p.get('body') or '').strip()
    first = body.splitlines()[0] if body else ''
    if not JIRA_LINE.search(first):
        out.append(('지적 후보', '본문 첫 줄이 JIRA 링크가 아니다(머지메시지·TC-PR-본문 규약)'))
    if 'TC Test Environment' in body or not body:
        out.append(('지적 후보', '본문이 비었거나 봇 기본 본문'))
    if p['isDraft']:
        out.append(('정보', '바뀐 파일이 있는데 Draft — 엔진 PR draft 해제 때 함께 ready 여야(pr_ready)'))
    shell = [f for f in files if f.startswith('shell/')]
    if shell:
        out.append(('정보', f'shell TC {len(shell)}개 — private-ex 룰북(shell/AGENTS.md → shell/rules/*.md)과 대조: write_ok 문구·sh 호환·finish·반기 디렉터리·수정 전 NOK/수정 후 OK 표기'))
    ans = [f for f in files if re.search(r'\.answer(_cci|_debug)?$|\.result$', f)]
    cases = [f for f in files if f not in ans]
    if ans and not cases:
        out.append(('정보', f'답안만 {len(ans)}개 바뀜 — 본문이 답안 정렬 5항목(의도·바뀐 줄·엔진 원인·의도를 해치는지·검증)을 갖췄나(TC-PR-본문.md)'))
    if re.search(r'\.result$', ' '.join(files)):
        out.append(('지적 후보', '`.result` 파일이 커밋됐다 — 실행 산출물(H-07)'))
    if any(re.search(r'_temp_diff$|csql\.err$|\.log$', f) for f in files):
        out.append(('지적 후보', '실행 잔재(_temp_diff·csql.err·.log)가 커밋됐다'))
    return out


def collect(pr, run=sh):
    res = {'pr': pr, 'repos': []}
    for repo in TC_REPOS:
        prs = tc_prs(pr, repo, run)
        if prs is None:
            res['repos'].append({'repo': repo, 'error': '조회 실패'})
            continue
        for p in prs:
            item = {'repo': repo, 'number': p['number'], 'state': p['state'], 'isDraft': p['isDraft'], 'title': p['title'],
                    'body': p.get('body') or '', 'changedFiles': p['changedFiles'], 'url': p.get('url'),
                    'files': [(f['path'], f.get('additions', 0), f.get('deletions', 0)) for f in p.get('files') or []],
                    'checks': checks(p, repo), 'diff': []}
            if p['changedFiles'] > 0:
                rc, d = run('gh', 'pr', 'diff', str(p['number']), '-R', repo, timeout=300)
                item['diff'] = split_diff(d) if rc == 0 else []
                if rc != 0:
                    item['checks'].append(('정보', 'diff 조회 실패'))
            res['repos'].append(item)
    return res


def to_md(res):
    md = [f"# TC PR — 엔진 PR #{res['pr']} (tc/pr-{res['pr']})", '',
          '리뷰 단계는 이 TC 변경도 본다(스킬 harness-review 절차 2-2). 지적은 그 TC PR 에 게시한다(`post_review --repo <TC 리포>`).', '']
    total = 0
    reviewable = [r for r in res['repos'] if r.get('changedFiles')]
    md += [f"- 리뷰할 TC PR: {len(reviewable)}개 — " + (', '.join(f"{r['repo'].split('/')[1]}#{r['number']}({r['changedFiles']} files)" for r in reviewable) or '없음'), '']
    for r in res['repos']:
        if r.get('error'):
            md += [f"## {r['repo']} — {r['error']}", '']
            continue
        md += [f"## {r['repo']}#{r['number']} — {r['state']}{' · Draft' if r['isDraft'] else ''} · 파일 {r['changedFiles']}", '',
               f"- 제목: {r['title']}", f"- URL: {r['url']}", '']
        md += ['### 점검(결정론)', ''] + ([f'- [{lv}] {s}' for lv, s in r['checks']] or ['- 이상 없음']) + ['']
        if not r['changedFiles']:
            continue
        md += ['### 본문', '', '```', r['body'].strip()[:6000], '```', '', '### 바뀐 파일', '']
        md += [f'- `{p}` +{a} -{d}' for p, a, d in r['files']] + ['']
        md += ['### diff', '']
        for path, lines in r['diff']:
            if total >= MAX_TOTAL_LINES:
                md += [f'- (잘림: 전체 {MAX_TOTAL_LINES}줄 상한 — `{path}` 이후는 `gh pr diff {r["number"]} -R {r["repo"]}` 로)']
                break
            keep = lines[:MAX_FILE_LINES]
            total += len(keep)
            md += [f'#### `{path}`', '', '```diff'] + keep + (['… (잘림: 파일당 %d줄)' % MAX_FILE_LINES] if len(lines) > MAX_FILE_LINES else []) + ['```', '']
    return '\n'.join(md)


def node(c, a):
    """pipeline 노드 — OUT/tc_pack.md·json, review_request 에 요약을 싣는다."""
    res = collect(c['pr'])
    open(os.path.join(c['out'], 'tc_pack.md'), 'w', encoding='utf-8').write(to_md(res))
    json.dump(res, open(os.path.join(c['out'], 'tc_pack.json'), 'w'), ensure_ascii=False, indent=1)
    rv = [r for r in res['repos'] if r.get('changedFiles')]
    c['tc_pack'] = res
    c['log'](f"tc_pack: 리뷰할 TC PR {len(rv)}개" + (' — ' + ', '.join(f"{r['repo'].split('/')[1]}#{r['number']}({r['changedFiles']})" for r in rv) if rv else ''))
    return res


def summary_md(c):
    """review_request 에 싣는 요약 — 본문·diff 는 tc_pack.md 에 있다."""
    res = c.get('tc_pack')
    if not res:
        return '## TC PR (tc_pack)\n- (미실행)\n'
    rv = [r for r in res['repos'] if r.get('changedFiles')]
    if not rv:
        return '## TC PR (tc_pack)\n- 바뀐 파일이 있는 TC PR 없음 — TC 리뷰 대상 없음\n'
    lines = ['## TC PR (tc_pack) — 함께 리뷰한다', '', f"`{os.path.join(c['out'], 'tc_pack.md')}` 를 읽고, TC 지적은 `file` 을 TC 리포 경로로, `repo` 필드에 TC 리포를 적는다.", '']
    for r in rv:
        lines.append(f"- {r['repo']}#{r['number']} ({r['changedFiles']} files{', Draft' if r['isDraft'] else ''}): " + '; '.join(f'[{lv}] {s}' for lv, s in r['checks'] if lv == '지적 후보') or '기계 점검 이상 없음')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', type=int, required=True)
    ap.add_argument('--out')
    a = ap.parse_args(argv)
    out = a.out
    if not out:
        rc, head = sh('gh', 'pr', 'view', str(a.pr), '-R', 'CUBRID/cubrid', '--json', 'headRefOid', '-q', '.headRefOid')
        out = os.path.join(os.path.expanduser('~/dev/utils/harness-out'), str(a.pr), head.strip()[:9])
    os.makedirs(out, exist_ok=True)
    res = collect(a.pr)
    p = os.path.join(out, 'tc_pack.md')
    open(p, 'w', encoding='utf-8').write(to_md(res))
    json.dump(res, open(os.path.join(out, 'tc_pack.json'), 'w'), ensure_ascii=False, indent=1)
    rv = [r for r in res['repos'] if r.get('changedFiles')]
    print(f"tc_pack: 리뷰할 TC PR {len(rv)}개 -> {p}")


if __name__ == '__main__':
    main()
