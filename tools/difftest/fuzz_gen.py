#!/usr/bin/env python3
# 무작위 바인드 퍼저 생성기 — 문장 모양(T) × 값 풀(G) 로 세션별 PREPARE/EXECUTE/DEALLOCATE 스크립트를 만든다.
#   python3 fuzz_gen.py <seed> <세션수> <세션당 문장수> <출력접두> [shapes.txt]
# shapes.txt 를 주면 한 줄에 문장 하나(? 개수는 자동 계산)로 T 를 바꾼다. 기본 T 는 2026-10-07 PR#8022 때 쓴 15개(t1/t2 스키마 기준).
# 같은 시드면 같은 스크립트 — 동시 실행 출력과 순차 단독 출력을 그대로 비교할 수 있다(세션 간 간섭 검사).
import random, sys
seed=int(sys.argv[1]); nsess=int(sys.argv[2]); nstmt=int(sys.argv[3]); out=sys.argv[4]
shapes=sys.argv[5] if len(sys.argv)>5 else None
random.seed(seed)
G=["1","0","-1","1.5","-0.0","1e3","1e308","1e-308","2147483647","2147483648","-2147483649","9223372036854775807","9223372036854775808",
   "'1'","'1e3'","' 12 '","'0x1F'","'1,000'","'1.5.5'","'--1'","'+5'","''","'   '","'NaN'","'inf'","'abc'","'2024-01-05'","'2024-13-45'","'2024-01-05 10:00:00'",
   "'10:00:00'","'2024-01-05T10:00:00Z'","'{\"k\":1}'","'[1,2]'","'{1,2}'","'x'","'alpha'","'ALPHA'","'일이삼'","'\\\\'","'%'","'_'","'a''b'",
   "NULL","DATE'2024-01-05'","TIMESTAMP'2024-01-05 10:00:00'","TIME'10:00:00'","DATETIME'2024-01-05 10:00:00.123'","CAST(1 AS BIGINT)","12.345678901234567890","'"+"z"*300+"'",
   "B'1010'","{1,2}","{'a'}","TRUE","FALSE"]
T=[("SELECT id, a + ?, NVL(v, ?), CASE WHEN a > ? THEN ? ELSE ? END FROM t1 ORDER BY id",5),
   ("SELECT id FROM t1 WHERE a IN (?, ?, ?) OR v IN (?, ?) ORDER BY id",5),
   ("SELECT NVL(g2, ?) k, SUM(a * ?), COUNT(*), MAX(NVL(?, v)) FROM t1 GROUP BY NVL(g2, ?) ORDER BY 1",4),
   ("SELECT id, CASE WHEN a > ? THEN a ELSE ? END k FROM t1 ORDER BY k, id",2),
   ("SELECT id, NULLIF(a, ?), COALESCE(?, ?, a), NVL2(?, ?, ?), DECODE(g, ?, ?, ?) FROM t1 ORDER BY id",9),
   ("SELECT t1.id, t2.id, t1.a + t2.val * ? FROM t1 JOIN t2 ON NVL(t1.a, ?) = NVL(t2.rf, ?) * ? ORDER BY 1, 2",4),
   ("SELECT id FROM t1 WHERE id BETWEEN ? AND ? AND (v LIKE ? OR n > ?) ORDER BY id",4),
   ("SELECT CAST(? AS INT), CAST(? AS NUMERIC(10,3)), CAST(? AS DATE), CAST(? AS VARCHAR(5)), ? + 1, ? || 'x', ? IS NULL FROM db_root",7),
   ("SELECT id, dt + ?, ts - ?, SUBSTR(v, ?, ?), ROUND(n, ?), a / NVL(?, 1) FROM t1 WHERE id IN (1, 3, 5) ORDER BY id",6),
   ("SELECT ? x FROM t1 WHERE id = 1 UNION ALL SELECT ? FROM t1 WHERE id = 2 UNION ALL SELECT a FROM t1 WHERE id = 3 ORDER BY 1",2),
   ("SELECT id, ROW_NUMBER() OVER (ORDER BY NVL(?, a), id), SUM(NVL(a, ?)) OVER (PARTITION BY NVL(g2, ?) ORDER BY id) FROM t1 ORDER BY id",3),
   ("SELECT id FROM t1 WHERE CASE WHEN a > ? THEN 1 / ? ELSE 1 END = 1 AND (a IS NULL OR a * ? > ?) ORDER BY id",4),
   ("SELECT g, MEDIAN(NVL(n, ?)), GROUP_CONCAT(NVL(v, ?)), AVG(CASE WHEN a > ? THEN ? ELSE ? END) FROM t1 GROUP BY g HAVING COUNT(*) > ? ORDER BY g",6),
   ("SELECT id, e = ?, NVL(e, ?), j, JSON_EXTRACT(j, ?), ? IN st FROM t1 WHERE id IN (1, 2, 5) ORDER BY id",4),
   ("SELECT id FROM t1 ORDER BY NVL(?, a) DESC, v || ? LIMIT ?",3)]
if shapes:
    T=[(l.strip(), l.count("?")) for l in open(shapes) if l.strip() and not l.startswith("--")]
for s in range(nsess):
    lines=[]
    for i in range(nstmt):
        sql,n=random.choice(T); name=f"f{s}_{i}"
        binds=', '.join(random.choice(G) for _ in range(n))
        lines.append(f"PREPARE {name} FROM '{sql.replace(chr(39), chr(39)*2)}'; EXECUTE {name} USING {binds};")
        if random.random()<0.5:
            binds2=', '.join(random.choice(G) for _ in range(n)); lines.append(f"EXECUTE {name} USING {binds2};")
        if random.random()<0.8: lines.append(f"DEALLOCATE PREPARE {name};")
    open(f"{out}_S{s}.sql","w").write("\n".join(lines)+"\n")
print("ok")
