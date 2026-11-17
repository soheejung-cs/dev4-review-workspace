#!/usr/bin/env python3
"""trace_to_tla — 계측한 실행의 NDJSON trace 를 TLA+ 모듈(TraceData.tla)로 바꾸고, trace 자체의 결함을 먼저 거른다.

skills/harness-review/tla.md §3-T(코드 대조, Specula 방식 trace validation)의 도구다. 계측은 tools/tla/trace/tla_trace.h.

사용:
  python3 tools/tla/trace_to_tla.py <trace 디렉터리|파일…> --out <TraceData.tla> [--events A,B,…] [--module TraceData]
                                    [--max-events 2000] [--allow-sequential]
입력: 한 줄 = {"tag":"trace","ev":..,"tid":..,"t0":..,"t1":..,"st":{..}} (Category A 처럼 시각이 하나면 "t" 하나도 된다 → t0 = t1).
  디렉터리면 그 안의 *.ndjson 전부. 파일 하나에 여러 스레드가 섞여 있어도 된다.
출력 모듈:
  Trace   == << [ev |-> "Inc", tid |-> 1, t0 |-> 1, t1 |-> 2, st |-> [c |-> 1]], … >>   (t0 순으로 정렬)
  Threads == {1, 2, …}
  - tid 는 처음 나온 순서대로 1.. 로 바꾼다.
  - 시각은 **순위**로 바꾼다(모든 t0·t1 을 정렬한 자리) — 크기 비교는 그대로 남고, TLC 의 정수 범위(±2^31)를 넘지 않는다.
    TraceCheck 는 "아직 소비하지 않은 행동 j 중 t1(j) < t0(i) 인 것이 없을 때 i 를 다음으로 소비" 만 쓰므로 순위로 충분하다.
거르는 것(오류 → 종료 2, TraceData 를 쓰지 않는다):
  - tag 가 "trace" 가 아닌 줄, JSON 이 아닌 줄, ev·tid·시각이 없는 줄
  - 한 스레드 안에서 구간이 거꾸로이거나(t0 > t1) 앞 행동과 겹침 — 계측 자리가 틀렸다
  - --events 를 주면 그 밖의 행동 이름(명세 행동과 1:1 이 아님)
  - 시각이 1씩 느는 정수뿐(손으로 쓴 trace 의 흔적 — Specula 규칙). 일부러면 --allow-sequential
경고(종료 0):
  - 스레드 사이에 겹친 구간이 하나도 없음 — 시간 상자 엇갈림을 시험하지 못한 실행이다(동시성 명세면 시나리오를 다시 짠다)
  - --events 중 한 번도 나오지 않은 행동 — 그 행동의 대응은 확인하지 못했다
  - 행동 수가 --max-events 를 넘음 — TLC 가 느려진다. 시나리오를 줄인다
값 변환: 정수 → 정수, true/false → TRUE/FALSE, 문자열 → "…", null → "null", 배열 → <<…>>, 객체 → [k |-> v](키는 TLA+ 이름이어야 한다).
부작용: --out 하나만 쓴다. 네트워크를 쓰지 않는다. python3.6 에서 돈다.
"""
import argparse
import glob
import json
import os
import re
import sys

NAME = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')


def tla_value(v, where):
    if isinstance(v, bool):
        return 'TRUE' if v else 'FALSE'
    if isinstance(v, int):
        if not -(2 ** 31) <= v < 2 ** 31:
            raise ValueError('%s: 정수 %d 가 TLC 범위(±2^31)를 넘는다' % (where, v))
        return str(v)
    if isinstance(v, float):
        raise ValueError('%s: 실수 %r — TLA+ 에 실수가 없다. 정수로 계측한다' % (where, v))
    if v is None:
        return '"null"'
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, list):
        return '<<%s>>' % ', '.join(tla_value(x, where) for x in v)
    if isinstance(v, dict):
        if not v:
            return '[__empty |-> TRUE]'
        for k in v:
            if not NAME.match(k):
                raise ValueError('%s: 키 %r 는 TLA+ 레코드 필드 이름이 될 수 없다' % (where, k))
        return '[%s]' % ', '.join('%s |-> %s' % (k, tla_value(x, where)) for k, x in sorted(v.items()))
    raise ValueError('%s: 바꿀 수 없는 값 %r' % (where, v))


def read_events(paths):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.ndjson'))) if os.path.isdir(p) else [p]
    if not files:
        raise ValueError('trace 파일이 없다: %s' % ' '.join(paths))
    evs = []
    for f in files:
        with open(f) as fh:
            for n, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                where = '%s:%d' % (os.path.basename(f), n)
                try:
                    o = json.loads(line)
                except ValueError as e:
                    raise ValueError('%s: JSON 이 아니다 (%s)' % (where, e))
                if o.get('tag') != 'trace':
                    raise ValueError('%s: "tag":"trace" 가 없다' % where)
                if 't' in o and 't0' not in o:
                    o['t0'] = o['t1'] = o['t']
                for k in ('ev', 'tid', 't0', 't1'):
                    if k not in o:
                        raise ValueError('%s: %s 가 없다' % (where, k))
                o['_where'] = where
                evs.append(o)
    return files, evs


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--module', default='TraceData')
    ap.add_argument('--events', help='명세의 행동 이름(쉼표) — 그 밖의 이름이 나오면 오류')
    ap.add_argument('--max-events', type=int, default=2000)
    ap.add_argument('--allow-sequential', action='store_true')
    a = ap.parse_args(argv)
    try:
        files, evs = read_events(a.inputs)
        if not evs:
            raise ValueError('행동이 하나도 없다')
        allowed = set(x.strip() for x in a.events.split(',')) if a.events else None
        if allowed is not None:
            bad = sorted(set(e['ev'] for e in evs) - allowed)
            if bad:
                raise ValueError('명세에 없는 행동 이름: %s (명세: %s)' % (', '.join(bad), ', '.join(sorted(allowed))))
        tids = {}
        for e in sorted(evs, key=lambda e: (e['t0'], e['t1'])):
            tids.setdefault(e['tid'], len(tids) + 1)
        by_tid = {}
        for e in evs:
            if e['t0'] > e['t1']:
                raise ValueError('%s: t0 > t1' % e['_where'])
            by_tid.setdefault(e['tid'], []).append(e)
        for tid, lst in by_tid.items():
            lst.sort(key=lambda e: e['t0'])
            for p, q in zip(lst, lst[1:]):
                if q['t0'] < p['t1']:
                    raise ValueError('스레드 %s 안에서 구간이 겹친다: %s 와 %s — BEGIN/END 자리를 확인한다'
                                     % (tid, p['_where'], q['_where']))
        stamps = sorted(set([e['t0'] for e in evs] + [e['t1'] for e in evs]))
        if not a.allow_sequential and len(stamps) > 2 and stamps[-1] - stamps[0] == len(stamps) - 1:
            raise ValueError('시각이 1씩 느는 정수뿐이다 — 손으로 쓴 trace 로 보인다(실제 실행으로 만든다, 일부러면 --allow-sequential)')
        rank = {t: i + 1 for i, t in enumerate(stamps)}
        evs.sort(key=lambda e: (e['t0'], e['t1']))
        recs = []
        for e in evs:
            recs.append('[ev |-> %s, tid |-> %d, t0 |-> %d, t1 |-> %d, st |-> %s]'
                        % (json.dumps(e['ev']), tids[e['tid']], rank[e['t0']], rank[e['t1']],
                           tla_value(e.get('st') or {}, e['_where'])))
    except ValueError as err:
        print('error: %s' % err)
        return 2

    warns = []
    overlap = 0
    for i, p in enumerate(evs):
        for q in evs[i + 1:]:
            if q['t0'] >= p['t1']:
                break
            if q['tid'] != p['tid']:
                overlap += 1
    if len(by_tid) > 1 and overlap == 0:
        warns.append('스레드 사이에 겹친 구간이 없다 — 엇갈림을 시험하지 못한 실행이다')
    if allowed is not None:
        unseen = sorted(allowed - set(e['ev'] for e in evs))
        if unseen:
            warns.append('한 번도 나오지 않은 행동: %s — 그 행동의 대응은 확인하지 못했다' % ', '.join(unseen))
    if len(evs) > a.max_events:
        warns.append('행동 %d개 > %d — TLC 가 느려진다, 시나리오를 줄인다' % (len(evs), a.max_events))

    with open(a.out, 'w') as f:
        f.write('---- MODULE %s ----\n' % a.module)
        f.write('\\* tools/tla/trace_to_tla.py 가 만든다 — 손으로 고치지 않는다. 입력: %s\n'
                % ', '.join(os.path.basename(x) for x in files))
        f.write('Trace == <<\n  %s\n>>\n' % ',\n  '.join(recs))
        f.write('Threads == {%s}\n' % ', '.join(str(i) for i in sorted(tids.values())))
        f.write('====\n')
    counts = {}
    for e in evs:
        counts[e['ev']] = counts.get(e['ev'], 0) + 1
    print('ok  %s  행동 %d · 스레드 %d · 스레드 사이 겹친 쌍 %d · %s'
          % (a.out, len(evs), len(tids), overlap, ' '.join('%s=%d' % kv for kv in sorted(counts.items()))))
    for w in warns:
        print('⚠ ' + w)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
