/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU statics of PR#8325 (CBRD-27811).
 *
 * The HAVING estimate as 5cfef7f5b has it: qo_has_aggregate_pre (), qo_having_value (), qo_having_row_compare (),
 * qo_having_compare (), qo_having_equal (), qo_having_between (), qo_having_range (), qo_having_in (),
 * qo_having_aggregate_term (), qo_having_disjunct_selectivity (), qo_having_pred_selectivity (),
 * qo_having_selectivity (), qo_having_env_init () over a QO_HAVING_ENV. They exist where the PR's
 * QO_HAVING_DEFAULT_SELECTIVITY does (5cfef7f5b on); elsewhere (develop, and the earlier head 766999f0e whose
 * qo_having_aggregate_selectivity () took a bare group size) the hooks give sentinels and unitseam_has_having () 0,
 * so the cases on them fail with "not on this revision" (new code -- no base comparison). qo_estimate_ngroups ()
 * exists on every revision (the PR changed it).
 *
 * Every entry point sets env->catch_ the way qo_optimize_query () does, so a QO_ASSERT that fires (qo_abort () in
 * query_graph.c longjmps there) comes back as *aborted = 1 instead of jumping into a dead frame. */

#if defined (QO_HAVING_DEFAULT_SELECTIVITY)
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

double
unitseam_having_default (void)
{
  return QO_HAVING_DEFAULT_SELECTIVITY;
}

/* the flags the PR's walk finds in one predicate, walked the way qo_having_disjunct_selectivity () walks it */
int
unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred)
{
  int found = 0;

  (void) parser_walk_tree (parser, pred, qo_has_aggregate_pre, &found, NULL, NULL);
  return found;
}

static void
unitseam_henv (QO_HAVING_ENV * henv, QO_ENV * env, double group_size, PT_NODE * group_attr, double group_scale)
{
  henv->env = env;
  henv->group_size = group_size;
  henv->group_attr = group_attr;
  henv->group_scale = group_scale;
}

/* which estimator: 0 qo_having_aggregate_term () (raw, -1 = not a priced form), 1 qo_having_disjunct_selectivity (),
 * 2 qo_having_pred_selectivity () (the or_next list), 3 qo_having_selectivity () (the next list) */
double
unitseam_having_estimate (int which, QO_ENV * env, PT_NODE * pred, double group_size, PT_NODE * group_attr,
			  double group_scale, int *aborted)
{
  QO_HAVING_ENV henv;

  unitseam_henv (&henv, env, group_size, group_attr, group_scale);
  *aborted = 0;
  if (setjmp (env->catch_) != 0)
    {
      *aborted = 1;
      return -9.0;
    }
  switch (which)
    {
    case 0:
      return qo_having_aggregate_term (&henv, pred);
    case 1:
      return qo_having_disjunct_selectivity (&henv, pred);
    case 2:
      return qo_having_pred_selectivity (&henv, pred);
    default:
      return qo_having_selectivity (&henv, pred);
    }
}

/* qo_having_row_compare (): "x op value" over the rows of the annotated name attr */
double
unitseam_having_row_compare (PT_NODE * attr, DB_VALUE * value, bool is_ge, bool include_equal)
{
  return qo_having_row_compare (attr, value, is_ge, include_equal);
}

/* qo_having_env_init () for the plan's statement: what it found for the GROUP BY */
void
unitseam_having_env_init (QO_PLAN * plan, double group_size, PT_NODE ** group_attr, double *group_scale,
			  double *out_group_size, int *aborted)
{
  QO_HAVING_ENV henv;

  *aborted = 0;
  if (setjmp (plan->info->env->catch_) != 0)
    {
      *aborted = 1;
      return;
    }
  qo_having_env_init (&henv, plan, group_size);
  *group_attr = henv.group_attr;
  *group_scale = henv.group_scale;
  *out_group_size = henv.group_size;
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

double
unitseam_having_default (void)
{
  return -2.0;
}

int
unitseam_having_found (PARSER_CONTEXT * parser, PT_NODE * pred)
{
  return -1;
}

double
unitseam_having_estimate (int which, QO_ENV * env, PT_NODE * pred, double group_size, PT_NODE * group_attr,
			  double group_scale, int *aborted)
{
  *aborted = 0;
  return -2.0;
}

double
unitseam_having_row_compare (PT_NODE * attr, DB_VALUE * value, bool is_ge, bool include_equal)
{
  return -2.0;
}

void
unitseam_having_env_init (QO_PLAN * plan, double group_size, PT_NODE ** group_attr, double *group_scale,
			  double *out_group_size, int *aborted)
{
  *aborted = 0;
  *group_attr = NULL;
  *group_scale = -2.0;
  *out_group_size = -2.0;
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
