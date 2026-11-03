/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU statics of PR#8325 (CBRD-27811).
 *
 * qo_has_aggregate_pre (), qo_having_aggregate_selectivity () and qo_having_selectivity () exist only where the PR's
 * QO_HAVING_HAS_AGGREGATE does (the PR head); elsewhere the hooks give sentinels and unitseam_has_having () 0, so the
 * cases on them fail with "not on this revision" (new code -- no base comparison). qo_estimate_ngroups () exists on
 * both revisions (the PR changed it).
 *
 * Every entry point sets env->catch_ the way qo_optimize_query () does, so a QO_ASSERT that fires (qo_abort () in
 * query_graph.c longjmps there) comes back as *aborted = 1 instead of jumping into a dead frame. */

#if defined (QO_HAVING_HAS_AGGREGATE)
int
unitseam_has_having (void)
{
  return 1;
}

int
unitseam_having_flag_aggregate (void)
{
  return QO_HAVING_HAS_AGGREGATE;
}

int
unitseam_having_flag_groupby_num (void)
{
  return QO_HAVING_HAS_GROUPBY_NUM;
}

/* the flags the PR's walk finds in one conjunct, walked the way qo_having_selectivity () walks it */
int
unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred)
{
  int found = 0;

  (void) parser_walk_tree (parser, pred, qo_has_aggregate_pre, &found, NULL, NULL);
  return found;
}

double
unitseam_having_aggregate_selectivity (QO_ENV * env, PT_NODE * pred, double group_size, int *aborted)
{
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return -9.0;
    }
  return qo_having_aggregate_selectivity (env, pred, group_size);
}

double
unitseam_having_selectivity (QO_ENV * env, PT_NODE * having, double group_size, int *aborted)
{
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return -9.0;
    }
  return qo_having_selectivity (env, having, group_size);
}
#else
int
unitseam_has_having (void)
{
  return 0;
}

int
unitseam_having_flag_aggregate (void)
{
  return 0;
}

int
unitseam_having_flag_groupby_num (void)
{
  return 0;
}

int
unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred)
{
  return -1;
}

double
unitseam_having_aggregate_selectivity (QO_ENV * env, PT_NODE * pred, double group_size, int *aborted)
{
  *aborted = 0;
  return -2.0;
}

double
unitseam_having_selectivity (QO_ENV * env, PT_NODE * having, double group_size, int *aborted)
{
  *aborted = 0;
  return -2.0;
}
#endif

void
unitseam_estimate_ngroups (QO_PLAN * plan, SORT_TYPE sort_type, int *aborted)
{
  *aborted = 0;
  if (setjmp (plan->info->env->catch_) != 0)
    {
      *aborted = 1;
      return;
    }
  qo_estimate_ngroups (plan, sort_type);
}
