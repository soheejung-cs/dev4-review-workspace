---
name: review-board
description: 리뷰 보드 갱신 — review-to-do 웹 보드(http://192.168.6.51:8827/)를 다시 생성·호스팅한다. 추적 GitHub ID 가 assignee(없으면 작성자)인 open·non-draft PR 의 미해결 스레드 수, 리뷰어 응답 상태, 연동 TC PR, 이 컨테이너의 리뷰 문서 링크를 보여준다. "리뷰 보드 갱신", "리뷰 현황 보여줘", 추적 ID 추가 요청 때.
---

# 리뷰 보드 갱신 (review-board)

## 무엇을 보여주나
`tools/roster.json` 의 **`tracked_github`** 에 든 로그인이 **assignee** 인(없으면 **작성자**인) PR 중 **open 이고 draft 가 아닌 것**만
(사용자 지시 2026-09-16·10-14). PR 마다 미해결/전체 스레드, 리뷰어 상태(✓·✗·… 와 `미착수`·`승인 전` 필), 연동된 TC PR, 갱신일.
머리에는 '미완료 PR' 수, 사람별 표에는 내 PR / 내가 리뷰어 / 머지 대기. 툴바에서 사람을 고르면 그 사람이 담당자·리뷰어인 행만 남고
**붉은 행 = 고른 사람이 움직일 차례**, 회색 = 승인 끝·머지 대기. `내 전용 열 보기` 로 에이전트 리뷰·리뷰 문서 열을 켠다.
**UI 명세(필의 뜻·툴바·붉은 행 판정표·내 전용 열)의 정본은 생성기 `tools/review_board_gen.py` 머리말**이다 — 보드를 고치거나 화면이 이 설명과 다르면 거기를 본다(H-40).

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
- CI FAIL 은 `build/test_*` 만 실제 문제(PR브랜치-규칙 §5).
- "에이전트 리뷰: 아직" 이면서 리뷰어 대기에 에이전트 계정이 있으면 → **이 세션이 움직일 차례**(`code-review` → `review-response`).
- 리뷰 문서가 "없음"인 PR 을 리뷰하게 되면 먼저 `record` 스킬대로 `projects/<JIRA키>/` 를 만든다.

## 한계
- assignee 에 추적 ID 가 없으면 **작성자**로 잡는다(사용자 지시 2026-10-14 — 그 전엔 작성자만으로는 추적하지 않아 assignee 를 비운 PR 이 빠졌다). 그런 행은 제목 아래에 `작성자 기준(assignee 없음)` 으로 표시되고 `board.json` 의 `tracked_via` 가 `author` 다.
- 리뷰 문서는 `http://192.168.6.51:8827/docs/<리포>/…` 로 열린다(`site/docs → ~/dev/docs` 심링크, `.md`/`.sh` 는 text/plain 으로 바로 표시). 내부망 어디서든 접근 가능.

## 싣지 않는 것 (이력은 git log)
- 추천 리뷰어 열 — 사용자 지시 2026-10-06 으로 제거. 추천이 필요하면 `review-recommend` 스킬을 PR 단위로 쓴다(보드에는 싣지 않는다).
- Teams 알림 — 사용자 지시 2026-10-07 로 제거(알림은 다른 담당자의 agents 채널이 맡고, 보드는 10분 갱신만). 되살릴 때는 커밋 `1eef5a3` 의 `tools/review_board_notify.py`.
