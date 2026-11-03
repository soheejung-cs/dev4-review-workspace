/*
 * CBRD-27146 -- qo_plan_cmp () of src/optimizer/query_planner.c (CS_MODE), reached through hooks.cpp:
 *   head: two plans of a query without LIMIT are ranked by one value of each plan, its cost (less the analytic sort an
 *         analytic index scan saves) with a share of 1% for each sort a plan of one scan does not skip and off for its
 *         covering index scan (qo_plan_pref_cost ()); two plans whose values fall into one cell of a 1e-6 log grid by
 *         values of each plan in a fixed order (qo_plan_tie_cmp ()). Two plans of a LIMIT query, or of a query with an
 *         INST_NUM () term or a forced filter index (planner->plan_cmp_by_rules), keep the band and its rules
 *         (qo_plan_cmp_limit ()).
 *   base: the band (cost difference < 50 and ratio < 1.2, x10 under LIMIT) lets about twenty rules pick the plan.
 *
 * Real code: the TU (qo_plan_cmp and what it calls in query_planner.c), query_bitset.c, system_parameter.c (prm_Def
 * static initializers: cost_seq_page, cost_cpu_tuples_per_page for the analytic sort cost).
 * Fakes: the five plan predicates of plan_generation.c that the rules call (qo_is_index_covering_scan,
 * qo_is_index_iss_scan, qo_is_index_loose_scan, qo_is_index_mro_scan, qo_plan_multi_range_opt), copied from that file
 * without their asserts, and qo_is_filter_index () of query_graph.c; db_Io_page_size = 16K.
 * Structures: calloc'ed; only the fields the comparison reads are set (index_equi so the base rules take the key count
 * from the index statistics instead of a term selectivity).
 * The [contract] cases over generated sets run on the head only: on base the rules read fields of the scans and
 * terms that the generated plans do not fill -- base's lack of a total order is what D2 shows.
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "system_parameter.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "query_bitset.h"
#include "storage_common.h"
#include "string_regex.hpp"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

PGLENGTH db_Io_page_size = 16384;

/* plan_generation.c, without the asserts */
bool
qo_is_index_covering_scan (QO_PLAN * plan)
{
  return qo_is_interesting_order_scan (plan) && plan->plan_un.scan.index_cover;
}

bool
qo_is_index_iss_scan (QO_PLAN * plan)
{
  return qo_is_interesting_order_scan (plan) && plan->plan_un.scan.index_iss;
}

bool
qo_is_index_loose_scan (QO_PLAN * plan)
{
  return plan != NULL && plan->plan_type == QO_PLANTYPE_SCAN
    && (plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_SCAN
	|| plan->plan_un.scan.scan_method == QO_SCANMETHOD_INDEX_SCAN_INSPECT) && plan->plan_un.scan.index_loose;
}

bool
qo_is_index_mro_scan (QO_PLAN * plan)
{
  return qo_is_interesting_order_scan (plan) && plan->multi_range_opt_use == PLAN_MULTI_RANGE_OPT_USE;
}

bool
qo_plan_multi_range_opt (QO_PLAN * plan)
{
  return plan != NULL && plan->multi_range_opt_use == PLAN_MULTI_RANGE_OPT_USE;
}

/* query_graph.c */
bool
qo_is_filter_index (QO_INDEX_ENTRY * ent)
{
  return ent != NULL && ent->constraints != NULL && ent->constraints->filter_predicate != NULL && ent->force > 0;
}

QO_PLAN_COMPARE_RESULT unitseam_plan_cmp (QO_PLAN * a, QO_PLAN * b);
int unitseam_has_cost_order (void);
void unitseam_set_cmp_by_rules (QO_PLANNER * planner, int on);
int unitseam_plan_cmp_by_rules (QO_PLANNER * planner);

namespace
{
  struct scene
  {
    std::vector<void *> mem;
    PT_NODE *sel;
    QO_ENV *env;
    QO_PLANNER *planner;

    void *alloc (size_t n)
    {
      void *p = calloc (1, n);
      mem.push_back (p);
      return p;
    }

    explicit scene (bool limit)
    {
      sel = (PT_NODE *) alloc (sizeof (PT_NODE));
      sel->node_type = PT_SELECT;
      if (limit)
	{
	  /* QO_PLAN_HAS_LIMIT () looks only at its presence */
	  sel->info.query.limit = (PT_NODE *) alloc (sizeof (PT_NODE));
	}
      env = (QO_ENV *) alloc (sizeof (QO_ENV));
      env->pt_tree = sel;
      planner = (QO_PLANNER *) alloc (sizeof (QO_PLANNER));
    }

    ~scene ()
    {
      for (void *p : mem)
	{
	  free (p);
	}
    }

    QO_NODE *node (const char *alias, int idx)
    {
      QO_NODE *n = (QO_NODE *) alloc (sizeof (QO_NODE));
      n->env = env;
      n->class_name = alias;
      n->idx = idx;
      n->ncard = 3000;
      n->tcard = 50;
      return n;
    }

    QO_INFO *info (double card, int width)
    {
      QO_INFO *i = (QO_INFO *) alloc (sizeof (QO_INFO));
      i->env = env;
      i->planner = planner;
      i->cardinality = card;
      i->projected_size = width;
      return i;
    }

    QO_NODE_INDEX_ENTRY *index (const char *name, int pages)
    {
      QO_NODE_INDEX_ENTRY *ni = (QO_NODE_INDEX_ENTRY *) alloc (sizeof (QO_NODE_INDEX_ENTRY));
      QO_INDEX_ENTRY *e = (QO_INDEX_ENTRY *) alloc (sizeof (QO_INDEX_ENTRY));
      SM_CLASS_CONSTRAINT *c = (SM_CLASS_CONSTRAINT *) alloc (sizeof (SM_CLASS_CONSTRAINT));
      c->name = name;
      e->constraints = c;
      e->col_num = 1;
      e->cover_segments = true;
      ni->head = e;
      ni->n = 1;
      ni->cum_stats.pages = pages;
      ni->cum_stats.leafs = pages > 1 ? pages - 1 : 1;
      ni->cum_stats.height = 2;
      ni->cum_stats.keys = 1000;
      ni->cum_stats.pkeys_size = 1;
      ni->cum_stats.pkeys = (INT64 *) alloc (sizeof (INT64));
      ni->cum_stats.pkeys[0] = 1000;
      return ni;
    }

    QO_PLAN *plan (QO_PLANTYPE type, QO_INFO * i, double cost)
    {
      QO_PLAN *p = (QO_PLAN *) alloc (sizeof (QO_PLAN));
      p->plan_type = type;
      p->info = i;
      p->top_rooted = true;
      p->fixed_io_cost = cost;
      p->multi_range_opt_use = PLAN_MULTI_RANGE_OPT_NO;
      return p;
    }

    QO_PLAN *seq (QO_NODE * n, QO_INFO * i, double cost)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_SCAN, i, cost);
      p->plan_un.scan.scan_method = QO_SCANMETHOD_SEQ_SCAN;
      p->plan_un.scan.node = n;
      bitset_init (&p->plan_un.scan.terms, env);
      bitset_init (&p->plan_un.scan.kf_terms, env);
      return p;
    }

    QO_PLAN *iscan (QO_NODE * n, QO_INFO * i, QO_NODE_INDEX_ENTRY * ix, double cost, int nterms, int nkf, bool cover,
		    QO_SCANMETHOD method = QO_SCANMETHOD_INDEX_SCAN)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_SCAN, i, cost);
      p->plan_un.scan.scan_method = method;
      p->plan_un.scan.node = n;
      p->plan_un.scan.index = ix;
      p->plan_un.scan.index_equi = true;
      p->plan_un.scan.index_cover = cover;
      bitset_init (&p->plan_un.scan.terms, env);
      bitset_init (&p->plan_un.scan.kf_terms, env);
      for (int k = 0; k < nterms; k++)
	{
	  bitset_add (&p->plan_un.scan.terms, k);
	}
      for (int k = 0; k < nkf; k++)
	{
	  bitset_add (&p->plan_un.scan.kf_terms, nterms + k);
	}
      return p;
    }

    QO_PLAN *sort (QO_PLAN * sub, SORT_TYPE type, double extra)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_SORT, sub->info, sub->fixed_io_cost + extra);
      p->plan_un.sort.sort_type = type;
      p->plan_un.sort.subplan = sub;
      return p;
    }

    QO_PLAN *join (QO_PLAN * outer, QO_PLAN * inner, QO_JOINMETHOD method, QO_INFO * i, double cost)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_JOIN, i, cost);
      p->plan_un.join.join_type = JOIN_INNER;
      p->plan_un.join.join_method = method;
      p->plan_un.join.outer = outer;
      p->plan_un.join.inner = inner;
      bitset_init (&p->plan_un.join.join_terms, env);
      bitset_init (&p->plan_un.join.during_join_terms, env);
      bitset_init (&p->plan_un.join.other_outer_join_terms, env);
      bitset_init (&p->plan_un.join.after_join_terms, env);
      bitset_init (&p->plan_un.join.hash_terms, env);
      return p;
    }
  };

  int sgn (QO_PLAN_COMPARE_RESULT r)
  {
    return r == PLAN_COMP_LT ? -1 : r == PLAN_COMP_GT ? 1 : r == PLAN_COMP_EQ ? 0 : 9;
  }

  /* antisymmetry and transitivity of the comparison over a set: the number of violations */
  int order_violations (const std::vector<QO_PLAN *> &set, std::string &first)
  {
    size_t n = set.size ();
    std::vector<int> c (n * n);
    int bad = 0;

    for (size_t i = 0; i < n; i++)
      {
	for (size_t j = 0; j < n; j++)
	  {
	    c[i * n + j] = sgn (unitseam_plan_cmp (set[i], set[j]));
	  }
      }
    for (size_t i = 0; i < n; i++)
      {
	for (size_t j = 0; j < n; j++)
	  {
	    if (c[i * n + j] == 9 || c[i * n + j] != -c[j * n + i])
	      {
		if (bad++ == 0)
		  {
		    first = "antisymmetry " + std::to_string (i) + " " + std::to_string (j);
		  }
	      }
	    for (size_t k = 0; k < n; k++)
	      {
		int ij = c[i * n + j], jk = c[j * n + k], ik = c[i * n + k];
		if ((ij <= 0 && jk <= 0 && ik > 0) || (ij == 0 && jk == 0 && ik != 0))
		  {
		    if (bad++ == 0)
		      {
			first = "transitivity " + std::to_string (i) + " " + std::to_string (j) + " " + std::to_string (k);
		      }
		  }
	      }
	  }
      }
    return bad;
  }

  /* what qo_check_planvec () keeps of a set offered in this order: a plan replaces the kept one on PLAN_COMP_GT */
  QO_PLAN *kept_of (const std::vector<QO_PLAN *> &order)
  {
    QO_PLAN *kept = order[0];
    for (size_t k = 1; k < order.size (); k++)
      {
	if (unitseam_plan_cmp (kept, order[k]) == PLAN_COMP_GT)
	  {
	    kept = order[k];
	  }
      }
    return kept;
  }
}

TEST_CASE ("C1 without LIMIT the cheaper plan wins over the rules (F1: iscan 99 vs sscan 56)", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (2990.0, 220);
  QO_PLAN *ix = s.iscan (t, i, s.index ("i_t_a", 10), 99.0, 1, 0, false);
  QO_PLAN *sq = s.seq (t, i, 56.0);

  CHECK (unitseam_plan_cmp (ix, sq) == PLAN_COMP_GT);
  CHECK (unitseam_plan_cmp (sq, ix) == PLAN_COMP_LT);
}

TEST_CASE ("C2 a plan compared with itself is equal", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *sq = s.seq (t, i, 56.0);

  CHECK (unitseam_plan_cmp (sq, sq) == PLAN_COMP_EQ);
}

TEST_CASE ("C3 within its share of the cost the covering index scan wins (101.0 covering vs 100.3)", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_c", 20), 101.0, 1, 0, true);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_p", 20), 100.3, 1, 0, false);

  CHECK (unitseam_plan_cmp (cov, plain) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (plain, cov) == PLAN_COMP_GT);
}

TEST_CASE ("C4 beyond its share the cheaper plan wins even when the other is covering (101.5 vs 100.3)",
	   "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_c", 20), 101.5, 1, 0, true);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_p", 20), 100.3, 1, 0, false);

  CHECK (unitseam_plan_cmp (cov, plain) == PLAN_COMP_GT);
  CHECK (unitseam_plan_cmp (plain, cov) == PLAN_COMP_LT);
}

TEST_CASE ("C5 under LIMIT the ORDER BY skip keeps winning as on develop (F2: iscan 99 vs sort over sscan 89)",
	   "[contract]")
{
  scene s (true);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (3000.0, 220);
  QO_NODE_INDEX_ENTRY *ix = s.index ("i_t_a", 10);
  QO_PLAN *skip, *sorted;

  ix->head->orderby_skip = true;
  skip = s.iscan (t, i, ix, 99.0, 1, 0, false);
  sorted = s.sort (s.seq (t, i, 56.0), SORT_ORDERBY, 33.0);

  CHECK (unitseam_plan_cmp (skip, sorted) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (sorted, skip) == PLAN_COMP_GT);
}

TEST_CASE ("C6 two join orders of equal cost: the same one wins whatever the FROM list order", "[contract]")
{
  for (int from_ba = 0; from_ba < 2; from_ba++)
    {
      scene s (false);
      QO_NODE *a = s.node ("a", from_ba ? 1 : 0);
      QO_NODE *b = s.node ("b", from_ba ? 0 : 1);
      QO_INFO *ia = s.info (100.0, 64), *ib = s.info (100.0, 64), *iab = s.info (100.0, 128);
      QO_PLAN *ab = s.join (s.seq (a, ia, 10.0), s.seq (b, ib, 10.0), QO_JOINMETHOD_NL_JOIN, iab, 500.0);
      QO_PLAN *ba = s.join (s.seq (b, ib, 10.0), s.seq (a, ia, 10.0), QO_JOINMETHOD_NL_JOIN, iab, 500.0);

      INFO ("FROM " << (from_ba ? "b, a" : "a, b"));
      CHECK (unitseam_plan_cmp (ab, ba) == PLAN_COMP_LT);
      CHECK (unitseam_plan_cmp (ba, ab) == PLAN_COMP_GT);
    }
}

TEST_CASE ("C7 two indexes alike in cost and statistics: the same one wins whatever the creation order",
	   "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *by_a = s.iscan (t, i, s.index ("i_a", 10), 80.0, 1, 0, false);
  QO_PLAN *by_b = s.iscan (t, i, s.index ("i_b", 10), 80.0, 1, 0, false);

  CHECK (unitseam_plan_cmp (by_a, by_b) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (by_b, by_a) == PLAN_COMP_GT);
}

TEST_CASE ("C8 the analytic index scan is weighed with the analytic sort it saves (vs sscan, vs another iscan)",
	   "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  /* 3000 rows of 100 bytes: the sort qo_analytic_sort_cost () counts is about 5 + 7.5 + 18.3 */
  QO_INFO *i = s.info (3000.0, 100);
  QO_PLAN *an = s.iscan (t, i, s.index ("i_an", 30), 120.0, 0, 0, false, QO_SCANMETHOD_INDEX_ANALYTIC_SCAN);
  QO_PLAN *sq = s.seq (t, i, 100.0);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_p", 10), 95.0, 1, 0, false);

  CHECK (unitseam_plan_cmp (an, sq) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (sq, an) == PLAN_COMP_GT);
  if (!unitseam_has_cost_order ())
    {
      WARN ("base: the analytic iscan against a plain iscan goes into the iscan rules -- not run");
      return;
    }
  /* head: 120 - 30.8 < 95 -- the plain iscan leaves the analytic sort to the evaluation as well */
  CHECK (unitseam_plan_cmp (an, plain) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (plain, an) == PLAN_COMP_GT);
}

TEST_CASE ("C9 generated scans and sorts of one table: antisymmetric and transitive", "[contract]")
{
  if (!unitseam_has_cost_order ())
    {
      WARN ("base: the rules read scan and term fields the generated plans do not fill -- not run (see D2)");
      return;
    }

  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (3000.0, 100);
  const double costs[] = { 56.0, 99.0, 99.9, 100.1, 100.7, 100.9, 101.4, 101.5, 130.0, 160.0 };
  const char *names[] = { "i_a", "i_b", "i_c", "i_d" };
  std::vector<QO_PLAN *> set;
  std::string first;
  int k = 0;

  for (double c : costs)
    {
      set.push_back (s.seq (t, i, c));
      for (int v = 0; v < 4; v++, k++)
	{
	  QO_PLAN *ix = s.iscan (t, i, s.index (names[k % 4], 5 + (k * 7) % 30), c + 0.1 * v, k % 3, (k / 3) % 2,
				 (k % 5) == 0);
	  set.push_back (ix);
	  if (v == 0)
	    {
	      set.push_back (s.sort (ix, SORT_ORDERBY, 1.0 + k % 4));
	    }
	}
    }
  set.push_back (s.iscan (t, i, s.index ("i_an", 30), 120.0, 0, 0, false, QO_SCANMETHOD_INDEX_ANALYTIC_SCAN));
  /* a 1% window would chain 100.0 / 100.7 / 101.4, the dearer ones covering or with more terms */
  set.push_back (s.iscan (t, i, s.index ("w_a", 10), 100.0, 1, 0, false));
  set.push_back (s.iscan (t, i, s.index ("w_b", 10), 100.7, 2, 0, false));
  set.push_back (s.iscan (t, i, s.index ("w_c", 10), 101.4, 2, 0, true));

  int bad = order_violations (set, first);
  INFO ("plans " << set.size () << ", first violation: " << first);
  CHECK (bad == 0);
}

TEST_CASE ("C10 generated join plans of two tables: antisymmetric and transitive", "[contract]")
{
  if (!unitseam_has_cost_order ())
    {
      WARN ("base: not run (see D2)");
      return;
    }

  scene s (false);
  QO_NODE *a = s.node ("a", 0);
  QO_NODE *b = s.node ("b", 1);
  QO_INFO *ia = s.info (100.0, 64), *ib = s.info (100.0, 64), *iab = s.info (100.0, 128);
  const double costs[] = { 499.0, 500.0, 500.0, 502.0, 504.9, 505.0, 510.0 };
  const QO_JOINMETHOD methods[] = { QO_JOINMETHOD_NL_JOIN, QO_JOINMETHOD_HASH_JOIN };
  std::vector<QO_PLAN *> set;
  std::string first;

  for (double c : costs)
    {
      for (QO_JOINMETHOD m : methods)
	{
	  set.push_back (s.join (s.seq (a, ia, 10.0), s.seq (b, ib, 10.0), m, iab, c));
	  set.push_back (s.join (s.seq (b, ib, 10.0), s.seq (a, ia, 10.0), m, iab, c));
	}
    }
  set.push_back (s.sort (set[0], SORT_ORDERBY, 0.5));

  int bad = order_violations (set, first);
  INFO ("plans " << set.size () << ", first violation: " << first);
  CHECK (bad == 0);
}

TEST_CASE ("C11 two index joins of one cost and order: the smaller inner index wins (TPC-H q4: fk vs pk of lineitem)",
	   "[contract]")
{
  scene s (false);
  QO_NODE *o = s.node ("orders", 0);
  QO_NODE *l = s.node ("lineitem", 1);
  QO_INFO *io = s.info (1000.0, 64), *il = s.info (6000.0, 64), *iol = s.info (1000.0, 128);
  QO_PLAN *by_fk = s.join (s.seq (o, io, 500.0), s.iscan (l, il, s.index ("fk_l_orderkey", 30), 7.0, 1, 0, false),
			   QO_JOINMETHOD_IDX_JOIN, iol, 627788.0);
  QO_PLAN *by_pk = s.join (s.seq (o, io, 500.0), s.iscan (l, il, s.index ("pk_l_orderkey_linenumber", 80), 91.0, 1,
							   0, false), QO_JOINMETHOD_IDX_JOIN, iol, 627788.0);

  by_fk->plan_un.join.inner->top_rooted = false;
  by_pk->plan_un.join.inner->top_rooted = false;
  CHECK (unitseam_plan_cmp (by_fk, by_pk) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (by_pk, by_fk) == PLAN_COMP_GT);
}

TEST_CASE ("C12 two joins 0.3% apart: the cheaper wins over more index terms and a smaller inner index",
	   "[contract]")
{
  scene s (false);
  QO_NODE *o = s.node ("o", 0);
  QO_NODE *l = s.node ("l", 1);
  QO_INFO *io = s.info (1000.0, 64), *il = s.info (6000.0, 64), *iol = s.info (1000.0, 128);
  QO_PLAN *cheap = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_big", 80), 9.0, 1, 0, false),
			   QO_JOINMETHOD_IDX_JOIN, iol, 500.0);
  QO_PLAN *dear = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_small", 30), 7.0, 2, 0, false),
			  QO_JOINMETHOD_IDX_JOIN, iol, 501.5);

  cheap->plan_un.join.inner->top_rooted = false;
  dear->plan_un.join.inner->top_rooted = false;
  CHECK (unitseam_plan_cmp (cheap, dear) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (dear, cheap) == PLAN_COMP_GT);
}

TEST_CASE ("C13 three plans a chain of 1e-6 windows apart, the keys against the costs: no cycle, one plan kept",
	   "[contract]")
{
  if (!unitseam_has_cost_order ())
    {
      WARN ("base: the rules read scan and term fields the generated plans do not fill -- not run (see D2)");
      return;
    }

  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_NODE *x = s.node ("x", 0);
  QO_NODE *y = s.node ("y", 1);
  QO_INFO *i = s.info (100.0, 64), *ix = s.info (100.0, 64), *iy = s.info (100.0, 64), *ixy = s.info (100.0, 128);
  std::string first;
  /* neighbours within 1e-6 of each other, the ends 1.8e-6 apart; the names and the methods prefer the dearer */
  QO_PLAN *a = s.iscan (t, i, s.index ("i_3", 10), 1000.0, 1, 0, false);
  QO_PLAN *b = s.iscan (t, i, s.index ("i_2", 10), 1000.0009, 1, 0, false);
  QO_PLAN *c = s.iscan (t, i, s.index ("i_1", 10), 1000.0018, 1, 0, false);
  QO_PLAN *h = s.join (s.seq (x, ix, 10.0), s.seq (y, iy, 10.0), QO_JOINMETHOD_HASH_JOIN, ixy, 1000000.0);
  QO_PLAN *d = s.join (s.seq (x, ix, 10.0), s.seq (y, iy, 10.0), QO_JOINMETHOD_IDX_JOIN, ixy, 1000000.9);
  QO_PLAN *n = s.join (s.seq (x, ix, 10.0), s.seq (y, iy, 10.0), QO_JOINMETHOD_NL_JOIN, ixy, 1000001.8);

  CHECK (order_violations ({a, b, c}, first) == 0);
  CHECK (kept_of ({a, b, c}) == kept_of ({c, b, a}));
  CHECK (kept_of ({a, b, c}) == kept_of ({b, c, a}));
  CHECK (order_violations ({h, d, n}, first) == 0);
  CHECK (kept_of ({h, d, n}) == kept_of ({n, d, h}));
  CHECK (kept_of ({h, d, n}) == kept_of ({d, n, h}));
}

TEST_CASE ("C14 scaling every cost by 1.002 keeps the winner of C3's pair (covering 101.0 vs plain 100.3)",
	   "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_c", 20), 101.0, 1, 0, true);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_p", 20), 100.3, 1, 0, false);
  QO_PLAN *cov2 = s.iscan (t, i, s.index ("i_c", 20), 101.0 * 1.002, 1, 0, true);
  QO_PLAN *plain2 = s.iscan (t, i, s.index ("i_p", 20), 100.3 * 1.002, 1, 0, false);

  CHECK (unitseam_plan_cmp (cov, plain) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (cov2, plain2) == PLAN_COMP_LT);
}

TEST_CASE ("C15 a pair of one cost ratio (covering 1.005x vs plain x) as x grows from 100 to 110: the covering scan "
	   "wins at every step", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_NODE_INDEX_ENTRY *ic = s.index ("i_c", 20), *ip = s.index ("i_p", 20);
  int cov_wins = 0, steps = 0;

  for (int k = 0; k < 1000; k++, steps++)
    {
      double x = 100.0 + 0.01 * k;
      QO_PLAN *cov = s.iscan (t, i, ic, 1.005 * x, 1, 0, true);
      QO_PLAN *plain = s.iscan (t, i, ip, x, 1, 0, false);

      cov_wins += (unitseam_plan_cmp (cov, plain) == PLAN_COMP_LT) ? 1 : 0;
    }
  INFO ("steps " << steps << ", covering wins " << cov_wins);
  CHECK (cov_wins == steps);
}

TEST_CASE ("C16 with an INST_NUM () term (plan_cmp_by_rules) the plans keep the band and rules: F1's iscan 99 beats "
	   "sscan 56 as on develop", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (2990.0, 220);
  QO_PLAN *ix = s.iscan (t, i, s.index ("i_t_a", 10), 99.0, 1, 0, false);
  QO_PLAN *sq = s.seq (t, i, 56.0);

  unitseam_set_cmp_by_rules (s.planner, 1);
  CHECK (unitseam_plan_cmp (ix, sq) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (sq, ix) == PLAN_COMP_GT);
}

TEST_CASE ("C17 qo_plan_cmp_by_rules () marks a query with an INST_NUM () term or a forced filter index, no other",
	   "[contract]")
{
  if (unitseam_plan_cmp_by_rules (NULL) < 0)
    {
      WARN ("base: no qo_plan_cmp_by_rules () -- not run");
      return;
    }

  scene s (false);
  QO_TERM *terms = (QO_TERM *) s.alloc (2 * sizeof (QO_TERM));
  QO_NODE *nodes = (QO_NODE *) s.alloc (2 * sizeof (QO_NODE));
  QO_NODE_INDEX *ni = (QO_NODE_INDEX *) s.alloc (sizeof (QO_NODE_INDEX) + sizeof (QO_NODE_INDEX_ENTRY));
  QO_INDEX_ENTRY *plain = (QO_INDEX_ENTRY *) s.alloc (sizeof (QO_INDEX_ENTRY));
  QO_INDEX_ENTRY *filter = (QO_INDEX_ENTRY *) s.alloc (sizeof (QO_INDEX_ENTRY));

  plain->constraints = (SM_CLASS_CONSTRAINT *) s.alloc (sizeof (SM_CLASS_CONSTRAINT));
  filter->constraints = (SM_CLASS_CONSTRAINT *) s.alloc (sizeof (SM_CLASS_CONSTRAINT));
  terms[0].term_class = QO_TC_SARG;
  terms[1].term_class = QO_TC_JOIN;
  ni->n = 2;
  ni->index[0].head = plain;
  ni->index[1].head = filter;
  nodes[1].indexes = ni;	/* the first node has none (a derived table) */
  s.planner->term = terms;
  s.planner->T = 2;
  s.planner->node = nodes;
  s.planner->N = 2;

  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 0);
  terms[1].term_class = QO_TC_TOTALLY_AFTER_JOIN;
  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 1);
  terms[1].term_class = QO_TC_OTHER;
  plain->force = 1;
  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 0);	/* forced, no filter */
  filter->constraints->filter_predicate = (SM_PREDICATE_INFO *) s.alloc (sizeof (SM_PREDICATE_INFO));
  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 0);	/* a filter index, not forced */
  filter->force = -1;
  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 0);	/* ignored */
  filter->force = 1;
  CHECK (unitseam_plan_cmp_by_rules (s.planner) == 1);
}

TEST_CASE ("C18 a join is ranked by its cost alone: a covering inner index does not win a join 0.5% dearer",
	   "[contract]")
{
  scene s (false);
  QO_NODE *o = s.node ("o", 0);
  QO_NODE *l = s.node ("l", 1);
  QO_INFO *io = s.info (1000.0, 64), *il = s.info (6000.0, 64), *iol = s.info (1000.0, 128);
  QO_PLAN *cheap = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_l", 30), 9.0, 1, 0, false),
			   QO_JOINMETHOD_IDX_JOIN, iol, 500.0);
  QO_PLAN *dear = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_lc", 30), 9.0, 1, 0, true),
			  QO_JOINMETHOD_IDX_JOIN, iol, 502.5);

  cheap->plan_un.join.inner->top_rooted = false;
  dear->plan_un.join.inner->top_rooted = false;
  CHECK (unitseam_plan_cmp (cheap, dear) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (dear, cheap) == PLAN_COMP_GT);
}

TEST_CASE ("C19 a scan that skips the ORDER BY wins over a sort that costs less by less than its share, not more",
	   "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (3000.0, 100);
  QO_NODE_INDEX_ENTRY *ix = s.index ("i_t_a", 10);
  QO_PLAN *skip, *near, *far;

  ix->head->orderby_skip = true;
  skip = s.iscan (t, i, ix, 100.5, 1, 0, false);
  near = s.sort (s.seq (t, i, 90.0), SORT_ORDERBY, 10.0);	/* 100.0: x1.01 = 101.0 */
  far = s.sort (s.seq (t, i, 90.0), SORT_ORDERBY, 8.0);	/* 98.0: x1.01 = 98.98 */

  CHECK (unitseam_plan_cmp (skip, near) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (near, skip) == PLAN_COMP_GT);
  CHECK (unitseam_plan_cmp (skip, far) == PLAN_COMP_GT);
  CHECK (unitseam_plan_cmp (far, skip) == PLAN_COMP_LT);
}

TEST_CASE ("C20 below zero (an analytic scan less its sort) the covering share still gains", "[contract]")
{
  if (!unitseam_has_cost_order ())
    {
      WARN ("base: the analytic iscans go into the iscan rules -- not run");
      return;
    }

  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  /* the sort of C8, about 30.8: the two scans rank at about -3.0 and -3.03 */
  QO_INFO *i = s.info (3000.0, 100);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_ac", 30), 27.8, 0, 0, true, QO_SCANMETHOD_INDEX_ANALYTIC_SCAN);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_ap", 30), 27.78, 0, 0, false, QO_SCANMETHOD_INDEX_ANALYTIC_SCAN);

  CHECK (unitseam_plan_cmp (cov, plain) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (plain, cov) == PLAN_COMP_GT);
}

TEST_CASE ("D1 develop: inside the band the iscan rule overrides the cost (iscan 99 beats sscan 56)", "[defect]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (2990.0, 220);
  QO_PLAN *ix = s.iscan (t, i, s.index ("i_t_a", 10), 99.0, 1, 0, false);
  QO_PLAN *sq = s.seq (t, i, 56.0);

  CHECK (unitseam_plan_cmp (ix, sq) == PLAN_COMP_LT);
}

TEST_CASE ("D2 develop: a cycle -- iscan beats sscan and the covering iscan beats the iscan by rules, sscan beats the "
	   "covering iscan by cost", "[defect]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (3000.0, 100);
  QO_PLAN *sq = s.seq (t, i, 100.0);
  QO_PLAN *ix = s.iscan (t, i, s.index ("i_p", 10), 130.0, 1, 0, false);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_c", 20), 160.0, 1, 0, true);

  CHECK (unitseam_plan_cmp (ix, sq) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (cov, ix) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (sq, cov) == PLAN_COMP_LT);
}
