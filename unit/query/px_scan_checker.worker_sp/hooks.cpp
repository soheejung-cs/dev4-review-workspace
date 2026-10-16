/* appended to src/query/parallel/px_scan/px_scan_checker.cpp (same TU): reach the namespace-local
 * check / sibling_check templates and the thread_local caches from the test.
 *
 * base b394c89f3 (develop before PR#8136) has neither CANNOT_RUN_IN_WORKER nor the sibling_check
 * cache. The fallback namespace is nominated by a using-directive inside parallel_scan, so a qualified
 * name parallel_scan::X finds the TU's own X when the revision declares one and the fallback only when
 * it does not ([namespace.qual]/2). One appendix therefore compiles at base, at the PR head and at
 * develop. At base the fallback flag is 0: "no such notion" - a test that needs it fails there. */
namespace unitseam_px_fallback
{
  const parallel_scan::possible_flags CANNOT_RUN_IN_WORKER = 0;
  thread_local std::unordered_map<XASL_NODE *, parallel_scan::possible_flags> xasl_sibling_check_cache[2];
}

namespace parallel_scan
{
  using namespace unitseam_px_fallback;
}

/* the four possible_flags bits by name: 0 PARALLEL_SCAN, 1 LIST_MERGE, 2 BUILDVALUE_OPT, 3 RUN_IN_WORKER */
unsigned
unitseam_px_flag (int which)
{
  switch (which)
    {
    case 0:
      return parallel_scan::CANNOT_PARALLEL_SCAN;
    case 1:
      return parallel_scan::CANNOT_LIST_MERGE;
    case 2:
      return parallel_scan::CANNOT_BUILDVALUE_OPT;
    case 3:
      return parallel_scan::CANNOT_RUN_IN_WORKER;
    default:
      return 0;
    }
}

bool
unitseam_px_has_sibling_cache ()
{
  return &parallel_scan::xasl_sibling_check_cache[0] != &unitseam_px_fallback::xasl_sibling_check_cache[0];
}

/* what scan_check_parallel_scan_possible () does before and after one run */
static void
unitseam_px_clear_caches ()
{
  parallel_scan::xasl_check_cache.clear ();
  parallel_scan::xasl_processing_set.clear ();
  parallel_scan::xasl_sibling_check_cache[0].clear ();
  parallel_scan::xasl_sibling_check_cache[1].clear ();
}

/* check<is_outptr_list> (REGU_VARIABLE *): the regu-level rule. outptr = the regu sits in an output list */
unsigned
unitseam_px_check_regu (REGU_VARIABLE *regu, bool outptr)
{
  unitseam_px_clear_caches ();
  unsigned flags = outptr ? parallel_scan::check<true> (regu) : parallel_scan::check<false> (regu);
  unitseam_px_clear_caches ();
  return flags;
}

/* sibling_check<is_outptr_list> (XASL_NODE *): what a regu-linked subquery reports. The caches are left
 * filled so the test can read their size; the next call clears them first. */
unsigned
unitseam_px_sibling_check (XASL_NODE *xasl, bool outptr)
{
  unitseam_px_clear_caches ();
  return outptr ? parallel_scan::sibling_check<true> (xasl) : parallel_scan::sibling_check<false> (xasl);
}

size_t
unitseam_px_sibling_cache_size (int is_outptr_list)
{
  return parallel_scan::xasl_sibling_check_cache[is_outptr_list ? 1 : 0].size ();
}
