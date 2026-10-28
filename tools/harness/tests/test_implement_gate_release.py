"""implement_gate (6) 수명 있는 상태의 해제 경로 전수(H-33, META-REVIEW PR#7658) 회귀.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_implement_gate_release"""
import os
import subprocess
import tempfile
import unittest

from tools.harness import codegraph as CG, implement_gate as IG, static_lint as SL

HAVE_TS = os.path.exists(SL.TS_LIB)

H_BASE = """typedef struct x_entry
{
  int n;
  char *name;
} X_ENTRY;
"""
H_HEAD = """typedef struct x_entry
{
  int n;
  char *name;
  double *slots;
} X_ENTRY;
"""
C = """#include "x.h"

void
x_free_entry (X_ENTRY * e)
{
  free (e->name);
  free (e);
}

void
x_retire_entry (X_ENTRY * e)
{
  free (e->slots);
}

int
x_count (X_ENTRY * e)
{
  return e->n;
}
"""


def sh(cwd, *a):
    return subprocess.run(list(a), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, check=True).stdout


@unittest.skipUnless(HAVE_TS, 'tree-sitter 문법 라이브러리 없음')
class ReleaseCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        r = cls.repo = os.path.join(cls.tmp.name, 'repo')
        os.makedirs(os.path.join(r, 'src', 'x'))
        sh(r, 'git', 'init', '-q')
        sh(r, 'git', 'config', 'user.email', 't@example.invalid')
        sh(r, 'git', 'config', 'user.name', 't')
        for rel, body in (('src/x/x.h', H_BASE), ('src/x/a.c', C)):
            with open(os.path.join(r, rel), 'w') as f:
                f.write(body)
        sh(r, 'git', 'add', '-A')
        sh(r, 'git', 'commit', '-q', '-m', 'base')
        with open(os.path.join(r, 'src/x/x.h'), 'w') as f:
            f.write(H_HEAD)                                   # 미커밋 변경 = 게이트가 보는 작업 트리
        diff = sh(r, 'git', 'diff', '-U0', 'HEAD', '--', 'src/x/x.h')
        cls.added = SL._added(diff)
        cls.g = CG.CodeGraph.build(r, ['src/x/x.h', 'src/x/a.c'], os.path.join(cls.tmp.name, 'after.sqlite3'), log=lambda m: None)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_new_pointer_field_without_sites_fails(self):
        fails, notes, cands = IG.release_check({}, self.repo, self.g, self.added)
        self.assertTrue(any('release_sites 가 비었다' in m and 'x_entry.slots' in m for m in fails), fails)
        names = sorted(c.split(':')[-1] for c in cands)
        self.assertEqual(names, ['x_free_entry', 'x_retire_entry'])      # x_count 는 놓는 이름이 아니다
        self.assertTrue(any('x_free_entry' in m and '새 필드는 안 다룬다' in m for m in fails), fails)

    def test_all_sites_judged_passes(self):
        plan = {'release_sites': [{'site': 'src/x/a.c:x_free_entry', 'verdict': 'released', 'why': '추가: free (e->slots)'},
                                  {'site': 'src/x/a.c:x_retire_entry', 'verdict': 'released', 'why': 'a.c:13'}]}
        fails, notes, cands = IG.release_check(plan, self.repo, self.g, self.added)
        self.assertEqual(fails, [])
        self.assertTrue(any('후보 2개' in n for n in notes), notes)

    def test_missing_one_site_fails(self):
        plan = {'release_sites': [{'site': 'src/x/a.c:x_retire_entry', 'verdict': 'released', 'why': 'a.c:13'}]}
        fails, _, _ = IG.release_check(plan, self.repo, self.g, self.added)
        self.assertEqual(len(fails), 1)
        self.assertIn('x_free_entry', fails[0])

    def test_no_lifetime_state_no_check(self):
        fails, notes, cands = IG.release_check({}, self.repo, self.g, {})
        self.assertEqual((fails, notes, cands), ([], [], []))

    def test_declared_state(self):
        fails, _, cands = IG.release_check({'lifetime_state': ['x_entry.name']}, self.repo, self.g, {})
        self.assertTrue(cands)
        self.assertTrue(any('release_sites 가 비었다' in m for m in fails))


if __name__ == '__main__':
    unittest.main()
