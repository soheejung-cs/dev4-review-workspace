/*
 * PR#8136 (CBRD-27596) -- parallel heap scan checker, src/query/parallel/px_scan/px_scan_checker.cpp.
 * Mode cs: the checker runs while the client builds the XASL (xasl_generation.c 17675, 17909, 19491,
 * 23872, 27679 call scan_check_parallel_scan_possible); cubrid/CMakeLists.txt does not build this file
 * into cub_server, only cs/ and sa/ do.
 *
 * Claim of the PR: an SP not declared PARALLEL_ENABLE (or a method) anywhere in a regu-linked subquery
 * must keep the subquery out of a px worker, because the thread that evaluates the regu runs the whole
 * subquery. sibling_check () reports it as CANNOT_RUN_IN_WORKER, and check (REGU_VARIABLE *) turns it
 * into CANNOT_LIST_MERGE in an output list (row-by-row: main evaluates the output) and into
 * CANNOT_PARALLEL_SCAN elsewhere (serial: workers evaluate the WHERE).
 *
 * Finding F1 of our review (posted on the PR head 4a5256c99, px_scan_checker.cpp:588): the hierarchical
 * part of a subquery, connect_by_ptr, is not looked at -- START WITH (proc.connect_by.start_with_pred),
 * CONNECT BY (connect_by_ptr->if_pred, and the single-table iteration scan filter on its spec) and the
 * hierarchical WHERE (proc.connect_by.after_connect_by_pred). The [defect] cases assert that behaviour
 * (PASS = defect present); the "F1 fix" [contract] case asserts the opposite.
 *
 * XASL is built the way xasl_generation.c builds it: nodes zeroed then option = Q_ALL,
 * scan_op_type = S_SELECT, parallelism = -1 (regu_init (xasl_node &), xasl_regu_alloc.cpp:54-63); access
 * specs zeroed then ACCESS_METHOD_SEQUENTIAL, num_parallel_threads = -1 (regu_spec_target_init); a scalar
 * subquery is a TYPE_CONSTANT regu whose xasl is the subquery, flagged XASL_LINK_TO_REGU_VARIABLE
 * (pt_make_regu_subquery, 7245-7280), and a correlated one sits on the outer scan's dptr_list; a stored
 * function call is TYPE_SP with sp_ptr {sig, args, value} (7040-7076). The PARALLEL_ENABLE judge is the
 * real inline px_sp_is_parallel_eligible () (px_sp_eligibility.hpp): sig->is_parallel_enabled.
 */
#include "catch.hpp"
#include "px_scan_checker.hpp"
#include "regu_var.hpp"
#include "xasl.h"
#include "xasl_predicate.hpp"
#include "xasl_aggregate.hpp"
#include "xasl_sp.hpp"
#include "pl_signature.hpp"
#include "storage_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

/* ---------------------------------------------------------------- hooks (appendix, same TU) */
unsigned unitseam_px_flag (int which);
bool unitseam_px_has_sibling_cache ();
unsigned unitseam_px_check_regu (REGU_VARIABLE *regu, bool outptr);
unsigned unitseam_px_sibling_check (XASL_NODE *xasl, bool outptr);
size_t unitseam_px_sibling_cache_size (int is_outptr_list);

static unsigned PS ()		/* CANNOT_PARALLEL_SCAN */
{
  return unitseam_px_flag (0);
}

static unsigned LM ()		/* CANNOT_LIST_MERGE */
{
  return unitseam_px_flag (1);
}

static unsigned RIW ()		/* CANNOT_RUN_IN_WORKER; 0 at base, where the notion does not exist */
{
  return unitseam_px_flag (3);
}

static bool
has (unsigned flags, unsigned bit)
{
  return bit != 0 && (flags & bit) != 0;
}

/* ---------------------------------------------------------------- fakes */
namespace cubpl
{
  /* pl_signature.cpp:45-53: pl_arg () -> pl_arg (0) -> set_arg_size (0), which allocates nothing */
  pl_arg::pl_arg ()
    : arg_size (0), arg_mode (nullptr), arg_type (nullptr), arg_default_value_size (nullptr),
      arg_default_value (nullptr)
  {
  }

  /* pl_signature.cpp:163-177, the !SERVER_MODE branch (is_disposable = true). The checker reads only
   * is_parallel_enabled. The vtable is never used (no virtual call), so its auto-stub may stay zero. */
  pl_signature::pl_signature ()
    : type {PL_TYPE_NONE}, name {nullptr}, auth {nullptr}, result_type {0}
  {
    is_parallel_enabled = false;
    memset (&ext, 0, sizeof (pl_ext));
    is_disposable = true;
  }
}

namespace parallel_scan
{
  /* px_scan_instnum.cpp:197-206 with is_plain_instnum_buildlist (151-195), for the only shape that
   * reaches it here: outputs that are TYPE_CONSTANT regus, for which regu_subtree_refs_instnum is
   * "dbvalptr == instnum_val" (its TYPE_CONSTANT case). sibling_check () asks it once per XASL it
   * evaluates that has an inst_num, so the call count is a visit counter for the cache test. */
  int g_renumberable_calls = 0;

  bool
  is_renumberable_instnum (XASL_NODE *x)
  {
    g_renumberable_calls++;
    if (x == nullptr || x->instnum_pred != nullptr)
      {
	return false;
      }
    if (x->instnum_val == nullptr || x->save_instnum_val != nullptr || x->ordbynum_pred != nullptr
	|| x->ordbynum_val != nullptr || x->type != BUILDLIST_PROC || x->proc.buildlist.groupby_list != nullptr
	|| x->proc.buildlist.g_agg_list != nullptr || x->proc.buildlist.a_eval_list != nullptr
	|| x->outptr_list == nullptr)
      {
	return false;
      }
    int passthrough = 0;
    for (regu_variable_list_node *v = x->outptr_list->valptrp; v != nullptr; v = v->next)
      {
	if (v->value.type != TYPE_CONSTANT)
	  {
	    fprintf (stderr, "fake is_renumberable_instnum: only TYPE_CONSTANT outputs are modelled\n");
	    abort ();
	  }
	if (v->value.value.dbvalptr == x->instnum_val)
	  {
	    passthrough++;
	  }
      }
    return passthrough >= 1;
  }
}

/* ---------------------------------------------------------------- XASL builders */
template <typename T>
static T *
zalloc ()
{
  /* regu_alloc () + regu_init (): packing-buffer memory, zeroed, no constructor */
  return static_cast<T *> (calloc (1, sizeof (T)));
}

static XASL_NODE *
xnode (PROC_TYPE type)
{
  XASL_NODE *x = zalloc<XASL_NODE> ();
  x->type = type;
  x->option = Q_ALL;
  x->scan_op_type = S_SELECT;
  x->parallelism = -1;
  return x;
}

static ACCESS_SPEC_TYPE *
spec_of (TARGET_TYPE type, PRED_EXPR *where)
{
  ACCESS_SPEC_TYPE *s = zalloc<ACCESS_SPEC_TYPE> ();
  s->type = type;
  s->access = ACCESS_METHOD_SEQUENTIAL;
  s->num_parallel_threads = -1;
  s->where_pred = where;
  return s;
}

static ACCESS_SPEC_TYPE *
heap_spec (PRED_EXPR *where = nullptr)
{
  return spec_of (TARGET_CLASS, where);
}

static REGU_VARIABLE *
regu (REGU_DATATYPE type)
{
  REGU_VARIABLE *r = zalloc<REGU_VARIABLE> ();
  r->type = type;
  return r;
}

static REGU_VARIABLE *
attr ()				/* a column of the scanned table */
{
  REGU_VARIABLE *r = regu (TYPE_ATTR_ID);
  r->value.attr_descr.type = DB_TYPE_INTEGER;
  return r;
}

static REGU_VARIABLE *
cnst (DB_VALUE *v = nullptr)	/* a constant, or a value slot (aggregate result, ROWNUM) */
{
  REGU_VARIABLE *r = regu (TYPE_CONSTANT);
  r->value.dbvalptr = v ? v : zalloc<DB_VALUE> ();
  return r;
}

static REGU_VARIABLE_LIST
rlist (std::initializer_list<REGU_VARIABLE *> items)
{
  REGU_VARIABLE_LIST head = nullptr, *tail = &head;
  for (REGU_VARIABLE *r : items)
    {
      if (r == nullptr)
	{
	  continue;
	}
      REGU_VARIABLE_LIST n = zalloc<regu_variable_list_node> ();
      n->value = *r;
      *tail = n;
      tail = &n->next;
    }
  return head;
}

static OUTPTR_LIST *
outlist (std::initializer_list<REGU_VARIABLE *> items)
{
  OUTPTR_LIST *o = zalloc<OUTPTR_LIST> ();
  o->valptrp = rlist (items);
  for (REGU_VARIABLE_LIST n = o->valptrp; n; n = n->next)
    {
      o->valptr_cnt++;
    }
  return o;
}

/* a stored function call f (arg) */
static REGU_VARIABLE *
sp_call (bool parallel_enable, REGU_VARIABLE *arg = nullptr)
{
  REGU_VARIABLE *r = regu (TYPE_SP);
  SP_TYPE *sp = zalloc<SP_TYPE> ();
  sp->sig = new cubpl::pl_signature ();
  sp->sig->is_parallel_enabled = parallel_enable;
  sp->args = arg ? rlist ({arg}) : nullptr;
  sp->value = zalloc<DB_VALUE> ();
  r->value.sp_ptr = sp;
  return r;
}

static PRED_EXPR *
cmp (REGU_VARIABLE *lhs, REL_OP op, REGU_VARIABLE *rhs)
{
  PRED_EXPR *p = zalloc<PRED_EXPR> ();
  p->type = T_EVAL_TERM;
  p->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
  p->pe.m_eval_term.et.et_comp.lhs = lhs;
  p->pe.m_eval_term.et.et_comp.rhs = rhs;
  p->pe.m_eval_term.et.et_comp.rel_op = op;
  return p;
}

static PRED_EXPR *
and_ (PRED_EXPR *a, PRED_EXPR *b)
{
  PRED_EXPR *p = zalloc<PRED_EXPR> ();
  p->type = T_PRED;
  p->pe.m_pred.lhs = a;
  p->pe.m_pred.rhs = b;
  p->pe.m_pred.bool_op = B_AND;
  return p;
}

/* "f (col) > 0" with f declared or not */
static PRED_EXPR *
sp_filter (bool parallel_enable)
{
  return cmp (sp_call (parallel_enable, attr ()), R_GT, cnst ());
}

/* the correlation term "h.k = o.a" (no SP) */
static PRED_EXPR *
corr_term ()
{
  return cmp (attr (), R_EQ, attr ());
}

static AGGREGATE_TYPE *
agg (FUNC_CODE fn, REGU_VARIABLE *operand, REGU_VARIABLE *percentile = nullptr)
{
  AGGREGATE_TYPE *a = zalloc<AGGREGATE_TYPE> ();
  a->function = fn;
  a->option = Q_ALL;
  a->operands = operand ? rlist ({operand}) : nullptr;
  a->info.percentile.percentile_reguvar = percentile;
  return a;
}

/* scalar subquery regu: pt_make_regu_subquery () */
static REGU_VARIABLE *
subq_regu (XASL_NODE *sub)
{
  REGU_VARIABLE *r = cnst ();
  r->xasl = sub;
  XASL_SET_FLAG (sub, XASL_LINK_TO_REGU_VARIABLE);
  sub->is_single_tuple = 1;
  return r;
}

/* (select <sel> from h where <where>) */
static XASL_NODE *
subq_list (REGU_VARIABLE *sel, PRED_EXPR *where)
{
  XASL_NODE *x = xnode (BUILDLIST_PROC);
  x->spec_list = heap_spec (where);
  x->outptr_list = outlist ({sel ? sel : attr ()});
  return x;
}

/* (select <agg> from h where <where>) */
static XASL_NODE *
subq_value (AGGREGATE_TYPE *a, PRED_EXPR *where)
{
  XASL_NODE *x = xnode (BUILDVALUE_PROC);
  x->spec_list = heap_spec (where);
  x->proc.buildvalue.agg_list = a;
  x->outptr_list = outlist ({cnst ()});
  return x;
}

/* subquery locations of an SP, as the PR's new sibling_check block enumerates them */
enum sp_place
{
  IN_SELECT,			/* (select f (h.x) from h where h.k = o.a) */
  IN_WHERE,			/* (select h.x from h where h.k = o.a and f (h.x) > 0) */
  IN_AGG_OPERAND,		/* (select sum (f (h.x)) from h where h.k = o.a) */
  IN_PERCENTILE_RATIO,		/* (select percentile_cont (f (h.x)) within group (order by h.x) from h where ...) */
  IN_LATER_JOIN_TABLE		/* (select h.x from h, g where h.k = o.a and g.id = h.g and f (g.y) > 0) */
};

static XASL_NODE *
subq_with_sp (sp_place place, bool parallel_enable)
{
  switch (place)
    {
    case IN_SELECT:
      return subq_list (sp_call (parallel_enable, attr ()), corr_term ());
    case IN_WHERE:
      return subq_list (nullptr, and_ (corr_term (), sp_filter (parallel_enable)));
    case IN_AGG_OPERAND:
      return subq_value (agg (PT_SUM, sp_call (parallel_enable, attr ())), corr_term ());
    case IN_PERCENTILE_RATIO:
      return subq_value (agg (PT_PERCENTILE_CONT, attr (), sp_call (parallel_enable, attr ())), corr_term ());
    case IN_LATER_JOIN_TABLE:
    {
      XASL_NODE *x = subq_list (nullptr, corr_term ());
      XASL_NODE *g = xnode (SCAN_PROC);
      g->spec_list = heap_spec (and_ (cmp (attr (), R_EQ, attr ()), sp_filter (parallel_enable)));
      x->scan_ptr = g;
      return x;
    }
    }
  return nullptr;
}

/* hierarchical subquery (select count (*) from h ... start with ... connect by ...), with the SP placed
 * where pt_make_connect_by_proc () (xasl_generation.c 690-960) puts each clause. */
enum hq_place
{
  HQ_NONE,
  HQ_START_WITH,		/* join path: START WITH -> connect_by_ptr->proc.connect_by.start_with_pred (786-790) */
  HQ_CONNECT_BY_PRED,		/* CONNECT BY term -> connect_by_ptr->if_pred: join path every term (796-801); single
				 * table a term with LEVEL etc. (765-771) and the planner's if terms such as the PRIOR
				 * term (qo_add_hq_iterations_access_spec, plan_generation.c:4062-4065) */
  HQ_CONNECT_BY_SCAN_FILTER,	/* single table: a CONNECT BY term without PRIOR / HQ op -> the iteration scan's
				 * filter, connect_by_ptr->spec_list (742-750, plan_generation.c:4058-4060) */
  HQ_WHERE			/* hierarchical WHERE -> proc.connect_by.after_connect_by_pred (805-826) */
};

static XASL_NODE *
subq_hierarchical (hq_place place, bool parallel_enable = false)
{
  /* single-table path except for START WITH, which only the join path keeps on connect_by_ptr (the
   * single-table rewrite moves START WITH into the subquery's own WHERE, query_rewrite.c:87-92) */
  const bool join = (place == HQ_START_WITH);
  XASL_NODE *x = subq_value (agg (PT_COUNT_STAR, nullptr), join ? nullptr : corr_term ());
  XASL_NODE *cb = xnode (CONNECTBY_PROC);
  cb->proc.connect_by.single_table_opt = !join;
  if (join)
    {
      /* FROM h, g: g is the subquery's later join table, the CONNECT BY scan is a list scan */
      XASL_NODE *g = xnode (SCAN_PROC);
      g->spec_list = heap_spec (cmp (attr (), R_EQ, attr ()));
      x->scan_ptr = g;
      cb->spec_list = spec_of (TARGET_LIST, nullptr);
      cb->proc.connect_by.start_with_pred = and_ (corr_term (), sp_filter (parallel_enable));
    }
  else
    {
      cb->spec_list = heap_spec (place == HQ_CONNECT_BY_SCAN_FILTER
				 ? and_ (cmp (attr (), R_EQ, attr ()), sp_filter (parallel_enable)) : nullptr);
    }
  /* "prior h.id = h.pid" and, for HQ_CONNECT_BY_PRED, "level <= f (3)" (the SP could equally sit in the PRIOR term) */
  PRED_EXPR *connect_by = cmp (attr (), R_EQ, attr ());
  if (place == HQ_CONNECT_BY_PRED)
    {
      connect_by = and_ (connect_by, cmp (cnst (), R_LE, sp_call (parallel_enable, cnst ())));
    }
  cb->if_pred = connect_by;
  if (place == HQ_WHERE)
    {
      cb->proc.connect_by.after_connect_by_pred = sp_filter (parallel_enable);
    }
  x->connect_by_ptr = cb;
  XASL_SET_FLAG (x, XASL_HAS_CONNECT_BY);
  return x;
}

/* outer query: select o.a [, <subquery>] from o [where o.a > <subquery>], one heap spec, the correlated
 * subquery on the scan's dptr_list */
enum outer_place
{
  OUTER_SELECT,
  OUTER_WHERE
};

struct outer_query
{
  XASL_NODE *xasl;
  ACCESS_SPEC_TYPE *spec;
};

static outer_query
outer_with (REGU_VARIABLE *sel, PRED_EXPR *where, XASL_NODE *corr_subq)
{
  outer_query q;
  q.xasl = xnode (BUILDLIST_PROC);
  q.spec = heap_spec (where);
  q.xasl->spec_list = q.spec;
  q.xasl->outptr_list = outlist ({attr (), sel});
  q.xasl->dptr_list = corr_subq;
  return q;
}

static outer_query
outer_with_subq (outer_place place, XASL_NODE *sub)
{
  REGU_VARIABLE *r = subq_regu (sub);
  return place == OUTER_SELECT ? outer_with (r, nullptr, sub) : outer_with (nullptr, cmp (attr (), R_GT, r), sub);
}

/* the mode the outer heap scan ends up in, read from the spec flags as px_scan reads them */
static std::string
plan_of (outer_query q)
{
  scan_check_parallel_scan_possible (q.xasl);
  if (ACCESS_SPEC_IS_FLAGED (q.spec, ACCESS_SPEC_FLAG_NO_PARALLEL_SCAN))
    {
      return "serial";
    }
  if (ACCESS_SPEC_IS_FLAGED (q.spec, ACCESS_SPEC_FLAG_BUILDVALUE_OPT))
    {
      return "buildvalue-opt";
    }
  if (ACCESS_SPEC_IS_FLAGED (q.spec, ACCESS_SPEC_FLAG_MERGEABLE_LIST))
    {
      return "list-merge";
    }
  return "row-by-row";
}

static const char *const place_name[] = { "select list", "WHERE", "aggregate operand", "percentile ratio",
					  "later join table" };

/* ================================================================= [contract] */

TEST_CASE ("PR8136 contract: an outer query without SP runs list-merge (control)", "[contract]")
{
  CHECK (plan_of (outer_with (nullptr, nullptr, nullptr)) == "list-merge");
  CHECK (plan_of (outer_with_subq (OUTER_SELECT, subq_list (nullptr, corr_term ()))) == "list-merge");
  CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_list (nullptr, corr_term ()))) == "list-merge");
}

TEST_CASE ("PR8136 contract: an undeclared SP written in the outer query -- select list row-by-row, WHERE serial",
	   "[contract]")
{
  /* TYPE_SP rule, px_scan_checker.cpp:214-221 (unchanged in outcome by the PR) */
  unsigned out = unitseam_px_check_regu (sp_call (false, attr ()), true);
  unsigned whr = unitseam_px_check_regu (sp_call (false, attr ()), false);
  CHECK (has (out, LM ()));
  CHECK_FALSE (has (out, PS ()));
  CHECK (has (whr, PS ()));
  CHECK (plan_of (outer_with (sp_call (false, attr ()), nullptr, nullptr)) == "row-by-row");
  CHECK (plan_of (outer_with (nullptr, sp_filter (false), nullptr)) == "serial");
  CHECK (plan_of (outer_with (sp_call (true, attr ()), nullptr, nullptr)) == "list-merge");
  CHECK (plan_of (outer_with (nullptr, sp_filter (true), nullptr)) == "list-merge");
}

TEST_CASE ("PR8136 contract: TYPE_SP of an undeclared SP raises CANNOT_RUN_IN_WORKER (new bit)", "[contract]")
{
  REQUIRE (RIW () != 0);	/* base: the flag does not exist */
  CHECK (has (unitseam_px_check_regu (sp_call (false, attr ()), true), RIW ()));
  CHECK (has (unitseam_px_check_regu (sp_call (false, attr ()), false), RIW ()));
  CHECK_FALSE (has (unitseam_px_check_regu (sp_call (true, attr ()), false), RIW ()));
}

static void
expect_subquery_sp_blocks (sp_place place, const char *outer_select_plan)
{
  INFO ("SP in the subquery's " << place_name[place]);
  CHECK (has (unitseam_px_sibling_check (subq_with_sp (place, false), false), RIW ()));
  CHECK (plan_of (outer_with_subq (OUTER_SELECT, subq_with_sp (place, false))) == outer_select_plan);
  CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_with_sp (place, false))) == "serial");
}

TEST_CASE ("PR8136 contract: undeclared SP in a subquery's select list -> RIW; outer select list row-by-row, outer WHERE serial",
	   "[contract]")
{
  expect_subquery_sp_blocks (IN_SELECT, "row-by-row");
}

TEST_CASE ("PR8136 contract: undeclared SP in a subquery's WHERE -> RIW; serial in both outer places (stricter, as before the PR)",
	   "[contract]")
{
  /* the WHERE of the subquery's own scan is checked by sibling_check (ACCESS_SPEC_TYPE *) with
   * is_outptr_list = false, so the SP there yields CANNOT_PARALLEL_SCAN directly (base too) */
  expect_subquery_sp_blocks (IN_WHERE, "serial");
}

TEST_CASE ("PR8136 contract: undeclared SP in a subquery's aggregate operand -> RIW; outer select list row-by-row, outer WHERE serial",
	   "[contract]")
{
  expect_subquery_sp_blocks (IN_AGG_OPERAND, "row-by-row");
}

TEST_CASE ("PR8136 contract: undeclared SP as a subquery's PERCENTILE_CONT ratio -> RIW; outer select list row-by-row, outer WHERE serial",
	   "[contract]")
{
  expect_subquery_sp_blocks (IN_PERCENTILE_RATIO, "row-by-row");
}

TEST_CASE ("PR8136 contract: undeclared SP on a subquery's later join table -> RIW; outer select list row-by-row, outer WHERE serial",
	   "[contract]")
{
  expect_subquery_sp_blocks (IN_LATER_JOIN_TABLE, "row-by-row");
}

TEST_CASE ("PR8136 contract: a PARALLEL_ENABLE SP in any of those subquery places blocks nothing (list-merge kept)",
	   "[contract]")
{
  for (int p = IN_SELECT; p <= IN_LATER_JOIN_TABLE; p++)
    {
      INFO ("PARALLEL_ENABLE SP in the subquery's " << place_name[p]);
      CHECK_FALSE (has (unitseam_px_sibling_check (subq_with_sp ((sp_place) p, true), false), RIW ()));
      CHECK (plan_of (outer_with_subq (OUTER_SELECT, subq_with_sp ((sp_place) p, true))) == "list-merge");
      CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_with_sp ((sp_place) p, true))) == "list-merge");
    }
}

TEST_CASE ("PR8136 contract: sibling_check cache guards an aptr cycle and still finds the SP behind it", "[contract]")
{
  /* A.aptr = B, B.aptr = A, both regu-linked; B's select list calls an undeclared SP. The PR's new
   * "sibling_check<false> (aptr) & RIW" (line 570) recurses on aptr; the visited sentinel (557) stops it. */
  XASL_NODE *a = subq_list (nullptr, corr_term ());
  XASL_NODE *b = subq_list (sp_call (false, attr ()), nullptr);
  XASL_SET_FLAG (a, XASL_LINK_TO_REGU_VARIABLE);
  XASL_SET_FLAG (b, XASL_LINK_TO_REGU_VARIABLE);
  a->aptr_list = b;
  b->aptr_list = a;
  unsigned flags = unitseam_px_sibling_check (a, false);	/* returns at all: no unbounded recursion */
  CHECK (has (flags, RIW ()));
  CHECK (unitseam_px_has_sibling_cache ());
}

TEST_CASE ("PR8136 contract: sibling_check cache visits each nested correlated subquery once (no 2^depth)", "[contract]")
{
  /* L1 > L2 > ... > L12: L(k+1) is correlated in L(k)'s WHERE, so it is reached through its regu and through
   * L(k)'s dptr_list (px_scan_checker.cpp:93-94). Each level outputs its ROWNUM, so sibling_check asks
   * is_renumberable_instnum () once per level it evaluates (line 613). L12 calls an undeclared SP. */
  const int depth = 12;
  XASL_NODE *levels[depth];
  for (int i = depth - 1; i >= 0; i--)
    {
      DB_VALUE *rownum = zalloc<DB_VALUE> ();
      PRED_EXPR *where = (i == depth - 1) ? and_ (corr_term (), sp_filter (false))
			 : and_ (corr_term (), cmp (attr (), R_GT, subq_regu (levels[i + 1])));
      XASL_NODE *x = subq_list (cnst (rownum), where);
      x->instnum_val = rownum;
      x->dptr_list = (i == depth - 1) ? nullptr : levels[i + 1];
      levels[i] = x;
    }
  parallel_scan::g_renumberable_calls = 0;
  unsigned flags = unitseam_px_sibling_check (levels[0], false);
  INFO ("sibling_check evaluations = " << parallel_scan::g_renumberable_calls << " for " << depth << " levels");
  CHECK (parallel_scan::g_renumberable_calls <= 2 * depth);
  CHECK (has (flags, RIW ()));
  CHECK (has (flags, PS ()));
}

TEST_CASE ("PR8136 contract: a hierarchical subquery without SP keeps the outer plan (F1 fix does not over-block)",
	   "[contract]")
{
  /* the fix ORs the connect_by part into "temp", of which only RIW survives (line 703), so a
   * CANNOT_PARALLEL_SCAN coming from the CONNECTBY node itself changes nothing */
  for (int p = HQ_NONE; p <= HQ_WHERE; p++)
    {
      INFO ("hq place " << p << " with a PARALLEL_ENABLE SP (HQ_NONE: no SP)");
      CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_hierarchical ((hq_place) p, true))) == "list-merge");
      CHECK (plan_of (outer_with_subq (OUTER_SELECT, subq_hierarchical ((hq_place) p, true))) == "list-merge");
    }
}

TEST_CASE ("PR8136 F1 fix contract: an undeclared SP in START WITH / CONNECT BY / hierarchical WHERE of a subquery -> RIW; outer WHERE serial, outer select list row-by-row",
	   "[contract]")
{
  for (int p = HQ_START_WITH; p <= HQ_WHERE; p++)
    {
      INFO ("hq place " << p << " (1 START WITH, 2 CONNECT BY pred, 3 CONNECT BY scan filter, 4 WHERE)");
      CHECK (has (unitseam_px_sibling_check (subq_hierarchical ((hq_place) p), false), RIW ()));
      CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_hierarchical ((hq_place) p))) == "serial");
      CHECK (plan_of (outer_with_subq (OUTER_SELECT, subq_hierarchical ((hq_place) p))) == "row-by-row");
    }
}

/* ================================================================= [defect] (PASS = defect present) */

static void
expect_hq_sp_missed (hq_place place)
{
  CHECK_FALSE (has (unitseam_px_sibling_check (subq_hierarchical (place), false), RIW ()));
  /* the outer heap scan stays parallel, so a px worker evaluates o.a > (subquery) and runs the SP */
  CHECK (plan_of (outer_with_subq (OUTER_WHERE, subq_hierarchical (place))) != "serial");
}

TEST_CASE ("PR8136 F1: undeclared SP in START WITH (connect_by_ptr->start_with_pred) is not seen -- outer heap scan stays parallel",
	   "[defect]")
{
  expect_hq_sp_missed (HQ_START_WITH);
}

TEST_CASE ("PR8136 F1: undeclared SP in a CONNECT BY term (connect_by_ptr->if_pred) is not seen -- outer heap scan stays parallel",
	   "[defect]")
{
  expect_hq_sp_missed (HQ_CONNECT_BY_PRED);
}

TEST_CASE ("PR8136 F1: undeclared SP in a single-table CONNECT BY filter (connect_by_ptr spec where_pred) is not seen -- outer heap scan stays parallel",
	   "[defect]")
{
  expect_hq_sp_missed (HQ_CONNECT_BY_SCAN_FILTER);
}

TEST_CASE ("PR8136 F1: undeclared SP in the hierarchical WHERE (after_connect_by_pred) is not seen -- outer heap scan stays parallel",
	   "[defect]")
{
  expect_hq_sp_missed (HQ_WHERE);
}
