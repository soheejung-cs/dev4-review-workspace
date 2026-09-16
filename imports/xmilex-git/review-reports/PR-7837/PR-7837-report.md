# PR #7837 코드 리뷰 보고서

**PR:** [CUBRID/cubrid#7837](https://github.com/CUBRID/cubrid/pull/7837)
**제목:** [CBRD-00000] Remove CAS: direct driver connection to multithreaded cub_server
**작성자:** xmilex-git
**HEAD SHA:** `eedf508b240085bfbef7d022f0d6dd5098c17b7e`
**리뷰 일시:** 2026-09-01

> **TL;DR** (Blocking): 리뷰 11건 중 5건은 코드와 CI에서 반영을 확인했고, 1건은 일부만 반영됐으며, 1건은 수정이 동작하지 않고, NIT 4건은 후속 작업으로 미뤄졌습니다. adoption endpoint 실패가 여전히 성공으로 반환되는 결함을 고쳐야 합니다.

## Summary

- **변경 요약**: CAS를 `cub_server`에 통합한 231파일 변경에서 1차 리뷰 후속 커밋 `c6f1bd140`을 최신 HEAD 기준으로 재검증
- **주요 이슈**: endpoint 실패 상태 미전파 1건, pre-admission 예외 처리 불완전 1건
- **확인 필요 사항**: 미완료 NIT 4건의 처리 시점과 fixed thread 7건의 resolve 여부

---

## Findings

### Blocking (must fix)
- `src/connection/server_support.c:712` - `cubconn::adoption::start()` 실패 시 `status`가 초기값 `NO_ERROR`인 채 `shutdown`으로 이동하고 `css_init()`가 그 값을 반환하므로, 에러를 기록해도 호출자는 기동 실패를 받지 못합니다.

### Non-blocking (should consider)
- `src/connection/adoption.cpp:880` - `std::make_shared<channel>()`와 `m->channels.emplace()`가 `try` 밖에 있어 `std::bad_alloc`은 accept thread 밖으로 전파되어 `std::terminate`를 일으키므로, 원래 리뷰에서 지적한 per-connection allocation 실패 경로는 일부만 처리됐습니다.
- GitHub review thread 11개가 모두 unresolved 상태이므로, 수정 완료 5건은 검증 후 resolve하고 미완료 6건은 열린 상태로 구분해야 합니다.

### Questions for the author
- 파일+라인 주석, 작업 표식 주석, include 순서, magic number의 NIT 4건을 이 PR에서 고치지 않고 분할 PR에서 처리한다면 추적할 issue/PR 좌표가 필요합니다.

## JIRA Context

제목의 `CBRD-00000`은 JIRA에서 404로 조회되어 실제 요구사항과 변경 범위를 대조할 수 없습니다. PR 본문도 CI용 integration draft이며 별도 reviewable PR로 분할할 예정이라고 명시합니다.

## Existing Comments

| Status | Count | Evidence |
|---|---:|---|
| 반영 확인 | 5 | body length 상한, shutdown UAF 회피, TLS config snapshot, SIGINT handler 분리, 표준 license |
| 일부 반영 | 1 | channel 수와 `std::thread` 실패는 처리했으나 `make_shared`/`emplace` 실패는 미처리 |
| 수정 무효 | 1 | adoption endpoint 실패 후 `status = ER_FAILED` 누락 |
| 후속 처리 예정 | 4 | stale file:line, stage/wf 표식, include 순서, magic number |

모든 thread에는 작성자 답변이 있으나 GitHub의 `isResolved` 값은 11건 모두 `false`입니다.

## Verification

- `license`, `pr-style`, `code-style`, `memory-monitor-check`: 성공
- `cppcheck`, CircleCI release/debug build: 확인 시점에 실행 중
- `Check TC PRs`: TC draft PR 2개가 열려 있어 실패; 코드 결함 신호는 아님
- 리뷰 후속 수정에 대한 targeted runtime test 증거는 아직 확인되지 않음
