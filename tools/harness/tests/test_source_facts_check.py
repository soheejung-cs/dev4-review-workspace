"""source_facts_check(H-18) 회귀 — 가짜 엔진 리포 두 커밋(기준 A, HEAD B)과 인용 문서로 판정 종류마다 하나씩.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_source_facts_check"""
import os
import subprocess
import tempfile
import unittest

from tools.harness import source_facts_check as SFC

A_C = """/* a.c */
int
foo (int x)
{
  return x + 1;
}

int
bar (int y)
{
  int z = y * 2;
  return z;
}
"""
# HEAD: foo 위에 세 줄이 더해지고(foo·bar 가 3줄 아래로), bar 의 본문 한 줄이 바뀐다
B_C = """/* a.c */
/* added 1 */
/* added 2 */
/* added 3 */
int
foo (int x)
{
  return x + 1;
}

int
bar (int y)
{
  int z = y * 3;
  return z;
}
"""


def sh(cwd, *a):
    return subprocess.run(list(a), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, check=True).stdout.strip()


class SourceFactsCheckTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        r = cls.repo = os.path.join(cls.tmp.name, 'eng')
        os.makedirs(os.path.join(r, 'src', 'x'))
        sh(r, 'git', 'init', '-q')
        sh(r, 'git', 'config', 'user.email', 't@example.invalid')
        sh(r, 'git', 'config', 'user.name', 't')
        with open(os.path.join(r, 'src/x/a.c'), 'w') as f:
            f.write(A_C)
        with open(os.path.join(r, 'src/x/a.h'), 'w') as f:
            f.write('int foo (int x);\n')
        sh(r, 'git', 'add', '-A')
        sh(r, 'git', 'commit', '-q', '-m', 'A')
        cls.A = sh(r, 'git', 'rev-parse', '--short=9', 'HEAD')
        with open(os.path.join(r, 'src/x/a.c'), 'w') as f:
            f.write(B_C)
        sh(r, 'git', 'commit', '-q', '-am', 'B')
        cls.B = sh(r, 'git', 'rev-parse', '--short=9', 'HEAD')
        doc = os.path.join(cls.tmp.name, 'note.md')
        A = cls.A
        with open(doc, 'w') as f:
            f.write('# 노트\n\n작성: t · 기준 리비전: develop `%s` · 2026-10-28\n\n' % A
                    + '- **foo 는 1 을 더한다** (.50, `%s`, 정적): `foo`(a.c:3), 본문 :5.\n' % A
                    + '- **bar 는 두 배** (.50, `%s`, 정적): `bar ()`(a.c:9) 의 계산 a.c:11.\n' % A
                    + '- **틀린 인용** (.50, `%s`): `bar`(a.c:3) 는 bar 가 아니다. 없는 줄 a.c:99.\n' % A
                    + '- **rev 없는 새 사실**: `foo`(a.c:8) — HEAD 에서만 맞다.\n'
                    + '- **헤더 다음 파일명 없는 줄** (.50, `%s`): a.h 의 선언, 정의는 `foo`(:3).\n' % A)
        cls.res = SFC.check_doc(doc, SFC.Repo(r, 'HEAD'), A, 'note.md')
        cls.by = {}
        for x in cls.res:
            cls.by.setdefault(x['cited'], []).append(x)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def st(self, cited, sym=None):
        xs = [x for x in self.by.get(cited, []) if sym is None or x.get('symbol') == sym]
        self.assertTrue(xs, (cited, sorted(self.by)))
        return xs[0]

    def test_moved(self):
        r = self.st('a.c:3', 'foo')
        self.assertEqual(r['status'], 'moved', r)
        self.assertEqual(r['head'], 'a.c:6')
        self.assertEqual(self.st('a.c:5')['status'], 'moved')          # 파일명 없는 `:5` 는 앞의 a.c

    def test_changed_inside_function(self):
        self.assertEqual(self.st('a.c:9', 'bar')['status'], 'moved')
        self.assertEqual(self.st('a.c:11')['status'], 'changed')        # bar 본문의 그 줄이 HEAD 에서 바뀌었다

    def test_errors(self):
        r = self.st('a.c:3', 'bar')
        self.assertEqual(r['status'], 'error')
        self.assertIn('`bar`', r['why'])
        self.assertEqual(self.st('a.c:99')['status'], 'error')

    def test_norev(self):
        r = self.st('a.c:8', 'foo')
        self.assertEqual(r['status'], 'norev', r)                      # 문서 rev(A)의 8줄은 bar 머리, HEAD 의 8줄은 foo 본문

    def test_implicit_header_sibling(self):
        r = [x for x in self.res if x.get('implicit_file') and x['cited'].startswith('a.h:3')]
        self.assertTrue(r)
        self.assertNotEqual(r[0]['status'], 'error', r[0])              # 헤더 다음 `:3` 을 같은 이름 .c 에서 찾았다

    def test_map_line(self):
        repo = SFC.Repo(self.repo, 'HEAD')
        self.assertEqual(repo.map_line(self.A, 'src/x/a.c', 3), 6)
        self.assertIsNone(repo.map_line(self.A, 'src/x/a.c', 11))
        self.assertEqual(repo.map_line(self.A, 'src/x/a.c', 12), 15)


if __name__ == '__main__':
    unittest.main()
