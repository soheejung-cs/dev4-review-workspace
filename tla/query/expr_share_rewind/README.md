# expr_share_rewind — 식 컴파일러가 되감을 때 공유 칸(필터 결과 슬롯을 읽는 칸)의 표시와 개수

- 이슈·PR: CBRD-27994 · CUBRID/cubrid#8469 (2026-11-10 자기 리뷰, head `1de189701` = `212a3fb51` + develop `b0b644811` merge — expr_compile.c 는 merge 로 바뀌지 않음)
- 대상 함수: `expr_build_rewind ()`·`expr_prog_finish ()`(이 PR 이 바꿈), `expr_compile_node ()`·`expr_compile_node_arith ()`·`expr_prog_compile_roots_impl ()`·`expr_valptr_prog_keep ()`·`expr_agg_prog_keep ()`·`expr_image_get_prog ()`(src/query/expr_compile.c), `qdata_agg_operand_prog_compile ()`(src/query/query_aggregate.cpp)
- 종류: 상태기계(동시성 없음 — 빌드 문맥은 컴파일마다 따로 힙에 잡힌다). 왜 모델로 보나: 되감기는 "시도 → 거절 → 그 앞으로 되돌림" 이 루트·하위 트리마다 중첩되는 재시도 루프이고, PR 이 지키겠다는 것은 그 단계들 사이의 관계(n_shared = 공유 칸 수, 되감긴 번호에 표시 없음)다. 단위 suite(`unit/query/expr_compile.share_rewind`)는 고른 순서 몇 개를, 이 모델은 범위 안의 모든 순서(중첩 깊이·칸 수·루트 수·표가 차는 경우)를 본다.
- 실행: `python3 tools/tla/tlc_run.py tla/query/expr_share_rewind/ExprShareRewind.tla --config tla/query/expr_share_rewind/before.cfg --expect violation`
  · `before_keep.cfg`·`before_active.cfg` 도 `--expect violation` · `after.cfg --expect ok`

## 무엇을 모델로 했나
목록 하나의 컴파일. 초기 상태가 목록의 종류(투영 목록 `proj` = only_compute_roots / 집계 피연산자 = wired-only 루트도 둠)와 루트 수(1..MaxRoots)를 고르고, 각 루트는 노드 틀(mark)을 MaxDepth 까지 중첩해 칸을 만든다 — 칸의 종류는 `VAL`(열·리터럴처럼 계산하는 스텝이 없는 칸), `STEP`(계산 스텝의 칸), `SHARE`(필터 결과 슬롯을 읽는 칸). 칸 표는 MaxCells 칸(EXPR_MAX_STEPS 축소)이고 차면 `full` 이 선다.

| 모델 행동 | 코드 (head `1de189701` / base `b0b644811`, src/query/expr_compile.c) |
|---|---|
| `RootBegin` | `expr_prog_compile_roots_impl ()` 의 루트 앞 `expr_build_mark ()` :6658 / :6636 |
| `NodeBegin` | `expr_compile_node ()` 산술 노드 — `depth++` :4740 · `expr_build_mark ()` :4745 / :4729·:4734 |
| `NewCell("VAL")` | 잎 `expr_compile_node_impl ()` TYPE_CONSTANT :5430 · TYPE_DBVAL :5450 → `expr_new_cell ()` :1797 (cell_shared[] 를 쓰지 않음) / :5419·:5439·:1797 |
| `NewCell("STEP")` | `expr_new_step_with_cell ()` :2015 → `expr_new_cell (…, EXPR_CSRC_STEP, …)` :2026 / 같음 |
| `ShareHit` | `expr_compile_node_arith ()` — `expr_new_cell (…, EXPR_CSRC_SHARE, …)` :5342, `cell_shared[cell] = true`·`n_shared++` :5345-5346 (칸이 없으면 셈도 없음) / :5331·:5334-5335 |
| `NodeEnd` | `expr_compile_node ()` 가 되감지 않고 돌아감(:4766-4778) — 노드가 됐거나, 실패해 부모가 이어서 실패(깊이 1 의 표 참은 중첩하지 않음) |
| `NodeNest` | `expr_compile_node ()` 의 `cell < 0 && full && depth > 1` :4758-4765 — `expr_build_rewind ()` :4762 뒤 `expr_compile_nested ()` :5242(새 프로그램 스텝 칸, 또는 하위 컴파일 거절·자리 없음) / :4747-4754·:4751·:5231 |
| `RootReject` | `expr_prog_compile_roots_impl ()` 의 `cell < 0` → `expr_build_rewind ()` :6701, `full = false` :6661 / :6679·:6639 |
| `RootAccept` | 같은 함수 :6662-6696 — only_compute_roots 면 계산 스텝이 그 칸을 내거나 `cell_shared[cell]` :6682 일 때만 남김, 아니면 되감기 / :6640-6674·:6660 · `root_cells[n_roots++]` :6728 / :6706 |
| `Finish` | `expr_prog_finish ()` :6493(`prog->n_shared = bctx->n_shared` :6506, PR 의 debug assert :6622) → 투영 목록 `expr_valptr_prog_keep ()` :6830(`n_compute == 0 && n_shared == 0` 이면 버림 :6838) / 집계 `expr_agg_prog_keep ()` :7209(루트 하나라도 덮으면 둠) → 이미지 `n_shared` :8327 → 서버 `expr_image_get_prog ()` :8687(`n_shared ∈ [0, n_cells]`, 아니면 거절) / base :6482·:6495·(assert 없음)·:6808·:6816·:7187·:8305·:8665 |
| `RewoundFlag`·`RewoundCount` | `expr_build_rewind ()` :2059 — PR 은 [mark->n_cells, n_cells) 의 표시를 지우고 그 수만큼 `n_shared` 를 뺀다 :2075-2085, develop 은 칸 수만 되돌린다 :2059-2083 |
| `Active` | 투영 목록은 위 keep 규칙, 집계는 서버 `qdata_agg_operand_prog_compile ()`(query_aggregate.cpp:1331 `n_compute == 0 && n_shared == 0` → state 3 "plain operands", 아니면 state 1 "active") |

## 성질
- `ImageAccepted` — 클라이언트가 쓴 이미지를 서버가 받는다. 아니면 debug 서버는 `expr_valptr_prog_settle ()` :9178 / `expr_agg_prog_settle ()` :9255 의 `image_size == 0 || prog != NULL` assert 로 죽고, release 는 그 목록을 해석 실행한다(CBRD-27994 증상).
- `KeptRootComputes` — 투영 목록은 무언가 계산하는 칸의 루트만 프로그램에 둔다(only_compute_roots).
- `ActiveComputes` — 소비자가 프로그램을 돌리는 것(투영 목록 keep, 집계 active)은 계산하는 칸이나 필터 값을 읽는 칸이 있을 때뿐이다.
- `CountMatches` — n_shared = 살아 있는 SHARE 칸 수(PR 이 `expr_prog_finish ()` 에서 debug assert 로 지키는 관계 — 모델은 모든 상태에서 본다).
- `FlagIsSrc` — cell_shared[j] ⇔ (j < n_cells ∧ SHARE). 되감긴 번호에 표시가 남지 않는다.
- `TypeOK`.

## 결과
| 날짜 | 대상 | cfg | 판정 | 상태(생성/고유) | 깊이 | 시간 | 도구 |
|---|---|---|---|---|---|---|---|
| 2026-11-10 (.51) | develop 동작(FIX=FALSE) | before.cfg | **ImageAccepted 위반**(기대대로) — 반례 11상태 | 81,512 / 33,527 | 11 | 2.8s | TLC2 2.19 (tla2tools v1.7.4, sha256 `936a2620…050e88`), Java 11 |
| 2026-11-10 (.51) | develop 동작 | before_keep.cfg | **KeptRootComputes 위반**(기대대로) — 반례 9상태 | 34,819 / 15,257 | 9 | 2.4s | 같음 |
| 2026-11-10 (.51) | develop 동작 | before_active.cfg | **ActiveComputes 위반**(기대대로) — 반례 10상태 | 47,599 / 20,055 | 10 | 3.2s | 같음 |
| 2026-11-10 (.51) | PR 헤드 `1de189701`(FIX=TRUE) | after.cfg | **전부 성립** — TypeOK·CountMatches·FlagIsSrc·ImageAccepted·KeptRootComputes·ActiveComputes | 349,362 / 97,192 | 19 | 6.1s | 같음 |

범위: MaxCells 3 · MaxDepth 4(루트 mark + 노드 틀 3) · MaxRoots 3 · MaxFrames 3. 결함이 나타나는 최소(칸 1·루트 2·틀 1)보다 한 단계 이상 크다 — 한 되감기 구간에 공유 칸 둘, 두 겹 중첩 되감기, 표가 차서 중첩(NodeNest)하는 경우가 들어간다.

develop 반례(가장 짧은 것):
- **before.cfg** (집계 피연산자, 루트 2): 루트 1 의 노드 틀에서 공유 적중 둘(칸 0·1, n_shared 2) → 루트 거절 → 되감기(n_cells 0, **n_shared 2 남음**) → 루트 2 는 평범한 열(VAL 칸 0, 집계는 wired-only 루트를 둠) → 이미지 n_shared 2 > n_cells 1 → 서버가 거절. 이슈의 꼴(거절된 루트 뒤에 다시 공유 적중)과 다른 꼴이다 — 코드로는 `[LEAST (LEAST (CAST (c1 AS DOUBLE), CAST (c1 AS DOUBLE)), CASE c1 WHEN c2 …), c1]`: 단위 suite F3 가 develop 에서 투영 목록으로도 같은 거절(n_shared 2 대 VALUE 칸 1)을 재현했다(평범한 열이 남은 표시로 only_compute_roots 를 통과하므로).
- **before_keep.cfg** (투영 목록): 공유 적중 하나를 품은 루트 거절 → 평범한 열이 같은 번호(칸 0)에 만들어지고 남은 표시로 '계산된 칸' 이 되어 남는다 — PR 본문 Remarks 의 '반대 방향' 꼴(`[LEAST (…), c1]`), 단위 F2.
- **before_active.cfg** (집계 피연산자): 같은 순서에서 집계는 평범한 열을 루트로 두고, 남은 n_shared 1 때문에 서버가 그 목록을 plain operands 가 아니라 active 프로그램으로 둔다 — 단위 A2(base n_shared 1).

## 모델이 보지 않는 것
- 스텝·CSE·guard·slot 표를 따로 두지 않았다 — 셋 다 칸 표 하나(`full`)로 묶었다. 표가 차는 자리가 달라도 되감기 범위는 노드 틀의 mark 로 정해지므로 n_shared 관계에는 영향이 없다고 보았다.
- CSE 로 앞 칸을 다시 쓰는 루트는 `RootAccept` 가 살아 있는 아무 칸이나 고르는 것으로 넓게 잡았다(공유 적중은 CSE 를 거치지 않으므로 실제로는 SHARE 칸을 CSE 로 다시 고르지 않는다 — 넓게 잡아도 성질은 그대로).
- `expr_scan_side_compile ()`(스캔 필터 쪽 되감기, :2841) — 필터 빌드는 공유를 붙이지 않아(`expr_share_attach ()` 는 roots_impl 만) n_shared 가 늘 0 이다.
- 이미지 왕복의 나머지(노드 표·스텝 인코딩), debug 의 서버 재컴파일 대조(`expr_image_same_prog ()`) — 클라이언트와 서버가 같은 코드로 같은 순서를 밟는다고 두었다.
- 값·타입·NULL 같은 질의 결과 — 이 PR 은 결과를 바꾸지 않는다(이미지가 거절되면 해석 실행으로 같은 값).
