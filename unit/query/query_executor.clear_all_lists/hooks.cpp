/* unitseam appendix to src/query/query_executor.c (same TU): calls the static qexec_clear_all_lists () on a block
 * that test.cpp builds by hand. qexec_intprt_fnc () is its only caller (once per row, for the block it runs). */
void
unitseam_clear_all_lists (THREAD_ENTRY * thread_p, XASL_NODE * xasl)
{
  qexec_clear_all_lists (thread_p, xasl);
}
