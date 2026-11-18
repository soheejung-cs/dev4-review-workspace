/* appended to src/object/object_domain.c: reach the static helper of CBRD-28066 */
bool
unit_tp_domain_coll_flags_match (const TP_DOMAIN * dom1, const TP_DOMAIN * dom2)
{
  return tp_domain_coll_flags_match (dom1, dom2);
}
