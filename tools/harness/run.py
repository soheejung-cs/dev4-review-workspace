"""CLI: python3 -m tools.harness.run {review|design|adjudicate} --pr N [--findings f.json] [--repo] [--out] [--model]
실행 그래프는 harness/pipelines/<mode>.yaml 이 소유한다(pipeline.py). 산출물 out/<pr>/<sha>/."""
import argparse, os, subprocess
from . import pipeline

# agent-todo 보드(퍼스널 claude-workspace)의 세션 표시 — 있는 곳에서만 쓴다
PRESENCE = os.path.expanduser('~/dev/docs/claude-workspace/skills/agent-board/presence.py')

def _presence_review(pr):
    """보드의 세션 줄을 '카드 없음' 대신 리뷰중 PR#n 으로(사용자 요청 2026-11-17) — 뒤에서 조용히, 실패해도 리뷰를 막지 않는다."""
    if os.path.exists(PRESENCE) and os.environ.get('CLAUDE_CODE_SESSION_ID'):
        subprocess.Popen(['python3', PRESENCE, 'focus', f'PR#{pr}', '--quiet'], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, start_new_session=True)

def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument('mode', choices=['full', 'review', 'design', 'adjudicate'], help='full = 리뷰+설계+성능 통합(권장); 나머지는 부분집합')
    ap.add_argument('--pr', type=int, required=True); ap.add_argument('--repo', default=os.path.expanduser('~/dev/sources/cubrid'))
    ap.add_argument('--out', default=os.path.expanduser('~/dev/utils/harness-out')); ap.add_argument('--findings'); ap.add_argument('--model')
    ap.add_argument('--sha', help='PR head 대신 이 로컬 리비전을 리뷰(미push 자기 리뷰). --base 와 함께')
    ap.add_argument('--base', help='--sha 의 diff 기준 (기본 upstream/develop; 머지 커밋을 주면 그 이후 커밋만)')
    a = ap.parse_args(argv)
    if a.mode == 'adjudicate' and not a.findings: ap.error('adjudicate needs --findings')
    if a.mode != 'adjudicate' and not a.sha: _presence_review(a.pr)   # --sha 는 미push 자기 리뷰 — 그 카드가 초점이다
    yml = os.path.join(pipeline.ROOT, 'harness', 'pipelines', f'{a.mode}.yaml')
    pipeline.run(yml, a.pr, a.out, a.repo, a.findings, a.model, a.sha, a.base)

if __name__ == '__main__': main()
