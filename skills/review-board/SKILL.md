---
name: review-board
description: 리뷰 보드 갱신 — review-to-do 웹 보드(http://192.168.6.51:8827/)를 다시 생성·호스팅한다. 추적 GitHub ID 가 assignee 인 open·non-draft PR 의 미해결 스레드 수, 리뷰어 응답 상태, CI, 이 컨테이너의 리뷰 문서 링크를 보여준다. "리뷰 보드 갱신", "리뷰 현황 보여줘", 추적 ID 추가 요청 때.
---

# 리뷰 보드 갱신 (review-board)

## 무엇을 보여주나
`tools/roster.json` 의 **`tracked_github`** 에 든 로그인이 **assignee** 인 PR 중 **open 이고 draft 가 아닌 것**만
(사용자 지시 2026-09-16: 머지된 것·draft 는 추적 안 함, 기준은 JIRA 가 아니라 GitHub). PR 마다:
- 미해결 리뷰 스레드 수 / 전체 스레드 수, 미해결 스레드를 남긴 사람별 개수
- 리뷰어: 최신 리뷰 상태(✓ APPROVED · ✗ CHANGES_REQUESTED · … COMMENTED), **응답 대기(요청됐지만 아직 리뷰 안 함)**, reviewDecision
- CI 요약(`Check TC PRs` 는 규약상 무시), 갱신·생성일
- **이 컨테이너에 있는 리뷰 문서**: `claude-workspace/projects/<JIRA키>/`, `dev4-review-workspace/examples|reviews` 에서 JIRA 키·PR 번호로 찾은 것(file:// 링크)
- 연결된 TC PR(`cubrid#<n> in:title`, draft 표시)

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
- 미해결 > 0 이고 마지막 리뷰가 CHANGES_REQUESTED → **작성자가 움직일 차례**.
- 미해결 = 0 이고 "대기" 필이 있으면 → **리뷰어가 움직일 차례**(응답 재촉 대상).
- CI FAIL 은 `build/test_*` 만 실제 문제, `Check TC PRs` 는 무시(PR브랜치-규칙 §5).
- 리뷰 문서가 "없음"인 PR 을 리뷰하게 되면 먼저 `record` 스킬대로 `projects/<JIRA키>/` 를 만든다.

## 한계
- assignee 가 비어 있는 PR 은 잡히지 않는다(작성자만으로는 추적하지 않음 — 사용자 지시).
- 리뷰 문서 링크는 `file://` 이라 이 컨테이너(.51)에서 열 때만 유효하다.
