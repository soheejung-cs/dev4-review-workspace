# tla/ — TLA+ 명세 저장소

harness-review 하위 스킬 [`skills/harness-review/tla.md`](../skills/harness-review/tla.md)가 쓰는 명세를 모듈별로 쌓는다(`unit/<모듈>/<suite>/` 와 같은 자리).
다음 PR 이 같은 함수를 바꾸면 그 명세를 그 PR 판으로 맞춰 다시 돌린다(회귀). 실행 도구는 `tools/tla/tlc_run.py`, 후보 고르기는 `tools/tla/tla_candidates.py`.

디렉터리 하나 = 모델 하나: `<Spec>.tla` · `before.cfg`(develop, 위반 기대) · `after.cfg`(PR, 성립 기대) · `README.md`(대상 함수, 모델 ↔ file:line 대응표, 성질, 결과 표, 모델이 보지 않는 것). `out/` 은 커밋하지 않는다.

| 명세 | 대상 함수 | 이슈·PR | 종류 | 결과 |
|---|---|---|---|---|
| [query/scan_minmax_turn](query/scan_minmax_turn/README.md) | `scan_next_index_scan ()` S_GO_BACKWARD · `qexec_intprt_fnc ()` MIN/MAX 방향 전환 | CBRD-28007 · #8472 | 상태기계 | 11-10: develop 위반(반례 7걸음) · PR 성립(17,848 상태) |
| [query/expr_share_rewind](query/expr_share_rewind/README.md) | `expr_build_rewind ()`·`expr_prog_finish ()` 공유 칸 표시·개수 · `expr_prog_compile_roots_impl ()` 루트 거절/only_compute_roots · 이미지 리더 | CBRD-27994 · #8469 | 상태기계 | 11-10: develop 위반 3종(ImageAccepted 11상태·KeptRootComputes 9·ActiveComputes 10) · PR 성립(97,192 상태) |
| [transaction/session_sleeper_conn](transaction/session_sleeper_conn/README.md) | `logtb_set_tran_index_interrupt ()` SERVER_MODE 블록 · `logtb_interrupt_session_sleeper_mapfunc ()` 다른 작업의 conn_entry 읽기 | CBRD-27960 · #8427(+ 리뷰 스레드 r4489520248) | 동시성 | 11-10: develop NULL 역참조(반례 7·11상태) · PR 성립(5,222 상태) · 스레드(17런): PR·제안 위반, client_id 검증 스케치 성립(4,962 상태) |
| [query/clear_all_lists_sibling](query/clear_all_lists_sibling/README.md) | `qexec_clear_all_lists ()` 행마다 정리의 범위 · 병렬 부질의 실행의 작업 분배 | CBRD-27959 · #8436 | 동시성 | 11-10: develop 위반(반례 9걸음 — 둘째 갈래에만 부질의여도) · PR 성립(222,360 상태) |
