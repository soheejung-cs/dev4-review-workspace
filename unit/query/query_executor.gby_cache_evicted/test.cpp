/* PR#8196 (CBRD-27361) -- page batch hash GROUP BY cache, src/query/query_executor.c.
 * qexec_gby_cache_evicted () replays the LRU head groups a row-path call evicted (noted before the call:
 * value pointer + batch_slot) by removing their cache entries; qexec_gby_cache_remove () is a backward-shift
 * delete of a linear-probe table, so removing one entry moves the later entries of its cluster.
 * Claim under test (bot comment at query_executor.c:11913, verified here): with two or more evicted groups
 * in one cluster, the second's noted slot no longer holds it after the first removal, so its entry stays. */
#include "catch.hpp"

int unitseam_gby_evict_scenario (int n_evict, int home, int *n_left, int *stale_found, int *stale_slot);
int unitseam_gby_evict_scenario_keep (int n, int home, unsigned keep_mask, int *n_left, int *stale_found,
				      int *stale_slot, int *kept_found);
extern int unitseam_gby_pool_balanced;

TEST_CASE ("one evicted group of a cluster leaves no entry", "[contract]")
{
  int left = -1, stale = -1, slot = -1;
  unitseam_gby_evict_scenario (1, 5, &left, &stale, &slot);
  CHECK (left == 0);
  CHECK (stale == 0);
}

TEST_CASE ("evicted entries give their key buffers back to the cache's pool (68caaa830)", "[contract]")
{
  int left = -1, stale = -1, slot = -1, kept = -1;
  unitseam_gby_evict_scenario_keep (4, 1022, 1u << 1, &left, &stale, &slot, &kept);
  CHECK (left == 1);
  CHECK (unitseam_gby_pool_balanced == 1);
}

TEST_CASE ("two groups of one cluster evicted together leave no entry", "[contract]")
{
  int left = -1, stale = -1, slot = -1;
  unitseam_gby_evict_scenario (2, 5, &left, &stale, &slot);
  CHECK (left == 0);
  CHECK (stale == 0);
}

TEST_CASE ("three groups of one cluster evicted together leave no entry", "[contract]")
{
  int left = -1, stale = -1, slot = -1;
  unitseam_gby_evict_scenario (3, 5, &left, &stale, &slot);
  CHECK (left == 0);
  CHECK (stale == 0);
}

TEST_CASE ("cluster at the table's wrap-around: three evicted, none left", "[contract]")
{
  int left = -1, stale = -1, slot = -1;
  unitseam_gby_evict_scenario (3, 1023, &left, &stale, &slot);
  CHECK (left == 0);
  CHECK (stale == 0);
}

TEST_CASE ("cluster probe at the wrap-around: a kept group between two evicted ones, the tagged entries are pulled back and still removed, the kept one still found", "[contract]")
{
  /* cluster home 1022: groups 0..3 in slots 1022, 1023, 0, 1; group 1 stays. Removing group 0 shifts 1, 2, 3
   * back by one (across the wrap): the tags of 2 and 3 now sit in slots 1023 and 0, where the home-cluster
   * probe must find them; a table-wide rescan is not what finds them any more. */
  int left = -1, stale = -1, slot = -1, kept = -1;
  unitseam_gby_evict_scenario_keep (4, 1022, 1u << 1, &left, &stale, &slot, &kept);
  CHECK (left == 1);
  CHECK (stale == 0);
  CHECK (kept == 1);
}

TEST_CASE ("cluster probe: the kept group is the cluster's last entry (n=3 wrap), both evicted before it removed", "[contract]")
{
  int left = -1, stale = -1, slot = -1, kept = -1;
  unitseam_gby_evict_scenario_keep (3, 1023, 1u << 2, &left, &stale, &slot, &kept);
  CHECK (left == 1);
  CHECK (stale == 0);
  CHECK (kept == 1);
}

TEST_CASE ("PR8196 F1: the second evicted group of a cluster stays in the cache, moved into the first's slot (PASS = defect present)", "[defect]")
{
  int left = -1, stale = -1, slot = -1;
  unitseam_gby_evict_scenario (2, 5, &left, &stale, &slot);
  CHECK (left == 1);
  CHECK (stale == 1);
  CHECK (slot == 5);
}
