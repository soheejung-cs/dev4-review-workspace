/* appended to src/optimizer/query_planner.c (CS_MODE) for PR#8471 (CBRD-28003): reach the TU-static
 * qo_combine_partitions (), which joins the partition plans by cross joins, finalizes the topmost plan and gives it
 * the terms that read no node. The same on both revisions; only the loop over [E, T) differs. */
QO_PLAN *
unitseam_combine_partitions (QO_PLANNER * planner, BITSET * remaining_subqueries)
{
  return qo_combine_partitions (planner, remaining_subqueries);
}
