# 메타 하네스 리뷰 보고서 — dev4-review-workspace (v2, 2026-09-17, 정소희 세션)

> **TL;DR** (총점 **3.9 / 4**, v1 2.4): v1 에서 "프롬프트에만 있고 코드가 강제하지 않는다"고 적었던 항목을 코드로 내렸다. YAML 이 실행을 소유하는 러너(`pipeline.py`), 예산 초과 시 함수 단위 배치 분할, 불변조건 상시 주입, 변수 단위 관문 짝, finding 스키마 검증 → 재질의 파일, repro 의무 강등, PR 본문 성능 주장 자동 MEAS 검사, 게시 코멘트→episodic 메모리 적재, manifest 결정론 기록(pipeline/rules/skills SHA·codegraph fingerprint·model_id), `ServerSession` RAII + 잔존 정리 + repro 템플릿. **남은 0.1**: LLM 호출 자체의 결정론(temperature/seed)은 하네스 밖이라 기록만 한다.

## 채점표 (v1 → v2)

| Primitive | v1 | v2 | 무엇이 바뀌었나(코드) | 증거(실측) |
|---|---|---|---|---|
| 1. Context Delivery & Compaction | 3.0 | **4.0** | `context_pack.batches()`: 예산 초과 시 **함수(fid) 단위 배치**, 공유 절(invariants·rules·episodic) 배치마다 복제, 한 함수가 혼자 예산을 넘으면 본문을 잘라 "나머지는 줄 범위로 요청" 표시. `_invariants()`: 설계-리뷰-규칙 §2 INV 표 + 손댄 디렉터리 AGENTS.md 의 latch/lock/deadlock 줄을 **우선순위 2 로 항상** 주입. struct 참조를 정규식 대신 tree-sitter `type_identifier`(`fn_types` 테이블)로. episodic 절(우선순위 6). | PR#7899: 31,607 tok → **5 배치**(8.1k/7.9k/7.3k/6.9k/4.4k, 모두 ≤12k, dropped 0) — v1 은 29k 팩 그대로 |
| 2. Determinism & Isolation | 2.5 | **3.9** | `pipeline.py` 가 `harness/pipelines/*.yaml` 을 읽어 실행 — 모르는 impl·비매핑 노드는 **로딩 시 실패**. `manifest.json` 에 `pipeline_sha·harness_git·rules_sha·skills_sha·codegraph_fingerprint·codegraph_complete·model_id·n_batches` 기록 → 두 실행의 차이를 설명 가능. CodeGraph 스키마 버전(`SCHEMA_VERSION`) 불일치 시 캐시 폐기(부분 마이그레이션 금지). `Worktree` RAII 는 `finally` 에서 제거. YAML `executor:` 로 컨테이너 역할 명시. | 7937 manifest: `pipeline_sha 789b945c…, rules_sha fc94453c…, skills_sha f9b795e7…, codegraph_fingerprint 00a76b3a…, complete=True`. 같은 입력 재실행 시 `codegraph: cache hit`. **−0.1**: `model_id` 는 환경변수 기록일 뿐 LLM 결정론은 보장 불가 |
| 3. DB Lifecycle & Resource Management | 2.5 | **3.9** | `session.ServerSession`(with): conf 디렉터리·`databases.txt` 스냅샷 → 격리 포트 → 기동, `__exit__` 에서 stop·복원·`cleanup_leftovers()`(고아 `cub_cas/cub_server` 만, `pgrep -x`). `harness/templates/repro.sh`: 버릴 DB·랜덤 포트·`trap cleanup EXIT INT TERM`(stop·deletedb·conf 복원)·판정은 `$OUT` 파일. `reachability.latch_pairing_by_var()`: **변수 단위** fix/unfix·alloc/free·lock/unlock 짝, 반환·out-param 소유권 이전 제외 → 카운트 방식의 `qo_env_new` 오탐 제거. | 7899 `btree_compact_fix_leaf`: `page`/`child_page` 각각 fix→unfix 대응 확인(facts.var). **−0.1**: 조건 분기(CFG) 를 따라가진 않음 — 후보 표시까지 |
| 4. Verification & Self-Correction Loop | 1.5 | **3.8** | `adjudicate.validate_findings()`(jsonschema, `finding.json`) → 실패는 `requery.json` 으로(이유 포함); `dedup()`; `adjudicate()` 의무 5개; `repro_obligation()`: valid+blocking 코드 finding 은 `projects/<JIRA>/repro/*` 또는 `graph_supports=True` 없으면 **non-blocking 강등**; `self_check_build()`(-fsyntax-only, compile_commands 경로 환경변수); `perf_claims.analyze()`: PR 본문에서 MEAS-01/04/05/07 자동 finding(layer=설계, evidence `pr-body:N`); `episodic.collect()`: 게시된 코멘트·작성자 응답을 accepted/rebutted/open 으로 적재 → 다음 팩에 주입. | 7937 adjudicate: 3 ok/0 schema fail, F2(diff 밖 anchor+미지 규칙 ID) → inconclusive → `requery.json` 1건; 7658 본문 → `MEAS-07-auto`(중앙값만, 산포 없음) 정확히 1건(v1 정규식은 MEAS-01 오탐). **−0.2**: 재질의 자체(LLM 호출)는 러너 밖; self_check 는 이 컨테이너에 compile_commands 가 없어 `ran=False` |

## 남은 한계 (정직하게)
- LLM 호출은 여전히 하네스 밖 — 러너는 산출물 파일로만 소통한다. 결정론은 "입력이 같으면 같은 팩·같은 판정"까지고, 모델 출력의 재현은 `model_id` 기록으로 추적만 한다.
- `gate-imbalance`/`latch_pairing_by_var` 는 함수 내 선형 근사. `if/goto` 경로별 검사(CFG)는 다음 단계.
- 매크로가 함수로 잡힘(`NET_SERVER_REQUEST_ITEM`), 함수 포인터·가상 호출 미해석, `changed+1` 범위 밖은 `root(no caller resolved)`.
- `self_check_build` 는 compile_commands.json 이 있는 컨테이너(.50/.52)에서만 실제로 돈다.

## 유지할 것
규칙 ID 인용 · 세 원칙(재현/측정/승인 없이 ~하지 않는다) · 보드 공개 · "설계용/리뷰용 분리, 레퍼런스 공유, TC 별도" 구조.
