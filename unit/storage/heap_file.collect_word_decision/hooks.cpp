/* appended to src/storage/heap_file.c: HEAP_CHAIN and HEAP_HDR_STATS are private to the TU */
size_t
w_heap_chain_size (void)
{
  return sizeof (HEAP_CHAIN);
}

size_t
w_heap_hdr_stats_size (void)
{
  return sizeof (HEAP_HDR_STATS);
}

/* slot 0 of an ordinary heap page: the chain, with the page's max MVCCID and flags (heap_page_set_chain () writes
 * the same members) */
void
w_heap_chain_fill (void *buf, MVCCID max_mvccid, bool bestspace)
{
  HEAP_CHAIN *chain = (HEAP_CHAIN *) buf;

  memset (chain, 0, sizeof (*chain));
  OID_SET_NULL (&chain->class_oid);
  VPID_SET_NULL (&chain->prev_vpid);
  VPID_SET_NULL (&chain->next_vpid);
  chain->max_mvccid = max_mvccid;
  chain->flags = bestspace ? HEAP_PAGE_FLAG_BESTSPACE : 0;
}
