# PR #7673 코드 리뷰 보고서

**PR:** [CUBRID/cubrid#7673](https://github.com/CUBRID/cubrid/pull/7673)
**제목:** [CBRD-27176] GROUP BY 절에서 PK/유니크 키에 함수적으로 종속된 컬럼을 그룹 키에서 제거하는 최적화
**작성자:** Hamkua
**HEAD SHA:** `f65ecb79ab30a697424456cf72ee19daff5751d4`
**리뷰 일시:** 2026-08-31

> **TL;DR** (Non-blocking): 현재 스키마 제약 아래에서 오답이 나는 경로는 찾지 못했다. 다만 `qo_is_row_identifying_key()`가 `cons->filter_predicate` (필터 인덱스, 즉 WHERE 조건을 만족하는 행만 담는 부분 인덱스)를 검사하지 않아, PR 본문이 약속한 "필터 인덱스 제외"가 코드에 없다. 지금 안전한 이유는 이 함수가 아니라 문법 파일 한 곳이 filtered unique index 생성을 막고 있기 때문이다.

## Summary

- **변경 요약**: GROUP BY에 어떤 테이블의 PK 또는 모든 컬럼이 NOT NULL 인 UNIQUE 키가 전부 있으면, 같은 테이블의 나머지 GROUP BY 컬럼을 그룹 키에서 제거. `query_rewrite_select.c` 한 파일에 함수 3개와 호출 1줄 추가.
- **주요 이슈**: 필터 인덱스 가드 부재. 그리고 새 함수 주석의 `pt_to_aggregate()` 설명이 실제 경로와 어긋남.
- **확인 필요 사항**: JIRA AC 3번(축소의 전제인 제약이 사라지면 캐시된 실행 계획을 재사용하지 않아야 함)에 대응하는 TC. 별개로 TC Merge Gate 가 아직 열려 있어 머지는 막혀 있음.

---

## Findings

### Non-blocking (should consider)

- `src/optimizer/rewriter/query_rewrite_select.c:1274` — `qo_is_row_identifying_key()` 가 `cons->filter_predicate` 를 검사하지 않는다. 필터 인덱스는 필터를 통과한 행 사이에서만 유일성을 보장하므로, 필터 밖 행끼리는 같은 키 값을 여러 번 가질 수 있다. 이 제약이 근거로 채택되면 `group by a, c` 에서 `c` 가 지워지고 서로 다른 행이 한 그룹으로 합쳐진다. 지금은 `csql_grammar.y` 의 `unique_constraint` 규칙이 filtered/function unique index 를 막아 도달 불가이지만, 같은 자리 주석이 "Unique filter/function index code is removed from grammar module only ... easily support this feature later by adding in grammar only" 라고 적어 두었다. 문법 한 줄이 풀리면 이 함수는 그대로 오답을 만든다. 같은 파일 `qo_rewrite_nonnull_count()` 는 이미 `if (cons_iter->filter_predicate)` 로 자기 재작성을 포기한다.

  ```c
    if (cons->index_status != SM_NORMAL_INDEX)      /* :1288 */
      {
        return false;
      }
  ```

  참고로 `func_index_info` 쪽은 가드가 없어도 안전하다. `execute_schema.c` 의 `create_or_drop_index_helper()` 가 `attr_index_start = nnames - func_no_args` 로 두므로 `cons->attributes` 에 함수 인자 컬럼까지 들어가고, `(a, f(b))` 의 유일성은 `(a, b)` 의 유일성을 함의한다. 즉 실제로 필요한 것은 `filter_predicate` 한 줄이다.

- `src/optimizer/rewriter/query_rewrite_select.c:1360` — `qo_reduce_group_by()` 의 Note 주석이 "`pt_to_aggregate ()` appends every select list name that is missing from the intermediate output list" 라고만 적었다. `pt_to_aggregate()` (집계 XASL 생성 시 중간 출력 리스트를 채우는 함수) 는 실제로 select list 와 `HAVING` 두 곳을 순회하며, 윈도우 절에만 등장하는 제거 컬럼을 살리는 것은 이 함수가 아니라 파생 테이블 재작성이다. shparkcubrid 님이 PR 본문에 대해 같은 지적을 하셨는데, 본문만 고치면 소스 주석에는 같은 설명이 그대로 남는다.

  ```c
    select_node->info.query.q.select.list =
      parser_walk_tree (parser, select_list, pt_to_aggregate_node, &info, pt_continue_walk, NULL);
    select_node->info.query.q.select.having =
      parser_walk_tree (parser, having, pt_to_aggregate_node, &info, pt_continue_walk, NULL);
  ```

### Questions for the author

- CBRD-27176 의 Acceptance Criteria 3번은 "축소의 전제가 되는 유니크 제약 또는 NOT NULL 속성을 제거하면 축소된 캐시 실행 계획이 재사용되지 않는다" 인데, 이번 diff 에는 이에 해당하는 코드가 없다. 스키마 변경 시 XASL 캐시가 클래스 단위로 무효화되는 기존 동작에 기대는 것으로 이해하면 되는지, 그리고 `ALTER TABLE ... DROP CONSTRAINT` / `MODIFY ... NULL` / `ALTER INDEX ... INVISIBLE` 이후 재실행을 확인하는 TC 가 tc/pr-7673 에 들어가 있는지 확인 부탁드립니다.

## JIRA Context

CBRD-27176 은 PostgreSQL 의 `remove_useless_groupby_columns()` 에 대응하는 최적화를 요구하며, 본 PR 은 그 범위 안에 있다. AC 1(Q10 그룹 키 7컬럼 -> 2컬럼)과 AC 2(결과 불변)는 PR 본문의 TPC-H SF10 측정과 shparkcubrid 님의 퍼저 대조로 근거가 제시되어 있고, AC 3 은 위 질문으로 남는다.

## Existing Comments

| 코멘트 | 위치 | 상태 |
| --- | --- | --- |
| greptile-apps[bot]: 중복 키 컬럼이 `seen` 을 증가시켜 완성 시점을 앞당긴다 | `query_rewrite_select.c:1495` | 리뷰 시점 커밋 3a619f73 기준. 현재 HEAD 의 `qo_reduce_group_by()` 에는 `seen` 카운터가 없고 위치 무관 판정으로 바뀌었으며 `a, a, b, c` 케이스도 본문 표에 포함되어 있음. 해소되었다는 답글이 필요함 |
