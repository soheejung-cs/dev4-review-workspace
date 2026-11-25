/* appended to src/optimizer/query_graph.c (CS_MODE): reach qo_expr_segs () and the TU-static
 * qo_discover_sort_limit_nodes () of PR#8664 (CBRD-28204).
 *
 * Every entry point sets env->catch_ the way qo_optimize_query () does, so a QO_ASSERT that fires
 * (qo_abort () longjmps there) comes back as *aborted = 1, with the tree as qo_abort () left it. */
void
unitseam_expr_segs (QO_ENV * env, PT_NODE * expr, BITSET * result, int *aborted)
{
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return;
    }
  qo_expr_segs (env, expr, result);
}

void
unitseam_sort_limit_nodes (QO_ENV * env, int *aborted)
{
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return;
    }
  qo_discover_sort_limit_nodes (env);
}
