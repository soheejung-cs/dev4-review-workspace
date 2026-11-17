"""board_presence — agent-todo 보드(퍼스널 claude-workspace)의 세션 표시.

리뷰를 시작하면 '리뷰중 PR#n'(run.py), 게시해 끝내면 푼다(post_review.py --post) — 사용자 요청 2026-11-17
("카드 없음이 아니라 리뷰중으로", "이미 리뷰 끝난 건 추적하지마"). 퍼스널 스크립트가 있는 곳에서만, Claude Code 세션
안에서만, 뒤에서 조용히 돈다 — 실패해도 리뷰를 막지 않는다.
"""
import os, subprocess

PRESENCE = os.path.expanduser('~/dev/docs/claude-workspace/skills/agent-board/presence.py')


def _run(cmd, pr):
    if os.path.exists(PRESENCE) and os.environ.get('CLAUDE_CODE_SESSION_ID'):
        subprocess.Popen(['python3', PRESENCE, cmd, f'PR#{pr}', '--quiet'], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)


def review_started(pr):
    _run('focus', pr)


def review_posted(pr):
    """리뷰를 게시해 끝냈다 — 그 PR 은 추적하지 않으므로 표시도 푼다(지금 초점이 그 PR 일 때만)."""
    _run('release', pr)
