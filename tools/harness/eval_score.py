#!/usr/bin/env python3
"""eval_score — 리뷰 하네스 평가셋 채점기(코드 채점). harness/평가.md 의 도구다.

근거: harness/하네스-작성-규칙.md §8 V-01~V-05 (R-26 "Demystifying evals for AI agents").
사용:
  python3 -m tools.harness.eval_score --tasks harness/evals/tasks.jsonl --runs <DIR> [--only id1,id2] [--json out.json]
  <DIR>/<task id>/<trial>/ 아래의 findings*.json(판정기 산출이 있으면 *.adjudicated.json 을 우선) 을 읽는다.
과제(한 줄 JSON, harness/evals/tasks.jsonl):
  id, kind(capability|regression), expect(finding|no_finding), pr, head, source(어디서 왔나), note
  anchor: {"file": "src/…", "line": N, "window": 15}   — 없으면 파일·줄을 보지 않고 낱말만 본다(PR·JIRA 본문 지적)
  keywords: [["정규식", "또는 이것"], ["그리고 이것"]]   — 묶음마다 하나 이상이 claim+why+proposal 에 있어야 한다(AND of OR)
  min_importance: "high"|"medium"|"low"   — no_finding 과제에서 이 이상인 지적만 '찾음' 으로 센다(기본 high)
시도 하나의 판정:
  expect=finding:    앵커 창 안 + 낱말 모두 맞는 지적이 있으면 pass, 앵커만 맞고 낱말이 안 맞으면 unknown(사람이 transcript 를 본다),
                     없으면 fail
  expect=no_finding: 앵커 창 안에 min_importance 이상 지적이 없으면 pass, 있고 낱말까지 맞으면 fail, 앵커만 맞으면 unknown
  findings 파일이 없으면 missing(실행이 안 끝났다 — 점수에 넣지 않는다)
과제의 점수(k = pass·fail 시도 수, unknown 은 사람이 정할 때까지 빼고 따로 센다):
  pass@k = 한 번이라도 pass · pass^k = 모두 pass (R-26: 리뷰는 한 번 돌리므로 pass^k 가 실제 품질에 가깝다)
출력: 과제별 한 줄 + 합계(capability·regression 따로). 종료 코드: regression 과제 중 pass^k 가 아닌 것이 있으면 1, 아니면 0.
부작용: --json 파일 하나만 쓴다.
"""
import argparse
import glob
import json
import os
import re
import sys

RANK = {'low': 1, 'medium': 2, 'high': 3}


def load_findings(trial_dir):
    files = sorted(glob.glob(os.path.join(trial_dir, '*.adjudicated.json'))) or \
        sorted(glob.glob(os.path.join(trial_dir, 'findings*.json')))
    if not files:
        return None
    out = []
    for f in files:
        d = json.load(open(f, encoding='utf-8'))
        out += d.get('findings', []) if isinstance(d, dict) else d
    return out


def in_anchor(f, anchor):
    if not anchor:
        return True
    fp = (f.get('file') or '').lstrip('./')
    if not fp or not (fp.endswith(anchor['file']) or anchor['file'].endswith(fp)):
        return False
    try:
        line = int(str(f.get('line', '')).split('-')[0])
    except ValueError:
        return False
    return abs(line - int(anchor['line'])) <= int(anchor.get('window', 15))


def words_ok(f, groups):
    text = ' '.join(str(f.get(k) or '') for k in ('claim', 'why', 'proposal'))
    return all(any(re.search(w, text, re.I) for w in g) for g in (groups or []))


def judge(task, findings):
    if findings is None:
        return 'missing', None
    anchor, groups = task.get('anchor'), task.get('keywords') or []
    near = [f for f in findings if in_anchor(f, anchor)]
    if task['expect'] == 'no_finding':
        floor = RANK[task.get('min_importance', 'high')]
        near = [f for f in near if RANK.get(f.get('importance') or 'medium', 2) >= floor and f.get('status') != 'dropped']
        if not near:
            return 'pass', None
        hit = next((f for f in near if words_ok(f, groups)), None)
        return ('fail', hit.get('id')) if hit else ('unknown', near[0].get('id'))
    hit = next((f for f in near if words_ok(f, groups)), None)
    if hit:
        return 'pass', hit.get('id')
    return ('unknown', near[0].get('id')) if (near and anchor) else ('fail', None)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tasks', required=True)
    ap.add_argument('--runs', required=True)
    ap.add_argument('--only', default='')
    ap.add_argument('--json')
    a = ap.parse_args(argv)
    tasks = [json.loads(l) for l in open(a.tasks, encoding='utf-8') if l.strip() and not l.startswith('//')]
    only = set(x for x in a.only.split(',') if x)
    rows, bad_regression = [], 0
    for t in tasks:
        if only and t['id'] not in only:
            continue
        tdir = os.path.join(a.runs, t['id'])
        trials = sorted(d for d in glob.glob(os.path.join(tdir, '*')) if os.path.isdir(d))
        res = [(os.path.basename(d),) + judge(t, load_findings(d)) for d in trials]
        scored = [r for r in res if r[1] in ('pass', 'fail')]
        k = len(scored)
        at_k = k > 0 and any(r[1] == 'pass' for r in scored)
        all_k = k > 0 and all(r[1] == 'pass' for r in scored)
        unk = sum(1 for r in res if r[1] == 'unknown')
        rows.append({'id': t['id'], 'kind': t['kind'], 'expect': t['expect'], 'k': k, 'pass@k': at_k, 'pass^k': all_k,
                     'unknown': unk, 'trials': [{'trial': r[0], 'verdict': r[1], 'finding': r[2]} for r in res]})
        if t['kind'] == 'regression' and not all_k:
            bad_regression += 1
        mark = '—' if k == 0 else ('✓' if all_k else ('½' if at_k else '✗'))
        print('%s %-28s %-10s %-10s k=%d pass@k=%s pass^k=%s%s  %s'
              % (mark, t['id'], t['kind'], t['expect'], k, int(at_k), int(all_k),
                 ' unknown=%d' % unk if unk else '', ' '.join('%s:%s' % (r[0], r[1]) for r in res) or '(시도 없음)'))
    for kind in ('capability', 'regression'):
        rs = [r for r in rows if r['kind'] == kind and r['k']]
        if rs:
            print('합계 %-10s 과제 %d · pass@k %d/%d · pass^k %d/%d'
                  % (kind, len(rs), sum(r['pass@k'] for r in rs), len(rs), sum(r['pass^k'] for r in rs), len(rs)))
    if any(r['unknown'] for r in rows):
        print('unknown 은 앵커는 맞고 낱말이 안 맞은 시도다 — transcript·findings 를 읽고 과제의 keywords 를 고치거나 판정을 적는다(V-05)')
    if a.json:
        json.dump(rows, open(a.json, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    return 1 if bad_regression else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
