/*
 * PR#8006 (CBRD-27143) -- LIKE / NOT LIKE pattern host variables in the bind fingerprint
 * (src/optimizer/histogram/histogram_cl.cpp, cs mode).
 *
 *   histogram_split_hv_predicate (TU static, via hooks.cpp)  the candidate test both bind walks share:
 *       `col LIKE ?` / `col NOT LIKE ?` with a USER host variable as the pattern itself; not the
 *       ESCAPE form (arg2 = PT_LIKE_ESCAPE (?, c)), not the reversed `? LIKE col`.
 *   histogram_bind_fingerprint -> bind_fp_walk                the LIKE component: frexp exponent of the
 *       estimate (2x bands, edge at 2^k), NOT LIKE banded on 1 - LIKE, a zero estimate in its own band
 *       (Greptile P1 G1, 1d5858374).
 *   histogram_stmt_has_hv_predicate -> hv_pred_walk           the unpeeked flag: a candidate operator AND
 *       a histogram on the column (Greptile P1 G2, 1d5858374).
 *
 * The walks, the candidate test, the band rule and the hash mixing are the real code. Fakes (seams):
 *   histogram_get_like_selectivity   returns the LIKE estimate the test chose -- only the fingerprint
 *                                    arithmetic is under test, not the estimator
 *   histogram_get_equal/comp_selectivity  same, for the comparison controls
 *   histogram_blob_from_class_cache  the class-cache fallback of histogram_column_has_histogram
 * Each fake keeps the real function's "cannot price" exits (see suite.json fakes).
 *
 * PR#8013 rewrites these functions (bind_term_* walks): after it merges this suite needs updating.
 */
#include "catch.hpp"
#include "dbtype_def.h"
#include "parser.h"
#include "parse_tree.h"
#include "histogram_cl.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

/* ---------------------------------------------------------------- fakes */

static PARSER_CONTEXT *g_parser;	/* the scene's parser: maps a bound DB_VALUE back to its index */
static DB_VALUE g_blob;		/* stands for a histogram blob; only its address is used */
static std::set<std::string> g_cache;	/* columns whose class cache holds a histogram */
static std::map<int, double> g_sel;	/* host variable index -> the estimate the histogram reports */
static std::deque<std::string> g_text;	/* backing store of bound strings (stable addresses) */

static int g_cache_calls;	/* histogram_blob_from_class_cache calls */
static int g_like_calls;	/* histogram_get_like_selectivity calls */
static std::string g_like_column;	/* column node the last LIKE probe received */
static int g_like_hv;		/* host variable index of the DB_VALUE the last LIKE probe received */

static int
hv_index_of (const DB_VALUE *v)
{
  return (g_parser != NULL && v != NULL) ? (int) (v - g_parser->host_variables) : -1;
}

/* parser_support.c pt_get_end_path_node, verbatim */
PT_NODE *
pt_get_end_path_node (PT_NODE *node)
{
  while (node != NULL && node->node_type == PT_DOT_)
    {
      node = node->info.dot.arg2;
    }
  return node;
}

/* histogram_init_reader_from_lhs (histogram_cl.cpp:590): the name-node annotation, else -- while a
 * fingerprint walk is active, which is the only time these probes run here -- the class cache */
static bool
column_has_histogram (PT_NODE *lhs)
{
  PT_NODE *name = pt_get_end_path_node (lhs);

  if (name == NULL || name->node_type != PT_NAME)
    {
      return false;
    }
  return name->info.name.histogram != NULL
    || (name->info.name.original != NULL && g_cache.count (name->info.name.original) > 0);
}

/* db_value_type (dbtype_function.i:833): a NULL value reports DB_TYPE_NULL whatever its type field */
static DB_TYPE
value_type (const DB_VALUE *v)
{
  return v->domain.general_info.is_null ? DB_TYPE_NULL : (DB_TYPE) v->domain.general_info.type;
}

/* histogram_blob_from_class_cache (histogram_cl.cpp:532): spec -> class -> au_fetch_class ->
 * class_->histogram (fetched once by stats_get_histogram, which always leaves a HIST_STATS with
 * attr_ids -1 for columns without one) -> histogram[h] of the column's attr id, else NULL */
DB_VALUE *
histogram_blob_from_class_cache (PARSER_CONTEXT *parser, PT_NODE *statement, PT_NODE *name)
{
  g_cache_calls++;
  if (name == NULL || name->info.name.original == NULL)
    {
      return NULL;
    }
  return g_cache.count (name->info.name.original) > 0 ? &g_blob : NULL;
}

/* histogram_get_like_selectivity (histogram_cl.cpp:1854). Its "cannot price" exits: rhs NULL (1858),
 * no histogram (1865), DB_VALUE_TYPE neither STRING nor CHAR -- NULL, an uncast INTEGER (1880), empty
 * pattern (1909). Otherwise success with a selectivity floored at 1/N (2003) -- here the test's value. */
void
histogram_get_like_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, double *selectivity, bool *success)
{
  PT_NODE *name = pt_get_end_path_node (lhs);

  g_like_calls++;
  g_like_column = (name != NULL && name->node_type == PT_NAME && name->info.name.original != NULL)
    ? name->info.name.original : "";
  g_like_hv = hv_index_of (rhs_db_value);

  *success = false;
  if (rhs_db_value == NULL || !column_has_histogram (lhs))
    {
      return;
    }
  if (value_type (rhs_db_value) != DB_TYPE_STRING && value_type (rhs_db_value) != DB_TYPE_CHAR)
    {
      return;
    }
  if (rhs_db_value->data.ch.medium.size <= 0)
    {
      return;
    }
  auto p = g_sel.find (g_like_hv);
  if (p == g_sel.end ())
    {
      return;
    }
  *selectivity = p->second;
  *success = true;
}

/* histogram_get_equal/comp_selectivity (histogram_cl.cpp:1032/1216): rhs NULL, no histogram, or a value
 * histogram_extract_key cannot key (DB_TYPE_NULL falls to its default) -> success false */
static void
fake_price (PT_NODE *lhs, DB_VALUE *rhs, double *selectivity, bool *success)
{
  *success = false;
  if (rhs == NULL || !column_has_histogram (lhs) || value_type (rhs) == DB_TYPE_NULL)
    {
      return;
    }
  auto p = g_sel.find (hv_index_of (rhs));
  if (p == g_sel.end ())
    {
      return;
    }
  *selectivity = p->second;
  *success = true;
}

void
histogram_get_equal_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, double *selectivity, bool *success)
{
  fake_price (lhs, rhs_db_value, selectivity, success);
}

void
histogram_get_comp_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, bool is_ge, bool include_equal,
				double *selectivity, bool *success)
{
  fake_price (lhs, rhs_db_value, selectivity, success);
}

/* parse_tree_cl.c pt_walk_private, for the node kinds built here: pre; then (CONTINUE/LEAF) the
 * sub-trees -- PT_EXPR arg1, arg2, arg3 -- ; then or_next; then (CONTINUE/LIST) the rest of the
 * `next` list; then post. PT_APPLY_WALK skips NULL and stops on PT_STOP_WALK. */
static PT_NODE *
walk (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE_WALK_FUNCTION pre, void *pre_arg, PT_NODE_WALK_FUNCTION post,
      void *post_arg, int *cont)
{
  int save;

  if (node == NULL || *cont == PT_STOP_WALK)
    {
      return node;
    }
  if (pre != NULL)
    {
      node = pre (parser, node, pre_arg, cont);
      if (node == NULL)
	{
	  return NULL;
	}
    }
  if (*cont != PT_STOP_WALK)
    {
      save = *cont;
      if ((save == PT_CONTINUE_WALK || save == PT_LEAF_WALK) && node->node_type == PT_EXPR)
	{
	  walk (parser, node->info.expr.arg1, pre, pre_arg, post, post_arg, cont);
	  walk (parser, node->info.expr.arg2, pre, pre_arg, post, post_arg, cont);
	  walk (parser, node->info.expr.arg3, pre, pre_arg, post, post_arg, cont);
	}
      if (save == PT_CONTINUE_WALK || save == PT_LEAF_WALK || save == PT_LIST_WALK)
	{
	  walk (parser, node->or_next, pre, pre_arg, post, post_arg, cont);
	}
      if (save == PT_CONTINUE_WALK || save == PT_LIST_WALK)
	{
	  walk (parser, node->next, pre, pre_arg, post, post_arg, cont);
	}
      if (*cont != PT_STOP_WALK)
	{
	  *cont = save;
	}
    }
  if (post != NULL)
    {
      node = post (parser, node, post_arg, cont);
    }
  return node;
}

PT_NODE *
parser_walk_tree (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE_WALK_FUNCTION pre_function, void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  int cont = PT_CONTINUE_WALK;

  return walk (parser, node, pre_function, pre_argument, post_function, post_argument, &cont);
}

/* ---------------------------------------------------------------- appendix hook */
bool unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
				  bool *out_reversed);

/* ---------------------------------------------------------------- builders */

static PT_NODE *
mk (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

/* a resolved column of spec 1; `annotated` = the planner left the histogram blob on the node */
static PT_NODE *
col (const char *name, bool annotated = false)
{
  PT_NODE *n = mk (PT_NAME);
  n->info.name.original = name;
  n->info.name.resolved = "t";
  n->info.name.spec_id = 1;
  n->info.name.histogram = annotated ? &g_blob : NULL;
  return n;
}

/* t.col kept as a path chain */
static PT_NODE *
dot (const char *name)
{
  PT_NODE *d = mk (PT_DOT_);
  d->info.dot.arg1 = col ("t");
  d->info.dot.arg2 = col (name);
  return d;
}

static PT_NODE *
hv (int index)
{
  PT_NODE *n = mk (PT_HOST_VAR);
  n->info.host_var.index = index;
  return n;
}

static PT_NODE *
literal ()
{
  PT_NODE *n = mk (PT_VALUE);
  n->type_enum = PT_TYPE_VARCHAR;
  return n;
}

static PT_NODE *
ex (PT_OP_TYPE op, PT_NODE *a, PT_NODE *b, PT_NODE *c = NULL)
{
  PT_NODE *n = mk (PT_EXPR);
  n->info.expr.op = op;
  n->info.expr.arg1 = a;
  n->info.expr.arg2 = b;
  n->info.expr.arg3 = c;
  return n;
}

/* `col LIKE ? ESCAPE c` as the type checker leaves it: PT_LIKE (col, PT_LIKE_ESCAPE (?, c))
 * (type_checking.c:9112-9118 / 9337-9340) */
static PT_NODE *
like_escape (PT_OP_TYPE op, PT_NODE *column, PT_NODE *pattern)
{
  return ex (op, column, ex (PT_LIKE_ESCAPE, pattern, literal ()));
}

/* `col RANGE (? GT_INF max)` as qo_convert_to_range makes of `col > ?` */
static PT_NODE *
range_gt (PT_NODE *column, PT_NODE *bound)
{
  return ex (PT_RANGE, column, ex (PT_BETWEEN_GT_INF, bound, NULL));
}

static void
set_null (DB_VALUE *v)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_NULL;
  v->domain.general_info.is_null = 1;
}

static void
set_int (DB_VALUE *v, int x)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_INTEGER;
  v->data.i = x;
}

/* db_value_domain_init (VARCHAR) + db_make_db_char: fields set directly */
static void
set_str (DB_VALUE *v, const char *s)
{
  g_text.emplace_back (s);
  const std::string &k = g_text.back ();

  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_STRING;
  v->domain.general_info.is_null = 0;
  v->domain.char_info.length = DB_MAX_VARCHAR_PRECISION;
  v->data.ch.info.style = MEDIUM_STRING;
  v->data.ch.medium.size = (int) k.size ();
  v->data.ch.medium.buf = k.data ();
  v->data.ch.medium.compressed_buf = NULL;
  v->data.ch.medium.compressed_size = DB_NOT_YET_COMPRESSED;
  v->data.ch.medium.length = -1;
}

/* one statement over t: a WHERE list of predicates, `nhv` user host variables (+ `nauto`
 * auto-parameterized literals after them) */
struct scene
{
  PARSER_CONTEXT *parser;
  PT_NODE *stmt = NULL, *tail = NULL;

  explicit scene (int nhv, int nauto = 0)
  {
    parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    parser->host_variables = (DB_VALUE *) calloc (nhv + nauto, sizeof (DB_VALUE));
    parser->host_var_count = nhv;
    parser->auto_param_count = nauto;
    for (int i = 0; i < nhv + nauto; i++)
      {
	set_null (&parser->host_variables[i]);
      }
    g_parser = parser;
    g_cache.clear ();
    g_sel.clear ();
    g_cache_calls = 0;
    g_like_calls = 0;
    g_like_column.clear ();
    g_like_hv = -2;
  }
  void hist (const char *column)	/* the class cache holds a histogram for this column */
  {
    g_cache.insert (column);
  }
  PT_NODE *add (PT_NODE *pred)
  {
    if (tail == NULL)
      {
	stmt = pred;
      }
    else
      {
	tail->next = pred;
      }
    tail = pred;
    return pred;
  }
  void bind (int i, const char *pattern, double sel)	/* a pattern the histogram prices at sel */
  {
    set_str (&parser->host_variables[i], pattern);
    g_sel[i] = sel;
  }
  void bind_null (int i)
  {
    set_null (&parser->host_variables[i]);
    g_sel.erase (i);
  }
  void bind_int (int i, int x, double sel)
  {
    set_int (&parser->host_variables[i], x);
    g_sel[i] = sel;
  }
  bool fp (UINT64 *out)
  {
    return histogram_bind_fingerprint (parser, stmt, out);
  }
  bool flagged ()
  {
    return histogram_stmt_has_hv_predicate (parser, stmt);
  }
};

/* fingerprint of `s <op> ?` (s has a histogram) with the LIKE estimator reporting `like_sel`;
 * false when the statement yields no fingerprint */
static bool
like_fp (PT_OP_TYPE op, double like_sel, UINT64 *out, const char *pattern = "abc%", const char *column = "s")
{
  scene s (1);
  s.hist (column);
  s.add (ex (op, col (column), hv (0)));
  s.bind (0, pattern, like_sel);
  return s.fp (out);
}

#define LIKE_FP(op, sel, var) UINT64 var = 0; REQUIRE (like_fp (op, sel, &var))

/* ================================================================ 1. candidate test */

TEST_CASE ("PR8006 split: `col LIKE ?` and `col NOT LIKE ?` with a user host variable are candidates "
	   "(column node, host variable node, not reversed)", "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      PT_NODE *c = col ("s"), *h = hv (0), *name = NULL, *var = NULL;
      bool rev = true;

      CHECK (unitseam_split_hv_predicate (s.parser, ex (op, c, h), &name, &var, &rev));
      CHECK (name == c);
      CHECK (var == h);
      CHECK_FALSE (rev);
    }
}

TEST_CASE ("PR8006 split: `t.col LIKE ?` (path chain) hands back the end-of-path column", "[contract][split]")
{
  scene s (1);
  PT_NODE *d = dot ("s"), *name = NULL, *var = NULL;

  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_LIKE, d, hv (0)), &name, &var, NULL));
  CHECK (name == d->info.dot.arg2);
}

TEST_CASE ("PR8006 split: `? LIKE col` / `? NOT LIKE col` (the column is the pattern) is not a candidate",
	   "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      PT_NODE *name = (PT_NODE *) 1, *var = (PT_NODE *) 1;
      bool rev = true;

      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, hv (0), col ("s")), &name, &var, &rev));
      CHECK (name == NULL);	/* out-params reset on a miss */
      CHECK (var == NULL);
      CHECK_FALSE (rev);
    }
}

TEST_CASE ("PR8006 split: `col LIKE ? ESCAPE c` (arg2 = PT_LIKE_ESCAPE (?, c)) is not a candidate, nor is the "
	   "escape node itself", "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      PT_NODE *e = like_escape (op, col ("s"), hv (0));

      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, e, NULL, NULL, NULL));
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, e->info.expr.arg2, NULL, NULL, NULL));
    }
}

TEST_CASE ("PR8006 split: LIKE with a literal pattern or an auto-parameterized literal is not a candidate",
	   "[contract][split]")
{
  scene s (1, 1);		/* ?0 user, index 1 auto-parameterized */

  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_LIKE, col ("s"), literal ()), NULL, NULL, NULL));
  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_LIKE, col ("s"), hv (1)), NULL, NULL, NULL));
  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_NOT_LIKE, col ("s"), hv (1)), NULL, NULL, NULL));
}

TEST_CASE ("PR8006 split: RLIKE is outside the candidate set (scope: LIKE / NOT LIKE only)", "[contract][split]")
{
  scene s (1);

  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_RLIKE, col ("s"), hv (0)), NULL, NULL, NULL));
  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_NOT_RLIKE, col ("s"), hv (0)), NULL, NULL, NULL));
}

TEST_CASE ("PR8006 split control: comparisons unchanged -- `col = ?` plain, `? < col` reversed", "[contract][split]")
{
  scene s (1);
  PT_NODE *c = col ("a"), *h = hv (0), *name = NULL, *var = NULL;
  bool rev = true;

  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_EQ, c, h), &name, &var, &rev));
  CHECK (name == c);
  CHECK_FALSE (rev);

  PT_NODE *c2 = col ("a"), *h2 = hv (0);
  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_LT, h2, c2), &name, &var, &rev));
  CHECK (name == c2);
  CHECK (var == h2);
  CHECK (rev);
}

/* ================================================================ 2. LIKE fingerprint component */

TEST_CASE ("PR8006 fp: a LIKE predicate is priced by histogram_get_like_selectivity on the column node and "
	   "host_variables[index]", "[contract][fp]")
{
  scene s (2);
  s.hist ("s");
  s.add (ex (PT_LIKE, col ("s"), hv (1)));
  s.bind (1, "abc%", 0.3);
  UINT64 fp = 0;

  CHECK (s.fp (&fp));
  CHECK (fp != 0);
  CHECK (g_like_calls == 1);
  CHECK (g_like_column == "s");
  CHECK (g_like_hv == 1);
}

TEST_CASE ("PR8006 fp: LIKE estimates in the same power-of-two band share a fingerprint, whatever the pattern text",
	   "[contract][fp][band]")
{
  LIKE_FP (PT_LIKE, 0.26, a);
  UINT64 b = 0, c = 0, d = 0, e = 0;

  REQUIRE (like_fp (PT_LIKE, 0.30, &b, "xyz1%"));
  REQUIRE (like_fp (PT_LIKE, 0.49, &c, "%q%"));
  CHECK (a == b);		/* [0.25, 0.5) */
  CHECK (a == c);

  REQUIRE (like_fp (PT_LIKE, 0.0011, &d, "abc%"));
  REQUIRE (like_fp (PT_LIKE, 0.0019, &e, "zzz%"));
  CHECK (d == e);		/* [2^-10, 2^-9) */
}

TEST_CASE ("PR8006 fp: LIKE band edge is the power of two -- 2^-k shares with the top of its band "
	   "(just under 2^-k+1) and not with the value just under it (k = 1, 2, 3, 10, 20)", "[contract][fp][band]")
{
  for (int k : { 1, 2, 3, 10, 20 })
    {
      const double edge = std::ldexp (1.0, -k);
      const double top = std::nextafter (2.0 * edge, 0.0);
      const double below = std::nextafter (edge, 0.0);
      UINT64 at = 0, up = 0, under = 0;

      INFO ("k = " << k);
      REQUIRE (like_fp (PT_LIKE, edge, &at));
      REQUIRE (like_fp (PT_LIKE, top, &up));
      REQUIRE (like_fp (PT_LIKE, below, &under));
      CHECK (at == up);
      CHECK (at != under);
    }
}

TEST_CASE ("PR8006 fp: LIKE bands tell a 10x rarer prefix apart (1e-3 vs 1e-2) and any 2x step "
	   "(the range rule's 0.01 steps would fold both)", "[contract][fp][band]")
{
  LIKE_FP (PT_LIKE, 0.001, rare);
  LIKE_FP (PT_LIKE, 0.01, common);
  LIKE_FP (PT_LIKE, 0.003, x);
  LIKE_FP (PT_LIKE, 0.006, x2);

  CHECK (rare != common);
  CHECK (x != x2);
}

TEST_CASE ("PR8006 fp: LIKE estimate of exactly 1.0 (and an FP overshoot 1+ulp) is the band [1, 2) -- apart from "
	   "[0.5, 1)", "[contract][fp][band]")
{
  LIKE_FP (PT_LIKE, 1.0, one);
  LIKE_FP (PT_LIKE, std::nextafter (1.0, 2.0), over);
  LIKE_FP (PT_LIKE, std::nextafter (1.0, 0.0), under);
  LIKE_FP (PT_LIKE, 0.5, half);

  CHECK (one == over);
  CHECK (one != under);
  CHECK (under == half);
}

TEST_CASE ("PR8006 fp: NOT LIKE is banded on 1 - LIKE (0.9 -> 0.1 vs 0.95 -> 0.05 differ; 0.6 -> 0.4 and "
	   "0.7 -> 0.3 share; 0.001 and 0.4 both land in [0.5, 1))", "[contract][fp][band]")
{
  LIKE_FP (PT_NOT_LIKE, 0.90, n90);
  LIKE_FP (PT_NOT_LIKE, 0.95, n95);
  LIKE_FP (PT_NOT_LIKE, 0.60, n60);
  LIKE_FP (PT_NOT_LIKE, 0.70, n70);
  LIKE_FP (PT_NOT_LIKE, 0.001, n001);
  LIKE_FP (PT_NOT_LIKE, 0.40, n40);

  CHECK (n90 != n95);
  CHECK (n60 == n70);
  CHECK (n001 == n40);
}

TEST_CASE ("PR8006 fp: LIKE and NOT LIKE of the same estimate, and LIKE on two columns, get different fingerprints",
	   "[contract][fp]")
{
  /* LIKE 0.375 and NOT LIKE (1 - 0.625 = 0.375) land in the same band [0.25, 0.5): the op is mixed in, and
   * so is the column */
  LIKE_FP (PT_LIKE, 0.375, like);
  LIKE_FP (PT_NOT_LIKE, 0.625, not_like);	/* 1 - 0.625 = 0.375: same band value as `like` */
  UINT64 other = 0;

  REQUIRE (like_fp (PT_LIKE, 0.375, &other, "abc%", "s2"));
  CHECK (like != not_like);
  CHECK (like != other);
}

TEST_CASE ("PR8006 G1: NOT LIKE of a pattern every value matches (1 - LIKE = 0) has a band of its own, apart from "
	   "[0.5, 1) -- before 1d5858374 frexp (0) put it in [0.5, 1)", "[contract][fp][G1]")
{
  LIKE_FP (PT_NOT_LIKE, 1.0, zero);	/* 1 - 1.0 = 0: no rows */
  LIKE_FP (PT_NOT_LIKE, 0.3, most);	/* 0.7: most of the table */
  LIKE_FP (PT_NOT_LIKE, 0.001, nearly_all);	/* 0.999 */

  CHECK (zero != most);
  CHECK (zero != nearly_all);
}

TEST_CASE ("PR8006 G1: a 1 - LIKE below zero (LIKE = 1+ulp) folds into the zero band; the smallest positive "
	   "1 - LIKE (2^-53) stays out of it", "[contract][fp][G1]")
{
  LIKE_FP (PT_NOT_LIKE, 1.0, zero);
  LIKE_FP (PT_NOT_LIKE, std::nextafter (1.0, 2.0), negative);
  LIKE_FP (PT_NOT_LIKE, std::nextafter (1.0, 0.0), tiny);	/* 1 - (1 - 2^-53) = 2^-53 */

  CHECK (zero == negative);
  CHECK (zero != tiny);
}

TEST_CASE ("PR8006 fp: a pattern the estimator cannot price (NULL, uncast INTEGER) contributes nothing -- "
	   "no value hash; alone the statement has no fingerprint", "[contract][fp][null]")
{
  /* `a = ?0 AND s LIKE ?1` with ?1 unpriceable == `a = ?0` alone */
  UINT64 base_fp = 0;
  {
    scene s (1);
    s.hist ("a");
    s.add (ex (PT_EQ, col ("a"), hv (0)));
    s.bind (0, "k", 0.02);
    REQUIRE (s.fp (&base_fp));
  }
  for (int variant = 0; variant < 2; variant++)
    {
      scene s (2);
      s.hist ("a");
      s.hist ("s");
      s.add (ex (PT_EQ, col ("a"), hv (0)));
      s.add (ex (PT_LIKE, col ("s"), hv (1)));
      s.bind (0, "k", 0.02);
      if (variant == 0)
	{
	  s.bind_null (1);
	}
      else
	{
	  s.bind_int (1, 123, 0.3);	/* the EXECUTE paths cast to the column domain first; uncast it is unpriceable */
	}
      UINT64 fp = 0;

      INFO ("variant " << (variant == 0 ? "NULL" : "INTEGER"));
      REQUIRE (s.fp (&fp));
      CHECK (fp == base_fp);
      CHECK (g_like_calls == 1);	/* asked, declined */
    }
  {
    scene s (1);
    s.hist ("s");
    s.add (ex (PT_LIKE, col ("s"), hv (0)));
    s.bind_null (0);
    UINT64 fp = 0;

    CHECK_FALSE (s.fp (&fp));
  }
}

TEST_CASE ("PR8006 fp: the ESCAPE form, `? LIKE col` and an auto-parameterized pattern never reach the LIKE "
	   "estimator and give no fingerprint", "[contract][fp]")
{
  {
    scene s (1);
    s.hist ("s");
    s.add (like_escape (PT_LIKE, col ("s"), hv (0)));
    s.bind (0, "a\\%%", 0.3);
    UINT64 fp = 0;
    CHECK_FALSE (s.fp (&fp));
  }
  {
    scene s (1);
    s.hist ("s");
    s.add (ex (PT_NOT_LIKE, hv (0), col ("s")));
    s.bind (0, "abc", 0.3);
    UINT64 fp = 0;
    CHECK_FALSE (s.fp (&fp));
  }
  {
    scene s (0, 1);
    s.hist ("s");
    s.add (ex (PT_LIKE, col ("s"), hv (0)));
    s.bind (0, "abc%", 0.3);
    UINT64 fp = 0;
    CHECK_FALSE (s.fp (&fp));
  }
  CHECK (g_like_calls == 0);
}

TEST_CASE ("PR8006 fp: LIKE on a column without a histogram contributes nothing", "[contract][fp]")
{
  scene s (1);
  s.add (ex (PT_LIKE, col ("s"), hv (0)));
  s.bind (0, "abc%", 0.3);
  UINT64 fp = 0;

  CHECK_FALSE (s.fp (&fp));
}

TEST_CASE ("PR8006 fp control: comparison components unchanged -- `a = ?` stepwise (1e-12), `b > ?` 0.01 steps",
	   "[contract][fp]")
{
  auto one = [] (PT_OP_TYPE op, const char *c, double sel) -> UINT64
  {
    scene s (1);
    s.hist (c);
    s.add (ex (op, col (c), hv (0)));
    s.bind (0, "k", sel);
    UINT64 fp = 0;
    REQUIRE (s.fp (&fp));
    return fp;
  };

  CHECK (one (PT_EQ, "a", 0.3) != one (PT_EQ, "a", 0.3000001));
  CHECK (one (PT_EQ, "a", 0.3) == one (PT_EQ, "a", 0.3));
  CHECK (one (PT_GT, "b", 0.301) == one (PT_GT, "b", 0.309));
  CHECK (one (PT_GT, "b", 0.301) != one (PT_GT, "b", 0.311));
}

/* ================================================================ 3. unpeeked flag (G2) */

TEST_CASE ("PR8006 G2: a candidate on a column with a histogram flags the plan -- from the name-node annotation "
	   "without a catalog probe, else from the class cache", "[contract][G2]")
{
  {
    scene s (1);
    s.add (ex (PT_EQ, col ("a", true), hv (0)));
    CHECK (s.flagged ());
    CHECK (g_cache_calls == 0);
  }
  {
    scene s (1);
    s.hist ("a");
    s.add (ex (PT_EQ, col ("a"), hv (0)));
    CHECK (s.flagged ());
  }
}

TEST_CASE ("PR8006 G2: a candidate on a column without a histogram does not flag (`a = ?`, `b RANGE (? GT_INF)`, "
	   "`s LIKE ?`) -- base flagged the first two on the operator alone", "[contract][G2]")
{
  {
    scene s (1);
    s.add (ex (PT_EQ, col ("a"), hv (0)));
    CHECK_FALSE (s.flagged ());
  }
  {
    scene s (1);
    s.add (range_gt (col ("b"), hv (0)));
    CHECK_FALSE (s.flagged ());
  }
  {
    scene s (1);
    s.add (ex (PT_LIKE, col ("s"), hv (0)));
    CHECK_FALSE (s.flagged ());
  }
}

TEST_CASE ("PR8006 G2: the walk goes past a histogram-less candidate to a later one that has a histogram",
	   "[contract][G2]")
{
  scene s (2);
  s.hist ("b");
  s.add (ex (PT_EQ, col ("a"), hv (0)));
  s.add (range_gt (col ("b"), hv (1)));

  CHECK (s.flagged ());
}

TEST_CASE ("PR8006 G2: `s LIKE ?` / `s NOT LIKE ?` on a column with a histogram flag the plan; the ESCAPE form and "
	   "`? LIKE s` do not", "[contract][G2]")
{
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      scene s (1);
      s.hist ("s");
      s.add (ex (op, col ("s"), hv (0)));
      CHECK (s.flagged ());
    }
  {
    scene s (1);
    s.hist ("s");
    s.add (like_escape (PT_LIKE, col ("s"), hv (0)));
    s.add (ex (PT_LIKE, hv (0), col ("s")));
    CHECK_FALSE (s.flagged ());
  }
}

TEST_CASE ("PR8006 G2 vs fp: the flag is structural, the fingerprint value-dependent -- `s LIKE ?` on a column "
	   "with a histogram is flagged, yet a NULL pattern yields no fingerprint (db_vdb.c:2244 then clears the "
	   "unpeeked flag)", "[contract][G2][null]")
{
  scene s (1);
  s.hist ("s");
  s.add (ex (PT_LIKE, col ("s"), hv (0)));
  s.bind_null (0);
  UINT64 fp = 0;

  CHECK (s.flagged ());
  CHECK_FALSE (s.fp (&fp));
}
