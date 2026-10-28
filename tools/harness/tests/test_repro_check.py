"""repro_check(H-27) 회귀 — 초안 절 파싱·conf 사본·--verify. SA 실행은 설치본이 필요해 기본으로 돌리지 않는다
(REPRO_CHECK_INSTALL=<설치본> 을 주면 그 설치본을 develop·PR 양쪽으로 한 절을 실제로 돌린다 — 버릴 DB 를 만들고 지운다).
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_repro_check"""
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from tools.harness import repro_check as RC

DRAFT = """# 초안

## 1번 코멘트 — a.c:1
앵커: `src/a.c:1` (g1-01)

[코드 리뷰] [확인 질문] 🟡

```sql
create table t (a int);
```

뒤이어:

```sql
select a from t;
```

```suggestion
  int a;
```

---

## 2번 코멘트 — sql 없음
[코드 리뷰] [주석 제안] 🟢
```c
int x;
```
"""

REPLIES = """## T12 — PR 코멘트에 답
> [코드 리뷰] 재현했습니다.
>
> ```sql
> select 1 from db_root;
> ```
"""


class SectionsTest(unittest.TestCase):
    def test_comment_draft(self):
        secs = RC.sections(DRAFT)
        self.assertEqual([(k, len(b)) for k, _, b in secs], [('1번', 2), ('2번', 0)])
        self.assertEqual(secs[0][2], ['create table t (a int);', 'select a from t;'])   # suggestion·c 울타리는 돌리지 않는다

    def test_replies_quoted(self):
        secs = RC.sections(REPLIES)
        self.assertEqual(secs, [('T12', 'T12 — PR 코멘트에 답', ['select 1 from db_root;'])])


class ConfTest(unittest.TestCase):
    def test_buffer_cap_and_params(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, 'conf'))
            with open(os.path.join(d, 'conf', 'cubrid.conf'), 'w') as f:
                f.write('[common]\ndata_buffer_size=16G\n')
            conf, notes = RC._conf(d, ['max_plan_cache_entries=0'], '2G', os.path.join(d, 'c.conf'))
            with open(conf) as f:
                txt = f.read()
            self.assertIn('max_plan_cache_entries=0', txt)
            self.assertIn('data_buffer_size=2G', txt)
            self.assertTrue(any('16G → 2G' in n for n in notes))
            conf2, notes2 = RC._conf(d, [], '32G', os.path.join(d, 'c2.conf'))
            self.assertIsNone(conf2)                         # 손댈 것이 없으면 사본을 만들지 않는다(설치본 conf 그대로)


class VerifyTest(unittest.TestCase):
    def test_verify(self):
        with tempfile.TemporaryDirectory() as d:
            dp = os.path.join(d, 'comments.draft.md')
            with open(dp, 'w') as f:
                f.write(DRAFT)
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(RC.verify(dp, d), 1)        # 산출물 없음
            rec = {'draft_sha1': RC.sha1(dp), 'installs': {'dev': {'rev': 'a'}, 'pr': {'rev': 'b'}}, 'finished': 'x',
                   'sections': [{'key': '1번', 'dev': {'ran': True}, 'pr': {'ran': True}}]}
            with open(os.path.join(d, 'repro_check.json'), 'w') as f:
                json.dump(rec, f)
            with redirect_stdout(buf):
                self.assertEqual(RC.verify(dp, d), 0)
            rec['sections'][0]['pr']['ran'] = False
            with open(os.path.join(d, 'repro_check.json'), 'w') as f:
                json.dump(rec, f)
            with redirect_stdout(buf):
                self.assertEqual(RC.verify(dp, d), 1)        # 안 돈 절
            with open(dp, 'a') as f:
                f.write('\n고침\n')
            with redirect_stdout(buf):
                self.assertEqual(RC.verify(dp, d), 1)        # 초안이 바뀜
            self.assertIn('초안이 재실행 뒤 바뀌었다', buf.getvalue())


@unittest.skipUnless(os.environ.get('REPRO_CHECK_INSTALL'), 'REPRO_CHECK_INSTALL 미설정 — SA 실행 시험 생략')
class LiveTest(unittest.TestCase):
    def test_run(self):
        inst = os.path.expanduser(os.environ['REPRO_CHECK_INSTALL'])
        with tempfile.TemporaryDirectory() as d:
            dp = os.path.join(d, 'comments.draft.md')
            with open(dp, 'w') as f:
                f.write(DRAFT)
            with redirect_stdout(io.StringIO()):
                rc = RC.main(['--draft', dp, '--dev', inst, '--pr', inst])
                self.assertEqual(rc, 0)
                self.assertEqual(RC.verify(dp, d), 0)
            with open(os.path.join(d, 'repro_check.json')) as f:
                rec = json.load(f)
            self.assertTrue(rec['sections'][0]['same'])
            self.assertFalse(os.path.exists(os.path.join(d, 'repro_check', 'db')))   # 버릴 DB 를 지웠다


if __name__ == '__main__':
    unittest.main()
