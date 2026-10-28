"""unitseam 의 리비전별 공유 worktree(H-13) — 같은 rev 를 병렬로 불러도 모두 완성된 worktree 를 받는가,
만들다 만 잔재·표지 없는 옛 worktree 를 어떻게 다루는가. 엔진 리포 대신 임시 git 리포로 돈다(수 초).
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_unitseam_worktree"""
import multiprocessing as mp
import os
import shutil
import subprocess
import tempfile
import unittest

from tools.harness import unitseam as US


def git(*a, cwd=None):
    return subprocess.run(['git', *a], cwd=cwd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True).stdout.strip()


def _child(args):
    wt_root, repo, rev = args
    US.WT_ROOT = wt_root
    sha, path = US.worktree(repo, rev)
    # 돌려받은 순간 파일이 다 있어야 한다 — 경합이면 체크아웃 중인 디렉터리를 받아 `no-file` 이 났다
    return os.path.isfile(os.path.join(path, 'src', 'f99.c'))


class WorktreeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='unitseam-wt-')
        self.repo = os.path.join(self.tmp, 'repo')
        os.makedirs(os.path.join(self.repo, 'src'))
        git('init', '-q', self.repo)
        for i in range(100):   # 체크아웃이 순간에 끝나지 않을 만큼
            with open(os.path.join(self.repo, 'src', 'f%02d.c' % i), 'w') as f:
                f.write('int f%d (void) { return %d; }\n' % (i, i) * 200)
        git('add', '-A', cwd=self.repo)
        git('-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', 'init', cwd=self.repo)
        self.rev = git('rev-parse', 'HEAD', cwd=self.repo)
        self.saved = US.WT_ROOT
        US.WT_ROOT = os.path.join(self.tmp, 'out')

    def tearDown(self):
        US.WT_ROOT = self.saved
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_parallel_same_rev(self):
        with mp.Pool(6) as p:
            ok = p.map(_child, [(US.WT_ROOT, self.repo, self.rev)] * 6)
        self.assertEqual(ok, [True] * 6)
        path = os.path.join(US.WT_ROOT, 'wt-unit-' + self.rev[:9])
        self.assertTrue(os.path.isfile(os.path.join(US._admin_dir(path), US.READY)))

    def test_partial_directory_is_rebuilt(self):
        path = os.path.join(US.WT_ROOT, 'wt-unit-' + self.rev[:9])
        os.makedirs(os.path.join(path, 'src'))
        open(os.path.join(path, 'src', 'junk'), 'w').close()      # .git 없는 반쪽 디렉터리
        _, got = US.worktree(self.repo, self.rev)
        self.assertTrue(os.path.isfile(os.path.join(got, 'src', 'f00.c')))
        self.assertFalse(os.path.exists(os.path.join(got, 'src', 'junk')))

    def test_legacy_clean_worktree_is_adopted(self):
        path = os.path.join(US.WT_ROOT, 'wt-unit-' + self.rev[:9])
        os.makedirs(US.WT_ROOT)
        git('-C', self.repo, 'worktree', 'add', '--detach', path, self.rev)   # 표지 없이 만든 옛 worktree
        open(os.path.join(path, 'src', 'f00.unitseam.123.c'), 'w').close()      # 추적 밖 TU 사본은 판정에 끼지 않는다
        US.worktree(self.repo, self.rev)
        self.assertEqual(open(os.path.join(US._admin_dir(path), US.READY)).read().strip(), 'adopted')
        self.assertTrue(os.path.exists(os.path.join(path, 'src', 'f00.unitseam.123.c')))   # 다시 만들지 않았다

    def test_legacy_incomplete_worktree_is_rebuilt(self):
        path = os.path.join(US.WT_ROOT, 'wt-unit-' + self.rev[:9])
        os.makedirs(US.WT_ROOT)
        git('-C', self.repo, 'worktree', 'add', '--detach', path, self.rev)
        os.remove(os.path.join(path, 'src', 'f50.c'))          # 체크아웃 도중 죽은 흉내 — 추적 파일이 빠졌다
        US.worktree(self.repo, self.rev)
        self.assertTrue(os.path.isfile(os.path.join(path, 'src', 'f50.c')))
        self.assertEqual(open(os.path.join(US._admin_dir(path), US.READY)).read().strip(), self.rev)


if __name__ == '__main__':
    unittest.main()
