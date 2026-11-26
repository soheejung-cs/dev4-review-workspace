#!/usr/bin/env python3
"""Run BtreeLatchOrder over every multiset of programs (2..K threads) and list the minimal deadlocking ones.

usage: enumerate.py <work dir> <FIX> <K> <programs,...> [--jobs N]
A multiset that contains a smaller deadlocking multiset is skipped (only minimal combinations are reported).
Output: <work dir>/<FIX>.tsv  (combo, verdict, states, who holds what / waits for what in the deadlocked state)
"""
import itertools, os, re, subprocess, sys, shutil, collections
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
JAVA = '/usr/lib/jvm/java-11/bin/java'
JAR = os.path.expanduser('~/tools/tla/tla2tools.jar')

def run(work, fix, combo, idx):
    d = os.path.join(work, f'{fix}-{idx}')
    os.makedirs(d, exist_ok=True)
    shutil.copy(os.path.join(HERE, 'BtreeLatchOrder.tla'), d)
    progs = ', '.join(f'"{p}"' for p in combo)
    open(os.path.join(d, 'MC.tla'), 'w').write(
        f'---- MODULE MC ----\nEXTENDS BtreeLatchOrder\nMCProgs == << {progs} >>\nMCFix == "{fix}"\n====\n')
    open(os.path.join(d, 'MC.cfg'), 'w').write(
        'SPECIFICATION Spec\nCONSTANTS\n    Progs <- MCProgs\n    FIX <- MCFix\nINVARIANTS TypeOK LatchSafe\n')
    p = subprocess.run([JAVA, '-Xmx384m', '-XX:+UseSerialGC', '-Djava.io.tmpdir=' + d, '-cp', JAR, 'tlc2.TLC', '-workers', '1',
                        '-metadir', os.path.join(d, 'st'), '-config', 'MC.cfg', 'MC.tla'],
                       cwd=d, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    out = p.stdout
    m = re.search(r'(\d+) states generated, (\d+) distinct', out)
    states = m.group(2) if m else '?'
    if 'Deadlock reached' in out:
        verdict, why = 'DEADLOCK', summarize(out, combo)
    elif 'is violated' in out or 'Error:' in out:
        verdict, why = 'ERROR', out[out.find('Error:'):][:300].replace('\n', ' ')
    else:
        verdict, why = 'ok', ''
    shutil.rmtree(d, ignore_errors=True)
    return combo, verdict, states, why

def summarize(out, combo):
    last = re.sub(r'\n\s+(?!/\\)', ' ', out[out.rfind('State '):])  # TLC wraps long values
    def grab(name):
        m = re.search(r'/\\ ' + name + r' = (.*)', last)
        return m.group(1) if m else ''
    st = re.findall(r'"(\w+)"', grab('st'))
    cnt = dict(re.findall(r'(\w+) \|-> <<([^>]*)>>', grab('cnt')))
    mode = dict(re.findall(r'(\w+) \|-> "(\w)"', grab('mode')))
    q = grab('queue')
    waits = collections.defaultdict(list)
    for pg, body in re.findall(r'(\w+) \|-> <<((?:<<\d+, "\w">>,? ?)*)>>', q):
        for t, m in re.findall(r'<<(\d+), "(\w)">>', body):
            waits[int(t)].append(f'{pg}:{m}')
    parts = []
    for i, prog in enumerate(combo, 1):
        held = [f'{pg}:{mode[pg]}' for pg, c in cnt.items() if [int(x) for x in c.split(',')][i - 1] > 0]
        parts.append(f't{i} {prog} holds[{",".join(held)}] waits[{",".join(waits[i])}] {st[i-1] if i-1 < len(st) else ""}')
    return ' | '.join(parts)

def contains(big, small):
    cb, cs = collections.Counter(big), collections.Counter(small)
    return all(cb[k] >= v for k, v in cs.items())

def main():
    work, fix, k, progs = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4].split(',')
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else 8
    os.makedirs(work, exist_ok=True)
    found, rows, idx = [], [], 0
    for size in range(2, k + 1):
        todo = [c for c in itertools.combinations_with_replacement(progs, size)
                if not any(contains(c, f) for f in found)]
        with ThreadPoolExecutor(jobs) as ex:
            res = list(ex.map(lambda a: run(work, fix, a[1], f'{size}-{a[0]}'), enumerate(todo)))
        for combo, verdict, states, why in res:
            rows.append((size, ' '.join(combo), verdict, states, why))
            if verdict == 'DEADLOCK':
                found.append(combo)
        print(f'size {size}: ran {len(todo)}, deadlock {sum(1 for r in res if r[1] == "DEADLOCK")}, '
              f'error {sum(1 for r in res if r[1] == "ERROR")}', flush=True)
    with open(os.path.join(work, f'{fix}.tsv'), 'w') as f:
        f.write('size\tcombo\tverdict\tdistinct\tdeadlocked state\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')

if __name__ == '__main__':
    main()
