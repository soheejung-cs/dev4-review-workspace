"""precheck(H-31) — 그룹 findings 에 판정기의 결정론 검사만 돌려 precheck.<g>.json 하나만 쓰는가.
임시 git 리포(기준·head 두 커밋)와 그 head 로 만든 codegraph 로 `run full` 이 남기는 OUT 을 흉내 낸다.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_precheck"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

from tools.harness import codegraph as CG
from tools.harness import precheck as PC

BASE = """int f_keep (int x)
{
  return x + 1;
}

int f_change (int x)
{
  int y = x * 2;
  return y;
}
"""
HEAD = BASE.replace('int y = x * 2;', 'int y = x * 3;\n  y = y - 1;')


def git(repo, *a):
    return subprocess.run(['git', '-C', repo, *a], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          universal_newlines=True).stdout.strip()


def finding(fid, line, **kw):
    f = {'id': fid, 'layer': '코드', 'file': 'src/a.c', 'line': line, 'claim': '곱셈 상수가 바뀌어 결과가 달라진다',
         'why': '호출자가 기대하던 값과 달라져 다음 단계의 비교가 다른 분기로 간다 — 결과가 바뀐다.',
         'proposal': '상수를 되돌리거나 호출자의 기대값을 함께 고친다: `int y = x * 2;`', 'evidence': ['src/a.c:8'],
         'severity': 'non-blocking', 'category': '성능 검토', 'rule_ids': ['MEAS-R-26']}
    f.update(kw)
    return f


class PrecheckTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='precheck-t-')
        self.repo = os.path.join(self.tmp, 'repo')
        os.makedirs(os.path.join(self.repo, 'src'))
        git(self.tmp, 'init', '-q', self.repo)
        c = ['-c', 'user.name=t', '-c', 'user.email=t@t']
        with open(os.path.join(self.repo, 'src', 'a.c'), 'w') as f:
            f.write(BASE)
        git(self.repo, 'add', '-A'); git(self.repo, *c, 'commit', '-q', '-m', 'base')
        base = git(self.repo, 'rev-parse', 'HEAD')
        git(self.repo, 'update-ref', 'refs/remotes/upstream/develop', base)
        with open(os.path.join(self.repo, 'src', 'a.c'), 'w') as f:
            f.write(HEAD)
        git(self.repo, *c, 'commit', '-q', '-am', 'head')
        self.head = git(self.repo, 'rev-parse', 'HEAD')
        self.out = os.path.join(self.tmp, 'out')
        os.makedirs(self.out)
        with open(os.path.join(self.out, 'manifest.json'), 'w') as f:
            json.dump({'head': self.head}, f)
        with open(os.path.join(self.out, 'pr.diff'), 'w') as f:
            f.write(git(self.repo, 'diff', base, self.head) + '\n')
        CG.CodeGraph.build(self.repo, ['src/a.c'], os.path.join(self.out, 'codegraph.sqlite3'), log=lambda *_: None).db.close()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_statuses_and_only_output(self):
        fs = [finding('F1', 8),                                          # diff 안, MEAS-R 꼴 규칙 ID
              finding('F2', 99),                                         # head 에 없는 줄
              finding('F3', 8, rule_ids=['ZZZ-99']),                     # 모르는 규칙 ID
              finding('F4', 8, rule_ids=['BR-04/A59']),                  # 판정 행 ID 칸 그대로 — 나눠서 통과
              {k: v for k, v in finding('F5', 8).items() if k != 'why'}]  # 스키마 실패
        src = os.path.join(self.out, 'findings.g7.json')
        with open(src, 'w') as f:
            json.dump(fs, f, ensure_ascii=False)
        before = set(os.listdir(self.out))
        dst, rows = PC.run(self.out, src, self.repo)
        self.assertEqual(os.path.basename(dst), 'precheck.g7.json')
        self.assertEqual(set(os.listdir(self.out)) - before, {'precheck.g7.json'})   # 다른 산출물은 쓰지 않는다
        st = {r['id']: r for r in rows}
        self.assertEqual(st['F1']['status'], 'valid')
        self.assertTrue(any('규칙집에 있음' in p for p in st['F1']['passed']))
        self.assertEqual(st['F2']['status'], 'invalid')
        self.assertTrue(any('모르는 규칙 ID' in r for r in st['F3']['reasons']))
        self.assertEqual(st['F4']['status'], 'valid')
        self.assertEqual(st['F5']['status'], 'schema-fail')

    def test_missing_out_is_explained(self):
        os.remove(os.path.join(self.out, 'pr.diff'))
        with self.assertRaises(SystemExit) as cm:
            PC.run(self.out, os.path.join(self.out, 'x.json'), self.repo)
        self.assertIn('run full', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
