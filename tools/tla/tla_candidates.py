#!/usr/bin/env python3
"""tla_candidates — PR diff 에서 TLA+ 모델 검사 후보(동시성·상태기계 변경)를 고른다.

harness-review 하위 스킬 skills/harness-review/tla.md 의 1단계(돌릴지 정하기)에 쓴다. 판정은 하지 않고 근거만 낸다 —
신호가 있는 변경 함수와 맞은 신호를 보이고, 모델로 만들지는 리뷰어가 tla.md 「언제」로 정한다.

사용:
  python3 tools/tla/tla_candidates.py --pr <N>            # gh pr diff -R CUBRID/cubrid <N>
  python3 tools/tla/tla_candidates.py --diff <file.diff>  # 미push 커밋이면 git diff base..head > file
출력: 후보마다 `<파일>:<함수>  [동시성|상태기계] 신호…` 한 줄, 끝에 `TLA 후보 N개` (없으면 `TLA 후보 없음`).
종료 코드: 후보가 있으면 0, 없으면 3, diff 를 못 얻으면 2. 부작용 없음(gh 읽기만).
신호는 diff 의 +/- 줄과 hunk 머리의 함수 이름에서 찾는다 — 바뀌지 않은 문맥 줄은 보지 않는다.
"""
import argparse
import re
import subprocess
import sys

# 동시성: 스레드·잠금·래치·원자 연산·데몬 — 엇갈림(interleaving)이 결과를 바꿀 수 있는 곳
CONCURRENCY = [
    r'pthread_(mutex|cond|rwlock|spin)', r'std::(mutex|shared_mutex|condition_variable|atomic|thread|lock_guard|unique_lock)',
    r'\bATOMIC_\w+', r'\batomic_\w+', r'compare_exchange', r'memory_order', r'\bcsect_(enter|exit)\w*',
    r'\bpgbuf_(fix|unfix|ordered_fix|promote)\w*', r'\blatch\w*', r'\block_(object|unlock|hold|demote|scan|subclass)\w*',
    r'\blf_\w+', r'\bcubthread::', r'\bdaemon\w*', r'\bworker_pool\w*', r'\bvacuum_\w+', r'\bwait_for\w*',
    r'\b(notify_one|notify_all|wakeup|sleep)\b', r'\bvolatile\b', r'\blog_(append|flush|wakeup)\w*',
]
# 상태기계: 위치·상태 변수와 그 전이 — 한 스레드라도 단계 사이의 불변식이 깨질 수 있는 곳
STATE_MACHINE = [
    r'\bposition\s*[=!]=?\s*S_\w+', r'\bS_(BEFORE|ON|AFTER|GO_BACKWARD|END)\b', r'->\s*state\s*=[^=]',
    r'\bswitch\s*\(\s*[\w>.-]*state\b', r'\b\w+_STATE_\w+\b', r'\bgoto\s+(retry|restart)\b',
    r'\bcurr_\w+(no|p)\b',
]


def get_diff(a):
    if a.diff:
        with open(a.diff, encoding='utf-8', errors='replace') as f:
            return f.read()
    p = subprocess.run(['gh', 'pr', 'diff', '-R', a.repo, str(a.pr)], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    if p.returncode != 0:
        print('error: gh pr diff 실패 — %s' % p.stderr.strip()[:200])
        return None
    return p.stdout


def scan(diff):
    found = {}   # (file, func) -> {'동시성': set, '상태기계': set}
    file = func = None
    for line in diff.splitlines():
        if line.startswith('+++ '):
            file = line[6:] if line.startswith('+++ b/') else None
            continue
        if file is None or not re.search(r'\.(c|cpp|h|hpp|cc)$', file):
            continue
        m = re.match(r'^@@ [^@]* @@\s*(.*)$', line)
        if m:
            fm = re.search(r'(\w+)\s*\(', m.group(1))
            func = fm.group(1) if fm else (m.group(1).strip() or '?')
            continue
        if not line or line[0] not in '+-' or line.startswith(('+++', '---')):
            continue
        body = line[1:]
        for kind, pats in (('동시성', CONCURRENCY), ('상태기계', STATE_MACHINE)):
            for pat in pats:
                for hit in re.finditer(pat, body):
                    found.setdefault((file, func or '?'), {'동시성': set(), '상태기계': set()})[kind].add(hit.group(0).strip())
    return found


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--pr', type=int)
    g.add_argument('--diff')
    ap.add_argument('--repo', default='CUBRID/cubrid')
    a = ap.parse_args(argv)
    diff = get_diff(a)
    if diff is None:
        return 2
    found = scan(diff)
    for (file, func), kinds in sorted(found.items()):
        tags = ' '.join('[%s] %s' % (k, ', '.join(sorted(v)[:6])) for k, v in kinds.items() if v)
        print('%s:%s  %s' % (file, func, tags))
    print('TLA 후보 %d개' % len(found) if found else 'TLA 후보 없음')
    return 0 if found else 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
