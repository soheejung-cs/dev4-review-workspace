"""unitseam — 리뷰용 단위 테스트 러너 (gha-ci 용이 아니다. 리뷰 중 지적을 동적으로 확인하는 데 쓴다).

한 번역 단위(TU)를 그 빌드 모드(server|cs|sa)의 실제 컴파일 플래그로 **단독** 컴파일하고 Catch2 v2 테스트와 링크한다.
서버 부팅·버퍼풀·카탈로그 없이 함수 하나를 돌리기 위해 세 가지 이음매(seam)를 둔다.

  seam      지정한 함수 정의를 weak 로 바꾼다(static 이면 푼다). 테스트가 같은 시그니처의 강한 정의(가짜)를 두면 그쪽이 불린다.
  appendix  TU 끝에 붙일 코드. 같은 TU 안이라 static 전역·static 함수에 닿는다(테스트용 훅을 여기 둔다).
  prelude   부팅된 프로세스라면 가졌을 값이 필요한 전역(예: mmon_disabled=true)은 unitseam_prelude.cpp 에 weak 로 둔다.
  patch     (변이) 제안 수정 diff(헤더 포함 아무 파일, repo 기준 -p1)를 그 리비전의 전용 worktree 에 git apply 해 돌린다 — [defect] 가 FAIL,
            [contract] 가 PASS 면 테스트가 그 조건을 정확히 가리킨다. 전용 worktree 는 실행 뒤 지운다.
  stub      링크 때 해석되지 않는 외부 심볼은 자동으로 weak stub 을 만든다. 함수는 호출되면 이름을 찍고 abort
            (UNITSEAM_STUB=zero 이거나 UNITSEAM_ZERO 목록에 있으면 0 을 돌려준다), 데이터는 0 으로 채운 4 KiB.

같은 테스트를 여러 리비전(--rev 반복: 보통 PR head 와 base)에 돌려 결과를 나란히 남긴다.

  python3 -m tools.harness.unitseam run --rev <head> [--rev <base>] --file src/query/xasl_cache.c --mode server \
      --test t.cpp [--seam f1,f2] [--appendix hook.cpp] [--out DIR] [--build-tree ~/dev/build/build_x86_64_release]

산출(DIR/<rev9>/): tu.o test.o stubs.s unit.bin build.log result.xml result.json, DIR/summary.json
한계: 데이터처럼 쓰인(주소만 취한) 함수 심볼은 0 데이터로 stub 된다 — 호출되면 죽는다. 생성 헤더(문법·메시지)는 빌드 트리 것을 쓰므로
PR 이 그것을 바꾸면 결과가 PR 을 반영하지 않는다(build.log 에 경고).
"""
import argparse, json, os, re, shlex, subprocess, sys, xml.etree.ElementTree as ET

HOME = os.path.expanduser('~')
DEF_REPO = f'{HOME}/dev/sources/cubrid'
DEF_BUILD = f'{HOME}/dev/build/build_x86_64_release'
CATCH_DIR = f'{HOME}/dev/utils/catch2'
WT_ROOT = f'{HOME}/dev/utils/harness-out'
PRELUDE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'unitseam_prelude.cpp')
MODE_DEF = {'server': '-DSERVER_MODE', 'cs': '-DCS_MODE', 'sa': '-DSA_MODE'}


def sh(cmd, cwd=None, check=True, timeout=1800):
    p = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str), capture_output=True, text=True, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f'{cmd if isinstance(cmd, str) else " ".join(cmd)}\n{p.stdout}\n{p.stderr}')
    return p


def compdb(build):
    """빌드 트리의 compile_commands — ninja 1.8 은 규칙 이름을 줘야 하므로 rules.ninja 에서 읽는다. 결과는 빌드 트리에 캐시."""
    cache = os.path.join(build, 'compile_commands.unitseam.json')
    ninja = os.path.join(build, 'build.ninja')
    if os.path.exists(cache) and os.path.getmtime(cache) >= os.path.getmtime(ninja):
        return json.load(open(cache))
    rules = set()
    for rn in ('rules.ninja', 'CMakeFiles/rules.ninja'):
        p = os.path.join(build, rn)
        if os.path.exists(p):
            rules |= {l.split()[1] for l in open(p) if l.startswith('rule ') and re.match(r'rule (C|CXX)_COMPILER', l)}
    out = sh(['ninja', '-t', 'compdb', *sorted(rules)], cwd=build).stdout
    open(cache, 'w').write(out)
    return json.loads(out)


def source_root(build):
    for l in open(os.path.join(build, 'CMakeCache.txt')):
        if l.startswith('CMAKE_HOME_DIRECTORY:'):
            return l.split('=', 1)[1].strip()
    return DEF_REPO


def worktree(repo, rev):
    sha = sh(['git', '-C', repo, 'rev-parse', rev]).stdout.strip()
    path = os.path.join(WT_ROOT, f'wt-unit-{sha[:9]}')
    if not os.path.isdir(path):
        r = sh(['git', '-C', repo, 'worktree', 'add', '--detach', path, sha], check=False)
        if r.returncode:
            raise RuntimeError(r.stderr)
    return sha, path


def flags_for(db, srcroot, rel, mode):
    want = MODE_DEF[mode]
    for e in db:
        if os.path.normpath(e['file']) == os.path.normpath(os.path.join(srcroot, rel)) and want in e['command']:
            toks = shlex.split(e['command'])
            break
    else:
        # 빌드 트리가 모르는 파일(PR 이 새로 만든 .c/.cpp): 같은 디렉터리 → 같은 모드 아무 파일 순으로 플래그를 빌린다
        d = os.path.dirname(os.path.join(srcroot, rel))
        cand = [e for e in db if want in e['command'] and os.path.dirname(e['file']) == d] or \
               [e for e in db if want in e['command'] and e['file'].endswith(('.c', '.cpp'))]
        if not cand:
            raise SystemExit(f'compile command 없음: {rel} ({mode}) — 그 모드로 빌드되지 않는 파일일 수 있다')
        e = cand[0]
        toks = shlex.split(e['command'].replace(e['file'], os.path.join(srcroot, rel)))
        print(f'unitseam: {rel} 는 빌드 트리에 없다 — {os.path.relpath(e["file"], srcroot)} 의 {mode} 플래그를 빌린다', file=sys.stderr)
    cc, out, skip = toks[0], [], False
    for i, t in enumerate(toks[1:]):
        if skip:
            skip = False
            continue
        if t in ('-o', '-MT', '-MF'):
            skip = True
            continue
        if t in ('-c', '-MD') or t.startswith(('-Werror', '-O', '-g')) or os.path.normpath(t) == os.path.normpath(os.path.join(srcroot, rel)):
            continue
        out.append(t)
    return cc, out + ['-O0', '-g', '-fno-inline', '-fno-omit-frame-pointer']


def retarget(flags, srcroot, wt):
    return [f.replace(srcroot, wt) if f.startswith(('-I', '-isystem')) or f.startswith(srcroot) else f for f in flags]


def apply_seams(text, seams):
    lines = text.split('\n')
    done = []
    for name in seams:
        is_var = name.startswith('var:')
        name = name[4:] if is_var else name
        hit = False
        for i, l in enumerate(lines):
            if is_var:
                if re.match(rf'^static\s+[^;(]*\b{re.escape(name)}\b', l):
                    lines[i] = re.sub(r'^static\s+', '', l)
                    hit = True
                continue
            if re.match(rf'^{re.escape(name)}\s*\(', l):
                j = i
                while j < len(lines) and not lines[j].rstrip().endswith((')', ';', '{')):
                    j += 1
                k = j + 1 if lines[j].rstrip().endswith(')') else j
                if k < len(lines) and lines[k].strip() == '{':           # 정의
                    rt = i - 1
                    lines[rt] = '__attribute__ ((weak)) ' + re.sub(r'^static\s+', '', lines[rt])
                    hit = True
            elif re.match(rf'^static\b.*\b{re.escape(name)}\s*\(', l):  # static 원형
                lines[i] = re.sub(r'^static\s+', '', l)
        done.append((name, hit))
    return '\n'.join(lines), done


RT_C = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
long __unitseam_stub (const char *name)
{
  const char *mode = getenv ("UNITSEAM_STUB"), *zero = getenv ("UNITSEAM_ZERO");
  fprintf (stderr, "unitseam: stub called: %s\n", name);
  if ((mode && strcmp (mode, "zero") == 0) || (zero && strstr (zero, name)))
    return 0;
  fprintf (stderr, "unitseam: unexpected call (give the test a fake, or UNITSEAM_ZERO=%s)\n", name);
  abort ();
}
'''


def make_stubs(objs, names, path):
    plt = set()
    for o in objs:
        for l in sh(['objdump', '-r', o]).stdout.splitlines():
            m = re.search(r'R_X86_64_PLT32\s+(\S+?)(?:-0x[0-9a-f]+)?$', l)
            if m:
                plt.add(m.group(1))
    s = ['\t.text', '\t.extern __unitseam_stub']
    data = []
    for n, sym in enumerate(sorted(names)):
        if sym in plt:
            s += [f'\t.weak {sym}', f'\t.type {sym},@function', f'{sym}:',
                  f'\tleaq .Lus{n}(%rip), %rdi', '\tjmp __unitseam_stub@PLT',
                  '\t.section .rodata', f'.Lus{n}:\t.asciz "{sym}"', '\t.text']
        else:
            data.append(sym)
    s.append('\t.bss')
    for sym in data:
        s += ['\t.p2align 6', f'\t.weak {sym}', f'\t.type {sym},@object', f'\t.size {sym},4096', f'{sym}:', '\t.zero 4096']
    s.append('\t.section .note.GNU-stack,"",@progbits')
    open(path, 'w').write('\n'.join(s) + '\n')
    return len(names) - len(data), len(data)


def catch_main(cc):
    o = os.path.join(CATCH_DIR, 'catch_main.o')
    if not os.path.exists(o):
        src = os.path.join(CATCH_DIR, 'catch_main.cpp')
        open(src, 'w').write('#define CATCH_CONFIG_MAIN\n#include "catch.hpp"\n')
        sh([cc, '-std=c++17', '-O1', '-c', src, '-I', CATCH_DIR, '-o', o])
    return o


def mutation_worktree(repo, sha, patch):
    """변이 실행 전용 worktree — 같은 리비전을 따로 꺼내 diff(헤더 포함 아무 파일)를 git apply 한다. 호출자가 지운다."""
    path = os.path.join(WT_ROOT, f'wt-unit-{sha[:9]}-mut-{os.getpid()}')
    sh(['git', '-C', repo, 'worktree', 'add', '--detach', path, sha])
    r = sh(['git', '-C', path, 'apply', '--whitespace=nowarn', os.path.abspath(patch)], check=False)
    if r.returncode:
        sh(['git', '-C', repo, 'worktree', 'remove', '--force', path], check=False)
        raise RuntimeError(f'patch 적용 실패: {r.stderr}')
    return path


def build_and_run(a, rev, db, srcroot):
    sha, wt = worktree(a.repo, rev)
    mut = None
    if getattr(a, 'patch', ''):
        mut = wt = mutation_worktree(a.repo, sha, a.patch)
    try:
        return _build_and_run(a, sha, wt, db, srcroot)
    finally:
        if mut:
            sh(['git', '-C', a.repo, 'worktree', 'remove', '--force', mut], check=False)


def _build_and_run(a, sha, wt, db, srcroot):
    out = os.path.join(a.out, sha[:9])
    os.makedirs(out, exist_ok=True)
    log = []
    cc, flags = flags_for(db, srcroot, a.file, a.mode)
    flags = retarget(flags, srcroot, wt)
    src = os.path.join(wt, a.file)
    if not os.path.exists(src):
        res = {'rev': sha, 'status': 'no-file', 'note': f'{a.file} 가 이 리비전에 없다'}
        json.dump(res, open(os.path.join(out, 'result.json'), 'w'), ensure_ascii=False, indent=1)
        return res
    orig = open(src).read()
    text, seams = apply_seams(orig, [s for s in a.seam.split(',') if s])
    if a.appendix:
        text += '\n/* ---- unitseam appendix ---- */\n' + open(a.appendix).read()
    stem, ext = os.path.splitext(src)
    # 실행마다 다른 이름 — 같은 worktree·같은 파일을 여러 suite 가 동시에 돌려도 사본이 겹치지 않게
    tu = f'{stem}.unitseam.{os.getpid()}{ext}'
    open(tu, 'w').write(text)
    log.append(f'seams: {seams}')
    miss = [n for n, h in seams if not h]
    if miss:
        log.append(f'WARN seam 정의를 찾지 못함: {miss}')
    gen = sh(['git', '-C', a.repo, 'diff', '--name-only', f'{sha}~1', sha], check=False).stdout
    if re.search(r'\.(y|l|msg)$', gen, re.M):
        log.append('WARN 이 리비전이 생성 원본(.y/.l/.msg)을 바꿨다 — 생성 헤더는 빌드 트리 것이다')
    o_tu, o_test = os.path.join(out, 'tu.o'), os.path.join(out, 'test.o')
    for (inp, obj, extra) in ((tu, o_tu, []), (os.path.abspath(a.test), o_test, ['-I', CATCH_DIR, '-I', os.path.dirname(src)])):
        r = sh([cc, *flags, *extra, '-c', inp, '-o', obj], check=False)
        if inp == tu:
            open(os.path.join(out, 'tu' + ext), 'w').write(text)   # 디버깅용으로 사본 내용은 산출 디렉터리에 남긴다
            os.remove(tu)
        log.append(f'$ compile {os.path.basename(inp)} rc={r.returncode}\n{r.stderr[-4000:]}')
        if r.returncode:
            open(os.path.join(out, 'build.log'), 'w').write('\n'.join(log))
            res = {'rev': sha, 'status': 'compile-error', 'log': os.path.join(out, 'build.log')}
            json.dump(res, open(os.path.join(out, 'result.json'), 'w'), ensure_ascii=False, indent=1)
            return res
    rt_c = os.path.join(out, 'stubs_rt.c')
    open(rt_c, 'w').write(RT_C)
    o_rt = os.path.join(out, 'stubs_rt.o')
    sh(['cc', '-c', '-O1', rt_c, '-o', o_rt])
    o_pre = os.path.join(out, 'prelude.o')
    sh([cc, '-std=c++17', '-c', PRELUDE, '-o', o_pre])
    objs = [o_tu, o_test, catch_main(cc), o_rt, o_pre]
    libs = ['-lpthread', '-ldl', '-lm', '-lrt']
    exe = os.path.join(out, 'unit.bin')
    r = sh([cc, *objs, *libs, '-Wl,--no-demangle', '-o', exe], check=False)
    und = sorted(set(re.findall(r"undefined reference to [`']([^'`]+)'", r.stderr)))
    stubs = os.path.join(out, 'stubs.s')
    nf, nd = make_stubs([o_tu, o_test], und, stubs)
    log.append(f'stubs: functions {nf}, data {nd}')
    o_st = os.path.join(out, 'stubs.o')
    sh(['cc', '-c', stubs, '-o', o_st])
    r = sh([cc, *objs, o_st, *libs, '-o', exe], check=False)
    log.append(f'$ link rc={r.returncode}\n{r.stderr[-4000:]}')
    if r.returncode:
        open(os.path.join(out, 'build.log'), 'w').write('\n'.join(log))
        res = {'rev': sha, 'status': 'link-error', 'log': os.path.join(out, 'build.log')}
        json.dump(res, open(os.path.join(out, 'result.json'), 'w'), ensure_ascii=False, indent=1)
        return res
    xmlp = os.path.join(out, 'result.xml')
    r = sh([exe, '-r', 'xml', '-o', xmlp, *a.catch_args], check=False, timeout=a.timeout)
    log.append(f'$ run rc={r.returncode}\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}')
    open(os.path.join(out, 'build.log'), 'w').write('\n'.join(log))
    cases = []
    try:
        root = ET.parse(xmlp).getroot()
        for tc in root.iter('TestCase'):
            ok = tc.find('OverallResult')
            fails = [(e.get('filename', '').split('/')[-1] + ':' + e.get('line', ''),
                      (e.findtext('Original') or '').strip(), (e.findtext('Expanded') or '').strip())
                     for e in tc.iter('Expression') if e.get('success') == 'false']
            fails += [(e.get('filename', '').split('/')[-1] + ':' + e.get('line', ''), 'FAIL()', (e.text or '').strip())
                      for e in tc.iter('Failure')]
            fails += [(e.get('filename', '').split('/')[-1] + ':' + e.get('line', ''), 'exception', (e.text or '').strip())
                      for e in tc.iter('Exception')]
            cases.append({'name': tc.get('name'), 'tags': tc.get('tags', ''), 'ok': ok is not None and ok.get('success') == 'true', 'failures': fails})
    except Exception as e:
        cases.append({'name': '(xml unreadable — crashed?)', 'ok': False, 'failures': [[str(e), r.stderr[-500:], '']]})
    res = {'rev': sha, 'status': 'ran', 'rc': r.returncode, 'cases': cases,
           'stub_calls': sorted(set(re.findall(r'unitseam: stub called: (\S+)', r.stderr))), 'out': out}
    json.dump(res, open(os.path.join(out, 'result.json'), 'w'), ensure_ascii=False, indent=1)
    return res


def run_suite(suite_dir, revs, out, repo=DEF_REPO, build=DEF_BUILD, timeout=300):
    """unit/<module>/<suite>/suite.json 하나를 revs 에 돌린다(unit_regress 가 부른다)."""
    s = json.load(open(os.path.join(suite_dir, 'suite.json'), encoding='utf-8'))
    a = argparse.Namespace(repo=repo, build_tree=build, file=s['file'], mode=s['mode'],
                           seam=','.join(s.get('seams', [])),
                           appendix=os.path.join(suite_dir, s['appendix']) if s.get('appendix') else '',
                           test=os.path.join(suite_dir, s.get('test', 'test.cpp')), out=out, timeout=timeout, catch_args=[], patch='')
    os.makedirs(out, exist_ok=True)
    db = compdb(build)
    return [build_and_run(a, rev, db, source_root(build)) for rev in revs]


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    r = sp.add_parser('run')
    r.add_argument('--rev', action='append', required=True, help='반복 가능: 보통 PR head 와 base')
    r.add_argument('--file', required=True, help='repo 기준 경로 (src/...)')
    r.add_argument('--mode', choices=list(MODE_DEF), required=True)
    r.add_argument('--test', required=True)
    r.add_argument('--seam', default='', help='weak 로 바꿀 함수, 쉼표 구분. static 변수는 var:이름')
    r.add_argument('--appendix', default='')
    r.add_argument('--patch', default='', help='변이: 제안 수정 unified diff(repo 기준, 헤더 포함). 전용 worktree 에 적용해 [defect] 가 FAIL 하는지 본다')
    r.add_argument('--out', default='')
    r.add_argument('--repo', default=DEF_REPO)
    r.add_argument('--build-tree', default=DEF_BUILD)
    r.add_argument('--timeout', type=int, default=300)
    r.add_argument('catch_args', nargs='*')
    a = ap.parse_args()
    a.out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.test)), 'unitseam-out')
    os.makedirs(a.out, exist_ok=True)
    db = compdb(a.build_tree)
    srcroot = source_root(a.build_tree)
    summary = [build_and_run(a, rev, db, srcroot) for rev in a.rev]
    json.dump(summary, open(os.path.join(a.out, 'summary.json'), 'w'), ensure_ascii=False, indent=1)
    for s in summary:
        print(f"== {s['rev'][:9]} {s['status']}" + (f" rc={s.get('rc')}" if 'rc' in s else ''))
        for c in s.get('cases', []):
            print(f"   {'PASS' if c['ok'] else 'FAIL'}  {c['name']}")
            for f in c['failures'][:3]:
                print(f'          {f[0]}  {f[1]}  =>  {f[2]}')
        if s.get('stub_calls'):
            print(f"   stub calls: {', '.join(s['stub_calls'][:12])}")
        if s['status'] != 'ran':
            print(f"   see {s.get('log', '')} {s.get('note', '')}")
    print(os.path.join(a.out, 'summary.json'))


if __name__ == '__main__':
    main()
