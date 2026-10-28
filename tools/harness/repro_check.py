#!/usr/bin/env python3
"""repro_check — 게시 전, 코멘트 초안에 적은 ```sql 블록**만** 새 SA 프로세스로 develop·PR 설치본에 다시 돌린다(H-27).

왜: review-response §2 3-1 "게시 전에 코멘트에 적힌 줄만 새 프로세스에 그대로 넣어 한 번 더 돌린다" 를 사람이 기억했다 —
    2026-10-07 PR#8022 에서 같은 날 두 번 빠졌고(앞선 PREPARE·ALTER 가 코멘트에 없었다), 10-12 게이트키퍼가 기록이 없어 막았다.
    이 도구가 그 재실행이고, 게이트키퍼는 산출물(repro_check.json)이 지금 초안과 맞는지만 본다(--verify).

사용(리포 루트에서):
    python3 -m tools.harness.repro_check --draft <comments.draft.md|replies.md> --dev <develop 설치본> --pr <PR 설치본> [--out OUT]
                                         [--only 1,3 | T12] [--param k=v ...] [--timeout 300] [--keep-db]
    python3 -m tools.harness.repro_check --draft <초안> --verify [--out OUT]      # 게이트키퍼: 산출물이 지금 초안의 재실행인가
입력: 초안의 `## ` 절마다(comments.draft.md 의 "## N번 코멘트", replies.md 의 "## T<n> — …" — `> ` 인용도 벗긴다) ```sql 울타리 블록을
      순서대로 이어 한 스크립트로 만든다. 다른 울타리(```suggestion·```c)는 돌리지 않는다. --param 은 설치본 conf 사본에 덧붙여
      CUBRID_CONF_FILE 로 넘긴다(설치본 conf 무편집 — sql-difftest §0, H-39).
동작: 절 하나 × 설치본 하나마다 버릴 DB 를 OUT/repro_check/db/ 아래에 만들고(createdb, 볼륨 20M), `csql -S`(SA) 새 프로세스로 돌린 뒤
      DB 를 지운다 — 절 사이에 상태가 새지 않는다. 떠 있는 서버는 건드리지 않는다(버릴 DB 는 서버가 서비스하지 않는다).
      설치본 conf 의 data_buffer_size 가 --max-buffer(기본 2G)보다 크면 사본에서 그 값으로 낮춘다(측정용 16G conf 로 SA 를 띄우지 않게 — 결과에 적는다).
출력: OUT/repro_check.json(초안 sha1·설치본 리비전·절마다 SQL·출력 경로·오류 줄·두 빌드의 같음 여부·all_ran) · OUT/repro_check.md(표) ·
      OUT/repro_check/<절>.sql·<절>.<dev|pr>.out. 종료 코드: 0 = 전부 돌았다(차이가 있어도 0 — 차이는 판정 재료), 2 = 못 돈 절이 있다,
      3 = 초안에 ```sql 이 없다. --verify: 0 = 산출물이 지금 초안의 재실행이고 전부 돌았다, 1 = 없음·초안이 바뀜·안 돈 절.
한계: 결과가 맞는지는 판정하지 않는다(사람·harness-repro 의 몫) — 두 빌드 출력과 오류 줄만 나란히 놓는다. 세션 여러 개·서버 모드(CS)가
      필요한 재현은 돌리지 못한다(그 절은 초안에 ```sql 대신 다른 울타리로 적고 사람이 돌린다). 사이드이펙트: OUT 아래 파일 쓰기와
      임시 DB 생성·삭제뿐(외부 게시 없음).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

FENCE = re.compile(r'^[ \t]*(`{3,}|~{3,})[ \t]*([A-Za-z0-9_+-]*)[^\n]*$')
HEAD = re.compile(r'^## (.+)$')
DB_VOL = '20M'          # 버릴 DB 의 볼륨 크기 — createdb 최소에 가깝게(절마다 만들고 지운다, .50 실측 1.7초·약 215MB)
MAX_BUFFER = '2G'       # SA 가 설치본 conf 의 측정용 버퍼(16G)를 그대로 잡지 않게 하는 상한


def sha1(path):
    with open(path, 'rb') as f:
        return hashlib.sha1(f.read()).hexdigest()


def sections(txt):
    """[(key, title, [sql 블록…])] — `## ` 절마다, 인용(`> `)을 벗기고 ```sql 블록만."""
    out, cur, lines = [], None, []

    def flush():
        if cur is None:
            return
        body = [l[2:] if l.startswith('> ') else (l[1:] if l.startswith('>') else l) for l in lines]
        blocks, inb, buf, fence = [], False, [], ''
        for l in body:
            m = FENCE.match(l)
            if not inb and m:
                inb, fence, lang, buf = True, m.group(1), m.group(2).lower(), []
                continue
            if inb and m and l.strip().startswith(fence[0] * 3) and not m.group(2):
                if lang == 'sql':
                    blocks.append('\n'.join(buf).strip('\n'))
                inb = False
                continue
            if inb:
                buf.append(l)
        m = re.match(r'(\d+)번', cur) or re.match(r'(T\d+)\b', cur)
        key = (m.group(1) + ('번' if m.group(1).isdigit() else '')) if m else re.sub(r'\W+', '_', cur)[:24]
        out.append((key, cur.strip(), blocks))

    for ln in txt.split('\n'):
        h = HEAD.match(ln)
        if h:
            flush()
            cur, lines = h.group(1), []
        elif cur is not None:
            lines.append(ln)
    flush()
    return out


def _size(v):
    m = re.match(r'^\s*(\d+(?:\.\d+)?)\s*([KMGT]?)B?\s*$', v or '', re.I)
    if not m:
        return None
    return float(m.group(1)) * {'': 1, 'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30, 'T': 1 << 40}[m.group(2).upper()]


def _conf(inst, params, max_buffer, dest):
    """설치본 conf 사본 + --param 줄(+ 버퍼 상한). 반환 (conf 경로|None, 메모)."""
    src = os.path.join(inst, 'conf', 'cubrid.conf')
    notes, extra = [], list(params)
    try:
        with open(src, encoding='utf-8', errors='replace') as f:
            txt = f.read()
    except OSError:
        return None, ['conf 없음: ' + src]
    m = re.findall(r'^\s*data_buffer_size\s*=\s*(\S+)', txt, re.M)
    if m and _size(m[-1]) and _size(max_buffer) and _size(m[-1]) > _size(max_buffer) and not any(p.startswith('data_buffer_size') for p in params):
        extra.append('data_buffer_size=' + max_buffer)
        notes.append('data_buffer_size %s → %s (SA 메모리 상한)' % (m[-1], max_buffer))
    if not extra:
        return None, notes
    with open(dest, 'w', encoding='utf-8') as f:
        f.write(txt + '\n# repro_check\n' + '\n'.join(extra) + '\n')
    return dest, notes + (['--param ' + ' '.join(params)] if params else [])


def _env(inst, dbdir, conf):
    env = dict(os.environ, CUBRID=inst, CUBRID_DATABASES=dbdir, PATH=os.path.join(inst, 'bin') + ':' + os.environ.get('PATH', ''),
               LD_LIBRARY_PATH=os.path.join(inst, 'lib'))
    env.pop('CUBRID_CONF_FILE', None)
    if conf:
        env['CUBRID_CONF_FILE'] = conf
    return env


def _rev(inst):
    try:
        with open(os.path.join(inst, 'BUILD_REV'), encoding='utf-8', errors='replace') as f:
            m = re.search(r'\b([0-9a-f]{7,40})\b', f.readline())
            return m.group(1) if m else '?'
    except OSError:
        return '?'


NORM = re.compile(r'\(\d+\.\d+ sec\)|OID: [0-9|]+|CLASS_OID: [0-9|]+')


def run_one(inst, tag, key, sql, workdir, conf, timeout, keep):
    """버릴 DB 하나를 만들고 SA 새 프로세스로 sql 을 돌린 뒤 지운다. 반환 dict."""
    dbname = 'rc%s%s' % (re.sub(r'[^A-Za-z0-9]', '', key)[:6], tag[:3])     # DB 이름은 ASCII 만
    dbdir = os.path.join(workdir, 'db', tag, key)
    shutil.rmtree(dbdir, ignore_errors=True)
    os.makedirs(dbdir)
    open(os.path.join(dbdir, 'databases.txt'), 'w').close()
    env = _env(inst, dbdir, conf)
    t0 = time.time()
    cr = subprocess.run([os.path.join(inst, 'bin', 'cubrid'), 'createdb', '--db-volume-size=' + DB_VOL, '--log-volume-size=' + DB_VOL,
                         '-F', dbdir, '-L', dbdir, dbname, 'en_US.utf8'], cwd=dbdir, env=env, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, universal_newlines=True, timeout=600)
    res = {'createdb_rc': cr.returncode, 'out': os.path.join(workdir, '%s.%s.out' % (key, tag))}
    if cr.returncode:
        res.update(ran=False, error='createdb 실패: ' + cr.stdout.strip()[-300:])
        return res
    try:
        r = subprocess.run([os.path.join(inst, 'bin', 'csql'), '-S', '-u', 'dba', dbname, '-i', sql], cwd=dbdir, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, timeout=timeout)
        out, rc = r.stdout, r.returncode
    except subprocess.TimeoutExpired as e:
        out, rc = (e.stdout or '') if isinstance(e.stdout, str) else '', 'timeout'
    with open(res['out'], 'w', encoding='utf-8') as f:
        f.write(out)
    errs = [l.strip() for l in out.split('\n') if re.search(r'\bERROR\b', l)]
    res.update(ran=rc != 'timeout', rc=rc, seconds=round(time.time() - t0, 1), errors=errs[:20], n_errors=len(errs),
               norm=NORM.sub('', out))
    if not keep:
        subprocess.run([os.path.join(inst, 'bin', 'cubrid'), 'deletedb', dbname], cwd=dbdir, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.rmtree(dbdir, ignore_errors=True)
    return res


def verify(draft, out):
    p = os.path.join(out, 'repro_check.json')
    if not os.path.isfile(p):
        print('✗ repro_check.json 없음 — 게시 전 재실행 기록이 없다: python3 -m tools.harness.repro_check --draft %s --dev … --pr …' % draft)
        return 1
    with open(p, encoding='utf-8') as f:
        rec = json.load(f)
    with open(draft, encoding='utf-8') as f:
        secs = [s for s in sections(f.read()) if s[2]]
    bad = []
    if rec.get('draft_sha1') != sha1(draft):
        bad.append('초안이 재실행 뒤 바뀌었다(sha1 %s → %s) — 다시 돌린다' % (rec.get('draft_sha1', '?')[:10], sha1(draft)[:10]))
    want = {k for k, _, _ in secs}
    got = {s['key'] for s in rec.get('sections', []) if all(s.get(t, {}).get('ran') for t in ('dev', 'pr'))}
    if want - got:
        bad.append('돌지 않은 절: %s' % sorted(want - got))
    if bad:
        print('✗ ' + ' / '.join(bad))
        return 1
    print('✓ repro_check: 초안 sha1 일치, ```sql 절 %d개 모두 develop(%s)·PR(%s) 새 SA 프로세스로 돎 (%s)' % (
        len(want), rec['installs']['dev']['rev'], rec['installs']['pr']['rev'], rec.get('finished', '?')))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0], epilog=__doc__[__doc__.index('사용'):],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--draft', required=True)
    ap.add_argument('--dev', help='develop(기준) 설치본 경로')
    ap.add_argument('--pr', help='PR 설치본 경로')
    ap.add_argument('--out', default='', help='산출 디렉터리(기본: 초안이 있는 디렉터리)')
    ap.add_argument('--only', default='', help='돌릴 절 키(1,3 또는 T12) — 기본 전부')
    ap.add_argument('--param', action='append', default=[], help='conf 사본에 덧붙일 k=v (반복)')
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--max-buffer', default=MAX_BUFFER)
    ap.add_argument('--keep-db', action='store_true', help='버릴 DB 를 지우지 않는다(조사용 — 끝나면 직접 지운다)')
    ap.add_argument('--verify', action='store_true', help='게이트키퍼용: 산출물이 지금 초안의 재실행인지만 본다')
    a = ap.parse_args(argv)
    draft = os.path.abspath(os.path.expanduser(a.draft))
    out = os.path.abspath(os.path.expanduser(a.out)) if a.out else os.path.dirname(draft)
    if a.verify:
        return verify(draft, out)
    if not (a.dev and a.pr):
        ap.error('--dev 와 --pr 설치본이 필요하다(--verify 가 아니면)')
    with open(draft, encoding='utf-8') as f:
        secs = sections(f.read())
    only = {x.strip() for x in a.only.split(',') if x.strip()}
    todo = [s for s in secs if s[2] and (not only or s[0] in only or s[0].rstrip('번') in only)]
    if not todo:
        print('초안에 ```sql 블록이 있는 절이 없다(%d절) — 재실행할 것이 없다' % len(secs))
        return 3
    work = os.path.join(out, 'repro_check')
    os.makedirs(work, exist_ok=True)
    installs = {}
    for tag, inst in (('dev', a.dev), ('pr', a.pr)):
        inst = os.path.abspath(os.path.expanduser(inst))
        if not os.access(os.path.join(inst, 'bin', 'csql'), os.X_OK):
            ap.error('%s: %s/bin/csql 없음' % (tag, inst))
        conf, notes = _conf(inst, a.param, a.max_buffer, os.path.join(work, 'cubrid.%s.conf' % tag))
        installs[tag] = {'path': inst, 'rev': _rev(inst), 'conf': conf, 'conf_notes': notes}
    rec = {'draft': draft, 'draft_sha1': sha1(draft), 'installs': installs, 'params': a.param,
           'started': time.strftime('%Y-%m-%dT%H:%M:%S'), 'sections': []}
    for key, title, blocks in todo:
        sql = os.path.join(work, key + '.sql')
        with open(sql, 'w', encoding='utf-8') as f:
            f.write('\n'.join(blocks) + '\n')
        s = {'key': key, 'title': title, 'blocks': len(blocks), 'sql': sql}
        for tag in ('dev', 'pr'):               # 순차 — 동시에 띄우면 PL 서버 기동이 부딪힌다(harness-repro 절차 2)
            r = run_one(installs[tag]['path'], tag, key, sql, work, installs[tag]['conf'], a.timeout, a.keep_db)
            s[tag] = {k: v for k, v in r.items() if k != 'norm'}
            s[tag + '_norm'] = r.get('norm')
        dn, pn = s.pop('dev_norm'), s.pop('pr_norm')
        s['same'] = dn is not None and dn == pn
        rec['sections'].append(s)
        print('%-6s 블록 %d · dev %s(오류 %s) · pr %s(오류 %s) · %s' % (key, len(blocks), 'ok' if s['dev'].get('ran') else '✗',
                                                                   s['dev'].get('n_errors', '-'), 'ok' if s['pr'].get('ran') else '✗',
                                                                   s['pr'].get('n_errors', '-'), '같음' if s['same'] else '다름'))
    if not a.keep_db:
        shutil.rmtree(os.path.join(work, 'db'), ignore_errors=True)     # 절마다 지운 빈 디렉터리까지
    rec['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S')
    rec['all_ran'] = all(s[t].get('ran') for s in rec['sections'] for t in ('dev', 'pr'))
    with open(os.path.join(out, 'repro_check.json'), 'w', encoding='utf-8') as f:
        json.dump(rec, f, ensure_ascii=False, indent=1)
    md = ['# repro_check — %s' % os.path.basename(draft), '',
          '초안 sha1 `%s` · develop `%s` · PR `%s` · %s → %s' % (rec['draft_sha1'][:12], installs['dev']['rev'], installs['pr']['rev'],
                                                              rec['started'], rec['finished'])]
    for tag in ('dev', 'pr'):
        if installs[tag]['conf_notes']:
            md.append('- %s conf 사본: %s' % (tag, '; '.join(installs[tag]['conf_notes'])))
    md += ['', '| 절 | 블록 | develop | PR | 두 빌드 | SQL |', '|---|---:|---|---|---|---|']
    for s in rec['sections']:
        cell = lambda r: ('✗ ' + r.get('error', str(r.get('rc')))[:40]) if not r.get('ran') else ('오류 %d' % r['n_errors'] + (' — ' + r['errors'][0][:60] if r['errors'] else ''))
        md.append('| %s | %d | %s | %s | %s | `%s` |' % (s['key'], s['blocks'], cell(s['dev']), cell(s['pr']), '같음' if s['same'] else '**다름**',
                                                      os.path.relpath(s['sql'], out)))
    md += ['', '출력: `repro_check/<절>.dev.out`·`<절>.pr.out`. 결과가 코멘트 서술과 맞는지는 사람이 본다 — 안 맞으면 조건(셋업·앞선 실행·ALTER)이 빠진 것이다(review-response §2 3-1).']
    with open(os.path.join(out, 'repro_check.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(md) + '\n')
    print('→ %s/repro_check.md · all_ran=%s' % (out, rec['all_ran']))
    return 0 if rec['all_ran'] else 2


if __name__ == '__main__':
    sys.exit(main())
