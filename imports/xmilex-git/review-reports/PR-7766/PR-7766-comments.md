# PR #7766 게시용 리뷰 코멘트 (초안, 미게시)

## C1. 인라인 — `src/query/parallel/px_scan/px_scan.cpp:390`

`NIT:` assert 위치가 세 곳에서 조금 다릅니다. list(`:859`)와 index(`:1307`)는 함수 상단이라 `ACCESS_SPEC_FLAG_NO_PARALLEL_SCAN`으로 이미 병렬이 막힌 spec도 assert를 지나는데, heap은 `if (spec->curent == nullptr)` 안쪽이라 파티션 재오픈 경로는 건너갑니다. 원래 검사가 그 `||` 조건에 섞여 있었으니 위치를 그대로 둔 건 이해합니다. 불변식 확인이 목적이라면 위쪽 `assert (thread_p != nullptr)` 묶음 옆으로 모으는 것은 어떨까요?

## C2. top-level (선택)

게이트가 사문화됐다는 판단은 확인했습니다. `thread_entry` 생성자가 본문에서 `db_create_private_heap ()`을 호출하고(`thread_entry.cpp:189`), 그 함수는 SERVER_MODE에서 조건 없이 `hl_register_lea_heap ()`을 부르는 것으로 알고 있습니다. 워커 풀 엔트리도 `new entry[m_max_threads]`로 같은 생성자를 거치니 예외가 없어 보입니다.

이후 `private_heap_id`를 0으로 바꾸는 곳은 `db_change_private_heap (thread_p, 0)` 관용구와 `px_scan_index_overflow_chain_pool.cpp:43`이 있는데, 전부 `pr_clone_value` / `pr_clear_value` 같은 단말 연산만 감싸는 저장-복원 구간이었습니다. 쿼리 실행 진입점을 감싸는 `qexec_execute_build_indexes` / `qexec_execute_build_columns`도 `qexec_start_mainblock_iterations`까지만 가고 `qexec_open_scan`은 부르지 않네요. 그래서 scan open 시점에는 이 조건이 참이 될 일이 없어 보입니다.
