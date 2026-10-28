# 융합 하네스 아키텍처 — Metis CodeGraph/Reachability × Azure Architecture-Review 파이프라인

대상: CUBRID C/C++ 엔진의 **설계 리뷰**와 **패치 리뷰**. 두 하네스는 initialize·context·adjudicate 를 공유하고, 설계용만 `infer` 단계를 더 갖는다. 레퍼런스(`rules/`, `references/`)는 공유. TC 변경·CI 분석은 `dev4-tc-workspace` 로 분리.

목차: §1 한 장 그림 · §2 Metis 에서 가져온 것 · §3 Azure 에서 가져온 것 · §4 코드 패턴 · §5 통합 실행(full)과 두 층의 경계(서브에이전트 분해, 하네스가 `references/` 를 읽지 않는 이유) · §6 v2 모듈 · §7 알려진 한계 · §8 정직한 신호와 부가 검사

## 1. 한 장 그림

```
                 ┌──────────────── 결정론 구간 (LLM 없음) ────────────────┐
PR/diff ──▶ Worktree(RAII) ──▶ CodeGraph(tree-sitter→sqlite3) ──▶ diff→함수 매핑 ──▶ Reachability(관문·진입점 경로, 짝 검사)
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
                 Adjudicate(의무 5개: anchor·in-diff·evidence·rule·graph) ──▶ valid/invalid/inconclusive ──▶ self_check(-fsyntax-only)*
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

## 5. 통합 실행(full) 과 두 층의 경계
\* `self_check` 는 `HARNESS_COMPILE_COMMANDS`(기본: 빌드트리의 `compile_commands.json`)가 있으면 그 명령으로 `-fsyntax-only`, 없거나 비어 있으면(goto 빌드 디렉터리) **ninja 오브젝트 타깃으로 실제 컴파일**한다(`adjudicate.self_check_ninja` — 빌드 디렉터리가 가리키는 메인 체크아웃 소스만). 건너뛴 검사가 있으면 `GATE OK*` 로 찍힌다. `compile_commands.json` 만드는 법은 리포 README 의 `ninja -t compdb` 한 줄.

`run full --pr N` 은 리뷰용·설계용을 **한 번**에 돈다: initialize → context(성능 규칙 행 + 설계 불변조건 + episodic) → infer(arch) → **review_request**(LLM 입력: 공통 지시·두 규칙의 의무 항목·자동 MEAS finding·arch 요약·finding 스키마는 `preamble.md` 에 **한 번**, 컨텍스트 팩은 `review_request.batch<N>.md` 에 배치별 — 배치가 하나면 둘을 합친 `review_request.md`. 배치 지도·그룹 제안은 `batch_index.{md,json}`. **배치 번호 N 은 `context_pack.batch<N>.md` 의 N 과 같다**) → [LLM 이 findings.json 작성] → `--findings` 로 verify(validate→gate→self_check→report.md) → memory(episodic). 층 구분은 finding 의 `layer` 필드와 게시 코멘트 첫 줄 태그로만 남는다. review/design/adjudicate yaml 은 이 그래프의 부분집합이다(디버그용).

> **단위 테스트는 리뷰의 표준 단계다**(사용자 지시 2026-10-16) — 결정론 구간은 `run full` 의 context 단계 `unit_regress`(결과는 preamble·report), LLM 구간은 `skills/harness-review/SKILL.md` 절차 3-1(정본).

### 두 층의 경계(finding 단위)
| | 리뷰용(review) | 설계용(design) |
|---|---|---|
| 입력 | PR diff | PR diff 또는 설계 문서(`--doc`, Azure 식 텍스트 유추는 후속) |
| 추가 단계 | — | infer(arch.json, 리스크, 다이어그램) |
| LLM 스킬/규칙 | `code-review` + `성능-리뷰-규칙` | `design-review` + `설계-리뷰-규칙` |
| finding.layer | 코드 (anchor 가 diff 안이어야 valid) | 설계 (anchor 는 코드 줄이되 diff 밖 허용, `arch_edge` 인용 필수) |
| 산출물 | 보고서 + 인라인 코멘트 | 대안표·결정 요청 Q + 덱 + 다이어그램 |
| 공유 | Worktree · CodeGraph · ContextPack · Adjudicate · `references/` · `rules/` · `examples/` | |

> **LLM 구간의 서브에이전트 분해(2026-10-08)**: 수집 `harness-pack-reader`·`harness-threads` → 판정 `harness-reviewer`·`harness-repro` → 반증 `harness-refuter` → 종합 `harness-synthesizer` → **감시 `harness-gatekeeper`(게시 전)·`harness-auditor`(작업 끝)**. 정의 `agents/*.md`, 표·흐름 `agents/README.md`. 러너는 그대로 파일로만 소통한다 — 에이전트들도 `OUT/` 의 파일로 주고받는다.
> 여러 에이전트로 쪼갤 때의 모델·추론 강도 등급은 [모델-선택.md](모델-선택.md) —
> 수집은 낮추고 판정·종합·반증은 유지, 애매하면 올려보낸다(사용자 결정 2026-09-21).

> **하네스는 `references/` 를 읽지 않는다.** `context_pack._rule_rows()` 와 `_invariants()` 가 긁는 것은 `rules/*.md` 의
> 판정 행(`| <규칙 ID> | …`)뿐이다. ID 꼴은 `tools/harness/rule_ids.py` 한 곳 — `MEM-03`·`MEAS-R-26`·`APX-A-01`·`PRIO-04.5`·`INV-1`·`A01`,
> ID 칸이 둘인 행(`BR-04/A59`)은 둘 다 그 행의 ID 다. 같은 꼴을 finding 스키마의 rule_ids pattern 과 판정기의 '아는 ID' 가 쓴다(H-28).
> 따라서 **인용 가능한 판정 행은 `rules/` 에 두고** `references/` 에는 근거·해설만 둔다. 같은 표를 양쪽에 두면 갈라진다.
> `_rule_rows()` 는 ID 당 1행(처음 나온 행), 축은 ID 의 번호 앞 부분(+ 문장의 'X 위반' 이 가리키는 축 — 17장 안티패턴 행), 상한 60행(넘으면 뺀 수를 적는다).
> 관문 사실로 안 잡히는 축은 디렉터리로 더한다(`AXIS_BY_DIR`: `src/optimizer` → MEAS-R).
> `_invariants()` 는 항상 주입(잘리지 않음), `_rule_rows()` 는 우선순위 5 라 예산이 모자라면 통째로 잘린다 —
> 잘리면 안 되는 축은 `_invariants()` 로 승격한다(현재 INV-* 전부 + BTL-*는 `src/storage`·`src/transaction` 을 건드릴 때만).
> 손댄 모듈의 소스 지식 `dev4-ai-source/modules/<모듈>.md` §3(이미 규명된 함정)은 `known pitfalls` 절로 항상 주입한다(우선순위 2, 그 모듈의 함수가
> 든 배치에만, 해소됨 항목은 첫 줄만, 모듈당 3,000 tok 상한 — H-29). 노트 리포 위치는 `DEV4_SOURCE_NOTES`, 읽은 판은 manifest `source_notes_git`.

## 6. v2 에서 추가된 모듈
| 모듈 | 역할 |
|---|---|
| `pipeline.py` | YAML 러너 — `harness/pipelines/{review,design,adjudicate}.yaml` 의 stage/node 를 레지스트리로 실행, `manifest.json` 결정론 기록 |
| `perf_claims.py` | PR 본문 성능 주장 → MEAS-01/04/05/07 자동 finding |
| `episodic.py` | 게시된 리뷰 코멘트 + 작성자 응답 → `examples/episodic/PR-<n>.json` (accepted/rebutted/open) |
| `unitseam.py` + `unitseam_prelude.cpp` | 리뷰용 단위 테스트(gha-ci 아님): TU 를 그 모드(server/cs/sa)의 빌드 트리 플래그로 worktree 에서 단독 컴파일 → seam(지정 함수 weak 화)·appendix(TU 끝 훅)·prelude(부팅 기본값 전역)·자동 stub(미해석 함수=trap/0, 데이터=0 4KiB) → Catch2 v2 링크·실행, `--rev` 반복으로 head/base 비교. 에이전트 `harness-unit` (2026-10-16) |
| `unit_regress.py` + `unit/` | 리뷰용 단위 suite 라이브러리(`unit/<module>/<suite>/suite.json`). `run full` 의 context 단계 `unit_regress` 가 PR 이 건드린 파일(`file`·`watch`)에 걸린 suite 를 head/base 에 돌려 `[contract]` 회귀 후보·`[defect]` 재현 여부를 `unit_regress.md` 로 preamble·report 에 싣는다(`HARNESS_NO_UNIT=1` 로 끔) |
| `session.py` | `ServerSession` RAII(conf·databases.txt 복원, 종료 보장), `cleanup_leftovers()` |
| `harness/templates/repro.sh` | trap 정리·격리 포트·산출물 판정 템플릿 |
| `context_pack.batches()` / `_invariants()` / `_known_pitfalls()` / `_episodic()` | 배치 분할 · 불변조건 상시 주입 · 손댄 모듈 노트 §3 주입(그 모듈 배치에만, H-29) · 과거 지적 주입 |
| `rule_ids.py` | 규칙 ID 꼴·축·판정 행 파싱의 한 곳(H-28) — `_rule_rows`·finding 스키마 pattern·판정기의 '아는 ID' 가 같이 쓴다 |
| `precheck.py` | 그룹 findings 에 판정기의 결정론 검사만(OUT 의 codegraph·pr.diff 를 읽기 전용으로, head 파일은 `git show`) → `precheck.<g>.json`. 반증(`harness-refuter`)의 입력 — refuter 는 기계 검사를 다시 하지 않는다(H-31). 최종 판정 산출물은 쓰지 않는다 |
| `reachability.latch_pairing_by_var()` | 변수 단위 관문 짝 |
| `adjudicate.validate_findings/dedup/repro_obligation/requery` | 스키마 검증 → 재질의 파일, 중복 제거, repro 의무. repro 의무의 산출물 경로는 `adjudicate.records_root()` 가 `DEV4_RECORDS_ROOT` → `roster.json:records_root` → 기본값 순으로 해석하고, **그 경로가 없으면 강등하지 않고 blocking 을 유지**한다(평가 불가 ≠ 재현 없음) |
| `static_lint.py` | 결정론 린트 노드 `static_lint`(context 단계, or_buf 뒤 — findings.auto.json 에 append, 집계·건너뛴 수 `static_lint.json`): 실수 인자 바깥 꼴 범위 검사(`fp-range-outside`, 손댄 파일에서 고친 꼴이 남은 자리는 `-siblings` 한 건으로), encode/coerce 상태 버림(`discarded-status`), er_set 뒤 NULL 자원 사용·continue(`error-path-continues`), 추적 안 되는 정렬 할당(`untracked-aligned-alloc`), 추정 모델 변경 시 조인 형상 체크리스트(`join-shape-checklist`). CLI 는 rev 전체 훑기(오탐률·전후 비교) — H-15·H-33 |
| `md_lint.py` | 게시 본문 교정·검사의 한 곳(문장 뒤 울타리 분리·울타리 홀수 거부·절/열거 복원·길이 경고) — `review_reply`·`post_review` 가 같이 쓴다(H-32) |
| `repro_check.py` | 코멘트 초안의 `sql` 울타리 블록만 절마다 버릴 DB·새 SA 프로세스로 develop·PR 에 돌려 `repro_check.{json,md}`, `--verify` 는 게이트키퍼용(초안 sha1·전부 돌았나) — H-27 |
| `source_facts_check.py` | 소스 지식 문서(dev4-ai-source staging·modules, skills)의 file:line·심볼 인용을 항목의 기준 rev 와 develop HEAD 에서 확인(ok·moved·changed·norev·error) — K-05, H-18 |
| `implement_gate` (6) | 수명 있는 상태(구조체 포인터 필드·plan.lifetime_state)를 더하면 그래프가 뽑은 해제 경로 후보 하나하나가 plan.release_sites 에 판정돼 있어야 통과 — H-33 |

## 7. 알려진 한계 (META-REVIEW v2 참조)
- 매크로(`NET_SERVER_REQUEST_ITEM`)가 함수로 잡힌다; 함수 포인터·가상 호출 미해석; `changed+1` 범위 밖 호출은 `root(no caller resolved)`.
- `gate-imbalance`(카운트)는 여전히 오탐 가능; 팩에는 변수 단위 짝(`pairing by variable`)이 함께 실려 LLM 이 대조할 수 있다. CFG 경로별 검사는 미구현.

## 8. 정직한 신호와 부가 검사 (현재 동작 — 도입 경위는 git log)
- **파서 품질**: codegraph 는 파일별 `parse_quality`(잃은 함수 정의 수 — 휴리스틱 정의 시작, static_assert·attribute·프로토타입·연속줄 제외)를 manifest 에 적고, 10% 초과 **그리고** 3개 이상 잃은 파일이 있으면 `codegraph_complete=false`. C 문법이 잃으면 C++ 문법으로 재시도(.c 도 C++ 로 컴파일되는 저장소), 세미콜론 없는 최상위 매크로 호출 줄은 파싱 전에 지우고, `#if/#else/#endif` 로 헤더가 갈린 debug/release 쌍은 첫 헤더만 남겨 파싱한다. 핫 파일(expr_compile·page_buffer·btree·heap_file·system_parameter)은 0 lost. C++ 구문이 섞인 파일(list_file.c·file_io.c 등)은 줄을 잃을 수 있다 — `codegraph_parse_quality` 에 뜬 파일은 팩·관문 판정이 불완전할 수 있다.
- **컴파일 확인**: implement_gate 는 건너뛴 검사가 있으면 `GATE OK*`. compile_commands 가 없으면 ninja 오브젝트 타깃으로 컴파일(`adjudicate.self_check_ninja`), 워크트리는 `ninja -t commands` 의 컴파일 명령을 빌려 `-fsyntax-only`. `--commits` 로 커밋 단위 컴파일(`compile_check`).
- **adjudicate**: `passed`(통과 의무) 기록, `mentions_resolve`(지적이 `name ()` 로 부르는 함수가 그래프에 있나), `verification.artifact` 경로 존재 확인(static 인데 file:line 인용이 없으면 표시).
- **pipeline**: `pr_refs` 노드(context 단계), `--sha/--base` 로컬 head 리뷰, arch 의 `include_edges`(diff 의 새 `#include` → 계층 간선) + `new-include` 리스크(바닥으로 내려가면 무해, 바닥이 위를 include 하면 high), diff 의 모드 가드(`#if … SERVER_MODE|CS_MODE|SA_MODE`) 추가·삭제를 INV-3 신호로.
- **episodic**: 로컬 판정도 적재(`source=local`), 팩의 episodic 절이 `[local valid/non-blocking]` 로 표시, 작은 episodic 절은 팩 예산에서 잘리지 않음.
- **도구**: `review_threads`(스레드 분류) · `pr_refs`(본문 참조 확인) · `compile_check`(커밋 단위 컴파일) · `review_reply.py`(답글 초안 ↔ 스레드 dry-run/게시) · `tools/ab/`(A/B 표준, `ab_pair.sh`). 구조도: `harness/구조도.html`.
