/* unitseam appendix for src/query/expr_compile.c: the walkers eb_build_filter () asks through
 * expr_scan_pred_kid_has_side_effect () / expr_scan_pred_kid_runs_query () are TU-static */

bool
unitseam_regu_runs_query (const REGU_VARIABLE * regu)
{
  return expr_regu_runs (regu, EXPR_RUNS_QUERY);
}

bool
unitseam_regu_runs_side_effect (const REGU_VARIABLE * regu)
{
  return expr_regu_runs (regu, EXPR_RUNS_SIDE_EFFECT);
}

bool
unitseam_pred_runs_query (const PRED_EXPR * pr)
{
  return expr_pred_runs (pr, EXPR_RUNS_QUERY);
}

bool
unitseam_pred_runs_side_effect (const PRED_EXPR * pr)
{
  return expr_pred_runs (pr, EXPR_RUNS_SIDE_EFFECT);
}
