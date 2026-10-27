/*
 * unitseam appendix for src/storage/btree.c (compiled at the end of that TU) - for
 * unit/storage/btree.prefix_value_hash.
 *
 * unitseam_prefix_value_hash (v): the 64-bit value btree_get_stats_midxkey () mixes into the prefix hash reservoir for
 * one non-NULL key element at this revision ("hash = hash * 1099511628211ULL + val_hash"; at level 0 the reservoir
 * slot holds exactly this value):
 *   - a revision with btree_get_stats_prefix_value_hash () (CBRD-27701, PR#8237): that function;
 *   - a revision before it: mht_valhash (&elem, 0xFFFFFFFEU) on the one local DB_VALUE 'elem' the function reuses for
 *     every element (btree.c:6842/6850 @89722cf4e) - the template below, copying into one static DB_VALUE.
 * Overload resolution picks: when the TU has the non-template btree_get_stats_prefix_value_hash (const DB_VALUE *),
 * it beats the template for a const DB_VALUE * argument; otherwise the template is the only candidate.
 *
 * unitseam_duj1_estimate (): btree_get_stats_duj1_estimate () (static) on a reservoir of n level-0 hashes - what the
 * pkeys[0] estimate becomes when the skip scan is exhausted or priced out (btree_get_stats_prefix_skip_scan).
 */
static DB_VALUE unitseam_elem;
static bool unitseam_used_legacy = false;

template < typename V > static UINT64
btree_get_stats_prefix_value_hash (const V * value)
{
  unitseam_used_legacy = true;
  unitseam_elem = *value;
  return (UINT64) mht_valhash (&unitseam_elem, 0xFFFFFFFEU);
}

UINT64
unitseam_prefix_value_hash (const DB_VALUE * value)
{
  return btree_get_stats_prefix_value_hash (value);
}

/* true when this revision has no btree_get_stats_prefix_value_hash () and the legacy element hash is in use */
bool
unitseam_prefix_hash_is_legacy (void)
{
  DB_VALUE v;

  db_make_int (&v, 1);
  unitseam_used_legacy = false;
  (void) btree_get_stats_prefix_value_hash (&v);
  return unitseam_used_legacy;
}

static UINT64 unitseam_reservoir[BTREE_STATS_DUJ1_SAMPLE];

INT64
unitseam_duj1_estimate (const UINT64 * hashes, int n, INT64 total_keys)
{
  static BTREE_STATS stats;
  static BTREE_STATS_ENV env;

  assert (n > 0 && n <= BTREE_STATS_DUJ1_SAMPLE);
  memcpy (unitseam_reservoir, hashes, sizeof (UINT64) * n);
  stats.keys = total_keys;
  env.stat_info = &stats;
  env.sample_size = n;
  env.prefix_sample = unitseam_reservoir;
  return btree_get_stats_duj1_estimate (&env, 0);
}

int
unitseam_duj1_sample_capacity (void)
{
  return BTREE_STATS_DUJ1_SAMPLE;
}
