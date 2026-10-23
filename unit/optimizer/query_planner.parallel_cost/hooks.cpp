/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU-static cost helpers of PR#8212
 * (CBRD-27474). qo_mackert_lohman_pages () exists on both revisions; the parallel helpers exist only
 * where QO_PARALLEL_COST_DIVISOR is defined (the PR head), so base gets sentinels and the test skips. */
double
unitseam_ml_pages (double T, double N)
{
  return qo_mackert_lohman_pages (T, N);
}

#if defined (QO_PARALLEL_COST_DIVISOR)
int
unitseam_has_parallel_cost (void)
{
  return 1;
}

double
unitseam_divisor (QO_PLAN * p)
{
  return qo_parallel_cost_divisor (p);
}

int
unitseam_hjoin_runs_parallel (QO_PLAN * p)
{
  return qo_hjoin_runs_parallel (p, qo_parallel_cost_divisor (p)) ? 1 : 0;
}

int
unitseam_scan_runs_parallel (QO_PLAN * p)
{
  return qo_scan_runs_parallel (p) ? 1 : 0;
}

double
unitseam_input_factor (QO_PLAN * p, double d)
{
  return qo_join_input_factor (p, d);
}
#else
int
unitseam_has_parallel_cost (void)
{
  return 0;
}

double
unitseam_divisor (QO_PLAN * p)
{
  return -1.0;
}

int
unitseam_hjoin_runs_parallel (QO_PLAN * p)
{
  return -1;
}

int
unitseam_scan_runs_parallel (QO_PLAN * p)
{
  return -1;
}

double
unitseam_input_factor (QO_PLAN * p, double d)
{
  return -1.0;
}
#endif
