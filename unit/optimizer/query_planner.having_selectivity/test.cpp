/*
 * PR#8325 (CBRD-27811) -- the HAVING selectivity applied to the GROUP BY row estimate, src/optimizer/query_planner.c
 * (CS_MODE), reached through appendix hooks (hooks.cpp):
 *
 *   qo_estimate_ngroups (plan, sort_type)       the GROUP BY / DISTINCT group count kept in plan->info->group_rows
 *     -> qo_having_selectivity (env, having, group_size)            the share of the groups that pass HAVING
 *          -> parser_walk_tree (conjunct, qo_has_aggregate_pre)     aggregate / GROUPBY_NUM () outside subqueries
 *          -> qo_expr_selectivity (conjunct)                        a conjunct without an aggregate (seam)
 *          -> qo_having_aggregate_selectivity (env, pred, size)     "aggregate op constant"
 *               -> lookup_node / lookup_seg (query_graph.c, real)    the HAVING name's segment
 *               -> histogram_get_comp_selectivity (fake)             x op c on the segment's annotated name
 *
 * The contract read off the functions and their header comments (query_planner.c:754-1091 at 766999f0e):
 *   A  AVG (x) op c = p (the histogram selectivity of x op c); SUM (x) op c = x op c / group size; MAX (x) > c and
 *      MIN (x) < c = some row of the group passes, MAX (x) < c and MIN (x) > c = every row does, the group sizes
 *      geometric with the given mean (r = 1 - 1 / size); COUNT (*) op c = the group size op c (P (size >= k) =
 *      r ^ (k - 1)), no histogram; a NULL share f: an all-NULL group fails, the others aggregate their non-NULL rows;
 *      c op AGG (x) is AGG (x) reversed op c; the result lies in [DEFAULT_EQUAL_SELECTIVITY, 1]; any other shape
 *      gives -1
 *   H  the conjuncts multiply; one without an aggregate is a WHERE term (qo_expr_selectivity); one with GROUPBY_NUM ()
 *      is left to the LIMIT handling; any other aggregate conjunct takes DEFAULT_SELECTIVITY; an aggregate inside a
 *      subquery does not count; each conjunct is walked alone and the list is left as it was
 *   E  the groups (NDV known: estimate_ndv, unknown: the rows) are scaled by the HAVING selectivity at the group size
 *      rows / groups (unknown: 1), floored at 1; HAVING only for the GROUP BY sort; group_rows only goes down (the
 *      info is shared with a DISTINCT over the GROUP BY)
 *
 * Real code: query_planner.c (the TU), query_graph.c (lookup_node / lookup_seg / qo_abort), parser_support.c
 * (pt_is_aggregate_function, pt_get_end_path_node, pt_name_equal, pt_is_function_index_expression), parse_tree.c +
 * parse_tree_cl.c (parser_create_parser, parser_new_node, parser_walk_tree with its apply table,
 * pt_is_function_index_expr), object_domain.c + object_primitive.c + domain_rules.c + numeric_opfunc.c
 * (tp_value_coerce to tp_Double_domain: INTEGER, DOUBLE, NUMERIC), db_macro.c, language_support.c + intl_support.c +
 * chartype.c (intl_identifier_casecmp, en_US.utf8), memory_alloc.c, system_parameter.c.
 * Seams: qo_get_group_ndv (the GROUP BY / select-list NDV from index statistics), qo_expr_selectivity (the WHERE-term
 * estimate). Fakes (see suite.json "fakes"): histogram_get_comp_selectivity (a uniform column), er_*,
 * pt_parser_line_col, pt_free_dblink_remote_cols, envvar_get / envvar_trim_char, init_builtin_calendar_names /
 * db_date_locale_init, cubregex::get_engine_name.
 *
 * Tags: [contract] expected behaviour (head FAIL + base PASS = regression candidate); `... develop` cases hold on base
 *       too; the others are this PR's (base FAIL = new). [defect] reproduces a defect (PASS = the defect is there).
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
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
#include <initializer_list>
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

/* ---------------------------------------------------------------- the histogram (fake) */

/* histogram_cl.cpp histogram_get_comp_selectivity () (1377 at 766999f0e), its exits:
 *   rhs NULL -> not priced;
 *   histogram_init_reader_from_lhs (724): the end of the path must be a PT_NAME carrying the annotation
 *     (info.name.histogram; the class-cache fallback needs bind_fp_active_statement, set only during a bind
 *     fingerprint walk, never while planning) -> else not priced;
 *   histogram_extract_key + the kind check (1395-1440): a numeric column takes INTEGER / SHORT / BIGINT / FLOAT /
 *     DOUBLE / NUMERIC keys (promoted), anything else (a string, a NULL) -> not priced;
 *   total_rows <= 0 -> success, 0;
 *   otherwise the share of the NON-NULL rows (the real one divides the null mass back out, 1496-1501), clamped to
 *     [0, 1], and an estimate of 0 floored at 1 / total_rows (1515-1518).
 * The column here holds values spread evenly over [g_lo, g_hi] (continuous, so include_equal does not move it):
 * P (x < v) = (v - lo) / (hi - lo). Every probe is recorded. */
namespace
{
  double g_lo = 0.0, g_hi = 100.0, g_total = 1000000.0;
  int g_probes = 0;
  PT_NODE *g_probe_lhs = NULL;
  DB_VALUE *g_probe_value_ptr = NULL;
  double g_probe_value = 0.0;
  DB_TYPE g_probe_type = DB_TYPE_NULL;
  bool g_probe_is_ge = false, g_probe_incl = false;

  bool numeric_key (const DB_VALUE * v, double *out)
  {
    if (DB_IS_NULL (v))
      {
	return false;
      }
    switch (DB_VALUE_DOMAIN_TYPE (v))
      {
      case DB_TYPE_INTEGER:
	*out = db_get_int (v);
	return true;
      case DB_TYPE_SHORT:
	*out = db_get_short (v);
	return true;
      case DB_TYPE_BIGINT:
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
  PT_NODE *name = pt_get_end_path_node (lhs);
  if (name == NULL || name->node_type != PT_NAME || name->info.name.histogram == NULL)
    {
      return;
    }
  double v;
  if (!numeric_key (rhs_db_value, &v))
    {
      return;
    }
  g_probe_value = v;
  if (g_total <= 0.0)
    {
      *selectivity = 0.0;
      *success = true;
      return;
    }
  double lt = std::min (std::max ((v - g_lo) / (g_hi - g_lo), 0.0), 1.0);
  double sel = is_ge ? 1.0 - lt : lt;
  sel = std::min (std::max (sel, 0.0), 1.0);
  if (sel <= 0.0)
    {
      sel = 1.0 / g_total;
    }
  *selectivity = sel;
  *success = true;
}

/* ---------------------------------------------------------------- seams */

/* qo_get_group_ndv (query_planner.c, static): -1 when the statement is not a SELECT; otherwise the product of the
 * index-statistics NDV of the columns of the GROUP BY list (SORT_GROUPBY) or the select list (SORT_DISTINCT),
 * -1 when none of them has one (qo_get_col_product_ndv multiplies by 1 for an NDV of 0, and a product of 1 is -1).
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

/* qo_expr_selectivity (query_planner.c): the WHERE-term estimate of one conjunct (its or_next list); the real one
 * QO_ASSERTs a PT_EXPR. The test sets the estimate and reads which conjuncts came. */
namespace
{
  double g_where_sel = 0.3;
  std::vector < PT_NODE * >g_where_preds;
  bool g_where_non_expr = false;
}

double
qo_expr_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  g_where_preds.push_back (pt_expr);
  if (pt_expr == NULL || pt_expr->node_type != PT_EXPR)
    {
      g_where_non_expr = true;
    }
  return g_where_sel;
}

/* ---------------------------------------------------------------- hooks.cpp (appended to query_planner.c) */
int unitseam_has_having (void);
int unitseam_having_flag_aggregate (void);
int unitseam_having_flag_groupby_num (void);
int unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred);
double unitseam_having_aggregate_selectivity (QO_ENV * env, PT_NODE * pred, double group_size, int *aborted);
double unitseam_having_selectivity (QO_ENV * env, PT_NODE * having, double group_size, int *aborted);
void unitseam_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type, int *aborted);

/* ---------------------------------------------------------------- the model, written out (oracle) */
namespace
{
  const double EQ_FLOOR = DEFAULT_EQUAL_SELECTIVITY;	/* 0.001 */

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

  /* the fake column: share of non-NULL rows with x > c / x < c */
  double p_gt (double c)
  {
    return std::min (std::max ((g_hi - c) / (g_hi - g_lo), 0.0), 1.0);
  }

  double p_lt (double c)
  {
    return 1.0 - p_gt (c);
  }

  void reset_probes ()
  {
    g_probes = 0;
    g_probe_lhs = NULL;
    g_probe_value_ptr = NULL;
    g_probe_value = 0.0;
    g_probe_type = DB_TYPE_NULL;
    g_probe_is_ge = g_probe_incl = false;
    g_where_preds.clear ();
    g_where_non_expr = false;
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
    g_ndv_groupby = g_ndv_distinct = -1;
    g_ndv_calls = 0;
    g_where_sel = 0.3;
    g_er_id = NO_ERROR;
    g_er_sets = 0;
    reset_probes ();
    return lang_charset () == INTL_CODESET_UTF8;
  }

  const UINTPTR SPEC_T = 0x1001;	/* t: the FROM table of the block, a node of the environment */
  const UINTPTR SPEC_U = 0x2002;	/* u: only in subqueries, not joined (no node here) */

  /* one block `SELECT ... FROM t GROUP BY a HAVING ...` as the optimizer has it: one node (t), segments x (with a
   * histogram), y (without) and a (the GROUP BY column), each a PT_NAME the annotation pass wrote the histogram and
   * the NULL share on (qo_get_attr_info, query_graph.c:5509-5524); the names in HAVING are other PT_NAME nodes of
   * the same column, not annotated. Nodes come from the real parser_new_node. */
  struct scene
  {
    PARSER_CONTEXT *parser;
    QO_ENV *env;
    PT_NODE *spec_t;
    PT_NODE *seg_x, *seg_y, *seg_a;
    DB_VALUE blob;		/* stands for x's histogram blob; only its address is read */

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

    PT_NODE *truth ()
    {
      PT_NODE *n = parser_new_node (parser, PT_VALUE);
      n->type_enum = PT_TYPE_LOGICAL;
      n->info.value.data_value.i = 1;
      db_make_int (&n->info.value.db_value, 1);
      n->info.value.db_value_is_initialized = true;
      return n;
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
      e->type_enum = (op == PT_PLUS) ? PT_TYPE_DOUBLE : PT_TYPE_LOGICAL;
      return e;
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

    double agg_sel (PT_NODE * pred, double group_size)
    {
      int aborted = 0;
      double s = unitseam_having_aggregate_selectivity (env, pred, group_size, &aborted);
      CHECK (aborted == 0);
      return s;
    }

    double having_sel (PT_NODE * having, double group_size)
    {
      int aborted = 0;
      double s = unitseam_having_selectivity (env, having, group_size, &aborted);
      CHECK (aborted == 0);
      return s;
    }
  };

  /* plan->info as qo_sort_new () hands it to qo_estimate_ngroups (): the subplan's info, shared by the sort plans
   * over it, group_rows starting at the cardinality (qo_alloc_info, query_planner.c:8051) */
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
      select->info.query.q.select.group_by = s.col ("a");
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
    INFO ("qo_having_selectivity / qo_having_aggregate_selectivity are not on this revision (new in PR#8325)"); \
    REQUIRE (on_this_revision ()); \
  } while (0)

/* ================================================================ A: qo_having_aggregate_selectivity () */

TEST_CASE ("PR8325 A1: control -- AVG (x) op c is the histogram selectivity of x op c, probed on the segment's "
	   "annotated name (the HAVING name is not annotated), whatever the group size", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

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

TEST_CASE ("PR8325 A5: COUNT (*) op c is the group size against c, geometric with the given mean "
	   "(P (size >= k) = r^(k-1)) -- no histogram, a host variable read like a constant", "[contract][aggregate]")
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

  /* one-row groups (the caller's group size when the group count is unknown) */
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

TEST_CASE ("PR8325 A8: the result lies in [DEFAULT_EQUAL_SELECTIVITY, 1] -- an empty range, every-row of a large "
	   "group, a full range", "[contract][aggregate]")
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
}

TEST_CASE ("PR8325 A9: `AGG (x) op ?` reads the input host variable; a NULL value or an output host variable is not "
	   "this form", "[contract][aggregate]")
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
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (1)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.hvar (1)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.hvar (0, PT_HOST_OUT)), 10.0) == -1.0);
  CHECK (g_probes == 0);
}

TEST_CASE ("PR8325 A10: any other shape is not this form (-1) -- operator, OR, no or two aggregates, COUNT (x) and "
	   "the others, DISTINCT SUM / AVG, an expression argument, a column without a histogram or outside the block, "
	   "the NULL literal, a subquery, a key the histogram cannot take", "[contract][aggregate]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.agg_sel (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_NE, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_EQ, s.count_star (), s.ival (3)), 10.0) == -1.0);
  CHECK (s.agg_sel (scene::disj ({s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
				  s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (5))}), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.col ("x"), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_LT, s.ival (25), s.col ("x")), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.agg (PT_MIN, s.col ("x"))), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_STDDEV, s.col ("x")), s.ival (3)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x"), PT_DISTINCT), s.ival (500)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x"), PT_DISTINCT), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.ex (PT_PLUS, s.col ("x"), s.ival (1))), s.ival (25)), 10.0)
	 == -1.0);
  CHECK (g_probes == 0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("y")), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.name ("z", SPEC_U)), s.ival (25)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.null_literal ()), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.subquery (PT_MAX)), 10.0) == -1.0);
  CHECK (s.agg_sel (s.ex (PT_GT, s.count_star (), s.subquery (PT_COUNT_STAR)), 10.0) == -1.0);
  CHECK (g_probes == 0);
  /* reaches the histogram, which cannot take a string key on a numeric column */
  CHECK (s.agg_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.sval ("abc")), 10.0) == -1.0);
  CHECK (g_probes == 1);
}

/* ================================================================ H: qo_having_selectivity () */

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

TEST_CASE ("PR8325 H2: a conjunct without an aggregate is a WHERE term (qo_expr_selectivity on that conjunct) -- an "
	   "aggregate in a later conjunct does not make it one, and the list is left as it was", "[contract][having]")
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
}

TEST_CASE ("PR8325 H3: a conjunct with GROUPBY_NUM () is left to the LIMIT handling -- it contributes 1, whatever "
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
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H4: any other aggregate conjunct takes DEFAULT_SELECTIVITY", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)), 10.0) == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_COUNT, s.col ("x")), s.ival (3)), 10.0) == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_AVG, s.col ("y")), s.ival (25)), 10.0) == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.subquery (PT_MAX)), 10.0)
	 == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (scene::disj ({s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
				     s.ex (PT_LT, s.agg (PT_AVG, s.col ("x")), s.ival (5))}), 10.0)
	 == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (s.ex (PT_GT, s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)), s.ival (500)), 10.0)
	 == Approx (DEFAULT_SELECTIVITY));
  CHECK (s.having_sel (scene::conj ({s.ex (PT_EQ, s.agg (PT_AVG, s.col ("x")), s.ival (25)),
				     s.ex (PT_GT, s.agg (PT_AVG, s.col ("x")), s.ival (25))}), 10.0)
	 == Approx (DEFAULT_SELECTIVITY * 0.75));
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

TEST_CASE ("PR8325 H6: a conjunct that is not an expression is skipped", "[contract][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  CHECK (s.having_sel (scene::conj ({s.truth (), s.ex (PT_GT, s.count_star (), s.ival (100))}), 100.0)
	 == Approx (pow (0.99, 100)));
  CHECK (g_where_preds.empty ());
}

TEST_CASE ("PR8325 H7: qo_has_aggregate_pre finds an aggregate anywhere in the conjunct outside subqueries -- nested "
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
  /* an aggregate visited before the subquery is still seen */
  CHECK (unitseam_having_found (s.parser, s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.subquery (PT_MAX))) == AGG);
}

TEST_CASE ("PR8325 U1: after a subquery the walk stops descending into what follows -- `a > (SELECT MAX (u.z) FROM u) "
	   "OR SUM (x) > 500` and `(SELECT COUNT (*) FROM u) < SUM (x) + 1` are not seen as aggregate conjuncts "
	   "(qo_has_aggregate_pre leaves PT_LIST_WALK set; it never resets *continue_walk) -- [defect] latent, "
	   "PASS = present", "[defect][having]")
{
  REQUIRE (boot ());
  NEEDS_HAVING ();
  scene s;

  PT_NODE *c1 = scene::disj ({s.ex (PT_GT, s.col ("a"), s.subquery (PT_MAX)),
			      s.ex (PT_GT, s.agg (PT_SUM, s.col ("x")), s.ival (500))});
  CHECK (unitseam_having_found (s.parser, c1) == 0);
  PT_NODE *c2 = s.ex (PT_LT, s.subquery (PT_COUNT_STAR),
		      s.ex (PT_PLUS, s.agg (PT_SUM, s.col ("x")), s.ival (1)));
  CHECK (unitseam_having_found (s.parser, c2) == 0);

  /* so the conjunct goes to the WHERE-term estimate instead of the aggregate path (DEFAULT_SELECTIVITY) */
  CHECK (s.having_sel (c1, 10.0) == Approx (g_where_sel));
  REQUIRE (g_where_preds.size () == 1);
  CHECK (g_where_preds[0] == c1);
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

TEST_CASE ("PR8325 E6: an unknown group count (no index NDV) -- each row a group: the rows times the HAVING "
	   "selectivity at group size 1, at least one row", "[contract][ngroups]")
{
  REQUIRE (boot ());
  scene s;

  g_ndv_groupby = -1;
  {
    grouping gb (s, 10000.0, 10000.0, s.ex (PT_GT, s.count_star (), s.ival (1)));
    CHECK (gb.run (SORT_GROUPBY) == Approx (10000.0 * EQ_FLOOR));
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
