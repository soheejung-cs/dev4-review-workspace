# PR #7767 게시용 리뷰 코멘트 (C1 + C3 게시 결정)

## C1. 인라인 — `src/query/parallel/px_scan/px_scan_checker.cpp:557`

이 커밋(`afca53740`)이 막는 게 `result_handler<MERGEABLE_LIST>` 생성자에서 `proc.buildlist.g_hash_eligible`을 읽는 부분(`px_scan_result_handler.cpp:99`)으로 이해하고 있습니다. `proc`가 union이라 BUILDVALUE 노드에서는 쓰레기를 읽고, 그 값이 참이면 `agg_hash_context->part_list_id`까지 역참조하는 것으로 보입니다.

그런데 이 경로는 술어가 붙으면 empty-pred+rest 게이트로도 막히지 않아서, 커밋 1과 무관하게 develop에도 있는 것 같습니다. 아래 두 쿼리를 200만 행 테이블에서 확인해보니 `WHERE`가 붙은 쪽만 mergeable list로 갔습니다.

```sql
SELECT 1 FROM t1 HAVING 1 = 1;             -- gather: row by row
SELECT 1 FROM t1 WHERE a > 0 HAVING 1 = 1; -- gather: mergeable list
```

CBRD-27292 본문은 COUNT(*)/buildvalue_opt를 "해당 없음"으로 빼두고 있어서, 이 커밋 사유를 JIRA나 PR 본문에 한 줄 남겨주시면 좋겠습니다. 위 `WHERE` 쿼리를 회귀 TC로 넣는 것은 어떨까요?

## C3. top-level

empty-pred+rest 게이트 제거 자체는 안전한 것으로 보입니다. `pt_split_attrs`가 `referenced_attrs`를 pred/rest로 나누기 때문에 둘이 모두 비는 건 이 테이블 컬럼이 쿼리 어디에서도 참조되지 않았다는 뜻이고, 워커의 write 경로는 `qdata_generate_tuple_desc_for_valptr_list`로 outptr_list만 투영하니 채워지지 않은 값을 읽을 일이 없어 보입니다. 인덱스 스캔에서 술어가 key/range로 흡수되는 형태도 같은 이유로 괜찮아 보입니다.

확인 부탁드립니다
- 두 번째 커밋(BUILDVALUE list merge 차단) 사유를 JIRA/PR에 명시
- `SELECT 1 FROM t WHERE ... HAVING ...` 형태의 회귀 TC 추가 여부

---

(C2 — `px_scan_checker.cpp:556` 주석 문구 NIT — 는 게시하지 않기로 결정.)
