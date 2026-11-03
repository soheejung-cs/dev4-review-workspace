"""놓친 지적 대조(missed_review) — 다른 리뷰어 지적과 우리 findings·스레드를 근처·같은 심볼·놓침 후보·요약으로 가르는가.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_missed_review"""
import unittest

from tools.harness import missed_review as M

RUN = {'findings': [{'id': 'G1-01', 'file': 'src/a.c', 'line': 100, 'text': '`foo_bar ()` 가 캐시 키를 모른다'},
                    {'id': 'G2-02', 'file': 'src/b.c', 'line': 50, 'text': 'qo_discover_edges 가 다시 정렬하지 않는다'}],
       'changed': set(), 'codegraph': '/nonexistent'}


def item(path='', line=0, body='', kind='thread'):
    return {'kind': kind, 'path': path, 'line': line, 'body': body}


class MatchTest(unittest.TestCase):
    def test_near_same_file_within_window(self):
        self.assertEqual(M.match(item('src/a.c', 110, 'x'), RUN, [])[0], '근처')

    def test_outside_window_is_not_near(self):
        self.assertEqual(M.match(item('src/a.c', 100 + M.DEFAULT_WINDOW + 1, '아무 말'), RUN, [])[0], '놓침 후보')

    def test_shared_symbol_on_another_file(self):
        m, ev = M.match(item('src/c.c', 5, '`foo_bar ()` 가 값을 공유한다'), RUN, [])
        self.assertEqual(m, '같은 심볼')
        self.assertIn('foo_bar', ev[0])

    def test_our_thread_counts_as_near(self):
        mine = [{'path': 'src/z.c', 'line': 10, 'url': 'u', 'first_body': ''}]
        self.assertEqual(M.match(item('src/z.c', 12, 'x'), None, mine)[0], '근처')

    def test_review_body_without_symbols_is_summary(self):
        self.assertEqual(M.match(item(body='의견이 모두 반영되어 approve 합니다.', kind='review'), RUN, [])[0], '요약')

    def test_review_body_with_symbol_is_compared(self):
        self.assertEqual(M.match(item(body='`qo_discover_edges` 정렬이 틀렸다', kind='review'), RUN, [])[0], '같은 심볼')

    def test_no_harness_output_means_missed_candidate(self):
        self.assertEqual(M.match(item('src/a.c', 100, '`foo_bar ()`'), None, [])[0], '놓침 후보')


class PackTest(unittest.TestCase):
    def test_no_run(self):
        self.assertEqual(M.pack_status(None, 'src/a.c', 1)[0], '하네스 산출 없음')

    def test_file_in_pack_but_unknown_function(self):
        run = {'changed': {'src/a.c:foo'}, 'codegraph': '/nonexistent'}
        self.assertEqual(M.pack_status(run, 'src/a.c', 5)[0], '팩 파일, 변경 함수 밖')
        self.assertEqual(M.pack_status(run, 'src/q.c', 5)[0], '팩 밖')


if __name__ == '__main__':
    unittest.main()
