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
2. `OUT/review_request*.md` 를 **배치 순서대로 전부** 읽는다(다른 파일은 열지 않는다 — 팩에 없는 코드는 "없음"). `manifest.json` 의 `codegraph_complete` 가 false 면 보고서에 적는다.
3. 지적을 `OUT/findings.json` 으로 쓴다 — 스키마 `harness/schemas/finding.json`. `layer` 코드/설계, `evidence` 는 팩 안의 `file:line` 또는 `pr-body:N`, 성능 지적은 `rule_ids`, 설계 지적은 `arch_edge`. `findings.auto.json`(MEAS) 은 건드리지 않는다(러너가 합친다).
4. `python3 -m tools.harness.run full --pr <PR> --findings OUT/findings.json` → `findings.adjudicated.json`, `requery.json`, `report.md`.
5. `requery.json` 이 비어 있지 않으면 **한 번만** 그 항목을 다시 검토해 findings.json 을 고치고 4 를 재실행한다(그래도 inconclusive 면 보고서 "판정 보류"에 남긴다).
6. `report.md` 를 사람 말투로 다듬어 `examples/리뷰보고서-예시-PR<PR>.md` 와 `claude-workspace/projects/<JIRA>/리뷰보고서-PR<PR>.md` 에 둔다. **게시는 하지 않는다** — 사용자에게 "N번 코멘트 — [층] 본문" 초안을 보이고 승인 후 `review-response` 규약(인라인)으로 게시.
7. 기록: `record` 스킬(episodic 은 러너가 이미 적재).

## 하지 않는 것
- 팩 밖 저장소 탐색, 줄 번호 추측, 판정 없이 게시, CTP/벤치 실행(`review-testing` 으로 제안만).
