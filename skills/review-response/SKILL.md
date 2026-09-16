---
name: review-response
description: 내 PR 에 달린 리뷰 코멘트(봇·사람)에 대응하는 규약 — 스레드 수집, 결함이면 재현→수정→재검증, 답글 초안을 사용자 검토 받아 게시, 봇만 resolve. "리뷰에 대응해줘", "이 PR 코멘트 답 달아줘", TC PR 의 답안 변경 사유 코멘트를 쓸 때.
---

# 리뷰 대응 (review-response)

## 0. 누구 PR 인가
- **내(soheejung-cs) PR**: 수정 가능. 답글 → 작성자가 **봇이면 resolve**, **사람이면 resolve 안 함**(리뷰어가 닫는다).
  리뷰어가 "종결하겠습니다" 한 스레드는 그대로 둔다.
- **타인 PR**: 코드 수정 금지. 코멘트는 게시하지 않고 사용자에게 보고 → 승인 후에만.

## 1. 수집
```
gh api repos/CUBRID/cubrid/pulls/<n>/comments --paginate \
  -q '.[] | "=== id=\(.id) \(.user.login) \(.created_at[0:16]) \(.path):\(.line // .original_line) in_reply_to=\(.in_reply_to_id // "-")\n\(.body)\n"'
gh pr view <n> --json reviews,comments,statusCheckRollup     # 리뷰 요약·이슈 코멘트·CI
```
스레드 단위로 묶고(루트 id 기준), 분류: **결함 주장 / 확인 요청 / 제안 / 설계 이견**. 같은 결함을 두 스레드가
지적하면 하나에 상세, 다른 하나에 "~스레드에 적었습니다"로 연결.

## 2. 결함 주장은 재현이 먼저
1. **수정 전 빌드(PR 헤드)로 재현** — 리뷰어가 준 시나리오를 그대로 스크립트로 만든다. 재현되지 않으면 그 사실을 답한다.
2. 원인 확정 → 수정 → **같은 스크립트로 release·debug 재검증**. 기능을 죽이지 않았는지 양성 대조도(예: piggyback 유지).
3. 리뷰어가 짚지 않은 **반대 방향**도 본다(같은 원인이 다른 증상을 내고 있었나) — 답글에 함께 적으면 신뢰가 붙는다.
4. 스크립트는 `claude-workspace/projects/CBRD-XXXXX/repro/` 에 남기고, 재현용이면 JIRA 에도 첨부.

## 3. 답글 초안 → **사용자 검토 → 게시** (이 순서를 어기지 않는다)
사용자에게 이렇게 보여준다:
```
## N번 코멘트 — <작성자> (<파일:줄>, <주제>) · <봇이면 "답글 후 resolve">
> 답글 본문 …
```
**문체 규약** (사용자 지시 2026-09-16):
- **사람이 동료에게 말하듯** 쓴다. 표·헤더로 짜인 보고서 말투가 아니라 문단으로.
- **빌드 번호·시각으로 설명하지 않는다.** 재현 시나리오는 "세션 A 가 …하면, B 는 …" 식으로 **서술**한다.
- 구조: ① 인정/재현 사실 ② 원인(왜 그 코드가 그렇게 동작했나) ③ **반대 방향 손실**이 있었으면 그것도
  ④ **수정을 구체적으로**(어느 함수, 어떤 규칙, 왜 그게 맞나) ⑤ **수정 후 결과**(수정 전/후 대비, release·debug).
- **CTP 수치는 붙이지 않는다**(사용자 지시). 시나리오 실측(ms, 행 수, null_frequency 등)은 괜찮다.
- 제안을 받지 않을 때는 이유를 락 그래프·계약 수준에서 설명하고 대안(예: 릴리스 노트 기재)을 제시한다.
  "다르게 보시면 그대로 반영하겠습니다"로 닫는다.
- 재현·수정에 리뷰어의 확인이 도움됐으면 그 사실을 적는다.

## 4. 게시
```
gh api repos/CUBRID/cubrid/pulls/<pr>/comments/<root_comment_id>/replies -X POST -F body=@reply.md -q .html_url
# 봇 스레드 resolve
TID=$(gh api graphql -f query='{repository(owner:"CUBRID",name:"cubrid"){pullRequest(number:<pr>){reviewThreads(first:50){nodes{id comments(first:1){nodes{databaseId}}}}}}}' -q '.data.repository.pullRequest.reviewThreads.nodes[] | select(.comments.nodes[0].databaseId==<id>) | .id')
gh api graphql -f query="mutation{resolveReviewThread(input:{threadId:\"$TID\"}){thread{isResolved}}}"
```
PR 본문 갱신은 `gh api -X PATCH repos/CUBRID/cubrid/pulls/<n> -F body=@file` (`gh pr edit` 는 이 서버에서 실패).
**CI 트리거(`/run all`)는 사용자·리뷰어가 친다.** TC 답안을 고쳤으면 `/run all` 전에 `tc/pr-<n>` push 가 끝나 있어야 한다.

## 5. TC PR(cubrid-testcases-private-ex `tc/pr-<n>`) 의 답글
- **답안 변경 사유 코멘트**: 답안마다 ① 케이스 의도 ② 무엇이 바뀌었나(줄 단위) ③ 왜(엔진 커밋과 원인) ④ 의도 훼손 여부
  ⑤ 검증(재생성 빌드·실행 횟수). 예: `examples/리뷰답글-예시-PR7900.md` 뒤쪽.
- **리뷰어가 TC 추가를 제안하면** 만든다: 진입 순서를 **동기화**(예: `cubrid lockdb` 에 대기가 보일 때까지 기다린 뒤 진행),
  판정은 문장의 메시지가 아니라 **카탈로그/상태**로, 그리고 **수정 전 빌드에서 NOK 가 나는지** 확인해 TC 가 버그를 실제로
  잡는다는 것을 답글에 적는다. shell TC 는 `write_ok "<케이스> : <무엇>"` 로 서브체크마다 이름을 남긴다.

## 6. 게시 후
보드 카드에 요약 코멘트, `projects/CBRD-XXXXX/세션기록-*.md` 갱신 (`record` 스킬).
