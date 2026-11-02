/* appended to btree.c (same TU): reach the static btree_range_scan_start from the test. */
int
unitseam_range_scan_start (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  return btree_range_scan_start (thread_p, bts);
}
