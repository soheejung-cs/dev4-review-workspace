"""implement_gate (0-1) develop_repro(H-169)·(7) NUMERIC 경로 전환(H-180) 회귀 — 작은 가짜 리포(develop 커밋 + 작업 커밋)와 가짜 설치본으로.
실행: cd dev4-review-workspace && DEV4_HARNESS_REEXEC=1 python3 -m unittest tools.harness.tests.test_implement_gate_repro"""
import os
import subprocess
import tempfile
import unittest

from tools.harness import implement_gate as IG


def sh(cwd, *a):
    return subprocess.run(list(a), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, check=True).stdout.strip()


class DevelopReproTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        r = cls.repo = os.path.join(cls.tmp.name, 'repo')
        os.makedirs(r)
        sh(r, 'git', 'init', '-q')
        sh(r, 'git', 'config', 'user.email', 't@example.invalid')
        sh(r, 'git', 'config', 'user.name', 't')
        with open(os.path.join(r, 'a.c'), 'w') as f:
            f.write('int a;\n')
        sh(r, 'git', 'add', '-A')
        sh(r, 'git', 'commit', '-q', '-m', 'develop')
        cls.dev = sh(r, 'git', 'rev-parse', 'HEAD')
        sh(r, 'git', 'update-ref', 'refs/remotes/upstream/develop', 'HEAD')
        with open(os.path.join(r, 'a.c'), 'a') as f:
            f.write('int b;\n')
        sh(r, 'git', 'commit', '-q', '-am', 'work')
        cls.work = sh(r, 'git', 'rev-parse', 'HEAD')
        cls.inst = os.path.join(cls.tmp.name, 'CUBRID-goto-x')
        os.makedirs(cls.inst)
        cls.write_rev(cls.dev[:9])

    @classmethod
    def write_rev(cls, rev9):
        with open(os.path.join(cls.inst, 'BUILD_REV'), 'w', encoding='utf-8') as f:
            f.write('빌드 리비전: %s [CBRD-1] subject\n모드       : debug\n' % rev9)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def tearDown(self):
        self.write_rev(self.dev[:9])

    def check(self, **dr):
        base = {'rev': self.dev[:9], 'install': self.inst, 'result': 'reproduced', 'artifact': 'repro/x.sql'}
        base.update(dr)
        return IG.develop_repro_check({'develop_repro': {k: v for k, v in base.items() if v is not None}}, self.repo)

    def fails(self, out):
        return [m for k, m in out if k == 'develop-repro']

    def test_missing(self):
        out = IG.develop_repro_check({}, self.repo)
        self.assertEqual(len(self.fails(out)), 1)
        self.assertIn('develop_repro 가 없다', out[0][1])

    def test_ok(self):
        self.assertEqual(self.check(), [])

    def test_work_commit_rev(self):
        f = self.fails(self.check(rev=self.work[:9]))
        self.assertTrue(any('upstream/develop 의 커밋이 아니다' in m for m in f), f)

    def test_install_rev_mismatch(self):
        self.write_rev(self.work[:9])
        f = self.fails(self.check())
        self.assertTrue(any('BUILD_REV 첫 줄' in m for m in f), f)

    def test_not_reproduced(self):
        self.assertTrue(any('재현되지 않으면' in m for m in self.fails(self.check(result='not-reproduced'))))

    def test_not_applicable(self):
        self.assertEqual(len(self.fails(self.check(result='not-applicable'))), 1)
        self.assertEqual(self.check(result='not-applicable', note='기능 추가 — 재현할 증상 없음'), [])

    def test_install_gone_is_note(self):
        out = self.check(install=os.path.join(self.tmp.name, 'gone'))
        self.assertEqual(self.fails(out), [])
        self.assertTrue(any(k == 'develop-repro-note' for k, _ in out))


class NumericSwitchGateTest(unittest.TestCase):
    D = """diff --git a/src/q/x.c b/src/q/x.c
--- a/src/q/x.c
+++ b/src/q/x.c
@@ -10,1 +10,1 @@ int f (void)
-  dom = tp_domain_resolve_default (DB_TYPE_DOUBLE);
+  dom = tp_domain_resolve_default (DB_TYPE_NUMERIC);
"""

    def test_needs_performance_effect(self):
        sw, msgs = IG.numeric_switch_check({'adverse_effects': [{'aspect': 'correctness'}]}, self.D, None)
        self.assertEqual(len(sw), 1)
        self.assertEqual([k for k, _ in msgs], ['numeric-switch-note', 'numeric-switch'])
        sw, msgs = IG.numeric_switch_check({'adverse_effects': [{'aspect': 'performance'}]}, self.D, None)
        self.assertEqual([k for k, _ in msgs], ['numeric-switch-note'])

    def test_no_switch(self):
        sw, msgs = IG.numeric_switch_check({'adverse_effects': []}, self.D.replace('DB_TYPE_NUMERIC', 'DB_TYPE_BIGINT'), None)
        self.assertEqual((sw, msgs), ([], []))


if __name__ == '__main__':
    unittest.main()
