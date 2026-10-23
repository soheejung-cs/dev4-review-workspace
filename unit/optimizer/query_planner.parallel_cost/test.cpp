/*
 * PR#8212 (CBRD-27474) -- client-side cost helpers of src/optimizer/query_planner.c (CS_MODE):
 *   qo_mackert_lohman_pages ()   effective cache b = MAX (cost_effective_cache_pages, data_buffer_pages)
 *   qo_parallel_cost_divisor ()  MIN (2, parallelism / 2), 1 under a stoppable LIMIT
 *   qo_hjoin_runs_parallel ()    hash join costed as parallel
 *   qo_scan_runs_parallel () / qo_join_input_factor ()   heap scan input costed as parallel
 * reached through appendix hooks (hooks.cpp).
 *
 * Real code: the TU statics above, system_parameter.c (prm_Def static initializers + prm_set_integer_value).
 * Structures: QO_PLAN / QO_INFO / QO_ENV / QO_PLANNER / QO_NODE / PT_NODE are calloc'ed and only the
 * fields the helpers read are set (plan_type, join_method, scan_method, outer/inner, parallel_opt_use,
 * info->env/planner/cardinality/projected_size, env->pt_tree, node->tcard, select hint and
 * num_parallel_threads, planner->can_apply_limit_card).
 *
 * What the executor does with the same query (read, not run):
 *   - PARALLEL(0|1) hint: qo_check_hjoin_for_parallel_opt () (query_planner.c:16015-16027) returns
 *     PLAN_PARALLEL_OPT_NO, which plan_generation.c:3012-3020 turns into parallelism 0 for the hash join;
 *     xasl_generation.c:17409 passes num_parallel_threads to the scans and compute_parallel_degree ()
 *     (px_parallel.cpp: "hint >= 0 and < start_degree disables parallel execution") runs them serially.
 *   - MERGE: qo_check_hjoin_for_parallel_opt () returns PLAN_PARALLEL_OPT_CANNOT_USE.
 * [defect] cases PASS while the cost model still discounts those plans as parallel.
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "system_parameter.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "string_regex.hpp"
#include <cstdlib>
#include <cmath>

/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

double unitseam_ml_pages (double T, double N);
int unitseam_has_parallel_cost (void);
double unitseam_divisor (QO_PLAN * p);
int unitseam_hjoin_runs_parallel (QO_PLAN * p);
int unitseam_scan_runs_parallel (QO_PLAN * p);
double unitseam_input_factor (QO_PLAN * p, double d);

namespace
{
  /* develop's formula with an explicit b (query_planner.c qo_mackert_lohman_pages, both revisions) */
  double ml_ref (double T, double N, double b)
  {
    double lim, pages;
    T = T < 1.0 ? 1.0 : T;
    if (T <= b)
      {
	pages = (2.0 * T * N) / (2.0 * T + N);
	return pages > T ? T : pages;
      }
    lim = (2.0 * T * b) / (2.0 * T - b);
    return (N <= lim) ? (2.0 * T * N) / (2.0 * T + N) : b + (N - lim) * (T - b) / T;
  }

  struct scene
  {
    PT_NODE *sel;
    QO_ENV *env;
    QO_PLANNER *planner;

    scene (PT_HINT_ENUM hint, int nthreads)
    {
      sel = (PT_NODE *) calloc (1, sizeof (PT_NODE));
      sel->node_type = PT_SELECT;
      sel->info.query.q.select.hint = hint;
      sel->info.query.q.select.num_parallel_threads = nthreads;
      env = (QO_ENV *) calloc (1, sizeof (QO_ENV));
      env->pt_tree = sel;
      planner = (QO_PLANNER *) calloc (1, sizeof (QO_PLANNER));
      planner->can_apply_limit_card = false;
    }

    QO_INFO *info (double card, int projected_size)
    {
      QO_INFO *i = (QO_INFO *) calloc (1, sizeof (QO_INFO));
      i->env = env;
      i->planner = planner;
      i->cardinality = card;
      i->projected_size = projected_size;
      return i;
    }

    /* a heap scan of `pages` pages producing `card` rows of `width` bytes */
    QO_PLAN *seq_scan (double pages, double card, int width)
    {
      QO_PLAN *p = (QO_PLAN *) calloc (1, sizeof (QO_PLAN));
      QO_NODE *n = (QO_NODE *) calloc (1, sizeof (QO_NODE));
      n->tcard = (int) pages;
      p->plan_type = QO_PLANTYPE_SCAN;
      p->plan_un.scan.scan_method = QO_SCANMETHOD_SEQ_SCAN;
      p->plan_un.scan.node = n;
      p->info = info (card, width);
      return p;
    }

    QO_PLAN *hash_join (QO_PLAN * outer, QO_PLAN * inner, QO_PLAN_PARALLEL_OPT_USE opt)
    {
      QO_PLAN *p = (QO_PLAN *) calloc (1, sizeof (QO_PLAN));
      p->plan_type = QO_PLANTYPE_JOIN;
      p->plan_un.join.join_method = QO_JOINMETHOD_HASH_JOIN;
      p->plan_un.join.join_type = JOIN_INNER;
      p->plan_un.join.outer = outer;
      p->plan_un.join.inner = inner;
      p->parallel_opt_use = opt;
      p->info = info (outer->info->cardinality, 64);
      return p;
    }
  };

  void set_defaults ()
  {
    prm_set_integer_value (PRM_ID_PARALLELISM, 4);
    prm_set_integer_value (PRM_ID_PARALLEL_SCAN_PAGE_THRESHOLD, 256);
    prm_set_integer_value (PRM_ID_PARALLEL_HASH_JOIN_PAGE_THRESHOLD, 256);
    prm_set_integer_value (PRM_ID_COST_EFFECTIVE_CACHE_PAGES, 32768);
    prm_set_integer_value (PRM_ID_PB_NBUFFERS, 32768);
  }

  /* 1,000,000 rows x 200 bytes = ~12,207 list pages of 16K: well above the 256-page thresholds */
  const double BIG_CARD = 1000000.0;
  const int BIG_WIDTH = 200;
}

TEST_CASE ("PR8212 ML: data_buffer_pages <= cost_effective_cache_pages prices exactly as before (b = the parameter)",
	   "[contract]")
{
  set_defaults ();
  prm_set_integer_value (PRM_ID_PB_NBUFFERS, 16384);	/* 256M buffer */
  for (double T : { 10.0, 1000.0, 30000.0, 100000.0, 5000000.0 })
    for (double N : { 1.0, 50.0, 40000.0, 1e6, 1e8 })
      {
	CHECK (unitseam_ml_pages (T, N) == ml_ref (T, N, 32768.0));
      }
}

TEST_CASE ("PR8212 ML: a 16G buffer (1,048,576 pages) caches a 100,000-page table that the 32,768-page default could not",
	   "[contract]")
{
  set_defaults ();
  prm_set_integer_value (PRM_ID_PB_NBUFFERS, 1048576);
  /* head: T <= b -> saturates at T = 100,000; base (b = 32,768): ~678,741 */
  CHECK (unitseam_ml_pages (100000.0, 1e6) == ml_ref (100000.0, 1e6, 1048576.0));
  CHECK (unitseam_ml_pages (100000.0, 1e6) == 100000.0);
}

TEST_CASE ("PR8212 ML: cost_effective_cache_pages 0 degrades to 'no caching' (pages fetched == N), as system_parameter.c documents",
	   "[contract]")
{
  set_defaults ();
  prm_set_integer_value (PRM_ID_COST_EFFECTIVE_CACHE_PAGES, 0);
  /* data_buffer_pages keeps its default (its lower limit is 1024 anyway) */
  CHECK (unitseam_ml_pages (100000.0, 5000.0) == 5000.0);
  CHECK (unitseam_ml_pages (10.0, 5000.0) == 5000.0);
  prm_set_integer_value (PRM_ID_COST_EFFECTIVE_CACHE_PAGES, 32768);
}

TEST_CASE ("PR8212 divisor: MIN (2, parallelism / 2); 1 when parallelism <= 2", "[contract]")
{
  if (!unitseam_has_parallel_cost ())
    {
      SUCCEED ("base: no parallel cost helpers");
      return;
    }
  set_defaults ();
  scene s ((PT_HINT_ENUM) 0, 0);
  QO_PLAN *hj = s.hash_join (s.seq_scan (20000, BIG_CARD, BIG_WIDTH), s.seq_scan (20000, BIG_CARD, BIG_WIDTH),
			     PLAN_PARALLEL_OPT_CAN_USE);
  CHECK (unitseam_divisor (hj) == 2.0);
  prm_set_integer_value (PRM_ID_PARALLELISM, 3);
  CHECK (unitseam_divisor (hj) == 1.5);
  prm_set_integer_value (PRM_ID_PARALLELISM, 2);
  CHECK (unitseam_divisor (hj) == 1.0);
  prm_set_integer_value (PRM_ID_PARALLELISM, 16);
  CHECK (unitseam_divisor (hj) == 2.0);
  set_defaults ();
}

TEST_CASE ("PR8212 hash join: no hint, larger input >= 256 pages -> costed parallel; NO_PARALLEL_HASH_JOIN -> serial",
	   "[contract]")
{
  if (!unitseam_has_parallel_cost ())
    {
      SUCCEED ("base: no parallel cost helpers");
      return;
    }
  set_defaults ();
  {
    scene s ((PT_HINT_ENUM) 0, 0);
    QO_PLAN *hj = s.hash_join (s.seq_scan (20000, BIG_CARD, BIG_WIDTH), s.seq_scan (10, 100, 16),
			       PLAN_PARALLEL_OPT_CAN_USE);
    CHECK (unitseam_hjoin_runs_parallel (hj) == 1);
  }
  {
    scene s (PT_HINT_NO_PARALLEL_HASH_JOIN, 0);
    QO_PLAN *hj = s.hash_join (s.seq_scan (20000, BIG_CARD, BIG_WIDTH), s.seq_scan (10, 100, 16),
			       PLAN_PARALLEL_OPT_NO);
    CHECK (unitseam_hjoin_runs_parallel (hj) == 0);
  }
}

TEST_CASE ("PR8212 F2 hash join: PARALLEL(1) -- executor serial (parallel_opt_use = NO), cost model still divides",
	   "[defect]")
{
  if (!unitseam_has_parallel_cost ())
    {
      FAIL ("base: no parallel cost helpers (not applicable)");
    }
  set_defaults ();
  scene s (PT_HINT_PARALLEL, 1);
  /* qo_check_hjoin_for_parallel_opt () for this hint: PLAN_PARALLEL_OPT_NO (query_planner.c:16015-16027) */
  QO_PLAN *hj = s.hash_join (s.seq_scan (20000, BIG_CARD, BIG_WIDTH), s.seq_scan (10, 100, 16), PLAN_PARALLEL_OPT_NO);
  CHECK (unitseam_hjoin_runs_parallel (hj) == 1);
}

TEST_CASE ("PR8212 F2 hash join: MERGE statement -- executor never parallel (parallel_opt_use = CANNOT_USE), cost model still divides",
	   "[defect]")
{
  if (!unitseam_has_parallel_cost ())
    {
      FAIL ("base: no parallel cost helpers (not applicable)");
    }
  set_defaults ();
  scene s ((PT_HINT_ENUM) 0, 0);
  QO_PLAN *hj = s.hash_join (s.seq_scan (20000, BIG_CARD, BIG_WIDTH), s.seq_scan (10, 100, 16),
			     PLAN_PARALLEL_OPT_CANNOT_USE);
  CHECK (unitseam_hjoin_runs_parallel (hj) == 1);
}

TEST_CASE ("PR8212 scan input: 20,000-page heap scan, no hint -> factor 1/2; NO_PARALLEL_SCAN -> 1", "[contract]")
{
  if (!unitseam_has_parallel_cost ())
    {
      SUCCEED ("base: no parallel cost helpers");
      return;
    }
  set_defaults ();
  {
    scene s ((PT_HINT_ENUM) 0, 0);
    QO_PLAN *sc = s.seq_scan (20000, BIG_CARD, BIG_WIDTH);
    CHECK (unitseam_scan_runs_parallel (sc) == 1);
    CHECK (unitseam_input_factor (sc, 2.0) == 0.5);
  }
  {
    scene s (PT_HINT_NO_PARALLEL_SCAN, 0);
    QO_PLAN *sc = s.seq_scan (20000, BIG_CARD, BIG_WIDTH);
    CHECK (unitseam_scan_runs_parallel (sc) == 0);
    CHECK (unitseam_input_factor (sc, 2.0) == 1.0);
  }
}

TEST_CASE ("PR8212 F2 scan input: PARALLEL(0) / PARALLEL(1) -- executor serial, cost model still halves the scan",
	   "[defect]")
{
  if (!unitseam_has_parallel_cost ())
    {
      FAIL ("base: no parallel cost helpers (not applicable)");
    }
  set_defaults ();
  for (int n : { 0, 1 })
    {
      scene s (PT_HINT_PARALLEL, n);
      QO_PLAN *sc = s.seq_scan (20000, BIG_CARD, BIG_WIDTH);
      CHECK (unitseam_scan_runs_parallel (sc) == 1);
      CHECK (unitseam_input_factor (sc, 2.0) == 0.5);
    }
}
