/*
 * PR#8325 (CBRD-27811) -- the HAVING selectivity applied to the GROUP BY row estimate, src/optimizer/query_planner.c
 * (CS_MODE) as 5cfef7f5b has it, reached through appendix hooks (hooks.cpp):
 *
 *   qo_estimate_ngroups (plan, sort_type)              the GROUP BY / DISTINCT group count kept in plan->info->group_rows
 *     -> qo_having_env_init (henv, plan, group_size)   the size, and the GROUP BY column with a histogram (one column)
 *     -> qo_having_selectivity (henv, having)          the conjuncts (next) multiply
 *          -> qo_having_pred_selectivity               the disjuncts (or_next) combine as OR
 *               -> qo_having_disjunct_selectivity      one disjunct, next and or_next cut:
 *                    folded constant 1/0; GROUPBY_NUM () 1; no aggregate -> qo_expr_selectivity (seam);
 *                    AND / OR / NOT -> recurse; else qo_having_aggregate_term, [0.001, 1], not priced = 0.5
 *                    -> qo_having_compare / _equal / _between / _range / _in   (qo_having_value, qo_having_row_compare)
 *                         -> histogram_get_comp_selectivity (fake)            x op c on the segment's annotated name
 *                         -> histogram_get_group_size_selectivity (fake)      COUNT (*) from the GROUP BY column
 *
 * The contract read off the functions and their comments (query_planner.c:778-1478 at 5cfef7f5b):
 *   A  one aggregate predicate. AVG (x) op c = p (the histogram selectivity of x op c); SUM (x) op c = x op c / group
 *      size; MAX (x) > c, MIN (x) < c = some row of the group passes, MAX (x) < c, MIN (x) > c = every row does, sizes
 *      geometric with the given mean; COUNT (*) op c = the group size op c -- from the GROUP BY column's histogram when
 *      there is one, else geometric (P (size >= k) = r^(k-1)), not priced when the size is unknown; a NULL share f:
 *      an all-NULL group fails, the others aggregate their non-NULL rows; c op AGG is AGG reversed op c; = is
 *      P (>= c) - P (> c), <> its complement; BETWEEN is P (>= low) + P (<= high) - 1; RANGE and IN add their items;
 *      a constant the histogram cannot take that lies beyond +-9e18 is past every value; the disjunct lies in
 *      [DEFAULT_EQUAL_SELECTIVITY, 1]; a form it cannot price takes QO_HAVING_DEFAULT_SELECTIVITY (0.5)
 *   H  the conjuncts multiply, the disjuncts of a conjunct and AND / OR / NOT combine as in WHERE; a predicate
 *      without an aggregate is a WHERE term (qo_expr_selectivity on that predicate alone); one with GROUPBY_NUM () is
 *      left to the LIMIT handling; an aggregate inside a subquery does not count; lists are left as they were
 *   E  the groups (NDV known: estimate_ndv, unknown: the rows) are scaled by the HAVING selectivity at the group size
 *      rows / groups (unknown: 0 -- COUNT (*) not priced), floored at 1; HAVING only for the GROUP BY sort;
 *      group_rows only goes down (the info is shared with a DISTINCT over the GROUP BY)
 *
 * Real code: query_planner.c (the TU), query_graph.c (lookup_node / lookup_seg / qo_abort), parser_support.c
 * (pt_is_aggregate_function, pt_get_end_path_node, pt_name_equal, pt_is_function_index_expression), parse_tree.c +
 * parse_tree_cl.c (parser_create_parser, parser_new_node, parser_walk_tree with its apply table,
 * pt_is_function_index_expr), object_domain.c + object_primitive.c + domain_rules.c + numeric_opfunc.c
 * (tp_value_coerce to tp_Double_domain: INTEGER, DOUBLE, NUMERIC, strings; tp_value_compare), db_macro.c,
 * language_support.c + intl_support.c + chartype.c (en_US.utf8), memory_alloc.c, system_parameter.c.
 * Seams: qo_get_group_ndv (the GROUP BY / select-list NDV from statistics), qo_expr_selectivity (the WHERE-term
 * estimate). Fakes (see suite.json "fakes"): histogram_get_comp_selectivity (column x), histogram_get_total_rows /
 * histogram_get_column_ndv / histogram_get_group_size_selectivity (GROUP BY column a), db_set_size / db_set_get (a
 * folded IN list), pt_false_search_condition (type_checking.c body), er_*, pt_parser_line_col,
 * pt_free_dblink_remote_cols, envvar_get / envvar_trim_char, init_builtin_calendar_names / db_date_locale_init,
 * cubregex::get_engine_name.
 *
 * Tags: [contract] expected behaviour (head FAIL + base PASS = regression candidate); `... develop` cases hold on base
 *       too; the others are this PR's (base FAIL = new). [defect] reproduces a defect (PASS = the defect is there).
 *       "(5cfef7f5b: ...)" in a name marks a case the commit 5cfef7f5b changed on purpose (the value the earlier
 *       head 766999f0e gave is in the case's comment).
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "db_set_function.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "query_list.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "numeric_opfunc.h"
#include "histogram_cl.hpp"
#include "string_regex.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------- environment fakes */

/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c envvar_trim_char body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* string_opfunc.c / db_date.c: locale side tables (calendar names, AM/PM) nothing here reads */
void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

void
db_date_locale_init (void)
{
}

/* scanner_support.c pt_parser_line_col () (C linkage): no lexer, position 0 */
extern "C" void
pt_parser_line_col (PT_NODE * node)
{
  if (node != NULL)
    {
      node->line_number = 0;
      node->column_number = 0;
    }
}

/* name_resolution.c pt_free_dblink_remote_cols (): a parser that resolved no dblink table has none */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

/* type_checking.c:7248 pt_false_search_condition () at 5cfef7f5b, the body as is */
bool
pt_false_search_condition (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  while (node)
    {
      if (node->or_next == NULL
	  && (node->type_enum == PT_TYPE_NA || node->type_enum == PT_TYPE_NULL
	      || (node->node_type == PT_VALUE && node->type_enum == PT_TYPE_LOGICAL
		  && node->info.value.data_value.i == 0)))
	{
	  return true;
	}

      node = node->next;
    }

  return false;
}

/* error_manager: one level; qo_abort () (query_graph.c) er_set ()s ER_FAILED_ASSERTION before its longjmp */
namespace
{
  int g_er_id = NO_ERROR;
  int g_er_sets = 0;
}

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  g_er_id = err_id;
  g_er_sets++;
}

int
er_errid (void)
{
  return g_er_id;
}

void
er_clear (void)
{
  g_er_id = NO_ERROR;
}

/* ---------------------------------------------------------------- a folded IN list (fake set) */

/* db_set.c db_set_size () / db_set_get () (C linkage): the element count, NULLs included, and a copy of one element
 * (set_get_element; the caller pr_clear_value ()s it). The real ones also refuse to run without a server connection
 * (CHECK_CONNECT_*), which the optimizer always has. The set here is a vector of INTEGER / DOUBLE / NULL values, for
 * which the copy is the value itself. */
namespace
{
  std::deque < std::vector < DB_VALUE > >g_sets;
  int g_set_gets = 0;
}

int
db_set_size (DB_COLLECTION * set)
{
  if (set == NULL)
    {
      return -1;
    }
  return (int) reinterpret_cast < std::vector < DB_VALUE > *>(set)->size ();
}

int
db_set_get (DB_COLLECTION * set, int index, DB_VALUE * value)
{
  g_set_gets++;
  if (set == NULL || value == NULL)
    {
      return ER_OBJ_INVALID_ARGUMENTS;
    }
  std::vector < DB_VALUE > &v = *reinterpret_cast < std::vector < DB_VALUE > *>(set);
  if (index < 0 || index >= (int) v.size ())
    {
      return ER_SET_INVALID_INDEX;
    }
  *value = v[index];
  return NO_ERROR;
}

/* ---------------------------------------------------------------- the histograms (fakes) */

/* histogram_init_reader_from_lhs (histogram_cl.cpp:724): the end of a path, a PT_NAME carrying the annotation
 * (info.name.histogram; the class-cache fallback needs bind_fp_active_statement, set only during a bind fingerprint
 * walk, never while planning) -- otherwise nothing to read. */
static PT_NODE *
reader_name (PT_NODE * lhs)
{
  if (lhs != NULL && lhs->node_type == PT_DOT_)
    {
      lhs = pt_get_end_path_node (lhs);
    }
  if (lhs == NULL || lhs->node_type != PT_NAME || lhs->info.name.histogram == NULL)
    {
      return NULL;
    }
  return lhs;
}

/* column x -- histogram_get_comp_selectivity () (histogram_cl.cpp:1377 at 5cfef7f5b), its exits:
 *   rhs NULL, no readable histogram -> not priced;
 *   histogram_extract_key + the kind check (1395-1440): a numeric key only (a string, a NULL -> not priced);
 *     INTEGER / SHORT / BIGINT are i64 keys, FLOAT / DOUBLE / NUMERIC / MONETARY dbl keys; against a DOUBLE column an
 *     i64 key is promoted; against an INTEGER column a dbl key outside [-9e18, 9e18] is not priced, a fraction
 *     becomes floor + 1 with include_equal = is_ge;
 *   total_rows <= 0 -> success, 0;
 *   otherwise the share of the NON-NULL rows (the real one divides the null mass back out, 1496-1501), clamped to
 *     [0, 1], and an estimate of 0 floored at 1 / total_rows (1515-1518).
 * The values: spread evenly over [g_lo, g_hi] (P (x < v) = (v - lo) / (hi - lo)) plus a point mass g_eq at each v in
 * [lo, hi] (P (x <= v) = P (x < v) + g_eq) -- with g_eq = 0 include_equal does not move it. Every probe is recorded. */
namespace
{
  double g_lo = 0.0, g_hi = 100.0, g_total = 1000000.0, g_eq = 0.0;
  bool g_x_i64 = false;		/* the column x: false = DOUBLE, true = INTEGER */
  DB_VALUE *g_blob_a = NULL;	/* the GROUP BY column a's annotation (the group histogram below) */
  int g_unexpected = 0;		/* a fake asked about a column it does not model */
  int g_probes = 0, g_probe_ok = 0;
  PT_NODE *g_probe_lhs = NULL;
  DB_VALUE *g_probe_value_ptr = NULL;
  double g_probe_value = 0.0;
  DB_TYPE g_probe_type = DB_TYPE_NULL;
  bool g_probe_is_ge = false, g_probe_incl = false;

  bool numeric_key (const DB_VALUE * v, double *out, bool *is_dbl)
  {
    if (DB_IS_NULL (v))
      {
	return false;
      }
    *is_dbl = true;
    switch (DB_VALUE_DOMAIN_TYPE (v))
      {
      case DB_TYPE_INTEGER:
	*is_dbl = false;
	*out = db_get_int (v);
	return true;
      case DB_TYPE_SHORT:
	*is_dbl = false;
	*out = db_get_short (v);
	return true;
      case DB_TYPE_BIGINT:
	*is_dbl = false;
	*out = (double) db_get_bigint (v);
	return true;
      case DB_TYPE_FLOAT:
	*out = db_get_float (v);
	return true;
      case DB_TYPE_DOUBLE:
	*out = db_get_double (v);
	return true;
      case DB_TYPE_NUMERIC:
	{
	  DB_VALUE d;
	  db_make_null (&d);
	  if (tp_value_coerce (v, &d, &tp_Double_domain) != DOMAIN_COMPATIBLE)
	    {
	      return false;
	    }
	  *out = db_get_double (&d);
	  return true;
	}
      default:
	return false;
      }
  }

  double unit (double v)
  {
    return std::min (std::max (v, 0.0), 1.0);
  }

  /* the model of x, as fractions of the non-NULL rows */
  double x_lt (double v)
  {
    return unit ((v - g_lo) / (g_hi - g_lo));
  }

  double x_eq (double v)
  {
    return (v >= g_lo && v <= g_hi) ? g_eq : 0.0;
  }

  double x_le (double v)
  {
    return std::min (x_lt (v) + x_eq (v), 1.0);
  }
}

void
histogram_get_comp_selectivity (PT_NODE * lhs, DB_VALUE * rhs_db_value, bool is_ge, bool include_equal,
				double *selectivity, bool *success)
{
  g_probes++;
  g_probe_lhs = lhs;
  g_probe_value_ptr = rhs_db_value;
  g_probe_is_ge = is_ge;
  g_probe_incl = include_equal;
  g_probe_type = rhs_db_value != NULL ? DB_VALUE_DOMAIN_TYPE (rhs_db_value) : DB_TYPE_NULL;
  *success = false;

  if (rhs_db_value == NULL)
    {
      return;
    }
  PT_NODE *name = reader_name (lhs);
  if (name == NULL)
    {
      return;
    }
  if (g_blob_a != NULL && name->info.name.histogram == g_blob_a)
    {
      g_unexpected++;
      return;
    }
  double v;
  bool is_dbl;
  if (!numeric_key (rhs_db_value, &v, &is_dbl))
    {
      return;
    }
  if (g_x_i64 && is_dbl)
    {
      if (!(v >= -9.0e18 && v <= 9.0e18))
	{
	  return;
	}
      const double fl = std::floor (v);
      if (v != fl)
	{
	  v = fl + 1.0;
	  include_equal = is_ge;
	}
    }
  g_probe_value = v;
  if (g_total <= 0.0)
    {
      *selectivity = 0.0;
      *success = true;
      g_probe_ok++;
      return;
    }
  double sel = is_ge ? (include_equal ? 1.0 - x_lt (v) : 1.0 - x_le (v)) : (include_equal ? x_le (v) : x_lt (v));
  sel = unit (sel);
  if (sel <= 0.0)
    {
      sel = 1.0 / g_total;
    }
  *selectivity = sel;
  *success = true;
  g_probe_ok++;
}

/* column a (the GROUP BY column) -- its value counts: MCV frequencies over all rows and buckets of (rows, distinct
 * values). The three readers follow histogram_cl.cpp at 5cfef7f5b over that spec instead of a blob:
 *   histogram_get_total_rows (1541): no readable histogram -> false; the rows; true when > 0
 *   histogram_get_column_ndv (2086): no readable histogram or no distinct value -> not found; MCVs + bucket distincts
 *   histogram_get_group_size_selectivity (2123): no readable histogram, rows <= 0, no distinct value or scale <= 0
 *     -> not found; an MCV counts when freq * rows * scale + 0.5 >= min_rows, a bucket adds
 *     ndv * r^(ceil (min_rows) - 1) (1 for min_rows <= 1), r = 1 - 1 / max (rows * scale / ndv, 1); the sum over
 *     the distinct values, clamped to [0, 1].
 * The real reader is checked against blobs in suite histogram_cl.group_size_selectivity. */
namespace
{
  struct group_hist
  {
    double total_rows;
    std::vector < double >mcv_freq;
    std::vector < std::pair < double, double >>buckets;	/* rows, distinct values */
  };

  group_hist g_a;
  int g_gs_calls = 0, g_rows_calls = 0, g_colndv_calls = 0;
  PT_NODE *g_gs_attr = NULL;
  double g_gs_min_rows = 0.0, g_gs_scale = 0.0;

  void default_group_hist ()
  {
    /* 10000 rows, 100 values: three MCVs of 2000, 1000 and 500 rows, 97 values over the other 6500 rows */
    g_a.total_rows = 10000.0;
    g_a.mcv_freq = { 0.2, 0.1, 0.05 };
    g_a.buckets = { { 6500.0, 97.0 } };
  }

  double group_distinct ()
  {
    double d = (double) g_a.mcv_freq.size ();
    for (const auto & b:g_a.buckets)
      {
	d += b.second;
      }
    return d;
  }

  /* P (a value's group has min_rows rows or more), the counts scaled by scale */
  bool group_size_oracle (double min_rows, double scale, double *sel)
  {
    const double distinct = group_distinct ();
    if (g_a.total_rows <= 0.0 || distinct <= 0.0 || scale <= 0.0)
      {
	return false;
      }
    double groups = 0.0;
    for (double f:g_a.mcv_freq)
      {
	if (f * g_a.total_rows * scale + 0.5 >= min_rows)
	  {
	    groups += 1.0;
	  }
      }
    for (const auto & b:g_a.buckets)
      {
	if (b.second <= 0.0)
	  {
	    continue;
	  }
	const double mean = b.first * scale / b.second;
	const double r = 1.0 - 1.0 / std::max (mean, 1.0);
	groups += b.second * ((min_rows <= 1.0) ? 1.0 : pow (r, std::ceil (min_rows) - 1.0));
      }
    *sel = unit (groups / distinct);
    return true;
  }

  double group_sel (double min_rows, double scale)
  {
    double s = -1.0;
    (void) group_size_oracle (min_rows, scale, &s);
    return s;
  }

  bool is_group_column (PT_NODE * name)
  {
    if (name->info.name.histogram != g_blob_a)
      {
	g_unexpected++;
	return false;
      }
    return true;
  }
}

bool
histogram_get_total_rows (PT_NODE * lhs, double *total_rows)
{
  g_rows_calls++;
  PT_NODE *name = reader_name (lhs);
  if (name == NULL || !is_group_column (name))
    {
      return false;
    }
  *total_rows = g_a.total_rows;
  return *total_rows > 0.0;
}

void
histogram_get_column_ndv (PT_NODE * attr, double *ndv, bool *success)
{
  g_colndv_calls++;
  *success = false;
  PT_NODE *name = reader_name (attr);
  if (name == NULL || !is_group_column (name))
    {
      return;
    }
  const double distinct = group_distinct ();
  if (distinct <= 0.0)
    {
      return;
    }
  *ndv = distinct;
  *success = true;
}

void
histogram_get_group_size_selectivity (PT_NODE * attr, double min_rows, double scale, double *selectivity,
				      bool *success)
{
  g_gs_calls++;
  g_gs_attr = attr;
  g_gs_min_rows = min_rows;
  g_gs_scale = scale;
  *success = false;
  PT_NODE *name = reader_name (attr);
  if (name == NULL || !is_group_column (name))
    {
      return;
    }
  *success = group_size_oracle (min_rows, scale, selectivity);
}

/* ---------------------------------------------------------------- seams */

/* qo_get_group_ndv (query_planner.c, static): -1 when the statement is not a SELECT; otherwise the product of the
 * statistics NDV of the columns of the GROUP BY list (SORT_GROUPBY) or the select list (SORT_DISTINCT), -1 when none
 * of them has one (qo_get_col_product_ndv multiplies by 1 for an NDV of 0, and a product of 1 is -1).
 * The test sets the product per list. */
namespace
{
  INT64 g_ndv_groupby = -1, g_ndv_distinct = -1;
  int g_ndv_calls = 0;
}

INT64
qo_get_group_ndv (QO_PLAN * plan, SORT_TYPE sort_type)
{
  g_ndv_calls++;
  if (QO_ENV_PT_TREE (plan->info->env)->node_type != PT_SELECT)
    {
      return -1;
    }
  return sort_type == SORT_GROUPBY ? g_ndv_groupby : g_ndv_distinct;
}

/* qo_expr_selectivity (query_planner.c): the WHERE-term estimate of a predicate and its or_next list; the real one
 * QO_ASSERTs a PT_EXPR. The test sets the estimate and reads which predicates came, and whether their next and
 * or_next were cut when they came. */
namespace
{
  double g_where_sel = 0.3;
  std::vector < PT_NODE * >g_where_preds;
  bool g_where_non_expr = false;
  bool g_where_had_list = false;
}

double
qo_expr_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  g_where_preds.push_back (pt_expr);
  if (pt_expr == NULL || pt_expr->node_type != PT_EXPR)
    {
      g_where_non_expr = true;
    }
  else if (pt_expr->next != NULL || pt_expr->or_next != NULL)
    {
      g_where_had_list = true;
    }
  return g_where_sel;
}

/* ---------------------------------------------------------------- hooks.cpp (appended to query_planner.c) */
int unitseam_has_having (void);
int unitseam_having_flag_aggregate (void);
int unitseam_having_flag_groupby_num (void);
double unitseam_having_default (void);
int unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred);
double unitseam_having_estimate (int which, QO_ENV * env, PT_NODE * pred, double group_size, PT_NODE * group_attr,
				 double group_scale, int *aborted);
double unitseam_having_row_compare (PT_NODE * attr, DB_VALUE * value, bool is_ge, bool include_equal);
void unitseam_having_env_init (QO_PLAN * plan, double group_size, PT_NODE ** group_attr, double *group_scale,
			       double *out_group_size, int *aborted);
void unitseam_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type, int *aborted);

/* ---------------------------------------------------------------- the model, written out (oracle) */
namespace
{
  const double EQ_FLOOR = DEFAULT_EQUAL_SELECTIVITY;	/* 0.001 */
  const double HDEF = 0.5;	/* QO_HAVING_DEFAULT_SELECTIVITY */

  /* group sizes geometric with mean g (sizes 1, 2, ...): r = 1 - 1 / max (g, 1) */
  double geo_r (double g)
  {
    return 1.0 - 1.0 / std::max (g, 1.0);
  }

  /* E [s ^ size]: every row of the group satisfies a predicate of selectivity s */
  double every_row (double s, double g)
  {
    double r = geo_r (g);
    return s * (1.0 - r) / (1.0 - r * s);
  }

  /* 1 - E [(1 - s) ^ size]: some row of the group does */
  double some_row (double s, double g)
  {
    return 1.0 - every_row (1.0 - s, g);
  }

  /* P (size >= k) = r ^ (k - 1), 1 for k <= 1 */
  double size_at_least (double k, double g)
  {
    return k <= 1.0 ? 1.0 : pow (geo_r (g), k - 1.0);
  }

  double bounded (double p)
  {
    return std::min (std::max (p, EQ_FLOOR), 1.0);
  }

  /* NULL share f: the chance a group is not all NULL, and the non-NULL rows of such a group */
  double has_value (double f, double g)
  {
    return 1.0 - every_row (f, g);
  }

  double value_size (double f, double g)
  {
    return std::max (g * (1.0 - f) / has_value (f, g), 1.0);
  }

  /* the fake column x: share of non-NULL rows with x > c, x >= c, x < c, x <= c */
  double p_gt (double c)
  {
    return 1.0 - x_le (c);
  }

  double p_ge (double c)
  {
    return 1.0 - x_lt (c);
  }

  double p_lt (double c)
  {
    return x_lt (c);
  }

  double p_le (double c)
  {
    return x_le (c);
  }

  double or_sel (double a, double b)
  {
    return a + b - a * b;
  }

  void reset_probes ()
  {
    g_probes = g_probe_ok = 0;
    g_probe_lhs = NULL;
    g_probe_value_ptr = NULL;
    g_probe_value = 0.0;
    g_probe_type = DB_TYPE_NULL;
    g_probe_is_ge = g_probe_incl = false;
    g_where_preds.clear ();
    g_where_non_expr = g_where_had_list = false;
    g_gs_calls = g_rows_calls = g_colndv_calls = 0;
    g_gs_attr = NULL;
    g_gs_min_rows = g_gs_scale = 0.0;
    g_set_gets = 0;
  }

  bool boot ()
  {
    static bool done = false;
    if (!done)
      {
	done = true;
	lang_init_builtin ();
	/* lang_init () would also load user locales; lang_set_charset_lang () only asserts lang_Initialized */
	if (lang_set_charset_lang ("en_US.utf8") != NO_ERROR)
	  {
	    return false;
	  }
      }
    g_lo = 0.0;
    g_hi = 100.0;
    g_total = 1000000.0;
    g_eq = 0.0;
    g_x_i64 = false;
    g_blob_a = NULL;
    g_unexpected = 0;
    default_group_hist ();
    g_ndv_groupby = g_ndv_distinct = -1;
    g_ndv_calls = 0;
    g_where_sel = 0.3;
    g_er_id = NO_ERROR;
    g_er_sets = 0;
    g_sets.clear ();
    reset_probes ();
    return lang_charset () == INTL_CODESET_UTF8;
  }

  DB_VALUE vint (int v)
  {
    DB_VALUE d;
    db_make_int (&d, v);
    return d;
  }

  DB_VALUE vdbl (double v)
  {
    DB_VALUE d;
    db_make_double (&d, v);
    return d;
  }

  DB_VALUE vnull ()
  {
    DB_VALUE d;
    db_make_null (&d);
    return d;
  }

  const UINTPTR SPEC_T = 0x1001;	/* t: the FROM table of the block, a node of the environment */
  const UINTPTR SPEC_U = 0x2002;	/* u: only in subqueries, not joined (no node here) */

  /* one block `SELECT ... FROM t GROUP BY a HAVING ...` as the optimizer has it: one node (t), segments x (with a
   * histogram), y (without) and a (the GROUP BY column; a histogram only after annotate_a ()), each a PT_NAME the
   * annotation pass wrote the histogram and the NULL share on (qo_get_attr_info, query_graph.c:5509-5524); the names
   * in HAVING and GROUP BY are other PT_NAME nodes of the same column, not annotated. Nodes come from the real
   * parser_new_node. */
  struct scene
  {
    PARSER_CONTEXT *parser;
    QO_ENV *env;
    PT_NODE *spec_t;
    PT_NODE *seg_x, *seg_y, *seg_a;
    DB_VALUE blob;		/* stands for x's histogram blob; only its address is read */
    DB_VALUE blob_a;		/* a's */

    explicit scene (double null_frequency = 0.0)
    {
      parser = parser_create_parser ();
      parser->host_variables = (DB_VALUE *) calloc (4, sizeof (DB_VALUE));
      parser->host_var_count = 4;
      for (int i = 0; i < 4; i++)
	{
	  db_make_null (&parser->host_variables[i]);
	}
      db_make_null (&blob);
      db_make_null (&blob_a);

      spec_t = parser_new_node (parser, PT_SPEC);
      spec_t->info.spec.id = SPEC_T;
      spec_t->info.spec.entity_name = name ("t", 0);

      seg_x = name ("x", SPEC_T);
      seg_x->info.name.histogram = &blob;
      seg_x->info.name.null_frequency = null_frequency;
      seg_y = name ("y", SPEC_T);
      seg_a = name ("a", SPEC_T);

      env = (QO_ENV *) calloc (1, sizeof (QO_ENV));
      env->parser = parser;
      env->nnodes = env->Nnodes = 1;
      env->nodes = (QO_NODE *) calloc (1, sizeof (QO_NODE));
      QO_NODE_ENTITY_SPEC (&env->nodes[0]) = spec_t;
      PT_NODE *segs[3] = { seg_x, seg_y, seg_a };
      env->nsegs = env->Nsegs = 3;
      env->segs = (QO_SEGMENT *) calloc (3, sizeof (QO_SEGMENT));
      for (int i = 0; i < 3; i++)
	{
	  env->segs[i].env = env;
	  QO_SEG_PT_NODE (&env->segs[i]) = segs[i];
	  QO_SEG_HEAD (&env->segs[i]) = &env->nodes[0];
	}
    }

    ~scene ()
    {
      free (env->segs);
      free (env->nodes);
      free (env);
      parser_free_parser (parser);
    }

    /* the GROUP BY column a gets its histogram (the counts in g_a) */
    void annotate_a ()
    {
      seg_a->info.name.histogram = &blob_a;
      g_blob_a = &blob_a;
    }

    PT_NODE *name (const char *col, UINTPTR spec_id)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, col);
      n->info.name.resolved = "t";
      n->info.name.spec_id = spec_id;
      n->info.name.meta_class = PT_NORMAL;
      n->type_enum = PT_TYPE_INTEGER;
      return n;
    }

    /* a column of t as HAVING names it (not annotated) */
    PT_NODE *col (const char *c)
    {
      return name (c, SPEC_T);
    }

    PT_NODE *agg (FUNC_CODE f, PT_NODE * arg, PT_MISC_TYPE all_or_distinct = PT_ALL)
    {
      PT_NODE *n = parser_new_node (parser, PT_FUNCTION);
      n->info.function.function_type = f;
      n->info.function.all_or_distinct = all_or_distinct;
      n->info.function.arg_list = arg;
      n->type_enum = (f == PT_COUNT || f == PT_COUNT_STAR) ? PT_TYPE_INTEGER : PT_TYPE_DOUBLE;
      return n;
    }

    PT_NODE *count_star ()
    {
      return agg (PT_COUNT_STAR, NULL);
    }

    PT_NODE *groupby_num ()
    {
      PT_NODE *n = agg (PT_GROUPBY_NUM, NULL);
      n->type_enum = PT_TYPE_BIGINT;
      return n;
    }

    PT_NODE *ival (int v)
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_INTEGER;
      n->info.value.data_value.i = v;
      db_make_int (&n->info.value.db_value, v);
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    PT_NODE *dval (double v)
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_DOUBLE;
      n->info.value.data_value.d = v;
      db_make_double (&n->info.value.db_value, v);
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    /* a NUMERIC literal (2.5 is NUMERIC (2, 1) in CUBRID SQL), made by numeric_opfunc.c */
    PT_NODE *nval (const char *text)
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_NUMERIC;
      if (numeric_coerce_string_to_num (text, (int) strlen (text), INTL_CODESET_UTF8, &n->info.value.db_value)
	  != NO_ERROR)
	{
	  return NULL;
	}
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    PT_NODE *sval (const char *text)
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_VARCHAR;
      db_make_varchar (&n->info.value.db_value, 64, text, (int) strlen (text), INTL_CODESET_UTF8,
		       LANG_COLL_UTF8_BINARY);
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    /* the NULL literal: type_enum PT_TYPE_NULL */
    PT_NODE *null_literal ()
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_NULL;
      db_make_null (&n->info.value.db_value);
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    /* a folded logical constant */
    PT_NODE *logical (int v)
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_LOGICAL;
      n->info.value.data_value.i = v;
      db_make_int (&n->info.value.db_value, v);
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    PT_NODE *truth ()
    {
      return logical (1);
    }

    PT_NODE *hvar (int index, PT_MISC_TYPE var_type = PT_HOST_IN)
    {
      PT_NODE *n = parser_new_node (parser, PT_HOST_VAR);
      n->info.host_var.index = index;
      n->info.host_var.var_type = var_type;
      return n;
    }

    PT_NODE *ex (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->type_enum = (op == PT_PLUS || op == PT_ABS) ? PT_TYPE_DOUBLE : PT_TYPE_LOGICAL;
      return e;
    }

    /* AGG BETWEEN low AND high: PT_BETWEEN (agg, PT_BETWEEN_AND (low, high)) as the grammar builds it */
    PT_NODE *between (PT_NODE * a, PT_NODE * low, PT_NODE * high, PT_OP_TYPE op = PT_BETWEEN)
    {
      return ex (op, a, ex (PT_BETWEEN_AND, low, high));
    }

    /* AGG RANGE (item or_next item ...): the shape qo_convert_to_range builds for an attribute */
    PT_NODE *range (PT_NODE * a, std::initializer_list < PT_NODE * >items)
    {
      return ex (PT_RANGE, a, disj (items));
    }

    /* a constant IN list as the semantic check folds it: one set VALUE (PT_TYPE_SEQUENCE) with the DB_VALUE set
     * initialized, data_value.set holding the element nodes */
    PT_NODE *in_list (std::initializer_list < DB_VALUE > elems)
    {
      return in_list_v (std::vector < DB_VALUE > (elems));
    }

    PT_NODE *in_list_v (const std::vector < DB_VALUE > &elems)
    {
      g_sets.emplace_back (elems);
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_SEQUENCE;
      PT_NODE *head = NULL, *tail = NULL;
      for (const DB_VALUE & e:elems)
	{
	  PT_NODE *v = parser_new_node (parser, PT_VALUE);
	  v->type_enum = DB_IS_NULL (&e) ? PT_TYPE_NULL
	    : (DB_VALUE_DOMAIN_TYPE (&e) == DB_TYPE_DOUBLE ? PT_TYPE_DOUBLE : PT_TYPE_INTEGER);
	  v->info.value.db_value = e;
	  v->info.value.db_value_is_initialized = true;
	  (head == NULL ? head : tail->next) = v;
	  tail = v;
	}
      n->info.value.data_value.set = head;
      DB_VALUE *d = &n->info.value.db_value;
      db_make_null (d);
      d->domain.general_info.type = DB_TYPE_SEQUENCE;
      d->domain.general_info.is_null = 0;
      d->data.set = reinterpret_cast < DB_COLLECTION * >(&g_sets.back ());
      d->need_clear = false;
      n->info.value.db_value_is_initialized = true;
      return n;
    }

    /* (?, ?) as the IN list of host variables: a set function, not folded */
    PT_NODE *hv_list (int i1, int i2)
    {
      PT_NODE *f = parser_new_node (parser, PT_FUNCTION);
      f->info.function.function_type = F_SEQUENCE;
      f->type_enum = PT_TYPE_SEQUENCE;
      f->info.function.arg_list = hvar (i1);
      f->info.function.arg_list->next = hvar (i2);
      return f;
    }

    /* (SELECT f (u.z) FROM u): a subquery over a table this block does not join */
    PT_NODE *subquery (FUNC_CODE f)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      PT_NODE *spec_u = parser_new_node (parser, PT_SPEC);
      spec_u->info.spec.id = SPEC_U;
      spec_u->info.spec.entity_name = name ("u", 0);
      q->info.query.q.select.list = agg (f, f == PT_COUNT_STAR ? NULL : name ("z", SPEC_U));
      q->info.query.q.select.from = spec_u;
      return q;
    }

    /* a CNF list of conjuncts (next) / a disjunction inside one conjunct (or_next) */
    static PT_NODE *conj (std::initializer_list < PT_NODE * >terms)
    {
      PT_NODE *head = NULL, *tail = NULL;
      for (PT_NODE * t:terms)
	{
	  (head == NULL ? head : tail->next) = t;
	  tail = t;
	}
      return head;
    }

    static PT_NODE *disj (std::initializer_list < PT_NODE * >terms)
    {
      PT_NODE *head = NULL, *tail = NULL;
      for (PT_NODE * t:terms)
	{
	  (head == NULL ? head : tail->or_next) = t;
	  tail = t;
	}
      return head;
    }

    double est (int which, PT_NODE * pred, double group_size, PT_NODE * group_attr, double group_scale)
    {
      int aborted = 0;
      double s = unitseam_having_estimate (which, env, pred, group_size, group_attr, group_scale, &aborted);
      CHECK (aborted == 0);
      return s;
    }

    /* qo_having_aggregate_term (): raw, -1 when the form is not priced */
    double term (PT_NODE * pred, double group_size, PT_NODE * group_attr = NULL, double group_scale = 1.0)
    {
      return est (0, pred, group_size, group_attr, group_scale);
    }

    /* qo_having_disjunct_selectivity (): one predicate as HAVING prices it -- [0.001, 1], 0.5 when not priced */
    double agg_sel (PT_NODE * pred, double group_size, PT_NODE * group_attr = NULL, double group_scale = 1.0)
    {
      return est (1, pred, group_size, group_attr, group_scale);
    }

    /* qo_having_pred_selectivity (): a predicate and its or_next disjuncts */
    double pred_sel (PT_NODE * pred, double group_size, PT_NODE * group_attr = NULL, double group_scale = 1.0)
    {
      return est (2, pred, group_size, group_attr, group_scale);
    }

    /* qo_having_selectivity (): the conjuncts */
    double having_sel (PT_NODE * having, double group_size, PT_NODE * group_attr = NULL, double group_scale = 1.0)
    {
      return est (3, having, group_size, group_attr, group_scale);
    }
  };

  /* plan->info as qo_sort_new () hands it to qo_estimate_ngroups (): the subplan's info, shared by the sort plans
   * over it, group_rows starting at the cardinality (qo_alloc_info). GROUP BY a is a PT_SORT_SPEC (csql_grammar.y
   * group_spec). */
  struct grouping
  {
    scene & s;
    PT_NODE *select;
    QO_INFO *info;
    QO_PLAN *plan;

    grouping (scene & sc, double cardinality, double total_rows, PT_NODE * having):s (sc)
    {
      select = parser_new_node (s.parser, PT_SELECT);
      select->info.query.q.select.from = s.spec_t;
      select->info.query.q.select.group_by = sort_spec (s.col ("a"));
      select->info.query.q.select.having = having;
      s.env->pt_tree = select;
      info = (QO_INFO *) calloc (1, sizeof (QO_INFO));
      info->env = s.env;
      info->cardinality = cardinality;
      info->total_rows = total_rows;
      info->group_rows = cardinality;
      plan = (QO_PLAN *) calloc (1, sizeof (QO_PLAN));
      plan->info = info;
    }

    ~grouping ()
    {
      free (plan);
      free (info);
    }

    PT_NODE *sort_spec (PT_NODE * expr)
    {
      PT_NODE *n = parser_new_node (s.parser, PT_SORT_SPEC);
      n->info.sort_spec.expr = expr;
      n->info.sort_spec.asc_or_desc = PT_ASC;
      return n;
    }

    double run (SORT_TYPE sort_type)
    {
      int aborted = 0;
      unitseam_estimate_ngroups (plan, sort_type, &aborted);
      CHECK (aborted == 0);
      return info->group_rows;
    }
  };

  bool on_this_revision ()
  {
    return unitseam_has_having () == 1;
  }
}

#define NEEDS_HAVING() \
  do { \
    INFO ("the HAVING estimate of 5cfef7f5b (qo_having_aggregate_term ... over QO_HAVING_ENV) is not on this " \
	  "revision (new in PR#8325)"); \
    REQUIRE (on_this_revision ()); \
  } while (0)

/* ================================================================ A: one aggregate predicate */

TEST_CASE ("PR8325 A1: control -- AVG (x) op c is the histogram selectivity of x op c, probed on the segment's "
	   "annotated name (the HAVING name is not annotated), whatever the group size", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (unitseam_having_default () == HDEF);
  for (double g : { 1.0, 10.0, 1000.0 })
    {
      PT_NODE *attr = s.col ("x");
      reset_probes ();
      CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, attr), s.ival (25)), g) == Approx (0.75));
      CHECK (g_probes == 1);
      CHECK (g_probe_lhs == s.seg_x);
      CHECK (g_probe_type == DB_TYPE_INTEGER);
      CHECK (g_probe_value == 25.0);
      CHECK (g_probe_is_ge);
      CHECK_FALSE (g_probe_incl);
      CHECK (attr->info.name.histogram == NULL);
    }

  reset_probes ();
  CHECK (s.agg_sel (s.ex (PT_GE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (0.75));
  CHECK ((g_probe_is_ge && g_probe_incl));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (0.25));
  CHECK ((!g_probe_is_ge && !g_probe_incl));
  CHECK (s.agg_sel (s.ex (PT_LE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (0.25));
  CHECK ((!g_probe_is_ge && g_probe_incl));
  CHECK (s.term (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (0.75));
  CHECK (g_unexpected == 0);
}

TEST_CASE ("PR8325 A2: c op AGG (x) is AGG (x) with the operator reversed", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  /* 25 < AVG (x) = AVG (x) > 25 */
  CHECK (s.agg_sel (s.ex (PT_LT, s.ival (25), s.agg (PT_AVG, s.col ("x"))), 10.0) == Approx (0.75));
  CHECK ((g_probe_is_ge && !g_probe_incl && g_probe_lhs == s.seg_x));
  /* 25 <= AVG (x) = AVG (x) >= 25 */
  CHECK (s.agg_sel (s.ex (PT_LE, s.ival (25), s.agg (PT_AVG, s.col ("x"))), 10.0) == Approx (0.75));
  CHECK ((g_probe_is_ge && g_probe_incl));
  /* 25 > AVG (x) = AVG (x) < 25 */
  CHECK (s.agg_sel (s.ex (PT_GT, s.ival (25), s.agg (PT_AVG, s.col ("x"))), 10.0) == Approx (0.25));
  CHECK ((!g_probe_is_ge && !g_probe_incl));
  /* 25 >= AVG (x) = AVG (x) <= 25 */
  CHECK (s.agg_sel (s.ex (PT_GE, s.ival (25), s.agg (PT_AVG, s.col ("x"))), 10.0) == Approx (0.25));
  CHECK ((!g_probe_is_ge && g_probe_incl));
  /* 500 < SUM (x) = SUM (x) > 500, the constant divided by the group size as for the plain form */
  CHECK (s.agg_sel (s.ex (PT_LT, s.ival (500), s.agg (PT_SUM, s.col ("x"))), 10.0) == Approx (0.5));
  CHECK (g_probe_value == Approx (50.0));
  /* 100 < COUNT (*) = COUNT (*) > 100 */
  CHECK (s.agg_sel (s.ex (PT_LT, s.ival (100), s.count_star ()), 100.0) == Approx (size_at_least (101, 100.0)));
}

TEST_CASE ("PR8325 A3: SUM (x) op c probes x op c / group size as a DOUBLE (the average row of the group); a group "
	   "size below 1 is taken as 1", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500)), 10.0) == Approx (0.5));
  CHECK (g_probe_type == DB_TYPE_DOUBLE);
  CHECK (g_probe_value == Approx (50.0));
  CHECK (g_probe_lhs == s.seg_x);

  CHECK (s.agg_sel (s.ex (PT_LE, s.agg (PT_SUM, s.col ("x")), s.ival (200)), 8.0) == Approx (0.25));
  CHECK (g_probe_value == Approx (25.0));
  CHECK ((!g_probe_is_ge && g_probe_incl));

  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (50)), 1.0) == Approx (0.5));
  CHECK (g_probe_value == Approx (50.0));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (50)), 0.5) == Approx (0.5));
  CHECK (g_probe_value == Approx (50.0));
  /* a size of 0 is the unknown size: SUM still reads it as one-row groups */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (50)), 0.0) == Approx (0.5));
  CHECK (g_probe_value == Approx (50.0));
}

TEST_CASE ("PR8325 A4: MAX (x) > c and MIN (x) < c -- some row of the group passes; MAX (x) < c and MIN (x) > c -- "
	   "every row does; group sizes geometric with the given mean; a group of one row is the row",
	   "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;

  /* x > 90 and x < 10 are 0.1 of the rows, x < 90 and x > 10 are 0.9 */
  double max_gt = s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), g);
  CHECK (max_gt == Approx (some_row (0.1, g)));
  CHECK (max_gt == Approx (1.0 - 0.9 * 0.1 / 0.19));
  CHECK ((g_probe_is_ge && g_probe_lhs == s.seg_x));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_MIN, s.col ("x")), s.ival (10)), g) == Approx (some_row (0.1, g)));
  CHECK (!g_probe_is_ge);
  double max_lt = s.agg_sel (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), g);
  CHECK (max_lt == Approx (every_row (0.9, g)));
  CHECK (max_lt == Approx (0.9 * 0.1 / 0.19));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MIN, s.col ("x")), s.ival (10)), g) == Approx (every_row (0.9, g)));
  /* MAX (x) > 90 and MAX (x) <= 90 split the groups */
  CHECK (max_gt + s.agg_sel (s.ex (PT_LE, s.agg (PT_MAX, s.col ("x")), s.ival (90)), g) == Approx (1.0));
  /* DISTINCT does not change MIN / MAX */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x"), PT_DISTINCT), s.ival (90)), g) == Approx (max_gt));

  /* one-row groups: MIN = MAX = the row */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 1.0) == Approx (0.1));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MIN, s.col ("x")), s.ival (10)), 1.0) == Approx (0.9));

  /* larger groups: more of them have some row above 90, fewer have every row below 90 */
  double prev_some = 0.0, prev_every = 1.0;
  for (double size : { 1.0, 2.0, 10.0, 100.0 })
    {
      double some = s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), size);
      double every = s.agg_sel (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), size);
      CHECK (some >= prev_some);
      CHECK (every <= prev_every);
      prev_some = some;
      prev_every = every;
    }
}

TEST_CASE ("PR8325 A5: COUNT (*) op c without a GROUP BY histogram is the group size against c, geometric with the "
	   "given mean (P (size >= k) = r^(k-1)) -- no histogram probe, a host variable read like a constant",
	   "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 100.0;	/* r = 0.99 */

  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (100)), g) == Approx (pow (0.99, 100)));
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (100)), g) == Approx (pow (0.99, 99)));
  CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), s.ival (100)), g) == Approx (1.0 - pow (0.99, 99)));
  CHECK (s.agg_sel (s.ex (PT_LE, s.count_star (), s.ival (100)), g) == Approx (1.0 - pow (0.99, 100)));
  CHECK (g_probes == 0);

  db_make_int (&s.parser->host_variables[0], 100);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.hvar (0)), g) == Approx (pow (0.99, 100)));
  CHECK (g_probes == 0);
  CHECK (g_gs_calls == 0);
}

TEST_CASE ("PR8325 A6: COUNT (*) against a non-integer or a small constant -- the count is an integer (k = floor (c) "
	   "+ 1 for > and <=, ceil (c) for >= and <); c <= 1 keeps every group, < 1 none (the floor)",
	   "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;	/* r = 0.9 */

  /* 2.5e0 (DOUBLE) and 2.5 (NUMERIC): size > 2.5 = size >= 2.5 = size >= 3 */
  for (int numeric = 0; numeric < 2; numeric++)
    {
      auto c = [&] ()
      {
	return numeric ? s.nval ("2.5") : s.dval (2.5);
      };
      REQUIRE (c () != NULL);
      CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), c ()), g) == Approx (0.81));
      CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), c ()), g) == Approx (0.81));
      CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), c ()), g) == Approx (0.19));
      CHECK (s.agg_sel (s.ex (PT_LE, s.count_star (), c ()), g) == Approx (0.19));
    }

  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (0)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (1)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (-3)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), s.ival (1)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LE, s.count_star (), s.ival (0)), g) == Approx (EQ_FLOOR));

  /* groups known to be one row each (size 1, not the unknown size 0 -- that is A17) */
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (1)), 1.0) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (1)), 1.0) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_LE, s.count_star (), s.ival (1)), 1.0) == 1.0);
  CHECK (g_probes == 0);
}

TEST_CASE ("PR8325 A7: a NULL share f -- an all-NULL group fails (has_value = 1 - f(1-r)/(1-rf)), the others "
	   "aggregate their non-NULL rows (value_size = g(1-f)/has_value); one-row groups give the WHERE estimate p(1-f)",
	   "[contract][aggregate][null]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();

  {
    scene s (0.2);
    /* one-row groups: x > 25 over all rows is 0.75 of the non-NULL 80% */
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 1.0) == Approx (0.75 * 0.8));
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 1.0) == Approx (0.1 * 0.8));
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (25)), 1.0) == Approx (0.75 * 0.8));
    CHECK (g_probe_value == Approx (25.0));

    /* groups of 10: has_value = 0.8 / 0.82, value_size = 8 / has_value = 8.2 */
    const double hv = has_value (0.2, 10.0), vs = value_size (0.2, 10.0);
    REQUIRE (hv == Approx (0.8 / 0.82));
    REQUIRE (vs == Approx (8.2));
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (0.75 * hv));
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (410)), 10.0) == Approx (0.5 * hv));
    CHECK (g_probe_value == Approx (50.0));	/* 410 / 8.2: the non-NULL rows of a group, not its 10 rows */
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 10.0)
	   == Approx (some_row (0.1, vs) * hv));
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MIN, s.col ("x")), s.ival (10)), 10.0)
	   == Approx (every_row (0.9, vs) * hv));
    /* COUNT (*) counts the rows, NULL or not */
    CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (100)), 100.0) == Approx (pow (0.99, 100)));
  }

  {
    /* every row NULL: no group has a value, no probe */
    scene s (1.0);
    reset_probes ();
    CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (EQ_FLOOR));
    CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 1.0) == Approx (EQ_FLOOR));
    CHECK (g_probes == 0);
  }

  {
    /* a share outside [0, 1] is clamped */
    scene lo (-0.5), hi (1.5);
    CHECK (lo.agg_sel (lo.ex (PT_GT, lo.agg (PT_AVG, lo.col ("x")), lo.ival (25)), 10.0) == Approx (0.75));
    CHECK (hi.agg_sel (hi.ex (PT_GT, hi.agg (PT_AVG, hi.col ("x")), hi.ival (25)), 10.0) == Approx (EQ_FLOOR));
  }
}

TEST_CASE ("PR8325 A8: the priced disjunct lies in [DEFAULT_EQUAL_SELECTIVITY, 1] -- an empty range, every-row of a "
	   "large group, a full range", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  /* the histogram's own floor is 1 / total_rows = 1e-6 */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (100)), 10.0) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (0)), 10.0) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 1000000.0) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (100000)), 10.0) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (-10)), 10.0) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.ival (-10)), 10.0) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MIN, s.col ("x")), s.ival (-10)), 10.0) == 1.0);
  /* the raw term is not floored -- the floor is the disjunct's */
  CHECK (s.term (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.ival (90)), 1000000.0) < EQ_FLOOR);
}

TEST_CASE ("PR8325 A9: `AGG (x) op ?` reads the input host variable; a NULL value or an output host variable is not "
	   "a priced form (term -1, the disjunct 0.5)", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  db_make_int (&s.parser->host_variables[0], 25);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (0)), 10.0) == Approx (0.75));
  CHECK (g_probe_value_ptr == &s.parser->host_variables[0]);
  CHECK (g_probe_value == 25.0);

  reset_probes ();
  db_make_null (&s.parser->host_variables[1]);
  CHECK (s.term (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (1)), 10.0) == -1.0);
  CHECK (s.term (s.ex (PT_GT, s.count_star (), s.hvar (1)), 10.0) == -1.0);
  CHECK (s.term (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (0, PT_HOST_OUT)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (1)), 10.0) == HDEF);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (0, PT_HOST_OUT)), 10.0) == HDEF);
  CHECK (g_probes == 0);
}

TEST_CASE ("PR8325 A10: any other shape is not a priced form (term -1) and the disjunct takes "
	   "QO_HAVING_DEFAULT_SELECTIVITY 0.5 -- no or two aggregates, COUNT (x) and the others, DISTINCT SUM / AVG, an "
	   "expression argument, a column without a histogram or outside the block, the NULL literal, a subquery, a key "
	   "neither the histogram nor a number takes (5cfef7f5b: =, <> and COUNT (*) = c are priced now -- A11; OR is "
	   "combined -- H8; the default was DEFAULT_SELECTIVITY 0.1)", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  auto unpriced = [&] (PT_NODE * pred)
  {
    PT_NODE *copy_next = pred->next;
    double t = s.term (pred, 10.0);
    double d = s.agg_sel (pred, 10.0);
    CHECK (pred->next == copy_next);
    return t == -1.0 && d == HDEF;
  };

  CHECK (unpriced (s.ex (PT_GT, s.col ("x"), s.ival (25))) == false);	/* no aggregate: a WHERE term (H2) */
  CHECK (s.term (s.ex (PT_GT, s.col ("x"), s.ival (25)), 10.0) == -1.0);
  CHECK (s.term (s.ex (PT_LT, s.ival (25), s.col ("x")), 10.0) == -1.0);
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.agg (PT_MIN, s.col ("x")))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_STDDEV, s.col ("x")), s.ival (3))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x"), PT_DISTINCT), s.ival (500))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x"), PT_DISTINCT), s.ival (25))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.ex (PT_PLUS, s.col ("x"), s.ival (1))), s.ival (25))));
  CHECK (unpriced (s.ex (PT_LIKE, s.agg (PT_MAX, s.col ("x")), s.sval ("1%"))));
  CHECK (unpriced (s.ex (PT_GT, s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)), s.ival (500))));
  CHECK (g_probes == 0);
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("y")), s.ival (25))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.name ("z", SPEC_U)), s.ival (25))));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.null_literal ())));
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.subquery (PT_MAX))));
  CHECK (unpriced (s.ex (PT_GT, s.count_star (), s.subquery (PT_COUNT_STAR))));
  CHECK (g_probes == 0);
  /* reaches the histogram, which cannot take a string key on a numeric column, nor is 'abc' a number */
  CHECK (unpriced (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.sval ("abc"))));
  CHECK (g_probes == 2);
  CHECK (g_probe_ok == 0);
}

TEST_CASE ("PR8325 A11: AGG = c is P (AGG >= c) - P (AGG > c), AGG <> c its complement, c = AGG the same; COUNT (*) = "
	   "c from the integer split; an equality the mapping gives 0 is floored", "[contract][aggregate][equal]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;	/* r = 0.9 */

  /* the continuous column: x = 25 has no mass, the mapping gives 0 -> the floor; <> keeps every group */
  CHECK (s.term (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), g) == 0.0);
  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_NE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), g) == 1.0);

  /* a point mass of 0.02 at every value */
  g_eq = 0.02;
  reset_probes ();
  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), g) == Approx (0.02));
  CHECK (g_probes == 2);	/* >= and > */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.ival (25), s.agg (PT_AVG, s.col ("x"))), g) == Approx (0.02));
  CHECK (s.agg_sel (s.ex (PT_NE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), g) == Approx (0.98));
  CHECK (s.agg_sel (s.ex (PT_NE, s.ival (25), s.agg (PT_AVG, s.col ("x"))), g) == Approx (0.98));
  /* SUM (x) = 500 over groups of 10: the average row x = 50 */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_SUM, s.col ("x")), s.ival (500)), g) == Approx (0.02));
  CHECK (g_probe_value == Approx (50.0));
  /* MAX (x) = 90: some row >= 90 but none > 90 */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_MAX, s.col ("x")), s.ival (90)), g)
	 == Approx (some_row (p_ge (90), g) - some_row (p_gt (90), g)));
  /* MIN (x) = 10: every row >= 10 but not every row > 10 */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_MIN, s.col ("x")), s.ival (10)), g)
	 == Approx (every_row (p_ge (10), g) - every_row (p_gt (10), g)));

  /* COUNT (*) = 3: P (size >= 3) - P (size >= 4) = r^2 - r^3; = 1 is 1 - r; a fraction or 0 is never a count */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (3)), g) == Approx (0.81 - 0.729));
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (1)), g) == Approx (0.1));
  CHECK (s.agg_sel (s.ex (PT_NE, s.count_star (), s.ival (1)), g) == Approx (0.9));
  CHECK (s.term (s.ex (PT_EQ, s.count_star (), s.dval (2.5)), g) == 0.0);
  CHECK (s.term (s.ex (PT_EQ, s.count_star (), s.ival (0)), g) == 0.0);
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (0)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_NE, s.count_star (), s.ival (0)), g) == 1.0);
}

TEST_CASE ("PR8325 A12: AGG BETWEEN low AND high is P (AGG >= low) + P (AGG <= high) - 1 (no NULLs), NOT BETWEEN "
	   "its complement; reversed bounds keep no group; a bound that is not a constant is not priced",
	   "[contract][aggregate][between]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;	/* r = 0.9 */

  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75)), g) == Approx (0.5));
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75), PT_NOT_BETWEEN), g)
	 == Approx (0.5));
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (10), s.ival (30)), g) == Approx (0.2));
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (10), s.ival (30), PT_NOT_BETWEEN), g)
	 == Approx (0.8));
  /* SUM (x) BETWEEN 250 AND 750 over groups of 10: the average row in [25, 75] */
  CHECK (s.agg_sel (s.between (s.agg (PT_SUM, s.col ("x")), s.ival (250), s.ival (750)), g) == Approx (0.5));
  /* MAX (x) BETWEEN 80 AND 90 = P (MAX >= 80) - P (MAX > 90) */
  CHECK (s.agg_sel (s.between (s.agg (PT_MAX, s.col ("x")), s.ival (80), s.ival (90)), g)
	 == Approx (some_row (0.2, g) - some_row (0.1, g)));
  /* MIN (x) BETWEEN 10 AND 20 = P (MIN >= 10) - P (MIN > 20) */
  CHECK (s.agg_sel (s.between (s.agg (PT_MIN, s.col ("x")), s.ival (10), s.ival (20)), g)
	 == Approx (every_row (0.9, g) - every_row (0.8, g)));
  /* COUNT (*) BETWEEN 2 AND 3 = P (2 <= size <= 3) = r - r^3 */
  CHECK (s.agg_sel (s.between (s.count_star (), s.ival (2), s.ival (3)), g) == Approx (0.9 - 0.729));
  CHECK (g_unexpected == 0);

  /* reversed bounds: none, NOT BETWEEN all */
  CHECK (s.term (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (75), s.ival (25)), g) == 0.0);
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (75), s.ival (25)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (75), s.ival (25), PT_NOT_BETWEEN), g) == 1.0);

  /* a host variable bound is read; a NULL literal, a column or a subquery bound is not priced; nor a non-aggregate
   * left side */
  db_make_int (&s.parser->host_variables[0], 75);
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.hvar (0)), g) == Approx (0.5));
  CHECK (s.term (s.between (s.agg (PT_AVG, s.col ("x")), s.null_literal (), s.ival (75)), g) == -1.0);
  CHECK (s.term (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.col ("y")), g) == -1.0);
  CHECK (s.term (s.between (s.agg (PT_AVG, s.col ("x")), s.subquery (PT_MIN), s.ival (75)), g) == -1.0);
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.subquery (PT_MIN), s.ival (75)), g) == HDEF);
  CHECK (s.term (s.between (s.ival (25), s.agg (PT_MIN, s.col ("x")), s.agg (PT_MAX, s.col ("x"))), g) == -1.0);
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("y")), s.ival (25), s.ival (75)), g) == HDEF);
}

TEST_CASE ("PR8325 A13: AGG IN (constants) adds P (AGG = v) over the folded list, capped at 1, a NULL element adds 0; "
	   "NOT IN is the complement; one element the histogram cannot price, a list of host variables or a subquery is "
	   "not priced", "[contract][aggregate][in]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;	/* r = 0.9 */

  /* COUNT (*) IN (1, 3): (1 - r) + (r^2 - r^3) */
  const double eq1 = 0.1, eq3 = 0.81 - 0.729;
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.count_star (), s.in_list ({vint (1), vint (3)})), g) == Approx (eq1 + eq3));
  CHECK (s.agg_sel (s.ex (PT_IS_NOT_IN, s.count_star (), s.in_list ({vint (1), vint (3)})), g)
	 == Approx (1.0 - eq1 - eq3));
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.count_star (), s.in_list ({vint (1), vnull (), vint (3)})), g)
	 == Approx (eq1 + eq3));
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.count_star (), s.in_list ({vdbl (3.0)})), g) == Approx (eq3));
  /* AVG (x) IN (25, 50) with a point mass of 0.02 */
  g_eq = 0.02;
  /* 60 distinct values of 0.02 each: the sum stops at 1 */
  {
    std::vector < DB_VALUE > sixty;
    for (int v = 0; v < 60; v++)
      {
	sixty.push_back (vint (v));
      }
    CHECK (s.term (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("x")), s.in_list_v (sixty)), g) == 1.0);
  }
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("x")), s.in_list ({vint (25), vint (50)})), g)
	 == Approx (0.04));
  CHECK (s.agg_sel (s.ex (PT_IS_NOT_IN, s.agg (PT_AVG, s.col ("x")), s.in_list ({vint (25), vint (50)})), g)
	 == Approx (0.96));
  /* a NULL element adds 0 here too (priced as a value, NULL would leave the whole list unpriced) */
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("x")), s.in_list ({vint (25), vnull (), vint (50)})), g)
	 == Approx (0.04));

  /* not priced: an element the histogram cannot take, a host-variable list, a subquery, an aggregate without a
   * histogram */
  CHECK (s.term (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("y")), s.in_list ({vint (25), vint (50)})), g) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("y")), s.in_list ({vint (25), vint (50)})), g) == HDEF);
  CHECK (s.agg_sel (s.ex (PT_IS_NOT_IN, s.agg (PT_AVG, s.col ("y")), s.in_list ({vint (25)})), g) == HDEF);
  db_make_int (&s.parser->host_variables[0], 1);
  db_make_int (&s.parser->host_variables[1], 3);
  CHECK (s.term (s.ex (PT_IS_IN, s.count_star (), s.hv_list (0, 1)), g) == -1.0);
  CHECK (s.term (s.ex (PT_IS_IN, s.count_star (), s.subquery (PT_COUNT_STAR)), g) == -1.0);
  CHECK (s.term (s.ex (PT_IS_IN, s.ival (3), s.in_list ({vint (1), vint (3)})), g) == -1.0);
}

TEST_CASE ("PR8325 A14: AGG RANGE (items) adds its items -- = v, two-sided and open ranges (an open low end has its "
	   "bound in arg1), capped at 1; an item not priced leaves the whole unpriced (no producer found: "
	   "qo_convert_to_range takes an attribute left side only)", "[contract][aggregate][range]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;

  g_eq = 0.02;
  PT_NODE *avg = NULL;
  auto A = [&] ()
  {
    return avg = s.agg (PT_AVG, s.col ("x"));
  };
  auto it = [&] (PT_OP_TYPE op, PT_NODE * v1, PT_NODE * v2 = NULL)
  {
    return s.ex (op, v1, v2);
  };

  /* = 25 */
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_EQ_NA, s.ival (25))}), g) == Approx (0.02));
  /* [25, 50] + (60, 70) + [80, 90) */
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_GE_LE, s.ival (25), s.ival (50)),
					   it (PT_BETWEEN_GT_LT, s.ival (60), s.ival (70)),
					   it (PT_BETWEEN_GE_LT, s.ival (80), s.ival (90))}), g)
	 == Approx ((p_ge (25) + p_le (50) - 1) + (p_gt (60) + p_lt (70) - 1) + (p_ge (80) + p_lt (90) - 1)));
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_GT_LE, s.ival (25), s.ival (50))}), g)
	 == Approx (p_gt (25) + p_le (50) - 1));
  /* open ends: >= 90, > 90, <= 10 and < 10 (the bound in arg1) */
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_GE_INF, s.ival (90))}), g) == Approx (p_ge (90)));
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_GT_INF, s.ival (90))}), g) == Approx (p_gt (90)));
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_INF_LE, s.ival (10))}), g) == Approx (p_le (10)));
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_INF_LT, s.ival (10))}), g) == Approx (p_lt (10)));
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_INF_LT, s.ival (10)), it (PT_BETWEEN_GE_INF, s.ival (90))}), g)
	 == Approx (p_lt (10) + p_ge (90)));
  /* <= 60 and >= 40 overlap: capped at 1 */
  CHECK (s.term (s.range (A (), {it (PT_BETWEEN_INF_LE, s.ival (60)), it (PT_BETWEEN_GE_INF, s.ival (40))}), g)
	 == 1.0);

  /* not priced: an item bound that is not a constant, an operator a range does not hold, an item that is not an
   * expression, a non-aggregate left side */
  CHECK (s.term (s.range (A (), {it (PT_BETWEEN_EQ_NA, s.ival (25)), it (PT_BETWEEN_GE_LE, s.ival (25), s.col ("y"))}),
		 g) == -1.0);
  CHECK (s.term (s.range (A (), {it (PT_EQ, s.ival (25), s.ival (25))}), g) == -1.0);
  CHECK (s.term (s.range (A (), {s.ival (25)}), g) == -1.0);
  CHECK (s.term (s.range (s.col ("x"), {it (PT_BETWEEN_EQ_NA, s.ival (25))}), g) == -1.0);
  CHECK (s.agg_sel (s.range (A (), {it (PT_BETWEEN_GE_LE, s.ival (25), s.col ("y"))}), g) == HDEF);
}

TEST_CASE ("PR8325 A15: a constant the histogram cannot take that lies beyond +-9e18 is past every value -- x > c none, "
	   "x < c all (and mirrored below); a constant inside that range or not a number stays unpriced; aggregates map "
	   "it as any p", "[contract][aggregate][out_of_range]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  g_x_i64 = true;		/* x is an INTEGER column: a DOUBLE key beyond +-9e18 is not a key */

  DB_VALUE big, small, mid, word, num_word, small_word;
  db_make_double (&big, 1e19);
  db_make_double (&small, -1e19);
  db_make_double (&mid, 5e18);
  db_make_varchar (&word, 64, "abc", 3, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_varchar (&num_word, 64, "1e30", 4, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_varchar (&small_word, 64, "5", 1, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);

  reset_probes ();
  CHECK (unitseam_having_row_compare (s.seg_x, &big, true, false) == 0.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &big, true, true) == 0.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &big, false, false) == 1.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &small, true, false) == 1.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &small, false, true) == 0.0);
  CHECK (g_probes == 5);
  CHECK (g_probe_ok == 0);
  /* inside the INTEGER range the histogram prices it; a word that is no number, or a number inside the range the
   * histogram did not take, is not priced; a numeric word beyond it is */
  CHECK (unitseam_having_row_compare (s.seg_x, &mid, true, false) == Approx (1.0 / g_total));
  CHECK (g_probe_ok == 1);
  CHECK (unitseam_having_row_compare (s.seg_x, &word, true, false) == -1.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &small_word, true, false) == -1.0);
  CHECK (unitseam_having_row_compare (s.seg_x, &num_word, true, false) == 0.0);

  const double g = 10.0;
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.dval (1e19)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.dval (1e19)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GE, s.agg (PT_AVG, s.col ("x")), s.dval (-1e19)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MAX, s.col ("x")), s.dval (1e19)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_MAX, s.col ("x")), s.dval (1e19)), g) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_MIN, s.col ("x")), s.dval (-1e19)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_MIN, s.col ("x")), s.dval (-1e19)), g) == 1.0);
  /* SUM (x) > 1e30: the average row past 1e29 */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.dval (1e30)), g) == Approx (EQ_FLOOR));
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_SUM, s.col ("x")), s.dval (1e30)), g) == 1.0);
  /* a NUMERIC literal of 20 digits */
  PT_NODE *n20 = s.nval ("99999999999999999999");
  REQUIRE (n20 != NULL);
  CHECK (s.agg_sel (s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), n20), g) == 1.0);
  /* BETWEEN with one bound past the values: the other bound alone */
  CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.dval (1e19)), g) == Approx (0.75));
  CHECK (s.agg_sel (s.ex (PT_NE, s.agg (PT_AVG, s.col ("x")), s.dval (1e19)), g) == 1.0);
  CHECK (g_probe_ok == 2);	/* only `mid` and the 25 bound of the BETWEEN were the histogram's */

  /* with a NULL share the all-NULL groups still fail */
  {
    scene n (0.2);
    CHECK (n.agg_sel (n.ex (PT_LT, n.agg (PT_AVG, n.col ("x")), n.dval (1e19)), g) == Approx (has_value (0.2, g)));
  }
}

TEST_CASE ("PR8325 A16: COUNT (*) op c with the GROUP BY column's histogram -- P (size >= k) from its value counts "
	   "(histogram_get_group_size_selectivity on the segment's annotated name, k as for the geometric model, the "
	   "scale passed on); not found = not priced", "[contract][aggregate][count]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  s.annotate_a ();
  const double g = 100.0;

  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (600)), g, s.seg_a, 1.0) == Approx (group_sel (601, 1.0)));
  CHECK (g_gs_calls == 1);
  CHECK (g_gs_attr == s.seg_a);
  CHECK (g_gs_min_rows == 601.0);
  CHECK (g_gs_scale == 1.0);
  /* two of the hundred values (the MCVs of 2000 and 1000 rows) and a sliver of the bucket: not the geometric 0.99^600 */
  CHECK (group_sel (601, 1.0) == Approx (0.02).epsilon (0.01));
  CHECK (pow (0.99, 600) < 0.003);
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (600)), g, s.seg_a, 1.0) == Approx (group_sel (600, 1.0)));
  CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), s.ival (600)), g, s.seg_a, 1.0)
	 == Approx (1.0 - group_sel (600, 1.0)));
  CHECK (s.agg_sel (s.ex (PT_LE, s.count_star (), s.ival (600)), g, s.seg_a, 1.0)
	 == Approx (1.0 - group_sel (601, 1.0)));
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.dval (599.5)), g, s.seg_a, 1.0) == Approx (group_sel (600, 1.0)));
  CHECK (g_gs_min_rows == 600.0);
  /* COUNT (*) = 2000: the one value with 2000 rows (and the bucket's share of exactly 2000) */
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (2000)), g, s.seg_a, 1.0)
	 == Approx (group_sel (2000, 1.0) - group_sel (2001, 1.0)));
  CHECK (group_sel (2000, 1.0) - group_sel (2001, 1.0) == Approx (0.01).epsilon (0.01));
  /* the scale moves the counts: half of them */
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (600)), g, s.seg_a, 0.5) == Approx (group_sel (601, 0.5)));
  CHECK (g_gs_scale == 0.5);
  /* k <= 1 needs no histogram */
  reset_probes ();
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (1)), g, s.seg_a, 1.0) == 1.0);
  CHECK (g_gs_calls == 0);
  /* the histogram does not answer (scale <= 0 is one of its exits): not priced, no fall back to the geometric model */
  CHECK (s.term (s.ex (PT_GT, s.count_star (), s.ival (600)), g, s.seg_a, 0.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (600)), g, s.seg_a, 0.0) == HDEF);
  CHECK (g_gs_calls == 2);
  CHECK (g_probes == 0);
  CHECK (g_unexpected == 0);
}

TEST_CASE ("PR8325 A17: COUNT (*) when the group size is unknown (0) and no GROUP BY histogram -- not priced (0.5); "
	   "c <= 1 still keeps every group (5cfef7f5b: was one row a group, COUNT (*) > 1 at the floor)",
	   "[contract][aggregate][count]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.term (s.ex (PT_GT, s.count_star (), s.ival (1)), 0.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (1)), 0.0) == HDEF);
  CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), s.ival (5)), 0.0) == HDEF);
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (1)), 0.0) == HDEF);
  CHECK (s.agg_sel (s.between (s.count_star (), s.ival (0), s.ival (5)), 0.0) == HDEF);
  CHECK (s.agg_sel (s.ex (PT_GE, s.count_star (), s.ival (1)), 0.0) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.ival (0)), 0.0) == 1.0);
  CHECK (s.agg_sel (s.ex (PT_LT, s.count_star (), s.ival (1)), 0.0) == Approx (EQ_FLOOR));
  /* the other aggregates take an unknown size as one-row groups */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 0.0) == Approx (0.75));
  CHECK (g_gs_calls == 0);
}

/* ================================================================ H: the HAVING list */

TEST_CASE ("PR8325 H1: the HAVING conjuncts multiply; no HAVING is 1", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *having = scene::conj ({s.ex (PT_GT, s.count_star (), s.ival (100)),
				  s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))});
  CHECK (s.having_sel (having, 100.0) == Approx (pow (0.99, 100) * 0.75));
  CHECK (s.having_sel (NULL, 100.0) == 1.0);
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H2: a predicate without an aggregate is a WHERE term (qo_expr_selectivity on that predicate alone, "
	   "next and or_next cut) -- an aggregate in a later conjunct does not make it one, and the list is left as it "
	   "was", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *c1 = s.ex (PT_GT, s.col ("a"), s.ival (5));
  PT_NODE *c2 = s.ex (PT_GT, s.count_star (), s.ival (100));
  PT_NODE *having = scene::conj ({c1, c2});
  CHECK (s.having_sel (having, 100.0) == Approx (0.3 * pow (0.99, 100)));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == c1);
  CHECK (c1->next == c2);
  CHECK (c2->next == NULL);

  reset_probes ();
  PT_NODE *d1 = s.ex (PT_GT, s.count_star (), s.ival (100));
  PT_NODE *d2 = s.ex (PT_GT, s.col ("a"), s.ival (5));
  PT_NODE *d3 = s.ex (PT_LT, s.col ("a"), s.ival (50));
  CHECK (s.having_sel (scene::conj ({d1, d2, d3}), 100.0) == Approx (pow (0.99, 100) * 0.3 * 0.3));
  REQUIRE (g_where_preds.size () == 2);
  CHECK ((g_where_preds[0] == d2 && g_where_preds[1] == d3));
  CHECK ((d1->next == d2 && d2->next == d3 && d3->next == NULL));
  CHECK_FALSE (g_where_non_expr);
  CHECK_FALSE (g_where_had_list);
}

TEST_CASE ("PR8325 H3: a predicate with GROUPBY_NUM () is left to the LIMIT handling -- it contributes 1, whatever "
	   "else it holds", "[contract][having][groupby_num]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (s.ex (PT_LE, s.groupby_num (), s.ival (10)), 100.0) == 1.0);
  CHECK (s.having_sel (scene::conj ({s.ex (PT_LE, s.groupby_num (), s.ival (10)),
				     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))}), 100.0)
	 == Approx (0.75));
  CHECK (s.having_sel (scene::disj ({s.ex (PT_LE, s.groupby_num (), s.ival (10)),
				     s.ex (PT_GT, s.count_star (), s.ival (100))}), 100.0) == 1.0);
  CHECK (s.having_sel (s.ex (PT_LT, s.ival (10), s.groupby_num ()), 100.0) == 1.0);
  CHECK (s.having_sel (s.ex (PT_AND, s.ex (PT_LE, s.groupby_num (), s.ival (10)),
			     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))), 100.0) == 1.0);
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H4: any aggregate predicate the histograms cannot price takes QO_HAVING_DEFAULT_SELECTIVITY 0.5 "
	   "(5cfef7f5b: was DEFAULT_SELECTIVITY 0.1; AVG (x) = 25 and the OR of two priced terms are priced now)",
	   "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)), 10.0) == HDEF);
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("y")), s.ival (25)), 10.0) == HDEF);
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.subquery (PT_MAX)), 10.0) == HDEF);
  CHECK (s.having_sel (s.ex (PT_GT, s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)), s.ival (500)), 10.0)
	 == HDEF);
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x"), PT_DISTINCT), s.ival (500)), 10.0) == HDEF);
  /* 766999f0e: DEFAULT_SELECTIVITY 0.1 for both of these */
  CHECK (s.having_sel (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (EQ_FLOOR));
  CHECK (s.having_sel (scene::disj ({s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
				     s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (5))}), 10.0)
	 == Approx (or_sel (0.75, 0.05)));
  CHECK (s.having_sel (scene::conj ({s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)),
				     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))}), 10.0)
	 == Approx (HDEF * 0.75));
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H5: an aggregate inside a subquery is not this block's -- `a > (SELECT MAX (u.z) FROM u)` is a "
	   "WHERE term", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *c = s.ex (PT_GT, s.col ("a"), s.subquery (PT_MAX));
  CHECK (unitseam_having_found (s.parser, c) == 0);
  CHECK (s.having_sel (c, 10.0) == Approx (0.3));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == c);
}

TEST_CASE ("PR8325 H6: a folded constant -- true keeps every group, false none (5cfef7f5b: a false constant was "
	   "skipped as 1)", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (scene::conj ({s.truth (), s.ex (PT_GT, s.count_star (), s.ival (100))}), 100.0)
	 == Approx (pow (0.99, 100)));
  CHECK (s.having_sel (scene::conj ({s.ex (PT_GT, s.count_star (), s.ival (100)), s.truth ()}), 100.0)
	 == Approx (pow (0.99, 100)));
  CHECK (s.having_sel (scene::conj ({s.logical (0), s.ex (PT_GT, s.count_star (), s.ival (100))}), 100.0) == 0.0);
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H7: qo_has_aggregate_pre finds an aggregate anywhere in the predicate outside subqueries -- nested "
	   "in an expression, on the right, in an OR branch -- and GROUPBY_NUM () as both flags", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const int AGG = unitseam_having_flag_aggregate (), GBN = unitseam_having_flag_groupby_num ();
  REQUIRE ((AGG != 0 && GBN != 0 && AGG != GBN));

  CHECK (unitseam_having_found (s.parser, s.ex (PT_GT, s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)),
						 s.ival (500))) == AGG);
  CHECK (unitseam_having_found (s.parser, s.ex (PT_LT, s.ival (500), s.agg (PT_SUM, s.col ("x")))) == AGG);
  CHECK (unitseam_having_found (s.parser, scene::disj ({s.ex (PT_GT, s.col ("a"), s.ival (5)),
							s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500))}))
	 == AGG);
  CHECK (unitseam_having_found (s.parser, s.ex (PT_LE, s.groupby_num (), s.ival (10))) == (AGG | GBN));
  CHECK (unitseam_having_found (s.parser, s.ex (PT_GT, s.col ("a"), s.ival (5))) == 0);
  /* the subquery's own aggregate, before anything else in the walk */
  CHECK (unitseam_having_found (s.parser, s.ex (PT_GT, s.subquery (PT_COUNT_STAR), s.ival (5))) == 0);
  /* an aggregate visited before the subquery is still seen; so is one that is the subquery's next sibling itself */
  CHECK (unitseam_having_found (s.parser, s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.subquery (PT_MAX))) == AGG);
  CHECK (unitseam_having_found (s.parser, s.ex (PT_LT, s.subquery (PT_MAX), s.agg (PT_SUM, s.col ("x")))) == AGG);
  /* a subquery in one operand of AND / OR does not hide the other operand (its node restores the walk) */
  CHECK (unitseam_having_found (s.parser, s.ex (PT_OR, s.ex (PT_GT, s.col ("a"), s.subquery (PT_MAX)),
						 s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500)))) == AGG);
}

TEST_CASE ("PR8325 H8: the disjuncts of a conjunct (or_next) are priced one by one and combine as OR (p + q - pq) -- "
	   "a non-aggregate disjunct is a WHERE term alone, a GROUPBY_NUM () disjunct makes the OR 1; the list is left "
	   "as it was (5cfef7f5b: the OR of aggregates took 0.1)", "[contract][having][or]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *o1 = s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25));
  PT_NODE *o2 = s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (5));
  PT_NODE *o3 = s.ex (PT_GT, s.count_star (), s.ival (100));
  PT_NODE *c = scene::disj ({o1, o2, o3});
  CHECK (s.pred_sel (c, 100.0) == Approx (or_sel (or_sel (0.75, 0.05), pow (0.99, 100))));
  CHECK ((o1->or_next == o2 && o2->or_next == o3 && o3->or_next == NULL));

  reset_probes ();
  PT_NODE *w = s.ex (PT_GT, s.col ("a"), s.ival (5));
  PT_NODE *v = s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500));
  CHECK (s.having_sel (scene::disj ({w, v}), 10.0) == Approx (or_sel (0.3, 0.5)));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == w);
  CHECK_FALSE (g_where_had_list);
  CHECK (w->or_next == v);

  /* an unpriced disjunct takes 0.5 inside the OR */
  CHECK (s.having_sel (scene::disj ({s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)),
				     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))}), 10.0)
	 == Approx (or_sel (HDEF, 0.75)));
  CHECK (s.having_sel (scene::disj ({s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
				     s.ex (PT_LE, s.groupby_num (), s.ival (3))}), 10.0) == 1.0);
}

TEST_CASE ("PR8325 H9: AND, OR and NOT nodes inside a predicate combine as in WHERE -- product, p + q - pq, 1 - p -- "
	   "over operands priced the same way; a NOT without an aggregate is the WHERE estimate of the NOT",
	   "[contract][having][logic]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (s.ex (PT_AND, s.ex (PT_GT, s.count_star (), s.ival (100)),
			     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))), 100.0)
	 == Approx (pow (0.99, 100) * 0.75));
  PT_NODE *w = s.ex (PT_GT, s.col ("a"), s.ival (5));
  CHECK (s.having_sel (s.ex (PT_OR, w, s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500))), 10.0)
	 == Approx (or_sel (0.3, 0.5)));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == w);
  CHECK (s.having_sel (s.ex (PT_NOT, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))), 10.0) == Approx (0.25));
  CHECK (s.having_sel (s.ex (PT_NOT, s.ex (PT_OR, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
					   s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (5)))), 10.0)
	 == Approx (1.0 - or_sel (0.75, 0.05)));
  /* an unpriced operand is 0.5 inside the combination */
  CHECK (s.having_sel (s.ex (PT_AND, s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)),
			     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))), 10.0) == Approx (HDEF * 0.75));
  /* no aggregate under the NOT: one WHERE term, the NOT node itself */
  reset_probes ();
  PT_NODE *nw = s.ex (PT_NOT, s.ex (PT_GT, s.col ("a"), s.ival (5)));
  CHECK (s.having_sel (nw, 10.0) == Approx (0.3));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == nw);
}

TEST_CASE ("PR8325 H10: `a > (SELECT MAX (u.z) FROM u) OR SUM (x) > 500` -- each disjunct walked alone, so the SUM "
	   "after the subquery is found (5cfef7f5b: on 766999f0e this was U1's first form, the whole conjunct a WHERE "
	   "term)", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *d1 = s.ex (PT_GT, s.col ("a"), s.subquery (PT_MAX));
  PT_NODE *d2 = s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500));
  PT_NODE *c1 = scene::disj ({d1, d2});
  CHECK (s.having_sel (c1, 10.0) == Approx (or_sel (0.3, 0.5)));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == d1);
  CHECK_FALSE (g_where_had_list);
}

TEST_CASE ("PR8325 U1: a subquery hides the aggregates of its later siblings -- in `(SELECT COUNT (*) FROM u) < "
	   "SUM (x) + 1` the SUM is not seen and the predicate is a WHERE term instead of the HAVING default 0.5 "
	   "(qo_has_aggregate_pre leaves PT_LIST_WALK set; it never resets *continue_walk) -- [defect] latent, "
	   "PASS = present", "[defect][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *c2 = s.ex (PT_LT, s.subquery (PT_COUNT_STAR), s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)));
  CHECK (unitseam_having_found (s.parser, c2) == 0);
  CHECK (s.having_sel (c2, 10.0) == Approx (g_where_sel));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == c2);

  /* under a NOT as well: the NOT goes to the WHERE estimate whole */
  reset_probes ();
  PT_NODE *n2 = s.ex (PT_NOT, s.ex (PT_LT, s.subquery (PT_COUNT_STAR),
				    s.ex (PT_ABS, s.agg (PT_SUM, s.col ("x")))));
  CHECK (unitseam_having_found (s.parser, n2) == 0);
  CHECK (s.having_sel (n2, 10.0) == Approx (g_where_sel));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == n2);
}

TEST_CASE ("PR8325 U2: BETWEEN subtracts 1 instead of the share of groups with a value -- with a NULL share f each "
	   "bound's P (AGG >= low), P (AGG <= high) carries has_value, so P (>= low) + P (<= high) - 1 is short by the "
	   "all-NULL groups (1 - has_value) and drops to the floor (qo_having_between, query_planner.c:1060) -- "
	   "[defect] PASS = present", "[defect][aggregate][between][null]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();

  {
    /* x 90% NULL, one-row groups (the size of an unknown group count): has_value 0.1. AVG (x) BETWEEN 25 AND 75:
     * the groups with a value whose x is in [25, 75] are 0.1 * 0.5 = 0.05; the estimate is 0.075 + 0.075 - 1 < 0 */
    scene s (0.9);
    const double hv = has_value (0.9, 1.0);
    REQUIRE (hv == Approx (0.1));
    const double ge = s.agg_sel (s.ex (PT_GE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 1.0);
    const double gt = s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (75)), 1.0);
    CHECK (ge - gt == Approx (0.05));	/* what the two comparisons say, and the model's own answer */
    CHECK (s.term (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75)), 1.0) == 0.0);
    CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75)), 1.0) == Approx (EQ_FLOOR));
    /* NOT BETWEEN then keeps every group, the all-NULL ones included */
    CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75), PT_NOT_BETWEEN), 1.0) == 1.0);
  }
  {
    /* 20% NULL, groups of 10: has_value 0.9756 -- short by 0.0244 */
    scene s (0.2);
    const double hv = has_value (0.2, 10.0);
    const double est = s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (25), s.ival (75)), 10.0);
    CHECK (est == Approx (hv * (0.75 + 0.75) - 1.0));
    CHECK (est < hv * 0.5 - 0.02);
    /* a narrow range: 20% NULL at one-row groups (has_value 0.8) and [45, 55] gives the floor instead of 0.08 */
    CHECK (s.agg_sel (s.between (s.agg (PT_AVG, s.col ("x")), s.ival (45), s.ival (55)), 1.0) == Approx (EQ_FLOOR));
    CHECK (0.8 * 0.1 == Approx (0.08));
  }
}

TEST_CASE ("PR8325 U3: IN adds a value listed twice twice -- `COUNT (*) IN (3, 3)` is 2 P (COUNT (*) = 3) and NOT IN "
	   "loses as much (the WHERE estimate counts a repeated element once, qo_all_some_in_selectivity) -- "
	   "[defect] latent/low, PASS = present", "[defect][aggregate][in]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  const double g = 10.0;
  const double eq3 = 0.81 - 0.729;

  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.count_star (), s.in_list ({vint (3), vint (3)})), g) == Approx (2 * eq3));
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.count_star (), s.in_list ({vint (3), vdbl (3.0)})), g) == Approx (2 * eq3));
  CHECK (s.agg_sel (s.ex (PT_IS_NOT_IN, s.count_star (), s.in_list ({vint (3), vint (3)})), g)
	 == Approx (1.0 - 2 * eq3));
  g_eq = 0.02;
  CHECK (s.agg_sel (s.ex (PT_IS_IN, s.agg (PT_AVG, s.col ("x")), s.in_list ({vint (25), vint (25), vint (25)})), g)
	 == Approx (0.06));
}

/* ================================================================ E: qo_estimate_ngroups () */

TEST_CASE ("PR8325 E1 develop: control -- no HAVING: all rows read, the groups are the GROUP BY NDV; behind a filter, "
	   "qo_estimate_ndv (N, p, n)", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = 100;
  {
    grouping gb (s, 10000.0, 10000.0, NULL);
    CHECK (gb.run (SORT_GROUPBY) == 100.0);
  }
  {
    grouping gb (s, 1000.0, 10000.0, NULL);
    /* 100 (1 - 0.9^100) = 99.997 */
    CHECK (gb.run (SORT_GROUPBY) == 99.0);
  }
  CHECK (g_probes == 0);
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 E2: HAVING scales the groups by its selectivity at the group size rows / groups", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = 100;
  {
    /* groups of 100 rows: COUNT (*) > 100 passes 0.99^100 = 0.366 of them */
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (100)));
    CHECK (gb.run (SORT_GROUPBY) == 36.0);
  }
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)));
    CHECK (gb.run (SORT_GROUPBY) == 75.0);
  }
  {
    /* behind a filter: 99 groups of 1000 / 99 rows */
    grouping gb (s, 1000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (10)));
    const double expect = (double) (INT64) (99 * size_at_least (11, 1000.0 / 99));
    REQUIRE (expect == 34.0);
    CHECK (gb.run (SORT_GROUPBY) == expect);
  }
  {
    /* two conjuncts */
    grouping gb (s, 10000.0, 10000.0, scene::conj ({s.ex (PT_GT, s.count_star (), s.ival (100)),
						    s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))}));
    CHECK (gb.run (SORT_GROUPBY) == (double) (INT64) (100 * pow (0.99, 100) * 0.75));
  }
  CHECK (g_gs_calls == 0);
}

TEST_CASE ("PR8325 E3: whatever HAVING keeps, at least one group is left", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = 100;
  grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (100)));
  /* 100 * 0.001 = 0.1 groups */
  CHECK (gb.run (SORT_GROUPBY) == 1.0);
}

TEST_CASE ("PR8325 E4 develop: a HAVING of GROUPBY_NUM () only leaves the groups as they are", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = 100;
  grouping gb (s, 10000.0, 10000.0, s.ex (PT_LE, s.groupby_num (), s.ival (10)));
  CHECK (gb.run (SORT_GROUPBY) == 100.0);
}

TEST_CASE ("PR8325 E5 develop: HAVING belongs to the GROUP BY sort -- a DISTINCT sort keeps its select-list NDV",
	   "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_distinct = 50;
  grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (100)));
  CHECK (gb.run (SORT_DISTINCT) == 50.0);
  CHECK (g_probes == 0);
}

TEST_CASE ("PR8325 E6: an unknown group count (no NDV) -- each row a group: the rows times the HAVING selectivity at "
	   "the unknown size, at least one row; COUNT (*) is not priced there (5cfef7f5b: COUNT (*) > 1 was one row a "
	   "group, 10 rows; now 0.5, 5000)", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = -1;
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (1)));
    CHECK (gb.run (SORT_GROUPBY) == Approx (10000.0 * HDEF));
  }
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)));
    CHECK (gb.run (SORT_GROUPBY) == Approx (7500.0));
  }
  {
    /* SUM over one-row groups is the row: x > 50 */
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (50)));
    CHECK (gb.run (SORT_GROUPBY) == Approx (5000.0));
    CHECK (g_probe_value == Approx (50.0));
  }
  {
    grouping gb (s, 500.0, 500.0, s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (100)));
    CHECK (gb.run (SORT_GROUPBY) == 1.0);
  }
  {
    /* no HAVING: the rows, as before */
    grouping gb (s, 10000.0, 10000.0, NULL);
    CHECK (gb.run (SORT_GROUPBY) == 10000.0);
  }
  {
    /* even with a histogram on the GROUP BY column the unknown count does not look at it */
    s.annotate_a ();
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (600)));
    CHECK (gb.run (SORT_GROUPBY) == Approx (10000.0 * HDEF));
    CHECK (g_gs_calls == 0);
    CHECK (g_rows_calls == 0);
  }
}

TEST_CASE ("PR8325 E7: the info is shared -- a DISTINCT over the GROUP BY lowers the groups, never raises them "
	   "(select-list NDV unknown or larger)", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  {
    g_ndv_groupby = 100;
    g_ndv_distinct = -1;
    grouping gb (s, 10000.0, 10000.0, NULL);
    CHECK (gb.run (SORT_GROUPBY) == 100.0);
    CHECK (gb.run (SORT_DISTINCT) == 100.0);
  }
  {
    g_ndv_groupby = 100;
    g_ndv_distinct = 50;
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (100)));
    CHECK (gb.run (SORT_GROUPBY) == 36.0);
    CHECK (gb.run (SORT_DISTINCT) == 36.0);
  }
  {
    /* a DISTINCT with a smaller NDV still lowers them (develop does too) */
    g_ndv_groupby = 100;
    g_ndv_distinct = 20;
    grouping gb (s, 10000.0, 10000.0, NULL);
    CHECK (gb.run (SORT_GROUPBY) == 100.0);
    CHECK (gb.run (SORT_DISTINCT) == 20.0);
  }
}

TEST_CASE ("PR8325 E8: qo_having_env_init -- a single GROUP BY column whose segment carries a histogram gives that "
	   "annotated name and the scale group size / (rows / distinct values); a size of 0, two columns, an expression, "
	   "no histogram, no rows or no distinct value give none", "[contract][ngroups][count]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;
  s.annotate_a ();

  PT_NODE *attr = NULL;
  double scale = 0.0, size = 0.0;
  int aborted = 0;
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (1)));
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (aborted == 0);
    CHECK (attr == s.seg_a);
    CHECK (scale == Approx (100.0 / (10000.0 / 100.0)));
    CHECK (size == 100.0);
    unitseam_having_env_init (gb.plan, 25.0, &attr, &scale, &size, &aborted);
    CHECK (attr == s.seg_a);
    CHECK (scale == Approx (0.25));

    /* unknown size */
    unitseam_having_env_init (gb.plan, 0.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
    CHECK (scale == 1.0);
    CHECK (size == 0.0);

    /* GROUP BY a, y */
    gb.select->info.query.q.select.group_by->next = gb.sort_spec (s.col ("y"));
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
    /* GROUP BY a + 1 */
    gb.select->info.query.q.select.group_by = gb.sort_spec (s.ex (PT_PLUS, s.col ("a"), s.ival (1)));
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
    /* GROUP BY y (no histogram) */
    gb.select->info.query.q.select.group_by = gb.sort_spec (s.col ("y"));
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
    /* GROUP BY u.z (not this block's) */
    gb.select->info.query.q.select.group_by = gb.sort_spec (s.name ("z", SPEC_U));
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (aborted == 0);
    CHECK (attr == NULL);
  }
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (1)));
    g_a.total_rows = 0.0;
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
    default_group_hist ();
    g_a.mcv_freq.clear ();
    g_a.buckets.clear ();
    unitseam_having_env_init (gb.plan, 100.0, &attr, &scale, &size, &aborted);
    CHECK (attr == NULL);
  }
  CHECK (g_unexpected == 0);
}

TEST_CASE ("PR8325 E9: with the GROUP BY column's histogram COUNT (*) takes the value counts at the scale of the group "
	   "size rows / groups -- unfiltered (scale 1) and behind a filter", "[contract][ngroups][count]")
{
  REQUIRE (boot ());
  scene s;
  NEEDS_HAVING ();
  s.annotate_a ();

  g_ndv_groupby = 100;
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (600)));
    const double expect = (double) std::max ((INT64) (100 * bounded (group_sel (601, 1.0))), (INT64) 1);
    CHECK (gb.run (SORT_GROUPBY) == expect);
    CHECK (expect == 2.0);
    CHECK (g_gs_attr == s.seg_a);
    CHECK (g_gs_min_rows == 601.0);
    CHECK (g_gs_scale == Approx (1.0));
  }
  {
    /* 1000 of the 10000 rows: 99 groups of 1000 / 99 rows, a tenth of the value counts */
    grouping gb (s, 1000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (30)));
    const double sc = (1000.0 / 99) / (10000.0 / 100);
    const double expect = (double) std::max ((INT64) (99 * bounded (group_sel (31, sc))), (INT64) 1);
    CHECK (gb.run (SORT_GROUPBY) == expect);
    CHECK (g_gs_scale == Approx (sc));
    CHECK (g_gs_min_rows == 31.0);
    /* 3.8 groups: the three MCVs (202, 101 and 51 rows at this scale) and 0.8 of the bucket's; the geometric model
     * alone gives 99 * 0.901^30 = 4.3 */
    CHECK (expect == 3.0);
    CHECK ((double) (INT64) (99 * size_at_least (31, 1000.0 / 99)) == 4.0);
  }
  CHECK (g_unexpected == 0);
}
