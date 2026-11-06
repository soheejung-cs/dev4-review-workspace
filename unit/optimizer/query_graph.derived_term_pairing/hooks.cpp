/* appended to query_graph.c: reach the TU-static compensation pass of CBRD-27306 (PR#8375 moved it and
 * widened its origin search).  Both the head and the base revision define qo_derived_term_compensate (). */
void
unitseam_derived_compensate (QO_ENV * env)
{
  qo_derived_term_compensate (env);
}
