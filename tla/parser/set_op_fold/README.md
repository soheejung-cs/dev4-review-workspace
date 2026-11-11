# set_op_fold — 빈 갈래가 있는 집합 연산의 접기 (문장 종류·중복 제거·남은 갈래)

- 이슈·PR: CBRD-28019 · CUBRID/cubrid#8489 (2026-11-11 구현 하네스, head `ff2e6de3b` = develop `38c931e7d` + 1커밋)
- 대상 함수: `pt_check_union_is_foldable ()`·`pt_fold_union ()`·`pt_select_takes_distinct ()`(src/parser/semantic_check.c), `pt_eval_type_pre ()`·`pt_eval_type ()`(src/parser/type_checking.c), `db_execute_and_keep_statement_local ()`(src/compat/db_vdb.c) — 결과를 만드는 쪽: `mq_set_union_query ()`(view_transform.c), `do_statement ()`·`do_select_internal ()`(execute_statement.c)
- 종류: 상태기계(동시성 없음). 하위 스킬 [`tla.md`](../../../skills/harness-review/tla.md) §1 은 파서 같은 순수 계산을 돌리지 않는다고 하지만, 사용자 요청(2026-11-11 "TLA 검증도 해 주길 원함")으로 돌렸다. 왜 모델로 보나: 접기는 타입 계산 한 번에 "갈래를 값으로 바꿀까 → 집합 연산을 어떻게 접을까 → 번역이 남은 갈래를 받나 → 실행이 무엇을 읽나" 가 이어지고, 그 결정이 집합 연산 종류·ALL·빈 갈래 위치·괄호·WITH·LIMIT·INTO·중첩·패스(컴파일 / 실행 때 다시)의 조합에 달려 있다. 재현 SQL 은 고른 조합 몇 개를, 이 모델은 범위 안의 모든 조합을 본다.
- 실행: `python3 tools/tla/tlc_run.py tla/parser/set_op_fold/SetOpFold.tla --config tla/parser/set_op_fold/<cfg> --expect <violation|ok>` — before_*·plan12_scan·plan3_into 는 `violation`, after·after_nested 는 `ok`

## 무엇을 모델로 했나
문장 하나의 타입 계산 패스 한 번과 그 실행. 초기 상태가 패스(컴파일 / 실행 때 바인드 값으로 다시 — `exec`), 집합 연산 T 의 종류·ALL·WITH, 갈래(SELECT) 마다 행(값 {1, 2} 의 백 4가지: {}, {1}, {1, 1}, {1, 1, 2}), 이 패스에서 WHERE 가 거짓인지(`pt_false_where ()`), 괄호(PT_IS_SUBQUERY), LIMIT 2(값 순서 — 같은 블록에서 DISTINCT 를 못 받는 갈래; LIMIT 없는 ORDER BY 는 행을 바꾸지 않아 받는다), WITH, 첫 갈래의 INTO 를 고른다. `Nested` 면 T 의 첫 갈래가 집합 연산 N(A, B) 다(SQL `A op B op C` 의 파스 모양). 실행 때는 맨 위 사슬의 갈래가 PT_IS_UNION_QUERY 라(mq_translate ()) 괄호 표시가 없다.

| 모델 행동 | 코드 (head `ff2e6de3b` / base `38c931e7d`) |
|---|---|
| `Mark` | PR: db_vdb.c:3607 실행 때 복사한 문장에 `flag.do_not_fold`(base 없음) · type_checking.c:7573·7578 `pt_eval_type_pre ()` 가 집합 연산의 두 갈래에 `flag.do_not_fold`(base 없음) |
| `Convert` | type_checking.c:8325 `pt_eval_type ()` PT_SELECT — 거짓 WHERE 의 부질의(PT_IS_SUBQUERY·UNION_SUBQUERY·CTE)를 `pt_to_false_subquery ()` 로 값으로. PR 은 `flag.do_not_fold` 면 건너뜀 / base :8320 |
| `Decide` (FoldN·FoldT 안) | semantic_check.c:19026 `pt_check_union_is_foldable ()` — 기본 판정(base :19025~19080), PR: 표시된 집합 연산의 AS_NULL → 빈 갈래로 접기 :19083, WITH 두 개 포기 :19100(base :19082), DISTINCT 를 그 자리에서 못 받는 갈래면 접지 않음(INTO 갈래는 예외) :19103-19104 |
| `Fold` (FoldN·FoldT 안) | semantic_check.c:19165 `pt_fold_union ()` — AS_NULL 값 노드 :19188(base :19129), AS_ARG 갈래로 접기, PR: 표시 물려받기 :19253, 남은 갈래 DISTINCT :19257(조건 :19211) |
| `Translate` | view_transform.c:9913-9914 `mq_translate_helper ()` → `mq_set_union_query ()` :803 — SELECT·집합 연산이 아닌 갈래면 `assert (0)` :821 (컴파일만; base 같음) |
| `Execute` | execute_statement.c:3607·3889 `do_statement ()` default — 값 문장은 ER_PT_UNKNOWN_STATEMENT 를 남기고 NO_ERROR(드라이버 실행이면 CAS 가 연결을 끊음) · :15414 `do_select_internal ()` 맨 위 SELECT 의 거짓 WHERE 는 실행하지 않음(base 같음) |

## 성질
- `ResultMatches` — 돌려준 행이 SQL 이 뜻하는 행(갈래는 자기 블록, 집합 연산은 백/집합 의미)과 같다. INTO 문장은 행 대신 한 행을 대입하므로 뺀다.
- `ExecStaysQuery` — 실행 때 다시 하는 타입 계산이 문장을 값으로 바꾸지 않는다(CBRD-28019).
- `NoValueOperand` — 컴파일 때 남은 집합 연산의 갈래가 값이 아니다(debug assert).
- `KindOnlyAllEmpty` — 값 문장은 develop 도 만드는 경우뿐: 컴파일 때 모든 갈래가 빈 집합(알려진 동작 [3], 이 PR 범위 밖).
- `ScanOnlyWhenKept` — 거짓 WHERE 의 갈래를 실행기가 읽는 것은 PR 이 받아들인 이유로 남긴 집합 연산 아래뿐(DISTINCT 를 못 받는 갈래, WITH 두 개).
- `ExecEmptyReadsNothing` — 실행 때 모든 갈래가 빈 문장은 표를 읽지 않는다(맨 위가 거짓 WHERE 의 SELECT). T 와 갈래가 둘 다 WITH 면 develop 처럼 접지 않아 제외.
- `IntoFoldsLikeDevelop` — develop 이 INTO 갈래로 접어 대입하던 문장은 PR 도 그 갈래로 접는다(집합 연산은 첫 갈래의 INTO 를 대입하지 않는다).

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-11 (.52) | develop(FIX=FALSE) | before_result.cfg | **ResultMatches 위반**(기대대로) — 반례 6상태: UNION(A 거짓, C {1,1,2}) 가 C 로 접혀 {1,1,2}(뜻은 {1,2}) | 222,939 / 211,848 | 6 | 39.6s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…050e88`), Java 11 |
| 2026-11-11 (.52) | develop | before_exec.cfg | **ExecStaysQuery 위반**(기대대로) — 실행 때 두 갈래가 거짓이 되어 값 문장 | 251,909 / 240,818 | 6 | 42.6s | 같음 |
| 2026-11-11 (.52) | develop | before_assert.cfg | **NoValueOperand 위반**(기대대로) — 괄호 갈래가 값이 된 뒤 WITH 두 개로 집합 연산이 남음 | 238,134 / 227,043 | 6 | 49.8s | 같음 |
| 2026-11-11 (.52) | develop, Nested | before_nested.cfg | **ResultMatches 위반**(기대대로) | 3,476,083 / 3,292,486 | 7 | 181.3s | 같음 |
| 2026-11-11 (.52) | PR 1·2차 안(KeepAtExec: 실행 때 맨 위 사슬을 접지 않음) | plan12_scan.cfg | **ExecEmptyReadsNothing 위반**(기대대로) — 빈 갈래를 읽음 | 265,394 / 258,866 | 6 | 8.1s | 같음 |
| 2026-11-11 (.52) | PR 3차 첫 판(IntoFix 없음) | plan3_into.cfg | **IntoFoldsLikeDevelop 위반**(기대대로) — INTO·LIMIT 갈래로 접지 않아 대입을 잃음 | 235,290 / 228,762 | 6 | 7.8s | 같음 |
| 2026-11-11 (.52) | PR(FIX=TRUE) | after.cfg | **모두 성립** | 316,416 / 268,416 | 6 | 9.0s | 같음 |
| 2026-11-11 (.52) | PR, Nested | after_nested.cfg | **모두 성립** | 4,890,240 / 4,242,240 | 7 | 71.2s | 같음 |

## 모델이 보지 않는 것
- 집합 연산 자신의 ORDER BY·LIMIT(접을 때 남은 갈래로 옮기거나 파생 표로 감싸는 기존 경로), CTE 의 재귀 부분, 부질의 안의 집합 연산이 값이 되는 경우(EXISTS·IN 이 빈 결과로 읽음 — CBRD-27957), dblink.
- INTO 가 받는 값(모델은 문장이 그 SELECT 인지만 본다 — 대입은 TC `cbrd_28019` Case 15·`bug_bts_12326` 이 본다).
- 결과 열 제목·타입(드라이버 실행에서는 PREPARE 때 정해져 바뀌지 않는다), 플랜 모양, XASL 캐시.
- LIMIT 외의 '같은 블록에서 DISTINCT 를 못 받는' 이유(ORDERBY_NUM·INST_NUM·CONNECT BY·분석 함수·숨은 열·값 질의) — 모델은 LIMIT 2 하나로 그 종류를 대표한다. LIMIT 없는 ORDER BY 는 행을 바꾸지 않아 모델에 없다(순서는 TC `cbrd_28019` Case 10·`bug_bts_12326` 이 본다).
- 갈래 세 개를 넘는 깊이, 오른쪽으로 중첩된 집합 연산(SQL 은 왼쪽으로 묶는다; 괄호로 오른쪽에 둔 것은 PT_IS_SUBQUERY 갈래와 같은 길).
