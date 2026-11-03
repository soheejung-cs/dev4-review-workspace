# TC PR 본문·제목 쓰는 법 (`tc/pr-<N>` — cubrid-testcases · cubrid-testcases-private-ex)

사용자 지시 2026-11-03: "수정 파일이 있으면 무조건 해당 tc 의 본문을 업데이트" · "쓰는 법도 하네스에 기록".
짝 문서: [머지메시지.md](머지메시지.md)(세 리포의 스쿼시 머지 메시지). 규칙 정본은 claude-workspace 메모리 `PR브랜치-규칙` §3, QA 정본은 private-ex `shell/AGENTS.md` 「The PR」 이다(둘이 다르면 룰북을 따른다).
머지 전 기계 검사는 claude-workspace `tools/merge_gate.py` 4) — 바뀐 파일이 있는 TC PR 의 봇 기본 본문·봇 제목·첫 줄 JIRA 링크 없음을 FAIL 로 낸다.

## 언제 쓰나
- `tc/pr-<N>` 에 TC 를 **처음 push 할 때**: 봇(cubrid-tc-sync-bot)이 연 Draft TC PR 의 본문·제목을 바로 바꾼다.
- **케이스·답안을 더하거나 고칠 때마다**: 본문의 케이스 목록과 판정을 파일과 같게 맞춘다.
- 바꿀 파일이 0 인 리포의 Draft TC PR 은 본문을 쓰지 않는다. 닫을 대상이라고 사용자에게 알린다(닫기는 사용자 몫, PR브랜치-규칙 §5).

## 제목 (영어)
- `[CBRD-XXXXX] <무엇을 더하거나 바꾸나>` 꼴로 쓴다. 스쿼시 머지는 PR 제목을 develop 커밋 제목으로 쓴다.
- 봇 기본 제목 `[CBRD-XXXXX] Draft: TC changes for PR CUBRID/cubrid#N` 을 남기지 않는다(룰북 「The PR」).
- 예:
  - `[CBRD-27852] Add a shell TC: a repeated EXECUTE handle keeps no compilation per execution`
  - `[CBRD-27231] Add a shell TC: an executing kept prepared tree survives nested DEALLOCATE / re-PREPARE of its name`
  - 답안만 바꾸면 `[CBRD-XXXXX] Update the answer of cbrd_yyyyy for <원인>`

## 본문 (한국어, 엔진 PR 과 같은 세 절)
**첫 줄은 JIRA 링크다**(룰북 "put the Jira link on the first line of the body"). 같은 줄에 엔진 PR 을 적는다. GitHub 는 줄 머리의 bare URL 도 링크로 그린다(동료 TC PR 들도 첫 줄에 JIRA URL 을 둔다).

```
http://jira.cubrid.org/browse/CBRD-XXXXX · 엔진 PR CUBRID/cubrid#N

### Purpose
이 TC 가 무엇을 확인하나 — 한두 문장, 확인하는 동작이 둘 이상이면 목록으로.

### Implementation
`shell/_06_issues/_<yy>_<1|2>h/cbrd_XXXXX`(또는 sql 경로) — 실행 방식(csql 분할 입력·C API·서버 모드 등).
표: 행 수·인덱스·통계(히스토그램 여부) — 기대값이 기대는 것만.
케이스:
1. <조건>: <기대>
2. …

### Remarks
- 판정: 수정 전 빌드 NOK n(어느 케이스가 어떻게 — rc·증가량·틀린 값) · OK m / 수정 후 OK 전부.
- 다른 PR·이슈 의존(예: CBRD-27143 #8006 이 머지되면 Case 1 은 수정을 검사하지 못한다).
- 환경 제약(Linux 전용 등), 리뷰에서 더한 케이스와 그 이유.
```

- **답안만 바꾼 PR**은 Implementation 에 답안마다 다음 다섯 가지를 쓴다(review-response §5 와 같다).
  1. 케이스의 의도
  2. 바뀐 줄
  3. 엔진 원인(커밋)
  4. 의도를 해치는지
  5. 검증(재생성 빌드·실행 횟수)
- **쓰지 않는 것**: 로컬 경로·로컬 도구(`goto`, `~/bin/*`, `/home/cubrid/…`), 다른 DB 이름, CTP 수치.

## 갱신하는 법
- `gh pr edit` 를 쓰지 않는다. 이 서버에서 Projects-classic 오류로 조용히 실패한다(H-25).
- 본문과 제목을 PATCH 한 번으로 바꾼다.
  ```
  python3 -c "import json; print(json.dumps({'title': '[CBRD-XXXXX] Add a shell TC: …', 'body': open('body.md').read()}))" > patch.json
  gh api -X PATCH repos/CUBRID/<repo>/pulls/<n> --input patch.json
  ```
- 확인할 것:
  - `gh pr view <n> -R CUBRID/<repo> --json title,body`.
  - 첫 줄이 링크로 그려지는지 본다. `gh api repos/CUBRID/<repo>/pulls/<n> -H "Accept: application/vnd.github.html+json" -q .body_html | head -1` 에 `<a href="http://jira.cubrid.org/browse/…` 가 있어야 한다.
- 게시 전 스캔은 `grep -cP '\bgoto\s+\w|~/bin/|/home/cubrid/|file://' body.md` 가 0 이어야 한다.
- assignee 는 `soheejung-cs` 다. 봇이 만든 TC PR 은 비어 있다(PR브랜치-규칙 §3).

## 예
- private-ex #4669 (CBRD-27852): C API 로 같은 EXECUTE 핸들을 4케이스 반복한다. 수정 전 NOK 3 · OK 1, 수정 후 OK 4.
- private-ex #4614 (CBRD-27231): csql 분할 입력 10케이스. 수정 전 8·9 NOK(csql rc 139), 수정 후 10/10.
  - 이 PR 은 케이스 10개를 push 한 채 봇 기본 본문으로 남아 있었다. 그것이 이 문서의 계기다.
