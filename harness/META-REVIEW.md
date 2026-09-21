# 메타 하네스 리뷰 보고서 — dev4-review-workspace (v2, 2026-09-17, 정소희 세션)

> **TL;DR** (총점 **3.9 / 4**, v1 2.4): v1 에서 "프롬프트에만 있고 코드가 강제하지 않는다"고 적었던 항목을 코드로 내렸다. YAML 이 실행을 소유하는 러너(`pipeline.py`), 예산 초과 시 함수 단위 배치 분할, 불변조건 상시 주입, 변수 단위 관문 짝, finding 스키마 검증 → 재질의 파일, repro 의무 강등, PR 본문 성능 주장 자동 MEAS 검사, 게시 코멘트→episodic 메모리 적재, manifest 결정론 기록(pipeline/rules/skills SHA·codegraph fingerprint·model_id), `ServerSession` RAII + 잔존 정리 + repro 템플릿. **남은 0.1**: LLM 호출 자체의 결정론(temperature/seed)은 하네스 밖이라 기록만 한다.

## 채점표 (v1 → v2)

| Primitive | v1 | v2 | 무엇이 바뀌었나(코드) | 증거(실측) |
|---|---|---|---|---|
| 1. Context Delivery & Compaction | 3.0 | **4.0** | `context_pack.batches()`: 예산 초과 시 **함수(fid) 단위 배치**, 공유 절(invariants·rules·episodic) 배치마다 복제, 한 함수가 혼자 예산을 넘으면 본문을 잘라 "나머지는 줄 범위로 요청" 표시. `_invariants()`: 설계-리뷰-규칙 §2 INV 표 + 손댄 디렉터리 AGENTS.md 의 latch/lock/deadlock 줄을 **우선순위 2 로 항상** 주입. struct 참조를 정규식 대신 tree-sitter `type_identifier`(`fn_types` 테이블)로. episodic 절(우선순위 6). | PR#7899: 31,607 tok → **5 배치**(8.1k/7.9k/7.3k/6.9k/4.4k, 모두 ≤12k, dropped 0) — v1 은 29k 팩 그대로 |
| 2. Determinism & Isolation | 2.5 | **3.9** | `pipeline.py` 가 `harness/pipelines/*.yaml` 을 읽어 실행 — 모르는 impl·비매핑 노드는 **로딩 시 실패**. `manifest.json` 에 `pipeline_sha·harness_git·rules_sha·skills_sha·codegraph_fingerprint·codegraph_complete·model_id·n_batches` 기록 → 두 실행의 차이를 설명 가능. CodeGraph 스키마 버전(`SCHEMA_VERSION`) 불일치 시 캐시 폐기(부분 마이그레이션 금지). `Worktree` RAII 는 `finally` 에서 제거. YAML `executor:` 로 컨테이너 역할 명시. | 7937 manifest: `pipeline_sha 789b945c…, rules_sha fc94453c…, skills_sha f9b795e7…, codegraph_fingerprint 00a76b3a…, complete=True`. 같은 입력 재실행 시 `codegraph: cache hit`. **−0.1**: `model_id` 는 환경변수 기록일 뿐 LLM 결정론은 보장 불가 |
| 3. DB Lifecycle & Resource Management | 2.5 | **3.9** | `session.ServerSession`(with): conf 디렉터리·`databases.txt` 스냅샷 → 격리 포트 → 기동, `__exit__` 에서 stop·복원·`cleanup_leftovers()`(고아 `cub_cas/cub_server` 만, `pgrep -x`). `harness/templates/repro.sh`: 버릴 DB·랜덤 포트·`trap cleanup EXIT INT TERM`(stop·deletedb·conf 복원)·판정은 `$OUT` 파일. `reachability.latch_pairing_by_var()`: **변수 단위** fix/unfix·alloc/free·lock/unlock 짝, 반환·out-param 소유권 이전 제외 → 카운트 방식의 `qo_env_new` 오탐 제거. | 7899 `btree_compact_fix_leaf`: `page`/`child_page` 각각 fix→unfix 대응 확인(facts.var). **−0.1**: 조건 분기(CFG) 를 따라가진 않음 — 후보 표시까지 |
| 4. Verification & Self-Correction Loop | 1.5 | **3.8** | `adjudicate.validate_findings()`(jsonschema, `finding.json`) → 실패는 `requery.json` 으로(이유 포함); `dedup()`; `adjudicate()` 의무 5개; `repro_obligation()`: valid+blocking 코드 finding 은 `projects/<JIRA>/repro/*` 또는 `graph_supports=True` 없으면 **non-blocking 강등**; `self_check_build()`(-fsyntax-only, compile_commands 경로 환경변수); `perf_claims.analyze()`: PR 본문에서 MEAS-01/04/05/07 자동 finding(layer=설계, evidence `pr-body:N`); `episodic.collect()`: 게시된 코멘트·작성자 응답을 accepted/rebutted/open 으로 적재 → 다음 팩에 주입. | 7937 adjudicate: 3 ok/0 schema fail, F2(diff 밖 anchor+미지 규칙 ID) → inconclusive → `requery.json` 1건; 7658 본문 → `MEAS-07-auto`(중앙값만, 산포 없음) 정확히 1건(v1 정규식은 MEAS-01 오탐). **−0.2**: 재질의 자체(LLM 호출)는 러너 밖; self_check 는 이 컨테이너에 compile_commands 가 없어 `ran=False` |

## v2.1 — 통합(full)
`run full` 하나가 리뷰+설계+성능+판정+메모리를 잇는다. 사용자 질문 "리뷰용 하네스가 된 게 맞나 / 설계·성능도 같이 되나"에 대한 답: v2 까지는 세 명령이 따로였고 LLM 단계가 밖이었다. v2.1 은 LLM 에 줄 입력(`review_request.md`)을 러너가 만들고 산출물을 같은 명령이 받아 판정·보고서까지 만든다. LLM 호출 자체는 여전히 이 에이전트가 수행한다(러너 안에 API 호출 없음 — 의도적: 결정론 기록과 사람 승인 지점을 지키기 위해).

## 남은 한계 (정직하게)
- LLM 호출은 여전히 하네스 밖 — 러너는 산출물 파일로만 소통한다. 결정론은 "입력이 같으면 같은 팩·같은 판정"까지고, 모델 출력의 재현은 `model_id` 기록으로 추적만 한다.
- `gate-imbalance`/`latch_pairing_by_var` 는 함수 내 선형 근사. `if/goto` 경로별 검사(CFG)는 다음 단계.
- 매크로가 함수로 잡힘(`NET_SERVER_REQUEST_ITEM`), 함수 포인터·가상 호출 미해석, `changed+1` 범위 밖은 `root(no caller resolved)`.
- `self_check_build` 는 compile_commands.json 이 있는 컨테이너(.50/.52)에서만 실제로 돈다.

## 유지할 것
규칙 ID 인용 · 세 원칙(재현/측정/승인 없이 ~하지 않는다) · 보드 공개 · "설계용/리뷰용 분리, 레퍼런스 공유, TC 별도" 구조.

## 놓친 지적 사례 — PR#7937 (2026-09-17, 타 리뷰어 대조)
같은 head(`02f49316d`) 에 shparkcubrid 가 4건을 올렸다. 하네스 2차(6 findings, 전부 non-blocking) 와 대조: **놓침 2, 동일 1, 범위 밖 1**. 상세 `claude-workspace/projects/CBRD-27186/리뷰대조-PR7937-shparkcubrid-20260917.md`.

| 놓친 것 | 원인 | 하네스에 내릴 것 |
|---|---|---|
| 자식 2·부모 1 이 한 스텝에서 연결되면 FK 바닥이 1/N² (card 10000→1, 🔴) | 작성자 프레임(self-ref 두 제약) 만 감사, `else` 분기의 "eqclass 에 다른 누가 있나" 를 안 물음, `question` 으로 두고 재현 생략 | 카디널리티·선택도 모델 변경(`planner_visit_node`, `qo_*_selectivity*`) 이 diff 에 있으면 **조인 형상 체크리스트**(자식1·부모1 / 자식2·부모1 같은 스텝 / 자식1·부모2 / 체인 / self-ref / semi·anti / outer) 를 불변조건 절에 주입하고, "과소·과대 추정" finding 은 severity 가 question 이어도 **repro 의무** 대상으로 |
| `er_set(ER_OUT_OF_VIRTUAL_MEMORY)` 뒤 `continue` (🟡) | 에러 경로를 콜드패스로 후순위 — 성능 규약을 정확성 축까지 확대 적용 | diff 한정 린트: `er_set(` 다음 문장이 `return`/`goto` 가 아니면 `error-path-continues` 리스크. `gate-imbalance` 와 나란히 |

교훈 한 줄: **PG 규칙을 인용했으면 CUBRID 코드의 어느 줄에서 그 규칙이 깨지는지 대응시켜라** — N2 는 punt 규칙을 정확히 알고도 self-ref 에만 붙였다.

## 놓친 지적 사례 — PR#7658 (2026-09-17, 사용자 질문이 잡음)

식 평가를 스텝 프로그램으로 컴파일하는 변경(CBRD-27215). 하네스 리뷰 2회 + 자기 리뷰 1회를 거쳤는데 **누수 3건이 전부 하네스 밖에서** 나왔다. 사용자가 "no_plan_cache 일 때도 잘 동작하는지 확인이 필요할 것 같다" 고 물은 것이 실마리였다.

셋 다 한 형태다: **같은 계약을 지켜야 하는 자리가 여러 개인데 일부만 고쳤다.** 그리고 셋 다 **답안 비교로는 안 잡힌다** — 결과는 맞고 메모리만 샌다.

| 놓친 것 | 원인 | 하네스에 내릴 것 |
|---|---|---|
| 플랜 캐시 세 모양 중 `max_plan_cache_clones=0` 에서 프로그램이 실행마다 누수. 작성자도 리뷰도 기본(clone 재사용)과 원샷 둘만 보았다 | 검증 규율에 **설정 축이 없었다** — 매트릭스가 전부 기본 설정 전제. 팩도 리뷰어가 본 두 경로만 담았고 `xcache_retire_clone ()` 은 들어오지 않았다 | ① `review-testing` §1-1 설정 변형 매트릭스(**추가함**) ② 팩 확장: 변경이 **구조체에 수명 있는 필드**를 더하면 그 필드를 읽는 **모든** 함수를 팩에 강제 포함 ③ `plan.json` 에 "이 상태를 놓는 자리 전수" 를 요구하는 칸 |
| 슬롯 배열만 `posix_memalign` 직접 호출 → 자체 메모리 모니터 사각. 앞선 리뷰 라운드가 **틀린 전제**("wrapper 는 operator new/delete 만 감쌈")로 지적 없음 판정 | 래퍼의 실제 범위를 확인하지 않고 기억으로 판정 | diff 한정 린트: `posix_memalign|aligned_alloc|mmap|valloc` 이 새로 들어오면 **추적되는 짝(`cub_aligned_alloc`)이 있는지 확인** 을 강제. `er_set` 린트와 나란히 |
| 프로그램 해제용 플래그 한 줄이 **값 해제를 반대로 뒤집음**(px 비클론 워커). 원샷 경로는 이미 2패스로 피하고 있었는데 병렬 워커만 1패스 | 같은 플래그를 반대 방향으로 읽는 자리가 있다는 것을 아무도 대조하지 않음. 전수 훑기는 사람이 따로 탐색 에이전트를 띄워서야 돌았다 | **해제 경로 전수 훑기를 절차에 넣는다**: 수명 있는 상태를 추가하면 `free_*`/`*_decache`/`*_retire` 호출부를 전수로 뽑아 각각 "해제되는가 / 붙을 수 없는가" 판정. 구현 하네스 게이트 후보 |

교훈 한 줄: **"이 상태를 만드는 곳" 이 아니라 "이 상태를 놓는 곳" 을 전수로 세라.** 만드는 곳은 하나지만 놓는 곳은 여러 개다.

측정 도구는 `harness/templates/leak/` 에 넣었다(RSS 배치 비교 · `cubrid memmon` · SA 모드 valgrind). 답안 비교가 못 잡는 종류는 이 셋 중 둘 이상으로 재고, **develop 대조군을 같은 방법으로 함께** 잰다.

## 놓친 지적 사례 — PR#7899 (2026-09-21, xmilex-git 대조)

`ALTER INDEX ... COMPACT` 의 새 서버 요청(CBRD-27401). 하네스 리뷰가 9건(코드 3·설계 6)을 냈는데 **전부 `btree.c`·`storage_common.h`·`csql_grammar.y`·`execute_schema.c`** 였다. xmilex-git 이 올린 지적은 **네트워크 응답 버퍼**였고 하네스는 0건이었다.

### 무엇을 놓쳤나 (사실 확인 완료)

`sbtree_compact_overflow ()` 는 응답 버퍼를 `OR_ALIGNED_BUF (OR_INT_SIZE + OR_INT64_SIZE * 3)` = **28B** 로 잡고 `or_pack_int` 하나 + `or_pack_int64` 셋을 쓴다. 그런데
- `or_pack_int64 ()`·`or_pack_double ()` 는 쓰기 전에 `PTR_ALIGN (ptr, MAX_ALIGNMENT)` 를 한다(`object_representation.c`) — **정렬하는 패커는 이 둘뿐**이다.
- 64비트에서 `OR_ALIGNED_BUF(size)` 는 `char buf[size]` 로 **여유 바이트가 없고**(32비트에서만 `+ MAX_ALIGNMENT`), `OR_ALIGNED_BUF_SIZE` 는 `sizeof (buf)` 그대로다.

따라서 실제 배치는 `int`(0~3) → 패딩(4~7) → `int64`(8~15, 16~23, 24~31) = **32B 필요**. 28B 버퍼에 4바이트를 넘겨 쓰고(스택), 송신 길이는 28B 라 마지막 `pairs_skipped` 가 잘린다. 클라이언트 `btree_compact_overflow ()` 도 같은 28B 선언이고 `or_unpack_int64` 역시 정렬하므로 **양쪽 다** 범위를 넘는다.

### 왜 못 봤나

1. **컨텍스트 문제가 아니다.** 그 두 함수는 팩에 그대로 있었다 — `network_interface_sr.cpp:sbtree_compact_overflow [5123-5149]`, `network_interface_cl.c:btree_compact_overflow [7069-7111]`, `dropped 0`. 코드를 받고도 읽고 넘어갔다.
2. **의무 항목에 직렬화 축이 없다.** 매 리뷰에 주입되는 것은 MEAS-01/04/05/06/07 + CHK-06(측정)과 INV-1~5(설계)뿐이다. 규칙 파일에 SER-01~04 가 있지만 **주입되지 않고**, 게다가 그 넷 중 어느 것도 "버퍼 크기 vs 패커 정렬 패딩"을 다루지 않는다(SER-03 은 *읽을 때* 캐스팅, SER-04 는 예약 필드).
3. **[D] 통신 규칙이 존재만 본다.** 설계-리뷰-규칙의 [D] 행은 "새 요청 추가 시 3파일 동시 변경" — 세 파일이 **바뀌었는지**만 묻고 **크기가 맞는지**는 묻지 않는다. 이 PR 은 3파일을 모두 바꿨으므로 그 체크는 통과한다.
4. **주의가 어려운 축으로 쏠렸다.** latch 커플링·MVCC·PG 대조에 9건을 쓰는 동안 요청/응답 배관은 "보일러플레이트"로 훑었다. 사람이 훑는 자리라는 것이 바로 도구가 봐야 할 이유다.

### 하네스에 내릴 것

| 내릴 것 | 어디에 | 근거 |
|---|---|---|
| **`or-buf-undersized` 결정론 검사기** — 함수 안에서 `OR_ALIGNED_BUF (EXPR) buf` 와 `OR_ALIGNED_BUF_START (buf)` 로 시작하는 `or_(un)pack_*` 체인을 모아 정렬을 시뮬레이션하고 선언값과 비교, 초과면 auto finding | `reachability.py` 의 `gate-imbalance` 옆, 산출은 `findings.auto.json` | 정렬 패커가 `or_pack_int64`·`or_pack_double`(+unpack 짝) **둘뿐**이라 규칙이 닫힌다. 이 결함은 LLM 판단이 아니라 산술이다 |
| 같은 검사를 **서버·클라이언트 쌍**으로 — 같은 `NET_SERVER_*` 상수를 쓰는 sr/cl 두 함수의 응답 크기 식이 다르면 지적 | 위와 같은 자리 | 7899 는 양쪽이 같은 값으로 **같이 틀렸다**. 한쪽만 보면 "일치하니 맞다"로 읽힌다 |
| **SER-05(신규)**: "고정 크기 요청/응답 버퍼는 패커의 정렬 규칙을 포함해 계산한다. `OR_INT_SIZE + OR_INT64_SIZE * n` 식 단순 합은 `or_pack_int64` 앞의 패딩을 빠뜨린다." | `rules/성능-리뷰-규칙.md` SER 절 | 인용할 ID 가 있어야 리뷰가 검증 가능해진다 |
| **주입 조건 확대**: diff 가 `src/communication/` 를 건드리거나 `OR_ALIGNED_BUF`·`or_pack_` 가 나오면 SER 절을 불변조건처럼 **항상** 주입 | `context_pack._invariants()` | 지금은 성능 변경일 때만 의무 항목이 뜬다 |

### 시제품에서 나온 부수 사실

정규식으로 저장소 전수 스캔을 해 봤더니 5건이 걸렸는데 **전부 오탐**이었다 — `ptr` 체인이 함수 경계를 넘어 다음 함수의 패킹까지 이어 붙었다. 하네스 안에서 해야 하는 이유가 여기 있다: CodeGraph 는 이미 함수 범위를 알고 있다.

교훈 한 줄: **배관처럼 보이는 코드가 가장 기계적으로 검증 가능한 자리다.** 사람이 "이건 그냥 요청/응답 boilerplate" 라며 훑고 지나가므로, 그 자리는 LLM 의 주의가 아니라 결정론적 검사로 덮어야 한다.
