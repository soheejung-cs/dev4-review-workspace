/* appended to src/query/scan_manager.c: the test reaches the reuse list (static) through these */
int
ut_oidbuf_pool_count (void)
{
  return scan_Iscan_oid_buf_list_count;
}

BTREE_ISCAN_OID_LIST *
ut_oidbuf_pool_head (void)
{
  return scan_Iscan_oid_buf_list;
}

void
ut_oidbuf_release (BTREE_ISCAN_OID_LIST * oid_list)
{
  /* what scan_close_scan () does with the scan's list: back to the reuse list */
  scan_free_iscan_oid_buf_list (oid_list);
}
