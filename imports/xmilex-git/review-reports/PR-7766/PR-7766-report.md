# PR #7766 코드 리뷰 보고서

**PR:** [CUBRID/cubrid#7766](https://github.com/CUBRID/cubrid/pull/7766)
**제목:** [CBRD-27290] Remove the dead private_heap_id == 0 gate from parallel scan open
**작성자:** HyunukLee
**HEAD SHA:** `f4786a0a07d0ed1132d56546c9ba053af45de911`
**리뷰 일시:** 2026-08-25

> **TL;DR** (Non-blocking): 제거 대상 게이트가 실제로 사문화된 코드임을 확인했다. `private_heap_id`는 heap 등록 OOM 외에는 어떤 스레드에서도 0이 되지 않으므로 이 검사는 스레드 역할을 구분한 적이 없고, 제거는 동작 중립이다. assert 배치가 세 곳에서 일관되지 않은 점만 남는다.

## Summary

- **변경 요약**: `px_scan.cpp`의 heap(:390) / list(:859) / index(:1307) scan open 3곳에서 `thread_p->private_heap_id == 0` 검사를 제거하고 `assert`로 대체
- **주요 이슈**: 없음
- **확인 필요 사항**: 없음

---

## Findings

### Blocking (must fix)
없음.

### Non-blocking (should consider)

- **게이트가 사문화됐다는 전제를 확인했다.** 검증 결과를 남긴다. `thread_entry::entry()` 생성자는 초기화 목록에서 `private_heap_id (0)`으로 두지만(`thread_entry.cpp:96`), 같은 생성자 본문에서 `thread_entry.cpp:189`의 `private_heap_id = db_create_private_heap ();`으로 실제 heap을 할당한다. `db_create_private_heap` (`memory_alloc.c:296-305`)은 SERVER_MODE에서 조건 없이 `hl_register_lea_heap ()`을 호출하며, 이 함수가 0을 돌려주는 것은 heap 등록 자체가 OOM으로 실패한 경우뿐이다. 즉 스레드 역할과 무관하다. 워커 풀 엔트리도 `thread_manager.cpp:94`의 `new entry[m_max_threads]`로 같은 생성자를 거치므로 예외가 없다. 이후 필드를 0으로 쓰는 곳은 `db_change_private_heap (thread_p, 0)` 관용구(약 60곳)와 `px_scan_index_overflow_chain_pool.cpp:43`의 `db_private_set_heapid_to_thread (NULL, 0)`인데, 모두 `pr_clone_value` / `pr_clear_value` / `tp_value_coerce` 같은 단말 DB_VALUE 연산만 감싸는 저장-복원 구간이다. 쿼리 실행 진입점을 감싸는 유일한 두 구간(`query_executor.c:24757`의 `qexec_execute_build_indexes`, `:25731`의 `qexec_execute_build_columns`)도 `qexec_start_mainblock_iterations`까지만 호출하며 이 함수는 list file과 확장 해시 파일만 열고 `qexec_open_scan`을 부르지 않는다. 따라서 scan open 시점에 이 검사가 참이 된 적이 없고, 제거는 동작 중립이다.

- **세 assert의 배치가 일관되지 않는다.** list(`:859`)와 index(`:1307`)에서는 함수 상단, `ACCESS_SPEC_FLAG_NO_PARALLEL_SCAN` 조기 반환보다 앞에 있어 병렬 금지로 명시된 spec도 assert를 평가한다. 반면 heap에서는 `if (spec->curent == nullptr)` 블록(`:381`) 안쪽이라 파티션 재오픈 경로는 건너뛴다:
  ```cpp
  /* px_scan.cpp:381 */
  if (spec->curent == nullptr)
    {
      ...
      assert (thread_p->private_heap_id != 0);
  ```
  heap의 경우 원래 검사가 그 `||` 조건의 일부였으니 위치 보존은 이해된다. 다만 불변식 확인이 목적이라면 세 곳 모두 함수 상단의 기존 `assert (thread_p != nullptr)` 묶음과 나란히 두는 것이 일관돼 보인다.

### Questions for the author
없음.

## JIRA Context

CBRD-27290 (Sub-task, Open, 부모 CBRD-27152)의 목표는 이 게이트 제거이고 변경 범위는 그 안에 있다. 수용 기준은 세 게이트 제거, 병렬 스캔 CTP 회귀에서 develop 대비 신규 실패 없음, 회귀 실행 중 assert 미발동이다. 위 첫 항목의 검증 결과는 세 번째 기준이 충족될 것임을 뒷받침한다.

## Existing Comments

작성자/메인테이너의 미해결 top-level 코멘트는 없다. 이슈 코멘트는 CI 봇 알림과 작성자의 `/run all` 1회뿐이다.
