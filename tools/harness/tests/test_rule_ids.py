"""규칙 ID 꼴(H-28) — 규칙 문서에 있는 꼴(MEAS-R-26·APX-A-01·A01·PRIO-04.5·INV-1)을 팩 주입·스키마·판정기가 같이 읽는가,
판정 행이 ID 당 1행으로 들어가는가.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_rule_ids"""
import json
import os
import shutil
import tempfile
import unittest

from tools.harness import adjudicate as AD
from tools.harness import context_pack as CP
from tools.harness import rule_ids as RI

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
FORMS = ['MEM-03', 'ALLOC-02', 'MEAS-R-26', 'APX-A-01', 'APX-B-03', 'A01', 'A66', 'PRIO-04.5', 'INV-1', 'BTL-07', 'CHK-06', 'PHYS-07']


class RuleIdTest(unittest.TestCase):
    def test_forms(self):
        for s in FORMS:
            self.assertTrue(RI.is_rule_id(s), s)
        for s in ('BR-04/A59', 'MEM', 'mem-01', 'L2134', 'CBRD-27701', 'ID', '---'):
            self.assertFalse(RI.is_rule_id(s), s)

    def test_axis(self):
        self.assertEqual([RI.axis(s) for s in ('MEAS-R-26', 'APX-A-01', 'A01', 'PRIO-04.5', 'MEM-03', 'INV-1')],
                         ['MEAS-R', 'APX-A', 'A', 'PRIO', 'MEM', 'INV'])

    def test_row_ids(self):
        self.assertEqual(RI.row_ids('| BR-04/A59 | 문장 | 근거 |\n'), ['BR-04', 'A59'])
        self.assertEqual(RI.row_ids('| PRIO-04.5 | 문장 |'), ['PRIO-04.5'])
        self.assertEqual(RI.row_ids('| ID | 판정 문장 |'), [])
        self.assertEqual(RI.row_ids('|---|---|'), [])
        self.assertEqual(RI.row_ids('| [A] 접속·중계 | x |'), [])

    def test_violated_axes(self):
        self.assertEqual(RI.violated_axes('| A01 | 핫루프 … → ALLOC-01 위반, 루프 밖 예약. | 위반 ALLOC-01 / 대안 |'), {'ALLOC'})
        self.assertEqual(RI.violated_axes('| PRIO-01 | … 순서 위반으로 지적한다 |'), set())

    def test_normalize(self):
        self.assertEqual(RI.normalize(['BR-04/A59', ' MEM-03 ', 'MEM-03', 'A01, A02']), ['BR-04', 'A59', 'MEM-03', 'A01', 'A02'])
        self.assertEqual(RI.normalize(None), [])

    def test_schema_pattern_matches_module(self):
        # JSON 스키마는 import 할 수 없어 같은 문자열을 둔다 — 갈라지면 여기서 잡는다
        for p in ('harness/schemas/finding.json', 'harness/schemas/plan.json'):
            txt = open(os.path.join(ROOT, p), encoding='utf-8').read()
            self.assertIn(json.dumps(RI.ID_PATTERN)[1:-1], txt, p)

    def test_schema_accepts_forms(self):
        import jsonschema
        schema = json.load(open(os.path.join(ROOT, 'harness/schemas/finding.json'), encoding='utf-8'))
        base = {'id': 'x', 'layer': '코드', 'file': 'a.c', 'line': 1, 'claim': 'c', 'why': 'w' * 40, 'proposal': 'p' * 20,
                'evidence': ['a.c:1'], 'severity': 'question', 'category': '성능 검토'}
        jsonschema.validate(dict(base, rule_ids=FORMS), schema)
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(dict(base, rule_ids=['BR-04/A59']), schema)
        ok, bad = AD.validate_findings([dict(base, rule_ids=['BR-04/A59'])], os.path.join(ROOT, 'harness/schemas/finding.json'))
        self.assertEqual((len(ok), len(bad)), (1, 0))      # 판정 행 ID 칸을 그대로 옮긴 것은 나눠서 통과
        self.assertEqual(ok[0]['rule_ids'], ['BR-04', 'A59'])

    def test_known_ids_real_rules(self):
        txt = ''.join(open(os.path.join(ROOT, 'rules', f), encoding='utf-8').read() for f in os.listdir(os.path.join(ROOT, 'rules')) if f.endswith('.md'))
        known = RI.known_ids(txt)
        for s in ('MEAS-R-26', 'A01', 'PRIO-04.5', 'APX-A-01', 'INV-1', 'BTL-01', 'A59', 'COH-04'):
            self.assertIn(s, known)
        self.assertNotIn('L2134', known)


RULES = """# 가짜 규칙
| ID | 판정 문장 |
|---|---|
| MEM-03 | 메모리 첫 행 |
| MEM-03 | 같은 ID 둘째 행 — 상한만 먹는다 |
| BR-04 | 분기 |
| BR-04/A59 | 분기 + 안티패턴 |
| A59 | 안티패턴 A59 단독 — BR-04/A59 가 이미 실었다 |
| A01 | 핫루프 malloc → ALLOC-01 위반 |
| A02 | 무관한 안티패턴 → DS-01 위반 |
| MEAS-01 | 측정 |
| MEAS-R-26 | 플랜 덤프 |
| PRIO-04.5 | 순서 |
"""


class RuleRowsTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix='rules-')
        with open(os.path.join(self.d, 'r.md'), 'w', encoding='utf-8') as f:
            f.write(RULES)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def ids(self, txt):
        return [RI.row_ids(l)[0] if RI.row_ids(l) else l for l in txt.split('\n')]

    def test_one_row_per_id(self):
        rows = CP._rule_rows(self.d, {'MEM', 'BR', 'A'})
        self.assertEqual(self.ids(rows), ['MEM-03', 'BR-04', 'BR-04', 'A01', 'A02'])   # BR-04/A59 는 A59 때문에 실린다
        self.assertEqual(rows.count('같은 ID 둘째 행'), 0)
        self.assertNotIn('A59 단독', rows)

    def test_violation_axis_brings_antipattern(self):
        self.assertEqual(self.ids(CP._rule_rows(self.d, {'ALLOC'})), ['A01'])

    def test_meas_r_only_with_its_axis(self):
        self.assertEqual(self.ids(CP._rule_rows(self.d, {'MEAS'})), ['MEAS-01'])
        self.assertEqual(self.ids(CP._rule_rows(self.d, {'MEAS-R'})), ['MEAS-R-26'])
        self.assertEqual(CP._dir_axes({'src/optimizer/histogram', 'src/query'}), {'MEAS-R'})

    def test_cap_reports_cut(self):
        rows = CP._rule_rows(self.d, {'MEM', 'BR', 'A', 'MEAS', 'MEAS-R', 'PRIO'}, cap=3)
        lines = rows.split('\n')
        self.assertEqual(len(lines), 4)
        self.assertIn('상한 3행', lines[-1])


if __name__ == '__main__':
    unittest.main()
