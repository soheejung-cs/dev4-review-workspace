# PR #7797 — 게시용 리뷰 코멘트 초안

게시하지 않았다. 아래 내용을 그대로 붙여넣을 수 있다.

---

## 인라인 코멘트 1

**대상:** `src/optimizer/rewriter/query_rewrite_subquery.c:118`

아래 질의가 이 PR 에서 컴파일에 실패합니다. 서브쿼리에 `/*+ NO_UNNEST */` 를 붙이면 정상 동작합니다.

```sql
create table w1(k int, a int);  create table w2(k int, y int);
insert into w1 values (1,10),(2,20),(3,30);
insert into w2 values (1,10),(2,20),(3,99);

select w1.a from w1
 where (select max(w1b.a) from w1 w1b where w1b.a = w1.a) in (select w2.y from w2 where w2.k = w1.k);
-- ERROR: check outer join syntax at 'w1b.a=[dba.w1].a'
```

여기 서브쿼리 검사가 `where` 와 `list` 만 보는데, IN 계열은 왼쪽값(`cnf_node->info.expr.arg1`)도 `왼쪽값 = select 항목` 등식으로 만들어 ON 에 올리므로 왼쪽값도 함께 봐야 하지 않나요. 그리고 `pt_check_subquery_post()` 가 `is_subquery == PT_IS_SUBQUERY` 인 `PT_SELECT` 만 잡아서, arm 이 `PT_IS_UNION_SUBQUERY` 로 표시되는 UNION 서브쿼리는 그냥 통과합니다. `exists (... and w2.y in (select ... union select ...))` 형태도 같은 에러입니다.

ON 에 서브쿼리가 남으면 `pt_mark_location()` 이 중첩 블록 자신의 조건에까지 이 spec 의 `location` 을 찍고, 마지막에 `qo_rewrite_queries_post()` 가 그 번호의 spec 을 못 찾아 위 에러가 납니다.

손으로 쓴 같은 조인은 `Cannot use a subquery in join condition clause.` 로 정상 거부되니, 두 곳을 각각 막는 것보다 재작성 직후 `pt_check_semi_anti_join()` 을 한 번 더 돌리는 편이 나아 보입니다. PR 설명의 "자동 생성된 조인이 손으로 쓴 조인과 같은 검증 · 같은 트리를 거치게 합니다" 와 같은 방향이라고 생각합니다.

---

## 인라인 코멘트 2

**대상:** `src/optimizer/rewriter/query_rewrite_subquery.c:451`

힌트 비트마스크를 통째로 옮기면서 `PT_HINT_PARALLEL` 도 같이 넘어가는데, 값이 들어 있는 `q.select.num_parallel_threads` 는 안 옮겨가서 바깥이 `PARALLEL(0)` 이 되지 않나요. `optimization level 513` 으로 본 재작성문입니다.

```
힌트 없음                : select count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
서브쿼리에만 PARALLEL(4)  : select /*+ PARALLEL(0) */ count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
바깥에 PARALLEL(4)       : select /*+ PARALLEL(4) */ count(*) from t1 t1 semi join t2 t2 on t2.k=t1.k
```

값이 0 이면 `plan_generation.c` 가 driving spec 에 `PT_SPEC_FLAG_NO_PARALLEL_SCAN` 을 달고 `query_planner.c` 도 parallel hash join 을 끄는 것으로 알고 있습니다. 사용자가 쓰지 않은 병렬 끄기가 바깥 질의 전체에 적용됩니다. `PT_HINT_LK_TIMEOUT` 의 `waitsecs_hint` 도 같은 구조라 바깥에 빈 `/*+ */` 만 남는 것을 확인했습니다.

`PT_HINT_NO_MERGE` 처럼 제외 목록에 넣거나, 값 필드까지 같이 옮기는 것은 어떨까요.

---

## top-level 코멘트

`[NOT] EXISTS` / `[NOT] IN` 을 SEMI / ANTI JOIN 으로 바꾸는 재작성기를 봤습니다. 판정과 트리 변경을 나눠서 거부 시 트리가 그대로 남게 한 구조가 좋았고, `NOT IN` 의 NULL 가드는 왼쪽값이 표현식인 경우 · 스칼라 서브쿼리인 경우 · 오른쪽이 표현식인 경우로 다섯 가지를 넣어 봤는데 전부 제대로 막혔습니다.

head `c330f9bd4` 를 optdebug 로 빌드해서, 같은 바이너리에 `/*+ NO_UNNEST */` 를 켜고 끈 A/B 로 확인했습니다. 두 가지를 인라인으로 남겼습니다.

확인 부탁드립니다.

- ON 에 서브쿼리가 들어가면서 기존에 잘 돌던 질의가 `check outer join syntax` 로 실패하는 건
- 서브쿼리 블록의 `PARALLEL` 힌트가 값 없이 바깥으로 올라가는 건
