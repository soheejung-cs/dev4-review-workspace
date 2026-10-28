"""review_threads 의 지적 시점 커밋 표시(H-30) — originalCommit ≠ head 인 미해결 스레드에 ⚠ 를 달고,
로컬 리포에 두 커밋이 있으면 그 사이 스레드 파일이 바뀌었는지 적는가. gh 를 부르지 않는다(가짜 GraphQL 응답).
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_review_threads"""
import os
import shutil
import subprocess
import tempfile
import unittest

from tools.harness import review_threads as RT


def git(repo, *a):
    return subprocess.run(['git', '-C', repo, *a], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True).stdout.strip()


def node(head, threads):
    return {'headRefOid': head, 'author': {'login': 'me'}, 'reviewThreads': {'nodes': threads}}


def thread(path, orig, resolved=False, last='rev'):
    cs = [{'author': {'login': 'rev'}, 'createdAt': '2026-10-21T00:00:00Z', 'body': 'x 가 틀렸다', 'url': 'u', 'originalCommit': {'oid': orig}}]
    if last == 'me':
        cs.append({'author': {'login': 'me'}, 'createdAt': '2026-10-22T00:00:00Z', 'body': '고쳤습니다', 'url': 'u2', 'originalCommit': {'oid': orig}})
    return {'id': 'T', 'isResolved': resolved, 'isOutdated': False, 'path': path, 'line': 3, 'originalLine': 3, 'comments': {'nodes': cs}}


class ThreadsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='threads-t-')
        self.repo = os.path.join(self.tmp, 'r')
        os.makedirs(self.repo)
        git(self.tmp, 'init', '-q', self.repo)
        c = ['-c', 'user.name=t', '-c', 'user.email=t@t']
        for name in ('a.c', 'b.c'):
            with open(os.path.join(self.repo, name), 'w') as f:
                f.write('int x;\n')
        git(self.repo, 'add', '-A'); git(self.repo, *c, 'commit', '-q', '-m', 'one')
        self.c1 = git(self.repo, 'rev-parse', 'HEAD')
        with open(os.path.join(self.repo, 'a.c'), 'w') as f:
            f.write('int x = 1;\n')
        git(self.repo, *c, 'commit', '-q', '-am', 'two')
        self.c2 = git(self.repo, 'rev-parse', 'HEAD')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_stale_and_changed_since(self):
        _, ts = RT.classify(node(self.c2, [thread('a.c', self.c1), thread('b.c', self.c1), thread('a.c', self.c2),
                                           thread('a.c', 'f' * 40)]), repo_dir=self.repo)
        self.assertEqual([t['stale'] for t in ts], [True, True, False, True])
        self.assertEqual([t['path_changed_since'] for t in ts], [True, False, None, None])   # 마지막은 로컬에 없는 커밋
        self.assertEqual(ts[0]['commits_since'], 1)
        self.assertIn('그 뒤 이 파일이 바뀜', RT.stale_note(ts[0]))
        self.assertIn('그대로', RT.stale_note(ts[1]))
        self.assertEqual(RT.stale_note(ts[2]), '')

    def test_md_marks_unresolved_only(self):
        a, ts = RT.classify(node(self.c2, [thread('a.c', self.c1), thread('a.c', self.c1, resolved=True), thread('b.c', self.c1, last='me')]),
                            repo_dir=self.repo)
        md = RT.to_md({'pr': 1, 'head': self.c2, 'collected': 'now', 'author': a, 'threads': ts})
        self.assertIn('미해결 2건이 지금 head 와 다른 커밋', md)
        heads = [l for l in md.split('\n') if l.startswith('### ')]
        self.assertEqual(sum('⚠ 지적 시점' in l for l in heads), 2)           # resolved 스레드에는 달지 않는다
        self.assertEqual([t['class'] for t in ts], ['대응 필요', 'resolved', 'resolve 대기'])

    def test_no_original_commit(self):
        t = thread('a.c', '')
        t['comments']['nodes'][0]['originalCommit'] = None
        _, ts = RT.classify(node(self.c2, [t]), repo_dir=self.repo)
        self.assertEqual((ts[0]['original_commit'], ts[0]['stale']), ('', False))


if __name__ == '__main__':
    unittest.main()
