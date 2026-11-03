/*
 * unitseam appendix for src/storage/heap_file.c (compiled at the end of that TU) - for
 * unit/storage/heap_file.share_string.
 *
 * unitseam_can_share_string (value, domain, &absent): heap_attrinfo_can_share_string () (static, CBRD-27847,
 * PR#8378) where the revision has it. A revision before it has no such function: heap_attrinfo_set_converted ()
 * casts every string, so the template below answers false and reports the function absent.
 * Overload resolution picks: when the TU has the non-template
 * heap_attrinfo_can_share_string (const DB_VALUE *, const TP_DOMAIN *), it beats the template for these argument
 * types; otherwise the template is the only candidate.
 */
static bool unitseam_share_fn_absent = false;

template < typename V, typename D > static bool
heap_attrinfo_can_share_string (const V *, const D *)
{
  unitseam_share_fn_absent = true;
  return false;
}

bool
unitseam_can_share_string (const DB_VALUE * value, const TP_DOMAIN * domain, bool * absent)
{
  bool shares;

  unitseam_share_fn_absent = false;
  shares = heap_attrinfo_can_share_string (value, domain);
  if (absent != NULL)
    {
      *absent = unitseam_share_fn_absent;
    }
  return shares;
}
