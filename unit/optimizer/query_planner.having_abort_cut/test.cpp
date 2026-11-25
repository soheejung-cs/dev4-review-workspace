/*
 * PR#8664 (CBRD-28204) -- a HAVING link the planner cuts by hand while a QO_ASSERT can fire, src/optimizer/query_planner.c
 * (CS_MODE), reached through the appendix hook (hooks.cpp):
 *
 *   qo_having_selectivity (henv, having)                    the conjuncts (next)
 *     -> qo_having_pred_selectivity                         the disjuncts (or_next)
 *          -> qo_having_disjunct_selectivity                cuts pred->next and pred->or_next by hand (save_next,
 *                                                           save_or_next), then for a predicate without an aggregate
 *               -> qo_expr_selectivity (seam)               the WHERE-term estimate, which QO_ASSERTs (its own
 *                                                           PT_EXPR check, qo_and/or/not_selectivity's [0, 1] checks,
 *                                                           qo_range/between_selectivity, qo_index_cardinality ->
 *                                                           lookup_node ())
 *
 * The PR makes qo_abort () put back the links recorded with qo_detach_link (); this cut is not recorded (and
 * qo_detach_link () is static to query_graph.c), so an abort under it leaves HAVING cut and the statement runs
 * without a plan on the cut HAVING.
 *
 * Real code: query_planner.c (the TU: qo_having_selectivity, qo_having_pred_selectivity,
 * qo_having_disjunct_selectivity, qo_has_aggregate_pre, qo_and_selectivity, qo_or_selectivity), query_graph.c
 * (--extra: qo_abort, and on the head its detached-link chain).
 * Seam: qo_expr_selectivity -- returns 0.3 and records the predicate with its next / or_next as it came; armed on one
 * predicate it calls qo_abort () as one of the real function's QO_ASSERTs failing would.
 * Fakes: parser_walk_tree (pre on the node, then arg1/arg2/arg3 of an expression, then or_next, then next -- the order
 * of pt_walk_private; post after), pt_is_aggregate_function (parser_support.c:635 body), er_set (one level).
 *
 * Tags: [contract] expected behaviour; [defect] reproduces a defect (PASS = the defect is there).
 *
 * scene: SELECT y, count (*) FROM t GROUP BY y HAVING (y > 1 OR y < -1) AND y = 5
 *        having = h1 (y > 1, or_next d2 (y < -1)) -> next h2 (y = 5)
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "optimizer.h"
#include "query_graph.h"
#include "error_manager.h"
#include <cmath>
#include <cstdlib>
#include <vector>

double unitseam_having_selectivity (QO_ENV * env, PT_NODE * having, int *aborted);

/* ---------------------------------------------------------------- fakes */

namespace
{
  struct seen
  {
    PT_NODE *pred, *next, *or_next;
  };

  PT_NODE *g_abort_on = NULL;
  std::vector < seen > g_seen;
  int g_er_sets = 0;
  std::vector < void *>g_alloc;

  void *zalloc (size_t n)
  {
    void *p = calloc (1, n);
    g_alloc.push_back (p);
    return p;
  }

  void free_all ()
  {
  for (void *p:g_alloc)
      {
	free (p);
      }
    g_alloc.clear ();
  }
}

/* error_manager: one level; qo_abort () er_set ()s ER_FAILED_ASSERTION before its longjmp */
void
er_set (int, const char *, const int, int, int, ...)
{
  g_er_sets++;
}

/* query_planner.c qo_expr_selectivity () (seam): the WHERE-term estimate of the predicate as it is handed over */
double
qo_expr_selectivity (QO_ENV * env, PT_NODE * pt_expr)
{
  g_seen.push_back (seen { pt_expr, pt_expr->next, pt_expr->or_next });
  if (pt_expr == g_abort_on)
    {
      /* one of the real function's QO_ASSERTs failing */
      qo_abort (env, __FILE__, __LINE__);
    }
  return 0.3;
}

/* parser_support.c:635 pt_is_aggregate_function () body */
bool
pt_is_aggregate_function (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  if (node->node_type == PT_FUNCTION)
    {
      FUNC_CODE f = node->info.function.function_type;
      if (!node->info.function.analytic.is_analytic
	  && (f == PT_MIN || f == PT_MAX || f == PT_SUM || f == PT_AVG || f == PT_STDDEV || f == PT_STDDEV_POP
	      || f == PT_STDDEV_SAMP || f == PT_VARIANCE || f == PT_VAR_POP || f == PT_VAR_SAMP || f == PT_GROUPBY_NUM
	      || f == PT_COUNT || f == PT_COUNT_STAR || f == PT_AGG_BIT_AND || f == PT_AGG_BIT_OR
	      || f == PT_AGG_BIT_XOR || f == PT_GROUP_CONCAT || f == PT_MEDIAN || f == PT_PERCENTILE_CONT
	      || f == PT_PERCENTILE_DISC || f == PT_CUME_DIST || f == PT_PERCENT_RANK || f == PT_JSON_ARRAYAGG
	      || f == PT_JSON_OBJECTAGG))
	{
	  return true;
	}
    }
  return false;
}

/* parse_tree_cl.c parser_walk_tree (): pre on the node, its sub-trees (here only an expression's args), then the
 * or_next list and the next list, post last -- the order of pt_walk_private */
static PT_NODE *
walk (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE_WALK_FUNCTION pre, void *pre_arg, PT_NODE_WALK_FUNCTION post,
      void *post_arg)
{
  if (node == NULL)
    {
      return NULL;
    }
  int cw = PT_CONTINUE_WALK;
  if (pre != NULL)
    {
      node = pre (parser, node, pre_arg, &cw);
    }
  if (cw != PT_STOP_WALK)
    {
      if ((cw == PT_CONTINUE_WALK || cw == PT_LEAF_WALK) && node->node_type == PT_EXPR)
	{
	  walk (parser, node->info.expr.arg1, pre, pre_arg, post, post_arg);
	  walk (parser, node->info.expr.arg2, pre, pre_arg, post, post_arg);
	  walk (parser, node->info.expr.arg3, pre, pre_arg, post, post_arg);
	}
      walk (parser, node->or_next, pre, pre_arg, post, post_arg);
      walk (parser, node->next, pre, pre_arg, post, post_arg);
    }
  if (post != NULL)
    {
      node = post (parser, node, post_arg, &cw);
    }
  return node;
}

PT_NODE *
parser_walk_tree (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE_WALK_FUNCTION pre_function, void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  return walk (parser, node, pre_function, pre_argument, post_function, post_argument);
}

/* ---------------------------------------------------------------- scene */

namespace
{
  PT_NODE *pt (PT_NODE_TYPE t)
  {
    PT_NODE *n = (PT_NODE *) zalloc (sizeof (PT_NODE));
    n->node_type = t;
    return n;
  }

  PT_NODE *col_y ()
  {
    PT_NODE *n = pt (PT_NAME);
    n->info.name.meta_class = PT_NORMAL;
    n->type_enum = PT_TYPE_INTEGER;
    return n;
  }

  PT_NODE *ival (int v)
  {
    PT_NODE *n = pt (PT_VALUE);
    n->type_enum = PT_TYPE_INTEGER;
    n->info.value.data_value.i = v;
    return n;
  }

  PT_NODE *cmp (PT_OP_TYPE op, int v)
  {
    PT_NODE *e = pt (PT_EXPR);
    e->info.expr.op = op;
    e->info.expr.arg1 = col_y ();
    e->info.expr.arg2 = ival (v);
    e->type_enum = PT_TYPE_LOGICAL;
    return e;
  }

  struct scene
  {
    PARSER_CONTEXT *parser;
    QO_ENV *env;
    PT_NODE *h1, *d2, *h2;

    scene ()
    {
      free_all ();
      g_abort_on = NULL;
      g_seen.clear ();
      g_er_sets = 0;

      parser = (PARSER_CONTEXT *) zalloc (sizeof (PARSER_CONTEXT));
      env = (QO_ENV *) zalloc (sizeof (QO_ENV));
      env->parser = parser;

      /* HAVING (y > 1 OR y < -1) AND y = 5 */
      h1 = cmp (PT_GT, 1);
      d2 = cmp (PT_LT, -1);
      h1->or_next = d2;
      h2 = cmp (PT_EQ, 5);
      h1->next = h2;
    }
  };

  bool near (double a, double b)
  {
    return std::fabs (a - b) <= 1e-12;
  }
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("PR8664 H0: control -- each HAVING predicate goes to the WHERE-term estimate alone (next and or_next cut "
	   "while it is estimated), the list is whole afterwards; (0.3 OR 0.3) AND 0.3", "[contract]")
{
  scene s;
  int aborted = -1;

  double sel = unitseam_having_selectivity (s.env, s.h1, &aborted);

  CHECK (aborted == 0);
  CHECK (near (sel, (0.3 + 0.3 - 0.09) * 0.3));
  REQUIRE (g_seen.size () == 3);
  CHECK (g_seen[0].pred == s.h1);
  CHECK (g_seen[0].next == NULL);
  CHECK (g_seen[0].or_next == NULL);
  CHECK (g_seen[1].pred == s.d2);
  CHECK (g_seen[2].pred == s.h2);
  CHECK (s.h1->next == s.h2);
  CHECK (s.h1->or_next == s.d2);
}

TEST_CASE ("PR8664 F1: an abort inside the WHERE-term estimate of a HAVING predicate (a QO_ASSERT of "
	   "qo_expr_selectivity () and its callees) leaves HAVING cut -- qo_having_disjunct_selectivity () cut h1->next "
	   "and h1->or_next by hand and qo_abort () does not know them: HAVING (y > 1 OR y < -1) AND y = 5 runs as "
	   "HAVING y > 1 (PASS = the defect is there)", "[defect]")
{
  scene s;
  int aborted = -1;

  g_abort_on = s.h1;
  (void) unitseam_having_selectivity (s.env, s.h1, &aborted);

  REQUIRE (aborted == 1);
  CHECK (g_er_sets == 1);
  CHECK (s.h1->next == NULL);	/* the conjunct y = 5 is gone */
  CHECK (s.h1->or_next == NULL);	/* the disjunct y < -1 is gone */
}
