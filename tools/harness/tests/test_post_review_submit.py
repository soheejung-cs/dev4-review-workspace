"""post_review 의 인라인 제출(H-43) — 파일 묶음으로 나눠 순서대로, 이미 있는 본문은 건너뛰고, 5xx·타임아웃 뒤에는
다시 보내지 않고 GET 으로 생성 여부를 확인하는가. gh 를 가짜로 바꿔 돌린다 — GitHub 에 아무것도 보내지 않는다.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_post_review_submit"""
import io
import json
import subprocess
import unittest
from unittest import mock

from tools.harness import post_review as PRV


def cm(path, n, body=None):
    return {'path': path, 'line': n, 'side': 'RIGHT', 'body': body or f'[코드 리뷰] [확인 질문] 🟡\n\n{path}:{n} 본문\n'}


class FakeGh:
    """gh 호출을 흉내 낸다. posts: POST 마다 돌려줄 결과 목록 — 'ok' | 'http422' | 'http502-created' | 'http502-lost' | 'timeout-created'.
    '-created' 는 서버가 에러를 내고도 리뷰를 만든 경우(2026-10-27 PR#8196)."""

    def __init__(self, existing=(), posts=()):
        self.inline = [{'path': p, 'body': b} for p, b in existing]
        self.issue = []
        self.posts = list(posts)
        self.calls = []

    def __call__(self, cmd, inp=None, timeout=None):
        self.calls.append(cmd)
        if '-X' not in cmd:   # GET 목록
            rows = self.inline if '/pulls/' in cmd[3] else [{'body': b} for b in self.issue]
            return subprocess.CompletedProcess(cmd, 0, '\n'.join(json.dumps(r, ensure_ascii=False) for r in rows) + '\n', '')
        kind = self.posts.pop(0)
        payload = None
        if '--input' in cmd:
            with open(cmd[cmd.index('--input') + 1], encoding='utf-8') as f:
                payload = json.load(f)
        if kind.endswith('created') or kind == 'ok':
            for c in (payload or {}).get('comments', []):
                self.inline.append({'path': c['path'], 'body': c['body']})
        if kind == 'ok':
            return subprocess.CompletedProcess(cmd, 0, json.dumps({'html_url': f'https://x/review/{len(self.calls)}'}), '')
        if kind == 'http422':
            return subprocess.CompletedProcess(cmd, 1, '{"message":"Line could not be resolved"}', 'gh: Unprocessable Entity (HTTP 422)')
        if kind.startswith('timeout'):
            raise subprocess.TimeoutExpired(cmd, timeout)
        return subprocess.CompletedProcess(cmd, 1, '', 'gh: Server Error (HTTP 502)')

    def posted_batches(self):
        return [c for c in self.calls if '-X' in c]


class SubmitTest(unittest.TestCase):
    def run_post(self, fake, comments, cap=PRV.MAX_PER_REVIEW):
        err = io.StringIO()
        with mock.patch.object(PRV, 'sh', fake), mock.patch.object(PRV.time, 'sleep', lambda s: None):
            res = PRV.post_inline(8196, 'CUBRID/cubrid', comments, cap, out=err)
        return res, err.getvalue()

    def test_batches_by_file_in_order_and_capped(self):
        cs = [cm('b.c', 1), cm('a.c', 1), cm('b.c', 2), cm('b.c', 3)]
        self.assertEqual([[c['path'] for c in g] for g in PRV.batches_by_file(cs, cap=2)], [['b.c', 'b.c'], ['b.c'], ['a.c']])

    def test_one_review_per_file(self):
        fake = FakeGh(posts=['ok', 'ok'])
        (urls, errors, left), _ = self.run_post(fake, [cm('a.c', 1), cm('b.c', 1), cm('a.c', 2)])
        self.assertEqual((len(urls), errors, left), (2, [], []))
        self.assertEqual(len(fake.posted_batches()), 2)

    def test_existing_bodies_are_skipped(self):
        c1, c2 = cm('a.c', 1), cm('a.c', 2)
        fake = FakeGh(existing=[('a.c', c1['body'].replace('\n', '\r\n') + '  ')], posts=['ok'])   # 줄끝·공백 차이는 같은 본문
        (urls, _, _), err = self.run_post(fake, [c1, c2])
        self.assertIn('1건은 건너뜀', err)
        self.assertEqual(len(fake.posted_batches()), 1)
        self.assertEqual(sum(1 for r in fake.inline if r['path'] == 'a.c'), 2)   # 있던 1 + 새로 1(c2) — c1 은 다시 가지 않았다

    def test_422_goes_on_to_next_file(self):
        fake = FakeGh(posts=['http422', 'ok'])
        (urls, errors, left), err = self.run_post(fake, [cm('a.c', 1), cm('b.c', 1)])
        self.assertEqual((len(urls), len(errors), left), (1, 1, []))
        self.assertIn('거부', err)

    def test_5xx_that_created_is_not_retried(self):
        fake = FakeGh(posts=['http502-created', 'ok'])
        (urls, errors, left), err = self.run_post(fake, [cm('a.c', 1), cm('a.c', 2), cm('b.c', 1)])
        self.assertEqual((errors, left), ([], []))
        self.assertEqual(len(fake.posted_batches()), 2)          # a.c 는 한 번만 POST
        self.assertIn('재시도하지 않고', err)
        self.assertEqual(sum(1 for r in fake.inline if r['path'] == 'a.c'), 2)   # 중복 없음

    def test_timeout_that_created_is_not_retried(self):
        fake = FakeGh(posts=['timeout-created'])
        (urls, errors, left), _ = self.run_post(fake, [cm('a.c', 1)])
        self.assertEqual((errors, left, len(fake.posted_batches())), ([], [], 1))

    def test_5xx_lost_stops_without_retry(self):
        fake = FakeGh(posts=['http502-lost'])
        (urls, errors, left), err = self.run_post(fake, [cm('a.c', 1), cm('b.c', 1)])
        self.assertEqual(len(fake.posted_batches()), 1)           # 다시 보내지도, 다음 묶음으로 가지도 않는다
        self.assertEqual([[c['path'] for c in g] for g in left], [['a.c'], ['b.c']])
        self.assertIn('멈춘다', err)
        # 사람이 확인한 뒤 같은 명령을 다시 실행하면 — 그새 서버가 a.c 를 만들었어도 중복 없이 b.c 만 올린다
        fake.inline.append({'path': 'a.c', 'body': cm('a.c', 1)['body']})
        fake.posts = ['ok']
        (urls, errors, left), _ = self.run_post(fake, [cm('a.c', 1), cm('b.c', 1)])
        self.assertEqual((len(urls), errors, left), (1, [], []))
        self.assertEqual(sum(1 for r in fake.inline if r['path'] == 'a.c'), 1)

    def test_unreadable_list_posts_nothing(self):
        class Down(FakeGh):
            def __call__(self, cmd, inp=None, timeout=None):
                self.calls.append(cmd)
                return subprocess.CompletedProcess(cmd, 1, '', 'gh: error connecting to api.github.com')
        fake = Down()
        (urls, errors, left), _ = self.run_post(fake, [cm('a.c', 1)])
        self.assertEqual((urls, len(errors), len(left), fake.posted_batches()), ([], 1, 1, []))

    def test_issue_comment_dedup_and_5xx(self):
        fake = FakeGh(posts=['http502-created'])
        fake.issue = []
        orig = fake.__call__

        def call(cmd, inp=None, timeout=None):   # 일반 코멘트 POST 가 5xx 를 내고도 만들어진 경우
            if '-X' in cmd and '/issues/' in cmd[4]:
                fake.calls.append(cmd)
                with open(cmd[cmd.index('-F') + 1].split('@', 1)[1], encoding='utf-8') as f:
                    body = f.read()
                fake.issue.append(body)
                return subprocess.CompletedProcess(cmd, 1, '', 'gh: Server Error (HTTP 500)')
            return orig(cmd, inp, timeout)
        err = io.StringIO()
        with mock.patch.object(PRV, 'sh', call), mock.patch.object(PRV.time, 'sleep', lambda s: None):
            self.assertTrue(PRV.post_issue_comment(1, 'CUBRID/cubrid', '본문 A', out=err))
            self.assertTrue(PRV.post_issue_comment(1, 'CUBRID/cubrid', '본문 A', out=err))   # 다시 실행 — 같은 본문이 있어 건너뜀
        self.assertEqual(fake.issue, ['본문 A'])
        self.assertIn('건너뜀', err.getvalue())


if __name__ == '__main__':
    unittest.main()
