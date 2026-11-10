#!/usr/bin/env python3
"""tlc_run — TLA+ 명세 하나를 SANY(문법·의미)와 TLC(모델 검사)로 돌리고 판정을 한 줄로 낸다.

harness-review 하위 스킬 skills/harness-review/tla.md 의 실행 도구다(사용자 지시 2026-11-10
"TLA 검증 단계를 추가하고 싶어 … harness review 시의 세미 스킬로").

사용:
  python3 tools/tla/tlc_run.py <spec.tla> [--config <x.cfg>] [--out <dir>] [--expect ok|violation]
                               [--workers N] [--timeout SEC] [--heap SIZE] [--no-deadlock] [--pcal]
  예) python3 tools/tla/tlc_run.py tla/query/scan_minmax_turn/ScanMinMaxTurn.tla \\
          --config tla/query/scan_minmax_turn/before.cfg --expect violation

판정(verdict):
  ok         cfg 의 불변식·성질이 상수 범위 안의 모든 상태에서 성립
  violation  불변식·성질 위반 또는 교착 — 반례 trace 를 trace.txt 에
  error      문법·설정·평가 오류, 도구를 못 찾음
  timeout    --timeout 안에 끝나지 않음 — cfg 의 상수 범위를 줄인다
종료 코드: ok 0 · violation 1 · error 2 · timeout 3. --expect 를 주면 ok·violation 은 기대와 같을 때 0, 다르면 1
  (error·timeout 은 그대로 2·3 — 도구 문제를 '기대와 다름' 과 섞지 않는다).
산출물(--out, 기본 <spec 디렉터리>/out/<cfg 이름>/): result.json(판정·위반한 성질·상태 수·깊이·시간·TLC 판·jar sha256),
  tlc.log(SANY·TLC 출력 전문), trace.txt(반례가 있을 때).
부작용: --out 아래에만 쓴다(TLC 상태 디렉터리도 그 아래에 두고 끝나면 지운다). --pcal 은 명세 파일의 번역 블록을 고쳐 쓴다
  (PlusCal 번역기 pcal.trans 의 동작). 네트워크를 쓰지 않는다.
필요한 것: tla2tools.jar — --jar, 환경변수 TLA2TOOLS_JAR, 기본 ~/tools/tla/tla2tools.jar 순서로 찾는다.
  Java 11 이상(TLA+ 1.7 의 요구) — --java, 환경변수 JAVA, $JAVA_HOME/bin/java, /usr/lib/jvm/java-1[1-9]*·java-2*, PATH 의 java 순서.
  설치는 사람이 한다(외부 코드라 에이전트가 받지 않는다 — 2026-11-10 자동 모드가 막음):
    mkdir -p ~/tools/tla && curl -sSL -o ~/tools/tla/tla2tools.jar \\
      https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar
한계: TLC 는 cfg 의 상수 범위 안에서만 전부 탐색한다. 그 범위 밖과 명세가 빠뜨린 코드 동작은 보장하지 않는다
  (skills/harness-review/tla.md 「무엇을 보장하나」).
"""
import argparse
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

DEFAULT_JAR = os.path.expanduser('~/tools/tla/tla2tools.jar')
# 리뷰용 모델은 작다(수만~수백만 상태). 공유 호스트에서 다른 측정을 흔들지 않게 워커 2개로 시작한다.
DEFAULT_WORKERS = 2
# 리뷰 단계 한 번에 쓰는 시간 상한. 넘으면 범위를 줄이라는 뜻이다(규칙: 큰 모델은 따로 돌리고 리뷰에는 결과만).
DEFAULT_TIMEOUT = 600
# 컨테이너 메모리 한도 64GiB(메모리 rules/메모리-과다-테스트-금지). JVM 기본 최대 힙은 호스트 메모리의 1/4(188G → 47G)이라 묶는다.
DEFAULT_HEAP = '8g'
INSTALL_HINT = ('mkdir -p ~/tools/tla && curl -sSL -o ~/tools/tla/tla2tools.jar '
                'https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar')


def java_major(java):
    try:
        p = subprocess.run([java, '-version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r'version "(\d+)(?:\.(\d+))?', p.stdout)
    if not m:
        return None
    major = int(m.group(1))
    return int(m.group(2) or 0) if major == 1 else major   # "1.8.0_412" 는 8


def find_java(arg):
    cands = [arg, os.environ.get('JAVA')]
    if os.environ.get('JAVA_HOME'):
        cands.append(os.path.join(os.environ['JAVA_HOME'], 'bin', 'java'))
    cands += sorted(glob.glob('/usr/lib/jvm/java-1[1-9]*/bin/java') + glob.glob('/usr/lib/jvm/java-2*/bin/java'), reverse=True)
    cands.append(shutil.which('java'))
    for c in cands:
        if c and os.path.exists(c) and (java_major(c) or 0) >= 11:
            return c
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def num(s):
    return int(s.replace(',', ''))


def parse_tlc(log):
    """TLC 출력에서 판정·성질·상태 수·깊이·반례를 뽑는다."""
    r = {'verdict': 'error', 'violated': None, 'states_generated': None, 'distinct_states': None, 'depth': None,
         'tlc_version': None, 'message': None}
    m = re.search(r'^(TLC2 Version .*)$', log, re.M)
    if m:
        r['tlc_version'] = m.group(1).strip()
    m = re.search(r'([\d,]+) states generated, ([\d,]+) distinct states found', log)
    if m:
        r['states_generated'], r['distinct_states'] = num(m.group(1)), num(m.group(2))
    m = re.search(r'The depth of the complete state graph search is (\d+)', log)
    if m:
        r['depth'] = int(m.group(1))
    if 'Model checking completed. No error has been found.' in log:
        r['verdict'] = 'ok'
        return r, None
    m = re.search(r'^Error: Invariant (\S+) is violated', log, re.M)
    if m:
        r['verdict'], r['violated'] = 'violation', m.group(1)
    elif re.search(r'^Error: Deadlock reached', log, re.M):
        r['verdict'], r['violated'] = 'violation', 'deadlock'
    elif re.search(r'^Error: Temporal properties were violated', log, re.M):
        r['verdict'], r['violated'] = 'violation', 'temporal property'
    elif re.search(r'^Error: Action property (\S+) is violated', log, re.M):
        r['verdict'], r['violated'] = 'violation', re.search(r'^Error: Action property (\S+)', log, re.M).group(1)
    else:
        m = re.search(r'^Error: (.*)$', log, re.M)
        r['message'] = m.group(1).strip() if m else 'TLC 가 판정 없이 끝났다 — tlc.log 를 본다'
    trace = None
    m = re.search(r'^Error: The behavior up to this point is:\s*$(.*?)^(?:[\d,]+ states generated|Finished in)',
                  log, re.M | re.S)
    if m:
        trace = m.group(1).strip() + '\n'
    return r, trace


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('spec')
    ap.add_argument('--config', help='TLC 설정(.cfg). 기본: 명세와 같은 이름의 .cfg')
    ap.add_argument('--out', help='산출 디렉터리. 기본: <명세 디렉터리>/out/<cfg 이름>')
    ap.add_argument('--expect', choices=('ok', 'violation'), help='기대 판정 — 같으면 종료 코드 0')
    ap.add_argument('--workers', type=int, default=DEFAULT_WORKERS)
    ap.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT, help='초')
    ap.add_argument('--heap', default=DEFAULT_HEAP, help='JVM 최대 힙(-Xmx)')
    ap.add_argument('--no-deadlock', action='store_true', help='교착 검사를 끈다(끝나는 상태에 Done 이 없는 명세)')
    ap.add_argument('--pcal', action='store_true', help='먼저 PlusCal 을 번역한다(명세 파일을 고쳐 쓴다)')
    ap.add_argument('--jar', default=os.environ.get('TLA2TOOLS_JAR', DEFAULT_JAR))
    ap.add_argument('--java')
    a = ap.parse_args(argv)

    spec = os.path.abspath(a.spec)
    if not spec.endswith('.tla') or not os.path.isfile(spec):
        print('error: 명세 파일이 없다(.tla 경로를 준다): %s' % a.spec)
        return 2
    sdir, module = os.path.dirname(spec), os.path.basename(spec)[:-4]
    cfg = os.path.abspath(a.config) if a.config else os.path.join(sdir, module + '.cfg')
    if not os.path.isfile(cfg):
        print('error: 설정 파일이 없다: %s — --config 로 준다' % cfg)
        return 2
    out = os.path.abspath(a.out) if a.out else os.path.join(sdir, 'out', os.path.basename(cfg)[:-4])
    os.makedirs(out, exist_ok=True)
    label = '%s/%s' % (module, os.path.basename(cfg))
    result = {'spec': spec, 'config': cfg, 'out': out, 'expect': a.expect}

    def finish(verdict, rc_plain, extra=None, log=''):
        result.update(extra or {})
        result['verdict'] = verdict
        with open(os.path.join(out, 'tlc.log'), 'w') as f:
            f.write(log)
        with open(os.path.join(out, 'result.json'), 'w') as f:
            json.dump(result, f, ensure_ascii=False, indent=1)
        if a.expect and verdict in ('ok', 'violation'):
            match = (verdict == a.expect)
            print('%s  %s  (expect %s, got %s)  %s' % ('AS EXPECTED' if match else 'UNEXPECTED', label, a.expect,
                                                       verdict, summary_line()))
            return 0 if match else 1
        print('%s  %s  %s' % (verdict.upper(), label, summary_line()))
        return rc_plain

    def summary_line():
        parts = []
        if result.get('violated'):
            parts.append('violated=%s' % result['violated'])
        if result.get('distinct_states') is not None:
            parts.append('states %s distinct %s' % (format(result['states_generated'], ','), format(result['distinct_states'], ',')))
        if result.get('depth') is not None:
            parts.append('depth %d' % result['depth'])
        if result.get('trace_states'):
            parts.append('trace %d states → %s' % (result['trace_states'], os.path.join(out, 'trace.txt')))
        if result.get('message'):
            parts.append(result['message'])
        if result.get('seconds') is not None:
            parts.append('%.1fs' % result['seconds'])
        return ' · '.join(parts)

    if not os.path.isfile(a.jar):
        return finish('error', 2, {'message': 'tla2tools.jar 가 없다(%s). 사람이 설치한다: %s' % (a.jar, INSTALL_HINT)})
    java = find_java(a.java)
    if not java:
        return finish('error', 2, {'message': 'Java 11 이상을 못 찾았다 — --java 또는 JAVA=/usr/lib/jvm/java-11/bin/java'})
    result.update({'jar': a.jar, 'jar_sha256': sha256(a.jar), 'java': java})
    base = [java, '-Xmx' + a.heap, '-XX:+UseParallelGC', '-cp', a.jar]
    log = ''
    t0 = time.time()
    try:
        if a.pcal:
            p = subprocess.run(base + ['pcal.trans', '-nocfg', spec], cwd=sdir, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, universal_newlines=True, timeout=a.timeout)
            log += '=== pcal.trans\n' + p.stdout
            if p.returncode != 0:
                return finish('error', 2, {'message': 'PlusCal 번역 실패 — tlc.log 의 pcal.trans 절', 'seconds': time.time() - t0}, log)
        p = subprocess.run(base + ['tla2sany.SANY', spec], cwd=sdir, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True, timeout=a.timeout)
        log += '=== SANY\n' + p.stdout
        if p.returncode != 0 or re.search(r'\*\*\* Errors:|Semantic errors|Parse Error|Fatal errors', p.stdout):
            first = next((l.strip() for l in p.stdout.splitlines() if re.search(r'error|Error|line \d+', l)), 'SANY 실패')
            return finish('error', 2, {'message': 'SANY: ' + first + ' — tlc.log 의 SANY 절', 'seconds': time.time() - t0}, log)
        states = os.path.join(out, 'states')
        cmd = base + ['tlc2.TLC', '-workers', str(a.workers), '-config', cfg, '-metadir', states,
                      '-noGenerateSpecTE'] + (['-deadlock'] if a.no_deadlock else []) + [spec]
        p = subprocess.run(cmd, cwd=sdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=max(1, a.timeout - int(time.time() - t0)))
        log += '=== TLC (%s)\n' % ' '.join(cmd[len(base):]) + p.stdout
        shutil.rmtree(states, ignore_errors=True)
    except subprocess.TimeoutExpired as e:
        shutil.rmtree(os.path.join(out, 'states'), ignore_errors=True)
        log += '\n=== TIMEOUT\n' + (e.output or '' if isinstance(e.output, str) else '')
        return finish('timeout', 3, {'message': '%ds 안에 끝나지 않음 — cfg 의 상수 범위를 줄인다' % a.timeout,
                                     'seconds': time.time() - t0}, log)
    r, trace = parse_tlc(p.stdout)
    r['seconds'] = time.time() - t0
    if trace:
        with open(os.path.join(out, 'trace.txt'), 'w') as f:
            f.write(trace)
        r['trace_states'] = len(re.findall(r'^State \d+', trace, re.M))
    v = r.pop('verdict')
    return finish(v, {'ok': 0, 'violation': 1}.get(v, 2), r, log)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
