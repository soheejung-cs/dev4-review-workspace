"""pr_refs(H-117) — 네트워크 없이 파싱·판단만. 실행: cd dev4-review-workspace && python3 -m unittest tools.harness.tests.test_pr_refs"""
import unittest
from unittest import mock

from tools.harness import pr_refs as P


class PrRefsTest(unittest.TestCase):
    def test_jira_key_with_korean_particle(self):
        self.assertEqual(P.JIRA_RE.findall('CBRD-27579가 그 DML을 막습니다. CBRD-1234567 아님'), ['CBRD-27579'])

    def test_prereq_unmerged_warns(self):
        body = '이런 INSERT 는 저장 프로시저에서만 일어나고, CBRD-27579가 그 DML을 오류로 막습니다.\nCBRD-28043 을 고칩니다.'
        with mock.patch.object(P, 'jira_prs', side_effect=lambda k, repo: [(8182, 'OPEN')] if k == 'CBRD-27579' else [(8497, 'OPEN')]), \
             mock.patch.object(P, 'jira_description', return_value=''), mock.patch.object(P, 'tc_text', return_value=('', 0)):
            r = P.check(8497, body, 'HEAD', jira_key='CBRD-28043')
        self.assertTrue(any('CBRD-27579' in w and 'develop 에 없다' in w for w in r['warnings']))
        self.assertFalse(any('CBRD-28043' in w for w in r['warnings']))

    def test_code_names_filters_paths_and_tc_names(self):
        t = '{{qo_is_func_index_cover_routed()}} `sql/_13_issues/_26_2h/cases/cbrd_28024.sql` `MALLOC_CHECK_=3` `cbrd_28024` `bug_bts_7459` `pt_cnf ()`'
        self.assertEqual(P.code_names(t), ['pt_cnf', 'qo_is_func_index_cover_routed'])

    def test_tc_claims_pair_short_spans(self):
        body = '- TC: 신규 케이스(`-0`을 낼 수 있는 식 포함)를 추가하고, 플랜에 `covered: true`가 나오는 답안을 갱신했습니다.'
        self.assertEqual([f for _, f in P.tc_claims(body)], ['covered: true'])

    def test_vague_tc_sentence(self):
        self.assertEqual(len(P.tc_vague('그 꼴은 TC·단위 테스트에도 더했습니다. `cbrd_1.sql` 을 TC 에 추가했습니다.')), 1)


if __name__ == '__main__':
    unittest.main()
