/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU-static plan comparison of CBRD-27146.
 * qo_plan_cmp () exists on both revisions; QO_COST_BUCKET_RATIO is defined only where the cost buckets are
 * (the head), so the test can tell which comparison it runs. */
QO_PLAN_COMPARE_RESULT
unitseam_plan_cmp (QO_PLAN * a, QO_PLAN * b)
{
  return qo_plan_cmp (a, b);
}

int
unitseam_has_cost_buckets (void)
{
#if defined (QO_COST_BUCKET_RATIO)
  return 1;
#else
  return 0;
#endif
}
