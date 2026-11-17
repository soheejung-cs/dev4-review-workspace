"""merge_check(H-63 ③)·adjudicate 결과 동일 표시(H-116 ⑥) — 2026-11-17 평가 기준점 사례를 축소한 것.
실행: cd dev4-review-workspace && python3 -m unittest tools.harness.tests.test_merge_check"""
import json
import os
import re
import tempfile
import unittest

from tools.harness import merge_check as MC
from tools.harness.adjudicate import RESULT_SAME_WORDS, DOUBLE_PASS_WORDS


def _w(d, name, obj):
    json.dump(obj, open(os.path.join(d, name), 'w', encoding='utf-8'), ensure_ascii=False)


class MergeCheckTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        g1 = {'id': 'G1-01', 'file': 'px_scan.cpp', 'line': 496, 'claim': 'manager reset 이 m_page_list 를 다시 모으지 않는다', 'why': 'scan_reset_scan_block 이 open 만 부른다'}
        g2 = {'id': 'G2-01', 'file': 'px_heap.cpp', 'line': 64, 'claim': 'reset 이 m_page_list 를 그대로 둔다', 'why': 'scan_reset_scan_block 경로에 build_serial 비교가 없다'}
        g3 = {'id': 'G3-01', 'file': 'other.c', 'line': 10, 'claim': 'qo_unrelated_function 의 오타', 'why': '주석이 틀렸다'}
        _w(self.d, 'findings.G1.json', [g1]); _w(self.d, 'findings.G2.json', [g2]); _w(self.d, 'findings.G3.json', {'findings': [g3]})
        _w(self.d, 'refute.G1.json', [{'id': 'G1-01', 'verdict': 'confirm', 'importance_after': '🟡', 'reason': '경로는 11868 하나'}])
        _w(self.d, 'refute.G2.json', [{'id': 'G2-01', 'verdict': 'confirm', 'importance_after': '🔴', 'reason': '안쪽 NL 스캔도'}])
        _w(self.d, 'refute.G3.json', [{'id': 'G3-01', 'verdict': 'weaken', 'importance_after': '🟢'}])
        self.g2 = g2

    def test_implicit_merge_with_conflicting_refutes(self):
        fs, rep = MC.check(self.d, [dict(self.g2)])
        self.assertTrue(fs[0].get('needs_judgment'))
        self.assertEqual(rep['implicit'][0]['into'], 'G2-01')
        self.assertEqual([s['gone'] for s in rep['silent']], ['G3-01'])   # 겹침 없고 drop 도 아님 → 말없이 사라짐

    def test_explicit_merged_from_same_judgment_ok(self):
        _w(self.d, 'refute.G1.json', [{'id': 'G1-01', 'verdict': 'confirm', 'importance_after': '🔴'}])
        fs, rep = MC.check(self.d, [dict(self.g2, merged_from=['G1-01'])])
        self.assertFalse(fs[0].get('needs_judgment'))
        self.assertEqual(rep['implicit'], [])

    def test_dropped_is_not_silent(self):
        _w(self.d, 'refute.G3.json', [{'id': 'G3-01', 'verdict': 'drop'}])
        _, rep = MC.check(self.d, [dict(self.g2, merged_from=['G1-01'])])
        self.assertEqual(rep['silent'], [])


class ResultSameTest(unittest.TestCase):
    def test_phrases(self):
        for t in ('결과는 바뀌지 않지만 인덱스가', '결과 행은 바뀌지 않는다', '오답은 없다', 'results are unchanged'):
            self.assertTrue(re.search(RESULT_SAME_WORDS, t, re.I), t)
        self.assertFalse(re.search(RESULT_SAME_WORDS, '결과가 다르다', re.I))
        self.assertTrue(re.search(DOUBLE_PASS_WORDS, 'UPDATE/DELETE 는 WHERE 가 두 번 재작성되는데'))


if __name__ == '__main__':
    unittest.main()
