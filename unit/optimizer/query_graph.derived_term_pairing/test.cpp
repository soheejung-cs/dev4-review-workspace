/*
 * PR#8375 (CBRD-27855) -- qo_derived_term_compensate () / qo_derived_term_origin () of
 * src/optimizer/query_graph.c (CS_MODE), reached through the appendix hook (hooks.cpp).
 *
 * The pass pairs every OR-derived term (QO_TERM_OR_DERIVED) with the predicate that implies it and
 * divides the origin by the product of what it implies; a term counted at a node scan
 * (qo_node_add_sarg (), class QO_TC_SARG only) is paid back from its origin, so node x join
 * products charge one constraint once.  The planner's join product skips every OR-derived term
 * (query_planner.c:11678), so a derived term that is NOT a sarg is counted nowhere.
 *
 * Real code: the TU static pass and its origin search, the bitset (query_bitset.c #included below).
 * No fakes are reached: the pass reads only QO_TERM fields, the PT_NODE op / or_next of each
 * candidate, malloc/free.  QO_TERM fields are filled as qo_analyze_term () /
 * qo_classify_outerjoin_terms () leave them.
 *
 * nodes: 0 = o (outer / preserved), 1 = i (inner / null-supplying)
 * segs : 0 o.k  1 o.a  2 o.b  3 i.k  4 i.x  5 i.y
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "optimizer.h"
#include "query_graph.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "query_bitset.c"

void unitseam_derived_compensate (QO_ENV * env);

static std::vector<void *> g_alloc;
static QO_ENV *g_env;

static void *
zalloc (size_t n)
{
  void *p = calloc (1, n);
  g_alloc.push_back (p);
  return p;
}

static void
scene (int max_terms)
{
  for (void *p:g_alloc)
    {
      free (p);
    }
  g_alloc.clear ();
  g_env = (QO_ENV *) zalloc (sizeof (QO_ENV));
  g_env->terms = (QO_TERM *) zalloc (sizeof (QO_TERM) * max_terms);
  g_env->Nterms = max_terms;
  g_env->nterms = 0;
}

static PT_NODE *
ex (PT_OP_TYPE op)
{
  PT_NODE *n = (PT_NODE *) zalloc (sizeof (PT_NODE));
  n->node_type = PT_EXPR;
  n->info.expr.op = op;
  return n;
}

/* `a = 1 or b = 2` as pt_cnf () leaves a CNF disjunction: an or_next chain */
static PT_NODE *
chain2 (void)
{
  PT_NODE *h = ex (PT_EQ);
  h->or_next = ex (PT_EQ);
  return h;
}

static QO_TERM *
term (PT_NODE * pt, QO_TERMCLASS tc, double sel, int flags, short location, std::initializer_list<int> nodes,
      std::initializer_list<int> segs)
{
  QO_TERM *t = QO_ENV_TERM (g_env, g_env->nterms);
  t->env = g_env;
  t->idx = g_env->nterms;
  t->pt_expr = pt;
  t->term_class = tc;
  t->selectivity = sel;
  t->flag = flags;
  t->location = location;
  t->can_use_index = 1;
  bitset_init (&t->nodes, g_env);
  bitset_init (&t->segments, g_env);
  for (int n:nodes)
    {
      bitset_add (&t->nodes, n);
    }
  for (int s:segs)
    {
      bitset_add (&t->segments, s);
    }
  g_env->nterms++;
  return t;
}

static const int OR_D = QO_TERM_OR_DERIVED;
static const int OR_DX = QO_TERM_OR_DERIVED | QO_TERM_OR_DERIVED_EXPENSIVE;
static const int EQ1 = QO_TERM_SINGLE_PRED | QO_TERM_EQUAL_OP;
static const int ORP = QO_TERM_SINGLE_PRED | QO_TERM_OR_PRED;

static bool
near (double a, double b)
{
  return std::fabs (a - b) <= 1e-9 * std::max (1.0, std::fabs (b));
}

/* ---------------------------------------------------------------- the design (contracts) */

TEST_CASE ("C1 a key range on one column pairs with its spec's restriction, which pairs with the factor: node i keeps s(R_i)",
	   "[contract]")
{
  scene (8);
  /* F = (o.a = 1 and i.x = 1 and i.y = 1) or (o.a = 2 and i.x = 2 and i.y = 2) */
  QO_TERM *f = term (ex (PT_OR), QO_TC_JOIN, 0.001, ORP, 0, { 0, 1 }, { 1, 4, 5 });
  QO_TERM *ro = term (ex (PT_RANGE), QO_TC_SARG, 0.1, OR_D, 0, { 0 }, { 1 });
  QO_TERM *ri = term (ex (PT_OR), QO_TC_SARG, 0.05, OR_D, 0, { 1 }, { 4, 5 });
  QO_TERM *kx = term (ex (PT_RANGE), QO_TC_SARG, 0.2, OR_DX, 0, { 1 }, { 4 });
  QO_TERM *ky = term (ex (PT_RANGE), QO_TC_SARG, 0.4, OR_DX, 0, { 1 }, { 5 });

  unitseam_derived_compensate (g_env);

  CHECK (near (ri->selectivity * kx->selectivity * ky->selectivity, 0.05));	/* node i = s (R_i) */
  CHECK (near (ro->selectivity, 0.1));
  CHECK (near (f->selectivity, 0.001 / (0.1 * 0.05)));
}

TEST_CASE ("C2 an ON-clause restriction on the preserved side (location > 0, during-join) leaves its origin undivided",
	   "[contract]")
{
  scene (4);
  term (ex (PT_EQ), QO_TC_JOIN, 0.01, EQ1, 1, { 0, 1 }, { 0, 3 });
  QO_TERM *f = term (ex (PT_OR), QO_TC_JOIN, 0.02, ORP, 1, { 0, 1 }, { 1, 4 });
  term (ex (PT_RANGE), QO_TC_DURING_JOIN, 0.1, OR_D, 1, { 0 }, { 1 });

  unitseam_derived_compensate (g_env);

  CHECK (near (f->selectivity, 0.02));
}

TEST_CASE ("C3 a WHERE restriction that stays a sarg (location 0, inner join) divides its origin", "[contract]")
{
  scene (4);
  term (ex (PT_EQ), QO_TC_JOIN, 0.01, EQ1, 0, { 0, 1 }, { 0, 3 });
  QO_TERM *f = term (ex (PT_OR), QO_TC_JOIN, 0.02, ORP, 0, { 0, 1 }, { 1, 4, 5 });
  term (chain2 (), QO_TC_SARG, 0.1, OR_D | QO_TERM_OR_PRED, 0, { 1 }, { 4, 5 });

  unitseam_derived_compensate (g_env);

  CHECK (near (f->selectivity, 0.2));
}

/* ---------------------------------------------------------------- finding 1 */

TEST_CASE ("D1 a WHERE restriction on the null-supplying side, classified after-join, still divides its origin",
	   "[defect]")
{
  /* o LEFT JOIN i ON o.k = i.k WHERE (o.a = 1 and i.x = 1) or i.y = 2
   *   F   : 2 nodes, i is LEFT OUTER          -> QO_TC_AFTER_JOIN (query_graph.c:6735 path, W2)
   *   R_i : i.x = 1 or i.y = 2, location 0     -> QO_TC_AFTER_JOIN (query_graph.c:6732-6735, W1)
   * R_i is not a sarg, so qo_node_add_sarg () never counts it, and the join product skips it as
   * OR-derived; dividing F by s (R_i) charges nobody: F alone should keep 0.02 */
  scene (4);
  term (ex (PT_EQ), QO_TC_JOIN, 0.01, EQ1, 1, { 0, 1 }, { 0, 3 });
  QO_TERM *f = term (ex (PT_OR), QO_TC_AFTER_JOIN, 0.02, ORP, 0, { 0, 1 }, { 1, 4, 5 });
  term (chain2 (), QO_TC_AFTER_JOIN, 0.1, OR_D | QO_TERM_OR_PRED, 0, { 1 }, { 4, 5 });

  unitseam_derived_compensate (g_env);

  CHECK (near (f->selectivity, 0.02 / 0.1));	/* PASS = the after-join filter loses 1/s(R_i) */
}

/* ---------------------------------------------------------------- finding 2 */

/* G (o, i) over o.a, i.x; F1 (i alone, kept whole) over i.x, i.y; their derived terms */
static double
two_factor_scene (bool o_first, QO_TERM ** r_i_g)
{
  scene (8);
  term (ex (PT_EQ), QO_TC_JOIN, 0.001, EQ1, 0, { 0, 1 }, { 0, 3 });
  term (ex (PT_OR), QO_TC_JOIN, 0.0001, ORP, 0, { 0, 1 }, { 1, 4 });	/* G */
  QO_TERM *f1 = term (ex (PT_OR), QO_TC_SARG, 0.3, ORP, 0, { 1 }, { 4, 5 });	/* F1 */
  QO_TERM *rig;
  if (o_first)
    {
      term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 0 }, { 1 });	/* R_o^G */
      rig = term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 1 }, { 4 });	/* R_i^G */
    }
  else
    {
      rig = term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 1 }, { 4 });
      term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 0 }, { 1 });
    }
  QO_TERM *kx = term (ex (PT_RANGE), QO_TC_SARG, 0.9, OR_DX, 0, { 1 }, { 4 });	/* K_x^F1 */
  QO_TERM *ky = term (ex (PT_RANGE), QO_TC_SARG, 0.4, OR_DX, 0, { 1 }, { 5 });	/* K_y^F1 */

  unitseam_derived_compensate (g_env);

  *r_i_g = rig;
  return f1->selectivity * rig->selectivity * kx->selectivity * ky->selectivity;	/* node i */
}

TEST_CASE ("C4 FROM i, o: G's restriction on i pairs with G, node i = s(F1) * s(R_i^G)", "[contract]")
{
  QO_TERM *rig;
  double node_i = two_factor_scene (false, &rig);

  CHECK (near (node_i, 0.3 * 0.02));
  CHECK (near (rig->selectivity, 0.02));
}

TEST_CASE ("D2 FROM o, i: G's restriction on i pairs with the single-spec factor F1 and is inflated by its correction",
	   "[defect]")
{
  QO_TERM *rig;
  double node_i = two_factor_scene (true, &rig);

  CHECK (near (rig->selectivity, 0.75));	/* 0.02 * 0.3 / (0.02 * 0.4): the key range of i.x priced at 75% */
  CHECK (near (node_i, 0.27));	/* vs 0.006 */
}

TEST_CASE ("D2b the scan estimate of i depends on the FROM order", "[defect]")
{
  QO_TERM *rig;
  double a = two_factor_scene (true, &rig);
  double b = two_factor_scene (false, &rig);

  CHECK (!near (a, b));
}

TEST_CASE ("D3 a restriction of factor G pairs with factor H's restriction on the same spec (nested), not with G",
	   "[defect]")
{
  scene (10);
  term (ex (PT_EQ), QO_TC_JOIN, 0.001, EQ1, 0, { 0, 1 }, { 0, 3 });
  QO_TERM *g = term (ex (PT_OR), QO_TC_JOIN, 0.0001, ORP, 0, { 0, 1 }, { 1, 4 });	/* G */
  term (ex (PT_OR), QO_TC_JOIN, 0.0002, ORP, 0, { 0, 1 }, { 2, 4, 5 });	/* H */
  term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 0 }, { 1 });	/* R_o^G */
  QO_TERM *rig = term (ex (PT_RANGE), QO_TC_SARG, 0.02, OR_D, 0, { 1 }, { 4 });	/* R_i^G */
  term (ex (PT_RANGE), QO_TC_SARG, 0.03, OR_D, 0, { 0 }, { 2 });	/* R_o^H */
  QO_TERM *rih = term (ex (PT_OR), QO_TC_SARG, 0.05, OR_D, 0, { 1 }, { 4, 5 });	/* R_i^H (nested-OR shape) */
  QO_TERM *kx = term (ex (PT_RANGE), QO_TC_SARG, 0.3, OR_DX, 0, { 1 }, { 4 });	/* K_x^H */
  QO_TERM *ky = term (ex (PT_RANGE), QO_TC_SARG, 0.4, OR_DX, 0, { 1 }, { 5 });	/* K_y^H */

  unitseam_derived_compensate (g_env);

  double node_i = rig->selectivity * rih->selectivity * kx->selectivity * ky->selectivity;
  CHECK (near (node_i, 0.05));	/* = s(R_i^H): R_i^G's filter is gone from the scan (intended 0.02 * 0.05) */
  CHECK (near (g->selectivity, 0.0001 / 0.02));	/* G paid back R_o^G only */
}

/* ---------------------------------------------------------------- finding 3 */

TEST_CASE ("D4 a derived term that is also an origin loses the raise its own origin's correction gave it", "[defect]")
{
  /* F = (o.k = i.k and i.x = 1 and i.y = 1) or (o.k = i.k and i.x = 2 and i.y = 2): no restriction on o,
   * R_i (nested-OR) with K_x, K_y; estimates inconsistent so that s(R_i) < s(F) and the containment
   * correction fires at F.  The correction's contract: F and what it implies multiply to s(F) */
  scene (4);
  QO_TERM *f = term (ex (PT_OR), QO_TC_JOIN, 0.05, ORP, 0, { 0, 1 }, { 0, 3, 4, 5 });
  QO_TERM *ri = term (ex (PT_OR), QO_TC_SARG, 0.04, OR_D, 0, { 1 }, { 4, 5 });
  QO_TERM *kx = term (ex (PT_RANGE), QO_TC_SARG, 0.4, OR_DX, 0, { 1 }, { 4 });
  QO_TERM *ky = term (ex (PT_RANGE), QO_TC_SARG, 0.5, OR_DX, 0, { 1 }, { 5 });

  unitseam_derived_compensate (g_env);

  double total = f->selectivity * ri->selectivity * kx->selectivity * ky->selectivity;
  CHECK (near (total, 0.04));	/* PASS = defect: s(R_i) instead of s(F) = 0.05 */
}
