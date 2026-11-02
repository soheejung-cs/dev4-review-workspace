/*
 * Contract suite -- CBRD-27851 (PR#8363): btree_range_scan_start (src/storage/btree.c, SERVER_MODE) and the
 * restart-from-root request (bts->force_restart_from_root) of a descending range scan.
 *   - a start meets a request left set (it locates from the root): it clears the request on entry;
 *   - a restart this start needs (raised while it advances to its first key) is returned with: the flag set, the scan
 *     not started -- btree_range_scan reads the flag after the start returns to start over;
 *   - so a start after one that had to restart positions normally (the hang of CBRD-27851 at base).
 * Real code: btree_range_scan_start (hooks.cpp), with lower_key NULL (the btree_find_lower_bound_leaf branch).
 * Fakes (seams):
 *   btree_find_lower_bound_leaf -- positions nothing (C_page stays NULL), counts calls.
 *   btree_range_scan_advance_over_filtered_keys -- the real one returns at once when it finds the flag set after its
 *       previous-leaf step (btree.c @570e44137 29381); the fake plays a scripted answer per call: "ok" leaves the flag
 *       as it finds it and reports that, "restart" raises it (the previous-leaf step lost its leaf).
 * Tags: [contract]; at base 1684945ed R1 and R3 FAIL (the request is never cleared on the start path).
 */
#include "catch.hpp"
#include "config.h"
#include "btree.h"
#include "btree_load.h"
#include <cstdlib>
#include <cstring>
#include <vector>

int unitseam_range_scan_start (THREAD_ENTRY * thread_p, BTREE_SCAN * bts);

static int g_find_calls;
static std::vector<int> g_script;	/* per advance call: 0 = ok, 1 = raise a restart */
static size_t g_step;
static std::vector<int> g_flag_seen;	/* the flag each advance call found */

int
btree_find_lower_bound_leaf (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTREE_STATS * stat_info_p)
{
  (void) thread_p;
  (void) stat_info_p;
  (void) bts;
  g_find_calls++;
  return NO_ERROR;
}

int
btree_range_scan_advance_over_filtered_keys (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  (void) thread_p;
  g_flag_seen.push_back (bts->force_restart_from_root ? 1 : 0);
  int what = g_step < g_script.size () ? g_script[g_step] : 0;
  g_step++;
  if (what == 1)
    {
      bts->force_restart_from_root = true;
    }
  return NO_ERROR;
}

static BTREE_SCAN *
mk_bts (bool restart_pending)
{
  BTREE_SCAN *bts = (BTREE_SCAN *) calloc (1, sizeof (BTREE_SCAN));
  VPID_SET_NULL (&bts->C_vpid);
  bts->C_page = NULL;
  bts->use_desc_index = true;
  bts->is_cur_key_compressed = false;
  bts->common_prefix_size = COMMON_PREFIX_UNKNOWN;
  bts->key_range.lower_key = NULL;
  bts->force_restart_from_root = restart_pending;
  return bts;
}

static void
reset (std::vector<int> script)
{
  g_find_calls = 0;
  g_script = script;
  g_step = 0;
  g_flag_seen.clear ();
}

TEST_CASE ("R1 a start meets a restart request left set: it clears it on entry and starts the scan", "[contract]")
{
  reset ({0});
  BTREE_SCAN *bts = mk_bts (true);
  REQUIRE (unitseam_range_scan_start (NULL, bts) == NO_ERROR);
  CHECK (g_find_calls == 1);
  REQUIRE (g_flag_seen.size () == 1);
  CHECK (g_flag_seen[0] == 0);	/* the advance saw no stale request */
  CHECK_FALSE (bts->force_restart_from_root);
  CHECK (bts->is_scan_started);
}

TEST_CASE ("R2 a restart this start needs is returned with: flag set, scan not started, nothing held", "[contract]")
{
  reset ({1});
  BTREE_SCAN *bts = mk_bts (false);
  REQUIRE (unitseam_range_scan_start (NULL, bts) == NO_ERROR);
  CHECK (bts->force_restart_from_root);
  CHECK_FALSE (bts->is_scan_started);
  CHECK (bts->C_page == NULL);
  CHECK (VPID_ISNULL (&bts->C_vpid));
}

TEST_CASE ("R3 the start after one that had to restart positions normally: the loop of starts ends", "[contract]")
{
  reset ({1, 0});
  BTREE_SCAN *bts = mk_bts (false);
  int starts = 0;
  /* btree_range_scan's loop: start again while the start returns with a restart request (bounded here) */
  do
    {
      REQUIRE (unitseam_range_scan_start (NULL, bts) == NO_ERROR);
      starts++;
    }
  while (bts->force_restart_from_root && !bts->is_scan_started && starts < 10);
  CHECK (starts == 2);
  CHECK (bts->is_scan_started);
  CHECK_FALSE (bts->force_restart_from_root);
  REQUIRE (g_flag_seen.size () == 2);
  CHECK (g_flag_seen[1] == 0);
}

TEST_CASE ("R4 a start with no request and no restart starts the scan and leaves the flag down", "[contract]")
{
  reset ({0});
  BTREE_SCAN *bts = mk_bts (false);
  REQUIRE (unitseam_range_scan_start (NULL, bts) == NO_ERROR);
  CHECK (bts->is_scan_started);
  CHECK_FALSE (bts->force_restart_from_root);
}
