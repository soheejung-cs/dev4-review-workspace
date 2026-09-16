#!/usr/bin/env python3
"""Oracle 매뉴얼 PDF → toc.md(북마크 전체, 쪽수) + 장별 텍스트(ch-NN-slug.txt) + 요약.md(목차 기반 저비용 요약).
사용: oracle_manual_index.py <dir-with-pdf> [--no-text]   (PyPDF2<3, python3.6)
"""
import sys, os, re, json, PyPDF2
d = sys.argv[1]; notext = '--no-text' in sys.argv
pdf = next(f for f in os.listdir(d) if f.endswith('.pdf')); r = PyPDF2.PdfReader(os.path.join(d, pdf))
title = r.metadata.get('/Title', pdf); N = len(r.pages)
flat = []  # (depth, title, page0)
def walk(o, depth=0):
    for x in o:
        if isinstance(x, list): walk(x, depth + 1)
        else:
            try: p = r.get_destination_page_number(x)
            except Exception: p = -1
            flat.append((depth, x.title.strip(), p))
walk(r.outline)
def slug(t): return re.sub(r'[^a-z0-9]+', '-', t.lower()).strip('-')[:40]
# 장(depth 0) 경계 → 파일명
chap = [(i, t, p) for i, (dp, t, p) in enumerate(flat) if dp == 0]
files = {}
for k, (i, t, p) in enumerate(chap):
    end = chap[k + 1][2] if k + 1 < len(chap) else N
    m = re.match(r'^(\d+|[A-Z])\s+(.*)', t)
    num = (m.group(1) if m else str(k)).zfill(2) if (m and m.group(1).isdigit()) else (m.group(1) if m else 'x%02d' % k)
    files[i] = ('ch-%s-%s.txt' % (num, slug(m.group(2) if m else t)), p, end)
# toc.md
L = ['# %s — 목차 (PDF 북마크, %d쪽)' % (title, N), '', '원본 `%s`. 쪽수는 **PDF 쪽(1부터)**. 장별 텍스트 파일은 `pdftotext` 없이 PyPDF2 로 뽑았다(레이아웃 손실 있음 — 정확한 인용은 PDF 쪽으로).' % pdf,
     '읽는 법(`skills/oracle-reference`): 이 파일에서 절을 찍고 → 해당 `ch-*.txt` 에서 절 제목으로 `grep -n` → 그 절만 읽는다.', '']
cur = None
for i, (dp, t, p) in enumerate(flat):
    if dp == 0:
        cur = files[i]; L.append(''); L.append('## %s  (p.%d–%d) → `%s`' % (t, p + 1, cur[2], cur[0]))
    else: L.append('%s- %s (p.%d)' % ('  ' * (dp - 1), t, p + 1))
open(os.path.join(d, 'toc.md'), 'w', encoding='utf-8').write('\n'.join(L) + '\n')
json.dump([{'depth': dp, 'title': t, 'page': p + 1} for dp, t, p in flat], open(os.path.join(d, 'toc.json'), 'w'), ensure_ascii=False)
# 요약.md — 목차만으로 만든 장 개요 + 리뷰 주제 매핑
topics = {  # CUBRID 리뷰 주제 → 목차에서 찍을 키워드
 '통계 수집·잠금 (UPDATE STATISTICS 대응)': ['ANALYZE', 'ASSOCIATE STATISTICS', 'DISASSOCIATE STATISTICS', 'DBMS_STATS'],
 '락·격리 수준': ['LOCK TABLE', 'FOR UPDATE', 'SET TRANSACTION', 'ISOLATION'],
 '트랜잭션 제어·DDL 커밋': ['COMMIT', 'ROLLBACK', 'SAVEPOINT', 'SET CONSTRAINT'],
 '인덱스': ['CREATE INDEX', 'ALTER INDEX', 'DROP INDEX', 'Bitmap', 'Function-Based'],
 '테이블·파티션 DDL': ['CREATE TABLE', 'ALTER TABLE', 'partition', 'PARTITION'],
 '뷰·MERGE·UPSERT': ['CREATE VIEW', 'MERGE', 'INSERT'],
 '조인·서브쿼리·계층 질의': ['Joins', 'Subquer', 'Hierarchical', 'CONNECT BY', 'WITH'],
 '집계·분석(윈도우) 함수': ['Aggregate Functions', 'Analytic Functions', 'OVER'],
 '정규식·문자열 함수': ['REGEXP', 'Regular Expression', 'Character Functions'],
 '데이터 타입·형변환·NULL': ['Datatypes', 'Data Conversion', 'Nulls', 'Literals'],
 '시퀀스·의사열': ['CREATE SEQUENCE', 'Pseudocolumns', 'ROWNUM', 'ROWID'],
 '표준 SQL 준수': ['Standard SQL', 'SQL Standards', 'Conformance'],
 '힌트(옵티마이저)': ['Hints', 'hint'],
 '트리거·PL/SQL 호출': ['CREATE TRIGGER', 'CREATE PROCEDURE', 'CREATE FUNCTION', 'CALL'],
 '예약어': ['Reserved Words'],
}
S = ['# %s — 저비용 요약 (목차 기반)' % title, '', '본문을 읽지 않고 **북마크만으로** 만든 개요다. 장의 성격은 제목으로 판단했고, 절 수는 북마크 수다. 실제 내용 인용은 `toc.md` → 해당 장 텍스트에서 그 절만 읽어 한다.', '',
     '## 장 개요', '', '| 장 | 쪽 | 절 수 | 파일 | 무엇 |', '|---|---|---|---|---|']
kind = lambda t: ('구문·의미 정의' if re.search(r'Statements|Clauses|Queries|Elements|Expressions|Conditions|Operators|Pseudocolumns|Functions', t) else '개요·부록')
for k, (i, t, p) in enumerate(chap):
    end = chap[k + 1][0] if k + 1 < len(chap) else len(flat)
    S.append('| %s | %d–%d | %d | `%s` | %s |' % (t, p + 1, files[i][2], end - i - 1, files[i][0], kind(t)))
S += ['', '## 리뷰 주제 → 어느 절을 여나 (키워드 매핑, 자동)', '', '| 주제 | 매핑된 절(쪽) |', '|---|---|']
for topic, kws in topics.items():
    hits = []
    for dp, t, p in flat:
        if any(k.lower() in t.lower() for k in kws) and dp >= 1 and len(hits) < 8: hits.append('%s(p.%d)' % (t, p + 1))
    S.append('| %s | %s |' % (topic, '; '.join(hits) or '— 목차에 없음(다른 책: Concepts/Tuning Guide)'))
S += ['', '## 이 책이 다루지 않는 것', '', '내부 동작(락 구현, MVCC/undo, 옵티마이저 비용 모델, 통계 수집 내부)은 *Concepts*·*Performance Tuning Guide*·*PL/SQL Packages(DBMS_STATS)* 에 있다. 이 책은 **문법과 의미(semantics)** 이므로 "Oracle 도 이 문법을 허용하나/의미가 같나" 비교에 쓴다.']
open(os.path.join(d, '요약.md'), 'w', encoding='utf-8').write('\n'.join(S) + '\n')
print('toc entries', len(flat), 'chapters', len(chap))
if notext: sys.exit()
for i, (fn, p, end) in files.items():
    with open(os.path.join(d, fn), 'w', encoding='utf-8') as f:
        f.write('# %s  (PDF p.%d–%d)\n' % (flat[i][1], p + 1, end))
        for pg in range(p, end):
            try: txt = r.pages[pg].extract_text() or ''
            except Exception as e: txt = '[extract error p.%d: %s]' % (pg + 1, e)
            f.write('\n\n=== p.%d ===\n' % (pg + 1)); f.write(txt)
    print('wrote', fn, p + 1, end, flush=True)
print('DONE')
