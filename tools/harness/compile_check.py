#!/usr/bin/env python3
"""컴파일 확인 — 변경 파일의 오브젝트만 굽는다(compile_commands 가 있으면 -fsyntax-only, 없으면 ninja 타깃).
사용:
  python3 -m tools.harness.compile_check --files src/a.c,src/b.cpp          # 지금 워킹트리
  python3 -m tools.harness.compile_check --commits <base>..<head>          # 커밋마다 체크아웃해 그 커밋이 바꾼 파일을 컴파일 (조각 커밋이 홀로 컴파일되는지)
커밋 모드는 워킹트리가 깨끗해야 하고, 끝나면 원래 ref 로 돌아온다. 결과: 커밋/파일별 ok|FAILED, 종료코드 1 = 실패 있음.
2026-09-30: 헝크로 쪼갠 커밋이 홀로 컴파일되지 않아 goto 빌드가 죽은 뒤 추가."""
import argparse, os, subprocess, sys
from . import adjudicate as AD

def sh(*a, cwd=None): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, cwd=cwd)

def check_files(repo, files):
    r = AD.self_check_build(repo, [f for f in files if f.endswith(('.c', '.cpp', '.cc'))])
    if not r.get('ran'): print(f'  skipped: {r.get("reason")}'); return None
    bad = 0
    for f, v in sorted(r['results'].items()):
        ok = v in ('ok', 'no-compile-command', 'no-ninja-target'); bad += 0 if ok else 1
        print(f'  {"ok " if ok else "FAIL"} {f}' + ('' if v == 'ok' else f'  ({v[:300]})'))
    return bad == 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--files', default='')
    ap.add_argument('--commits', default='', help='<base>..<head>: 커밋마다 체크아웃해 컴파일')
    a = ap.parse_args()
    ok_all = True
    if a.commits:
        dirty = sh('git', '-C', a.repo, 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules=all').stdout.strip()
        if dirty: sys.exit(f'워킹트리가 dirty 다 — 커밋 모드는 깨끗한 트리에서만 (git status: {dirty[:200]})')
        orig = sh('git', '-C', a.repo, 'symbolic-ref', '-q', '--short', 'HEAD').stdout.strip() or sh('git', '-C', a.repo, 'rev-parse', 'HEAD').stdout.strip()
        commits = sh('git', '-C', a.repo, 'rev-list', '--reverse', a.commits).stdout.split()
        try:
            for c in commits:
                files = sh('git', '-C', a.repo, 'diff-tree', '--no-commit-id', '--name-only', '-r', c).stdout.split()
                print(f'== {c[:9]} {sh("git","-C",a.repo,"log","-1","--format=%s",c).stdout.strip()[:80]}')
                r = sh('git', '-C', a.repo, 'checkout', '-q', c)
                if r.returncode: print(f'  checkout 실패: {r.stderr.strip()}'); ok_all = False; continue
                res = check_files(a.repo, files)
                ok_all = ok_all and (res is not False)
        finally:
            sh('git', '-C', a.repo, 'checkout', '-q', orig)
            print(f'== 복귀: {orig}')
    else:
        files = [f.strip() for f in a.files.split(',') if f.strip()] or sh('git', '-C', a.repo, 'diff', '--name-only', 'HEAD').stdout.split()
        res = check_files(a.repo, files); ok_all = res is not False
    sys.exit(0 if ok_all else 1)

if __name__ == '__main__': main()
