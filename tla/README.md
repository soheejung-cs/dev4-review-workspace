# tla/ — TLA+ 명세 저장소

harness-review 하위 스킬 [`skills/harness-review/tla.md`](../skills/harness-review/tla.md)가 쓰는 명세를 모듈별로 쌓는다(`unit/<모듈>/<suite>/` 와 같은 자리).
다음 PR 이 같은 함수를 바꾸면 그 명세를 그 PR 판으로 맞춰 다시 돌린다(회귀). 실행 도구는 `tools/tla/tlc_run.py`, 후보 고르기는 `tools/tla/tla_candidates.py`.

디렉터리 하나 = 모델 하나: `<Spec>.tla` · `before.cfg`(develop, 위반 기대) · `after.cfg`(PR, 성립 기대) · `README.md`(대상 함수, 모델 ↔ file:line 대응표, 성질, 결과 표, 모델이 보지 않는 것). `out/` 은 커밋하지 않는다.

| 명세 | 대상 함수 | 이슈·PR | 종류 | 결과 |
|---|---|---|---|---|
| [query/scan_minmax_turn](query/scan_minmax_turn/README.md) | `scan_next_index_scan ()` S_GO_BACKWARD · `qexec_intprt_fnc ()` MIN/MAX 방향 전환 | CBRD-28007 · #8472 | 상태기계 | 11-10: develop 위반(반례 7걸음) · PR 성립(17,848 상태) |
