"""static_lint(H-15·H-33) 회귀 — 작은 가짜 리포(base 커밋 + head 커밋)로 checker 마다 잡을 것·넘길 것을 본다.
실행: cd dev4-review-workspace && python3.11 -m unittest tools.harness.tests.test_static_lint
tree-sitter 문법 라이브러리(~/dev/utils/tree-sitter/langs.so)가 없으면 건너뛴다."""
import json
import os
import subprocess
import tempfile
import unittest

from tools.harness import codegraph as CG, context_pack as CP, static_lint as SL

HAVE_TS = os.path.exists(SL.TS_LIB)

HDR = """typedef struct db_monetary { double amount; int type; } DB_MONETARY;
#define OR_CHECK_SHORT_OVERFLOW(i)  ((i) > DB_INT16_MAX || (i) < DB_INT16_MIN)
extern int db_x_encode_core (int *out, int h, int *err);
"""

BASE_C = """#include "x.h"

static int
make_x_conversion (int *v, int h, int *err)
{
  return db_x_encode_core (v, h, err);
}

static int
noop_conversion (int *v)
{
  *v = 0;
  return NO_ERROR;
}

int
conv_short (double d, short *out)
{
  *out = (short) d;
  return 0;
}

int
conv_time (int src, int *v, int *err)
{
  int tmp;
  make_x_conversion (v, src % 86400, err);
  return 0;
}

int
copy_buf (const char *q, int n, char **dst)
{
  *dst = (char *) malloc (n);
  memcpy (*dst, q, n);
  return 0;
}

int
loop_set (int *a, int n)
{
  int i;
  for (i = 0; i < n; i++)
    {
      a[i] = i;
    }
  return 0;
}

void *
slots (int n)
{
  void *p = NULL;
  return p;
}
"""

HEAD_C = BASE_C.replace("""  *out = (short) d;
  return 0;""", """  if (OR_CHECK_SHORT_OVERFLOW (d))
    {
      return -1;
    }
  *out = (short) d;
  return 0;""").replace("""  make_x_conversion (v, src % 86400, err);
  return 0;""", """  make_x_conversion (v, src % 86400, err);
  noop_conversion (v);
  make_x_conversion (&tmp, 0, err);
  return 0;""").replace("""  *dst = (char *) malloc (n);
  memcpy (*dst, q, n);""", """  *dst = (char *) malloc (n);
  if (*dst == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, n);
    }
  memcpy (*dst, q, n);""").replace("""      a[i] = i;
    }""", """      if (a[i] < 0)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  continue;
	}
      a[i] = i;
    }""").replace("""  void *p = NULL;
  return p;""", """  void *p = NULL;
  if (posix_memalign (&p, 64, n) != 0)
    {
      return NULL;
    }
  return p;""")

TMPL = """template <int DST, typename T>
static bool
range_ok (T number)
{
  if constexpr (std::is_floating_point<T>::value)
    {
      return ! (number >= DB_INT16_MIN && number <= DB_INT16_MAX);
    }
  return OR_CHECK_SHORT_OVERFLOW (number);
}

template <int DST, typename T>
static bool
range_bad (T number)
{
  return OR_CHECK_SHORT_OVERFLOW (number);
}

static bool
inline_bad (double v)
{
  return v > DB_INT32_MAX || v < DB_INT32_MIN;
}
"""


def sh(cwd, *a):
    return subprocess.run(list(a), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True, check=True).stdout


@unittest.skipUnless(HAVE_TS, 'tree-sitter 문법 라이브러리 없음')
class StaticLintTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        r = cls.repo = os.path.join(cls.tmp.name, 'repo')
        os.makedirs(os.path.join(r, 'src', 'x'))
        sh(r, 'git', 'init', '-q')
        sh(r, 'git', 'config', 'user.email', 't@example.invalid')
        sh(r, 'git', 'config', 'user.name', 't')
        for rel, body in (('src/x/x.h', HDR), ('src/x/a.c', BASE_C), ('src/x/t.cpp', TMPL)):
            with open(os.path.join(r, rel), 'w') as f:
                f.write(body)
        sh(r, 'git', 'add', '-A')
        sh(r, 'git', 'commit', '-q', '-m', 'base')
        sh(r, 'git', 'update-ref', 'refs/remotes/upstream/develop', 'HEAD')
        with open(os.path.join(r, 'src/x/a.c'), 'w') as f:
            f.write(HEAD_C)
        sh(r, 'git', 'commit', '-q', '-am', 'head')
        cls.diff = sh(r, 'git', 'diff', 'upstream/develop', 'HEAD')
        cls.changed = CP.parse_unified_diff(cls.diff)
        cls.g = CG.CodeGraph.build(r, ['src/x/a.c', 'src/x/x.h', 'src/x/t.cpp'], os.path.join(cls.tmp.name, 'cg.sqlite3'), log=lambda m: None)
        cls.fids = sorted({fn.fid for f, ls in cls.changed.items() for fn in cls.g.functions_in(f, ls)})
        cls.auto, cls.stats = SL.analyze(r, cls.g, cls.changed, cls.fids, cls.diff)
        cls.by = {}
        for f in cls.auto:
            cls.by.setdefault(f['checker'], []).append(f)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_fp_range_outside_on_changed_line(self):
        fs = self.by.get('fp-range-outside', [])
        self.assertEqual(len(fs), 1, fs)
        self.assertEqual((fs[0]['severity'], fs[0]['category'], fs[0]['layer']), ('non-blocking', '버그 가능성', '코드'))
        self.assertIn('OR_CHECK_SHORT_OVERFLOW', fs[0]['claim'])

    def test_discarded_status(self):
        fs = sorted(self.by.get('discarded-status', []), key=lambda f: f['evidence'][0])
        # 바뀌지 않은 줄의 make_x_conversion (src % 86400) 은 선재(question, baseline same), 새로 넣은 noop_conversion 은 늘 성공이라 넘기고,
        # 상수 입력 make_x_conversion (v, 0, err) 은 실패할 수 없으므로 넘긴다
        self.assertEqual(len(fs), 1, fs)
        self.assertEqual(fs[0]['severity'], 'question')
        self.assertTrue(fs[0].get('preexisting'))
        self.assertEqual(fs[0]['baseline']['claim'], 'same')
        self.assertIn('return er_errid ()', fs[0]['proposal'])
        self.assertGreaterEqual(self.stats['discarded-status'].get('always_ok', 0), 1)
        self.assertGreaterEqual(self.stats['discarded-status'].get('const_args', 0), 1)

    def test_error_path(self):
        fs = {f['id'].split('-')[-2]: f for f in self.by.get('error-path-continues', [])}
        verdicts = sorted(f['claim'] for f in self.by.get('error-path-continues', []))
        self.assertEqual(len(verdicts), 2, verdicts)
        uaf = [f for f in self.by['error-path-continues'] if 'NULL 로' in f['claim'] or '그 자원' in f['claim']]
        self.assertEqual(len(uaf), 1)
        self.assertEqual(uaf[0]['severity'], 'non-blocking')
        cont = [f for f in self.by['error-path-continues'] if '`continue`' in f['claim']]
        self.assertEqual(len(cont), 1)
        self.assertEqual(cont[0]['severity'], 'question')

    def test_aligned_alloc(self):
        fs = self.by.get('untracked-aligned-alloc', [])
        self.assertEqual(len(fs), 1)
        self.assertIn('posix_memalign', fs[0]['claim'])

    def test_template_guard_and_inline(self):
        st = {}
        hits = SL.scan_file('src/x/t.cpp', TMPL.encode(), SL.GitSrc(self.repo, 'HEAD'), ['fp-range-outside'], st)
        fns = sorted(h.fn for h in hits)
        self.assertEqual(fns, ['inline_bad', 'range_bad'], [h.detail for h in hits])

    def test_base_line_mapping(self):
        # conv_time 의 바뀌지 않은 호출 줄: head 줄 → base 줄 (앞의 conv_short 에 4줄이 더해졌다)
        head = HEAD_C.split('\n').index('  make_x_conversion (v, src % 86400, err);') + 1
        base = BASE_C.split('\n').index('  make_x_conversion (v, src % 86400, err);') + 1
        self.assertEqual(SL._base_line(self.diff, 'src/x/a.c', head), base)
        added = HEAD_C.split('\n').index('  noop_conversion (v);') + 1
        self.assertIsNone(SL._base_line(self.diff, 'src/x/a.c', added))

    def test_node_appends_and_is_idempotent(self):
        out = tempfile.mkdtemp(dir=self.tmp.name)
        with open(os.path.join(out, 'findings.auto.json'), 'w') as f:
            json.dump([{'id': 'MEAS-07-auto', 'claim': 'x'}], f)
        logs = []
        c = dict(wt=self.repo, g=self.g, changed=self.changed, changed_fids=self.fids, diff=self.diff, out=out, log=logs.append)

        class Ctx(dict):
            def need(self, k):
                return self[k]
        for _ in range(2):                                   # 같은 OUT 에서 두 번 돌아도 LINT 항목이 겹치지 않는다
            SL.node(Ctx(c), {})
        with open(os.path.join(out, 'findings.auto.json')) as f:
            auto = json.load(f)
        self.assertEqual(auto[0]['id'], 'MEAS-07-auto')
        self.assertEqual(len([a for a in auto if a['id'].startswith('LINT-')]), len(self.auto))
        self.assertTrue(os.path.exists(os.path.join(out, 'static_lint.json')))
        self.assertIn('static_lint:', logs[-1])

    def test_findings_pass_schema(self):
        import jsonschema
        with open(os.path.join(os.path.dirname(__file__), '..', '..', '..', 'harness', 'schemas', 'finding.json'), encoding='utf-8') as f:
            schema = json.load(f)
        for f in self.auto:
            errs = list(jsonschema.Draft7Validator(schema).iter_errors(f))
            self.assertFalse(errs, (f['id'], [e.message for e in errs]))


if __name__ == '__main__':
    unittest.main()
