"""annotate: function_notes 를 PR head 소스에 한국어 주석으로 심어 리뷰 전용 draft PR 을 만든다.

왜: 보고서와 GitHub diff 를 왕복하며 줄 번호를 맞추는 대신, Files changed 한 화면에서
**원 변경 + 한국어 설명 + 지적**을 같이 읽게 한다(사용자 아이디어 2026-10-06).

브랜치 둘을 fork 에 올린다 —
  review/pr<N>-base : PR head 와 develop 의 merge-base (주석 없는 기준점)
  review/pr<N>-ko   : PR head + 주석 커밋 1개
draft PR 은 fork 안에서 base<-ko 로 연다. diff = PR 의 전체 변경 + 내 주석.
upstream 에 열지 않는다 — 팀 PR 목록·알림에 끼지 않게.

사용:
  python3 -m tools.harness.annotate --pr 8022 --head cb0514bb7 --notes notes.json [--push] [--pr-create]
"""
import argparse, json, os, re, subprocess, sys

FORK = 'soheejung-cs/cubrid'


def sh(cmd, cwd=None, check=True):
    p = subprocess.run(cmd, cwd=cwd, shell=isinstance(cmd, str), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    if check and p.returncode:
        raise RuntimeError(f'{cmd}\n{p.stdout}')
    return p.stdout.strip()


def comment_block(note, indent=''):
    """C 블록 주석 한 덩이. 코드가 C/C++ 둘 다라 /* */ 로 통일한다(//는 .c 에서 C89 경고)."""
    w = []
    w.append(f"[리뷰] {note['function']} — {note['role']}")
    w.append(f"develop: {note['before']}")
    w.append(f"이 PR:   {note['after']}")
    if note.get('changed'):
        w.append(f"바뀐 것: {note['changed']}")
    for fid in note.get('finding_ids') or []:
        w.append(f"[지적 {fid}]")
    body = []
    for i, line in enumerate(w):
        for seg in wrap(line, 108):
            body.append(f'{indent} * {seg}' if i or True else seg)
    return f'{indent}/*\n' + '\n'.join(body) + f'\n{indent} */\n'


def wrap(s, n):
    out, cur = [], ''
    for tok in s.split(' '):
        if cur and len(cur) + 1 + len(tok) > n:
            out.append(cur); cur = tok
        else:
            cur = f'{cur} {tok}'.strip()
    if cur:
        out.append(cur)
    return out or ['']


def apply_notes(repo, notes):
    """파일별로 아래에서 위로 삽입한다 — 위부터 넣으면 뒤 줄 번호가 밀린다."""
    by_file = {}
    for n in notes:
        by_file.setdefault(n['file'], []).append(n)
    touched = 0
    for f, ns in by_file.items():
        p = os.path.join(repo, f)
        if not os.path.isfile(p):
            print(f'  skip (파일 없음): {f}'); continue
        lines = open(p, encoding='utf-8', errors='surrogateescape').read().split('\n')
        for n in sorted(ns, key=lambda x: -int(x['line'])):
            i = int(n['line']) - 1
            if not (0 <= i < len(lines)):
                print(f"  skip (줄 범위 밖): {f}:{n['line']}"); continue
            indent = re.match(r'[ \t]*', lines[i]).group(0)
            lines.insert(i, comment_block(n, indent).rstrip('\n'))
            touched += 1
        open(p, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(lines))
    return touched


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pr', required=True)
    ap.add_argument('--head', required=True, help='PR head sha')
    ap.add_argument('--notes', required=True, help='function_notes JSON (배열 또는 {function_notes:[...]})')
    ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--base-ref', default='upstream/develop')
    ap.add_argument('--push', action='store_true')
    ap.add_argument('--pr-create', action='store_true')
    a = ap.parse_args()

    raw = json.load(open(a.notes, encoding='utf-8'))
    notes = raw['function_notes'] if isinstance(raw, dict) else raw
    print(f'function_notes {len(notes)}건')

    base = sh(f'git merge-base {a.head} {a.base_ref}', cwd=a.repo)
    bb, kb = f'review/pr{a.pr}-base', f'review/pr{a.pr}-ko'
    print(f'base={base[:9]} ({bb})  head={a.head[:9]} ({kb})')

    cur = sh('git rev-parse --abbrev-ref HEAD', cwd=a.repo)
    dirty = sh('git status --porcelain', cwd=a.repo)
    if dirty:
        raise SystemExit('워킹트리가 dirty 다 — 커밋하거나 비우고 다시 (goto 와 같은 이유)')

    sh(f'git checkout -q -B {kb} {a.head}', cwd=a.repo)
    n = apply_notes(a.repo, notes)
    print(f'주석 {n}곳 삽입')
    sh('git add -A', cwd=a.repo)
    msg = (f'[리뷰주석] PR #{a.pr} 변경 함수 {n}곳에 한국어 설명\n\n'
           '리뷰 전용 커밋이다 — 머지 대상이 아니다. 각 변경 함수 위에\n'
           'develop 에서 무엇이었고 / 이 PR 에서 무엇이 되었고 / 무엇이 바뀌었는지와\n'
           '달린 지적 번호를 붙였다. 원 변경과 같은 화면에서 읽히도록.\n')
    sh(['git', 'commit', '-q', '-m', msg], cwd=a.repo)
    note_sha = sh('git rev-parse --short=9 HEAD', cwd=a.repo)
    print(f'주석 커밋 {note_sha}')

    if a.push:
        sh(f'git push -q -f origin {base}:refs/heads/{bb}', cwd=a.repo)
        sh(f'git push -q -f origin {kb}:refs/heads/{kb}', cwd=a.repo)
        print(f'push 완료: {bb}, {kb}')
    if a.pr_create:
        body = (f'PR #{a.pr} 의 변경을 **한국어 설명 주석과 함께** 읽기 위한 리뷰 전용 draft 다. '
                f'머지 대상이 아니다.\n\n'
                f'- base `{bb}` = PR head 와 develop 의 merge-base (`{base[:9]}`)\n'
                f'- head `{kb}` = PR head (`{a.head[:9]}`) + 주석 커밋 (`{note_sha}`)\n'
                f'- 그래서 Files changed = **원 PR 의 전체 변경 + 주석 {n}곳**\n\n'
                f'원본: https://github.com/CUBRID/cubrid/pull/{a.pr}\n')
        out = sh(['gh', 'pr', 'create', '-R', FORK, '--draft', '--base', bb, '--head', kb,
                  '--title', f'[리뷰주석] PR #{a.pr} 한국어 설명본 (머지 대상 아님)', '--body', body], check=False)
        print(out)
    sh(f'git checkout -q {cur}', cwd=a.repo, check=False)


if __name__ == '__main__':
    main()
