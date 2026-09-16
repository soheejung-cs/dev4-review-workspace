# PR #7767 코드 리뷰 보고서

**PR:** [CUBRID/cubrid#7767](https://github.com/CUBRID/cubrid/pull/7767)
**제목:** [CBRD-27292] Keep parallelism for constant-output scans (empty pred+rest)
**작성자:** HyunukLee
**HEAD SHA:** `afca53740a2deafdbf2f93e78191700073804835`
**리뷰 일시:** 2026-08-25

> **TL;DR** (작성자 확인 필요): 커밋 1의 게이트 제거는 컴파일러 불변식으로 안전함을 확인했다. 설명에 없는 커밋 2가 union 오독 크래시를 막는 별도 수정인데, 그 결함은 develop에도 있어 이 PR 범위 밖이다. JIRA나 PR에 사유가 명시되면 그대로 진행해도 된다.

## Summary

- **변경 요약**: 커밋 1은 spec의 pred/rest regu list (쿼리에서 참조되는 컬럼 값을 DB_VALUE로 복사하는 regu 변수 목록)가 둘 다 비었을 때 걸던 `CANNOT_LIST_MERGE`를 제거. 커밋 2는 BUILDVALUE_PROC (GROUP BY 없는 단일 튜플 쿼리용 XASL proc) 전체를 list merge에서 차단
- **주요 이슈**: 커밋 2가 막는 결함이 이 PR과 무관하게 develop에도 존재
- **확인 필요 사항**: 커밋 2의 사유가 JIRA/PR에 기록되는지, 그리고 해당 형태의 회귀 TC 추가

---

## Findings

### Blocking (must fix)
없음.

### Non-blocking (should consider)

- **커밋 1의 게이트 제거는 안전하다.** 검증 결과를 남긴다. `pt_make_table_info` (`xasl_generation.c:4859`)는 `attribute_list`를 `referenced_attrs`(문장 전체의 SELECT/WHERE/GROUP BY/HAVING/ORDER BY에서 이 테이블 컬럼을 참조한 모든 PT_NAME)로 채우고, `pt_split_attrs` (`:2796`)가 이를 pred_attrs와 rest_attrs로 나눈다. 따라서 pred와 rest가 둘 다 비는 것은 "이 테이블의 컬럼이 쿼리 어디에서도 참조되지 않았다"는 컴파일러 자신의 증명이다. 워커의 list merge write 경로는 `qdata_generate_tuple_desc_for_valptr_list`로 outptr_list만 투영하고(`px_scan_result_handler.cpp:846-883`, 이 파일에 pred/rest regu list 참조 0건), outptr_list는 같은 심볼 테이블에서 만들어지므로 채워지지 않은 컬럼 값을 읽을 수 없다. 술어가 key/range로 전부 흡수되는 인덱스 스캔 형태도 같은 논리로 안전하다.

- **`px_scan_checker.cpp:556` 주석이 홀로 이해되지 않는다.** `/* agg-less buildvalue too: MERGEABLE_LIST would misread proc.buildlist in result_handler init. */` 의 "result_handler init"은 grep으로 찾을 수 있는 심볼이 아니다. 실제 위치는 `px_scan_result_handler.cpp:99`의 `result_handler<MERGEABLE_LIST>` 생성자다. 또 왜 오독인지(`proc`가 union이라는 점)를 적지 않아 다음 사람이 근거를 되짚을 수 없다. 심볼명과 이유를 함께 적는 것이 좋겠다.

### Questions for the author

- **커밋 2의 사유를 JIRA나 PR에 남겨주시겠습니까?** HEAD에는 커밋이 두 개이고(`e4a816e70`, `afca53740`), 커밋 2는 `px_scan_checker.cpp:557`에서 BUILDVALUE_PROC 전체에 무조건 `CANNOT_LIST_MERGE`를 세운다. 이 커밋이 막는 것은 `proc` union (`xasl.h:1185`) 오독이다:
  ```cpp
  /* px_scan_result_handler.cpp:99, result_handler<MERGEABLE_LIST> 생성자 */
  m_.g_hash_eligible = (bool) orig_xasl_tree_for_domain_resolve->proc.buildlist.g_hash_eligible;
  ```
  `BUILDLIST_PROC_NODE`(멤버 약 26개)와 `BUILDVALUE_PROC_NODE`(멤버 6개)는 크기가 달라 BUILDVALUE 노드에서는 쓰레기 값을 읽고, 참이면 `:695`에서 `buildlist_proc->agg_hash_context->part_list_id`를 역참조한다. agg-less BUILDVALUE는 실제로 만들어진다. `pt_has_aggregate` (`parser_support.c:3164`)가 STEP 2에서 집계 함수 유무와 무관하게 `having`만 있으면 참을 반환하고, `pt_is_single_tuple` (`xasl_generation.c:3679`)이 참이 되어 BUILDVALUE_PROC가 선택되지만 `pt_to_aggregate`는 집계 노드가 없어 NULL을 돌려주기 때문이다. 200만 행 테이블에서 `SET TRACE ON`으로 확인한 결과는 다음과 같다:
  ```
  SELECT 1 FROM t1 HAVING 1 = 1;             -> 1행, gather: row by row
  SELECT 1 FROM t1 WHERE a > 0 HAVING 1 = 1; -> 1행, gather: mergeable list
  ```
  술어가 붙은 쪽만 mergeable list에 도달하며, 이 형태는 제거 전 게이트로도 막히지 않으므로 develop에도 같은 경로가 있다. 즉 커밋 2는 이 PR의 범위 밖 결함을 고치는 것으로 보인다. 별도 티켓이 아니어도 좋으니 JIRA나 PR 본문에 "원래 이런 문제가 있어 함께 고쳤다"가 적혀 있으면 충분합니다. 아울러 위 `WHERE`가 붙은 쿼리를 회귀 TC로 넣는 것은 어떨까요? (참고: 위 트레이스는 develop 계보가 아닌 별도 빌드에서 얻은 것이라 플랜 형태의 도달 가능성만 보여준다.)

## JIRA Context

CBRD-27292 (Sub-task, Open, 부모 CBRD-27152)의 목표는 empty-pred+rest `CANNOT_LIST_MERGE` 제거이며 커밋 1은 그 범위 안이다. 티켓은 COUNT(*)/buildvalue_opt 경로를 "해당 없음"으로 명시해 범위에서 제외했고, 커밋 2에 해당하는 내용은 없다. 수용 기준은 `SELECT 1 FROM t`의 병렬 결과가 직렬과 일치, ROWNUM/Java SP 쿼리 정상 동작, CTP 회귀 통과다.

## Existing Comments

작성자/메인테이너의 미해결 top-level 코멘트는 없다. 이슈 코멘트는 CI 봇 알림과 작성자의 `/run all` (5회), `/run sql` (1회)뿐이다.
