/* appended to query_graph.c: reach the TU-static row-snapshot pass of CBRD-27478 (PR#8029) from the test.
 * The base revision has none of these functions -- this appendix does not compile there (new code). */
int
unitseam_snap_threshold (void)
{
  return qo_snapshot_threshold ();
}

DB_VALUE *
unitseam_snap_const_value (QO_ENV * env, PT_NODE * node)
{
  return qo_snapshot_const_value (env, node);
}

int
unitseam_snap_sarg_matches (QO_ENV * env, QO_NODE * node, QO_TERM * sarg, DB_OBJECT * obj)
{
  return qo_snapshot_sarg_matches (env, node, sarg, obj);
}

void
unitseam_snap_apply (QO_ENV * env)
{
  qo_snapshot_apply (env);
}
