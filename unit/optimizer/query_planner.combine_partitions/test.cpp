/*
 * PR#8471 (CBRD-28003) -- qo_combine_partitions () of src/optimizer/query_planner.c (CS_MODE), reached through
 * hooks.cpp. qo_planner_search () calls it once per query, after each partition of the join graph has its plan: it
 * joins the partition plans by cross joins (qo_cp_new ()), finalizes the topmost plan and then gives every term in
 * [E, T) that reads no node to the sarged terms of the topmost plan that is not an empty SORT plan.
 *   head b729cb5d4: an ON-clause term that reads no node and is classed QO_TC_DURING_JOIN is left out --
 *                   planner_visit_node () gives it to its join as a during-join term. query_graph.c classes so the
 *                   ON term of no node of an outer join (qo_classify_outerjoin_terms (), O4) and of an anti join
 *                   (qo_classify_antijoin_terms (), A2).
 *   base 36c7433d8: every term that reads no node goes to the topmost plan, the during-join one too. On a plan above
 *                   that join (the cross join of the partitions, or a join over it) it also filters the rows the
 *                   outer join preserves.
 *
 * Real code: the TU (qo_combine_partitions, sort_partitions, qo_plan_finalize, qo_detach_info, qo_uninit_planvec)
 * and query_bitset.c.
 * Fakes -- reached only by two partitions (C3 and D1's second scene; one partition builds no cross join), seams
 * qo_alloc_info and qo_cp_new:
 *   qo_alloc_info () as the real one without the projected segments and their size (qo_compute_projected_segs (),
 *     qo_compute_projected_size (): segments, for the cost) and with no ordered plan vector (planner->EQ = 0);
 *   qo_cp_new () as qo_join_new () builds a cross join -- JOIN_INNER, NL join, no join, during-join, after-join or
 *     hash terms, so sarg_out_terms is empty and the plan keeps every sarged term it is given; both inputs gain a
 *     reference -- without the cost, the SORT_TEMP over an inner that is not a file scan, the SORT-LIMIT plan and
 *     qo_top_plan_new () (the scenes have no ORDER BY, GROUP BY or DISTINCT).
 * Structures: calloc'ed. Terms carry their nodes, class and location (the classes query_graph.c gives, cited per
 * scene); plans carry their type, info, sarged terms and the links the function follows. The sarged terms a plan
 * holds before the call are chosen, not the ones planner_visit_node () would leave there -- the function only adds.
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
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

QO_PLAN *unitseam_combine_partitions (QO_PLANNER * planner, BITSET * remaining_subqueries);

namespace
{
  /* what the fakes were given and made -- reset by each scene */
  std::vector<QO_PLAN *> g_cp_plans;
  std::vector<std::vector<int>> g_cp_sarged_in;
  int g_alloc_info_calls;

  std::vector<int> members (const BITSET * s)
  {
    std::vector<int> v;
    BITSET_ITERATOR it;

    for (int i = bitset_iterate (s, &it); i != -1; i = bitset_next_member (&it))
      {
	v.push_back (i);
      }
    return v;
  }

  bool has (const BITSET * s, int t)
  {
    std::vector<int> v = members (s);
    return std::find (v.begin (), v.end (), t) != v.end ();
  }

  void add_all (BITSET * s, std::initializer_list<int> xs)
  {
    for (int x : xs)
      {
	bitset_add (s, x);
      }
  }

  void init_plan_sets (QO_PLAN * p, QO_ENV * env)
  {
    bitset_init (&p->sarged_terms, env);
    bitset_init (&p->subqueries, env);
    if (p->plan_type == QO_PLANTYPE_JOIN)
      {
	bitset_init (&p->plan_un.join.join_terms, env);
	bitset_init (&p->plan_un.join.during_join_terms, env);
	bitset_init (&p->plan_un.join.after_join_terms, env);
      }
  }

  void delset_plan (QO_PLAN * p)
  {
    bitset_delset (&p->sarged_terms);
    bitset_delset (&p->subqueries);
    if (p->plan_type == QO_PLANTYPE_JOIN)
      {
	bitset_delset (&p->plan_un.join.join_terms);
	bitset_delset (&p->plan_un.join.during_join_terms);
	bitset_delset (&p->plan_un.join.after_join_terms);
      }
  }

  void delset_info (QO_INFO * in)
  {
    bitset_delset (&in->nodes);
    bitset_delset (&in->terms);
    bitset_delset (&in->eqclasses);
    bitset_delset (&in->projected_segs);
  }
}

/* query_planner.c qo_alloc_info (), seam -- see the head comment */
QO_INFO *
qo_alloc_info (QO_PLANNER * planner, BITSET * nodes, BITSET * terms, BITSET * eqclasses, double cardinality,
	       double total_rows)
{
  QO_INFO *info = (QO_INFO *) calloc (1, sizeof (QO_INFO));

  g_alloc_info_calls++;
  info->env = planner->env;
  info->planner = planner;
  bitset_init (&info->nodes, planner->env);
  bitset_init (&info->terms, planner->env);
  bitset_init (&info->eqclasses, planner->env);
  bitset_init (&info->projected_segs, planner->env);
  bitset_assign (&info->nodes, nodes);
  bitset_assign (&info->terms, terms);
  bitset_assign (&info->eqclasses, eqclasses);
  info->cardinality = cardinality;
  info->total_rows = total_rows;
  info->planvec = NULL;		/* planner->EQ == 0 */
  info->join_unit = planner->join_unit;
  info->detached = false;
  info->next = planner->info_list;
  planner->info_list = info;
  return info;
}

/* query_planner.c qo_cp_new () = qo_join_new (info, JOIN_INNER, QO_JOINMETHOD_NL_JOIN, outer, inner, {}, {}, {},
 * sarged_terms, pinned_subqueries, {}), seam -- see the head comment */
QO_PLAN *
qo_cp_new (QO_INFO * info, QO_PLAN * outer, QO_PLAN * inner, BITSET * sarged_terms, BITSET * pinned_subqueries)
{
  QO_PLAN *plan = (QO_PLAN *) calloc (1, sizeof (QO_PLAN));

  g_cp_sarged_in.push_back (members (sarged_terms));
  plan->plan_type = QO_PLANTYPE_JOIN;
  init_plan_sets (plan, info->env);
  plan->info = info;
  plan->refcount = 0;
  plan->plan_un.join.join_type = JOIN_INNER;
  plan->plan_un.join.join_method = QO_JOINMETHOD_NL_JOIN;
  plan->plan_un.join.outer = qo_plan_add_ref (outer);
  plan->plan_un.join.inner = qo_plan_add_ref (inner);
  bitset_assign (&plan->sarged_terms, sarged_terms);	/* less sarg_out_terms: the join terms, none */
  bitset_assign (&plan->subqueries, pinned_subqueries);
  g_cp_plans.push_back (plan);
  return plan;
}

namespace
{
  struct scene
  {
    QO_ENV *env;
    QO_PLANNER *planner;
    QO_TERM *terms;
    QO_PARTITION *parts;
    int nterms, nparts;
    BITSET remaining_subqueries;
    std::vector<QO_PLAN *> plans;
    std::vector<QO_INFO *> infos;

    scene (int nterms_, int nedges, int nparts_) : nterms (nterms_), nparts (nparts_)
    {
      g_cp_plans.clear ();
      g_cp_sarged_in.clear ();
      g_alloc_info_calls = 0;

      env = (QO_ENV *) calloc (1, sizeof (QO_ENV));
      planner = (QO_PLANNER *) calloc (1, sizeof (QO_PLANNER));
      terms = (QO_TERM *) calloc (nterms, sizeof (QO_TERM));
      parts = (QO_PARTITION *) calloc (nparts, sizeof (QO_PARTITION));
      for (int i = 0; i < nterms; i++)
	{
	  terms[i].env = env;
	  terms[i].idx = i;
	  bitset_init (&terms[i].nodes, env);
	  bitset_init (&terms[i].segments, env);
	  bitset_init (&terms[i].subqueries, env);
	}
      for (int p = 0; p < nparts; p++)
	{
	  parts[p].idx = p;
	  bitset_init (&parts[p].nodes, env);
	  bitset_init (&parts[p].edges, env);
	  bitset_init (&parts[p].dependencies, env);
	}
      planner->env = env;
      planner->term = terms;
      planner->T = nterms;
      planner->E = nedges;		/* qo_discover_edges () puts the edges first: the loops look at [E, T) */
      planner->partition = parts;
      planner->P = nparts;
      planner->EQ = 0;
      /* qo_planner_search () allocates a cross join info slot per partition when there is more than one */
      planner->cp_info = nparts > 1 ? (QO_INFO **) calloc (nparts, sizeof (QO_INFO *)) : NULL;
      bitset_init (&remaining_subqueries, env);
    }

    ~scene ()
    {
      for (QO_PLAN * p:g_cp_plans)
	{
	  delset_plan (p);
	  free (p);
	}
      g_cp_plans.clear ();
      for (QO_INFO * in = planner->info_list; in != NULL;)
	{
	  QO_INFO *next = in->next;
	  delset_info (in);
	  free (in->planvec);
	  free (in);
	  in = next;
	}
      for (QO_PLAN * p:plans)
	{
	  delset_plan (p);
	  free (p);
	}
      for (QO_INFO * in:infos)
	{
	  delset_info (in);
	  free (in);
	}
      for (int i = 0; i < nterms; i++)
	{
	  bitset_delset (&terms[i].nodes);
	  bitset_delset (&terms[i].segments);
	  bitset_delset (&terms[i].subqueries);
	}
      for (int p = 0; p < nparts; p++)
	{
	  bitset_delset (&parts[p].nodes);
	  bitset_delset (&parts[p].edges);
	  bitset_delset (&parts[p].dependencies);
	}
      bitset_delset (&remaining_subqueries);
      free (planner->cp_info);
      free (parts);
      free (terms);
      free (planner);
      free (env);
    }

    void term (int idx, QO_TERMCLASS cls, std::initializer_list<int> nodes, short location)
    {
      terms[idx].term_class = cls;
      add_all (&terms[idx].nodes, nodes);
      terms[idx].location = location;
    }

    QO_INFO *info (std::initializer_list<int> nodes, std::initializer_list<int> tset, double rows)
    {
      QO_INFO *in = (QO_INFO *) calloc (1, sizeof (QO_INFO));

      in->env = env;
      in->planner = planner;
      bitset_init (&in->nodes, env);
      bitset_init (&in->terms, env);
      bitset_init (&in->eqclasses, env);
      bitset_init (&in->projected_segs, env);
      add_all (&in->nodes, nodes);
      add_all (&in->terms, tset);
      in->cardinality = rows;
      in->total_rows = rows;
      infos.push_back (in);
      return in;
    }

    QO_PLAN *plan (QO_PLANTYPE type, QO_INFO * in, std::initializer_list<int> sarged)
    {
      QO_PLAN *p = (QO_PLAN *) calloc (1, sizeof (QO_PLAN));

      p->plan_type = type;
      init_plan_sets (p, env);
      p->info = in;
      add_all (&p->sarged_terms, sarged);
      plans.push_back (p);
      return p;
    }

    QO_PLAN *scan (QO_INFO * in, std::initializer_list<int> sarged)
    {
      return plan (QO_PLANTYPE_SCAN, in, sarged);
    }

    /* as qo_join_new () leaves it: the join, during-join and after-join terms out of the sarged terms */
    QO_PLAN *join (JOIN_TYPE jt, QO_PLAN * outer, QO_PLAN * inner, QO_INFO * in, std::initializer_list<int> join_terms,
		   std::initializer_list<int> duj, std::initializer_list<int> afj, std::initializer_list<int> sarged)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_JOIN, in, sarged);

      p->plan_un.join.join_type = jt;
      p->plan_un.join.join_method = QO_JOINMETHOD_NL_JOIN;
      p->plan_un.join.outer = qo_plan_add_ref (outer);
      p->plan_un.join.inner = qo_plan_add_ref (inner);
      add_all (&p->plan_un.join.join_terms, join_terms);
      add_all (&p->plan_un.join.during_join_terms, duj);
      add_all (&p->plan_un.join.after_join_terms, afj);
      return p;
    }

    /* as qo_sort_new () makes it: the subplan's info, no sarged term */
    QO_PLAN *sort (QO_PLAN * sub, SORT_TYPE st)
    {
      QO_PLAN *p = plan (QO_PLANTYPE_SORT, sub->info, {});

      p->plan_un.sort.sort_type = st;
      p->plan_un.sort.subplan = qo_plan_add_ref (sub);
      return p;
    }

    /* qo_search_partition () leaves each partition its plan finalized: one reference */
    void partition (int p, QO_PLAN * top, std::initializer_list<int> nodes)
    {
      parts[p].plan = qo_plan_add_ref (top);
      add_all (&parts[p].nodes, nodes);
    }

    QO_PLAN *combine ()
    {
      return unitseam_combine_partitions (planner, &remaining_subqueries);
    }
  };

  /* nodes o 0, i 1, j 2
   *   SELECT ... FROM o LEFT JOIN i ON o.a = i.a AND ? = 2 AND o.c = 1
   *                     INNER JOIN j ON o.b = j.b AND ? = 3
   *   WHERE ? = 1 AND o.d > 0 AND ROWNUM <= 10
   * One partition; its plan is the inner join with j over the outer join (o, i). */
  enum
  { O = 0, I = 1, J = 2 };
  enum
  {
    T_OI = 0,			/* o.a = i.a       join (edge), ON of i */
    T_OJ = 1,			/* o.b = j.b       join (edge), ON of j */
    T_ON_NONE = 2,		/* ? = 2           ON of i, no node: during-join (qo_classify_outerjoin_terms () O4) */
    T_ON_O = 3,			/* o.c = 1         ON of i on the preserved side: during-join (null padded, STEP 3) */
    T_INNER_ON_NONE = 4,	/* ? = 3           ON of j, no node: other (the ON terms of an inner join are skipped) */
    T_WHERE_NONE = 5,		/* ? = 1           WHERE, no node: after-join (W4, a query with an outer join) */
    T_WHERE_O = 6,		/* o.d > 0         sarg on o */
    T_ROWNUM = 7,		/* ROWNUM <= 10    no node, INST_NUM: totally-after-join (qo_analyze_term ()) */
    T_COUNT = 8
  };

  struct one_partition
  {
    scene s;
    QO_PLAN *oi, *top;

    one_partition ():s (T_COUNT, 2, 1)
    {
      s.term (T_OI, QO_TC_JOIN, {O, I}, 1);
      s.term (T_OJ, QO_TC_JOIN, {O, J}, 2);
      s.term (T_ON_NONE, QO_TC_DURING_JOIN, {}, 1);
      s.term (T_ON_O, QO_TC_DURING_JOIN, {O}, 1);
      s.term (T_INNER_ON_NONE, QO_TC_OTHER, {}, 2);
      s.term (T_WHERE_NONE, QO_TC_AFTER_JOIN, {}, 0);
      s.term (T_WHERE_O, QO_TC_SARG, {O}, 0);
      s.term (T_ROWNUM, QO_TC_TOTALLY_AFTER_JOIN, {}, 0);

      QO_PLAN *so = s.scan (s.info ({O}, {T_WHERE_O}, 1000), {T_WHERE_O});
      QO_PLAN *si = s.scan (s.info ({I}, {}, 1000), {});
      QO_PLAN *sj = s.scan (s.info ({J}, {}, 100), {});

      oi = s.join (JOIN_LEFT, so, si, s.info ({O, I}, {T_OI, T_ON_NONE, T_ON_O, T_WHERE_O}, 1000), {T_OI},
		   {T_ON_NONE, T_ON_O}, {}, {});
      top = s.join (JOIN_INNER, oi, sj, s.info ({O, I, J}, {T_OI, T_OJ, T_ON_NONE, T_ON_O, T_WHERE_O}, 1000), {T_OJ},
		    {}, {}, {});
    }
  };

  /* nodes x 0, o 1, i 2
   *   SELECT ... FROM x, o LEFT JOIN i ON o.a = i.a AND ? = 2
   *   WHERE i.b = x.b AND ? = 1 AND x.c > 0 AND ROWNUM <= 5
   * i.b = x.b: its latest node i is null-supplied, so it is an after-join term (W2) and no edge -- x, joined with a
   * comma, is a partition of its own and the topmost plan is the cross join of the two partitions. */
  enum
  { X2 = 0, O2 = 1, I2 = 2 };
  enum
  {
    U_OI = 0,			/* o.a = i.a       join (edge), ON of i */
    U_ON_NONE = 1,		/* ? = 2           ON of i, no node: during-join (O4) */
    U_IX = 2,			/* i.b = x.b       WHERE, after-join (W2), across the partitions */
    U_WHERE_NONE = 3,		/* ? = 1           WHERE, no node: after-join (W4) */
    U_WHERE_X = 4,		/* x.c > 0         sarg on x */
    U_ROWNUM = 5,		/* ROWNUM <= 5     totally-after-join */
    U_COUNT = 6
  };

  struct two_partitions
  {
    scene s;
    QO_PLAN *sx, *oi;

    two_partitions ():s (U_COUNT, 1, 2)
    {
      s.term (U_OI, QO_TC_JOIN, {O2, I2}, 2);
      s.term (U_ON_NONE, QO_TC_DURING_JOIN, {}, 2);
      s.term (U_IX, QO_TC_AFTER_JOIN, {X2, I2}, 0);
      s.term (U_WHERE_NONE, QO_TC_AFTER_JOIN, {}, 0);
      s.term (U_WHERE_X, QO_TC_SARG, {X2}, 0);
      s.term (U_ROWNUM, QO_TC_TOTALLY_AFTER_JOIN, {}, 0);

      sx = s.scan (s.info ({X2}, {U_WHERE_X}, 10), {U_WHERE_X});
      QO_PLAN *so = s.scan (s.info ({O2}, {}, 100), {});
      QO_PLAN *si = s.scan (s.info ({I2}, {}, 100), {});

      /* planner_visit_node (): the ON term of no node is a during-join term of the outer join, the WHERE one an
       * after-join term of the plan that holds the whole partition */
      oi = s.join (JOIN_LEFT, so, si, s.info ({O2, I2}, {U_OI, U_ON_NONE, U_WHERE_NONE}, 100), {U_OI}, {U_ON_NONE},
		   {U_WHERE_NONE}, {});
      s.partition (0, sx, {X2});
      s.partition (1, oi, {O2, I2});
    }
  };
}

TEST_CASE ("PR8471 C1: one partition, an inner join over a left outer join on top -- of the terms that read no node "
	   "it gets the WHERE one (after-join), the inner join's ON one (other) and ROWNUM (totally-after-join), not "
	   "the outer join's ON one (during-join); terms that read a node are left alone", "[contract]")
{
  one_partition q;
  q.s.partition (0, q.top, {O, I, J});

  QO_PLAN *top = q.s.combine ();

  REQUIRE (top == q.top);
  CHECK (members (&top->sarged_terms) == std::vector<int> ({T_INNER_ON_NONE, T_WHERE_NONE, T_ROWNUM}));
  CHECK (members (&q.oi->sarged_terms).empty ());
  CHECK (members (&q.oi->plan_un.join.during_join_terms) == std::vector<int> ({T_ON_NONE, T_ON_O}));
  CHECK (top->refcount == 1);	/* the partition's reference, given back by qo_plan_finalize () */
  CHECK (g_cp_plans.empty ());
  CHECK (g_alloc_info_calls == 0);
}

TEST_CASE ("PR8471 C2: empty SORT plans on top (ORDER BY over GROUP BY) -- the terms that read no node go to the "
	   "join under them, the outer join's ON one does not; the SORT plans stay empty", "[contract]")
{
  one_partition q;
  QO_PLAN *g = q.s.sort (q.top, SORT_GROUPBY);
  QO_PLAN *ob = q.s.sort (g, SORT_ORDERBY);	/* qo_top_plan_new (): GROUP BY, then ORDER BY over it */
  q.s.partition (0, ob, {O, I, J});

  QO_PLAN *top = q.s.combine ();

  REQUIRE (top == ob);
  CHECK (members (&ob->sarged_terms).empty ());
  CHECK (members (&g->sarged_terms).empty ());
  CHECK (members (&q.top->sarged_terms) == std::vector<int> ({T_INNER_ON_NONE, T_WHERE_NONE, T_ROWNUM}));
  CHECK (ob->refcount == 1);
}

TEST_CASE ("PR8471 C3: two partitions (x joined with a comma) -- the cross join on top gets the WHERE term across "
	   "the partitions from the partition loop and the WHERE and ROWNUM terms that read no node, not the outer "
	   "join's ON term that reads no node", "[contract]")
{
  two_partitions q;

  QO_PLAN *top = q.s.combine ();

  REQUIRE (g_cp_plans.size () == 1);
  REQUIRE (top == g_cp_plans[0]);
  CHECK (g_alloc_info_calls == 1);
  /* the partition loop: only terms that read a node, of both partitions, not yet in either */
  REQUIRE (g_cp_sarged_in.size () == 1);
  CHECK (g_cp_sarged_in[0] == std::vector<int> ({U_IX}));
  CHECK (top->plan_un.join.outer == q.sx);
  CHECK (top->plan_un.join.inner == q.oi);
  CHECK (members (&top->sarged_terms) == std::vector<int> ({U_IX, U_WHERE_NONE, U_ROWNUM}));
  CHECK (members (&q.oi->sarged_terms).empty ());
  CHECK (members (&q.sx->sarged_terms) == std::vector<int> ({U_WHERE_X}));
  CHECK (top->refcount == 1);
  CHECK (q.sx->refcount == 1);	/* the partition's reference handed to the cross join */
  CHECK (q.oi->refcount == 1);
  CHECK (q.s.planner->cp_info[1] != NULL);
  CHECK (q.s.planner->cp_info[1]->detached);
}

TEST_CASE ("PR8471 C4: control, no outer join -- a single scan keeps its sarg and gets the WHERE term that reads no "
	   "node (other) and ROWNUM; develop the same", "[contract]")
{
  /* nodes t 0:  SELECT ... FROM t WHERE t.a > 0 AND ? = 1 AND ROWNUM <= 3 */
  scene s (3, 0, 1);
  s.term (0, QO_TC_SARG, {0}, 0);
  s.term (1, QO_TC_OTHER, {}, 0);	/* qo_analyze_term (): no node; no outer join term, so not reclassed */
  s.term (2, QO_TC_TOTALLY_AFTER_JOIN, {}, 0);
  QO_PLAN *st = s.scan (s.info ({0}, {0}, 1000), {0});
  s.partition (0, st, {0});

  QO_PLAN *top = s.combine ();

  REQUIRE (top == st);
  CHECK (members (&st->sarged_terms) == std::vector<int> ({0, 1, 2}));
  CHECK (st->refcount == 1);
}

TEST_CASE ("PR8471 C5: an anti join's ON term that reads no node is a during-join term too and is left to the anti "
	   "join; a semi join's (other) still goes to the topmost plan", "[contract]")
{
  /* nodes o 0, a 1, s 2
   *   SELECT ... FROM o ANTI JOIN a ON a.k = o.k AND ? = 1 SEMI JOIN s ON s.k = o.k AND ? = 2
   * (or NOT EXISTS / EXISTS unnested, query_rewrite_subquery.c). qo_classify_antijoin_terms () classes an other
   * term of an anti join's ON during-join (A2); qo_classify_outerjoin_terms () skips the ON terms of semi and anti
   * joins, so the semi join's keeps its class. */
  scene s (4, 2, 1);
  s.term (0, QO_TC_JOIN, {0, 1}, 1);
  s.term (1, QO_TC_JOIN, {0, 2}, 2);
  s.term (2, QO_TC_DURING_JOIN, {}, 1);
  s.term (3, QO_TC_OTHER, {}, 2);
  QO_PLAN *so = s.scan (s.info ({0}, {}, 1000), {});
  QO_PLAN *sa = s.scan (s.info ({1}, {}, 1000), {});
  QO_PLAN *ss = s.scan (s.info ({2}, {}, 1000), {});
  /* qo_join_new (): an anti inner keeps the during-join terms apart, like an outer join */
  QO_PLAN *oa = s.join (JOIN_INNER, so, sa, s.info ({0, 1}, {0, 2}, 500), {0}, {2}, {}, {});
  QO_PLAN *top = s.join (JOIN_INNER, oa, ss, s.info ({0, 1, 2}, {0, 1, 2}, 500), {1}, {}, {}, {});
  s.partition (0, top, {0, 1, 2});

  QO_PLAN *ret = s.combine ();

  REQUIRE (ret == top);
  CHECK (members (&top->sarged_terms) == std::vector<int> ({3}));
  CHECK (members (&oa->sarged_terms).empty ());
}

TEST_CASE ("PR8471 D1: develop gives the outer join's ON term that reads no node to the topmost plan as well -- the "
	   "inner join over the outer join, the cross join of the partitions [defect] PASS = present (develop "
	   "36c7433d8), gone at b729cb5d4", "[defect]")
{
  {
    one_partition q;
    q.s.partition (0, q.top, {O, I, J});

    QO_PLAN *top = q.s.combine ();

    REQUIRE (top == q.top);
    CHECK (has (&top->sarged_terms, T_ON_NONE));
  }
  {
    two_partitions q;

    QO_PLAN *top = q.s.combine ();

    REQUIRE (g_cp_plans.size () == 1);
    REQUIRE (top == g_cp_plans[0]);
    CHECK (has (&top->sarged_terms, U_ON_NONE));
  }
}
