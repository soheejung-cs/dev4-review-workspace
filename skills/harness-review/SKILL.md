---
name: harness-review
description: 통합 리뷰 하네스 실행 — `/harness-review <PR번호>`. 코드+설계+성능 입력(review_request.md)을 만들고, 그것을 읽어 findings.json 을 쓰고, 판정·report.md·episodic 까지 한 번에. "하네스 돌려", "PR N 리뷰 하네스", "/harness-review 7937".
---

# /harness-review <PR>

인자: PR 번호 또는 PR 제목/브랜치/JIRA 키(`$ARGUMENTS`). 저장소는 `CUBRID/cubrid`.

## 0. 번호 확정
숫자가 아니면 `gh pr list -R CUBRID/cubrid --state open --search "<인자>" --json number,title,author -q '.[]|"\(.number) \(.title) \(.author.login)"'` 로 찾는다. 후보가 둘 이상이면 표로 보이고 사용자에게 고르게 한다(추측 금지). 0건이면 `--state all` 로 한 번 더.

## 절차 (LLM 호출 = 이 세션의 나)
1. `cd ~/dev/docs/dev4-review-workspace && python3 -m tools.harness.run full --pr <PR>` → 마지막 줄이 산출 디렉터리 `OUT`.
   **미push 커밋을 자기 리뷰**할 때는 `--sha <로컬 rev> --base <기준>`(기준 생략 시 upstream/develop; 머지 커밋을 주면 그 뒤 커밋만) — PR head 대신 로컬 리비전으로 팩을 만든다(2026-09-30). manifest 의 `local_head: true`.
2. `OUT/preamble.md`(공통 지시·의무 항목·finding 스키마)를 **한 번** 읽고, `OUT/review_request.batch<N>.md`(팩)를 **번호 순으로 전부** 읽는다(다른 파일은 열지 않는다 — 팩에 없는 코드는 "없음"). 배치가 하나면 자립 파일 `review_request.md` 하나다.
   배치가 많으면 **`OUT/batch_index.md` 를 먼저** 본다 — 배치별 파일·변경 함수 수와 **그룹 제안**(연속 배치를 주 파일로 묶은 것)이 있다. 어느 배치에 무엇이 들었는지 번호로 짐작하지 않는다. `manifest.json` 의 `codegraph_complete` 가 false 면 보고서에 적고 `codegraph_parse_quality`(파일별 파서가 잃은 줄 비율)의 파일은 팩이 불완전할 수 있다고 본다.
   본문이 기대는 외부 사실은 `python3 -m tools.harness.pr_refs --pr <PR>` 로 한 번 확인한다 — 참조 PR 의 상태(미머지로 닫힌 것을 "포트했다" 고 적었나), 언급한 커밋 해시가 head 에 있나. 어긋나면 그 자체가 지적(`문서 제안` 또는 `설계 판정`)이다.
3. 지적을 `OUT/findings.json` 으로 쓴다 — 스키마 `harness/schemas/finding.json`. **모든 지적에 `why`(왜 문제가 되는지: 이대로 두면 누가/무엇이 어떻게 되나 + 근거)** 를 쓴다. 게시 코멘트에도 그 문단을 `왜 문제가 되는지:` 로 붙인다. **`category`·`importance`(첫 줄 `[층] [카테고리] 🔴/🟡/🟢`, `review-response` 표 참조) 와 `proposal`(제안+예시) 도 필수** — 주석 관련이면 확정 문구를 GitHub ```suggestion 블록으로(한 줄 앵커: 주석 + 원래 줄) — **울타리는 줄 맨 앞에서 열고 닫는다**(문장 뒤 같은 줄에 붙이면 블록이 안 열려 제안이 깨진다; `review_reply.py` 가 교정·거부한다), 코드는 스케치, 문서는 문구, 테스트는 TC 시나리오. **에러 우려**(데드락·크래시·누수·오답·UB) 는 게시 전에 확인한다 — 관련 함수를 직접 읽어 순서·초기화를 확정(static)하거나 빌드해 재현(dynamic)하고 `verification` 에 방법·결과를 적는다; 확인 못 하면 `question` 으로 게시한다(사용자 지시 2026-09-17). **재현·예시 SQL 은 최소 테이블로** — 테이블을 하나 빼도 현상이 남으면 빼고, 별칭은 역할(`o`/`i`)로, 부질의 전용 표는 조인 대상이 아니라고 적는다(사용자 지시 2026-10-08, `sql-difftest` §2-4) — 지적만 있고 결과가 없는 코멘트는 작성자가 우선순위를 판단할 수 없다(사용자 지시 2026-09-17). `layer` 코드/설계, `evidence` 는 팩 안의 `file:line` 또는 `pr-body:N`, 성능 지적은 `rule_ids`, 설계 지적은 `arch_edge`. `findings.auto.json`(MEAS) 은 건드리지 않는다(러너가 합친다).
4. `python3 -m tools.harness.run full --pr <PR> --findings OUT/findings.json` → `findings.adjudicated.json`, `requery.json`, `report.md`.
5. `requery.json` 이 비어 있지 않으면 **한 번만** 그 항목을 다시 검토해 findings.json 을 고치고 4 를 재실행한다(그래도 inconclusive 면 보고서 "판정 보류"에 남긴다).
6. `report.md` 를 사람 말투로 다듬되, **맨 앞에 `## 0. 먼저 알아야 할 것` 을 직접 쓴다** — 하네스 골격에는 없다(`code-review` §5 의 표 9항목: 무엇을 바꾸나 · 사용자가 보게 되는 변화 · TC·CI · **요청자 PR 과의 충돌** · 리뷰 신뢰도 · revert 비용 · 리뷰 분담 · 게시 비용 · 결정 Q). 그다음에 `examples/리뷰보고서-예시-PR<PR>.md` 와 `claude-workspace/projects/<JIRA>/리뷰보고서-PR<PR>.md` 에 둔다. **게시는 하지 않는다** — 사용자에게 "N번 코멘트 — [층] 본문" 초안을 보이고 승인 후 `review-response` 규약(인라인)으로 게시.
   **전달물은 보고서 파일이 아니라 7 의 리뷰 전용 PR 이다**(사용자 지시 2026-10-08): 보고서를 다듬은 뒤 반드시 7 로 가서 fork 에 draft PR 을 만들고 보고서 전문을 PR 본문에 넣어 그 링크를 건넨다. 본문 교체는 `gh pr edit --body-file` 이 Projects(classic) GraphQL 오류로 조용히 실패하므로 `gh api -X PATCH repos/soheejung-cs/cubrid/pulls/<n> --input body.json` 으로 하고 `gh pr view --json body` 로 줄 수를 확인한다. `function_notes` 항목에는 그 함수에 달린 지적의 `finding_ids` 를 넣는다(주석에 `[지적 A1]` 로 찍힌다).
7. **변경 지도·주석 PR**: 리뷰 단계가 `function_notes` 를 냈으면 `OUT/function_notes.json` 으로 모아 두면 `report` 가 §변경 지도를 자동으로 깐다. 읽기용 PR 이 필요하면
   `python3 -m tools.harness.annotate --pr <N> --head <sha> --notes OUT/function_notes.json --push --pr-create` (fork 안 draft, upstream 아님).
8. 기록: `record` 스킬(episodic 은 러너가 이미 적재).

## 서브에이전트 구성 (2026-10-08, 사용자 지시 "서브에이전트를 여러 개 두고 감시 역할")
정의는 `agents/*.md`(링크 `tools/agent_link.sh` → `~/.claude/agents/`), 표와 흐름은 [`agents/README.md`](../../agents/README.md). 등급·강도는 [`harness/모델-선택.md`](../../harness/모델-선택.md).
절차 2~3 을 아래로 바꾼다 (메인 루프 = 나는 **승인·게시·사용자 대화만**):
1. `batch_index.md` 의 그룹 제안대로 **4그룹씩** `Agent(subagent_type="harness-pack-reader")` → `observations.<g>.json`; 이어 `harness-reviewer` → `findings.<g>.json`. 프롬프트에는 `OUT`·`preamble.md` 경로·배치 파일 목록·그룹 이름만(지시문을 복사하지 않는다).
2. `harness-refuter` 에 findings 전부 → `refute.<g>.json`.
3. `harness-synthesizer` → `OUT/findings.json`·`comments.draft.md`·`report.head.md`.
4. 절차 4~5(`--findings` 판정·requery) 는 그대로.
5. **`harness-gatekeeper`** 에 `comments.draft.md` 와 재현 스크립트 경로 → `gate.*.md`. 차단급이 있으면 고쳐서 다시 돌린다 — 게시하지 않는다.
6. 사용자 승인 → 게시(review-response 규약) → **`harness-auditor`** 로 기록·배운것·보드·환경 복원 점검 → 미완이면 처리.
에이전트는 이 대화를 보지 못한다: 입력은 파일 경로, 출력은 파일. `needs_judgment` 가 올라오면 그 항목만 내가 판정한다.

배치가 수십 개인 대형 PR 은 **동시에 다 띄우지 않는다** — `batch_index.json` 의 `chunk_hint`(기본 4)만큼
끊어 돌리고, 그룹은 `batch_index.md` 의 제안을 쓴다. 에이전트에는 `preamble.md` 를 한 번만 읽히고
배치 파일은 팩만 읽힌다. 근거와 한도 실측은 모델-선택 §한도에 걸리지 않게.

## 하지 않는 것
- 팩 밖 저장소 탐색, 줄 번호 추측, 판정 없이 게시, CTP/벤치 실행(`review-testing` 으로 제안만).
