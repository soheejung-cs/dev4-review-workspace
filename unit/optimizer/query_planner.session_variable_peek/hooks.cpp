/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU statics of PR#8344 (CBRD-27144).
 * qo_session_variable_value () exists only where the parser has PT_EXPR_INFO_PEEK_SESSION_VARIABLE (the PR head), so
 * base gets sentinels and the test reports those cases as not applicable. qo_between_range_arg_value () exists on
 * both revisions (the PR added its PC_OTHER branch). */
#if defined (PT_EXPR_INFO_PEEK_SESSION_VARIABLE)
int
unitseam_has_session_peek (void)
{
  return 1;
}

DB_VALUE *
unitseam_session_variable_value (QO_ENV * env, PT_NODE * node)
{
  return qo_session_variable_value (env, node);
}

/* the entries the block keeps (env->session_var_peeks), -1 where there is no such list */
int
unitseam_session_peek_count (QO_ENV * env)
{
  int n = 0;
  for (QO_SESSION_VAR_PEEK * p = env->session_var_peeks; p != NULL; p = p->peek_next)
    {
      n++;
    }
  return n;
}
#else
int
unitseam_has_session_peek (void)
{
  return 0;
}

DB_VALUE *
unitseam_session_variable_value (QO_ENV * env, PT_NODE * node)
{
  return NULL;
}

int
unitseam_session_peek_count (QO_ENV * env)
{
  return -1;
}
#endif

DB_VALUE *
unitseam_between_range_arg_value (QO_ENV * env, PT_NODE * arg)
{
  return qo_between_range_arg_value (env, arg);
}

/* the equality estimate the read reaches (qo_equal_selectivity, static): `attr = @v` where @v is PC_OTHER */
double
unitseam_equal_selectivity (QO_ENV * env, PT_NODE * expr)
{
  return qo_equal_selectivity (env, expr);
}
