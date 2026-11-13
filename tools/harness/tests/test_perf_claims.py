"""perf_claims 오탐 줄이기(H-167) 회귀 — PR#8472·#8427·#8484 류 정확성 PR 본문에 MEAS 자동 지적이 붙던 꼴.
실행: cd dev4-review-workspace && DEV4_HARNESS_REEXEC=1 python3 -m unittest tools.harness.tests.test_perf_claims"""
import unittest

from tools.harness import perf_claims as PC


def ids(body, title=None):
    f = PC.analyze(body, 'x.c', 1)
    if title is None:
        return sorted(x['id'] for x in f)
    keep, notes = PC.split_by_title(f, title)
    return sorted(x['id'] for x in keep), sorted(x['id'] for x in notes)


class PerfClaimsTest(unittest.TestCase):
    def test_no_change_statement_is_not_a_claim(self):
        self.assertEqual(ids('한두 행만 읽고 끝나는 최적화는 그대로입니다. 성능은 같습니다.'), [])

    def test_https_is_not_tps(self):
        self.assertEqual(ids('재현: https://jira.example.invalid/browse/CBRD-1 의 첨부'), [])

    def test_as_is_table_is_not_a_measurement(self):
        body = '| AS-IS | TO-BE |\n|---|---|\n| 오류 | 결과 |\n'
        self.assertEqual(ids(body), [])

    def test_real_claim_still_flagged(self):
        got = ids('Q1 이 20% 빨라졌다.\n| q | before | after |\n|---|---|---|\n| q1 | 10 | 8 |')
        self.assertIn('MEAS-04-auto', got)       # 반복 횟수 없음
        self.assertIn('MEAS-06-auto', got)       # 율만 있고 절대량 없음
        self.assertIn('MEAS-05-auto', got)       # CTP 근거 없음

    def test_measured_claim_with_reps_and_units(self):
        body = 'Q1 이 빨라졌다: 32.2s → 15.3s (5회 중앙값, MAD 0.3). CTP sql 통과.\n| q1 | 32.2s | 15.3s |'
        self.assertEqual(ids(body), [])

    def test_title_without_perf_signal_moves_to_notes(self):
        body = 'Q1 이 20% 빨라졌다.'
        keep, notes = ids(body, '[CBRD-1] Fix a crash when the key is NULL')
        self.assertEqual(keep, [])
        self.assertTrue(notes)
        keep, notes = ids(body, '[CBRD-2] Speed up the hash join probe')
        self.assertTrue(keep)
        self.assertEqual(notes, [])


if __name__ == '__main__':
    unittest.main()
