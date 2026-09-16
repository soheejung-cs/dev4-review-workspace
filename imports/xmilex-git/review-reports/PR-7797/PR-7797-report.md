# PR #7797 코드 리뷰 보고서

**PR:** [CUBRID/cubrid#7797](https://github.com/CUBRID/cubrid/pull/7797)
**제목:** [CBRD-24043] Support subquery unnest (review against SEMI-ANTI-JOIN)
**작성자:** Hamkua
**HEAD SHA:** `c330f9bd4f15737f51381c47a56a4a6a57605a72`
**리뷰 일시:** 2026-08-29

> **TL;DR** (Blocking): 재작성기가 "ON 조건에는 서브쿼리가 올 수 없다"는 자기 전제를 두 경로에서 검사하지 않아, 지금까지 정상 동작하던 질의가 `check outer join syntax` 로 컴파일에 실패한다. 서브쿼리 블록의 `PARALLEL` 힌트도 값 없이 바깥으로 새어 바깥 질의에 `PARALLEL(0)` 을 찍는다. 둘 다 PR head 를 빌드해 재현했고, 기존 리뷰가 다루지 않은 경로다.

## Summary

- **변경 요약**: WHERE 의 `[NOT] EXISTS` / `[NOT] IN` 을 SEMI / ANTI JOIN 으로 재작성하는 `qo_rewrite_exists_semi_anti()` 신설
- **주요 이슈**: ON 서브쿼리 금지 게이트의 구멍 2개로 정상 질의가 컴파일 실패, `PARALLEL` 힌트 비트만 이관
- **확인 필요 사항**: 재작성 결과를 `pt_check_semi_anti_join()` 으로 재검증할지 (아래 수정 방향 참고)

---

## Findings

검증 방법: PR head 를 optdebug 로 빌드하고, 같은 바이너리에서 `/*+ NO_UNNEST */` 유무로 A/B 비교했다. 아래 2건 외에 `NOT IN` 의 NULL 가드 우회(5형태)와 중첩 상관 서브쿼리의 `correlation_level` 어긋남도 시도했으나 재현되지 않았고, 재작성기 파일 전체를 대상으로 한 독립 리뷰에서도 추가 발견은 없었다.

### Blocking (must fix)

**1. ON 조건에 서브쿼리가 들어가 정상 질의가 컴파일에 실패한다**

`src/optimizer/rewriter/query_rewrite_subquery.c:114-121`

```sql
create table w1(k int, a int);  create table w2(k int, y int);
insert into w1 values (1,10),(2,20),(3,30);
insert into w2 values (1,10),(2,20),(3,99);

select w1.a from w1
 where (select max(w1b.a) from w1 w1b where w1b.a = w1.a) in (select w2.y from w2 where w2.k = w1.k);
-- NO_UNNEST : 10, 20   (정답)
-- 이 PR     : ERROR: check outer join syntax at 'w1b.a=[dba.w1].a'
```

`IN` 계열을 SEMI JOIN 으로 바꾸려면 `왼쪽값 = select 항목` 등식을 만들어 ON 자리에 넣어야 한다. 그런데 CUBRID 문법은 ON 안의 서브쿼리를 금지하고(손으로 쓰면 `Cannot use a subquery in join condition clause.`), 재작성기도 이를 알고 게이트를 두었는데 그 게이트에 구멍이 둘이다.

* **왼쪽값을 검사하지 않는다.** 게이트는 서브쿼리의 WHERE 와 select 절만 훑고, ON 으로 올라가는 왼쪽값(`cnf_node->info.expr.arg1`, 같은 파일 303행)은 보지 않는다.
* **UNION 을 서브쿼리로 인식하지 못한다.** 검사에 쓰는 `pt_check_subquery_post()` 는 `is_subquery == PT_IS_SUBQUERY` 인 `PT_SELECT` 만 잡는데, UNION 의 arm 은 `PT_IS_UNION_SUBQUERY` 로 표시되고(`src/parser/parser_support.c:273`) `PT_UNION` 노드 자체는 `PT_SELECT` 가 아니라 그냥 통과한다. 그래서 `exists (select 1 from w2 where w2.k = w1.k and w2.y in (select ... union select ...))` 도 같은 에러를 낸다.

통과한 서브쿼리가 ON 에 남으면, `pt_mark_location()` (같은 파일 471행) 이 ON 아래 **모든** 노드에 이 조인의 `location` (spec 의 조인 단계 번호) 을 찍는다. ON 안에 서브쿼리가 있을 수 없다는 전제로 짜인 함수라서, 중첩 블록 자신의 조건인 `w1b.a = w1.a` 에까지 바깥 조인 번호가 찍힌다. 마지막에 `qo_rewrite_queries_post()` 가 `location > 0` 인 조건마다 같은 번호의 spec 을 찾는데, 그 블록의 FROM 에는 없다 -> 에러(`src/optimizer/rewriter/query_rewrite.c:640-643`, `:698-699`).

사용자는 SQL 을 바꾸지 않았는데 질의가 죽고, 에러 메시지는 OUTER JOIN 을 가리켜 원인 추적이 어렵다. 오답이나 크래시는 재현되지 않았다.

*수정 방향*: 게이트에서 왼쪽값도 같이 검사하고, `pt_check_subquery_pre/post` 가 `PT_UNION` / `PT_DIFFERENCE` / `PT_INTERSECTION` 도 잡도록 고치면 이 두 경로는 막힌다. 다만 같은 성격의 구멍이 또 나올 수 있으니, 재작성 직후 `pt_check_semi_anti_join()` 을 다시 돌려 재작성기가 만든 조인도 손으로 쓴 조인과 같은 검증을 통과하게 하는 편이 근본적이다. PR 본문이 선언한 "자동 생성된 조인이 손으로 쓴 조인과 같은 검증 · 같은 트리를 거치게 합니다" 가 정확히 이 이야기다.

**2. `PARALLEL` 힌트가 값 없이 바깥 SELECT 로 옮겨간다**

`src/optimizer/rewriter/query_rewrite_subquery.c:451-453` 에서 힌트 비트마스크를 통째로 OR 하는데, 값이 별도 필드에 있는 힌트를 제외하지 않는다. `PT_HINT_PARALLEL` 의 값은 `q.select.num_parallel_threads` 에 있고 이 필드는 옮기지 않으므로 바깥에서는 값이 0 이 된다. `optimization level 513` 의 재작성문:

```
힌트 없음                : select count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
서브쿼리에만 PARALLEL(4)  : select /*+ PARALLEL(0) */ count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
바깥에 PARALLEL(4)       : select /*+ PARALLEL(4) */ count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
```

값 0 은 "병렬 끄기"다. `src/optimizer/plan_generation.c:3213-3220` 은 힌트가 있고 `num_parallel_threads <= 1` 이면 driving spec 에 `PT_SPEC_FLAG_NO_PARALLEL_SCAN` 을 달고, `src/optimizer/query_planner.c:14170-14179` 도 같은 값으로 parallel hash join 을 끈다. 사용자가 쓰지 않은 `PARALLEL(0)` 이 바깥 질의 전체에 적용된다. `PT_HINT_LK_TIMEOUT` (`waitsecs_hint`) 도 같은 구조로, 바깥에 빈 `/*+ */` 만 남는 것을 확인했다.

## JIRA Context

CBRD-24043 은 IN / EXISTS 서브쿼리의 고정된 스캔 순서를 조인으로 바꿔 옵티마이저가 순서를 고르게 하자는 티켓이고, REWRITER / OPTIMIZER / EXECUTOR 세 계층을 모두 명시한다. 본 PR 은 REWRITER 부분에 해당하며 범위 안이다.

## 기존 리뷰가 확인했다고 적었지만 아직 위험한 지점

아래 3건은 기존 리뷰가 "확인했다 / 문제 없다" 로 결론지은 항목인데, 실측 결과 그 결론이 그대로 서지 않는다. 기존 리뷰가 이미 열어둔 미해결 지적들은 여기서 다시 다루지 않는다.

| 기존 리뷰의 결론 | 실제 |
|---|---|
| head `c330f9bd4` 에서 랜덤 3600개 중 "하드 에러 0" | 위 Blocking 1 이 하드 에러를 낸다. 해당 코퍼스는 상관 서브쿼리 x outer 1-2개 x conjunct 1-3개 조합이라 스칼라 서브쿼리 왼쪽값도 UNION 서브쿼리도 들어 있지 않다 |
| "중첩 서브쿼리 미변환" 을 develop 과 대조해 확인 | 평범한 중첩 `SELECT` 는 실제로 변환되지 않는다(대조군으로 확인). 왼쪽값과 UNION 두 경로에서만 뚫린다 |
| `PARALLEL(4)` vs `PARALLEL(0)` 로 600개를 돌려 "병렬 실행 쪽은 문제 없었습니다" | 힌트를 바깥 블록에 건 검사다. 서브쿼리 블록에 걸면 위 Blocking 2 가 발생한다 |
