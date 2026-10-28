"""md_lint(H-32) — 답글 도구와 인라인 게시 도구가 같은 교정·검사를 쓰는지.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_md_lint"""
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from unittest import mock

from tools.harness import md_lint as ML
from tools.harness import post_review as PRV
from tools.harness import review_reply as RR

TAIL_FENCE = "[코드 리뷰] [주석 제안] 🟢\n\n왜 문제가 되는지: 주석이 코드와 다르다 — 호출자가 잘못 읽는다.\n\n제안: ```suggestion\n/* 새 주석 */\n```\n"


class MdLintTest(unittest.TestCase):
    def test_tail_fence_is_split(self):
        body, fixes, fatal = ML.sanitize_markdown(TAIL_FENCE)
        self.assertIn('\n```suggestion\n', body)
        self.assertFalse(fatal)
        self.assertEqual(len(fixes), 1)

    def test_odd_fence_is_fatal(self):
        _, _, fatal = ML.sanitize_markdown('제안:\n\n```c\nint x;\n')
        self.assertTrue(fatal)

    def test_readability_sections_and_enum(self):
        body, fixes, warns = ML.readability('[코드 리뷰] [확인 질문] 🟡\n결과가 다르다 / 반증 보강: 둘 다 본다 ① 하나 ② 둘')
        self.assertIn('\n\n반증 보강', body)
        self.assertIn('\n- ② 둘', body)
        self.assertEqual(len(fixes), 2)

    def test_long_paragraph_and_budget(self):
        body = '[코드 리뷰] [주석 제안] 🟢\n\n' + '가' * (ML.BUDGET['🟢'] + 10)
        _, _, warns = ML.readability(body)
        self.assertTrue(any('문단 하나가' in w for w in warns))
        self.assertTrue(any('🟢 지적인데' in w for w in warns))

    def test_review_reply_reexports_same_functions(self):
        self.assertIs(RR.sanitize_markdown, ML.sanitize_markdown)
        self.assertIs(RR.readability, ML.readability)


class PostReviewUsesMdLintTest(unittest.TestCase):
    """post_review --draft 가 md_lint 교정을 거쳐 미리보기에 쓰는지(GitHub 호출 없이 diff_lines 를 바꿔 끼운다)."""

    def _run(self, draft_body):
        with tempfile.TemporaryDirectory() as d:
            findings = [{'id': 'g1-01', 'layer': '코드', 'file': 'src/a.c', 'line': 10, 'claim': 'x', 'category': '주석 제안',
                         'comment_basis': 'mismatch', 'status': 'valid'}]
            fp = os.path.join(d, 'findings.json')
            with open(fp, 'w') as f: json.dump(findings, f)
            dp = os.path.join(d, 'comments.draft.md')
            with open(dp, 'w') as f:
                f.write('## 1번 코멘트 — 테스트\n앵커: `src/a.c:10` (g1-01) · 게시 권고: 예\n\n' + draft_body + '\n---\n')
            out = os.path.join(d, 'preview.md')
            argv = ['post_review', '--pr', '1', '--findings', fp, '--draft', dp, '--out', out]
            err = io.StringIO()
            with mock.patch.object(PRV, 'diff_lines', return_value={'src/a.c': {9, 10, 11}}), \
                    mock.patch.object(sys, 'argv', argv), redirect_stderr(err), redirect_stdout(io.StringIO()):
                PRV.main()
            preview = ''
            if os.path.exists(out):
                with open(out) as f: preview = f.read()
            return err.getvalue(), preview

    def test_tail_fence_fixed_in_preview(self):
        err, preview = self._run(TAIL_FENCE)
        self.assertIn('↻ g1-01 교정', err)
        self.assertIn('\n```suggestion\n', preview)
        self.assertIn('앵커 OK 1건', err)

    def test_odd_fence_refused(self):
        err, preview = self._run('[코드 리뷰] [주석 제안] 🟢\n\n제안:\n\n```c\nint x;\n')
        self.assertIn('✗ g1-01', err)
        self.assertIn('앵커 OK 0건', err)


if __name__ == '__main__':
    unittest.main()
