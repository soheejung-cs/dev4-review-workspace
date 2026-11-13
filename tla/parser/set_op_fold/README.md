# set_op_fold — 빈 갈래가 있는 집합 연산의 접기 (문장 종류·중복 제거·남은 갈래)

- 이슈·PR: CBRD-28019 · CUBRID/cubrid#8489 (2026-11-11 구현 하네스, head `a88512630` = develop `38c931e7d` + 2커밋 + develop `6a54287e7` merge + 셋째 커밋 — 둘째 커밋은 자기 리뷰(1·2회차)가 찾은 표시 시점 구멍·ORDER BY 숨은 열·INTO/FOR UPDATE 갈래를, 셋째 커밋은 3회차가 찾은 집합 연산 ORDER BY·LIMIT 의 세 구멍(뷰 정의의 이름 키, 남긴 집합 연산의 select list 밖 키, GROUP BY 갈래로 가는 LIMIT)을 막음) · **넷째 커밋 `f5b50073f`(2026-11-13, 사용자 결정 Q1 (b)): 설계를 바꿈 — 남은 갈래를 파생 표로 감싼 SELECT DISTINCT, 갈래 표시 없음(실행 때 문장의 집합 연산만), 모델 상수 `Derived`**
- 대상 함수: `pt_check_union_is_foldable ()`·`pt_fold_union ()`·`pt_select_takes_distinct ()`·`pt_folded_operand_takes_distinct ()`·`pt_folds_without_distinct ()`·`pt_orderby_is_positions ()`·`pt_orderby_matches_select_list ()`·`pt_mark_union_leaf_nodes ()`·`pt_check_order_by ()`(src/parser/semantic_check.c), `pt_eval_type_pre ()`·`pt_eval_type ()`(src/parser/type_checking.c), `db_execute_and_keep_statement_local ()`(src/compat/db_vdb.c) — 결과를 만드는 쪽: `mq_set_union_query ()`(view_transform.c), `do_statement ()`·`do_select_internal ()`(execute_statement.c) · 넷째 커밋: `pt_fold_union_as_derived ()`·`pt_fold_union_needs_derived ()`·`pt_set_operation_select_list ()`·`pt_fold_union_collation ()`·`pt_check_create_entity ()`(semantic_check.c), `db_mark_set_operations_stay_queries ()`(db_vdb.c) — `pt_select_takes_distinct ()`·`pt_folded_operand_takes_distinct ()`·`pt_orderby_is_positions ()` 와 갈래 표시(`pt_mark_union_leaf_nodes ()`·`pt_eval_type_pre ()`·`pt_eval_type ()`)는 지움
- 종류: 상태기계(동시성 없음). 하위 스킬 [`tla.md`](../../../skills/harness-review/tla.md) §1 은 파서 같은 순수 계산을 돌리지 않는다고 하지만, 사용자 요청(2026-11-11 "TLA 검증도 해 주길 원함")으로 돌렸다. 왜 모델로 보나: 접기는 타입 계산 한 번에 "갈래를 값으로 바꿀까 → 집합 연산을 어떻게 접을까 → 번역이 남은 갈래를 받나 → 실행이 무엇을 읽나" 가 이어지고, 그 결정이 집합 연산 종류·ALL·빈 갈래 위치·괄호·WITH·LIMIT·INTO·중첩·패스(컴파일 / 실행 때 다시)의 조합에 달려 있다. 재현 SQL 은 고른 조합 몇 개를, 이 모델은 범위 안의 모든 조합을 본다.
- 실행: `python3 tools/tla/tlc_run.py tla/parser/set_op_fold/SetOpFold.tla --config tla/parser/set_op_fold/<cfg> --expect <violation|ok>` — before_*·plan12_scan·plan3_into·pr_mark_late*·r2_* 는 `violation`, guard_only·after·after_nested·after_clause 는 `ok`

## 무엇을 모델로 했나
문장 하나의 타입 계산 패스 한 번과 그 실행. 초기 상태가 패스(컴파일 / 실행 때 바인드 값으로 다시 — `exec`), 집합 연산 T 의 종류·ALL·WITH, 갈래(SELECT) 마다 행(값 {1, 2} 의 백 4가지: {}, {1}, {1, 1}, {1, 1, 2}), 이 패스에서 WHERE 가 거짓인지(`pt_false_where ()`), 괄호(PT_IS_SUBQUERY), LIMIT 2(값 순서 — 같은 블록에서 DISTINCT 를 못 받는 갈래; LIMIT 없는 ORDER BY 는 행을 바꾸지 않아 받는다), WITH, 첫 갈래의 INTO 를 고른다. `Nested` 면 T 의 첫 갈래가 집합 연산 N(A, B) 다(SQL `A op B op C` 의 파스 모양). 실행 때는 맨 위 사슬의 갈래가 PT_IS_UNION_QUERY 라(mq_translate ()) 괄호 표시가 없다.
평평한 트리는 T 자신의 절(`TClauseChoice`)도 고른다: ORDER BY 없는 LIMIT 2, 또는 LIMIT 없는 ORDER BY 의 키가 위치(`pos`)·select list 의 이름(`in`)·select list 밖의 이름(`out`). 접으면 절이 남은 갈래로 옮겨지고, `pt_check_order_by ()` 가 키를 select list 에 맞춘다 — 밖 키는 숨은 열이 되고, `CREATE VIEW` 의 검사(`View`, do_not_replace_orderby)에서는 이름으로 남는다. 옮긴 LIMIT 은 타입 계산에서 GROUP BY 갈래(`GroupChoice`)면 HAVING 의 GROUPBY_NUM () 이 되어 DISTINCT 보다 먼저, 아니면 ORDERBY_NUM () 이 되어 DISTINCT 뒤에 걸린다. 뷰는 접힌 질의를 저장하고, 그 뷰를 읽을 때마다 다시 컴파일한다(모델은 그 결과를 한 패스로 본다). `PosFix`·`OrderKeepFix`·`GroupLimitFix` 가 셋째 커밋이다.

`Derived`(넷째 커밋): 갈래를 표시하지 않는다 — 실행 때 바인드 값으로 다시 하는 타입 계산에서만 문장의 집합 연산(T, N)에 표시가 붙어(`db_mark_set_operations_stay_queries ()`) 모든 갈래가 빈 집합 연산이 값 대신 빈 갈래로 접힌다. 컴파일 때 괄호 갈래는 develop 처럼 값이 된다(`Convert`). ALL 없는 UNION·EXCEPT 가 비지 않은 질의 갈래로 접히면 그 갈래를 파생 표로 감싼 SELECT DISTINCT 가 된다 — 갈래 블록(LIMIT 2·GROUP BY)은 그대로, T 의 절은 DISTINCT 위(`LeafRun` 의 derived 갈래). 감싸기는 갈래에 LIMIT 이 있고 T 에 절이 있거나(develop 의 기존 감싸기) 둘 다 WITH 일 때도 한다(`NeedsDerived`). 그때 T 의 정렬 키가 select list 밖(`out`)이면, 또는 WITH 두 개 옆의 갈래에 INTO 가 있으면 접지 않는다(`NOTHING:derived` — 집합 연산이 그 키를 거절하거나 develop 처럼 남음). 남은 갈래가 INTO 면 감싸지 않고 develop 처럼 접는다. develop 의 감싸기는 `out` 키에서 Internal error 로 실패한다(`dev.error`).

| 모델 행동 | 코드 (head `a88512630` / base `38c931e7d`) |
|---|---|
| `Mark` | PR: db_vdb.c:3612 실행 때 복사한 문장에 `info.query.flag.stays_query`(base 없음) · 갈래 표시 — `EarlyMark`: semantic_check.c:18799·18809 `pt_mark_union_leaf_nodes ()`(의미 검사 전의 전위 걸음) + type_checking.c:7541·7545 `pt_eval_type_pre ()`(donot_fold 조기 반환 앞). 첫 커밋 `ff2e6de3b` 은 타입 계산의 pt_eval_type_pre 에서만(`flag.do_not_fold`) 달아, 의미 검사(pt_semantic_check_local 의 접기·괄호 갈래의 자기 타입 계산)가 먼저 접은 갈래는 표시 없이 값이 됐다(`EarlyMark = FALSE`) |
| `Convert` | type_checking.c:8331·8336 `pt_eval_type ()` PT_SELECT — 거짓 WHERE 의 부질의(PT_IS_SUBQUERY·UNION_SUBQUERY·CTE)를 `pt_to_false_subquery ()` 로 값으로. PR 은 `stays_query` 면 건너뜀 / base :8320 |
| `Decide` (FoldN·FoldT 안) | semantic_check.c:19089 `pt_check_union_is_foldable ()` — 기본 판정(base :19025~19080), PR: 표시된 집합 연산의 AS_NULL → 질의인 빈 갈래로 접기 :19146(`ValueGuard`), WITH 두 개 포기 :19169-19173(base :19082), DISTINCT 를 그 자리에서 못 받는 갈래면 접지 않음 :19175-19186 — 남은 갈래의 판정은 옮겨 갈 LIMIT 까지 본다(:19252 `pt_folded_operand_takes_distinct ()` → :19208 `pt_select_takes_distinct ()`; `GroupLimitFix`), 어느 갈래든 INTO·FOR UPDATE 면 예외(:19275 `pt_folds_without_distinct ()`), 빈 갈래가 이미 값이면 develop 처럼 접는다(`ValueGuard`), 집합 연산의 ORDER BY 키가 select list 밖이면 develop 처럼 접는다(:19319 `pt_orderby_matches_select_list ()`; `OrderKeepFix`) |
| `Fold` (FoldN·FoldT 안) | semantic_check.c:19371 `pt_fold_union ()` — AS_NULL 값 노드 :19393(base :19129), AS_ARG 갈래로 접기(T 의 ORDER BY·LIMIT 을 남은 갈래로 옮김), PR: 표시 물려받기 :19468, 남은 갈래 DISTINCT :19502(조건 :19423 keep_distinct, 옮긴 ORDER BY 를 :19492 `pt_check_order_by ()` 로 맞춘 뒤 키가 모두 위치이고(:19291 `pt_orderby_is_positions ()`; `PosFix`) 숨은 열이 없을 때 — :19498). `pt_check_order_by ()`(:15287)는 select list 밖 키를 숨은 열로 만들고, CREATE VIEW 의 검사(:8257 do_not_replace_orderby)에서는 이름으로 둔다(:15557), 집합 연산이면 거절한다(:15545) |
| `Translate` | view_transform.c:814-815 `mq_set_union_query ()` :801 — SELECT·집합 연산이 아닌 갈래면 `assert (0)` :821 (컴파일만; base 같음) |
| `Execute` | execute_statement.c:3607·3889 `do_statement ()` default — 값 문장은 ER_PT_UNKNOWN_STATEMENT 를 남기고 NO_ERROR(드라이버 실행이면 CAS 가 연결을 끊음) · :15412 `do_select_internal ()` 맨 위 SELECT 의 거짓 WHERE 는 실행하지 않음(base 같음). 옮긴 LIMIT 은 다음 타입 계산에서 type_checking.c:7650-7653 GROUP BY 블록이면 HAVING 의 GROUPBY_NUM ()(DISTINCT 앞), :7655-7659 DISTINCT 블록이면 ORDERBY_NUM ()(DISTINCT 뒤). 남은 집합 연산의 select list 밖 정렬 키(:15545)와 뷰가 저장한 SELECT DISTINCT 의 이름 정렬 키는 컴파일 오류 |

## 성질
- `ResultMatches` — 돌려준 행이 SQL 이 뜻하는 행(갈래는 자기 블록, 집합 연산은 백/집합 의미, T 의 LIMIT 은 그 결과 위)과 같다. INTO 문장은 행 대신 한 행을 대입하므로 뺀다.
- `ExecStaysQuery` — 실행 때 다시 하는 타입 계산이 문장을 값으로 바꾸지 않는다(CBRD-28019).
- `NoValueOperand` — 컴파일 때 남은 집합 연산의 갈래가 값이 아니다(debug assert). `Derived` 는 WITH 두 개 옆의 INTO 갈래를 develop 처럼 남기므로 그 문장만 뺀다(`IntoWithKept`, 받아들인 예외). T 의 정렬 키가 select list 밖인 집합 연산은 번역 전에 `pt_check_order_by ()` 가 거절해 이 검사에 닿지 않는다(`Translate`).
- `KindOnlyAllEmpty` — 값 문장은 develop 도 만드는 경우뿐: 컴파일 때 모든 갈래가 빈 집합(알려진 동작 [3], 이 PR 범위 밖).
- `ScanOnlyWhenKept` — 거짓 WHERE 의 갈래를 실행기가 읽는 것은 PR 이 받아들인 이유로 남긴 집합 연산 아래뿐(DISTINCT 를 못 받는 갈래, WITH 두 개).
- `ExecEmptyReadsNothing` — 실행 때 모든 갈래가 빈 문장은 표를 읽지 않는다(맨 위가 거짓 WHERE 의 SELECT). T 와 갈래가 둘 다 WITH 면 develop 처럼 접지 않아 제외.
- `IntoFoldsLikeDevelop` — develop 이 INTO 갈래로 접어 대입하던 문장은 PR 도 그 갈래로 접는다(집합 연산은 첫 갈래의 INTO 를 대입하지 않는다). develop 의 파생 표가 실패하던 문장(`dev.error`)은 뺀다.
- `NoNewOrderError` — ORDER BY 때문에 실패하는 문장(남긴 집합 연산의 select list 밖 키, 뷰가 저장한 SELECT DISTINCT 의 이름 키)은 develop 도 접지 않아 실패하던 것, 또는 develop 의 파생 표가 그 키에서 Internal error 로 실패하던 것(`dev.error`, 11-13 넷째 커밋과 함께 더함)뿐.
- `OutFoldsLikeDevelop` — select list 밖의 열로 정렬하는 집합 연산(접히지 않으면 오류)을 develop 이 갈래로 접으면, PR 도 같은 행(그 갈래, DISTINCT 없이)을 낸다. `ResultMatches` 는 이 문장을 뺀다.

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-11 (.52) | develop(FIX=FALSE) | before_result.cfg | **ResultMatches 위반**(기대대로) — 반례 6상태: UNION(A 거짓, C {1,1,2}) 가 C 로 접혀 {1,1,2}(뜻은 {1,2}) | 222,949 / 211,858 | 6 | 39.6s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…050e88`), Java 11 |
| 2026-11-11 (.52) | develop | before_exec.cfg | **ExecStaysQuery 위반**(기대대로) — 실행 때 두 갈래가 거짓이 되어 값 문장 | 251,960 / 240,869 | 6 | 42.6s | 같음 |
| 2026-11-11 (.52) | develop | before_assert.cfg | **NoValueOperand 위반**(기대대로) — 괄호 갈래가 값이 된 뒤 WITH 두 개로 집합 연산이 남음 | 238,142 / 227,051 | 6 | 49.8s | 같음 |
| 2026-11-11 (.52) | develop, Nested | before_nested.cfg | **ResultMatches 위반**(기대대로) | 3,476,104 / 3,292,507 | 7 | 181.3s | 같음 |
| 2026-11-11 (.52) | PR 1·2차 안(KeepAtExec: 실행 때 맨 위 사슬을 접지 않음) | plan12_scan.cfg | **ExecEmptyReadsNothing 위반**(기대대로) — 빈 갈래를 읽음 | 265,443 / 258,915 | 6 | 8.1s | 같음 |
| 2026-11-11 (.52) | PR 3차 첫 판(IntoFix 없음) | plan3_into.cfg | **IntoFoldsLikeDevelop 위반**(기대대로) — INTO·LIMIT 갈래로 접지 않아 대입을 잃음 | 235,294 / 228,766 | 6 | 7.8s | 같음 |
| 2026-11-11 (.52) | PR 첫 커밋 `ff2e6de3b`(EarlyMark·ValueGuard 없음), Nested | pr_mark_late.cfg | **NoValueOperand 위반**(기대대로, 자기 리뷰 G2-01) — 갈래가 모두 빈 중첩 집합 연산이 표시 전에 값이 되고, T 가 DISTINCT 때문에 남아 값 갈래 | 3,502,955 / 3,335,126 | 7 | — | 같음 |
| 2026-11-11 (.52) | 같음, 평평한 트리 | pr_mark_late_flat.cfg | **NoValueOperand 위반**(기대대로, G1-01) — 괄호 갈래가 의미 검사 때 이미 거짓(리터럴 FALSE)이라 표시 전에 값 | 224,725 / 215,266 | 6 | — | 같음 |
| 2026-11-11 (.52) | ValueGuard 만, Nested | guard_only.cfg | NoValueOperand·ExecStaysQuery **성립**(기대대로 — 방어만으로도 오류는 없지만, develop 처럼 DISTINCT 를 잃는 접기로 돌아간다) | 4,462,173 / 3,814,173 | 7 | 43.5s | 같음 |
| 2026-11-11 (.52) | PR(FIX·EarlyMark·ValueGuard) | after.cfg | **모두 성립** | 316,416 / 268,416 | 6 | 7.0s | 같음 |
| 2026-11-11 (.52) | PR, Nested | after_nested.cfg | **모두 성립** | 4,890,240 / 4,242,240 | 7 | 43.8s | 같음 |
| 2026-11-12 (.52) | PR 둘째 커밋 `a19b637a9`(`PosFix` 없음), 뷰 정의 | r2_view_order.cfg | **NoNewOrderError 위반**(기대대로, 3회차 R3-01) — UNION(A, C 거짓) ORDER BY 밖 키의 뷰가 A 로 접히며 DISTINCT 를 받아 `SELECT DISTINCT … ORDER BY b` 로 저장 | 185,488 / 178,960 | 6 | 9.6s | 같음 |
| 2026-11-12 (.52) | 둘째 커밋(`OrderKeepFix` 없음) | r2_keep_order.cfg | **NoNewOrderError 위반**(기대대로, R3-02) — DISTINCT 를 못 받는 갈래(LIMIT) 때문에 남긴 집합 연산이 select list 밖 정렬 키를 거절(develop 은 접어서 행) | 185,588 / 179,060 | 6 | 44.9s | 같음 |
| 2026-11-12 (.52) | 둘째 커밋(`GroupLimitFix` 없음) | r2_group_limit.cfg | **ResultMatches 위반**(기대대로, R3-04) — GROUP BY 갈래 C {1,1,2} 로 접히며 옮긴 LIMIT 2 가 DISTINCT 보다 먼저 걸려 {1}(뜻은 {1,2}) | 965,851 / 939,739 | 6 | 105.2s | 같음 |
| 2026-11-12 (.52) | PR 셋째 커밋 `a88512630`, 평평한 트리 + T 의 절(LIMIT·ORDER BY 위치/이름/밖 이름)·뷰 정의·GROUP BY 갈래 | after_clause.cfg | **모두 성립**(NoNewOrderError·OutFoldsLikeDevelop 포함) | 10,974,720 / 9,285,120 | 6 | 142.0s | 같음 |
| 2026-11-13 (.52) | 넷째 커밋 `f5b50073f`(`Derived`, 앞 커밋의 고침 상수는 모두 끔), 평평한 트리 + T 의 절·뷰 정의·GROUP BY·WITH·LIMIT | after_derived.cfg | **모두 성립**(NoNewOrderError·OutFoldsLikeDevelop 포함) | 10,345,455 / 8,655,855 | 6 | 86.7s | 같음 |
| 2026-11-13 (.52) | 넷째 커밋, Nested | after_derived_nested.cfg | **모두 성립** — 첫 실행의 ResultMatches 반례는 모델 실수(T 가 N 이 감싼 갈래로 접히며 감싼 표시를 덮어씀; 코드는 감싼 SELECT 가 DISTINCT 라 다시 감싸지 않음)라 표시를 누적하게 고쳐 다시 돌림 | 4,462,173 / 3,814,173 | 7 | 49.3s | 같음 |

11-12 에 상수(`TClauseChoice`·`ViewChoice`·`GroupChoice`·`PosFix`·`OrderKeepFix`·`GroupLimitFix`)를 더한 뒤 위의 11-11 cfg 11개를 다시 돌려 모두 같은 판정(기존 cfg 는 절 없음·뷰 아님·GROUP BY 없음 — 상태 수 같음 또는 ±10 안).

11-13 에 `Derived`(기존 cfg 는 FALSE)·`dev.error` 를 더한 뒤 기존 cfg 15개를 다시 돌려 14개는 같은 판정이다. **r2_keep_order 는 위반 → 성립으로 바뀐다**: 그 반례(LIMIT 갈래 때문에 남긴 집합 연산이 select list 밖 키를 거절)는 develop 도 같은 문장을 파생 표로 접다 Internal error 로 실패하므로(`dev.error`) 새 오류가 아니다. 셋째 회차 R3-02 가 가리킨 실제 꼴은 분석 함수 갈래(develop 은 감싸지 않고 접어 행을 냄 — TC Case 24)인데, 모델은 'DISTINCT 를 같은 블록에서 못 받는 갈래' 를 LIMIT 2 하나로 대표해 그 차이를 보지 못한다(아래 '보지 않는 것').

## 모델이 보지 않는 것
- (넷째 커밋) 콜레이션·열 이름·`CREATE TABLE … AS WITH …`(해제된 노드를 가리키던 결함) — 값은 {1, 2} 의 백이라 콜레이션 차이가 없고, 열 이름은 모델 밖이다. TC `cbrd_28019` Case 27(콜레이션)·28(열 이름)·14·32(CTAS)가 본다. 실행 때 표시가 문장의 집합 연산 전부에 붙는 것이 필요한 꼴(T 의 두 갈래가 모두 집합 연산)은 모델의 모양(T 의 둘째 갈래는 SELECT)에 없다 — TC Case 31 이 본다.
- (넷째 커밋 자기 리뷰 4회차, 11-13 — 같은 커밋에 고침) 모델 밖에서 나온 결함 넷: ① 이미 값이 된 빈 첫 갈래의 콜레이션을 집합 연산이 남긴 select list 사본(타입 없음)으로 읽어 행을 잃음 — 콜레이션은 모델 밖(Case 33), ② 번역 뒤 패스에서 감싼 SELECT 에 타입이 없어 부질의를 읽는 식이 실패 — 타입은 모델 밖(Case 34), ③ 첫 SELECT 의 숨은 열로 정렬하는 집합 연산이 값 갈래를 가진 채 남음(debug assert) — 숨은 열 키는 모델의 `TClauseChoice` 에 없음(Case 35), ④ 상관 EXISTS·IN 부질의에서 감싼 파생 표가 semi join 을 막아 develop 보다 9~12배 느림 — 성능·부질의 문맥은 모델 밖(Case 36, 술어가 갈래를 되돌려 받음). 위 '아직 보지 않은 곳' 원칙대로, 다음에 이 자리를 고치면 숨은 열 키를 `TClauseChoice` 에 넣는다.
- 집합 연산 자신의 ORDER BY 와 LIMIT 이 함께 있는 경우(값 순서가 아닌 정렬 키로 LIMIT 이 행을 고른다), 식으로 된 정렬 키, 중첩 트리의 절(T 의 절은 평평한 트리에서만), CTE 의 재귀 부분, 부질의 안의 집합 연산이 값이 되는 경우(EXISTS·IN 이 빈 결과로 읽음 — CBRD-27957), dblink.
- INTO 가 받는 값(모델은 문장이 그 SELECT 인지만 본다 — 대입은 TC `cbrd_28019` Case 15·`bug_bts_12326` 이 본다).
- 결과 열 제목·타입(드라이버 실행에서는 PREPARE 때 정해져 바뀌지 않는다), 플랜 모양, XASL 캐시.
- LIMIT 외의 '같은 블록에서 DISTINCT 를 못 받는' 이유(ORDERBY_NUM·INST_NUM·CONNECT BY·분석 함수·숨은 열·값 질의) — 모델은 LIMIT 2 하나로 그 종류를 대표한다. LIMIT 없는 ORDER BY 는 행을 바꾸지 않아 모델에 없다(순서는 TC `cbrd_28019` Case 10·`bug_bts_12326` 이 본다).
- 갈래 세 개를 넘는 깊이, 오른쪽으로 중첩된 집합 연산(SQL 은 왼쪽으로 묶는다; 괄호로 오른쪽에 둔 것은 PT_IS_SUBQUERY 갈래와 같은 길).
- 패스의 순서는 `Mark` 한 번으로 줄였다. `EarlyMark = FALSE` 는 컴파일의 의미 검사가 표시 전에 접고 바꾸는 최악의 경우다(갈래가 그때 이미 거짓 — 리터럴 FALSE, 또는 의미 검사가 먼저 접는 중첩 집합 연산). `1 = 0` 처럼 타입 계산 뒤에야 거짓이 되는 갈래는 첫 커밋에서도 표시가 앞섰다(TC 가 지킨 경우이고, 이 모델 첫 판이 가정한 순서). 첫 판은 그 가정이 실제 패스 순서와 다를 수 있음을 보지 않았고, 자기 리뷰(G2-01·G1-01)가 찾았다.
- FOR UPDATE 갈래(INTO 와 같은 예외, `pt_folds_without_distinct ()`), `pt_select_takes_distinct ()` 가 번호 함수를 찾는 자리(select list·WHERE·HAVING·orderby_for)와 갈래 자신의 ORDER BY 가 위치로 맞춰졌는지(파생 표·CTE 는 이름 해석 때 그 전에 접힐 수 있다 — 2회차 G2-R2-01, 뷰 정의는 밖 키를 이름으로 둔다) — 모델은 '복잡함' 하나로 둔다. 집합 연산 자신의 ORDER BY 는 `TClauseChoice` 로 본다.
- 2회차까지의 모델은 집합 연산 자신의 ORDER BY·LIMIT 을 이 목록에 두었고, 3회차 자기 리뷰의 결함 셋(R3-01·02·04)이 모두 그 자리에서 나왔다. 이 목록은 '안전하다고 본 곳' 이 아니라 '아직 보지 않은 곳' 이다 — 고친 코드가 그 목록의 항목을 읽거나 옮기면(여기서는 `pt_fold_union ()` 이 T 의 ORDER BY·LIMIT 을 옮기는 자리에 DISTINCT 를 더함) 그 항목을 모델에 넣는다.
