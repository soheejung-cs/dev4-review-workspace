# Oracle 매뉴얼 원문 — 위치 안내 (재배포 금지라 이 공개 리포에는 없다)

Oracle 문서는 재배포가 금지되어 공개 리포(`dev4-review-workspace`, public)에 두지 않는다(2026-09-16 분리). 원문은 아래 두 곳에 있다.

| 위치 | 접근 |
|---|---|
| **GitHub private** `soheejung-cs/dev4-oracle-manual` — https://github.com/soheejung-cs/dev4-oracle-manual | 팀원은 초대 후 clone: `gh repo clone soheejung-cs/dev4-oracle-manual ~/dev/docs/dev4-oracle-manual` |
| **로컬(.51)** `~/dev/docs/dev4-oracle-manual/sql-reference-10gR1/` | 내부망 브라우저: http://192.168.6.51:8827/docs/dev4-oracle-manual/sql-reference-10gR1/ (toc.md·요약.md·ch-*.txt 렌더됨) |

현재 있는 책: **Oracle Database SQL Reference 10g R1**(`b10759.pdf`, 1808쪽) — `toc.md`(북마크 673항목·쪽수), `요약.md`(목차 기반 개요 + 리뷰 주제→절 매핑), `ch-*.txt`(장별 텍스트, `=== p.N ===` 쪽 구분).
없는 책(리뷰에서 "매뉴얼 미확인"으로 표기되는 근거): Administrator's Guide(인덱스 COALESCE/REBUILD 판단 기준·INDEX_STATS), Concepts(인덱스 블록 split·삭제 항목 회수), Performance Tuning Guide(FK 기반 카디널리티) — 올리면 `tools/oracle_manual_index.py <디렉터리>` 로 같은 산출물을 만든다.

읽는 규약: `skills/oracle-reference` — 요청 시에만, `요약.md`/`toc.md` 로 절을 찍고 그 쪽만 읽는다. 인용은 절 번호(예: SQL Ref 10-90)로.
