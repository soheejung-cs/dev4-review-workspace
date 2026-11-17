"""eval_score 채점 규칙 시험 — 앵커·낱말·unknown·no_finding·missing."""
import json, os, tempfile, unittest
from tools.harness import eval_score as E

TASK = {'id': 't', 'kind': 'regression', 'expect': 'finding',
        'anchor': {'file': 'src/a.c', 'line': 100, 'window': 10}, 'keywords': [['UPDATE', 'DELETE'], ['ON']]}


class T(unittest.TestCase):
    def test_pass_when_anchor_and_words(self):
        f = [{'id': 'g1', 'file': 'src/a.c', 'line': 105, 'claim': 'UPDATE 부질의의 ON 범위가 사라진다'}]
        self.assertEqual(E.judge(TASK, f), ('pass', 'g1'))

    def test_unknown_when_anchor_only(self):
        f = [{'id': 'g2', 'file': 'src/a.c', 'line': 95, 'claim': '다른 결함'}]
        self.assertEqual(E.judge(TASK, f), ('unknown', 'g2'))

    def test_fail_outside_window(self):
        f = [{'id': 'g3', 'file': 'src/a.c', 'line': 130, 'claim': 'UPDATE ON'}]
        self.assertEqual(E.judge(TASK, f), ('fail', None))

    def test_line_range_string(self):
        f = [{'id': 'g4', 'file': './src/a.c', 'line': '101-104', 'claim': 'DELETE 의 ON'}]
        self.assertEqual(E.judge(TASK, f)[0], 'pass')

    def test_no_finding(self):
        t = dict(TASK, expect='no_finding')
        self.assertEqual(E.judge(t, [{'id': 'x', 'file': 'src/a.c', 'line': 100, 'importance': 'low', 'claim': 'UPDATE ON'}])[0], 'pass')
        self.assertEqual(E.judge(t, [{'id': 'y', 'file': 'src/a.c', 'line': 100, 'importance': 'high', 'claim': 'UPDATE ON'}])[0], 'fail')

    def test_missing_and_scores(self):
        d = tempfile.mkdtemp()
        tasks = os.path.join(d, 'tasks.jsonl')
        open(tasks, 'w').write('// c\n' + json.dumps(TASK) + '\n')
        for trial, claim in (('1', 'UPDATE ON'), ('2', '무관'), ('3', None)):
            os.makedirs(os.path.join(d, 'runs', 't', trial))
            if claim:
                json.dump({'findings': [{'id': 'g', 'file': 'src/a.c', 'line': 300 if claim == '무관' else 100, 'claim': claim}]},
                          open(os.path.join(d, 'runs', 't', trial, 'findings.json'), 'w'))
        out = os.path.join(d, 'r.json')
        rc = E.main(['--tasks', tasks, '--runs', os.path.join(d, 'runs'), '--json', out])
        row = json.load(open(out))[0]
        self.assertEqual((row['k'], row['pass@k'], row['pass^k']), (2, True, False))
        self.assertEqual(rc, 1)   # regression 과제가 pass^k 가 아니다


if __name__ == '__main__':
    unittest.main()
