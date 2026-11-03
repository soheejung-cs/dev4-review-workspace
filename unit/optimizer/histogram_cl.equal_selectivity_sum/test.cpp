/*
 * PR#8242 (CBRD-27478) -- the equality estimate of many constants against one column with the histogram
 * opened once (src/optimizer/histogram/histogram_cl.cpp, cs mode), and the join selectivity the row
 * snapshot derives from it (src/optimizer/histogram/row_snapshot_cl.cpp, linked as an extra TU).
 *
 *   histogram_get_equal_selectivity      one constant. Head: a wrapper over the new TU static
 *                                        histogram_equal_selectivity_with_reader (); base: the same body
 *                                        inline. Runs at both -- the split must not move an estimate.
 *   histogram_get_equal_selectivity_sum  new: the per-constant estimates added in order, NULL skipped,
 *                                        one unpriceable constant fails the sum, one reader open per call.
 *   row_snapshot_join_selectivity        new: (1 / k) * SUM P (other = v_i) * (1 - nf_other), k = every
 *                                        snapshot row (NULL keys included), false when the sum is 0 or a
 *                                        key cannot be priced.
 *
 * Real code: the estimator (histogram_extract_key, histogram_key_kind_for_type, the MCV / residual
 * formula, the null-mass division, the clamps), HistogramReader (histogram_reader.cpp compiled into this
 * test, byte_order.c behind OR_GET_*), row_snapshot_join_selectivity (row_snapshot_cl.cpp, --extra).
 * Seam: histogram_init_reader_from_lhs (histogram_cl.cpp:724, TU static) -- the fake is its body minus the
 * class-cache branch, which runs only inside a bind fingerprint walk (bind_fp_active_statement != NULL),
 * never on these paths: the end of a path, a PT_NAME, the name's histogram annotation, db_get_bit,
 * HistogramReader::reset. It counts the opens.
 *
 * The functions the PR added are declared weak, and row_snapshot_cl.hpp is included only where it
 * exists: at a revision without them those cases FAIL with "absent", the per-constant cases still run.
 * Every case is [contract]. Expected numbers come from an oracle over the blob spec written here
 * (the documented formula, histogram_cl.cpp histogram_equal_selectivity_with_reader), not from the code.
 */
#include "catch.hpp"

#include "byte_order.c"
#include "histogram_reader.cpp"

#include "dbtype_def.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "object_representation.h"
#include "histogram_cl.hpp"
#if __has_include ("row_snapshot_cl.hpp")
#include "row_snapshot_cl.hpp"
#define UNITSEAM_HAVE_ROW_SNAPSHOT 1
#else
#define UNITSEAM_HAVE_ROW_SNAPSHOT 0
#endif

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------- functions PR#8242 added (weak) */

void histogram_get_equal_selectivity_sum (PT_NODE *lhs, DB_VALUE **values, int n, double *sum, bool *success)
__attribute__ ((weak));
#if UNITSEAM_HAVE_ROW_SNAPSHOT
bool row_snapshot_join_selectivity (const ROW_SNAPSHOT *snapshot, int column, PT_NODE *other, double nf_other,
				    double *selectivity) __attribute__ ((weak));
#endif

#define REQUIRE_SUM_PRESENT() \
  do { if (&histogram_get_equal_selectivity_sum == nullptr) \
	 FAIL ("histogram_get_equal_selectivity_sum absent at this revision (new in PR#8242)"); } while (0)

/* ---------------------------------------------------------------- seam */

static int g_opens;		/* histogram_init_reader_from_lhs calls */

/* histogram_cl.cpp:724 histogram_init_reader_from_lhs, without the class-cache branch (see the header) */
bool
histogram_init_reader_from_lhs (PT_NODE *lhs, hist::HistogramReader &reader)
{
  g_opens++;
  while (lhs != NULL && lhs->node_type == PT_DOT_)
    {
      lhs = lhs->info.dot.arg2;	/* pt_get_end_path_node (parser_support.c) */
    }
  if (lhs == NULL || lhs->node_type != PT_NAME)
    {
      return false;
    }
  DB_VALUE *histogram_value = lhs->info.name.histogram;
  if (histogram_value == NULL)
    {
      return false;
    }
  int length = 0;
  const char *p = db_get_bit (histogram_value, &length);
  if (p == NULL || length <= 0)
    {
      return false;
    }
  return reader.reset (std::string_view (p, static_cast<std::size_t> (length / 8))) == NO_ERROR;
}

/* ---------------------------------------------------------------- HST2 blobs */

/* written with the OR_* macros the builder uses (histogram_builder.cpp write<> / write_value_slot / build):
 * header 48 B, MCV records (8 B value slot + f64 freq, ascending), bucket records (8 B hi + i64 cumulative +
 * i64 approx_ndv). Numeric columns only: no string blob. */
namespace blob
{
  const int MAGIC = 0, VERSION = 4, NMCV = 8, NBUCKETS = 12, STR_SIZE = 16, TYPE = 20, TOTAL_SIZE = 24,
	    PRECISION = 28, TOTAL_ROWS = 32, NULL_FREQ = 40, HEADER = 48;
  const int MCV_REC = 16, BUCKET_REC = 24;

  struct spec
  {
    DB_TYPE type = DB_TYPE_INTEGER;
    std::vector<std::pair<double, double>> mcvs;	/* value, frequency over all rows; ascending */
    std::vector<std::tuple<double, std::int64_t, std::int64_t>> buckets;	/* hi, cumulative, approx_ndv */
    std::int64_t total_rows = 1000;
    double null_freq = 0.0;
  };

  static void
  slot (char *at, DB_TYPE type, double v)
  {
    if (type == DB_TYPE_DOUBLE || type == DB_TYPE_FLOAT || type == DB_TYPE_NUMERIC || type == DB_TYPE_MONETARY)
      {
	OR_PUT_DOUBLE (at, v);
      }
    else
      {
	std::int64_t i = (std::int64_t) v;
	OR_PUT_INT64 (at, &i);
      }
  }

  static std::string
  make (const spec &s)
  {
    const std::size_t size = HEADER + MCV_REC * s.mcvs.size () + BUCKET_REC * s.buckets.size ();
    std::string b (size, '\0');

    for (std::size_t i = 0; i < s.mcvs.size (); i++)
      {
	const std::size_t at = HEADER + MCV_REC * i;
	slot (&b[at], s.type, s.mcvs[i].first);
	OR_PUT_DOUBLE (&b[at + 8], s.mcvs[i].second);
      }
    for (std::size_t j = 0; j < s.buckets.size (); j++)
      {
	const std::size_t at = HEADER + MCV_REC * s.mcvs.size () + BUCKET_REC * j;
	std::int64_t cum = std::get<1> (s.buckets[j]), ndv = std::get<2> (s.buckets[j]);
	slot (&b[at], s.type, std::get<0> (s.buckets[j]));
	OR_PUT_INT64 (&b[at + 8], &cum);
	OR_PUT_INT64 (&b[at + 16], &ndv);
      }
    memcpy (&b[MAGIC], "HST2", 4);
    OR_PUT_INT (&b[VERSION], 2);
    OR_PUT_INT (&b[NMCV], (int) s.mcvs.size ());
    OR_PUT_INT (&b[NBUCKETS], (int) s.buckets.size ());
    OR_PUT_INT (&b[STR_SIZE], 0);
    OR_PUT_INT (&b[TYPE], (int) s.type);
    OR_PUT_INT (&b[TOTAL_SIZE], (int) b.size ());
    OR_PUT_INT (&b[PRECISION], 0);
    std::int64_t rows = s.total_rows;
    OR_PUT_INT64 (&b[TOTAL_ROWS], &rows);
    OR_PUT_DOUBLE (&b[NULL_FREQ], s.null_freq);
    return b;
  }

  /* histogram_equal_selectivity_with_reader's formula over the spec: an MCV's frequency, else the
   * residual mass (1 - SUM mcv - null) over the non-MCV distinct values (1 / total_rows when there are
   * none); then divided by the non-null fraction, clamped to [0, 1], 0 lifted to 1 / total_rows */
  static double
  oracle (const spec &s, double v)
  {
    double sum_mcv = 0.0, sel = -1.0;
    std::int64_t ndv = 0;

    for (const auto &m : s.mcvs)
      {
	sum_mcv += m.second;
      }
    for (const auto &bk : s.buckets)
      {
	ndv += std::get<2> (bk);
      }
    for (const auto &m : s.mcvs)
      {
	if (m.first == v)
	  {
	    sel = m.second;
	  }
      }
    if (sel < 0.0)
      {
	double rest = 1.0 - sum_mcv - s.null_freq;
	if (rest < 0.0)
	  {
	    rest = 0.0;
	  }
	sel = (ndv >= 1) ? rest / (double) ndv : 1.0 / (double) s.total_rows;
      }
    const double nonnull = 1.0 - s.null_freq;
    if (nonnull > 1e-9)
      {
	sel /= nonnull;
      }
    if (sel < 0.0)
      {
	sel = 0.0;
      }
    if (sel > 1.0)
      {
	sel = 1.0;
      }
    if (sel <= 0.0)
      {
	sel = 1.0 / (double) s.total_rows;
      }
    return sel;
  }
}

/* ---------------------------------------------------------------- columns and constants */

static std::deque<std::string> g_bytes;	/* blob storage (stable addresses) */
static std::deque<std::string> g_text;	/* string constant storage */

/* a column name node the optimizer annotated with its histogram (query_graph.c qo_get_attr_info: a
 * VARBIT DB_VALUE copy of the blob); `bytes` empty = no histogram (annotation NULL) */
static PT_NODE *
column (const char *name, const std::string &bytes)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = PT_NAME;
  n->info.name.original = name;
  n->info.name.resolved = "t";
  if (!bytes.empty ())
    {
      g_bytes.push_back (bytes);
      DB_VALUE *v = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
      /* db_make_varbit (v, len * 8, buf, len * 8), field by field (dbtype_function.i db_make_db_char) */
      v->domain.general_info.type = DB_TYPE_VARBIT;
      v->domain.general_info.is_null = 0;
      v->data.ch.info.style = MEDIUM_STRING;
      v->data.ch.medium.codeset = INTL_CODESET_RAW_BITS;
      v->data.ch.medium.size = (int) g_bytes.back ().size () * 8;
      v->data.ch.medium.buf = g_bytes.back ().data ();
      v->data.ch.medium.compressed_buf = NULL;
      v->data.ch.medium.compressed_size = DB_NOT_YET_COMPRESSED;
      n->info.name.histogram = v;
    }
  return n;
}

static DB_VALUE
ival (int x)
{
  DB_VALUE v;
  memset (&v, 0, sizeof (v));
  db_make_int (&v, x);
  return v;
}

static DB_VALUE
bival (DB_BIGINT x)
{
  DB_VALUE v;
  memset (&v, 0, sizeof (v));
  db_make_bigint (&v, x);
  return v;
}

static DB_VALUE
dval (double x)
{
  DB_VALUE v;
  memset (&v, 0, sizeof (v));
  db_make_double (&v, x);
  return v;
}

static DB_VALUE
nullv ()
{
  DB_VALUE v;
  memset (&v, 0, sizeof (v));
  db_make_null (&v);
  return v;
}

/* a VARCHAR constant: db_value_domain_init + db_make_db_char, field by field */
static DB_VALUE
sval (const char *s)
{
  DB_VALUE v;
  g_text.push_back (s);
  memset (&v, 0, sizeof (v));
  v.domain.general_info.type = DB_TYPE_STRING;
  v.domain.char_info.length = 10;
  v.domain.char_info.collation_id = LANG_COLL_ISO_BINARY;
  v.domain.general_info.is_null = 0;
  v.data.ch.info.style = MEDIUM_STRING;
  v.data.ch.medium.codeset = INTL_CODESET_ISO88591;
  v.data.ch.medium.size = (int) g_text.back ().size ();
  v.data.ch.medium.buf = g_text.back ().data ();
  v.data.ch.medium.compressed_buf = NULL;
  v.data.ch.medium.compressed_size = DB_NOT_YET_COMPRESSED;
  v.data.ch.medium.length = -1;
  return v;
}

/* K: INT, 1000 rows, 20% NULL, MCVs 1 (30%), 2 (10%), 5 (5%), 70 more distinct values in two buckets */
static blob::spec
spec_k ()
{
  blob::spec s;
  s.type = DB_TYPE_INTEGER;
  s.total_rows = 1000;
  s.null_freq = 0.2;
  s.mcvs = { { 1, 0.30 }, { 2, 0.10 }, { 5, 0.05 } };
  s.buckets = { std::make_tuple (10.0, 200, 20), std::make_tuple (100.0, 350, 50) };
  return s;
}

/* M: INT, 500 rows, no NULL, MCVs 1 (60%) and 2 (40%), nothing else */
static blob::spec
spec_m ()
{
  blob::spec s;
  s.type = DB_TYPE_INTEGER;
  s.total_rows = 500;
  s.mcvs = { { 1, 0.6 }, { 2, 0.4 } };
  return s;
}

/* D: DOUBLE, 400 rows, 10% NULL, MCVs 1.5 (20%) and 2.0 (30%), 10 more distinct values */
static blob::spec
spec_d ()
{
  blob::spec s;
  s.type = DB_TYPE_DOUBLE;
  s.total_rows = 400;
  s.null_freq = 0.1;
  s.mcvs = { { 1.5, 0.2 }, { 2.0, 0.3 } };
  s.buckets = { std::make_tuple (10.0, 100, 10) };
  return s;
}

/* E: INT, 100 rows, every one 7 */
static blob::spec
spec_e ()
{
  blob::spec s;
  s.type = DB_TYPE_INTEGER;
  s.total_rows = 100;
  s.mcvs = { { 7, 1.0 } };
  return s;
}

/* histogram_get_equal_selectivity (lhs, v), with a sentinel to see whether it is written */
struct probe
{
  bool ok;
  double sel;
};

static probe
eq (PT_NODE *lhs, DB_VALUE *v)
{
  probe p = { true, -42.0 };
  histogram_get_equal_selectivity (lhs, v, &p.sel, &p.ok);
  return p;
}

static std::vector<DB_VALUE *>
ptrs (std::vector<DB_VALUE> &vals)
{
  std::vector<DB_VALUE *> out;
  for (DB_VALUE &v : vals)
    {
      out.push_back (&v);
    }
  return out;
}

/* ================================================================ 1. one constant (head and base) */

TEST_CASE ("PR8242 eq: an MCV is priced at its frequency over the non-null rows, any other value at the residual "
	   "mass over the non-MCV distinct values", "[contract][eq]")
{
  const blob::spec s = spec_k ();
  PT_NODE *k = column ("k", blob::make (s));

  for (int x : { 1, 2, 5, 42, -7, 100, 1000 })
    {
      INFO ("k = " << x);
      DB_VALUE v = ival (x);
      probe p = eq (k, &v);
      CHECK (p.ok);
      CHECK (p.sel == Approx (blob::oracle (s, x)).epsilon (1e-12));
    }
  /* the numbers: 0.30 / 0.8, and (1 - 0.45 - 0.2) / 70 / 0.8 */
  DB_VALUE one = ival (1), other = ival (42);
  CHECK (eq (k, &one).sel == Approx (0.375).epsilon (1e-12));
  CHECK (eq (k, &other).sel == Approx (0.00625).epsilon (1e-12));
  DB_VALUE big = bival (2);
  CHECK (eq (k, &big).sel == Approx (blob::oracle (s, 2)).epsilon (1e-12));	/* BIGINT keys like INT */
}

TEST_CASE ("PR8242 eq boundary: no residual mass and no non-MCV value gives 1/total_rows; numeric constants cross "
	   "INT/DOUBLE only when exact; a string, a NULL, a fraction against INT, no histogram or a corrupt blob fail "
	   "and leave the estimate unwritten", "[contract][eq]")
{
  const blob::spec sm = spec_m (), sd = spec_d (), sk = spec_k ();
  PT_NODE *m = column ("m", blob::make (sm));
  PT_NODE *d = column ("d", blob::make (sd));
  PT_NODE *k = column ("k", blob::make (sk));
  PT_NODE *none = column ("none", "");
  std::string corrupt = blob::make (sk);
  corrupt[blob::TOTAL_SIZE] ^= 1;	/* total size disagrees with the buffer: HistogramReader::reset fails */
  PT_NODE *z = column ("z", corrupt);

  DB_VALUE three = ival (3), one = ival (1);
  CHECK (eq (m, &three).sel == Approx (1.0 / 500).epsilon (1e-12));
  CHECK (eq (m, &one).sel == Approx (0.6).epsilon (1e-12));

  DB_VALUE i2 = ival (2), d15 = dval (1.5), d20 = dval (2.0), d25 = dval (2.5);
  CHECK (eq (d, &i2).sel == Approx (0.3 / 0.9).epsilon (1e-12));	/* INT 2 against DOUBLE: promoted */
  CHECK (eq (d, &d15).sel == Approx (0.2 / 0.9).epsilon (1e-12));
  CHECK (eq (k, &d20).sel == Approx (blob::oracle (sk, 2)).epsilon (1e-12));	/* 2.0 against INT: exact */

  DB_VALUE str = sval ("2"), nul = nullv ();
  for (std::pair<PT_NODE *, DB_VALUE *> c : std::vector<std::pair<PT_NODE *, DB_VALUE *>>
       { { k, &d25 }, { k, &str }, { k, &nul }, { k, nullptr }, { none, &one }, { z, &one }, { nullptr, &one } })
    {
      probe p = eq (c.first, c.second);
      CHECK_FALSE (p.ok);
      CHECK (p.sel == -42.0);
    }
}

/* ================================================================ 2. the sum (new) */

TEST_CASE ("PR8242 sum: equals the per-constant estimates added in order -- MCVs, other values, a repeated key, "
	   "INT and BIGINT", "[contract][sum]")
{
  REQUIRE_SUM_PRESENT ();
  const blob::spec s = spec_k ();
  PT_NODE *k = column ("k", blob::make (s));
  std::vector<DB_VALUE> vals = { ival (1), ival (42), ival (2), ival (5), ival (2), bival (-7), dval (5.0) };
  std::vector<DB_VALUE *> v = ptrs (vals);

  double each = 0.0, expect = 0.0;
  for (DB_VALUE *x : v)
    {
      probe p = eq (k, x);
      REQUIRE (p.ok);
      each += p.sel;
    }
  for (double x : { 1, 42, 2, 5, 2, -7, 5 })
    {
      expect += blob::oracle (s, x);
    }

  double sum = -1.0;
  bool ok = false;
  histogram_get_equal_selectivity_sum (k, v.data (), (int) v.size (), &sum, &ok);
  CHECK (ok);
  CHECK (sum == each);			/* the same estimates, the same order: bit for bit */
  CHECK (sum == Approx (expect).epsilon (1e-12));
}

TEST_CASE ("PR8242 sum boundary: NULL entries and NULL values add nothing; no constant sums to 0; no usable "
	   "histogram fails with 0 whatever the inputs", "[contract][sum]")
{
  REQUIRE_SUM_PRESENT ();
  const blob::spec s = spec_k ();
  PT_NODE *k = column ("k", blob::make (s));
  PT_NODE *none = column ("none", "");
  std::vector<DB_VALUE> vals = { nullv (), ival (1), nullv (), ival (5) };
  std::vector<DB_VALUE *> v = { nullptr, &vals[0], &vals[1], &vals[2], nullptr, &vals[3] };

  double sum = -1.0;
  bool ok = false;
  histogram_get_equal_selectivity_sum (k, v.data (), (int) v.size (), &sum, &ok);
  CHECK (ok);
  CHECK (sum == Approx (blob::oracle (s, 1) + blob::oracle (s, 5)).epsilon (1e-12));

  std::vector<DB_VALUE *> nulls = { nullptr, &vals[0], &vals[2] };
  sum = -1.0;
  ok = false;
  histogram_get_equal_selectivity_sum (k, nulls.data (), (int) nulls.size (), &sum, &ok);
  CHECK (ok);
  CHECK (sum == 0.0);

  sum = -1.0;
  ok = false;
  histogram_get_equal_selectivity_sum (k, v.data (), 0, &sum, &ok);
  CHECK (ok);
  CHECK (sum == 0.0);

  sum = 99.0;
  ok = true;
  histogram_get_equal_selectivity_sum (none, v.data (), (int) v.size (), &sum, &ok);
  CHECK_FALSE (ok);
  CHECK (sum == 0.0);
  sum = 99.0;
  ok = true;
  histogram_get_equal_selectivity_sum (none, v.data (), 0, &sum, &ok);
  CHECK_FALSE (ok);
  CHECK (sum == 0.0);
}

TEST_CASE ("PR8242 sum boundary: one constant the histogram cannot price fails the whole sum, wherever it is",
	   "[contract][sum]")
{
  REQUIRE_SUM_PRESENT ();
  PT_NODE *k = column ("k", blob::make (spec_k ()));

  for (int at = 0; at < 3; at++)
    {
      for (int bad = 0; bad < 2; bad++)
	{
	  INFO ("unpriceable " << (bad ? "string" : "2.5") << " at " << at);
	  std::vector<DB_VALUE> vals = { ival (1), ival (2), ival (5) };
	  vals[at] = bad ? sval ("2") : dval (2.5);
	  std::vector<DB_VALUE *> v = ptrs (vals);
	  double sum = -1.0;
	  bool ok = true;
	  histogram_get_equal_selectivity_sum (k, v.data (), (int) v.size (), &sum, &ok);
	  CHECK_FALSE (ok);
	}
    }
}

TEST_CASE ("PR8242 sum: the column's histogram is opened once per call whatever the number of constants (the "
	   "per-constant probe opens it for each)", "[contract][sum]")
{
  REQUIRE_SUM_PRESENT ();
  PT_NODE *k = column ("k", blob::make (spec_k ()));
  std::vector<DB_VALUE> vals;
  for (int i = 0; i < 200; i++)
    {
      vals.push_back (ival (i % 7));
    }
  std::vector<DB_VALUE *> v = ptrs (vals);
  double sum = 0.0;
  bool ok = false;

  g_opens = 0;
  histogram_get_equal_selectivity_sum (k, v.data (), (int) v.size (), &sum, &ok);
  CHECK (ok);
  CHECK (g_opens == 1);

  g_opens = 0;
  histogram_get_equal_selectivity_sum (k, v.data (), 0, &sum, &ok);
  CHECK (g_opens == 1);

  g_opens = 0;
  for (DB_VALUE *x : v)
    {
      (void) eq (k, x);
    }
  CHECK (g_opens == 200);
}

/* ================================================================ 3. snapshot join selectivity (new) */

#if UNITSEAM_HAVE_ROW_SNAPSHOT

#define REQUIRE_JOIN_PRESENT() \
  do { if (&row_snapshot_join_selectivity == nullptr) \
	 FAIL ("row_snapshot_join_selectivity not linked (row_snapshot_cl.cpp absent at this revision)"); } while (0)

/* k rows of (id, key): the key in column 1, so the row-major index is exercised */
struct snap
{
  ROW_SNAPSHOT s;
  std::vector<DB_VALUE> cells;
  int ids[2] = { 1, 2 };

  explicit snap (const std::vector<DB_VALUE> &keys)
  {
    for (std::size_t r = 0; r < keys.size (); r++)
      {
	cells.push_back (ival ((int) r + 100));
	cells.push_back (keys[r]);
      }
    s.n_attrs = 2;
    s.n_rows = (int) keys.size ();
    s.attr_ids = ids;
    s.values = cells.empty () ? NULL : cells.data ();
  }
};

struct join_result
{
  bool ok;
  double sel;
};

static join_result
join_sel (const ROW_SNAPSHOT *s, int column, PT_NODE *other, double nf)
{
  join_result r = { false, -42.0 };
  r.ok = row_snapshot_join_selectivity (s, column, other, nf, &r.sel);
  return r;
}

TEST_CASE ("PR8242 join: (1/k) * SUM P (other = key) * (1 - nf_other), k = the rows read", "[contract][join]")
{
  REQUIRE_JOIN_PRESENT ();
  const blob::spec s = spec_k ();
  PT_NODE *other = column ("k", blob::make (s));
  snap rows ({ ival (1), ival (2), ival (42) });

  join_result r = join_sel (&rows.s, 1, other, 0.2);
  CHECK (r.ok);
  double sum = blob::oracle (s, 1) + blob::oracle (s, 2) + blob::oracle (s, 42);
  CHECK (r.sel == Approx (sum / 3 * (1.0 - 0.2)).epsilon (1e-12));

  /* the same through the per-constant probe: 0.375, 0.125, 0.00625 -> 0.506.. / 3 * 0.8 */
  CHECK (r.sel == Approx ((0.375 + 0.125 + 0.00625) / 3 * 0.8).epsilon (1e-12));
}

TEST_CASE ("PR8242 join boundary: a NULL key stays in k and adds 0; keys that are all NULL give no estimate "
	   "(false, selectivity unwritten)", "[contract][join]")
{
  REQUIRE_JOIN_PRESENT ();
  const blob::spec s = spec_k ();
  PT_NODE *other = column ("k", blob::make (s));

  snap mixed ({ ival (1), nullv (), nullv (), ival (5) });
  join_result r = join_sel (&mixed.s, 1, other, 0.2);
  CHECK (r.ok);
  CHECK (r.sel == Approx ((blob::oracle (s, 1) + blob::oracle (s, 5)) / 4 * 0.8).epsilon (1e-12));

  snap nulls ({ nullv (), nullv () });
  r = join_sel (&nulls.s, 1, other, 0.2);
  CHECK_FALSE (r.ok);
  CHECK (r.sel == -42.0);
}

TEST_CASE ("PR8242 join boundary: nf_other in [0, 1) scales by (1 - nf), a negative (unknown) nf applies no factor, "
	   "a key every row of the other side carries reaches exactly 1.0 -- and nf_other == 1.0 applies no factor "
	   "(current behaviour)", "[contract][join]")
{
  REQUIRE_JOIN_PRESENT ();
  const blob::spec s = spec_k ();
  PT_NODE *other = column ("k", blob::make (s));
  snap rows ({ ival (2), ival (5) });
  const double base = (blob::oracle (s, 2) + blob::oracle (s, 5)) / 2;

  CHECK (join_sel (&rows.s, 1, other, 0.0).sel == Approx (base).epsilon (1e-12));
  CHECK (join_sel (&rows.s, 1, other, 0.25).sel == Approx (base * 0.75).epsilon (1e-12));
  CHECK (join_sel (&rows.s, 1, other, -1.0).sel == Approx (base).epsilon (1e-12));
  /* (current behaviour) an all-NULL other column would join nothing; the factor is skipped at 1.0,
   * as PR#8029's qo_snapshot_apply skipped it (unit query_graph.join_snapshot_selectivity) */
  join_result all_null = join_sel (&rows.s, 1, other, 1.0);
  CHECK (all_null.ok);
  CHECK (all_null.sel == Approx (base).epsilon (1e-12));

  PT_NODE *e = column ("e", blob::make (spec_e ()));
  snap sevens ({ ival (7), ival (7), ival (7) });
  join_result one = join_sel (&sevens.s, 1, e, 0.0);
  CHECK (one.ok);
  CHECK (one.sel == 1.0);
}

TEST_CASE ("PR8242 join boundary: no estimate (false, selectivity unwritten) for no histogram on the other side, a "
	   "key it cannot price, no rows, a column not read, or no snapshot", "[contract][join]")
{
  REQUIRE_JOIN_PRESENT ();
  PT_NODE *other = column ("k", blob::make (spec_k ()));
  PT_NODE *none = column ("none", "");
  snap rows ({ ival (1), ival (2) });
  snap strings ({ ival (1), sval ("2") });
  snap empty ({});

  join_result r = join_sel (&rows.s, 1, none, 0.0);
  CHECK_FALSE (r.ok);
  CHECK (r.sel == -42.0);
  r = join_sel (&strings.s, 1, other, 0.0);
  CHECK_FALSE (r.ok);
  CHECK (r.sel == -42.0);
  r = join_sel (&empty.s, 1, other, 0.0);
  CHECK_FALSE (r.ok);
  r = join_sel (&rows.s, -1, other, 0.0);
  CHECK_FALSE (r.ok);
  r = join_sel (NULL, 1, other, 0.0);
  CHECK_FALSE (r.ok);
  CHECK (r.sel == -42.0);
}

#else /* !UNITSEAM_HAVE_ROW_SNAPSHOT */

TEST_CASE ("PR8242 join: (1/k) * SUM P (other = key) * (1 - nf_other), k = the rows read", "[contract][join]")
{
  FAIL ("row_snapshot_cl.hpp absent at this revision (new in PR#8242)");
}

TEST_CASE ("PR8242 join boundary: a NULL key stays in k and adds 0; keys that are all NULL give no estimate "
	   "(false, selectivity unwritten)", "[contract][join]")
{
  FAIL ("row_snapshot_cl.hpp absent at this revision (new in PR#8242)");
}

TEST_CASE ("PR8242 join boundary: nf_other in [0, 1) scales by (1 - nf), a negative (unknown) nf applies no factor, "
	   "a key every row of the other side carries reaches exactly 1.0 -- and nf_other == 1.0 applies no factor "
	   "(current behaviour)", "[contract][join]")
{
  FAIL ("row_snapshot_cl.hpp absent at this revision (new in PR#8242)");
}

TEST_CASE ("PR8242 join boundary: no estimate (false, selectivity unwritten) for no histogram on the other side, a "
	   "key it cannot price, no rows, a column not read, or no snapshot", "[contract][join]")
{
  FAIL ("row_snapshot_cl.hpp absent at this revision (new in PR#8242)");
}

#endif
