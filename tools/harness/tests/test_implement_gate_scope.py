"""implement_gate 범위 검사의 C++ 이름 대조(in_plan_scope) — 2026-11-18 CBRD-28184.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_implement_gate_scope"""
import unittest

from tools.harness.implement_gate import in_plan_scope

F = 'src/q/a.cpp'
PLAN = {(F, 'trace_handler'), (F, 'add_trace'), (F, 'accumulative_trace_storage'), (F, 'qdump_x')}


class InPlanScope(unittest.TestCase):
    def test_plain_c_name(self):
        self.assertTrue(in_plan_scope(F, 'qdump_x', PLAN))
        self.assertFalse(in_plan_scope(F, 'qdump_y', PLAN))

    def test_qualified_method_needs_class_and_method(self):
        self.assertTrue(in_plan_scope(F, 'trace_handler::add_trace', PLAN))
        self.assertFalse(in_plan_scope(F, 'trace_handler::clear', PLAN))      # 메서드가 plan 에 없다
        self.assertFalse(in_plan_scope(F, 'other_class::add_trace', PLAN))    # 클래스가 plan 에 없다

    def test_class_body(self):
        self.assertTrue(in_plan_scope(F, 'accumulative_trace_storage::?', PLAN))
        self.assertFalse(in_plan_scope(F, 'other_class::?', PLAN))

    def test_template_method(self):
        plan = PLAN | {(F, 'task'), (F, 'initialize')}
        self.assertTrue(in_plan_scope(F, 'task<result_type, ST>::initialize', plan))
        self.assertFalse(in_plan_scope(F, 'task<result_type, ST>::finalize', plan))

    def test_other_file(self):
        self.assertFalse(in_plan_scope('src/q/b.cpp', 'trace_handler::add_trace', PLAN))


if __name__ == '__main__':
    unittest.main()
