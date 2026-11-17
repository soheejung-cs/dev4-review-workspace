#!/usr/bin/env python3
"""trace_to_tla — 계측한 실행의 NDJSON trace 를 TLA+ 모듈(TraceData.tla)과 Specula 병합 JSON 으로 바꾸고, trace 자체의 결함을 먼저 거른다.

skills/harness-review/tla.md §3-T(코드 대조, Specula 방식 trace validation)의 도구다. 계측은 tools/tla/trace/tla_trace.h.

사용:
  python3 tools/tla/trace_to_tla.py <trace 디렉터리|파일…> --out <TraceData.tla> [--merged <merged.json>]
                                    [--events A,B,…] [--module TraceData] [--max-events 2000] [--allow-sequential]
입력 — 한 줄 = 한 행동. 두 형식을 다 읽는다(2026-11-17 Specula 형식에 맞춤):
  Specula(기본): {"tag":"trace","event":..,"tid":<0부터>,"start":..,"end":..,"state":{..},"args":{..}?,"msg":{..}?}
                 파일 <prefix>-thread-<tid>.ndjson — 줄에 tid 가 없으면 파일 이름에서 읽는다
  옛 우리 형식:  {"tag":"trace","ev":..,"tid":..,"t0":..,"t1":..,"st":{..}} · Category A 처럼 시각 하나면 "t"(→ start = end)
  디렉터리면 그 안의 *.ndjson 전부.
출력:
  --out TraceData.tla — tools/tla/trace/selftest/TraceCheck.tla(Specula Trace.tla 와 같은 구조)가 EXTENDS 한다:
      Threads == 0..N-1
      Traces  == << <<thread 0 의 행동…>>, <<thread 1 의 행동…>> >>      (TraceOf(t) == Traces[t+1])
      행동 레코드 [event |-> "Inc", tid |-> 0, start |-> 3, end |-> 4, state |-> [...], args |-> [...]]
  --merged merged.json — Specula 전처리기 출력과 같은 꼴: {"num_threads":N,"max_timestamp":M,"traces":{"0":[…],"1":[…]}}
      (Specula Trace.tla 의 JsonDeserialize(TraceFile)·trace_debugger 에 그대로 넘길 수 있게 — 그쪽 실행은 Specula 설치 뒤 확인)
  - tid 는 처음 나온 순서대로 0.. 로 다시 매긴다(옛 형식의 OS 스레드 번호도).
  - 시각은 **순위**(모든 start·end 를 정렬한 1부터의 자리)로 바꾼다 — 크기 비교·구간 겹침은 그대로 남고 TLC 정수 범위를 넘지 않는다
    (Specula 전처리기와 같다).
거르는 것(오류 → 종료 2, 아무것도 쓰지 않는다):
  - tag 가 "trace" 가 아닌 줄, JSON 이 아닌 줄, event·시각이 없는 줄
  - 한 스레드 안에서 구간이 거꾸로이거나(start > end) 앞 행동과 겹침 — 계측 자리가 틀렸다
  - --events 를 주면 그 밖의 행동 이름(명세 MatchEvent 와 1:1 이 아님)
  - 시각이 1씩 느는 정수뿐(손으로 쓴 trace 의 흔적 — Specula 규칙). 일부러면 --allow-sequential
경고(종료 0):
  - 스레드 사이에 겹친 구간이 하나도 없음 — 시간 상자 엇갈림을 시험하지 못한 실행(Specula "concurrency quality" 검사)
  - --events 중 한 번도 나오지 않은 행동 — 그 행동의 대응은 확인하지 못했다(Specula "event coverage")
  - 행동 수가 --max-events 를 넘음 — TLC 가 느려진다
값 변환(TLA+): 정수 → 정수, true/false → TRUE/FALSE, 문자열 → "…", null → "null", 배열 → <<…>>, 객체 → [k |-> v](키는 TLA+ 이름).
부작용: --out·--merged 파일만 쓴다. 네트워크를 쓰지 않는다. python3.6 에서 돈다.
"""
import argparse
import glob
import json
import os
import re
import sys

NAME = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')
THREAD_FILE = re.compile(r'-thread-(\d+)\.ndjson$')


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


def normalize(o, where, file_tid):
    """두 형식을 Specula 필드 이름으로 맞춘다."""
    if o.get('tag') != 'trace':
        raise ValueError('%s: "tag":"trace" 가 없다' % where)
    e = {'event': o.get('event', o.get('ev')), 'state': o.get('state', o.get('st')) or {}}
    if 'start' in o or 'end' in o:
        e['start'], e['end'] = o.get('start'), o.get('end')
    elif 't0' in o or 't1' in o:
        e['start'], e['end'] = o.get('t0'), o.get('t1')
    elif 't' in o:
        e['start'] = e['end'] = o['t']
    else:
        e['start'] = e['end'] = None
    e['tid'] = o.get('tid', file_tid)
    for k in ('args', 'msg'):
        if k in o:
            e[k] = o[k]
    for k in ('event', 'start', 'end', 'tid'):
        if e[k] is None:
            raise ValueError('%s: %s 가 없다' % (where, k))
    e['_where'] = where
    return e


def read_events(paths):
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.ndjson'))) if os.path.isdir(p) else [p]
    if not files:
        raise ValueError('trace 파일이 없다: %s' % ' '.join(paths))
    evs = []
    for f in files:
        m = THREAD_FILE.search(f)
        file_tid = int(m.group(1)) if m else None
        with open(f) as fh:
            for n, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                where = '%s:%d' % (os.path.basename(f), n)
                try:
                    o = json.loads(line)
                except ValueError as err:
                    raise ValueError('%s: JSON 이 아니다 (%s)' % (where, err))
                evs.append(normalize(o, where, file_tid))
    return files, evs


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--merged')
    ap.add_argument('--module', default='TraceData')
    ap.add_argument('--events', help='명세 MatchEvent 의 행동 이름(쉼표) — 그 밖의 이름이 나오면 오류')
    ap.add_argument('--max-events', type=int, default=2000)
    ap.add_argument('--allow-sequential', action='store_true')
    a = ap.parse_args(argv)
    try:
        files, evs = read_events(a.inputs)
        if not evs:
            raise ValueError('행동이 하나도 없다')
        allowed = set(x.strip() for x in a.events.split(',')) if a.events else None
        if allowed is not None:
            bad = sorted(set(e['event'] for e in evs) - allowed)
            if bad:
                raise ValueError('명세에 없는 행동 이름: %s (명세: %s)' % (', '.join(bad), ', '.join(sorted(allowed))))
        by_tid = {}
        for e in evs:
            if e['start'] > e['end']:
                raise ValueError('%s: start > end' % e['_where'])
            by_tid.setdefault(e['tid'], []).append(e)
        for tid, lst in by_tid.items():
            lst.sort(key=lambda e: e['start'])
            for p, q in zip(lst, lst[1:]):
                if q['start'] < p['end']:
                    raise ValueError('스레드 %s 안에서 구간이 겹친다: %s 와 %s — BEGIN/END 자리를 확인한다'
                                     % (tid, p['_where'], q['_where']))
        stamps = sorted(set([e['start'] for e in evs] + [e['end'] for e in evs]))
        if not a.allow_sequential and len(stamps) > 2 and stamps[-1] - stamps[0] == len(stamps) - 1:
            raise ValueError('시각이 1씩 느는 정수뿐이다 — 손으로 쓴 trace 로 보인다(실제 실행으로 만든다, 일부러면 --allow-sequential)')
        rank = {t: i + 1 for i, t in enumerate(stamps)}
        order = sorted(by_tid, key=lambda t: min(e['start'] for e in by_tid[t]))
        newtid = {t: i for i, t in enumerate(order)}
        threads = []
        for t in order:
            seq = []
            for e in by_tid[t]:
                r = {'event': e['event'], 'tid': newtid[t], 'start': rank[e['start']], 'end': rank[e['end']],
                     'state': e['state']}
                for k in ('args', 'msg'):
                    if k in e:
                        r[k] = e[k]
                r['_where'] = e['_where']
                seq.append(r)
            threads.append(seq)
        tla_threads = []
        for seq in threads:
            recs = []
            for r in seq:
                fields = ['event |-> %s' % json.dumps(r['event']), 'tid |-> %d' % r['tid'],
                          'start |-> %d' % r['start'], 'end |-> %d' % r['end'],
                          'state |-> %s' % tla_value(r['state'], r['_where'])]
                for k in ('args', 'msg'):
                    if k in r:
                        fields.append('%s |-> %s' % (k, tla_value(r[k], r['_where'])))
                recs.append('[%s]' % ', '.join(fields))
            tla_threads.append('<<\n    %s\n  >>' % ',\n    '.join(recs))
    except ValueError as err:
        print('error: %s' % err)
        return 2

    warns = []
    flat = sorted((r for seq in threads for r in seq), key=lambda r: (r['start'], r['end']))
    overlap = 0
    for i, p in enumerate(flat):
        for q in flat[i + 1:]:
            if q['start'] >= p['end']:
                break
            if q['tid'] != p['tid']:
                overlap += 1
    if len(threads) > 1 and overlap == 0:
        warns.append('스레드 사이에 겹친 구간이 없다 — 엇갈림을 시험하지 못한 실행이다')
    if allowed is not None:
        unseen = sorted(allowed - set(r['event'] for r in flat))
        if unseen:
            warns.append('한 번도 나오지 않은 행동: %s — 그 행동의 대응은 확인하지 못했다' % ', '.join(unseen))
    if len(flat) > a.max_events:
        warns.append('행동 %d개 > %d — TLC 가 느려진다, 시나리오를 줄인다' % (len(flat), a.max_events))

    with open(a.out, 'w') as f:
        f.write('---- MODULE %s ----\n' % a.module)
        f.write('EXTENDS Naturals, Sequences\n')
        f.write('\\* tools/tla/trace_to_tla.py 가 만든다 — 손으로 고치지 않는다. 입력: %s\n'
                % ', '.join(os.path.basename(x) for x in files))
        f.write('\\* 구조는 Specula 병합 JSON(traces."<tid>") 과 같다 — 스레드 t 의 행동열은 Traces[t+1]\n')
        f.write('Threads == 0..%d\n' % (len(threads) - 1))
        f.write('Traces == <<\n  %s\n>>\n' % ',\n  '.join(tla_threads))
        f.write('====\n')
    if a.merged:
        merged = {'num_threads': len(threads), 'max_timestamp': len(stamps),
                  'traces': {str(i): [{k: v for k, v in r.items() if k != '_where'} for r in seq]
                             for i, seq in enumerate(threads)}}
        with open(a.merged, 'w') as f:
            json.dump(merged, f, ensure_ascii=False, indent=1)
    counts = {}
    for r in flat:
        counts[r['event']] = counts.get(r['event'], 0) + 1
    print('ok  %s%s  행동 %d · 스레드 %d · 스레드 사이 겹친 쌍 %d · %s'
          % (a.out, ' + ' + a.merged if a.merged else '', len(flat), len(threads), overlap,
             ' '.join('%s=%d' % kv for kv in sorted(counts.items()))))
    for w in warns:
        print('⚠ ' + w)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
