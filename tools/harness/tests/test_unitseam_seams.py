"""unitseam.apply_seams — 어떤 정의를 weak 로 바꾸나.
계기(PR#8497 GD-01, 2026-11-16): page_buffer.c 의 pgbuf_fix_release ()·pgbuf_unfix () 는 debug/release 서명을 #if 로 가르고
`#endif` 뒤에 본문 `{` 이 온다 — 서명 다음 줄만 `{` 로 보던 apply_seams 가 정의를 못 찾아 "seam 정의를 찾지 못함" 후 링크가
multiple definition 으로 죽었다. debug 서명(#else 앞)은 짝 #endif 까지 건너뛰어 찾는다.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_unitseam_seams"""
import unittest

from tools.harness import unitseam as US

SPLIT_SIGNATURE = """\
#if !defined(NDEBUG)
PAGE_PTR
pgbuf_fix_debug (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode,
		 const char *caller_file, int caller_line)
#else /* NDEBUG */
PAGE_PTR
pgbuf_fix_release (THREAD_ENTRY * thread_p, const VPID * vpid,
		   PAGE_FETCH_MODE fetch_mode)
#endif				/* NDEBUG */
{
  return NULL;
}
"""

PLAIN = """\
static int f (int a);

static int
f (int a)
{
  return a;
}

int
g (void);
"""


class ApplySeamsTest(unittest.TestCase):
    def test_release_signature_after_endif(self):
        text, done = US.apply_seams(SPLIT_SIGNATURE, ['pgbuf_fix_release'])
        self.assertEqual(done, [('pgbuf_fix_release', True)])
        lines = text.split('\n')
        # 반환형 줄(release 갈래의 PAGE_PTR)만 weak — debug 갈래는 그대로
        self.assertEqual(lines[5], '__attribute__ ((weak)) PAGE_PTR')
        self.assertEqual(lines[1], 'PAGE_PTR')

    def test_debug_signature_before_else(self):
        # debug 서명 뒤 `#else` 는 짝 `#endif` 까지(release 서명) 건너뛰고 본문 `{` 에 닿는다 — debug 빌드 트리의 seam
        text, done = US.apply_seams(SPLIT_SIGNATURE, ['pgbuf_fix_debug'])
        self.assertEqual(done, [('pgbuf_fix_debug', True)])
        lines = text.split('\n')
        self.assertEqual(lines[1], '__attribute__ ((weak)) PAGE_PTR')
        self.assertEqual(lines[5], 'PAGE_PTR')

    def test_nested_if_inside_else_branch(self):
        text = SPLIT_SIGNATURE.replace('#else /* NDEBUG */\n', '#else /* NDEBUG */\n#if defined (X)\nint x;\n#endif\n')
        out, done = US.apply_seams(text, ['pgbuf_fix_debug'])
        self.assertEqual(done, [('pgbuf_fix_debug', True)])

    def test_plain_definition_and_prototypes(self):
        text, done = US.apply_seams(PLAIN, ['f', 'g'])
        self.assertEqual(done, [('f', True), ('g', False)])
        lines = text.split('\n')
        self.assertEqual(lines[0], 'int f (int a);')                    # static 원형은 static 만 푼다
        self.assertEqual(lines[2], '__attribute__ ((weak)) int')       # 정의의 반환형 줄
        self.assertEqual(lines[8], 'int')                              # g 는 원형뿐 — 그대로


if __name__ == '__main__':
    unittest.main()
