/* appended to src/optimizer/query_planner.c (CS_MODE): reach the TU-static plan comparison of CBRD-27146.
 * qo_plan_cmp () exists on both revisions; QO_COST_CELL_RATIO is defined only where the plans are ranked by one value
 * each and the planner marks the queries left to the rules (the head), so the test can tell which comparison it
 * runs. */
QO_PLAN_COMPARE_RESULT
unitseam_plan_cmp (QO_PLAN * a, QO_PLAN * b)
{
  return qo_plan_cmp (a, b);
}

int
unitseam_has_cost_order (void)
{
#if defined (QO_COST_CELL_RATIO)
  return 1;
#else
  return 0;
#endif
}

/* base: no such mark; its plans keep the rules anyway */
void
unitseam_set_cmp_by_rules (QO_PLANNER * planner, int on)
{
#if defined (QO_COST_CELL_RATIO)
  planner->plan_cmp_by_rules = (on != 0);
#else
  (void) planner;
  (void) on;
#endif
}

/* -1 where there is no qo_plan_cmp_by_rules () */
int
unitseam_plan_cmp_by_rules (QO_PLANNER * planner)
{
#if defined (QO_COST_CELL_RATIO)
  return planner != NULL ? (qo_plan_cmp_by_rules (planner) ? 1 : 0) : 0;
#else
  (void) planner;
  return -1;
#endif
}
