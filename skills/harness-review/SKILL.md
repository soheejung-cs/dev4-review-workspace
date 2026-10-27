---
name: harness-review
description: 통합 리뷰 하네스 실행 — `/harness-review <PR번호>`. 코드+설계+성능 입력(review_request.md)을 만들고, 그것을 읽어 findings.json 을 쓰고, 판정·report.md·episodic 까지 한 번에. "하네스 돌려", "PR N 리뷰 하네스", "/harness-review 7937".
---

# /harness-review <PR>

인자: PR 번호 또는 PR 제목/브랜치/JIRA 키(`$ARGUMENTS`). 저장소는 `CUBRID/cubrid`.

## 0. 번호 확정
숫자가 아니면 `gh pr list -R CUBRID/cubrid --state open --search "<인자>" --json number,title,author -q '.[]|"\(.number) \(.title) \(.author.login)"'` 로 찾는다. 후보가 둘 이상이면 표로 보이고 사용자에게 고르게 한다(추측 금지). 0건이면 `--state all` 로 한 번 더.

## 절차 (LLM 호출 = 이 세션의 나 — 서브에이전트 사슬로 돌릴지는 아래 「서브에이전트 구성」)
1. **시작할 때 `references/자가진화-규칙.md` 에서 이 PR 이 건드린 축의 규칙을 먼저 훑는다**(2026-10-23). 그다음 `cd ~/dev/docs/dev4-review-workspace && python3 -m tools.harness.run full --pr <PR>` → 마지막 줄이 산출 디렉터리 `OUT`.
   **미push 커밋을 자기 리뷰**할 때는 `--sha <로컬 rev> --base <기준>`(기준 생략 시 upstream/develop; 머지 커밋을 주면 그 뒤 커밋만) — PR head 대신 로컬 리비전으로 팩을 만든다(2026-09-30). manifest 의 `local_head: true`.
2. `OUT/preamble.md`(공통 지시·의무 항목·finding 스키마)를 **한 번** 읽고, `OUT/review_request.batch<N>.md`(팩)를 **번호 순으로 전부** 읽는다(다른 파일은 열지 않는다 — 팩에 없는 코드는 "없음"). 배치가 하나면 자립 파일 `review_request.md` 하나다.
   **예외**: 손댄 모듈의 `dev4-ai-source/modules/<모듈>.md` §3(이미 규명된 함정)은 팩에 들어오지 않으므로 읽는다(주입 도구화 H-29 전까지).
   배치가 많으면 **`OUT/batch_index.md` 를 먼저** 본다 — 배치별 파일·변경 함수 수와 **그룹 제안**(연속 배치를 주 파일로 묶은 것)이 있다. 어느 배치에 무엇이 들었는지 번호로 짐작하지 않는다. `manifest.json` 의 `codegraph_complete` 가 false 면 보고서에 적고 `codegraph_parse_quality`(파일별 파서가 잃은 함수 정의 수/전체)의 파일은 팩이 불완전할 수 있다고 본다.
   본문이 기대는 외부 사실은 `python3 -m tools.harness.pr_refs --pr <PR>` 로 한 번 확인한다 — 참조 PR 의 상태(미머지로 닫힌 것을 "포트했다" 고 적었나), 언급한 커밋 해시가 head 에 있나. 어긋나면 그 자체가 지적(`문서 제안` 또는 `설계 판정`)이다.
3. 지적을 `OUT/findings.json` 으로 쓴다 — **필드·필수 여부·쓰는 법의 정본은 preamble 의 지시 3~3-8 과 스키마 `harness/schemas/finding.json`** 이다(모든 지적에 `why`·`proposal`, 에러 우려는 확인(static/dynamic) 없이는 `question` — 사용자 지시 2026-09-17: 지적만 있고 결과가 없는 코멘트는 작성자가 우선순위를 판단할 수 없다). preamble 에 아직 없는 두 가지도 지킨다: **재현·예시 SQL 은 최소 테이블로**(사용자 지시 2026-10-08, `sql-difftest` §2-4), ```suggestion **울타리는 줄 맨 앞에서 열고 닫는다**(`review-response` 「게시 형식」). `findings.auto.json`(MEAS) 은 건드리지 않는다(러너가 합친다).
3-1. **단위 테스트 — 모든 PR 리뷰에 붙인다**(사용자 지시 2026-10-16 — "PR 을 리뷰할 때마다 단위 테스트를 진행") — `harness-unit` / `tools/harness/unitseam.py`, 목 허용, gha-ci 용 아님.
   - **(a) 회귀**: `run full` 의 `unit_regress` 가 `unit/` 에 쌓인 suite 중 이 PR 이 건드린 파일 것을 head/base 에 자동으로 돌린다. `[contract]` 회귀 후보·`[defect]` 재현 안 됨은 지적 후보로 본다.
   - **(b) 지적 확인**: 버그 가능성 🟡 이상은 게시 전에 동적으로 확인한다 — SQL 로 닿으면 `harness-repro`(SA), 함수 단위면 `harness-unit`. 결과는 finding `verification.method=dynamic`, `artifact=OUT/unit/<id>/`.
   - **(c) 계약 suite**: 지적이 없어도 이 PR 의 **핵심 신설·변경 함수 1~3개**에 `[contract]` 케이스(정상 경로 + 경계 1개 이상)를 만들어 `unit/<module>/<suite>/` 에 쌓는다 — 다음 PR 이 같은 파일을 건드리면 (a) 가 돌린다. 고르는 기준: 상태기계·판정 함수·경계 계산처럼 입력→출력이 정해지는 것, 서버 자원은 seam 으로 끊을 수 있는 것.
   - 보고서 §0 의 '리뷰 신뢰도' 에 단위 결과(suite 수·케이스 head/base·재현 여부)를 한 줄로 적는다. 단위로 못 닿는 지적은 그 이유(필요한 seam 이 너무 많음 등)를 적는다.
4. `python3 -m tools.harness.run full --pr <PR> --findings OUT/findings.json` → `findings.adjudicated.json`, `requery.json`, `report.md`.
5. `requery.json` 이 비어 있지 않으면 **한 번만** 그 항목을 다시 검토해 findings.json 을 고치고 4 를 재실행한다(그래도 inconclusive 면 보고서 "판정 보류"에 남긴다).
6. `report.md` 를 사람 말투로 다듬되, **맨 앞에 `## 0. 먼저 알아야 할 것`** 을 둔다 — 항목의 정본은 `code-review` §5 표(9항목, 하네스 골격 report.md 에는 없다). 서브에이전트 사슬이면 synthesizer 의 `report.head.md` 를 검토·보완하고, 단일 세션이면 직접 쓴다. 그다음 `claude-workspace/projects/<JIRA>/리뷰보고서-PR<PR>.md`(비공개)에만 둔다 — 이 리포는 공개라 `examples/` 에 사본을 두지 않는다(2026-10-27: 공개 사본에 참조 구현 언급이 섞여 있었다). **게시는 하지 않는다** — 사용자에게 `comments.draft.md`(형식 정본: `tools/harness/post_review.py` 머리말) 초안을 보이고 승인 후 `post_review --draft` 로 인라인 게시 — 초안이 없는 지적이 있으면 도구가 게시를 거부한다.
   **전달물은 보고서 파일이 아니라 7 의 리뷰 전용 PR 이다**(사용자 지시 2026-10-08): 보고서를 다듬은 뒤 반드시 7 로 가서 fork 에 draft PR 을 만들고 보고서 전문을 PR 본문에 넣어 그 링크를 건넨다. 본문은 `gh api -X PATCH repos/<내 fork>/pulls/<n> --input body.json` 으로 넣고 `gh pr view --json body` 로 줄 수를 확인한다(`gh pr edit --body-file` 은 Projects(classic) GraphQL 오류로 조용히 실패 — H-25). `function_notes` 항목에는 그 함수에 달린 지적의 `finding_ids` 를 넣는다(주석에 `[지적 A1]` 로 찍힌다).
6-1. **주석 제안 기준**(사용자 지시 2026-10-27): 게시 초안에 `[주석 제안]` 은 ① 지금 있는 주석이 코드·동작과 다를 때(`comment_basis: mismatch`) ② 이 PR 이 새 API 를 만들어 쓰임을 제한해야 할 때(`new_api_contract`)만 넣는다. 그 밖의 주석 제안은 게시하지 않는다 — 판정기가 `withheld` 로 빼고 게시 도구도 건너뛴다(보고서 '게시하지 않음' 절과 변경 지도에만 남는다).
7. **변경 지도·주석 PR**: 리뷰 단계가 `function_notes` 를 냈으면 `OUT/function_notes.json` 으로 모아 두면 `report` 가 §변경 지도를 자동으로 깐다. 읽기용 PR 이 필요하면
   `python3 -m tools.harness.annotate --pr <N> --head <sha> --notes OUT/function_notes.json --push --pr-create` (fork 안 draft, upstream 아님).
8. 기록: `record` 스킬(episodic 은 러너가 이미 적재).
9. **자가진화 루프** — `/self-evolution <PR>`(2026-10-23): 이 리뷰가 잡은 결함 유형·놓칠 뻔한 것·하네스가 틀린 것을 `references/자가진화-규칙.md` 에 규칙으로, 기계로 막을 수 있는 것은 하네스 패치 대기열(H-NN)로.

## 서브에이전트 구성
**기본값: 메인 세션 모델이 Opus 면 아래 사슬이 기본, 그 밖의 모델이면 단일 세션(절차 2~3 을 이 세션이 직접, 필요할 때만 띄운다) — 사용자 결정 2026-10-27.** 사슬이어도 규모는 복잡도에 맞춘다(`harness/하네스-작성-규칙.md` S-05). 구성의 출발은 사용자 지시 2026-10-08 "서브에이전트를 여러 개 두고 감시 역할".
정의는 `agents/*.md`(링크 `tools/agent_link.sh` → `~/.claude/agents/`), 표와 흐름은 [`agents/README.md`](../../agents/README.md). 등급·강도·동시 실행 한도는 [`harness/모델-선택.md`](../../harness/모델-선택.md).
사슬일 때 절차 2~3 을 아래로 바꾼다 (메인 루프 = 나는 **승인·게시·사용자 대화만**):
1. `batch_index.md` 의 그룹 제안대로 그룹마다 **`harness-reviewer`(결함 판정 → `findings.<g>.json`)와 `harness-notes`(변경 지도 → `function_notes.<g>.json`)를 병렬로** 띄운다 — 판정은 **4개씩**(모델-선택 「한도」, `chunk_hint`), 변경 지도(sonnet·low)는 **2개씩** 따로(사용자 결정 2026-10-27 A+B: 판정 하나 35~42분 중 큰 몫이 함수 전수의 develop 전/후 읽기였다 — PR#8213). 판정 에이전트에게는 "function_notes 는 쓰지 않는다" 를 프롬프트에 적는다. 프롬프트에는 `OUT`·`preamble.md` 경로·배치 파일 목록·그룹 이름만(지시문을 복사하지 않는다).
   `harness-pack-reader`(수집)는 **기본 생략**(B) — 판정 에이전트가 팩을 직접 깊게 읽어 관찰이 겹친다. 그룹이 크고(12배치 이상) 함수 경계가 자주 잘린 팩이거나, 판정 에이전트에게 미리 좁혀 줄 질문이 필요할 때만 먼저 띄운다(결과는 반환 메시지 → 메인 루프가 `observations.<g>.md` 로 저장).
2. `harness-refuter` 에 findings 전부 → `refute.<g>.json`.
2-1. 단위 테스트(절차 3-1)는 `harness-unit` 에 맡긴다.
3. `harness-synthesizer` → `OUT/findings.json`·`comments.draft.md`·`report.head.md`.
4. 절차 4~5(`--findings` 판정·requery) 는 그대로.
5. **`harness-gatekeeper`** 에 `comments.draft.md` 와 재현 스크립트 경로 → `gate.*.md`. 차단급이 있으면 고쳐서 다시 돌린다 — 게시하지 않는다.
6. 사용자 승인 → 게시(review-response 규약) → **`harness-auditor`** 로 기록·배운것·보드·환경 복원 점검 → 미완이면 처리.
에이전트는 이 대화를 보지 못한다: 입력은 파일 경로, 출력은 파일. `needs_judgment` 가 올라오면 그 항목만 내가 판정한다.

## 하지 않는 것
- 팩 밖 저장소 탐색(절차 2 의 예외 제외), 줄 번호 추측, 판정 없이 게시, CTP/벤치 실행(`review-testing` 으로 제안만).
