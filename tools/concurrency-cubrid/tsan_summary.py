#!/usr/bin/env python3
"""tsan_summary.py <report files or directory>... [--top N] [--md out.md] — groups ThreadSanitizer reports.

A TSan-instrumented server (goto <rev> tsan) writes one file per process (TSAN_OPTIONS log_path=…/tsan → tsan.<pid>),
and the same race is reported again for every pair of threads and call paths that meet it. A report is keyed by its
kind and the top engine frames of its two accesses (the SUMMARY line's frame and the previous access's frame), so a
key is one place to look at. Frames of libtsan, libc and the C++ runtime are skipped.
Output: per key the count, the processes it came from, the two frames, the summary line and one sample file; with --md
the same as a markdown table plus the first sample report of each key.
"""
import argparse
import collections
import os
import re
import sys

SEP = re.compile(r'^={10,}\s*$', re.M)
KIND = re.compile(r'WARNING: ThreadSanitizer: ([^(\n]+?)\s*(?:\(pid=(\d+)\))?\s*$', re.M)
# '#N <function, a C++ one with its parameter list and spaces> <path>:<line>[:<col>] (<object>+0x<offset>)' or '… <null> (…)'
FRAME = re.compile(r'^\s+#(\d+)\s+(.*?)\s+(\S+)\s+\(([^()+]+)\+0x[0-9a-f]+\)\s*$')
LOC = re.compile(r'^(.*?):(\d+)(?::\d+)?$')
HAS_FRAME = re.compile(r'^\s+#\d+ ', re.M)   # a block with a stack (FRAME matches one line from its start)
SKIP_FN = re.compile(r'^(__tsan|__interceptor|pthread_|operator |std::|__gnu_cxx|_ZNSt|<null>|__libc|start_thread|clone)')
SKIP_OBJ = re.compile(r'(libtsan|libc\.so|libstdc\+\+|libpthread|ld-linux)')


def frames(block):
    out = []
    for line in block.split('\n'):
        m = FRAME.match(line)
        if not m:
            if out and not line.strip():
                break
            continue
        fn, where, obj = m.group(2), m.group(3), m.group(4)
        loc = LOC.match(where)
        path, ln = (loc.group(1), loc.group(2)) if loc else (where, '?')
        fn = re.sub(r'\(.*$', '', fn)          # the parameter list is noise for grouping
        if SKIP_FN.match(fn) or SKIP_OBJ.search(obj):
            continue
        out.append('%s %s:%s' % (fn, os.path.basename(path), ln))
    return out


def parse(text, src):
    for rep in SEP.split(text):
        k = KIND.search(rep)
        if not k:
            continue
        kind = k.group(1).strip()
        parts = re.split(r'\n\s*\n', rep[k.end():])
        stacks = [frames(p) for p in parts if HAS_FRAME.search(p)]
        first = stacks[0][0] if stacks and stacks[0] else '?'
        second = stacks[1][0] if len(stacks) > 1 and stacks[1] else '-'
        summ = re.search(r'^SUMMARY: ThreadSanitizer: (.*)$', rep, re.M)
        yield (kind, first, second), (summ.group(1) if summ else ''), src, rep.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('paths', nargs='+')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--md')
    a = ap.parse_args()
    files = []
    for p in a.paths:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.startswith('tsan')]
        elif os.path.isfile(p):
            files.append(p)
    groups = collections.OrderedDict()
    total = 0
    for f in files:
        with open(f, errors='replace') as fp:
            for key, summ, src, rep in parse(fp.read(), f):
                total += 1
                g = groups.setdefault(key, {'n': 0, 'procs': set(), 'summ': summ, 'sample': src, 'rep': rep})
                g['n'] += 1
                g['procs'].add(os.path.basename(src))
    ranked = sorted(groups.items(), key=lambda kv: -kv[1]['n'])
    print('%d reports in %d files, %d places' % (total, len(files), len(groups)))
    for (kind, first, second), g in ranked[:a.top]:
        print('%5d  %-22s %s\n       ↔ %s\n       %s  (%d proc, e.g. %s)' % (
            g['n'], kind[:22], first, second, g['summ'][:110], len(g['procs']), os.path.basename(g['sample'])))
    if a.md:
        with open(a.md, 'w') as out:
            out.write('| 횟수 | 종류 | 접근 1 | 접근 2 | 프로세스 |\n|---|---|---|---|---|\n')
            for (kind, first, second), g in ranked[:a.top]:
                out.write('| %d | %s | `%s` | `%s` | %d |\n' % (g['n'], kind, first, second, len(g['procs'])))
            for (kind, first, second), g in ranked[:a.top]:
                out.write('\n### %s — %s\n```\n%s\n```\n' % (kind, first, g['rep'][:4000]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
