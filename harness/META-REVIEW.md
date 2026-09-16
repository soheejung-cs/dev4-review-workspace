# 메타 하네스 리뷰 보고서 — dev4-review-workspace (2026-09-16, 정소희 세션)

> **TL;DR** (총점 **2.4 / 4**): 이 하네스의 강점은 *절차 규율*(REVIEW-절차, 규칙 ID 인용, 사람 승인 후 게시)과 *도메인 지식의 파일화*(rules/·modules/·examples/)다. 약점은 그 규율이 **거의 전부 프롬프트(SKILL.md)에 있고 코드가 강제하지 않는다**는 것 — 오늘 `tools/harness/` 로 initialize/context/adjudicate 를 코드로 내렸지만 LLM 산출물이 스키마를 지키는지, 실행 검증(CTP·벤치)이 격리되는지는 아직 사람이 지킨다. 4 Primitives 중 **Verification & Self-Correction 이 가장 낮다(1.5)**.

## 채점표

| Primitive | 점수 | 근거(현 상태) | 리팩토링 포인트(우선순위) |
|---|---|---|---|
| 1. Context Delivery & Compaction | **3.0** | ○ `context_pack.py`: 변경 함수 → 그래프 증거 → struct → 서명 → 규칙 행의 5단 우선순위, 예산 12k, 중복 제거, 거대 함수 창 절단. ○ `rules/` 두 문서가 5,145줄 레퍼런스를 ID 표로 압축(261+). ○ `modules/*.md` §2·§3 가 "이미 아는 함정"을 모듈별로 보관. ✕ 예산 초과 시 **배치 분할 미구현**(PR#7899: 29,314 tok 팩 그대로). ✕ struct 추출이 정규식(`[A-Z_]{4,}`) — 실제 참조 타입 해석이 아님. ✕ 락 순서 규칙(AGENTS.md storage "parent before child") 이 팩에 자동으로 들어가지 않음(사람이 안다). | **P1** Metis `_build_file_grouped_node_chunks` 식 파일 단위 배치 → LLM 여러 번 호출 + `finding_dedup`. **P1** 락 순서·계층 규칙을 `rules/`에서 팩 §"invariants" 로 항상 주입(우선순위 2). **P2** tree-sitter 로 타입 참조(`type_identifier`)를 직접 뽑아 struct 1-hop 정확화. |
| 2. Determinism & Isolation | **2.5** | ○ CodeGraph: 정렬 입력·fingerprint 캐시·모호 호출 미해석 유지 → 같은 입력 = 같은 SQLite. ○ Worktree RAII 로 작업 트리 오염 없음. ○ LLM 은 파일 산출물로만 소통(도구 호출 없음). ✕ **LLM 호출 자체의 결정론은 보장 못 함**(temperature/seed 계약 없음, 모델 버전 미기록 — manifest 에 없음). ✕ CTP 가 `$CUBRID/conf` 를 바꿔 놓는 부작용(ha_mode/port)이 규칙 문서에만 있고 코드로 복원하지 않음. ✕ 측정 격리는 `host_idle_guard.sh` 수동 호출. | **P1** manifest 에 `model_id, prompt_sha, rules_sha, codegraph_fingerprint` 기록 → 재실행 비교 가능. **P2** `ServerSession` 컨텍스트 매니저(conf 백업/복원, databases.txt 복원, 종료 보장). **P2** 리뷰 실행은 `.51`, 벤치는 `.50/.52` 라는 컨테이너 분리를 YAML `executors:` 로 명시. |
| 3. DB Lifecycle & Resource Management | **2.5** | ○ 하네스는 DB 서버를 띄우지 않는다(리뷰는 정적, 실행 검증은 요청자 확인 후) — 오염 표면 자체가 작다. ○ 재현 스크립트 규약: demodb/버릴 DB 에서만, 벤치 볼륨 금지. ✕ 재현 스크립트(`repro/*.sh`)에 **trap 기반 정리(서버 stop·deletedb)가 규약으로 강제되지 않음** — CBRD-27369 스크립트는 개별 구현. ✕ CTP 잔존 프로세스(PL 연결이 IX 를 쥔 `-494` 캐스케이드)를 하네스가 감지·정리하지 않음. ✕ `heap_attrinfo`/MemoryContext 류 **누수 검출은 사람 리뷰**(코드 패턴 검사 없음) — 오늘 `latch_pairing` 카운트가 첫 자동화. | **P1** `repro` 템플릿: `trap 'cubrid server stop; cubrid deletedb' EXIT`, 포트·SHM 격리 값 자동 부여. **P2** `gate-imbalance` 탐지기를 CFG 기반(에러 경로별 unfix 존재)으로 — tree-sitter `if_statement`/`goto` 추적. **P3** CTP 후 잔존 `cub_*` 정리 워커(`pgrep -x` 만 사용). |
| 4. Verification & Self-Correction Loop | **1.5** | ○ `adjudicate.py` 의무 5개 + inconclusive 강제(오늘 추가). ○ 절차상 "재현 없이 결함이라 하지 않는다". ✕ **LLM 산출물이 finding.json 스키마를 통과하는지 검증하는 코드가 없음**(스키마 파일만). ✕ `self_check_build` 는 `-fsyntax-only` 뿐 — 동적 검증(단위 재현 실행) 연결 없음. ✕ 비평 → 재검토 루프(Metis 의 "critique 후 재질의")가 없음: 지금은 사람(사용자)이 그 루프. ✕ 과거 오탐 기록(episodic memory) 을 다음 리뷰가 자동 참조하지 않음. | **P1** `validate_findings(schema)` + 위반 시 LLM 재질의 1회(구조화 실패 시 배치 분할, Metis 규칙). **P1** valid finding 마다 재현 의무: `repro/` 스크립트 존재 또는 `graph_supports=True` 없으면 non-blocking 으로 강등. **P2** 게시된 코멘트의 작성자 응답(수용/반박)을 `examples/` 에 episodic 기록으로 자동 적재 → 팩 우선순위 6. |

## 취약점 상세 (증거 포함)

1. **프롬프트 규율 ≠ 코드 강제.** `skills/*/SKILL.md` 9개(657줄)가 절차의 정본인데, 이를 읽지 않는 실행 경로(다른 에이전트·다른 계정)에는 아무 효력이 없다. 오늘 `harness/pipelines/*.yaml` 로 그래프를 적었지만 **YAML 을 읽어 실행하는 러너는 아직 없다**(`run.py` 가 하드코딩). → Metis 처럼 YAML 이 실행을 소유하게 러너를 만들거나, 최소한 `run.py` 가 YAML 과 어긋나면 실패하게.
2. **finding 의 근거 불일치를 사람이 잡고 있었다.** 오늘 PR#7658 리뷰에서 `memory_wrapper.hpp` 가 free 를 감싸는지, `regu_alloc` 이 0 초기화하는지는 내가 grep 으로 확인했다 — `adjudicate.graph_supports` 는 관문 짝만 본다. 확인 절차 자체를 `obligations` 로 확장(예: `claim` 에 함수명이 있으면 그 정의를 팩에 포함했는지).
3. **성능 주장 검증이 하네스 밖.** 규칙(MEAS-01~07)은 있으나 PR 본문의 표를 파싱해 "중앙값만/산포 없음/1회 실행"을 자동 지적하는 코드가 없다(오늘 PR#7658·7937 에서 손으로 지적). → PR 본문 표 파서 + MEAS 규칙 자동 finding(layer=설계).
4. **TC 하네스와 리뷰 하네스가 한 리포에 섞여 있었다.** `tc-analysis`·`gha-ci`·`tests/shell-tc-index`·`tools/tc_*` 는 리뷰 판정과 목적이 다르고(TC 답안 정렬은 코드 수정을 수반) 비공개 TC 내용을 다룬다 → `dev4-tc-workspace` 로 분리(이 커밋).
5. **결정론 기록 부재.** `examples/리뷰보고서-*.md` 에 HEAD SHA 는 있지만 사용한 규칙 문서 버전·모델은 없다. 두 달 뒤 같은 PR 을 다시 돌려 차이를 설명할 수 없다.

## 잘 된 것 (유지)
- 규칙을 ID 로 인용해 검증 가능하게 한 것, 그리고 레퍼런스를 "성능 볼 때 한 문서 / 설계 볼 때 한 문서"로 압축한 것 — Context Compaction 의 핵심이 이미 사람 손으로 돼 있다.
- "재현 없이 결함이라 하지 않는다 / 측정 없이 빠르다 하지 않는다 / 사용자 검토 없이 게시하지 않는다" 세 원칙이 모든 스킬을 관통.
- 리뷰 산출물이 보드(8827)로 팀에 공개되어 리뷰가 검토 가능한 산출물이 됨.

## 다음 3 커밋 제안
1. `context_pack`: 파일 단위 배치 분할 + invariants 절 상시 주입 (P1×2)
2. `adjudicate`: finding.json 스키마 검증 + 재질의 훅 + repro 의무 (P1×2) + manifest 에 model/prompt/rules SHA
3. `run.py` → YAML 러너(stage/node 등록), `ServerSession`/repro 템플릿 (P2)
