/*
 * PR#8166 (CBRD-27249) -- sm_update_all_statistics (with_fullscan, random_seed, no_histogram, skip_mops, n_skip)
 *
 * Unit under test: src/object/schema_manager.c (SA_MODE) -- sm_update_all_statistics (), the client side of
 * UPDATE STATISTICS ON ALL CLASSES and of optimizedb's all-classes run. It flushes the workspace, asks the
 * server to refresh every class's statistics (stats_update_all_statistics), re-reads the cached statistics of
 * the resident classes, then runs the client histogram pass (update_histogram_for_all_classes) unless
 * no_histogram. The PR threads optimizedb's --no-histogram-classes list (skip_mops, n_skip) through to the
 * histogram pass and returns early when the server-side refresh fails.
 *
 * Every callee is in another TU and is a fake that records the call (see the comment on each). The server
 * round trip (stats_update_all_statistics) and the histogram pass are fakes -- their arguments are what is
 * checked. The resident class list is the test's own (ws_Resident_classes is defined here).
 *
 * The PR changed the signature, so the call and the update_histogram_for_all_classes fake are selected by the
 * arity of the declaration in the revision's own header: the skip-list checks run only where it exists.
 */
#include "catch.hpp"
#include "config.h"

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "error_code.h"
#include "dbtype_def.h"
#include "work_space.h"
#include "class_object.h"
#include "schema_manager.h"
#include "statistics.h"
#include "locator_cl.h"
#include "network_interface_cl.h"

/* ---- arity of a function pointer type ---- */
template < typename F > struct arity;
template < typename R, typename ... A > struct arity <R (*)(A ...)>
{
  static constexpr int value = sizeof ... (A);
};

static constexpr int SM_ARITY = arity < decltype (&sm_update_all_statistics) >::value;	/* 3 base, 5 head */

template < typename F > static int
call_sm (F f, bool fullscan, int seed, int no_hist, MOP * skip, int n)
{
  if constexpr (arity < F >::value == 5)
    {
      return f (fullscan, seed, no_hist, skip, n);
    }
  else
    {
      return f (fullscan, seed, no_hist);
    }
}

static int
run_sm (bool fullscan, int seed, int no_hist, MOP * skip, int n)
{
  return call_sm (&sm_update_all_statistics, fullscan, seed, no_hist, skip, n);
}

/* ---- recorders ---- */
static std::vector < std::string > g_seq;	/* call order */
static int g_flush_rc, g_stats_rc, g_hist_rc, g_flush_class_rc, g_get_rc;
static int g_stats_fullscan = -1, g_stats_print = -1;
static int g_hist_seed = -1, g_hist_n = -1;
static std::vector < MOP > g_hist_skip;
static int g_hist_arity;
static int g_last_er;
static std::vector < CLASS_STATS * >g_freed;
static int g_got;

static void
reset ()
{
  g_seq.clear ();
  g_flush_rc = g_stats_rc = g_hist_rc = g_flush_class_rc = g_get_rc = NO_ERROR;
  g_stats_fullscan = g_stats_print = -1;
  g_hist_seed = g_hist_n = -1;
  g_hist_skip.clear ();
  g_hist_arity = 0;
  g_last_er = NO_ERROR;
  g_freed.clear ();
  g_got = 0;
}

/* ---- the resident class list (work_space.c global) ---- */
DB_OBJLIST *ws_Resident_classes = NULL;

static CLASS_STATS g_old_stats, g_new_stats;	/* only their addresses are used */

struct Resident
{
  DB_OBJECT mop;
  alignas (SM_CLASS) unsigned char cls[sizeof (SM_CLASS)];
  DB_OBJLIST link;
  bool deleted;
};

static Resident g_res[3];

static SM_CLASS *
res_class (int i)
{
  return reinterpret_cast < SM_CLASS * >(g_res[i].cls);
}

/* [0] a cached class with statistics, [1] a cached view, [2] a deleted class */
static void
resident_init (bool with_list)
{
  memset (g_res, 0, sizeof (g_res));
  for (int i = 0; i < 3; i++)
    {
      g_res[i].mop.oid_info.oid.pageid = 200;
      g_res[i].mop.oid_info.oid.slotid = (short) (i + 1);
      g_res[i].mop.object = g_res[i].cls;
      g_res[i].link.op = &g_res[i].mop;
      g_res[i].link.next = (i < 2) ? &g_res[i + 1].link : NULL;
    }
  res_class (0)->class_type = SM_CLASS_CT;
  res_class (0)->stats = &g_old_stats;
  res_class (1)->class_type = SM_VCLASS_CT;
  res_class (2)->class_type = SM_CLASS_CT;
  g_res[2].deleted = true;
  ws_Resident_classes = with_list ? &g_res[0].link : NULL;
}

/* ---- fakes ---- */

/* locator_cl.c: flush every dirty object; on failure er_errid () holds the error */
int
locator_all_flush (void)
{
  g_seq.push_back ("flush");
  if (g_flush_rc != NO_ERROR)
    {
      g_last_er = g_flush_rc;
    }
  return g_flush_rc;
}

int
locator_flush_class (MOP class_mop)
{
  g_seq.push_back ("flush_class");
  if (g_flush_class_rc != NO_ERROR)
    {
      g_last_er = g_flush_class_rc;
    }
  return g_flush_class_rc;
}

/* network_interface_cl.c: the server refresh of every top-level class (one round trip per class) */
int
stats_update_all_statistics (int with_fullscan, int print_summary)
{
  g_seq.push_back ("stats");
  g_stats_fullscan = with_fullscan;
  g_stats_print = print_summary;
  return g_stats_rc;
}

/* the histogram pass, PR base signature */
int
update_histogram_for_all_classes (int random_seed)
{
  g_seq.push_back ("hist");
  g_hist_arity = 1;
  g_hist_seed = random_seed;
  return g_hist_rc;
}

/* the histogram pass, PR head signature */
int
update_histogram_for_all_classes (int random_seed, MOP * skip_mops, int n_skip)
{
  g_seq.push_back ("hist");
  g_hist_arity = 3;
  g_hist_seed = random_seed;
  g_hist_n = n_skip;
  for (int i = 0; skip_mops != NULL && i < n_skip; i++)
    {
      g_hist_skip.push_back (skip_mops[i]);
    }
  return g_hist_rc;
}

/* work_space.c: the deleted flag */
int
ws_is_deleted (MOP mop)
{
  for (auto &r : g_res)
    {
      if (&r.mop == mop)
	{
	  return r.deleted ? 1 : 0;
	}
    }
  return 0;
}

/* statistics_cl.c */
void
stats_free_statistics (CLASS_STATS * stats)
{
  g_freed.push_back (stats);
}

int
stats_get_statistics (OID * classoid, unsigned int timestamp, CLASS_STATS ** stats_p)
{
  g_seq.push_back ("get:" + std::to_string (classoid->slotid));
  g_got++;
  if (g_get_rc != NO_ERROR)
    {
      return g_get_rc;
    }
  *stats_p = &g_new_stats;
  return NO_ERROR;
}

int
er_errid (void)
{
  return g_last_er;
}

static std::string
seq ()
{
  std::string s;
  for (auto &x : g_seq)
    {
      s += (s.empty ()? "" : ",") + x;
    }
  return s;
}

/* ================================================================================================ */

TEST_CASE ("histograms on: flush, server refresh (no summary), resident cache re-read, then the histogram pass",
	   "[contract]")
{
  reset ();
  resident_init (true);
  int rc = run_sm (false, 7, 0, NULL, 0);

  CHECK (rc == NO_ERROR);
  CHECK (seq () == "flush,stats,flush_class,get:1,hist");
  CHECK (g_stats_fullscan == 0);
  CHECK (g_stats_print == 0);
  CHECK (g_hist_seed == 7);
  REQUIRE (g_freed.size () == 1);
  CHECK (g_freed[0] == &g_old_stats);
  CHECK (res_class (0)->stats == &g_new_stats);
  CHECK (res_class (1)->stats == NULL);	/* views and deleted classes are left alone */
  CHECK (res_class (2)->stats == NULL);
}

TEST_CASE ("WITH FULLSCAN reaches the server refresh", "[contract]")
{
  reset ();
  resident_init (false);
  int rc = run_sm (true, 0, 0, NULL, 0);

  CHECK (rc == NO_ERROR);
  CHECK (g_stats_fullscan == 1);
}

TEST_CASE ("no_histogram: the server refresh prints the summary, the histogram pass is not run", "[contract]")
{
  reset ();
  resident_init (true);
  int rc = run_sm (false, 0, 1, NULL, 0);

  CHECK (rc == NO_ERROR);
  CHECK (seq () == "flush,stats,flush_class,get:1");
  CHECK (g_stats_print == 1);
  CHECK (g_hist_arity == 0);
}

TEST_CASE ("workspace flush fails: its error, nothing else runs", "[contract]")
{
  reset ();
  resident_init (true);
  g_flush_rc = ER_FAILED;
  int rc = run_sm (false, 0, 0, NULL, 0);

  CHECK (rc == ER_FAILED);
  CHECK (seq () == "flush");
}

TEST_CASE ("re-reading a resident class's statistics fails: its error, the histogram pass is not run", "[contract]")
{
  reset ();
  resident_init (true);
  g_get_rc = ER_FAILED;
  int rc = run_sm (false, 0, 0, NULL, 0);

  CHECK (rc == ER_FAILED);
  CHECK (seq () == "flush,stats,flush_class,get:1");
  CHECK (g_hist_arity == 0);
}

TEST_CASE ("server-side refresh fails: that error is returned and no histogram is built over it "
	   "(PR base swallowed it when the histogram pass succeeded)", "[contract]")
{
  reset ();
  resident_init (true);
  g_stats_rc = ER_FAILED;
  int rc = run_sm (false, 0, 0, NULL, 0);

  CHECK (rc == ER_FAILED);
  CHECK (g_hist_arity == 0);
  CHECK (g_got == 0);		/* the resident cache is not re-read over a failed refresh */
}

TEST_CASE ("server-side refresh fails with no_histogram: that error is returned", "[contract]")
{
  reset ();
  resident_init (true);
  g_stats_rc = ER_FAILED;
  int rc = run_sm (false, 0, 1, NULL, 0);

  CHECK (rc == ER_FAILED);
  CHECK (g_hist_arity == 0);
}

TEST_CASE ("the histogram pass's error is returned", "[contract]")
{
  reset ();
  resident_init (false);
  g_hist_rc = ER_FAILED;
  int rc = run_sm (false, 0, 0, NULL, 0);

  CHECK (rc == ER_FAILED);
  CHECK (seq () == "flush,stats,hist");
}

/* ---- the skip list (new parameters) ---- */

TEST_CASE ("skip list: the MOPs and their count reach the histogram pass unchanged", "[contract][new]")
{
  if (SM_ARITY != 5)
    {
      FAIL ("sm_update_all_statistics has no skip list in this revision (new code, not on the PR base)");
    }
  reset ();
  resident_init (false);
  DB_OBJECT a {}, b {};
  MOP skip[2] = { &a, &b };
  int rc = run_sm (false, 3, 0, skip, 2);

  CHECK (rc == NO_ERROR);
  CHECK (g_hist_arity == 3);
  CHECK (g_hist_seed == 3);
  CHECK (g_hist_n == 2);
  REQUIRE (g_hist_skip.size () == 2);
  CHECK (g_hist_skip[0] == &a);
  CHECK (g_hist_skip[1] == &b);
}

TEST_CASE ("skip list with no_histogram: nothing to skip, the histogram pass is not run", "[contract][new]")
{
  if (SM_ARITY != 5)
    {
      FAIL ("sm_update_all_statistics has no skip list in this revision (new code, not on the PR base)");
    }
  reset ();
  resident_init (false);
  DB_OBJECT a {};
  MOP skip[1] = { &a };
  int rc = run_sm (false, 0, 1, skip, 1);

  CHECK (rc == NO_ERROR);
  CHECK (g_hist_arity == 0);
  CHECK (g_stats_print == 1);
}

TEST_CASE ("no skip list (UPDATE STATISTICS ON ALL CLASSES passes NULL, 0)", "[contract]")
{
  reset ();
  resident_init (false);
  int rc = run_sm (false, 0, 0, NULL, 0);

  CHECK (rc == NO_ERROR);
  if (SM_ARITY == 5)
    {
      CHECK (g_hist_arity == 3);
      CHECK (g_hist_n == 0);
      CHECK (g_hist_skip.empty ());
    }
  else
    {
      CHECK (g_hist_arity == 1);
    }
}
