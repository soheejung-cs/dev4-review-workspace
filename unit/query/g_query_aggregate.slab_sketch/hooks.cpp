/* unitseam appendix to src/query/query_aggregate.cpp (same TU): the slab's layout -- struct agg_group_slab is
 * this TU's own type, so a test reads its block size and offsets through here. */
void
unitseam_slab_layout (const agg_group_slab * slab, size_t * block_size, size_t * off_keyptrs, size_t * off_keyvals,
		      size_t * off_value, size_t * off_accs, size_t * off_accvals, size_t * off_tuple, int *inline_cap)
{
  *block_size = slab->block_size;
  *off_keyptrs = slab->off_keyptrs;
  *off_keyvals = slab->off_keyvals;
  *off_value = slab->off_value;
  *off_accs = slab->off_accs;
  *off_accvals = slab->off_accvals;
  *off_tuple = slab->off_tuple;
  *inline_cap = slab->inline_cap;
}
