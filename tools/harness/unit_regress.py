"""unit_regress — 쌓아 둔 리뷰용 단위 테스트(unit/<module>/<suite>/suite.json)를, PR 이 그 suite 의 파일을 건드리면
PR head 와 base 에 다시 돌린다. 결과는 OUT/unit_regress.{json,md} — preamble 과 report 에 실린다.

판정(케이스의 Catch2 태그로):
  [contract]  계약. head FAIL & base PASS → **회귀 후보**. head PASS → 유지. 둘 다 FAIL → suite 가 낡았다(갱신 필요).
  [defect]    지난 리뷰의 결함 재현(PASS = 결함 있음). head PASS → 결함 여전함. head FAIL & base PASS → **고쳐졌거나 동작이 바뀌었다**.
  태그 없음  contract 로 본다.
head 컴파일·링크 실패 → 이 PR 이 시그니처·구조를 바꿨다 — suite 갱신 필요(지적 아님, 확인 대상).

  python3 -m tools.harness.unit_regress --head <rev> --base <rev> --files a.c,b.cpp --out DIR   (단독 실행)
환경 HARNESS_NO_UNIT=1 이면 파이프라인에서 건너뛴다.
"""
import argparse, glob, json, os
from . import unitseam as US

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UNIT = os.path.join(ROOT, 'unit')


def suites():
    for p in sorted(glob.glob(os.path.join(UNIT, '**', 'suite.json'), recursive=True)):
        s = json.load(open(p, encoding='utf-8'))
        s['_dir'] = os.path.dirname(p)
        s['_id'] = os.path.relpath(s['_dir'], UNIT)
        yield s


def select(changed):
    ch = set(changed)
    return [s for s in suites() if ch & set([s['file']] + s.get('watch', []))]


def verdict(kind, head_ok, base_ok):
    if kind == 'defect':
        if head_ok:
            return 'defect-present', '결함 재현됨(여전함)'
        return ('defect-gone', '**재현 안 됨 — 고쳐졌거나 동작이 바뀜**') if base_ok else ('defect-gone?', 'head·base 모두 재현 안 됨 — suite 확인')
    if head_ok:
        return 'ok', '유지'
    if base_ok:
        return 'regression', '**회귀 후보** (base PASS → head FAIL)'
    return 'stale', 'head·base 모두 FAIL — suite 갱신 필요'


def _sha(repo, rev):
    """브랜치·태그 이름을 SHA 로 — 보고서 머리줄이 `rev[:9]` 로 이름을 자르지 않게(pr8013-new → pr8013-ne)."""
    if not rev:
        return rev
    r = US.sh(['git', '-C', repo, 'rev-parse', '--verify', f'{rev}^{{commit}}'], check=False)
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else rev


def run(out, head, base, changed, log=print, repo=US.DEF_REPO, build=US.DEF_BUILD):
    head, base = _sha(repo, head), _sha(repo, base)
    sel = select(changed)
    rows, res = [], []
    for s in sel:
        o = os.path.join(out, 'unit_regress', s['_id'].replace('/', '__'))
        try:
            r = US.run_suite(s['_dir'], [head, base] if base else [head], o, repo=repo, build=build)
        except Exception as e:  # suite 하나의 실패가 리뷰를 막지 않게
            res.append({'suite': s['_id'], 'error': str(e)[:500]}); rows.append(f"| `{s['_id']}` | (실행 오류) | | | {str(e)[:120]} |"); continue
        h = r[0]; b = r[1] if len(r) > 1 else {}
        bmap = {c['name']: c for c in b.get('cases', [])}
        if h.get('status') != 'ran':
            rows.append(f"| `{s['_id']}` | (head {h.get('status')}) | {b.get('status', '-')} | stale | 이 PR 이 시그니처·구조를 바꿨다 — suite 갱신 필요 ({h.get('log', '')}) |")
        for c in h.get('cases', []):
            kind = 'defect' if '[defect]' in (c.get('tags') or '') else 'contract'
            bc = bmap.get(c['name'])
            v, txt = verdict(kind, c['ok'], bc['ok'] if bc else None)
            if not bc and b and b.get('status') != 'ran':
                txt += f" · base {b.get('status')}(base 에 없는 코드면 신설)"
            why = ''
            if not c['ok'] and c.get('failures'):
                f0 = c['failures'][0]; why = f" — head: {f0[2][:80]}"
            elif bc and not bc['ok'] and bc.get('failures'):
                f0 = bc['failures'][0]; why = f" — base: {f0[2][:80]}"
            rows.append(f"| `{s['_id']}` | {c['name']} | {'PASS' if c['ok'] else 'FAIL'} | {('PASS' if bc['ok'] else 'FAIL') if bc else '-'} | {txt}{why} |".replace('\n', ' '))
            c['verdict'] = v; c['kind'] = kind
        res.append({'suite': s['_id'], 'origin': s.get('origin'), 'head': h, 'base': b})
    md = ['## 단위 회귀 (unit/ 에 쌓인 리뷰용 suite)', '']
    if not sel:
        md.append('- 이 PR 이 건드린 파일에 걸린 suite 없음')
    else:
        md += [f'이 PR 이 건드린 파일에 걸린 suite {len(sel)}개를 head `{head[:9]}`' + (f' / base `{base[:9]}`' if base else '') + ' 에 돌렸다.', '',
               '| suite | 케이스 | head | base | 판정 |', '|---|---|---|---|---|'] + rows
        md += ['', '`[defect]` 케이스는 지난 리뷰의 결함 재현이다(PASS = 결함 있음). 회귀 후보·재현 안 됨은 리뷰에서 확인한다.']
    json.dump(res, open(os.path.join(out, 'unit_regress.json'), 'w'), ensure_ascii=False, indent=1)
    open(os.path.join(out, 'unit_regress.md'), 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    log(f'unit_regress: suites {len(sel)}, rows {len(rows)}')
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--head'); ap.add_argument('--base', default='')
    ap.add_argument('--files', default='', help='쉼표 구분 repo 경로 (PR 이 바꾼 파일)')
    ap.add_argument('--out')
    ap.add_argument('--list', action='store_true', help='suite 목록만')
    a = ap.parse_args()
    if a.list:
        for s in suites():
            print(s['_id'], s['file'], s['mode'])
        return
    if not (a.head and a.files and a.out):
        ap.error('--head, --files, --out 가 필요하다 (--list 제외)')
    os.makedirs(a.out, exist_ok=True)
    run(a.out, a.head, a.base, [f for f in a.files.split(',') if f])
    print(open(os.path.join(a.out, 'unit_regress.md')).read())


if __name__ == '__main__':
    main()
