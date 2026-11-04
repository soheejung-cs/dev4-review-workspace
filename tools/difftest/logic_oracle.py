#!/usr/bin/env python3
"""logic_oracle.py -- 기준 빌드 없이 한 빌드 안에서 논리 결함(오답·행 누락)을 찾는 오라클 (SQLancer 의 TLP·NoREC 방식).

빌드 간 A/B(difftest.sh·ab_sa.sh)는 기준 빌드가 있어야 하고, 기준 빌드도 틀리면(선재 결함) 차이로 안 보인다.
이 도구는 한 빌드의 답이 서로 맞물려야 하는 성질만 본다:
  TLP-COUNT : count(where p) + count(where not p) + count(where p 가 UNKNOWN) = count(*)      (술어 3분할)
  TLP-AGG   : 세 분할의 sum/min/max 를 합치면 전체의 sum/min/max
  NoREC     : count(*) where p  =  count(case when p then 1 end)  — 오른쪽은 p 를 식으로만 평가(인덱스·조건 이동 없음)
UNKNOWN 분할은 `(case when p then 1 when not (p) then 0 end) is null` 로 쓴다(`p is null` 은 술어에 못 쓴다).

생성기: 표 4개(lo_r1..lo_r4, NULL 10~15%, 인덱스 4개), 1~3표 내부/LEFT/RIGHT 조인, AND/OR/NOT 중첩, IN/BETWEEN/LIKE/IS NULL,
EXISTS/NOT EXISTS/IN/NOT IN/ANY/ALL/스칼라 부질의, 조인 힌트(USE_HASH/USE_NL/ORDERED/USE_MERGE). ON 절에는 부질의를 넣지 않는다(CUBRID 가 거부).

사용 예 (실행):
  python3 logic_oracle.py run --install ~/debug/CUBRID-goto-2ca10ed03 --db lodb --seed 2 --n 300 --out /tmp/lo   # SA, 서버 없이
  python3 logic_oracle.py run --cs --db coredb --seed 7 --n 500 --out /tmp/lo                                     # 떠 있는 서버(현재 PATH 의 csql)
  python3 logic_oracle.py run --install ~/release/CUBRID-11.5.develop --databases ~/scratch/dbs --db oldchk ...   # 다른 설치본·스크래치 DB
  python3 logic_oracle.py gen out.sql 2 300 ; csql ... -i out.sql > out.txt ; python3 logic_oracle.py check out.txt out.sql
산출물: <out>/lo_<seed>.sql(설정+검사), lo_<seed>.out(csql 출력), lo_<seed>.violations.txt(위반 전부와 질의). 화면에는 요약 몇 줄.
종료 코드: 0 위반 없음 / 1 위반 있음 / 2 사용법·실행 오류(DB 없음, SA 와 서버 충돌 등).
사이드이펙트: 대상 DB 에 표 lo_r1..lo_r4 를 지우고 다시 만든다(그 이름만 건드린다). 버릴 DB 에서 돌린다.
한계: 비결정 함수·LIMIT·윈도우는 생성하지 않는다. 오류가 난 검사는 건너뛴다(오류 자체는 판정하지 않음 — 건너뛴 수를 본다).
  위반이 나오면 그 검사의 p 를 최소 테이블로 줄여 재현하고, 옛 빌드에서도 같은지(선재인지) 따로 본다.
검증 이력: 2026-11-04 .52 — develop 2ca10ed03 debug 1,000검사 위반 0 / 2026-08-11 빌드(9217b50ef) 600검사 위반 5
  (전부 외부 조인의 null 채움 쪽에 상관된 EXISTS/NOT EXISTS — 그 사이 develop 에서 고쳐진 결함)를 기준 빌드 없이 잡음.
"""
import argparse, os, random, re, subprocess, sys

TABLES = {"lo_r1": 3000, "lo_r2": 2000, "lo_r3": 500, "lo_r4": 50}   # 행 수: 조인 결과가 수만 행 안쪽이 되게
COLS = ["a", "b", "c", "d", "e", "f", "id"]

def setup(rnd):
    P = ["drop table if exists %s;" % t for t in TABLES]
    for t, n in TABLES.items():
        P.append("create table %s (id int primary key, a int, b int, c varchar(8), d double, e numeric(10,2), f date);" % t)
        rows = []
        for i in range(1, n + 1):
            def v(p, x): return "null" if rnd.random() < p else x
            rows.append("(%d, %s, %s, %s, %s, %s, %s)" % (i, v(0.1, str(rnd.randint(0, 20))), v(0.15, str(rnd.randint(-5, 5))),
                v(0.1, "'%s'" % rnd.choice(["a", "b", "ab", "ba", "c", "", "A", "a "])), v(0.1, "%.1f" % rnd.uniform(-100, 100)),
                v(0.1, "%.2f" % rnd.uniform(-50, 50)), v(0.1, "date '2026-01-01' + %d" % rnd.randint(0, 400))))
        for k in range(0, n, 500):
            P.append("insert into %s values %s;" % (t, ", ".join(rows[k:k + 500])))
        P.append("create index %s_a on %s (a); create index %s_bc on %s (b, c); create index %s_f on %s (f); create index %s_e on %s (e);" % ((t, t) * 4))
    P.append("commit;")
    P.append("update statistics on %s with fullscan;" % ", ".join(TABLES))
    P.append("commit;")
    return P

def pred(rnd, als, depth=0):
    al = rnd.choice(als)
    r = rnd.random()
    if depth < 2 and r < 0.25:
        return "(%s %s %s)" % (pred(rnd, als, depth + 1), rnd.choice(["and", "or"]), pred(rnd, als, depth + 1))
    if depth < 2 and r < 0.32:
        return "not (%s)" % pred(rnd, als, depth + 1)
    c = rnd.choice(COLS)
    x = "%s.%s" % (al, c)
    if c in ("a", "b", "id"):
        k = rnd.randint(-5, 20)
        return rnd.choice(["%s = %d" % (x, k), "%s > %d" % (x, k), "%s <= %d" % (x, k), "%s <> %d" % (x, k),
                           "%s between %d and %d" % (x, rnd.randint(-5, 5), rnd.randint(5, 20)), "%s in (%d, %d, %d)" % (x, rnd.randint(0, 9), rnd.randint(0, 9), k),
                           "%s is null" % x, "%s is not null" % x, "%s not in (%d, %d)" % (x, rnd.randint(0, 9), k),
                           "%s in (select s.a from %s s where s.b = %d)" % (x, rnd.choice(list(TABLES)), rnd.randint(-5, 5)),
                           "%s not in (select s.a from %s s where s.b = %d and s.a is not null)" % (x, rnd.choice(list(TABLES)), rnd.randint(-5, 5)),
                           "exists (select 1 from %s s where s.a = %s and s.b > %d)" % (rnd.choice(list(TABLES)), x, rnd.randint(-5, 5)),
                           "not exists (select 1 from %s s where s.a = %s)" % (rnd.choice(list(TABLES)), x),
                           "%s = (select max(s.a) from %s s where s.b = %d)" % (x, rnd.choice(list(TABLES)), rnd.randint(-5, 5)),
                           "%s > all (select s.b from %s s where s.a = %d and s.b is not null)" % (x, rnd.choice(list(TABLES)), rnd.randint(0, 9)),
                           "%s = any (select s.b from %s s where s.a = %d)" % (x, rnd.choice(list(TABLES)), rnd.randint(0, 9)),
                           "coalesce(%s, -1) = -1" % x, "%s + 1 > %d" % (x, k), "mod(%s, 3) = 1" % x])
    if c == "c":
        return rnd.choice(["%s = '%s'" % (x, rnd.choice(["a", "b", "ab", "", "a "])), "%s like '%s'" % (x, rnd.choice(["a%", "%b", "_", "%", "a_"])),
                           "%s is null" % x, "%s in ('a', 'ab', '')" % x, "%s > 'a'" % x, "%s not like 'a%%'" % x, "length(%s) = 1" % x, "upper(%s) = 'A'" % x])
    if c in ("d", "e"):
        return rnd.choice(["%s > %d" % (x, rnd.randint(-60, 60)), "%s between %d and %d" % (x, rnd.randint(-60, 0), rnd.randint(0, 60)),
                           "%s is null" % x, "abs(%s) < 10" % x, "%s = %s" % (x, x), "round(%s) = %d" % (x, rnd.randint(-5, 5))])
    return rnd.choice(["%s < date '2026-06-01'" % x, "%s between date '2026-03-01' and date '2026-09-01'" % x, "%s is null" % x,
                       "month(%s) = %d" % (x, rnd.randint(1, 12))])

def pred_no_subquery(rnd, als):
    # CUBRID refuses a subquery in a join condition: draw again until the predicate has none
    while True:
        p = pred(rnd, als, 2)
        if "select" not in p:
            return p

def from_clause(rnd):
    nt = rnd.choice([1, 1, 2, 2, 2, 3])
    ts = rnd.sample(list(TABLES), nt)
    als = ["x%d" % k for k in range(nt)]
    f = "%s %s" % (ts[0], als[0])
    for k in range(1, nt):
        jt = rnd.choice(["inner join", "left outer join", "left outer join", "right outer join"])
        on = "%s.%s = %s.%s" % (als[k - 1], rnd.choice(["a", "b", "id"]), als[k], rnd.choice(["a", "b", "id"]))
        if rnd.random() < 0.3:
            on += " and " + pred_no_subquery(rnd, [als[k]])
        f += " %s %s %s on %s" % (jt, ts[k], als[k], on)
    return f, als

def gen(path, seed, n):
    rnd = random.Random(seed)
    out = setup(rnd)
    meta = []
    for i in range(n):
        f, als = from_clause(rnd)
        p = pred(rnd, als)
        hint = rnd.choice(["", "", "/*+ USE_HASH */ ", "/*+ USE_NL */ ", "/*+ ORDERED */ ", "/*+ USE_MERGE */ "])
        agg_col = "%s.%s" % (rnd.choice(als), rnd.choice(["a", "b", "id"]))
        unk = "(case when %s then 1 when not (%s) then 0 end) is null" % (p, p)
        qs = {
            "all":  "select %scount(*), sum(%s), min(%s), max(%s) from %s;" % (hint, agg_col, agg_col, agg_col, f),
            "p":    "select %scount(*), sum(%s), min(%s), max(%s) from %s where %s;" % (hint, agg_col, agg_col, agg_col, f, p),
            "np":   "select %scount(*), sum(%s), min(%s), max(%s) from %s where not (%s);" % (hint, agg_col, agg_col, agg_col, f, p),
            "nul":  "select %scount(*), sum(%s), min(%s), max(%s) from %s where %s;" % (hint, agg_col, agg_col, agg_col, f, unk),
            "norec": "select %scount(case when %s then 1 end) from %s;" % (hint, p, f),
        }
        meta.append((i, p, f))
        for k, q in qs.items():
            out.append("select 'MARK %05d %s' as mark from db_root;" % (i, k))
            out.append(q)
    open(path, "w").write("\n".join(out) + "\n")

def parse(path):
    res, key, buf = {}, None, []
    for l in open(path, errors="replace"):
        m = re.search(r"'MARK (\d{5}) (\w+)'", l)
        if m:
            if key: res[key] = buf
            key, buf = (int(m.group(1)), m.group(2)), []
            continue
        if key is None: continue
        if l.startswith("ERROR") or "ERROR:" in l:
            buf.append("ERROR " + l.strip())
        elif re.match(r"^\s+(NULL|-?[\d.]+(e[+-]?\d+)?)(\s+(NULL|-?[\d.]+(e[+-]?\d+)?))*\s*$", l) and "mark" not in l:
            buf.append(l.split())
    if key: res[key] = buf
    return res

def num(x):
    return None if x == "NULL" else float(x)

def check(outp, sqlp, detail=None):
    res = parse(outp)
    sql = open(sqlp).read().splitlines()
    qtext = {}
    for idx, l in enumerate(sql):
        m = re.search(r"'MARK (\d{5}) (\w+)'", l)
        if m: qtext[(int(m.group(1)), m.group(2))] = sql[idx + 1]
    ids = sorted({k[0] for k in res})
    ok = skipped = 0; bad = []
    for i in ids:
        r = {k: res.get((i, k), []) for k in ("all", "p", "np", "nul", "norec")}
        if any(any(isinstance(x, str) and x.startswith("ERROR") for x in v) or not v for v in r.values()):
            skipped += 1; continue
        a, p, np_, nu, nr = (r[k][0] for k in ("all", "p", "np", "nul", "norec"))
        problems = []
        cnt = [num(x[0]) for x in (a, p, np_, nu)]
        if cnt[1] + cnt[2] + cnt[3] != cnt[0]:
            problems.append("TLP-COUNT %s + %s + %s != %s" % (cnt[1], cnt[2], cnt[3], cnt[0]))
        if num(nr[0]) != cnt[1]:
            problems.append("NoREC count(where p)=%s vs count(case p)=%s" % (cnt[1], num(nr[0])))
        sums = [num(x[1]) for x in (p, np_, nu)]
        tot = num(a[1])
        s = sum(v for v in sums if v is not None) if any(v is not None for v in sums) else None
        if (s is None) != (tot is None) or (s is not None and abs(s - tot) > 1e-6 * max(1, abs(tot))):
            problems.append("TLP-SUM %s vs %s" % (sums, tot))
        mins = [num(x[2]) for x in (p, np_, nu) if num(x[2]) is not None]
        maxs = [num(x[3]) for x in (p, np_, nu) if num(x[3]) is not None]
        if (min(mins) if mins else None) != num(a[2]):
            problems.append("TLP-MIN %s vs %s" % (mins, a[2]))
        if (max(maxs) if maxs else None) != num(a[3]):
            problems.append("TLP-MAX %s vs %s" % (maxs, a[3]))
        if problems:
            bad.append((i, problems, qtext.get((i, "p")), qtext.get((i, "norec"))))
        else:
            ok += 1
    print("checks %d: ok %d, violated %d, skipped (error) %d" % (len(ids), ok, len(bad), skipped))
    lines = ["### %05d %s\n    %s\n    %s" % (i, "; ".join(pr), qp, qn) for i, pr, qp, qn in bad]
    if detail:
        open(detail, "w").write("\n".join(lines) + "\n")
        if bad: print("violations: %s" % detail)
    for l in lines[:5]:
        print(l)
    if len(lines) > 5: print("... %d more in %s" % (len(lines) - 5, detail))
    if len(ids) == 0: print("no checks parsed - csql failed? see %s" % outp)
    return len(bad) if ids else -1

def server_serves(db):
    """the PID of a cub_server serving db (SA mode would collide with it); pgrep -f matches its own command line"""
    p = subprocess.run(["pgrep", "-x", "cub_server"], stdout=subprocess.PIPE, text=True)
    for pid in p.stdout.split():
        try:
            argv = open("/proc/%s/cmdline" % pid).read().split("\0")
        except OSError:
            continue
        if len(argv) > 1 and argv[1] == db:
            return pid
    return None

def run(a):
    a.out = os.path.abspath(os.path.expanduser(a.out))   # csql runs with cwd=out: relative paths would break
    os.makedirs(a.out, exist_ok=True)
    sqlp = os.path.join(a.out, "lo_%d.sql" % a.seed)
    outp = os.path.join(a.out, "lo_%d.out" % a.seed)
    gen(sqlp, a.seed, a.n)
    env = dict(os.environ)
    if a.install:
        inst = os.path.expanduser(a.install)
        if not os.access(os.path.join(inst, "bin", "csql"), os.X_OK):
            print("no csql under %s/bin - give an install directory (e.g. ~/release/CUBRID-goto-<rev9>)" % inst); return 2
        env.update(CUBRID=inst, PATH=inst + "/bin:" + env["PATH"], LD_LIBRARY_PATH=inst + "/lib")
        env["CUBRID_DATABASES"] = os.path.expanduser(a.databases) if a.databases else inst + "/databases"
        env.pop("CUBRID_CONF_FILE", None)
    if not a.cs:
        pid = server_serves(a.db)
        if pid:
            print("cub_server %s serves %s - SA mode collides with it: stop it, or use --cs" % (pid, a.db)); return 2
    cmd = ["csql", "-u", "dba", a.db, "-i", sqlp] + ([] if a.cs else ["-S"])
    with open(outp, "w") as fo:   # csql leaves csql.err in its cwd: run it in the output directory
        subprocess.run(cmd, stdout=fo, stderr=subprocess.STDOUT, env=env, cwd=a.out, timeout=a.timeout)
    nbad = check(outp, sqlp, os.path.join(a.out, "lo_%d.violations.txt" % a.seed))
    return 2 if nbad < 0 else (1 if nbad else 0)

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen", help="write the SQL (setup + marked checks)")
    g.add_argument("sql"); g.add_argument("seed", type=int); g.add_argument("n", type=int)
    c = sub.add_parser("check", help="evaluate the invariants of a csql output")
    c.add_argument("out"); c.add_argument("sql")
    r = sub.add_parser("run", help="gen + csql + check")
    r.add_argument("--db", required=True, help="a DB to throw away: lo_r1..lo_r4 are dropped and created")
    r.add_argument("--install", help="install directory for SA mode (default: the csql on PATH)")
    r.add_argument("--databases", help="directory of the databases.txt to use with --install (default: <install>/databases)")
    r.add_argument("--cs", action="store_true", help="client-server mode against a running server")
    r.add_argument("--seed", type=int, default=1); r.add_argument("--n", type=int, default=300, help="checks (5 queries each)")
    r.add_argument("--out", required=True)
    r.add_argument("--timeout", type=int, default=3600, help="seconds for the csql run (debug builds: ~1s per check)")
    a = ap.parse_args()
    if a.cmd == "gen":
        gen(a.sql, a.seed, a.n)
    elif a.cmd == "check":
        n = check(a.out, a.sql)
        sys.exit(2 if n < 0 else (1 if n else 0))
    else:
        sys.exit(run(a))
