/*
 * unitseam appendix for src/query/domain_rules.c (compiled at the end of that TU): its statics, for
 * unit/query/domain_resolve.compare_collation.
 */
int
unitseam_compute_comparison (const DOMAIN_COMPARE_KEY * lhs, const DOMAIN_COMPARE_KEY * rhs, DOMAIN_COMPARE * result)
{
  return domain_compute_comparison (lhs, rhs, result);
}

bool
unitseam_compare_equal (const DOMAIN_COMPARE * a, const DOMAIN_COMPARE * b)
{
  return domain_compare_equal (a, b);
}
