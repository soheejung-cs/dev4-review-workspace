---
name: harness-implement
description: 구현 하네스 — `/harness-implement <CBRD-n>`. 심볼·키워드로 후보 함수 팩(implement_request.md)을 만들고 → plan.json(영역 판정·대안·바꿀 함수·관문 계약·TC) → **사용자 승인** → 코드 수정 → 게이트(범위·codestyle·-fsyntax-only·관문 짝) → TC/재현 → 자기 리뷰(`/harness-review` 로 내 diff) → PR 초안. "구현 하네스 돌려", "CBRD-n 구현 시작", "이 plan 으로 고쳐".
---

# /harness-implement <CBRD-n>

리뷰 하네스와 같은 규율: **도구가 컨텍스트를 고르고, LLM 은 계약된 산출물만 내고, 도구가 다시 판정한다.** 리뷰가 "지적에 근거가 있나"를 재면 구현은 **"있어야 할 자리인가(설계) · 깨뜨린 게 없나(게이트) · 내가 리뷰어면 뭐라 할까(자기 리뷰)"** 세 게이트다. 승인 지점 둘: **plan** 과 **PR**.

## S0. 준비
- `projects/CBRD-n/설계문서.md` 가 있으면 그것이 입력(없고 불변조건·공유 자원을 건드릴 것 같으면 먼저 `design-doc`). 작은 버그(단일 모듈·불변조건 무관·바꿀 함수 ≤ 3)는 설계문서 없이 plan 으로 바로 간다 — **기본값**(2026-09-17).
- 브랜치: `git fetch upstream develop && git checkout -B CBRD-n upstream/develop`(작업-생애주기 (B) 시작). 워킹트리 dirty 면 멈춘다. 빌드는 사용자가 정한다(빌드환경-규칙 §0).
- 손대는 모듈의 `AGENTS.md` + `dev4-ai-source/modules/<모듈>.md` §2·§3 를 읽는다 — 팩에도 불변조건이 들어가지만 §3(예비 이슈)은 사람이 봐야 한다.

## S1. 팩
```bash
cd ~/dev/docs/dev4-review-workspace
python3 -m tools.harness.implement_pack --key CBRD-n --symbols fn1,fn2 --grep "ER_XXX,메시지 조각" [--files src/a.c]
# → ~/dev/utils/harness-out/impl/CBRD-n/{implement_request[.batchN].md, codegraph.sqlite3, arch.json, manifest.json}
```
후보 함수 = 심볼의 **정의** + 키워드 줄을 포함하는 함수(같은 디렉터리로 그래프 범위). 24개를 넘으면 키워드가 넓은 것 — 좁힌다. `implement_request*.md` 를 배치 순서대로 **전부** 읽는다. 팩 밖 코드는 "없음".

## S2. plan.json (LLM) → 사용자 승인
스키마 `harness/schemas/plan.json`. 필수: 영역 판정 문장 · INV-1~5 각각 touched 여부와 이유 · Tier · 공유/격리(공유면 계약 3요소) · **대안 ≥ 2**(채택 표시) · `changes[]`(팩의 candidate 중에서만; 관문을 건드리면 `gate_contract` 필수; 성능 경로면 `rule_ids`) · `tests[]`(재현→기대) · `regression_risks` · `needs_more_context`(비어 있지 않으면 S1 로 돌아가 팩을 넓힌다) · `questions`(닫힌 질문).
`python3 -c "import json,jsonschema;jsonschema.validate(json.load(open('<out>/plan.json')),json.load(open('harness/schemas/plan.json')))"` 로 스키마 통과 후, **plan 을 표로 요약해 사용자에게 보이고 승인·수정을 받는다.** 설계문서가 있으면 「아이디어」·「대안표」와 어긋나는 점을 명시. 승인 전엔 코드를 고치지 않는다.

## S3. 구현
- plan 의 `changes[]` 함수만 고친다. C 파일은 C 스타일(모던 C++ 관용구 금지), 성능규칙집은 **해당 축만**(핫패스일 때). 새 서버 요청은 3파일 계약(network.h·network_sr.c·network_interface_sr/cl).
- 주석은 "왜"를 적는다(리뷰 하네스가 `why` 를 요구하듯). 저수준(pgbuf/heap/MVCC)은 검증된 코드 재사용(규칙 `저수준-검증된코드-재사용`).
- TC: plan.tests 를 `cubrid-testcases(-private-ex)` 의 해당 카테고리 형식으로. shell TC 는 `write_ok "<케이스> : <무엇>"`.

## S4. 게이트 (결정론)
```bash
python3 -m tools.harness.implement_gate --plan <out>/plan.json [--base HEAD|upstream/develop]
```
① 범위 — 바뀐 파일·함수 ⊆ plan.changes ② `codestyle.sh` 로 포맷 차이 0 ③ `-fsyntax-only`(compile_commands 있을 때) ④ 바뀐 함수의 **관문 카운트 불균형**이 있는데 `gate_contract` 가 없으면 실패. 실패 → 고치고 재실행(**재질의 상한 1회**; 두 번째도 실패면 사용자에게 상태 보고). 그다음 사용자 허락 하에 `goto CBRD-n` 빌드 → 재현 스크립트(`harness/templates/repro.sh`, `ServerSession`) → TC. **CTP 는 요청자 확인 후**(`review-testing`).

## S5. 자기 리뷰 → PR
1. 커밋(브랜치 확인 — detached 면 `git checkout -B CBRD-n HEAD` 먼저) → `git push origin CBRD-n` → **draft PR**(PR브랜치-규칙 §3: 본문 한국어, Purpose/Implementation/Remarks, 리뷰어 지정 안 함, `gh api PATCH` 로 본문).
2. **`/harness-review <내 PR>`** 을 그대로 돈다. 🟡 이상이 나오면 게시하지 말고 고쳐 S4 부터(상한 2회). 자기 리뷰의 finding 은 PR 본문 Remarks 에 "자기 리뷰에서 걸러낸 것" 으로 한 줄 남긴다(무엇을 고쳤나).
3. 사용자 승인 후 draft 해제. TC PR(`tc/pr-<n>`) 은 Merge Gate 순서(TC 먼저).
4. 기록: `record`(세션기록·staging 미수정 표기) + `agent-board`(카드 `[CBRD-n/PR#m]` 제목 갱신·worklog). 머지되면 `post-merge`.

## 산출물 (`~/dev/utils/harness-out/impl/CBRD-n/`)
`implement_request*.md` · `codegraph.sqlite3`(plan 시점) · `arch.json` · `plan.json` · `gate.json` · `codegraph.after.sqlite3` · `manifest.json`

## 여러 에이전트로 쪼갤 때
**수집(팩 만들기·심볼 조회·로그 읽기)은 낮추고, plan 판정·자기 리뷰·게이트 해석은 유지**한다 —
[`harness/모델-선택.md`](../../harness/모델-선택.md).

## 하지 않는 것
- plan 승인 전 코드 수정. plan 밖 함수 수정(필요하면 plan 을 고쳐 다시 승인). 팩 밖 저장소 탐색으로 설계를 바꾸기(팩을 넓혀서 한다).
- 요청 없는 빌드·CTP·벤치. 벤치 DB 에서 재현. `git stash -u`. 엔진 리포에 설계문서 커밋.
- 자기 리뷰 없이 ready PR.
