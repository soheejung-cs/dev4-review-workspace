# scan_minmax_turn — MIN/MAX 만 구하는 인덱스 스캔이 방향을 뒤집을 때

- 이슈·PR: CBRD-28007 · CUBRID/cubrid#8472 (2026-11-10, 첫 TLA+ 적용 — 사용자 지시 "이번 PR부터")
- 대상 함수: `scan_next_index_scan ()`(src/query/scan_manager.c), `qexec_intprt_fnc ()`(src/query/query_executor.c), `qdata_evaluate_aggregate_list ()`(src/query/query_aggregate.cpp)
- 실행: `python3 tools/tla/tlc_run.py tla/query/scan_minmax_turn/ScanMinMaxTurn.tla --config tla/query/scan_minmax_turn/before.cfg --expect violation`
  · `… --config tla/query/scan_minmax_turn/after.cfg --expect ok`

## 무엇을 모델로 했나
키 범위의 행을 인덱스 순서의 위치 1..n 으로 둔다. 그러면 답(오름차순 인덱스의 MAX, 내림차순 인덱스의 MIN)은 필터를 통과하는 마지막 위치다. 초기 상태가 n(1..MaxN)·OID 버퍼 용량 cap(1..MaxCap)·필터가 통과시키는 행의 집합(모든 부분집합)을 고르므로, 그 범위 안의 모든 경우를 TLC 가 끝까지 따라간다.

| 모델 | 코드 (PR 헤드 `cb73834f9`, base develop `433fa6c0f`) |
|---|---|
| `Before` | `scan_next_index_scan ()` S_BEFORE — scan_manager.c:6051, `curr_oidp` = 첫 OID :6090 |
| `Check` | 위치를 정한 뒤 `scan_next_index_lookup_heap ()` :6289 → `eval_data_filter ()` :6421 (curr_oidp 의 행) · 통과하면 `qexec_end_one_iteration ()` query_executor.c:14797 |
| `Check` 의 방향 전환 | `qexec_intprt_fnc ()` query_executor.c:14816-14843 — MIN/MAX 가 끝나지 않았으면 `use_desc_index` 반전·`bts_reset_scan ()`·`position = S_GO_BACKWARD` |
| `Check` 의 `result` | `qdata_evaluate_aggregate_list ()` query_aggregate.cpp:1548-1580 — 정방향 행은 보통 MAX 로 누적, 뒤집은 뒤 첫 행은 누적값을 바꾸고 `is_ended` |
| `Advance` | S_ON :6131 — 다음 OID :6160, 다음 묶음은 `curr_oidno = 0`·`curr_oidp` = 첫 OID :6249, 범위 끝이면 S_END |
| `GoBack` | S_GO_BACKWARD :6095 — 묶음을 반대쪽에서 다시 읽고 `curr_oidno = 0`; PR 은 덮지 않는 스캔의 `curr_oidp` 도 첫 OID 로(:6113-6128), develop 은 그대로(develop :6095-6112) |
| `buf`·`cnt` | `oid_list->oidp`·`oids_count` — 묶음이 짧으면 뒤쪽 칸은 이전 묶음의 OID 를 그대로 가진다 |

## 성질
- `MaxIsRight` — 끝났을 때 결과가 필터를 통과하는 마지막 행(없으면 NULL). 질의가 내야 하는 값.
- `PtrAtOidno` — 행을 읽는 순간 `curr_oidp` 가 `curr_oidno` 칸을 가리킨다(S_BEFORE·S_ON 이 지키는 관계).
- `PtrInBatch` — 읽는 칸이 지금 묶음 안이다.
- `Ends` — 스캔이 끝난다(약한 공정성).

## 결과
(TLC 실행 뒤 채움 — `out/before/result.json`·`out/after/result.json`)

## 모델이 보지 않는 것
- 인덱스가 덮는 스캔(list 를 다시 연다 — 이 PR 이 손대지 않음), `multi_range_opt`, index skip scan, 키 필터, 여러 범위(R_RANGELIST·R_KEYLIST).
- MVCC 가시성·동시 갱신 — 보이지 않는 행은 필터에 걸린 행과 같게 다뤄진다고 보고 따로 두지 않았다.
- `SELECT MIN(b), MAX(b)` 처럼 둘을 함께 구하는 경우의 MIN(정방향 첫 행으로 끝나므로 이 결함과 무관).
