#!/usr/bin/env python3
"""cbmc_equiv.py — 성능 변형의 동치 증명(CBMC). 사용자 지시 2026-11-20: "perf → LLM 변형 → CBMC/Alive2 동치 검증" 파이프라인의
3단계를 하네스에 넣는다(CBMC 먼저, 성능 변형일 때만 필수). 절차 정본: harness/동치검증-성능변형.md.

케이스 디렉터리(<case>/):
  case.json   {"file": "src/query/expr_batch.c", "function": "eb_x", "variant_function": "eb_x" (기본 같음),
               "base": "<develop 쪽 rev>", "head": "<변형 rev | WORKTREE>",
               "sources": ["stubs.h", "orig.c", "variant.c", "harness.c"], "unwind": 8, "flags": ["--bounds-check", …]}
  orig.c      원래 함수를 <function>__orig 로 이름만 바꿔 '그대로' 옮긴 것 + 필요한 타입·도우미 stub(stubs.h 로)
  variant.c   변형 함수를 <variant_function>__variant 로 이름만 바꿔 '그대로' 옮긴 것
  harness.c   main: 임의 입력(nondet_*)을 만들어 두 함수에 같이 넣고 결과·출력 버퍼가 같다고 assert
명령:
  run   --case DIR [--repo R] [--cbmc PATH]   본문 대조 → cbmc --unwinding-assertions → proof.json, 실패면 cex.txt(반례)
  check --case DIR [--repo R]                 게이트용: proof.json 이 SUCCESSFUL 이고, 지금 소스의 두 함수·모델 파일 해시가 증명 때와 같은가
본문 대조: base(head)의 <file> 에서 그 함수 정의를 뽑아(이름은 줄 머리, CUBRID 스타일) 주석·공백을 지운 꼴이, 이름을 바꾼 채
orig.c(variant.c) 안에 그대로 있어야 한다 — 모델이 손으로 고친 '원래 함수' 로 증명하는 일을 막는다.
CBMC 는 사람이 설치한다(~/tools/cbmc/bin/cbmc, 명령은 절차 문서 §설치). 읽기·실행만 하고 엔진 소스를 고치지 않는다.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

DEF_REPO = os.path.expanduser('~/dev/sources/cubrid')
DEF_CBMC = os.path.expanduser('~/tools/cbmc/bin/cbmc')
REQUIRED_FLAGS = ('--unwinding-assertions',)


def sh(*a, cwd=None):
    return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, cwd=cwd)


def file_text(repo, rev, path):
    if rev in ('', 'WORKTREE'):
        return open(os.path.join(repo, path), encoding='utf-8', errors='replace').read()
    p = sh('git', 'show', f'{rev}:{path}', cwd=repo)
    if p.returncode != 0:
        raise SystemExit(f'git show {rev}:{path} 실패: {p.stdout.strip()[:200]}')
    return p.stdout


def strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)


def norm(s):
    return re.sub(r'\s+', '', strip_comments(s))


def extract_function(src, name):
    """CUBRID 스타일 정의(이름이 줄 머리, 반환 타입은 앞 줄)의 '이름 (' 부터 짝이 맞는 '}' 까지. 없으면 None."""
    m = re.search(r'^' + re.escape(name) + r'\s*\(', src, re.M)
    if not m:
        return None
    i = src.find('{', m.end())
    semi = src.find(';', m.end())
    if i < 0 or (0 <= semi < i):
        return None  # 선언뿐
    depth = 0
    j = i
    in_str = None
    while j < len(src):
        c = src[j]
        if in_str:
            if c == '\\':
                j += 2
                continue
            if c == in_str:
                in_str = None
        elif src.startswith('/*', j):
            k = src.find('*/', j + 2)
            j = (k + 2) if k >= 0 else len(src)
            continue
        elif src.startswith('//', j):
            k = src.find('\n', j)
            j = k if k >= 0 else len(src)
            continue
        elif c in '"\'':
            in_str = c
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return src[m.start():j + 1]
        j += 1
    return None


def sha(s):
    return hashlib.sha256(s.encode('utf-8', 'replace')).hexdigest()


def load_case(d):
    c = json.load(open(os.path.join(d, 'case.json'), encoding='utf-8'))
    for k in ('file', 'function', 'base', 'head', 'sources'):
        if k not in c:
            raise SystemExit(f'case.json 에 {k} 가 없다')
    c.setdefault('variant_function', c['function'])
    c.setdefault('unwind', 8)
    c.setdefault('flags', ['--bounds-check', '--pointer-check', '--signed-overflow-check'])
    return c


def body_check(case, d, repo):
    """원래·변형 본문이 소스와 같은가 — (ok, msgs, hashes)"""
    msgs = []
    hashes = {}
    for side, rev, fname, model, suffix in (('orig', case['base'], case['function'], 'orig.c', '__orig'),
                                            ('variant', case['head'], case['variant_function'], 'variant.c', '__variant')):
        src = file_text(repo, rev, case['file'])
        fn = extract_function(src, fname)
        if fn is None:
            msgs.append(f'{side}: {rev}:{case["file"]} 에서 {fname} 정의를 못 찾음')
            continue
        hashes[side] = sha(norm(fn))
        mp = os.path.join(d, model)
        if not os.path.exists(mp):
            msgs.append(f'{side}: {model} 없음')
            continue
        renamed = re.sub(r'^' + re.escape(fname) + r'(\s*\()', fname + suffix + r'\1', fn, count=1, flags=re.M)
        if norm(renamed) not in norm(open(mp, encoding='utf-8', errors='replace').read()):
            msgs.append(f'{side}: {model} 의 {fname}{suffix} 본문이 {rev} 의 소스와 다르다(이름만 바꿔 그대로 옮긴다 — 주석·공백은 무시)')
    return not msgs, msgs, hashes


def model_hashes(case, d):
    return {f: sha(open(os.path.join(d, f), encoding='utf-8', errors='replace').read()) for f in case['sources']}


def cmd_run(a):
    d = os.path.abspath(a.case)
    case = load_case(d)
    ok, msgs, hashes = body_check(case, d, a.repo)
    if not ok:
        for m in msgs:
            print('✗ ' + m)
        return 2
    cbmc = a.cbmc
    if not os.path.exists(cbmc):
        print(f'✗ CBMC 가 없다: {cbmc} — 사람이 설치한다(harness/동치검증-성능변형.md §설치)')
        return 3
    flags = list(case['flags'])
    for f in REQUIRED_FLAGS:
        if f not in flags:
            flags.append(f)
    srcs = [s for s in case['sources'] if s.endswith('.c')]
    cmd = [cbmc] + srcs + ['--unwind', str(case['unwind'])] + flags
    p = sh(*cmd, cwd=d)
    out = p.stdout
    open(os.path.join(d, 'cbmc.log'), 'w').write(out)
    if 'VERIFICATION SUCCESSFUL' in out:
        result = 'SUCCESSFUL'
    elif 'VERIFICATION FAILED' in out:
        result = 'FAILED'
    else:
        result = 'ERROR'
    if result == 'FAILED':
        t = sh(*(cmd + ['--trace']), cwd=d)
        open(os.path.join(d, 'cex.txt'), 'w').write(t.stdout)
    ver = sh(cbmc, '--version').stdout.strip()
    proof = {'result': result, 'cbmc': ver, 'cmd': ' '.join(cmd[1:]), 'case': case,
             'function_sha256': hashes, 'model_sha256': model_hashes(case, d),
             'date': datetime.datetime.now().isoformat(timespec='seconds')}
    json.dump(proof, open(os.path.join(d, 'proof.json'), 'w'), ensure_ascii=False, indent=1)
    failed = [l for l in out.splitlines() if l.strip().startswith('[') and 'FAILURE' in l]
    print(f'{"✓" if result == "SUCCESSFUL" else "✗"} {result} — {case["function"]} ({case["base"]}) ≡ {case["variant_function"]} ({case["head"]}), unwind {case["unwind"]}')
    for l in failed[:12]:
        print('  ' + l.strip())
    if result == 'FAILED':
        print(f'  반례: {os.path.join(d, "cex.txt")} — 입력값(nondet 대입)을 변형 단계에 되돌려 고친다(상한은 절차 문서)')
    if result == 'ERROR':
        print(f'  cbmc 가 판정을 내지 못했다(파싱·링크 오류 등): {os.path.join(d, "cbmc.log")}')
    return 0 if result == 'SUCCESSFUL' else 1


def check(case_dir, repo):
    """게이트용 — (ok, msgs)"""
    d = os.path.abspath(case_dir)
    pp = os.path.join(d, 'proof.json')
    if not os.path.exists(pp):
        return False, [f'{d}: proof.json 없음 — cbmc_equiv.py run --case {d}']
    proof = json.load(open(pp, encoding='utf-8'))
    msgs = []
    if proof.get('result') != 'SUCCESSFUL':
        msgs.append(f'{d}: 증명 결과 {proof.get("result")} — 반례(cex.txt)로 변형을 고쳐 다시 run')
    if '--unwinding-assertions' not in proof.get('cmd', ''):
        msgs.append(f'{d}: --unwinding-assertions 없이 증명 — 상한 밖 경로를 놓친다')
    case = load_case(d)
    ok, bmsgs, hashes = body_check(case, d, repo)
    msgs += [f'{d}: {m}' for m in bmsgs]
    if ok and hashes != proof.get('function_sha256'):
        msgs.append(f'{d}: 증명 뒤 원래·변형 함수의 본문이 바뀌었다 — 다시 run')
    if model_hashes(case, d) != proof.get('model_sha256'):
        msgs.append(f'{d}: 증명 뒤 모델 파일(harness·stub)이 바뀌었다 — 다시 run')
    return not msgs, msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd')
    sp.required = True
    r = sp.add_parser('run')
    r.add_argument('--case', required=True)
    r.add_argument('--repo', default=DEF_REPO)
    r.add_argument('--cbmc', default=DEF_CBMC)
    c = sp.add_parser('check')
    c.add_argument('--case', required=True)
    c.add_argument('--repo', default=DEF_REPO)
    a = ap.parse_args()
    if a.cmd == 'run':
        return cmd_run(a)
    ok, msgs = check(a.case, a.repo)
    print('EQUIV OK' if ok else 'EQUIV FAIL')
    for m in msgs:
        print('  ' + m)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
