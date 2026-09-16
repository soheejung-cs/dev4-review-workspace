---
name: oracle-reference
description: Oracle 과 스펙·동작 비교가 **요청됐을 때만** — 매뉴얼 목차(toc.md)로 지정 항목을 매핑해 그 절만 읽고 CUBRID 와 대조한다. 매뉴얼 전체·다른 장은 읽지 않는다(토큰). "Oracle 은 이거 어떻게 동작해", "Oracle 스펙과 비교해", "표준/Oracle 호환인지".
---

# Oracle 참조 규약 (oracle-reference)

원문: **private 리포 `soheejung-cs/dev4-oracle-manual`** (로컬 `~/dev/docs/dev4-oracle-manual/<책>/`; 이 리포의 `references/oracle-manual/README.md` 는 포인터). 현재 있는 책: `sql-reference-10gR1/` (Oracle Database SQL Reference 10g R1, `b10759.pdf` 1808쪽) — `toc.md`(북마크 673항목·쪽수), `요약.md`(목차 기반 개요 + 리뷰 주제→절 매핑), `ch-*.txt`(장별 텍스트, `=== p.N ===` 쪽 구분). 새 책을 올리면 그 리포의 `tools/oracle_manual_index.py <디렉터리>` 로 같은 산출물을 만든다.
**요청이 없으면 이 스킬은 열지 않는다.** 리뷰 중 "Oracle 도 이런가?"가 궁금해도 사용자에게 한 줄로 묻고 멈춘다.

## 절차
1. **항목 확정** — 사용자가 지정한 항목을 한 줄로 되쓴다(예: "UPDATE STATISTICS 중 DML 차단 여부" → Oracle 은 `DBMS_STATS.GATHER_TABLE_STATS`, 통계 잠금은 `LOCK_TABLE_STATS`).
2. **책 선택** — 문법/의미 → `sql-language-reference` · 개념/아키텍처 → `concepts` · 통계/옵티마이저 → `tuning-guide`(SQL Tuning Guide) · 운영 → `admin-guide` · 패키지 → `pl-sql-packages`. 한 책부터.
3. **목차 매핑** — 먼저 `요약.md` 의 '리뷰 주제 → 어느 절' 표, 없으면 `grep -in '<키워드>' <책>/toc.md` 로 절과 쪽을 찍는다. 이 단계에서도 본문은 읽지 않는다.
4. **그 절만 읽기** — toc 가 준 쪽 범위로 `awk '/=== p.1565 ===/,/=== p.1569 ===/' ch-18-*.txt` 처럼 해당 쪽만 뽑는다(장 파일 통째로 Read 금지 — 7장은 290쪽이다). 상한: 한 항목에 절 3개·600줄. 넘으면 사용자에게 "더 볼 절 있음: …" 으로 보고하고 멈춘다.
5. **대조표** — `항목 | Oracle(절 인용, 책/절 번호) | CUBRID(코드·매뉴얼 위치) | 차이 | 리뷰 함의` 한 표. 인용은 절 번호로 검증 가능하게. Oracle 용어는 번역하지 않는다.
6. **기록** — 대조표는 리뷰 산출물(`claude-workspace/projects/<JIRA>/`)에, Oracle 동작에 대한 재사용 가능한 사실 한 줄은 `dev4-ai-source/reference-dbms/oracle/지식.md` 에 절 번호와 함께 추가(이름 표기, CONTRIBUTING).

## 금지
- 매뉴얼을 훑어 "관련 있을 법한" 장을 여러 개 읽는 것. 매핑이 안 되면 매핑 실패를 보고한다.
- 기억으로 Oracle 동작을 단정하는 것 — 절 인용 없으면 "매뉴얼 미확인"이라 쓴다.
- 인터넷 블로그(gurubee 등) 요약을 매뉴얼 대신 쓰는 것 — 보조 근거로만, 출처를 적는다.
