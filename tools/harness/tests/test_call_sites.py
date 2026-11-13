"""call_sites(H-177) 회귀 — 판정 함수 고르기·호출 자리·둘러싼 함수·패스.
실행: cd dev4-review-workspace && DEV4_HARNESS_REEXEC=1 python3 -m unittest tools.harness.tests.test_call_sites"""
import os
import subprocess
import tempfile
import unittest

from tools.harness import call_sites as CS

SEM = """static bool pt_is_foldable (PT_NODE * node);

bool
pt_is_foldable (PT_NODE * node)
{
  return node != NULL;
}

static PT_NODE *
pt_semantic_check_local (PARSER_CONTEXT * parser, PT_NODE * node)
{
  if (pt_is_foldable (node))
    {
      return node;
    }
  /* pt_is_foldable (node) in a comment */
  return NULL;
}
"""
TYPE = """#define FOLD(n) pt_is_foldable (n)

PT_NODE *
pt_eval_type (PARSER_CONTEXT * parser, PT_NODE * node)
{
  int do_fold;

  do_fold = pt_is_foldable (node);
  return do_fold ? node : NULL;
}
"""


class CallSitesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        r = cls.repo = cls.tmp.name
        os.makedirs(os.path.join(r, 'src', 'parser'))
        for rel, body in (('src/parser/semantic_check.c', SEM), ('src/parser/type_checking.c', TYPE)):
            with open(os.path.join(r, rel), 'w') as f:
                f.write(body)
        for a in (['init', '-q'], ['config', 'user.email', 't@example.invalid'], ['config', 'user.name', 't'], ['add', '-A'], ['commit', '-q', '-m', 'x']):
            subprocess.run(['git', '-C', r] + a, check=True, stdout=subprocess.PIPE)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_call_sites_skip_definitions_prototypes_macros_comments(self):
        for rev in (None, 'HEAD'):
            s = CS.call_sites(self.repo, 'pt_is_foldable', rev)
            self.assertEqual([(x['file'], x['caller'], x['pass']) for x in s],
                             [('src/parser/semantic_check.c', 'pt_semantic_check_local', '의미 검사'),
                              ('src/parser/type_checking.c', 'pt_eval_type', '타입 계산')], (rev, s))

    def test_predicate_functions(self):
        d = ('diff --git a/src/q/x.c b/src/q/x.c\n--- a/src/q/x.c\n+++ b/src/q/x.c\n'
             '@@ -10,1 +10,1 @@\n-  if (a > 0)\n+  if (a >= 0)\n@@ -40,1 +40,1 @@\n-  x = 1;\n+  x = 2;\n')
        fns = [('src/q/x.c', 'f', 5, 20), ('src/q/x.c', 'g', 30, 50), ('src/q/x.c', 'pt_is_ok', 60, 70)]
        self.assertEqual(CS.predicate_functions(d, fns), [('src/q/x.c', 'f', 5)])
        fns[1] = ('src/q/x.c', 'pt_need_fold', 30, 50)      # 판정꼴 이름이면 조건식이 아니어도
        self.assertEqual([x[1] for x in CS.predicate_functions(d, fns)], ['f', 'pt_need_fold'])

    def test_section_renders_table(self):
        md = CS.section(self.repo, [('src/parser/semantic_check.c', 'pt_is_foldable', 4)])
        self.assertIn('호출 2곳', md)
        self.assertIn('| `src/parser/type_checking.c:8` | `pt_eval_type` | 타입 계산 |', md)
        self.assertEqual(CS.section(self.repo, []), '')


if __name__ == '__main__':
    unittest.main()
