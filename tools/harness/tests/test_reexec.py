"""tools/harness/__init__ 의 재실행 인터프리터 고르기(H-42) — 후보가 yaml 을 import 하지 못하면 건너뛴다.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_reexec"""
import unittest

import tools.harness as H


def fake_env(installed):
    """installed: {'python3.11': {'yaml', 'jsonschema'}, ...} — which·imports_ok 가짜 한 쌍."""
    def which(name):
        return '/usr/bin/' + name if name in installed else None

    def imports_ok(py, mods):
        return set(mods) <= installed[py.rsplit('/', 1)[1]]
    return which, imports_ok


class PickInterpreterTest(unittest.TestCase):
    def pick(self, installed):
        which, ok = fake_env(installed)
        return H._pick_interpreter(which=which, imports_ok=ok)

    def test_newest_with_all_modules(self):
        py, skipped = self.pick({'python3.12': {'yaml', 'jsonschema'}, 'python3.11': {'yaml', 'jsonschema'}})
        self.assertEqual(py, '/usr/bin/python3.12')
        self.assertEqual(skipped, [])

    def test_skips_candidate_without_yaml(self):
        # .51 2026-10-27: python3.11 에 pip·PyYAML 이 없어 재실행된 run 이 import yaml 에서 죽었다
        py, skipped = self.pick({'python3.11': set(), 'python3.9': {'yaml', 'jsonschema'}})
        self.assertEqual(py, '/usr/bin/python3.9')
        self.assertEqual(skipped, [('/usr/bin/python3.11', ('yaml',))])

    def test_no_usable_candidate_stays(self):
        py, skipped = self.pick({'python3.11': set()})
        self.assertIsNone(py)
        self.assertEqual(skipped, [('/usr/bin/python3.11', ('yaml',))])

    def test_yaml_only_is_fallback(self):
        # yaml 만 있는 새 판보다 yaml+jsonschema 를 가진 옛 판을 먼저 고른다(--findings 판정까지 돈다)
        py, _ = self.pick({'python3.12': {'yaml'}, 'python3.8': {'yaml', 'jsonschema'}})
        self.assertEqual(py, '/usr/bin/python3.8')
        py, skipped = self.pick({'python3.12': {'yaml'}})
        self.assertEqual(py, '/usr/bin/python3.12')
        self.assertEqual(skipped, [('/usr/bin/python3.12', ('jsonschema',))])

    def test_no_candidates(self):
        self.assertEqual(self.pick({}), (None, []))


if __name__ == '__main__':
    unittest.main()
