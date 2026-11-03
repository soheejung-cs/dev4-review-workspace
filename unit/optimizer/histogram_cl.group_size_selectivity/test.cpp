/*
 * PR#8325 (CBRD-27811) -- what the PR changed in src/optimizer/histogram/histogram_cl.cpp (cs mode):
 *
 *   histogram_get_group_size_selectivity (attr, min_rows, scale)   new: the share of the GROUP BY column's values
 *       whose group holds min_rows rows or more -- an MCV by its count (frequency x rows x scale, rounded), a
 *       bucket's values geometric with the bucket's average count (x scale); over MCVs + bucket distinct values
 *   histogram_split_hv_predicate / histogram_hv_operand_column     a host-variable comparison now also takes a
 *       SUM / AVG / MIN / MAX of one column as its column (the HAVING forms qo_having_compare () prices)
 *   bind_target_walk                                               an aggregate equal to a value pins nothing
 *   bind_term_walk (unchanged, reached by the above)               the slot of `AGG (x) op ?` prices `x op ?`
 *
 * Real code: the estimators (histogram_get_group_size_selectivity, histogram_get_comp_selectivity,
 * histogram_get_total_rows, histogram_extract_key, comp_parts), HistogramReader (histogram_reader.cpp compiled into
 * this test, byte_order.c behind OR_GET_*), histogram_split_hv_predicate, histogram_hv_operand_column,
 * histogram_is_user_host_var, bind_target_walk / bind_target_add, bind_term_walk / bind_term_add /
 * bind_card_arg_value / bind_term_key (hooks.cpp).
 * Seam: histogram_init_reader_from_lhs (histogram_cl.cpp:724, TU static) -- its body minus the class-cache branch,
 * which runs only inside a bind fingerprint walk of a whole statement (bind_fp_active_statement != NULL; the hooks
 * walk one node, so it stays NULL): the end of a path, a PT_NAME, the name's histogram annotation, db_get_bit,
 * HistogramReader::reset.
 * Fakes (other TUs): pt_get_end_path_node and pt_is_derived_host_var (parser_support.c bodies), qo_classify
 * (query_planner.c body, with pt_is_function_index_expression's), or_packed_value_size / or_pack_value (a value
 * image: only "equal values, equal image" matters for bind_term_key, and no memo is used here).
 *
 * Tags: [contract] expected behaviour; `develop` in a name = holds at the base too. [defect] PASS = present.
 * Expected numbers come from an oracle over the blob spec written from the function's comment, not from its code.
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
#include "query_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------- the function PR#8325 added (weak) */

void histogram_get_group_size_selectivity (PT_NODE *attr, double min_rows, double scale, double *selectivity,
    bool *success) __attribute__ ((weak));

#define REQUIRE_GROUP_SIZE_PRESENT() \
  do { if (&histogram_get_group_size_selectivity == nullptr) \
	 FAIL ("histogram_get_group_size_selectivity absent at this revision (new in PR#8325)"); } while (0)

/* hooks.cpp */
bool unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
				  bool *out_reversed);
int unitseam_bind_target (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **name, bool *equality);
int unitseam_bind_term_rows (PARSER_CONTEXT *parser, PT_NODE *node, double *rows);

/* ---------------------------------------------------------------- seam */

static int g_opens;

/* histogram_cl.cpp:724 histogram_init_reader_from_lhs, without the class-cache branch (see the header) */
bool
histogram_init_reader_from_lhs (PT_NODE *lhs, hist::HistogramReader &reader)
{
  g_opens++;
  while (lhs != NULL && lhs->node_type == PT_DOT_)
    {
      lhs = lhs->info.dot.arg2;
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

/* ---------------------------------------------------------------- fakes of other TUs */

/* parser_support.c pt_get_end_path_node () body */
PT_NODE *
pt_get_end_path_node (PT_NODE *node)
{
  while (node != NULL && node->node_type == PT_DOT_)
    {
      node = node->info.dot.arg2;
    }
  return node;
}

/* parser_support.c pt_is_derived_host_var () body */
bool
pt_is_derived_host_var (const PARSER_CONTEXT *parser, int index)
{
  return parser->host_var_derived_from != NULL && index >= parser->host_var_count
	 && index < parser->host_var_count + parser->auto_param_count && parser->host_var_derived_from[index] >= 0;
}

/* query_planner.c qo_classify () body (pt_is_function_index_expression inlined: a PT_EXPR flagged
 * PT_EXPR_INFO_FUNCTION_INDEX) */
PRED_CLASS
qo_classify (PT_NODE *attr)
{
  switch (attr->node_type)
    {
    case PT_NAME:
    case PT_DOT_:
      return PC_ATTR;
    case PT_VALUE:
      if (PT_IS_SET_TYPE (attr))
	{
	  return PC_SET;
	}
      return (attr->type_enum == PT_TYPE_NULL) ? PC_OTHER : PC_CONST;
    case PT_HOST_VAR:
      return PC_HOST_VAR;
    case PT_SELECT:
    case PT_UNION:
    case PT_INTERSECTION:
    case PT_DIFFERENCE:
      return PC_SUBQUERY;
    case PT_FUNCTION:
      if (PT_IS_SET_TYPE (attr))
	{
	  for (PT_NODE *a = attr->info.function.arg_list; a; a = a->next)
	    {
	      if (a->node_type == PT_HOST_VAR)
		{
		  return PC_SET;
		}
	      else if (a->node_type != PT_NAME)
		{
		  return PC_OTHER;
		}
	    }
	  return PC_MULTI_ATTR;
	}
      return PC_OTHER;
    case PT_EXPR:
      if (PT_EXPR_INFO_IS_FLAGED (attr, PT_EXPR_INFO_FUNCTION_INDEX))
	{
	  return PC_ATTR;
	}
      return PC_OTHER;
    default:
      return PC_OTHER;
    }
}

/* object_representation.c: a value image for bind_term_key -- the type and the 8 value bytes */
int
or_packed_value_size (const DB_VALUE *value, int collapse_null, int include_domain, int include_domain_classoids)
{
  return 16;
}

char *
or_pack_value (char *buf, DB_VALUE *value)
{
  std::int64_t t = (std::int64_t) DB_VALUE_DOMAIN_TYPE (value);
  memcpy (buf, &t, 8);
  memcpy (buf + 8, &value->data, 8);
  return buf + 16;
}

/* ---------------------------------------------------------------- HST2 blobs */

/* written with the OR_* macros the builder uses (histogram_builder.cpp write<> / write_value_slot / build):
 * header 48 B, MCV records (8 B value slot + f64 freq, ascending), bucket records (8 B hi + i64 cumulative +
 * i64 approx_ndv). Numeric columns only: no string blob. (Same writer as suite histogram_cl.equal_selectivity_sum.) */
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

  /* the function's comment, over the spec: a group is the rows of one value; an MCV knows its count
   * (frequency over all rows x rows x scale, an integer up to rounding); a bucket knows only the average count of
   * its values (its rows x scale / its distinct values), the counts geometric with that average:
   * P (count >= k) = r^(k - 1), r = 1 - 1 / max (average, 1), k the least whole count >= min_rows. The share of
   * the distinct values (MCVs + bucket distinct values) whose group has min_rows rows or more. */
  static double
  group_oracle (const spec &s, double min_rows, double scale)
  {
    double groups = 0.0, distinct = (double) s.mcvs.size ();
    std::int64_t prev = 0;

    for (const auto &m : s.mcvs)
      {
	if (std::floor (m.second * (double) s.total_rows * scale + 0.5) >= min_rows)
	  {
	    groups += 1.0;
	  }
      }
    for (const auto &bk : s.buckets)
      {
	const double ndv = (double) std::get<2> (bk), rows = (double) (std::get<1> (bk) - prev);
	prev = std::get<1> (bk);
	distinct += ndv;
	if (ndv <= 0.0)
	  {
	    continue;
	  }
	const double r = 1.0 - 1.0 / std::max (rows * scale / ndv, 1.0);
	const double k = std::max (std::ceil (min_rows), 1.0);
	groups += ndv * std::pow (r, k - 1.0);
      }
    return groups / distinct;
  }
}

/* ---------------------------------------------------------------- nodes */

static std::deque<std::string> g_bytes;

static PT_NODE *
node (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

/* a column name node the optimizer annotated with its histogram (query_graph.c qo_get_attr_info: a VARBIT DB_VALUE
 * copy of the blob); `bytes` empty = no histogram */
static PT_NODE *
column (const char *name, const std::string &bytes = "")
{
  PT_NODE *n = node (PT_NAME);
  n->info.name.original = name;
  n->info.name.resolved = "t";
  n->info.name.spec_id = 0x1001;
  n->info.name.meta_class = PT_NORMAL;
  if (!bytes.empty ())
    {
      g_bytes.push_back (bytes);
      DB_VALUE *v = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
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

static PT_NODE *
dot (PT_NODE *spec_name, PT_NODE *name)
{
  PT_NODE *d = node (PT_DOT_);
  d->info.dot.arg1 = spec_name;
  d->info.dot.arg2 = name;
  return d;
}

static PT_NODE *
agg (FUNC_CODE f, PT_NODE *arg, PT_MISC_TYPE all_or_distinct = PT_ALL)
{
  PT_NODE *n = node (PT_FUNCTION);
  n->info.function.function_type = f;
  n->info.function.arg_list = arg;
  n->info.function.all_or_distinct = all_or_distinct;
  return n;
}

static PT_NODE *
hv (int index)
{
  PT_NODE *n = node (PT_HOST_VAR);
  n->info.host_var.index = index;
  n->info.host_var.var_type = PT_HOST_IN;
  return n;
}

static PT_NODE *
ival (int v)
{
  PT_NODE *n = node (PT_VALUE);
  n->type_enum = PT_TYPE_INTEGER;
  db_make_int (&n->info.value.db_value, v);
  n->info.value.db_value_is_initialized = true;
  return n;
}

static PT_NODE *
ex (PT_OP_TYPE op, PT_NODE *a1, PT_NODE *a2 = NULL)
{
  PT_NODE *e = node (PT_EXPR);
  e->info.expr.op = op;
  e->info.expr.arg1 = a1;
  e->info.expr.arg2 = a2;
  return e;
}

/* a parser with two user host variables (0, 1); 2 and 3 lie past host_var_count with no derivation table (an
 * auto-parameterized literal is not the user's) */
struct parser_scene
{
  PARSER_CONTEXT *parser;

  parser_scene ()
  {
    parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    parser->host_variables = (DB_VALUE *) calloc (4, sizeof (DB_VALUE));
    for (int i = 0; i < 4; i++)
      {
	db_make_null (&parser->host_variables[i]);
      }
    parser->host_var_count = 2;
    parser->auto_param_count = 2;
  }
};

/* ---------------------------------------------------------------- specs */

/* A: INT GROUP BY column, 10000 rows, 100 values -- MCVs 1 (2000 rows), 2 (1000), 5 (500); buckets of 40 values over
 * 3000 rows (average 75) and 57 values over 3500 rows (average 61.4) */
static blob::spec
spec_a ()
{
  blob::spec s;
  s.total_rows = 10000;
  s.mcvs = { { 1, 0.2 }, { 2, 0.1 }, { 5, 0.05 } };
  s.buckets = { std::make_tuple (100.0, 3000, 40), std::make_tuple (1000.0, 6500, 57) };
  return s;
}

/* X: INT column of 1000 rows spread over [1, 100]: ten buckets of 100 rows and 10 values */
static blob::spec
spec_x ()
{
  blob::spec s;
  s.total_rows = 1000;
  for (int b = 1; b <= 10; b++)
    {
      s.buckets.push_back (std::make_tuple (10.0 * b, (std::int64_t) 100 * b, (std::int64_t) 10));
    }
  return s;
}

struct gs_probe
{
  bool ok;
  double sel;
};

static gs_probe
gs (PT_NODE *attr, double min_rows, double scale)
{
  gs_probe p = { true, -42.0 };
  histogram_get_group_size_selectivity (attr, min_rows, scale, &p.sel, &p.ok);
  return p;
}

/* ================================================================ G: histogram_get_group_size_selectivity () */

TEST_CASE ("PR8325 G1: the share of values whose group holds min_rows rows -- an MCV by its count, a bucket's values "
	   "geometric with the bucket's average count", "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  const blob::spec s = spec_a ();
  PT_NODE *a = column ("a", blob::make (s));

  for (double k : { 2.0, 10.0, 50.0, 75.0, 100.0, 501.0, 600.0, 1000.0, 1001.0, 2000.0, 2001.0 })
    {
      INFO ("min_rows = " << k);
      gs_probe p = gs (a, k, 1.0);
      CHECK (p.ok);
      CHECK (p.sel == Approx (blob::group_oracle (s, k, 1.0)).epsilon (1e-12));
    }
  /* the numbers: at 600 two MCVs (2000, 1000) and 40 r1^599 + 57 r2^599 of the bucket values; past 2000 none of
   * the MCVs and almost no bucket value */
  CHECK (gs (a, 600, 1.0).sel
	 == Approx ((2 + 40 * pow (1 - 1 / 75.0, 599) + 57 * pow (1 - 1 / (3500 / 57.0), 599)) / 100).epsilon (1e-9));
  CHECK (gs (a, 2001, 1.0).sel < 1e-9);
  /* MCV 1 holds exactly 2000 rows: counted at 2000, not at 2001 */
  CHECK ((gs (a, 2000, 1.0).sel - gs (a, 2001, 1.0).sel) == Approx (0.01).epsilon (1e-6));
}

TEST_CASE ("PR8325 G2: min_rows of 1 or less keeps every value; a fractional min_rows is the next whole count for the "
	   "buckets and the MCV counts are compared with it as is", "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  const blob::spec s = spec_a ();
  PT_NODE *a = column ("a", blob::make (s));

  CHECK (gs (a, 1.0, 1.0).sel == 1.0);
  CHECK (gs (a, 0.5, 1.0).sel == 1.0);
  CHECK (gs (a, 0.0, 1.0).sel == 1.0);
  CHECK (gs (a, 600.2, 1.0).sel == Approx (blob::group_oracle (s, 600.2, 1.0)).epsilon (1e-12));
  CHECK (gs (a, 600.2, 1.0).sel == Approx (gs (a, 601, 1.0).sel).epsilon (1e-12));
}

TEST_CASE ("PR8325 G3: an MCV's count is its frequency times the rows rounded -- 599.6 rows count for 600, 599.4 do "
	   "not", "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  blob::spec s;
  s.total_rows = 10000;
  s.mcvs = { { 1, 0.05996 }, { 2, 0.05994 } };
  PT_NODE *a = column ("a", blob::make (s));

  CHECK (gs (a, 600, 1.0).sel == Approx (0.5));
  CHECK (gs (a, 599, 1.0).sel == 1.0);
  CHECK (gs (a, 601, 1.0).sel == 0.0);
}

TEST_CASE ("PR8325 G4: the scale shrinks or grows every count -- the MCV counts and the bucket averages",
	   "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  const blob::spec s = spec_a ();
  PT_NODE *a = column ("a", blob::make (s));

  for (double scale : { 0.1, 0.3, 0.5, 2.0 })
    {
      for (double k : { 2.0, 30.0, 151.0, 600.0, 1001.0 })
	{
	  INFO ("scale = " << scale << ", min_rows = " << k);
	  CHECK (gs (a, k, scale).sel == Approx (blob::group_oracle (s, k, scale)).epsilon (1e-12));
	}
    }
  /* half the counts: 1000, 500, 250 -- only MCV 1 reaches 600 */
  CHECK (gs (a, 600, 0.5).sel == Approx (0.01).epsilon (1e-3));
  /* MCV 5 at 0.3: 150 rows -- counted at 150, not at 151 */
  CHECK (gs (a, 150, 0.3).sel - gs (a, 151, 0.3).sel >= 0.01 - 1e-9);
}

TEST_CASE ("PR8325 G5: NULL rows are no value of the histogram -- the same shares whatever the NULL frequency; a "
	   "bucket of no distinct value adds nothing, a bucket whose average is below one row keeps only min_rows <= 1",
	   "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  blob::spec s = spec_a (), n = spec_a ();
  n.null_freq = 0.3;		/* the same value counts in a table with 3000 NULL rows more would read 13000 rows */
  PT_NODE *a = column ("a", blob::make (s));
  PT_NODE *an = column ("an", blob::make (n));
  CHECK (gs (an, 600, 1.0).sel == Approx (gs (a, 600, 1.0).sel).epsilon (1e-12));

  blob::spec z = spec_a ();
  z.buckets.push_back (std::make_tuple (2000.0, 6500, 0));
  PT_NODE *az = column ("az", blob::make (z));
  CHECK (gs (az, 600, 1.0).sel == Approx (gs (a, 600, 1.0).sel).epsilon (1e-12));

  blob::spec thin;
  thin.total_rows = 30;
  thin.buckets = { std::make_tuple (100.0, 10, 40) };	/* 40 values over 10 rows */
  PT_NODE *at = column ("at", blob::make (thin));
  CHECK (gs (at, 2, 1.0).sel == 0.0);
  CHECK (gs (at, 1, 1.0).sel == 1.0);
}

TEST_CASE ("PR8325 G6: not found -- no histogram, not a column, a blob the reader refuses, no rows, no distinct "
	   "value, a scale of 0 or less; the estimate is left unwritten; a path ends at its column",
	   "[contract][group_size]")
{
  REQUIRE_GROUP_SIZE_PRESENT ();
  const blob::spec s = spec_a ();
  std::string corrupt = blob::make (s);
  corrupt[blob::TOTAL_SIZE] ^= 1;
  blob::spec empty_rows = spec_a ();
  empty_rows.total_rows = 0;
  blob::spec nothing;
  nothing.total_rows = 100;

  for (PT_NODE *bad : { column ("none"), ival (3), column ("z", corrupt), column ("r", blob::make (empty_rows)),
			column ("e", blob::make (nothing)) })
    {
      gs_probe p = gs (bad, 600, 1.0);
      CHECK_FALSE (p.ok);
      CHECK (p.sel == -42.0);
    }
  PT_NODE *a = column ("a", blob::make (s));
  for (double scale : { 0.0, -1.0 })
    {
      gs_probe p = gs (a, 600, scale);
      CHECK_FALSE (p.ok);
      CHECK (p.sel == -42.0);
    }
  gs_probe p = gs (dot (column ("t"), a), 600, 1.0);
  CHECK (p.ok);
  CHECK (p.sel == Approx (blob::group_oracle (s, 600, 1.0)).epsilon (1e-12));
}

/* ================================================================ S: the host-variable predicate with an aggregate */

TEST_CASE ("PR8325 S1 develop: control -- `x op ?` and `? op x` are host-variable predicates on x (reversed for the "
	   "second), for =, >, >=, <, <=; not for <> or a constant", "[contract][split]")
{
  parser_scene ps;
  PT_NODE *x = column ("x");
  PT_NODE *name = NULL, *h = NULL;
  bool reversed = true;

  for (PT_OP_TYPE op : { PT_EQ, PT_GT, PT_GE, PT_LT, PT_LE })
    {
      PT_NODE *v = hv (0);
      CHECK (unitseam_split_hv_predicate (ps.parser, ex (op, x, v), &name, &h, &reversed));
      CHECK ((name == x && h == v && !reversed));
      CHECK (unitseam_split_hv_predicate (ps.parser, ex (op, v, x), &name, &h, &reversed));
      CHECK ((name == x && h == v && reversed));
    }
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_NE, x, hv (0)), &name, &h, &reversed));
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, x, ival (5)), &name, &h, &reversed));
  /* host variable 2: past the user's (an auto-parameterized literal), not derived */
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, x, hv (2)), &name, &h, &reversed));
  CHECK (name == NULL);
}

TEST_CASE ("PR8325 S2: SUM / AVG / MIN / MAX of one column compared with a host variable is a predicate on that "
	   "column, either way round; the column at the end of a path", "[contract][split]")
{
  parser_scene ps;
  PT_NODE *name = NULL, *h = NULL;
  bool reversed = true;

  for (FUNC_CODE f : { PT_SUM, PT_AVG, PT_MIN, PT_MAX })
    {
      PT_NODE *x = column ("x"), *v = hv (1);
      CHECK (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (f, x), v), &name, &h, &reversed));
      CHECK ((name == x && h == v && !reversed));
      CHECK (unitseam_split_hv_predicate (ps.parser, ex (PT_LE, v, agg (f, x)), &name, &h, &reversed));
      CHECK ((name == x && h == v && reversed));
      CHECK (unitseam_split_hv_predicate (ps.parser, ex (PT_EQ, agg (f, x), v), &name, &h, &reversed));
    }
  PT_NODE *x = column ("x");
  CHECK (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_SUM, dot (column ("t"), x)), hv (0)), &name, &h,
				     &reversed));
  CHECK (name == x);
}

TEST_CASE ("PR8325 S3: other aggregates and operands are not -- COUNT (*), COUNT (x), STDDEV (x), an expression "
	   "argument, two arguments, a constant; DISTINCT SUM is (current behaviour -- the HAVING estimate does not "
	   "price it)", "[contract][split]")
{
  parser_scene ps;
  PT_NODE *name = NULL, *h = NULL;
  bool reversed = false;

  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_COUNT_STAR, NULL), hv (0)), &name, &h,
					   &reversed));
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_COUNT, column ("x")), hv (0)), &name, &h,
					   &reversed));
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_STDDEV, column ("x")), hv (0)), &name, &h,
					   &reversed));
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_SUM, ex (PT_PLUS, column ("x"), ival (1))),
							     hv (0)), &name, &h, &reversed));
  PT_NODE *two = column ("x");
  two->next = column ("y");
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_MAX, two), hv (0)), &name, &h, &reversed));
  CHECK_FALSE (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_SUM, column ("x")), ival (500)), &name, &h,
					   &reversed));
  CHECK (name == NULL);
  CHECK (unitseam_split_hv_predicate (ps.parser, ex (PT_GT, agg (PT_SUM, column ("x"), PT_DISTINCT), hv (0)), &name,
				     &h, &reversed));
}

TEST_CASE ("PR8325 S4: a bind target from an aggregate pins nothing -- `x = ?` is an equality target, `MAX (x) = ?`, "
	   "`? = MIN (x)` and `SUM (x) > ?` add x without the equality", "[contract][bind_target]")
{
  parser_scene ps;
  PT_NODE *name = NULL;
  bool equality = false;
  PT_NODE *x = column ("x");

  CHECK (unitseam_bind_target (ps.parser, ex (PT_EQ, x, hv (0)), &name, &equality) == 1);
  CHECK ((name == x && equality));
  for (PT_NODE *pred : { ex (PT_EQ, agg (PT_MAX, x), hv (0)), ex (PT_EQ, hv (0), agg (PT_MIN, x)),
			 ex (PT_GT, agg (PT_SUM, x), hv (0)) })
    {
      equality = true;
      CHECK (unitseam_bind_target (ps.parser, pred, &name, &equality) == 1);
      CHECK ((name == x && !equality));
    }
  CHECK (unitseam_bind_target (ps.parser, ex (PT_EQ, agg (PT_COUNT_STAR, NULL), hv (0)), &name, &equality) == 0);
}

/* ================================================================ B: the bind fingerprint slot of `AGG (x) op ?` */

TEST_CASE ("PR8325 B1: the slot of `AVG (x) > ?` is the rows of x > ? -- it moves with the value the way the HAVING "
	   "estimate of AVG does", "[contract][bind_term]")
{
  parser_scene ps;
  const blob::spec s = spec_x ();
  PT_NODE *x = column ("x", blob::make (s));
  double rows50 = -1.0, rows90 = -1.0, sel = 0.0;
  bool ok = false;

  db_make_int (&ps.parser->host_variables[0], 50);
  REQUIRE (unitseam_bind_term_rows (ps.parser, ex (PT_GT, agg (PT_AVG, x), hv (0)), &rows50) == 1);
  histogram_get_comp_selectivity (x, &ps.parser->host_variables[0], true, false, &sel, &ok);
  REQUIRE (ok);
  CHECK (rows50 == Approx (std::max (1.0, sel * 1000)));
  db_make_int (&ps.parser->host_variables[0], 90);
  REQUIRE (unitseam_bind_term_rows (ps.parser, ex (PT_GT, agg (PT_AVG, x), hv (0)), &rows90) == 1);
  CHECK (rows50 / rows90 > 3.0);
}

TEST_CASE ("PR8325 U4: the slot of `SUM (x) > ?` is the rows of x > ? as well, but HAVING prices SUM (x) > v as "
	   "x > v / group size -- over groups of 10 the values 500 and 900 move the HAVING estimate 5x (x > 50, x > 90) "
	   "and leave the slot at 1 row, so the band check keeps the plan of the first value -- [defect] latent/low, "
	   "PASS = present", "[defect][bind_term]")
{
  parser_scene ps;
  const blob::spec s = spec_x ();
  PT_NODE *x = column ("x", blob::make (s));
  double rows500 = -1.0, rows900 = -1.0;

  db_make_int (&ps.parser->host_variables[0], 500);
  REQUIRE (unitseam_bind_term_rows (ps.parser, ex (PT_GT, agg (PT_SUM, x), hv (0)), &rows500) == 1);
  db_make_int (&ps.parser->host_variables[0], 900);
  REQUIRE (unitseam_bind_term_rows (ps.parser, ex (PT_GT, agg (PT_SUM, x), hv (0)), &rows900) == 1);
  CHECK (rows500 == 1.0);
  CHECK (rows900 == 1.0);

  /* what qo_having_compare () reads for those values over groups of 10 (SUM: the average row, v / 10) */
  DB_VALUE v50, v90;
  db_make_double (&v50, 50.0);
  db_make_double (&v90, 90.0);
  double s50 = 0.0, s90 = 0.0;
  bool ok50 = false, ok90 = false;
  histogram_get_comp_selectivity (x, &v50, true, false, &s50, &ok50);
  histogram_get_comp_selectivity (x, &v90, true, false, &s90, &ok90);
  REQUIRE ((ok50 && ok90));
  CHECK (s50 / s90 > 4.0);
}
