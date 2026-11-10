/*
 * unitseam appendix for src/transaction/log_tran_table.c (compiled at the end of that TU, SERVER_MODE) - for
 * unit/transaction/log_tran_table.session_sleepers.
 *
 * logtb_interrupt_session_sleeper_mapfunc () is static: the test reaches it through this wrapper, with the same
 * arguments thread_get_manager ()->map_entries () gives it in logtb_interrupt_session_sleepers ().
 */
void
unitseam_session_sleeper_mapfunc (THREAD_ENTRY & thread_ref, THREAD_ENTRY * caller_thread, SESSION_ID session_id)
{
  bool stop_mapper = false;

  logtb_interrupt_session_sleeper_mapfunc (thread_ref, stop_mapper, caller_thread, session_id);
}
