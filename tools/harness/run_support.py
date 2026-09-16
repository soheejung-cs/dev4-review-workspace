"""Worktree(RAII) 와 sh 헬퍼 — pipeline.py 가 쓴다."""
import os, subprocess

def sh(*a, **k): return subprocess.run(list(a), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, **k)

class Worktree:
    """PR 헤드의 읽기 전용 스냅샷. with 블록을 벗어나면 반드시 제거된다(예외 포함)."""
    def __init__(self, repo, sha, base):
        self.repo, self.sha, self.path = repo, sha, os.path.join(base, f'wt-{sha[:9]}')
    def __enter__(self):
        if not os.path.isdir(self.path):
            r = sh('git', '-C', self.repo, 'worktree', 'add', '--detach', self.path, self.sha)
            if r.returncode: sh('git', '-C', self.repo, 'fetch', '-q', 'origin', self.sha); r = sh('git', '-C', self.repo, 'worktree', 'add', '--detach', self.path, self.sha)
            if r.returncode: raise RuntimeError(r.stderr)
        return self.path
    def __exit__(self, *exc):
        sh('git', '-C', self.repo, 'worktree', 'remove', '--force', self.path); return False

