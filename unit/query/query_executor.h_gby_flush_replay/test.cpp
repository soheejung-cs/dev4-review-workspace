/* PR#8196 (CBRD-27361) -- the page batch hash GROUP BY node, src/query/query_executor.c, group H review (head 2381ce437).
 *
 * qexec_batch_gby_flush () claims to replay what the row path would have done for the pending rows of cached groups
 * (the comment block before QEXEC_GBY_CACHE_SIZE): each group's rows accumulated in row order, the groups touched in
 * the table's LRU in the order of their last row, the very-high-selectivity check made at the very tuple counts the
 * row path makes it at.  qexec_gby_cache_add () keeps at most half of QEXEC_GBY_CACHE_SIZE entries, starting over when
 * full, one key copy per entry.  These are contracts: a head FAIL with a base PASS would be a regression (base has no
 * such functions -- the suite applies from PR#8196 on). */
#include "catch.hpp"

#define H_MAX_ROWS 256
#define H_MAX_CALLS 64

struct h_flush_result
{
  int done;
  int n_calls;
  int call_group[H_MAX_CALLS];
  int call_n[H_MAX_CALLS];
  int call_sel[H_MAX_CALLS][H_MAX_ROWS];
  int n_touch;
  int touch_group[H_MAX_CALLS];
  int off;
  int dumps;
  int syncs;
  int tuple_count_after;
  int state_after;
  int evictions_after;
  int n_cached_after;
  int value_tuple_count[H_MAX_CALLS];
  int counts_reset;
  int sel_restored;
};

int h_flush_scenario (int n, const int *rows, const int *grp, int ng, int tuple_count0, int group_count0,
		      struct h_flush_result *res);
int h_cache_add_scenario (int n, int *found_first, int *found_last, int *copies, int *frees_key);
int h_cache_same_group_scenario (int *found_a, int *found_b, int *slot_is_b, int *copies, int *frees_key);

static h_flush_result R;

TEST_CASE ("flush: each group's rows go to its accumulators in row order, one call per group, groups in first-row order", "[contract]")
{
  /* chunk rows 3,5,8,9,12,14,20 of groups B A B C A B A */
  const int rows[7] = { 3, 5, 8, 9, 12, 14, 20 };
  const int grp[7] = { 1, 0, 1, 2, 0, 1, 0 };
  REQUIRE (h_flush_scenario (7, rows, grp, 3, 100, 10, &R) == 0);
  CHECK (R.done == 7);
  REQUIRE (R.n_calls == 3);
  /* first-row order: B (row 3), A (row 5), C (row 9) */
  CHECK (R.call_group[0] == 1);
  CHECK (R.call_group[1] == 0);
  CHECK (R.call_group[2] == 2);
  CHECK (R.call_n[0] == 3);
  CHECK ((R.call_sel[0][0] == 3 && R.call_sel[0][1] == 8 && R.call_sel[0][2] == 14));
  CHECK (R.call_n[1] == 3);
  CHECK ((R.call_sel[1][0] == 5 && R.call_sel[1][1] == 12 && R.call_sel[1][2] == 20));
  CHECK (R.call_n[2] == 1);
  CHECK (R.call_sel[2][0] == 9);
  CHECK (R.sel_restored == 1);
}

TEST_CASE ("flush: the groups are touched in the table's LRU in the order of their last row, each once", "[contract]")
{
  /* last rows: B at t=5 (row 14), A at t=6 (row 20), C at t=3 (row 9) -> C, B, A */
  const int rows[7] = { 3, 5, 8, 9, 12, 14, 20 };
  const int grp[7] = { 1, 0, 1, 2, 0, 1, 0 };
  REQUIRE (h_flush_scenario (7, rows, grp, 3, 100, 10, &R) == 0);
  REQUIRE (R.n_touch == 3);
  CHECK (R.touch_group[0] == 2);
  CHECK (R.touch_group[1] == 1);
  CHECK (R.touch_group[2] == 0);
  CHECK (R.counts_reset == 1);
}

TEST_CASE ("flush: the table's tuple count and each group's take the rows; the table keeps taking rows below the check", "[contract]")
{
  const int rows[7] = { 3, 5, 8, 9, 12, 14, 20 };
  const int grp[7] = { 1, 0, 1, 2, 0, 1, 0 };
  REQUIRE (h_flush_scenario (7, rows, grp, 3, 100, 10, &R) == 0);
  CHECK (R.tuple_count_after == 107);
  CHECK (R.value_tuple_count[0] == 3);
  CHECK (R.value_tuple_count[1] == 3);
  CHECK (R.value_tuple_count[2] == 1);
  CHECK (R.off == 0);
  CHECK (R.dumps == 0);
  CHECK (R.n_cached_after == 3);
}

TEST_CASE ("flush: the selectivity check at tuple 2000 stops the flush after that row, as the row path would, and dumps the table once", "[contract]")
{
  /* 1990 tuples, 1990 groups (ratio 0.995 > 0.95 at 2000): 20 pending rows, the 10th brings the count to 2000 */
  int rows[20], grp[20];
  for (int t = 0; t < 20; t++)
    {
      rows[t] = t;
      grp[t] = t % 2;
    }
  REQUIRE (h_flush_scenario (20, rows, grp, 2, 1990, 1990, &R) == 0);
  CHECK (R.done == 10);
  CHECK (R.tuple_count_after == 2000);
  CHECK (R.value_tuple_count[0] + R.value_tuple_count[1] == 10);
  CHECK (R.off == 1);
  CHECK (R.dumps == 1);
  CHECK (R.syncs == 1);
  CHECK (R.state_after == 2);	/* HS_REJECT_ALL */
  CHECK (R.evictions_after == 1);
  CHECK (R.n_cached_after == 0);
}

TEST_CASE ("flush: a ratio under the cutoff at tuple 2000 keeps the table (control)", "[contract]")
{
  int rows[20], grp[20];
  for (int t = 0; t < 20; t++)
    {
      rows[t] = t;
      grp[t] = t % 2;
    }
  REQUIRE (h_flush_scenario (20, rows, grp, 2, 1990, 500, &R) == 0);
  CHECK (R.done == 20);
  CHECK (R.tuple_count_after == 2010);
  CHECK (R.off == 0);
  CHECK (R.dumps == 0);
}

TEST_CASE ("cache_add: 512 entries at most -- the 513th add starts over, one key copy per entry, the clear releases them all", "[contract]")
{
  int first = -1, last = -1, copies = -1, frees = -1;
  int n_cached = h_cache_add_scenario (512, &first, &last, &copies, &frees);
  CHECK (n_cached == 512);
  CHECK (first == 1);
  CHECK (last == 1);
  CHECK (copies == 512);
  CHECK (frees == 0);

  n_cached = h_cache_add_scenario (513, &first, &last, &copies, &frees);
  CHECK (n_cached == 1);
  CHECK (first == 0);		/* the first 512 went with the clear */
  CHECK (last == 1);		/* the 513th is there, its value naming its slot */
  CHECK (copies == 513);
  CHECK (frees == 512);
}

/* e90e0705a: the cache's invariant (QEXEC_BATCH_GBY) -- a group has at most one entry, the one its batch_slot names,
 * which is the one qexec_gby_cache_evicted () drops when the table frees the group.  Before it a second byte string of
 * the group added a second entry and moved batch_slot to it, so the first outlived the group's eviction. */
TEST_CASE ("cache_add: one group under two byte strings keeps one entry -- the later bytes replace the earlier, batch_slot names it", "[contract]")
{
  int found_a = -1, found_b = -1, slot_is_b = -1, copies = -1, frees = -1;
  int n_cached = h_cache_same_group_scenario (&found_a, &found_b, &slot_is_b, &copies, &frees);
  CHECK (n_cached == 1);
  CHECK (found_a == 0);
  CHECK (found_b == 1);
  CHECK (slot_is_b == 1);
  CHECK (copies == 2);
  CHECK (frees == 1);		/* the earlier entry's key copy */
}
