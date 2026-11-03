/*
 * CBRD-27146 -- qo_plan_cmp () of src/optimizer/query_planner.c (CS_MODE), reached through hooks.cpp:
 *   head: two plans of a query without LIMIT are compared by the 1% log bucket of their cost (less the analytic sort
 *         an analytic index scan saves), two plans of one bucket by values of each plan in a fixed order
 *         (qo_plan_tie_cmp ()); two plans of a LIMIT query keep the band and its rules (qo_plan_cmp_limit ()).
 *   base: the band (cost difference < 50 and ratio < 1.2, x10 under LIMIT) lets about twenty rules pick the plan.
 *
 * Real code: the TU (qo_plan_cmp and what it calls in query_planner.c), query_bitset.c, system_parameter.c (prm_Def
 * static initializers: cost_seq_page, cost_cpu_tuples_per_page for the analytic sort cost).
 * Fakes: the five plan predicates of plan_generation.c that the rules call (qo_is_index_covering_scan,
 * qo_is_index_iss_scan, qo_is_index_loose_scan, qo_is_index_mro_scan, qo_plan_multi_range_opt), copied from that file
 * without their asserts; db_Io_page_size = 16K.
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

QO_PLAN_COMPARE_RESULT unitseam_plan_cmp (QO_PLAN * a, QO_PLAN * b);
int unitseam_has_cost_buckets (void);

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

  /* bucket boundaries of 1.01: 1.01^462 = 99.19, 1.01^463 = 100.18, 1.01^464 = 101.19 -- 100.3 and 101.0 share one */
}

TEST_CASE ("C1 without LIMIT a cheaper plan of another cost bucket wins over the rules (F1: iscan 99 vs sscan 56)",
	   "[contract]")
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

TEST_CASE ("C3 in one cost bucket the covering index scan wins (101.0 covering vs 100.3)", "[contract]")
{
  scene s (false);
  QO_NODE *t = s.node ("t", 0);
  QO_INFO *i = s.info (100.0, 64);
  QO_PLAN *cov = s.iscan (t, i, s.index ("i_c", 20), 101.0, 1, 0, true);
  QO_PLAN *plain = s.iscan (t, i, s.index ("i_p", 20), 100.3, 1, 0, false);

  CHECK (unitseam_plan_cmp (cov, plain) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (plain, cov) == PLAN_COMP_GT);
}

TEST_CASE ("C4 across cost buckets the cheaper plan wins even when the other is covering (101.5 vs 100.3)",
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
  if (!unitseam_has_cost_buckets ())
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
  if (!unitseam_has_cost_buckets ())
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
  /* the window counterexample: 100.0 / 100.7 / 101.4, keys preferring the dearer ones (covering, more terms) */
  set.push_back (s.iscan (t, i, s.index ("w_a", 10), 100.0, 1, 0, false));
  set.push_back (s.iscan (t, i, s.index ("w_b", 10), 100.7, 2, 0, false));
  set.push_back (s.iscan (t, i, s.index ("w_c", 10), 101.4, 2, 0, true));

  int bad = order_violations (set, first);
  INFO ("plans " << set.size () << ", first violation: " << first);
  CHECK (bad == 0);
}

TEST_CASE ("C10 generated join plans of two tables: antisymmetric and transitive", "[contract]")
{
  if (!unitseam_has_cost_buckets ())
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

TEST_CASE ("C12 two joins of one cost bucket: the cheaper wins over more index terms and a smaller inner index",
	   "[contract]")
{
  scene s (false);
  QO_NODE *o = s.node ("o", 0);
  QO_NODE *l = s.node ("l", 1);
  QO_INFO *io = s.info (1000.0, 64), *il = s.info (6000.0, 64), *iol = s.info (1000.0, 128);
  /* 500.0 and 501.5 share the bucket 625 */
  QO_PLAN *cheap = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_big", 80), 9.0, 1, 0, false),
			   QO_JOINMETHOD_IDX_JOIN, iol, 500.0);
  QO_PLAN *dear = s.join (s.seq (o, io, 50.0), s.iscan (l, il, s.index ("i_small", 30), 7.0, 2, 0, false),
			  QO_JOINMETHOD_IDX_JOIN, iol, 501.5);

  cheap->plan_un.join.inner->top_rooted = false;
  dear->plan_un.join.inner->top_rooted = false;
  CHECK (unitseam_plan_cmp (cheap, dear) == PLAN_COMP_LT);
  CHECK (unitseam_plan_cmp (dear, cheap) == PLAN_COMP_GT);
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
