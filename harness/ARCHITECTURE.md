# 융합 하네스 아키텍처 — Metis CodeGraph/Reachability × Azure Architecture-Review 파이프라인

대상: CUBRID(및 PG 계열) C/C++ 엔진의 **설계 리뷰**와 **패치 리뷰**. 두 하네스는 initialize·context·adjudicate 를 공유하고, 설계용만 `infer` 단계를 더 갖는다. 레퍼런스(`rules/`, `references/`)는 공유. TC 변경·CI 분석은 `dev4-tc-workspace` 로 분리.

## 1. 한 장 그림

```
                 ┌──────────────── 결정론 구간 (LLM 없음) ────────────────┐
PR/diff ──▶ Worktree(RAII) ──▶ CodeGraph(tree-sitter→SQLite) ──▶ diff→함수 매핑 ──▶ Reachability(관문·진입점 경로, 짝 검사)
                                        │                                                      │
                                        ▼                                                      ▼
                          [design] arch_infer: 계층 컴포넌트·계층 간 호출 ──▶ 규칙 리스크 ──▶ arch.json / .mmd / .excalidraw
                                        │
                                        ▼
                 ContextPack(예산 12k tok, 우선순위 5단, 중복 제거, 초과 시 파일 단위 배치) ──▶ context_pack.md
                 ┌──────────────── LLM 구간 (입력 = pack + arch.json, 출력 = finding.json 스키마) ────────────────┐
                 │  review: skills/code-review + rules/성능-리뷰-규칙   /   design: skills/design-review + rules/설계-리뷰-규칙  │
                 └──────────────────────────────────────────────────────────────────────────────────────────────┘
                                        ▼
                 Adjudicate(의무 5개: anchor·in-diff·evidence·rule·graph) ──▶ valid/invalid/inconclusive ──▶ self_check(-fsyntax-only)
                                        ▼
                 Publish: 보고서(로컬) · 인라인 코멘트(사용자 승인 후) · 덱(아티팩트 허용 시)
```

## 2. Metis 에서 가져온 것과 어떻게 붙였나

| Metis 메커니즘 | 원문 위치 | 이 하네스 | 차이 |
|---|---|---|---|
| CodeGraph = 심볼·호출·위치·언어 파생 사실만, LLM 추론 없음 | `engine/codegraph/models.py` | `tools/harness/codegraph.py` `FunctionNode/CallSite/DomainFact` | 언어 파생 사실을 **DB 관문**(pgbuf_fix/unfix, lock_object, log_append, sysop, alloc/free)으로 특화 — 사전설계 13장 P3 의 관문 목록이 프로파일 |
| Tree-sitter 제공자 + `.metis/codegraph.sqlite3` 영속 | `nodes/codegraph/store.py` | `CodeGraph.build()` → `out/<pr>/<sha>/codegraph.sqlite3`, 입력 fingerprint 로 캐시 | 범위를 `changed+1`(변경 파일 + 같은 디렉터리)로 제한 — 수백만 줄 전체 파싱 대신 호출 해석에 필요한 최소 |
| Reachability(`max_path_length`, fail-open) | `nodes/reachability/*` | `reachability.paths_to_gates / paths_from_entrypoints` | 보안 sink 대신 **관문**(latch/lock/log)과 **서버 진입점**(`x*`, `s*`, `net_server_*`) — "이 변경을 어느 요청이 태우고, 어느 관문을 지나나" |
| 결정론적 증거 → 프롬프트(`prompt_evidence.py` 의 압축·span 치환) | `nodes/reachability/prompt_evidence.py` | `context_pack.py` 우선순위·중복 제거·예산 절단 | 규칙 행(rules/ 의 ID 표)을 diff 가 건드린 축만 골라 넣는다 |
| Triage: obligation → 결정론 gate → SARIF 주석 | `docs/triage-flow.md` | `adjudicate.py` 의무 5개 + `finding.json` 스키마 | SARIF 대신 finding.json; "invalid→valid 직행 금지·의무 미충족 시 inconclusive 강제" 규칙 그대로 |
| Navigation 도구의 경계(코드베이스 안, 시간·출력 한도) | `docs/capabilities/navigation.md` | Worktree 밖 접근 없음, `gh` 만 외부 호출 | LLM 도구 호출은 이 CLI 에 없다 — 산출물 파일로만 소통(재현 가능) |
| Repository memory(semantic/episodic/procedural) | `docs/repository-memory.md` | `dev4-ai-source/modules/*.md`(semantic) · `examples/`+`projects/`(episodic) · `skills/`+`rules/`(procedural) | 파일 기반·git 이력이 authority. FTS 는 없음(gap) |

## 3. Azure Architecture Review Agent 에서 가져온 것

| Azure 메커니즘 | 원문 위치 | 이 하네스 | 차이 |
|---|---|---|---|
| smart_parse: 형식 판별 → 규칙 파서 → LLM 유추 폴백 | `tools.py parse_architecture / infer_architecture_llm` | `arch_infer.infer()` — 입력이 텍스트가 아니라 **CodeGraph** | 컴포넌트 = 사전설계 §3 계층([A]~[I]), 연결 = 계층 간 해석된 호출(횟수·샘플 3개) |
| 템플릿 리스크(SPOF·fan-in·shared-DB·anti-pattern) | `_detect_spof/_detect_scalability/...` | `detect_risks()` 5종: layer-cycle(§4 공식 사이클 4개 밖) · layer-inversion(base 가 위를 호출) · gate-imbalance(관문 짝) · latch-then-lock(래치 든 채 락 요청) · hot-shared(fan-in ≥20) | 리스크는 규칙(결정론), 설명만 LLM |
| 컴포넌트 맵(fan-in/out, orphan) | `build_component_map` | `arch.json.connections[].calls`, `touched_layers` | — |
| Excalidraw 요소·PNG | `generate_excalidraw_elements/_layout/_rect/_arrow` | `to_excalidraw()`(레이아웃 4열 그리드) + `to_mermaid()` | 변경이 닿은 계층은 채색, 위반 간선은 점선 |
| 구조화 보고서(요약·권고·심각도 버킷) | `build_review_report` | `finding.json` → 보고서 템플릿(`examples/리뷰보고서-예시-*.md`) | 사람 검토 후 게시 |

## 4. 코드 패턴 (실제 구현에서 발췌)

### 4.1 결정론적 CodeGraph 빌드 — 입력 fingerprint 캐시, 정렬된 입력, 해석 실패는 "미해석"으로 남긴다
```python
files = sorted(set(files))                                   # 입력 순서 무관
key = sha1(repo_fingerprint(repo) + '\n'.join(files))        # HEAD + dirty 상태 + 파일 목록
if meta.input_fingerprint == key: return cached
...
def _resolve(self):   # 같은 파일 static 우선 → 전역 유일 이름 → 그 외는 NULL(미해석). 모호함을 추측하지 않는다.
```
### 4.2 Reachability 의 fail-open
```python
if g.unresolved(cur): complete = False          # 경로에 미해석 호출이 있으면 '경로 없음' 을 단정하지 않는다
out.append(Path(path, reason, complete))        # LLM 프롬프트에 "[incomplete]" 로 표기된다
```
### 4.3 ContextPack 의 예산 절단 — 우선순위 낮은 것부터, 변경 함수·그래프 증거(1·2)는 절대 안 잘린다
```python
for s in sorted(sections, key=lambda s: (-s.priority, -s.tokens)):
    if total <= budget: break
    if s.priority <= 2: continue
    drop(s)                                       # dropped 목록은 pack 끝에 남겨 LLM 이 "없는 것"을 알게 한다
```
거대 함수(>400줄)는 변경 줄 ±40 만 넣고 `(N lines total, showing changed window)` 를 붙인다.
### 4.4 Adjudicate 의 의무 게이트 (Metis triage 미러)
```python
if anchor_exists and evidence_resolves and graph_supports is not False and (layer != '코드' or anchor_in_diff): status = 'valid'
elif graph_supports is False or not anchor_exists: status = 'invalid' if not anchor_exists else 'inconclusive'
```
`graph_supports`: 주장이 "unfix 누락/누수" 류면 그 함수의 관문 짝 카운트와 대조한다. 균형이면 모순 → inconclusive(조건 분기 확인 요구).
### 4.5 격리·자원 — Worktree RAII
```python
with Worktree(repo, sha, base) as wt:      # git worktree add --detach; __exit__ 에서 예외가 나도 remove --force
    g = CodeGraph.build(wt, ...)
```
DB 서버·CTP 는 이 CLI 가 띄우지 않는다(`review-testing` 스킬의 요청자 확인 절차). 띄우게 되면 같은 패턴의 `ServerSession` 컨텍스트 매니저로 기동/정지·`databases.txt` 복원·conf 복원을 묶는다(META-REVIEW §3 의 리팩토링 항목).

## 5. 두 하네스의 경계
| | 리뷰용(review) | 설계용(design) |
|---|---|---|
| 입력 | PR diff | PR diff 또는 설계 문서(`--doc`, Azure 식 텍스트 유추는 후속) |
| 추가 단계 | — | infer(arch.json, 리스크, 다이어그램) |
| LLM 스킬/규칙 | `code-review` + `성능-리뷰-규칙` | `design-review` + `설계-리뷰-규칙` |
| finding.layer | 코드 (anchor 가 diff 안이어야 valid) | 설계 (anchor 는 코드 줄이되 diff 밖 허용, `arch_edge` 인용 필수) |
| 산출물 | 보고서 + 인라인 코멘트 | 대안표·결정 요청 Q + 덱 + 다이어그램 |
| 공유 | Worktree · CodeGraph · ContextPack · Adjudicate · `references/` · `rules/` · `examples/` | |

## 6. v2 에서 추가된 모듈
| 모듈 | 역할 |
|---|---|
| `pipeline.py` | YAML 러너 — `harness/pipelines/{review,design,adjudicate}.yaml` 의 stage/node 를 레지스트리로 실행, `manifest.json` 결정론 기록 |
| `perf_claims.py` | PR 본문 성능 주장 → MEAS-01/04/05/07 자동 finding |
| `episodic.py` | 게시된 리뷰 코멘트 + 작성자 응답 → `examples/episodic/PR-<n>.json` (accepted/rebutted/open) |
| `session.py` | `ServerSession` RAII(conf·databases.txt 복원, 종료 보장), `cleanup_leftovers()` |
| `harness/templates/repro.sh` | trap 정리·격리 포트·산출물 판정 템플릿 |
| `context_pack.batches()` / `_invariants()` / `_episodic()` | 배치 분할 · 불변조건 상시 주입 · 과거 지적 주입 |
| `reachability.latch_pairing_by_var()` | 변수 단위 관문 짝 |
| `adjudicate.validate_findings/dedup/repro_obligation/requery` | 스키마 검증 → 재질의 파일, 중복 제거, repro 의무 |

## 7. 알려진 한계 (META-REVIEW v2 참조)
- 매크로(`NET_SERVER_REQUEST_ITEM`)가 함수로 잡힌다; 함수 포인터·가상 호출 미해석; `changed+1` 범위 밖 호출은 `root(no caller resolved)`.
- `gate-imbalance`(카운트)는 여전히 오탐 가능; 팩에는 변수 단위 짝(`pairing by variable`)이 함께 실려 LLM 이 대조할 수 있다. CFG 경로별 검사는 미구현.
