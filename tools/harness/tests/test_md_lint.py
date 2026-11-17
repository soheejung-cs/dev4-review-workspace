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


class ToneTest(unittest.TestCase):
    """H-64 ⑦ — 말투 검사(글쓰기-말투.md). 내부 ID 만 거부, 나머지는 경고."""
    def test_internal_ids_fatal_but_code_spans_and_external_sections_ok(self):
        w, f = ML.tone('[코드] [버그 가능성] 🔴\n\nG3-F4 와 H-64, 설계 규칙 §4 를 보세요. `G1-01` 은 코드다. Oracle SQL Reference 10g §8 참고.')
        self.assertEqual(len(f), 1)
        for x in ('G3-F4', 'H-64', '§4'): self.assertIn(x, f[0])
        self.assertNotIn('G1-01', f[0]); self.assertNotIn('§8', f[0])

    def test_tag_lines_coined_words_first_sentence(self):
        body = '[코드] [버그 가능성] 🟡\n\n' + '가' * 160 + '.\n\n왜 문제가 되는지: 닿는 조건이 있다.\n'
        w, f = ML.tone(body)
        self.assertFalse(f)
        self.assertTrue(any('첫 문장' in x for x in w))
        self.assertTrue(any('꼬리표' in x for x in w))
        self.assertTrue(any('지은 말' in x for x in w))

    def test_short_conclusion_first_is_clean(self):
        w, f = ML.tone('[코드] [버그 가능성] 🔴\n\n아래 UPDATE 가 고치면 안 되는 행을 고칩니다.\n\n```sql\nupdate t set a=1;\n```\n')
        self.assertEqual((w, f), ([], []))

    def test_count_warnings(self):
        self.assertEqual(ML.count_warnings(['🔴', '🟡', '🟢']), [])
        w = ML.count_warnings(['🟡'] * 9 + ['🟢', '🟢'])
        self.assertEqual(len(w), 3)


if __name__ == '__main__':
    unittest.main()
