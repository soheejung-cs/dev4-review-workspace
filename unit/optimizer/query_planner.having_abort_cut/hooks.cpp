/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU-static qo_having_selectivity () for PR#8664
 * (CBRD-28204). The HAVING estimate is unchanged by the PR; the hook only runs it the way the planner does, with
 * env->catch_ set as qo_optimize_query () sets it, so a QO_ASSERT that fires below (qo_abort () in query_graph.c
 * longjmps there) comes back as *aborted = 1 with the HAVING list as qo_abort () left it. */
double
unitseam_having_selectivity (QO_ENV * env, PT_NODE * having, int *aborted)
{
  QO_HAVING_ENV henv;

  henv.env = env;
  henv.group_size = 0.0;
  henv.group_attr = NULL;
  henv.group_scale = 0.0;
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return -9.0;
    }
  return qo_having_selectivity (&henv, having);
}
