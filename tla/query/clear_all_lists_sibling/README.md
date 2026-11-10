# clear_all_lists_sibling — 행마다 리스트 정리가 다음 갈래까지 지울 때 (병렬 부질의 실행)

- 이슈·PR: CBRD-27959 · CUBRID/cubrid#8436 (2026-11-10 자기 리뷰, tla.md 두 번째 적용 — `tla_candidates.py` 는 후보를 내지 않았고 PR 본문·JIRA 의 결함 상황으로 §1 '동시성'에 넣었다)
- 대상 함수: `qexec_clear_all_lists ()`(src/query/query_executor.c — PR 이 바꾼 함수), 그것을 행마다 부르는 `qexec_intprt_fnc ()`, 병렬 부질의 실행의 작업 분배(`px_query_executor.cpp` run_jobs·`px_query_task.cpp` execute_job_internal)
- 실행: `python3 tools/tla/tlc_run.py tla/query/clear_all_lists_sibling/ClearAllListsSibling.tla --config tla/query/clear_all_lists_sibling/before.cfg --expect violation`
  · `… --config …/after.cfg --expect ok` (그 밖의 cfg 는 아래 결과 표)

## 무엇을 모델로 했나
부모의 aptr 목록에 놓인 갈래 1..NOps(next(i) = i + 1 — UNION 의 두 갈래는 `pt_set_aptr ()`·`pt_append_xasl ()` 로 이렇게 이어진다)를 병렬 부질의 실행이 작업으로 나눈다: 첫 갈래는 루트 스레드가(m_job), 나머지는 큐에서 워커와 루트가 꺼낸다.
`hasD` 에 든 갈래는 안쪽 스캔의 dptr_list 에 바깥 표와 상관된 IN 부질의 D_i 가 있다(IN 항에 연결, LINK_TO_REGU_VARIABLE). 바깥 행마다 D_i 의 리스트를 비우고(앞 정리), IN 항이 D_i 를 실행해 새 리스트(임시 파일 구조 하나)에 쓰고 읽은 뒤, 행 끝 정리가 리스트를 없애 임시 파일 구조를 풀에 돌려준다.
초기 상태가 `hasD`(갈래의 모든 부분집합)와 리스트 모양(`reuse`: 행마다 없애고 다시 여는가 / truncate 해 다시 쓰는가)을 고르므로, 그 범위의 모든 경우와 두 스레드의 모든 엇갈림을 TLC 가 끝까지 따라간다.
`FIX = FALSE` 는 develop — 갈래 i 의 행 끝 정리가 i 뒤 모든 갈래의 D_j 도 지운다. `FIX = TRUE` 는 PR — D_i 만.

| 모델 | 코드 (PR 헤드 `482608da0`, base develop `953e83256` — 줄은 같고 다르면 따로 적음) |
|---|---|
| `Init` 의 루트 = 갈래 1, `nextJob` | aptr 루프의 `add_job ()` query_executor.c:22060(base :22058) — 첫 작업은 m_job, 나머지는 큐 · `run_jobs ()` 가 m_job 을 루트에서 실행 px_query_executor.cpp:143 |
| `Pop` | 루트의 `try_pop ()` 루프 px_query_executor.cpp:146-170 · 워커의 `pop ()` px_query_task.cpp:74 |
| `Scan` | `qexec_intprt_fnc ()` 의 바깥 스캔 query_executor.c:14686 → 안쪽 스캔 `qexec_execute_scan ()` :12306 |
| `PreRd`·`PreFr` | 안쪽 스캔의 `qexec_execute_dptr_list (…, true)` :12428 → `qexec_clear_head_lists_with_truncate ()` :3605 — 재사용 가능하면 `qfile_truncate_list ()` list_file.c:2970(임시 파일 유지), 아니면 `qexec_clear_xasl_head ()` :1557 → `qfile_destroy_list ()` list_file.c:2079(`tfile_vfid` 를 읽고 :2083, 반납 :2085, 리스트 id 비움 :2102 — 읽기와 비움 사이가 한 걸음이 아니다) |
| `Open` | IN 항이 연결 부질의를 실행 query_evaluator.c:2388(`EXECUTE_REGU_VARIABLE_XASL`, xasl.h:614 — 상태가 초기면) → `qexec_start_mainblock_iterations ()` → `qfile_open_list ()` list_file.c:1119 → `qmgr_create_new_temp_file ()` query_manager.c:3054(전역 반납 목록에서 꺼냄) |
| `Write` | `qfile_add_tuple_to_list ()` list_file.c:1647 → `qfile_allocate_new_page ()` :1377 → `qmgr_get_new_page (list_id->tfile_vfid)` query_manager.c:2922 |
| `Probe` | query_evaluator.c:2389 상태 확인(아니면 V_ERROR — "Query execution failure"), :2397 `tuple_cnt == 0` 이면 V_FALSE(행이 빠짐), 그다음 리스트를 읽음 |
| `EndRd`·`EndFr` | 행 끝 `qexec_clear_all_lists (thread_p, xasl)` query_executor.c:15053(base :15051) → PR :3760-3787 은 받은 블록만 / develop :3755-3784 는 `xasl->next` 를 따라 :3759 → `qexec_clear_scan_all_lists ()` :3728 → `qexec_clear_head_lists ()` :3706 → `qexec_clear_xasl_head ()` → `qfile_destroy_list ()` |
| `JEndRd`·`JEndFr` | 작업 끝 `qexec_clear_xasl_for_parallel_aptr (cur_thread_p, xasl, true)` px_query_task.cpp:214 → query_executor.c:3087 — 갈래의 하위(D_i 포함)를 지우고 next 는 지우지 않는다(":3561 (not) clear the next xasl node") |
| `owner`·`FREE` | `qmgr_free_list_temp_file ()` query_manager.c:3452-3494 — 질의 항목 목록에서 떼고(뮤텍스 :3471) 전역 반납 목록에 넣음 :3489. 이미 반납한 것을 다시 넣으면 같은 구조가 두 리스트에 나간다 |

## 성질
- `Safe` — 사용자에게 보이는 것: 쓰는 중인 리스트 id 가 비워지지 않고(`null`), 반납했거나 다른 리스트에 나간 임시 파일을 쓰지 않고(`uaf`·`shared`), 같은 임시 파일을 두 번 반납하지 않고(`double`), IN 항이 자기 행의 튜플을 읽는다(`stale` — 서버 죽음·"Query execution failure"·행 빠짐에 대응).
- `OnlyOwnLists` — PR 이 주장하는 관계: 한 작업의 행마다 정리는 그 작업 부질의의 리스트만 반납한다.
- `NoLeak` — 모든 스레드가 끝나면 임시 파일 구조가 모두 풀에 있다.
- `Ends` — 끝난다(약한 공정성).

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-10 (.51) | develop(FIX=FALSE), 갈래 3·행 2·워커 1, 모든 hasD, 두 리스트 모양 | before.cfg | **Safe 위반**(기대대로) — 반례 9걸음 | 2,210 / 1,406 | 9 | 1.9s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…`) |
| 2026-11-10 (.51) | develop, 이슈의 모양만(행마다 없애고 다시 엶) | before_destroy.cfg | **Safe 위반**(기대대로) — 반례 10걸음 | 1,378 / 869 | 10 | 1.9s | 같음 |
| 2026-11-10 (.51) | develop, 두 갈래 중 **둘째에만** 부질의 | second_only.cfg | **Safe 위반**(기대대로) — 반례 9걸음 | 228 / 163 | 9 | 1.6s | 같음 |
| 2026-11-10 (.51) | develop, 두 갈래 중 첫째에만 부질의 | first_only.cfg | 전부 성립 — TypeOK·Safe·OnlyOwnLists·NoLeak·Ends | 1,648 / 861 | 26 | 2.1s | 같음 |
| 2026-11-10 (.51) | develop, px 실행기 없음(NWorkers=0 — NO_PARALLEL_SUBQUERY 대조) | serial.cfg | 전부 성립 — TypeOK·Safe·NoLeak·Ends | 2,884 / 2,564 | 70 | 2.5s | 같음 |
| 2026-11-10 (.51) | PR 헤드 `482608da0`(FIX=TRUE), before.cfg 와 같은 범위 | after.cfg | **전부 성립** — TypeOK·Safe·OnlyOwnLists·NoLeak·Ends | 496,452 / 222,360 | 65 | 19.8s | 같음 |
| 2026-11-10 (.51) | PR, 워커 2(스레드 3), 행 1, 이슈의 모양만 | after_w2.cfg | 전부 성립 | 397,990 / 133,436 | 39 | 12.6s | 같음(`--workers 4`) |

after_w2 의 첫 판(행 2, 두 모양)은 600초 안에 끝나지 않아 범위를 줄였다.

before 반례(가장 짧은 것): `hasD = {2}` — **둘째 갈래에만 부질의가 있다.** 루트가 첫 갈래의 한 행을 읽고 행 끝 정리를 시작(`Scan` → 대상 D_2) → 워커가 둘째 갈래를 꺼내 한 행을 읽고 D_2 를 비운 뒤 새 리스트를 엶(f1) → 루트가 D_2 의 `tfile_vfid`(f1)를 읽고 반납·비움 → 워커가 D_2 에 튜플을 쓰려다 빈 리스트 id 를 만남(`null`).
코드로는 query_executor.c:15051(develop) → :3759 의 `xasl->next` 로 둘째 갈래 → :3782 `qexec_clear_scan_all_lists ()` → `qexec_clear_xasl_head ()` → `qfile_destroy_list ()` 가 워커의 `qfile_open_list ()` 직후 리스트를 지우고, 워커의 `qfile_add_tuple_to_list ()` 가 그 리스트에 쓴다.
before_destroy 반례는 읽기와 비움 사이의 엇갈림이다: 루트가 D_2 의 `tfile_vfid` 를 NULL 로 읽은 뒤 워커가 새 리스트를 열고, 루트의 `qfile_clear_list_id ()` 가 그 리스트 id 를 비운다(f1 은 반납되지 않은 채 남는다).

PR 본문의 "한 갈래에만 IN 부질의가 있으면 죽지 않고"는 **첫 갈래에만** 있을 때만 구조적으로 맞다(first_only 성립 · second_only 위반). 둘째에만 있을 때 실측이 죽지 않은 것은 첫 갈래(부질의 없는 9행 조인)가 워커가 둘째 갈래를 시작하기 전에 끝나는 시점 탓으로 본다 — 모델은 시점을 고르지 않는다.

## 모델이 보지 않는 것
- 정렬이 리스트를 바꿔 다는 것(`qfile_sort_list_with_func ()` — IN 부질의의 DISTINCT·정렬), `single_tuple` 값 해제(`pr_clear_value ()`), D 의 상태 되돌리기와 `EXECUTE_REGU_VARIABLE_XASL` 의 엇갈림 — 모두 같은 '남의 리스트를 지움' 의 다른 결과라 `Write`·`Probe` 의 실패 하나로 묶었다.
- 질의 항목의 임시 파일 목록 연결(`qmgr_free_list_temp_file ()` 의 잠금 없는 `if (query_p->temp_vfid)` 와 뮤텍스 안 unlink) — 반납 한 번을 한 걸음으로 두었다. 이중 반납이 그 목록을 깨는 것은 `double` 로만 센다.
- 갈래 자신의 결과 리스트(head), 부모의 join 뒤 정리(`qexec_clear_head_lists (xptr->aptr_list)`), 중첩 집합 연산(`(A ∪ B) ∪ C` 는 A→B 와 U(A,B)→C 두 목록), 해시 조인·병합 조인 입력, CTE.
- 한 바깥 행 안의 안쪽 행 여러 개(truncate 경로는 안쪽 행마다 돈다) — 바깥 행 하나에 안쪽 행 하나로 두었다.
- bptr·fptr 블록 아래 부질의 — `qexec_clear_all_lists ()` 가 같은 길로 지우므로 D_i 하나로 대신했다. 단위 suite `unit/query/query_executor.clear_all_lists` 가 그 갈래들을 실제 함수로 확인한다.
