#!/usr/bin/env python3
"""structure.json 검사 — 성능 작업의 '구조 먼저'(harness/구조우선-성능작업.md)를 결정론적으로 판정한다.

사용: python3 -m tools.harness.structure_check <structure.json> [--stage plan|gate]
  plan : plan 승인 전 — 측정 뒤 채우는 칸(plan_arrival.verified_by, mechanism_bench.result)은 비어도 된다
  gate : 구현 뒤 — 전부 채워져 있어야 한다
implement_gate 가 plan.perf=true 일 때 gate 단계로 부른다. 실패 사유는 각각 원칙 번호를 단다.
"""
import json, os, sys

STRATEGY_OK_FOR_LONG = {'arena', 'slab', 'pool', 'stack'}

def check(path, stage='gate'):
    errs, notes = [], []
    if not os.path.isfile(path):
        return [f'structure.json 없음: {path} — 성능 작업은 S1.5 구조 먼저(harness/구조우선-성능작업.md)'], notes
    s = json.load(open(path, encoding='utf-8'))
    try:
        import jsonschema
        schema = os.path.join(os.path.dirname(__file__), '..', '..', 'harness', 'schemas', 'structure.json')
        jsonschema.validate(s, json.load(open(schema, encoding='utf-8')))
    except ImportError:
        notes.append('jsonschema 없음 — 스키마 검사 건너뜀(필수 칸은 아래에서 본다)')
    except Exception as e:   # jsonschema.ValidationError
        errs.append(f'스키마: {str(e).splitlines()[0]}')
    base = os.path.dirname(os.path.abspath(path))
    def P(p):
        return p if os.path.isabs(os.path.expanduser(p)) else os.path.join(base, p)
    # 1 시간 지도와 상한
    keys = set()
    for t in s.get('time_map', []):
        tot = sum(t.get('buckets', {}).values()); keys |= set(t.get('buckets', {}))
        if not 90 <= tot <= 110:
            errs.append(f"[1] time_map '{t.get('query')}' 합 {tot:.0f}% — prof_bucket.py 출력을 그대로(90~110%)")
    for b in s.get('target', {}).get('buckets', []):
        if b not in keys:
            errs.append(f"[1] target 축 '{b}' 이 time_map 에 없다 — 줄이려는 축의 비율을 모르면 상한도 모른다")
    if not s.get('target', {}).get('ceiling', '').strip():
        errs.append('[1] target.ceiling 비어 있음 — 축 비율로 질의별 상한(Amdahl)을 적는다')
    # 2 횟수
    if not s.get('cost_census'):
        errs.append('[2] cost_census 비어 있음 — 단위(행·그룹·탐색)당 할당·fix·호출 횟수를 근거와 함께')
    for c in s.get('cost_census', []):
        if not str(c.get('evidence', '')).strip():
            errs.append(f"[2] cost_census {c.get('unit')}/{c.get('kind')} 근거 없음 — file:line 또는 trace 수치")
    # 3 커버리지
    for b in s.get('coverage', {}).get('breaks', []):
        if not str(b.get('decision', '')).strip():
            errs.append(f"[3] 파이프라인 끊김 '{b.get('where')}' 에 decision 없음 — 잇는다/남긴다(이유)")
    # 4 병렬 직렬 구간
    for x in s.get('parallel', {}).get('serial_added', []):
        if not str(x.get('justification', '')).strip() or not str(x.get('measured', '')).strip():
            errs.append(f"[4] 새 직렬 구간 '{x.get('where')}' — 병렬 대안 대비 측정과 사유가 있어야 한다")
    # 5 수명
    for m in s.get('memory', []):
        if m.get('allocs_per_unit', 0) >= 3 and m.get('lifetime') in ('group', 'operator', 'query') \
                and m.get('strategy') not in STRATEGY_OK_FOR_LONG and not str(m.get('justification', '')).strip():
            errs.append(f"[5] '{m.get('object')}' 단위당 할당 {m.get('allocs_per_unit')}회·수명 {m.get('lifetime')} 인데 {m.get('strategy')} — 아레나/slab 이거나 사유")
    # 6 도착
    for a in s.get('plan_arrival', []):
        if not str(a.get('arrives_as', '')).strip():
            errs.append(f"[6] '{a.get('shape')}' 이 옵티마이저 뒤 어떤 모양으로 도착하는지 비어 있음 — ;plan detail 의 Query stmt/term[]")
        if stage == 'gate' and not str(a.get('verified_by', '')).strip():
            errs.append(f"[6] '{a.get('shape')}' 경로를 탔다는 증거(trace) 없음")
    # 7 미시 벤치
    mb = s.get('mechanism_bench', {})
    sh = mb.get('shapes', '')
    if not sh or not os.path.isfile(os.path.expanduser(P(sh))):
        errs.append(f'[7] 미시 벤치 shapes 파일 없음: {sh!r}')
    else:
        n = sum(1 for l in open(os.path.expanduser(P(sh))) if '|' in l)
        if n < len(s.get('plan_arrival', [])):
            errs.append(f"[7] shapes {n}줄 < 기능 모양 {len(s.get('plan_arrival', []))}개 — 모양마다 질의 하나")
    if stage == 'gate' and not str(mb.get('result', '')).strip():
        errs.append('[7] mechanism_bench.result 비어 있음 — mech_summarize.py 출력')
    if stage == 'gate' and not s.get('residual'):
        notes.append('[7] residual 비어 있음 — 남은 큰 축(다음 목표)을 적어 두면 다음 작업의 출발점이 된다')
    # 8 범위
    sc = s.get('scope', {})
    if len(sc.get('risk_types', [])) >= 2 and not str(sc.get('decision', '')).strip():
        errs.append(f"[8] 위험 종류 {sc.get('risk_types')} — 한 PR 인지 나누는지 decision")
    return errs, notes

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    stage = 'plan' if '--stage' in sys.argv and sys.argv[sys.argv.index('--stage') + 1] == 'plan' else 'gate'
    if args and args[-1] in ('plan', 'gate'):
        args = args[:-1]
    errs, notes = check(args[0], stage)
    print(('STRUCTURE OK' if not errs else 'STRUCTURE FAIL') + f' ({stage})')
    for e in errs:
        print('  ' + e)
    for n in notes:
        print('  note: ' + n)
    sys.exit(1 if errs else 0)

if __name__ == '__main__':
    main()
