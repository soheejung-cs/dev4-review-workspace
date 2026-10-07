---
name: review-board
description: 리뷰 보드 갱신 — review-to-do 웹 보드(http://192.168.6.51:8827/)를 다시 생성·호스팅한다. 추적 GitHub ID 가 assignee 인 open·non-draft PR 의 미해결 스레드 수, 리뷰어 응답 상태, CI, 이 컨테이너의 리뷰 문서 링크를 보여준다. "리뷰 보드 갱신", "리뷰 현황 보여줘", 추적 ID 추가 요청 때.
---

# 리뷰 보드 갱신 (review-board)

## 무엇을 보여주나
`tools/roster.json` 의 **`tracked_github`** 에 든 로그인이 **assignee** 인 PR 중 **open 이고 draft 가 아닌 것**만
(사용자 지시 2026-09-16: 머지된 것·draft 는 추적 안 함, 기준은 JIRA 가 아니라 GitHub). PR 마다:
- 미해결 리뷰 스레드 수 / 전체 스레드 수, 미해결 스레드를 남긴 사람별 개수
- 리뷰어: 최신 리뷰 상태(✓ APPROVED · ✗ CHANGES_REQUESTED · … COMMENTED), reviewDecision, 그리고 **두 갈래의 남은 일**:
  **미착수**(요청됐고 리뷰 이력 0) / **승인 전**(리뷰했으나 APPROVED 아님). GitHub 은 리뷰를 제출하는 순간 그 사람을
  `reviewRequests` 에서 빼므로, 요청 목록만 보면 코멘트 한 번 남기고 승인하지 않은 리뷰어가 보드에서 사라진다
  (2026-09-21 지적 반영). 재요청되면 다시 `미착수` 로 잡힌다.
- CI 요약(`Check TC PRs` 는 규약상 무시), 갱신·생성일
- **이 컨테이너에 있는 리뷰 문서**: `claude-workspace/projects/<JIRA키>/`, `dev4-review-workspace/examples|reviews` 에서 JIRA 키·PR 번호로 찾은 것(file:// 링크)
- **툴바(2026-10-06 사용자 지시)**: 머리의 **「사람으로 모아 보기 — 담당자 + 리뷰어」** 버튼 줄에서 사람을 누르면 그 사람이 **리뷰어(미착수·승인 전·승인자)이거나 담당자(assignee)** 인 행을 모아 보여 주고 빈 섹션은 접는다. 담당자 포함은 이현욱(hyunuk.lee) 요청 2026-10-07 — 리뷰어만 걸면 **자기가 담당인 PR 이 필터에서 사라져** 쓸모가 반감된다. `승인 전·미착수만` 은 아직 응답이 남은 PR 만 추린다. 전부 클라이언트 JS 라 재생성 없이 즉시 바뀐다(행의 `data-rev`/`data-pend`/`data-own` 속성 기준).
- **붉은 행 = 내가 움직일 차례**(사용자 지시 2026-10-07). 왼쪽 테두리 3px + 옅은 배경 + PR 번호를 붉게(`tr.unres`). **기준은 툴바에서 고른 사람에 따라 바뀐다**:
  | 고른 사람과의 관계 | 붉은가 |
  |---|---|
  | **내가 담당자**(assignee) | **머지 전이면 붉다** — 이 보드는 open PR 만 싣으므로 사실상 늘 붉다 |
  | **내가 리뷰어** | **내가 아직 승인하지 않았으면** 붉다(남이 승인했어도 내가 안 했으면 붉다) |
  | 아무도 안 고름(전체) | 중립 기준 — resolve 안 한 코멘트가 남았으면 붉다 |
  | **승인 끝·미해결 0 인데 머지 안 됨** | **회색**(`tr.ready`) — 사라지지 않고 남아 "머지만 남았다"를 보인다(사용자 지시 2026-10-07). 사람별 표에도 `머지 대기` 열로 센다 |
  툴바의 `붉은색 = …` 문구가 현재 기준을 말해 준다. 판정은 행의 `data-own`/`data-rev`/`data-appr`/`data-unres` 로 클라이언트에서 다시 칠한다.
- **내 전용 열 — `에이전트 리뷰` · `리뷰 문서`(기본 **숨김**)**. 둘 다 나만 보는 것이라 툴바 오른쪽 **`내 전용 열 보기`** 하나로 같이 켠다(`.col-mine`, 사용자 지시 2026-10-06 "헷갈린다" + 2026-10-07 "리뷰 문서는 나만 보게"). 켜면 열이 나타나고, 선택은 `localStorage` 에 남는다. 내용은 `roster.json` 의 `agent_github` 로그인(또는 본문에 `agent_marker` 정규식)이 남긴 리뷰·인라인 코멘트·이슈 코멘트 건수와 마지막 날짜. **팀 공유 계정으로 돌리면 `agent_github` 만 그 계정으로** 바꾼다. 지금은 `soheejung-cs` 라 사용자 본인의 코멘트도 함께 세어진다(공유 계정 전환 전 한계).
- 연동된 TC PR: JIRA 키(또는 제목의 `cubrid#<n>`)가 같은 testcases / private-ex PR 을 엔진 PR 하위에 붙인다 — 미해결 수·대기 리뷰어 포함

## 갱신
```bash
~/bin/review_board_publish.sh                 # 1회 생성 (+ 8827 서버 없으면 기동)
nohup ~/bin/review_board_publish.sh --daemon 10 > ~/dev/utils/review-board/site/.daemon.log 2>&1 &   # 10분마다 (재부팅 후)
python3 ~/dev/docs/dev4-review-workspace/tools/review_board_gen.py [out_dir]   # 생성만
```
산출물 `~/dev/utils/review-board/site/{index.html,board.json}`. 서버는 `~/bin/httpd_threaded.py <dir> 8827`
(python `http.server` 는 단일 스레드·charset 없음 — agent-todo 보드와 같은 이유로 쓰지 않는다).
데몬이 죽었는지는 `.daemon.log` 의 마지막 시각과 페이지 머리의 "스냅샷" 시각으로 본다.

## 추적 ID 추가·제거
`tools/roster.json` → `tracked_github` 배열만 고친다(이름·JIRA 는 본인 등록 시). 커밋은 **본인 이름으로**. 다음 갱신에 반영.

## 판정 규칙 (보드를 읽는 법)
- **미해결 > 0** → **작성자가 움직일 차례**(마지막 리뷰가 CHANGES_REQUESTED 면 더 분명).
- **미해결 = 0 이고 승인자 0** → **리뷰어가 움직일 차례**. 요약의 **"승인만 남은 PR"** 이 이 칸이다 —
  `미착수` 도 없고 미해결도 없어 조용해 보이지만 아무도 승인하지 않아 멈춘 PR 이다. 리뷰어 재요청 대상.
- **미해결 = 0 이고 승인자 ≥ 1** → 머지 가능.
- `미착수` 필이 있으면 그 리뷰어는 아직 보지도 않았다. `승인 전` 필은 봤지만 승인하지 않은 사람이다 —
  미해결이 남아 있으면 그 사람이 아니라 **작성자**가 움직일 차례다.
- CI FAIL 은 `build/test_*` 만 실제 문제, `Check TC PRs` 는 무시(PR브랜치-규칙 §5).
- "에이전트 리뷰: 아직" 이면서 리뷰어 대기에 에이전트 계정이 있으면 → **이 세션이 움직일 차례**(`code-review` → `review-response`).
- 리뷰 문서가 "없음"인 PR 을 리뷰하게 되면 먼저 `record` 스킬대로 `projects/<JIRA키>/` 를 만든다.

## 한계
- assignee 가 비어 있는 PR 은 잡히지 않는다(작성자만으로는 추적하지 않음 — 사용자 지시).
- 리뷰 문서는 `http://192.168.6.51:8827/docs/<리포>/…` 로 열린다(`site/docs → ~/dev/docs` 심링크, `.md`/`.sh` 는 text/plain 으로 바로 표시). 내부망 어디서든 접근 가능.

## 제거된 것
- **추천 리뷰어 열**(2026-09-30 도입 → **2026-10-06 사용자 지시로 제거**). 난이도·추천 필·`과부하로 뒤로`·업무부하의 `추천받음` 열이 함께 빠졌고, `attach_recommendations()` 와 `review_recommend` 호출도 지웠다 — 생성 시간이 1.5분 줄었다. 추천이 다시 필요하면 `review-recommend` 스킬을 PR 단위로 쓴다(보드에는 싣지 않는다).

- **머리 통계·사람별 표는 '미완료 PR' 하나다**(사용자 지시 2026-10-06). 미완료 = 머지 가능이 아닌 것 = **미해결 스레드가 남았거나 승인자가 0**. 머리에는 전체 건수(`18 / 전체 21`), 표에는 사람별로 **내 PR**(assignee, 작성자가 움직일 차례)과 **내가 리뷰어**(미착수·승인 전)를 나눠 센다. 구 '업무 부하표'(본인 PR+리뷰 중 합계)는 대체됐다.

## Teams 알림 (2026-10-07 사용자 지시)
보드에서 달라진 것을 10분마다 감지해 팀 채널 웹훅으로 알린다. **토큰을 쓰지 않는 파이썬 데몬**이고, 보드 생성 데몬과 별개 프로세스다.
```bash
nohup ~/bin/review_board_notify.sh --daemon 10 > /dev/null 2>&1 &   # 재부팅 후 재기동
~/bin/review_board_notify.sh --dry                                    # 보내지 않고 카드만 출력(상태 미갱신)
tail ~/dev/utils/review-board/state/notify.log
```
- **감지**: 새 인라인 코멘트(→ PR 담당자) · **답글**(→ 담당자 + 그 스레드에 앞서 글 쓴 사람) · 리뷰 승인/변경요청/의견(→ 담당자) · PR 대화 코멘트 · 새 리뷰 요청(→ 요청받은 사람) · 내가 남긴 스레드의 해결. 수신 대상은 `tracked_github` 만, 봇 계정·본인 글은 제외.
- **처음 한 번은 기준선만** 잡고 알리지 않는다. 새로 잡힌 PR 은 리뷰 요청만 알린다. 한 주기의 변화는 **카드 한 장**(사람별 최대 8줄)으로 묶는다.
- **웹훅 URL**: `~/.config/review-board/teams_webhook`(chmod 600) 또는 `REVIEW_BOARD_TEAMS_WEBHOOK`. **파일이 없으면 보내지 않고** `notify.log` 에 "NO WEBHOOK — would send …" 만 남긴다(드라이런, 상태는 갱신). 전송 실패 시 상태를 갱신하지 않아 다음 주기에 재시도한다.
- **멘션**: `roster.json` 의 `teams: {로그인: {name, email}}` 를 채우면 `<at>` 멘션, 비우면 로그인 이름만 굵게.
- 상태 파일 `~/dev/utils/review-board/state/notify_state.json` 은 site/ 밖이다(웹 노출 없음). 규약 예외는 메모리 `rules/메일-발신-금지.md` §예외.
