"""unit_regress — 쌓아 둔 리뷰용 단위 테스트(unit/<module>/<suite>/suite.json)를, PR 이 그 suite 의 파일을 건드리면
PR head 와 base 에 다시 돌린다. 결과는 OUT/unit_regress.{json,md} — preamble 과 report 에 실린다.

판정(케이스의 Catch2 태그로):
  [contract]  계약. head FAIL & base PASS → **회귀 후보**. head PASS → 유지. 둘 다 FAIL → suite 가 낡았다(갱신 필요).
  [defect]    지난 리뷰의 결함 재현(PASS = 결함 있음). head PASS → 결함 여전함. head FAIL & base PASS → **고쳐졌거나 동작이 바뀌었다**.
              suite.json 의 defects_fixed_in(커밋)이 있으면 이력 케이스 — head FAIL = 정상(이력), head PASS = **재발**.
  태그 없음  contract 로 본다.
suite.json 의 requires(대상 심볼)로 적용 여부를 먼저 본다: head 에 없고 base 에 있으면 '이 PR 이 대상 코드를 지웠다 — 이관 필요',
둘 다 없으면 '적용 안 됨'(출처 PR 이 아직 안 들어온 리비전), base 에만 없으면 신설. 대상이 있는데 head 컴파일 실패 → suite 갱신 필요.

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


def verdict(kind, head_ok, base_ok, fixed_in=''):
    if kind == 'defect':
        if fixed_in:   # 이미 고쳐진 결함의 이력 케이스 — 다시 PASS 하면 재발이다
            return ('regression', f'**고쳐졌던 결함 재발** (fixed in {fixed_in})') if head_ok else ('history', f'고쳐진 결함(이력, {fixed_in})')
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


def has_symbols(repo, rev, syms):
    """suite 의 requires(대상 심볼)가 그 리비전 src/ 에 다 있나. 없으면 그 리비전은 suite 의 대상 코드보다 앞서거나 뒤다."""
    import subprocess
    for s in syms:
        if subprocess.run(['git', '-C', repo, 'grep', '-q', '-w', s, rev, '--', 'src/'], capture_output=True).returncode:
            return False, s
    return True, ''


def run(out, head, base, changed, log=print, repo=US.DEF_REPO, build=US.DEF_BUILD):
    head, base = _sha(repo, head), _sha(repo, base)
    sel = select(changed)
    rows, res = [], []
    for s in sel:
        o = os.path.join(out, 'unit_regress', s['_id'].replace('/', '__'))
        req = s.get('requires', [])
        h_has, h_miss = has_symbols(repo, head, req) if req else (True, '')
        b_has, _ = has_symbols(repo, base, req) if (req and base) else (bool(base), '')
        if not h_has:
            if b_has:
                rows.append(f"| `{s['_id']}` | (실행 안 함) | - | - | **이 PR 이 대상 코드 `{h_miss}` 를 지웠거나 바꿨다 — suite 이관 필요** |")
            else:
                rows.append(f"| `{s['_id']}` | (적용 안 됨) | - | - | 대상 코드 `{h_miss}` 가 head·base 모두에 없다(이 suite 의 출처 PR 이 아직 안 들어온 리비전) |")
            res.append({'suite': s['_id'], 'skipped': True, 'missing': h_miss, 'base_has': b_has}); continue
        revs = [head] + ([base] if (base and b_has) else [])
        try:
            r = US.run_suite(s['_dir'], revs, o, repo=repo, build=build)
        except Exception as e:  # suite 하나의 실패가 리뷰를 막지 않게
            res.append({'suite': s['_id'], 'error': str(e)[:500]}); rows.append(f"| `{s['_id']}` | (실행 오류) | | | {str(e)[:120]} |"); continue
        h = r[0]; b = r[1] if len(r) > 1 else {}
        bmap = {c['name']: c for c in b.get('cases', [])}
        if h.get('status') != 'ran':
            rows.append(f"| `{s['_id']}` | (head {h.get('status')}) | {b.get('status', '-')} | stale | 대상 코드는 있는데 suite 가 컴파일되지 않는다 — 이 PR 이 시그니처·구조를 바꿨을 수 있다, suite 갱신 필요 ({h.get('log', '')}) |")
        if base and not b_has:
            rows.append(f"| `{s['_id']}` | (base) | - | - | base 에 대상 코드가 없다 — 신설 코드라 base 비교 없음 |")
        for c in h.get('cases', []):
            kind = 'defect' if '[defect]' in (c.get('tags') or '') else 'contract'
            bc = bmap.get(c['name'])
            v, txt = verdict(kind, c['ok'], bc['ok'] if bc else None, s.get('defects_fixed_in', ''))
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
