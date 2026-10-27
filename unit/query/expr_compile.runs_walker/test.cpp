/*
 * PR#8196 (CBRD-27361) -- the two filter-term walkers of the page batch (src/query/expr_compile.c, server mode):
 * expr_regu_runs () / expr_pred_runs () answer EXPR_RUNS_SIDE_EFFECT ("does a row's evaluation run a node with a
 * side effect") and EXPR_RUNS_QUERY ("does it run another query"); eb_build_filter () declines a plan whose
 * per-row filter term says yes to either (704a3af72 added the second question).
 *
 *   contract   a column reads nothing; a linked scalar subquery (TYPE_CONSTANT + xasl), a list (TYPE_LIST_ID + xasl)
 *              and EXISTS over a list run a query; T_RAND anywhere under the arithmetic is a side effect
 *   current    a scalar subquery the block precomputed before the scan opened (status XASL_SUCCESS,
 *              precomp_owner_regu) still counts as running a query -- the walker reads no status
 *   F1 defect  a T_CASE / T_IF / T_DECODE / T_PREDICATE node evaluates arith->pred through eval_pred ()
 *              (fetch_arith_ops.c fetch_op_case / fetch_op_predicate); the walker never descends into it, so a
 *              subquery or a side-effect operator inside the predicate is invisible to both questions
 *
 * Nothing is executed: the trees are built in memory and only walked.  No extra TU; every external symbol is a
 * weak stub (never reached by the walkers).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "regu_var.hpp"
#include "xasl.h"
#include "xasl_predicate.hpp"

#include <cstring>

extern bool unitseam_regu_runs_query (const REGU_VARIABLE * regu);
extern bool unitseam_regu_runs_side_effect (const REGU_VARIABLE * regu);
extern bool unitseam_pred_runs_query (const PRED_EXPR * pr);
extern bool unitseam_pred_runs_side_effect (const PRED_EXPR * pr);

namespace
{
  /* storage for the nodes of one case; zero-filled like regu_alloc () leaves them */
  struct arena
  {
    REGU_VARIABLE regu[16];
    ARITH_TYPE arith[8];
    PRED_EXPR pred[8];
    QFILE_SORTED_LIST_ID srlist[4];
    XASL_NODE *xasl_dummy[4];
    char xasl_mem[4][4096];
    int n_regu = 0, n_arith = 0, n_pred = 0, n_srlist = 0, n_xasl = 0;

    arena ()
    {
      memset (regu, 0, sizeof (regu));
      memset (arith, 0, sizeof (arith));
      memset (pred, 0, sizeof (pred));
      memset (srlist, 0, sizeof (srlist));
      memset (xasl_mem, 0, sizeof (xasl_mem));
    }

    XASL_NODE *xasl (XASL_STATUS st, REGU_VARIABLE *owner)
    {
      /* only the pointer, status and precomp_owner_regu are read; the rest stays zero */
      XASL_NODE *x = (XASL_NODE *) xasl_mem[n_xasl++];
      x->status = st;
      x->precomp_owner_regu = owner;
      return x;
    }

    REGU_VARIABLE *column ()
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_ATTR_ID;
      return r;
    }

    REGU_VARIABLE *literal ()
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_DBVAL;
      return r;
    }

    REGU_VARIABLE *scalar_subquery (XASL_STATUS st, bool precomputed)
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_CONSTANT;		/* pt_make_regu_subquery (): is_single_tuple */
      r->xasl = xasl (st, precomputed ? r : NULL);
      return r;
    }

    REGU_VARIABLE *list_subquery (XASL_STATUS st)
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      r->type = TYPE_LIST_ID;		/* pt_make_regu_subquery (): UNBOX_AS_TABLE */
      r->value.srlist_id = &srlist[n_srlist++];
      r->xasl = xasl (st, NULL);
      return r;
    }

    REGU_VARIABLE *arith_node (OPERATOR_TYPE op, REGU_VARIABLE *l, REGU_VARIABLE *rr, PRED_EXPR *p)
    {
      REGU_VARIABLE *r = &regu[n_regu++];
      ARITH_TYPE *a = &arith[n_arith++];
      a->opcode = op;
      a->leftptr = l;
      a->rightptr = rr;
      a->pred = p;
      r->type = TYPE_INARITH;
      r->value.arithptr = a;
      return r;
    }

    PRED_EXPR *comp (REGU_VARIABLE *l, REGU_VARIABLE *rr, REL_OP op)
    {
      PRED_EXPR *p = &pred[n_pred++];
      p->type = T_EVAL_TERM;
      p->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
      p->pe.m_eval_term.et.et_comp.lhs = l;
      p->pe.m_eval_term.et.et_comp.rhs = rr;
      p->pe.m_eval_term.et.et_comp.rel_op = op;
      return p;
    }
  };
}

TEST_CASE ("contract: a column and a literal run nothing", "[contract]")
{
  arena a;
  REGU_VARIABLE *cmp = a.arith_node (T_ADD, a.column (), a.literal (), NULL);

  CHECK_FALSE (unitseam_regu_runs_query (cmp));
  CHECK_FALSE (unitseam_regu_runs_side_effect (cmp));
}

TEST_CASE ("contract: a linked scalar subquery, a list and EXISTS run a query", "[contract]")
{
  arena a;
  REGU_VARIABLE *sq = a.scalar_subquery (XASL_INITIALIZED, false);
  REGU_VARIABLE *lst = a.list_subquery (XASL_INITIALIZED);
  PRED_EXPR *exists = a.comp (a.list_subquery (XASL_INITIALIZED), NULL, R_EXISTS);

  CHECK (unitseam_regu_runs_query (sq));
  CHECK (unitseam_regu_runs_query (lst));
  CHECK (unitseam_pred_runs_query (exists));
  /* none of them is a side effect: the subquery runs once, its result is reused when the row path evaluates again */
  CHECK_FALSE (unitseam_regu_runs_side_effect (sq));
  CHECK_FALSE (unitseam_regu_runs_side_effect (lst));
}

TEST_CASE ("contract: a random number under the arithmetic is a side effect", "[contract]")
{
  arena a;
  REGU_VARIABLE *rnd = a.arith_node (T_RAND, NULL, NULL, NULL);
  REGU_VARIABLE *sum = a.arith_node (T_ADD, a.column (), rnd, NULL);

  CHECK (unitseam_regu_runs_side_effect (rnd));
  CHECK (unitseam_regu_runs_side_effect (sum));
  CHECK_FALSE (unitseam_regu_runs_query (sum));
}

TEST_CASE ("current: a precomputed uncorrelated scalar subquery still counts as running a query", "[contract]")
{
  /* qexec_execute_mainblock_internal () runs every aptr subquery with precomp_owner_regu before the scans open, so
   * at the chunk its status is XASL_SUCCESS and EXECUTE_REGU_VARIABLE_XASL () executes nothing; the walker reads
   * neither field ("c_acctbal > (SELECT avg (c_acctbal) ...)" of TPC-H Q22 declines the customer scan's batch) */
  arena a;
  REGU_VARIABLE *sq = a.scalar_subquery (XASL_SUCCESS, true);
  REGU_VARIABLE *lst = a.list_subquery (XASL_SUCCESS);

  CHECK (sq->xasl->precomp_owner_regu == sq);
  CHECK (unitseam_regu_runs_query (sq));
  CHECK (unitseam_regu_runs_query (lst));
}

TEST_CASE ("F1 defect: EXISTS inside a CASE predicate is not seen as running a query", "[defect]")
{
  /* CASE WHEN EXISTS (SELECT ...) THEN 1 ELSE 0 END > 0 : the subquery is under arith->pred */
  arena a;
  PRED_EXPR *exists = a.comp (a.list_subquery (XASL_INITIALIZED), NULL, R_EXISTS);
  REGU_VARIABLE *cs = a.arith_node (T_CASE, a.literal (), a.literal (), exists);

  REQUIRE (unitseam_pred_runs_query (exists));	/* the predicate alone is caught */
  CHECK_FALSE (unitseam_regu_runs_query (cs));	/* PASS = defect: the node that evaluates it is not */
}

TEST_CASE ("F1 defect: a scalar subquery inside IF () is not seen as running a query", "[defect]")
{
  /* IF (c > (SELECT max (x) FROM t2), 1, 0) = 1 */
  arena a;
  PRED_EXPR *gt = a.comp (a.column (), a.scalar_subquery (XASL_INITIALIZED, false), R_GT);
  REGU_VARIABLE *iff = a.arith_node (T_IF, a.literal (), a.literal (), gt);

  REQUIRE (unitseam_pred_runs_query (gt));
  CHECK_FALSE (unitseam_regu_runs_query (iff));	/* PASS = defect */
}

TEST_CASE ("F1 defect: a random number inside a CASE predicate is not seen as a side effect", "[defect]")
{
  /* CASE WHEN RAND () < 0.5 THEN a ELSE b END > 0 : evaluated again on the row path after a fallback */
  arena a;
  PRED_EXPR *lt = a.comp (a.arith_node (T_RAND, NULL, NULL, NULL), a.literal (), R_LT);
  REGU_VARIABLE *cs = a.arith_node (T_CASE, a.column (), a.column (), lt);

  REQUIRE (unitseam_pred_runs_side_effect (lt));
  CHECK_FALSE (unitseam_regu_runs_side_effect (cs));	/* PASS = defect */
}

TEST_CASE ("F1 defect: a session variable assignment inside a boolean-valued predicate is not a side effect",
	   "[defect]")
{
  /* (@n := @n + 1) > 10 used as a value (T_PREDICATE) */
  arena a;
  PRED_EXPR *gt = a.comp (a.arith_node (T_DEFINE_VARIABLE, a.literal (), a.literal (), NULL), a.literal (), R_GT);
  REGU_VARIABLE *pv = a.arith_node (T_PREDICATE, NULL, NULL, gt);

  REQUIRE (unitseam_pred_runs_side_effect (gt));
  CHECK_FALSE (unitseam_regu_runs_side_effect (pv));	/* PASS = defect */
}
