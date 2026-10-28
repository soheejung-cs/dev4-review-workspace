"""팩의 known pitfalls 절(H-29) — 손댄 모듈의 dev4-ai-source modules/<모듈>.md §3 이 실리는가, 해소됨 항목은 첫 줄만인가,
배치로 나뉠 때 그 모듈의 함수가 든 배치에만 들어가는가.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_context_pack_pitfalls"""
import os
import shutil
import tempfile
import unittest

from tools.harness import context_pack as CP

NOTE = """# src/query
## 2. 분석 내용
- 분석은 싣지 않는다
## 3. 예비 이슈 사항

> 미수정 버그·의심 … (안내 인용문은 싣지 않는다)

- **[해소됨 — 662729b67, .50]** `expr_scan_pred_build()` 가 깊이를 세지 않았다
  이어지는 설명 줄은 접힌다
- **[미수정 버그]** `px_scan_slot_iterator.cpp` — `qualified_rows++` 가 필터 판정 앞
  develop 95b79e7ed 에서 잔존 확인.
## 4. 진행중인 작업
- 진행 중은 싣지 않는다
"""


class PitfallsTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='notes-')
        os.makedirs(os.path.join(self.root, 'modules'))
        with open(os.path.join(self.root, 'modules', 'src-query.md'), 'w', encoding='utf-8') as f:
            f.write(NOTE)
        with open(os.path.join(self.root, 'modules', 'src-storage.md'), 'w', encoding='utf-8') as f:
            f.write('## 3. 예비 이슈 사항\n- **[미수정]** 저장소 함정\n## 4.\n')

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def test_module_of(self):
        self.assertEqual(CP._module_of('src/query/parallel/px_scan'), ('src/query', 'src-query.md'))
        self.assertEqual(CP._module_of('pl_engine/pl_server'), ('pl_engine', 'pl_engine.md'))
        self.assertEqual(CP._module_of(''), ('', ''))

    def test_section3_only_and_resolved_folded(self):
        secs = CP._known_pitfalls({'src/query/parallel', 'src/query', 'src/nonote'}, self.root)
        self.assertEqual([s.title for s in secs], ['known pitfalls src/query (dev4-ai-source modules/src-query.md §3)'])
        body = secs[0].body
        self.assertIn('[해소됨 — 662729b67', body)
        self.assertNotIn('이어지는 설명 줄', body)        # 해소됨은 첫 줄만
        self.assertIn('develop 95b79e7ed 에서 잔존', body)   # 미해결은 전문
        self.assertNotIn('분석은 싣지', body)
        self.assertNotIn('진행 중은', body)
        self.assertNotIn('안내 인용문', body)
        self.assertEqual(secs[0].priority, 2)               # 예산 컷에서 잘리지 않는 급

    def test_cap_truncates_with_pointer(self):
        sec = CP._known_pitfalls({'src/query'}, self.root, cap=12)[0]
        self.assertIn('modules/src-query.md §3 을 연다', sec.body.split('\n')[-1])

    def test_batches_scope_pitfalls_to_module(self):
        secs = CP._known_pitfalls({'src/query', 'src/storage'}, self.root)
        pack = CP.ContextPack(budget=400)
        pack.sections = [CP.Section('invariants (always included)', 'inv', 2)] + secs
        for f in ('src/query/a.c:qa', 'src/storage/b.c:sb'):
            pack.sections += [CP.Section(f'changed function {f} [1-9]', 'x' * 800, 1), CP.Section(f'graph evidence {f}', 'g', 2)]
        bs = CP.batches(pack, 400)
        self.assertEqual(len(bs), 2)
        titles = [[s.title for s in b.sections] for b in bs]
        q = next(t for t in titles if any('src/query/a.c' in x for x in t))
        s = next(t for t in titles if any('src/storage/b.c' in x for x in t))
        self.assertTrue(any(x.startswith('known pitfalls src/query') for x in q))
        self.assertFalse(any(x.startswith('known pitfalls src/storage') for x in q))
        self.assertTrue(any(x.startswith('known pitfalls src/storage') for x in s))
        self.assertFalse(any(x.startswith('known pitfalls src/query') for x in s))
        self.assertTrue(all(any(x.startswith('invariants') for x in t) for t in titles))

    def test_batches_implement_pack_keeps_pitfalls(self):
        # 구현 팩(label='candidate function')은 함수 키가 없어 한 덩어리로 간다 — 그래도 함정 절이 붙어야 한다
        secs = CP._known_pitfalls({'src/query'}, self.root)
        pack = CP.ContextPack(budget=300)
        pack.sections = secs + [CP.Section('candidate function src/query/a.c:qa [1-9]', 'x' * 1200, 1)]
        bs = CP.batches(pack, 300)
        self.assertTrue(any(s.title.startswith('known pitfalls src/query') for b in bs for s in b.sections))


if __name__ == '__main__':
    unittest.main()
