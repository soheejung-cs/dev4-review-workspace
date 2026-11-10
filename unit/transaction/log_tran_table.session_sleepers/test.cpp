/*
 * Contract suite -- CBRD-27960 (PR#8427): the two places of src/transaction/log_tran_table.c (SERVER_MODE) that read
 * the conn_entry of another worker when a transaction is interrupted.
 *   logtb_interrupt_session_sleeper_mapfunc () (static, reached through hooks.cpp): for one worker, under its
 *       tran_index_lock, set the interrupt of its transaction and wake it when it serves the given session.
 *   logtb_set_tran_index_interrupt (), the SERVER_MODE block: the worker found by the transaction index is woken, and
 *       the session of its connection goes to logtb_interrupt_session_sleepers ().
 * PR#8427 (body "Implementation"): both read the other worker's conn_entry once and use that one value for the NULL
 * check and for session_id; the rule that picks the workers to wake does not change.
 *
 * Real code: the two functions. Seams (same TU, made weak): logtb_find_thread_by_tran_index_except_me (returns the
 * scripted worker; a hook can move that worker before the function returns, as a worker that ends its request right
 * after the lock-free read of its tran_index), logtb_interrupt_session_sleepers (records the session id;
 * DB_EMPTY_SESSION returns at once, as the real one, log_tran_table.c:2891).
 * Fakes (other TUs), each playing the real function named:
 *   cubthread::entry::is_on_current_thread (thread_entry.cpp:372, m_id == this thread's id): true for the scripted
 *       entry only; a hook runs inside it - in the PR it runs after the mapfunc read conn_entry, in develop before.
 *   thread_check_suspend_reason_and_wakeup (thread_entry.cpp:816 -> 781-800: lock the entry, signal it when it waits
 *       for that reason, unlock - it does not wait for the woken thread): records the call; a hook plays what the
 *       woken worker and the other threads do before the caller goes on.
 *   er_set, pgbuf_force_to_check_for_interrupts: no-ops.
 * log_Gl is the auto stub (4 KiB of zeros); the test fills its first member, trantable (log_impl.h:679), for
 * LOG_FIND_TDES (log_impl.h:1281). perfmon_inc_stat () is inline and stops at perfmon_is_perf_tracking () (pstat_Global
 * is a zero stub). Thread entries, connection entries and transaction descriptors are raw storage with the fields the
 * two functions read.
 *
 * Tags. [contract]: head 488dcb882 and base 953e83256 both PASS, except "M10 (PR#8427 TO-BE)", which base FAILs: base
 * reads conn_entry after is_on_current_thread () and sees the NULL the ending worker wrote, the PR checks and uses the
 * value it read before.
 * [defect] (PASS = the defect is there), all three about which session the woken worker's session id names:
 *   B4 (review thread r4489520248, the PR's order): the woken worker ends its request, its client has disconnected,
 *      the connection is freed and the next client takes it before the caller reads session_id -> that client's
 *      session goes to the mapper. head PASS, base FAIL (develop reads conn_entry again after the wake: NULL).
 *   B5 (develop's order): the woken worker takes a request of another session before the caller reads conn_entry
 *      again -> that session goes to the mapper. base PASS, head FAIL (the PR read the pointer before the wake).
 *   B6 (both): the worker found by the transaction index has moved on to another session's request before the
 *      caller reads its conn_entry. head PASS, base PASS.
 * Mutations on head (--patch): mut_proposal.diff (session_id read before the wake, the review thread's suggestion):
 *   B4 FAIL, B5 FAIL, B6 PASS. mut_validate.diff (the session id dropped when the connection's client_id is not the
 *   transaction's tdes->client_id): B4 B5 B6 FAIL. The contracts PASS under both.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype_def.h"
#include "error_manager.h"
#include "connection_defs.h"
#include "log_impl.h"
#include "thread_entry.hpp"
#include "page_buffer.h"
#include <cstring>
#include <functional>
#include <pthread.h>
#include <vector>

/* hooks.cpp (end of log_tran_table.c) */
extern void unitseam_session_sleeper_mapfunc (THREAD_ENTRY & thread_ref, THREAD_ENTRY * caller_thread,
    SESSION_ID session_id);

namespace
{
  struct wake_call
  {
    cubthread::entry *thread;
    thread_resume_suspend_status resume;
    thread_resume_suspend_status suspend;
  };

  struct world
  {
    std::vector<wake_call> wakes;
    std::vector<SESSION_ID> sleeper_sids;	/* sessions given to logtb_interrupt_session_sleepers () */
    cubthread::entry *current = NULL;	/* the entry is_on_current_thread () is true for */
    cubthread::entry *found = NULL;	/* what logtb_find_thread_by_tran_index_except_me () returns */
    std::function<void ()> on_find;	/* runs once inside the find, before it returns */
    std::function<void ()> on_wake;	/* runs once inside the wake */
    std::function<void ()> on_current;	/* runs once inside is_on_current_thread () */
  };
  world W;

  const int X = 1;		/* the transaction that is interrupted */
  const int X3 = 3;		/* a transaction of session S3 */
  const SESSION_ID S1 = 7;	/* the session of X */
  const SESSION_ID S2 = 9;	/* the session of the client that takes X's freed connection */
  const SESSION_ID S3 = 3;

  alignas (cubthread::entry) unsigned char thr_mem[2][sizeof (cubthread::entry)];
  alignas (css_conn_entry) unsigned char conn_mem[2][sizeof (css_conn_entry)];
  alignas (LOG_TDES) unsigned char tdes_mem[4][sizeof (LOG_TDES)];
  LOG_TDES *tdes_ptrs[4];
  LOG_ADDR_TDESAREA area_dummy;

  /* T: the thread that interrupts X (KILL QUERY's server thread, or a parallel job that failed) */
  cubthread::entry &T () { return *reinterpret_cast < cubthread::entry * >(thr_mem[0]); }
  /* WK: the worker that serves X */
  cubthread::entry &WK () { return *reinterpret_cast < cubthread::entry * >(thr_mem[1]); }
  /* C1: X's connection; C3: a connection of session S3 */
  css_conn_entry &C1 () { return *reinterpret_cast < css_conn_entry * >(conn_mem[0]); }
  css_conn_entry &C3 () { return *reinterpret_cast < css_conn_entry * >(conn_mem[1]); }
  LOG_TDES &TD (int i) { return *reinterpret_cast < LOG_TDES * >(tdes_mem[i]); }

  void
  reset ()
  {
    W = world ();
    std::memset (thr_mem, 0, sizeof (thr_mem));
    std::memset (conn_mem, 0, sizeof (conn_mem));
    std::memset (tdes_mem, 0, sizeof (tdes_mem));
    pthread_mutex_init (&T ().tran_index_lock, NULL);
    pthread_mutex_init (&WK ().tran_index_lock, NULL);

    T ().type = TT_DAEMON;
    T ().tran_index = 2;
    T ().m_status = cubthread::entry::status::TS_RUN;

    WK ().type = TT_WORKER;
    WK ().tran_index = X;
    WK ().m_status = cubthread::entry::status::TS_WAIT;	/* suspended in SLEEP () */
    WK ().conn_entry = &C1 ();

    C1 ().session_id = S1;
    C1 ().client_id = 1;
    C3 ().session_id = S3;
    C3 ().client_id = 3;

    for (int i = 0; i < 4; i++)
      {
	tdes_ptrs[i] = &TD (i);
	TD (i).trid = 100 + i;
	TD (i).client_id = (i == X) ? 1 : (i == X3) ? 3 : -1;
	TD (i).interrupt = false;
      }
    log_Gl.trantable.num_total_indices = 4;
    log_Gl.trantable.all_tdes = tdes_ptrs;
    log_Gl.trantable.area = &area_dummy;
    log_Gl.trantable.num_interrupts = 0;

    W.current = &T ();
    W.found = &WK ();
  }

  void
  run_once (std::function<void ()> &hook)
  {
    if (hook)
      {
	std::function<void ()> f = hook;
	hook = nullptr;
	f ();
      }
  }

  void
  map (cubthread::entry & thread_ref, SESSION_ID sid)
  {
    unitseam_session_sleeper_mapfunc (thread_ref, &T (), sid);
  }

  bool
  woke_only (cubthread::entry * thread_p)
  {
    return W.wakes.size () == 1 && W.wakes[0].thread == thread_p
	   && W.wakes[0].resume == THREAD_RESUME_DUE_TO_INTERRUPT && W.wakes[0].suspend == THREAD_SLEEP_FUNC_SUSPENDED;
  }

  /* what the woken worker does when its client is gone: it ends the request (tran_index back, conn_entry NULL), the
   * connection is freed (DB_EMPTY_SESSION) and the next client takes it with a new client_id and its own session */
  void
  worker_ends_and_connection_goes_to_next_client ()
  {
    WK ().tran_index = NULL_TRAN_INDEX;
    WK ().conn_entry = NULL;
    WK ().m_status = cubthread::entry::status::TS_FREE;
    C1 ().client_id = 2;
    C1 ().session_id = S2;
  }

  /* what the woken worker does when another request waits: it ends X's request and takes one of session S3 */
  void
  worker_takes_request_of_s3 ()
  {
    WK ().conn_entry = &C3 ();
    WK ().tran_index = X3;
    WK ().m_status = cubthread::entry::status::TS_RUN;
  }
}

/* ======================================================================================================== fakes */

bool
cubthread::entry::is_on_current_thread () const
{
  run_once (W.on_current);
  return this == W.current;
}

void
thread_check_suspend_reason_and_wakeup (cubthread::entry * thread_p, thread_resume_suspend_status resume_reason,
					thread_resume_suspend_status suspend_reason)
{
  W.wakes.push_back ({thread_p, resume_reason, suspend_reason});
  run_once (W.on_wake);
}

THREAD_ENTRY *
logtb_find_thread_by_tran_index_except_me (int tran_index)
{
  (void) tran_index;
  THREAD_ENTRY *found = W.found;
  run_once (W.on_find);
  return found;
}

void
logtb_interrupt_session_sleepers (THREAD_ENTRY * caller_thread, SESSION_ID session_id)
{
  (void) caller_thread;
  if (session_id == DB_EMPTY_SESSION)
    {
      return;			/* log_tran_table.c:2891 */
    }
  W.sleeper_sids.push_back (session_id);
}

void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) err_id;
  (void) num_args;
}

void
pgbuf_force_to_check_for_interrupts (void)
{
}

/* =================================================== logtb_interrupt_session_sleeper_mapfunc (), one worker */

TEST_CASE ("M1 a worker of the session: its transaction is interrupted and it is woken", "[contract]")
{
  reset ();
  map (WK (), S1);
  CHECK (woke_only (&WK ()));
  CHECK (TD (X).interrupt == (int) true);
  CHECK (log_Gl.trantable.num_interrupts == 1);
}

TEST_CASE ("M2 a worker of another session is left alone", "[contract]")
{
  reset ();
  map (WK (), S1 + 1);
  CHECK (W.wakes.empty ());
  CHECK (TD (X).interrupt == (int) false);
  CHECK (log_Gl.trantable.num_interrupts == 0);
}

TEST_CASE ("M3 a worker without a connection is left alone", "[contract]")
{
  reset ();
  WK ().conn_entry = NULL;
  map (WK (), S1);
  CHECK (W.wakes.empty ());
  CHECK (TD (X).interrupt == (int) false);
}

TEST_CASE ("M4 the caller itself is skipped", "[contract]")
{
  reset ();
  unitseam_session_sleeper_mapfunc (WK (), &WK (), S1);
  CHECK (W.wakes.empty ());
  CHECK (TD (X).interrupt == (int) false);
}

TEST_CASE ("M5 a thread that is not a worker is skipped (loaddb, vacuum, daemon)", "[contract]")
{
  thread_type types[] = { TT_LOADDB, TT_VACUUM_WORKER, TT_DAEMON };
  for (thread_type t : types)
    {
      reset ();
      WK ().type = t;
      map (WK (), S1);
      CHECK (W.wakes.empty ());
      CHECK (TD (X).interrupt == (int) false);
    }
}

TEST_CASE ("M6 a worker that is free, dead or being checked is skipped; running or waiting is not", "[contract]")
{
  cubthread::entry::status skip[] =
    { cubthread::entry::status::TS_FREE, cubthread::entry::status::TS_DEAD, cubthread::entry::status::TS_CHECK };
  for (cubthread::entry::status s : skip)
    {
      reset ();
      WK ().m_status = s;
      map (WK (), S1);
      CHECK (W.wakes.empty ());
    }
  cubthread::entry::status go[] = { cubthread::entry::status::TS_RUN, cubthread::entry::status::TS_WAIT };
  for (cubthread::entry::status s : go)
    {
      reset ();
      WK ().m_status = s;
      map (WK (), S1);
      CHECK (woke_only (&WK ()));
    }
}

TEST_CASE ("M7 the thread the mapper runs on is skipped", "[contract]")
{
  reset ();
  W.current = &WK ();
  map (WK (), S1);
  CHECK (W.wakes.empty ());
}

TEST_CASE ("M8 a transaction already interrupted is woken but not counted again", "[contract]")
{
  reset ();
  TD (X).interrupt = true;
  map (WK (), S1);
  CHECK (woke_only (&WK ()));
  CHECK (log_Gl.trantable.num_interrupts == 0);
}

TEST_CASE ("M9 a worker without a live transaction is woken, nothing is interrupted", "[contract]")
{
  reset ();
  WK ().tran_index = NULL_TRAN_INDEX;
  map (WK (), S1);
  CHECK (woke_only (&WK ()));
  CHECK (log_Gl.trantable.num_interrupts == 0);

  reset ();
  TD (X).trid = NULL_TRANID;
  map (WK (), S1);
  CHECK (woke_only (&WK ()));
  CHECK (TD (X).interrupt == (int) false);
  CHECK (log_Gl.trantable.num_interrupts == 0);
}

TEST_CASE ("M10 (PR#8427 TO-BE) the NULL check and the session compare use one read of conn_entry", "[contract]")
{
  /* the worker ends its request while the mapper looks at it: conn_entry turns NULL inside is_on_current_thread ().
   * The PR read conn_entry before that call and decides on that value; develop reads the field after it. */
  reset ();
  W.on_current = []
  {
    WK ().conn_entry = NULL;
  };
  map (WK (), S1);
  CHECK (woke_only (&WK ()));
  CHECK (TD (X).interrupt == (int) true);
}

/* ===================================== logtb_set_tran_index_interrupt (), SERVER_MODE block (set == true) */

TEST_CASE ("B1 no worker of the transaction: the flag is set, nothing is woken", "[contract]")
{
  reset ();
  W.found = NULL;
  CHECK (logtb_set_tran_index_interrupt (&T (), X, true));
  CHECK (TD (X).interrupt == (int) true);
  CHECK (log_Gl.trantable.num_interrupts == 1);
  CHECK (W.wakes.empty ());
  CHECK (W.sleeper_sids.empty ());
}

TEST_CASE ("B2 the worker is woken and its session's sleepers are interrupted", "[contract]")
{
  reset ();
  CHECK (logtb_set_tran_index_interrupt (&T (), X, true));
  CHECK (TD (X).interrupt == (int) true);
  CHECK (woke_only (&WK ()));
  CHECK (W.sleeper_sids == std::vector<SESSION_ID> { S1 });
}

TEST_CASE ("B3 a worker without a connection is woken, no session is looked at", "[contract]")
{
  reset ();
  WK ().conn_entry = NULL;
  CHECK (logtb_set_tran_index_interrupt (&T (), X, true));
  CHECK (woke_only (&WK ()));
  CHECK (W.sleeper_sids.empty ());
}

TEST_CASE ("B3b clearing the flag wakes nobody", "[contract]")
{
  reset ();
  TD (X).interrupt = true;
  log_Gl.trantable.num_interrupts = 1;
  CHECK (logtb_set_tran_index_interrupt (&T (), X, false));
  CHECK (TD (X).interrupt == (int) false);
  CHECK (log_Gl.trantable.num_interrupts == 0);
  CHECK (W.wakes.empty ());
  CHECK (W.sleeper_sids.empty ());
}

TEST_CASE ("B3c a transaction index without a transaction: false, nothing is woken", "[contract]")
{
  reset ();
  TD (X).trid = NULL_TRANID;
  CHECK_FALSE (logtb_set_tran_index_interrupt (&T (), X, true));
  CHECK (W.wakes.empty ());
  CHECK (W.sleeper_sids.empty ());
}

TEST_CASE ("B4 (review thread) the connection goes to the next client during the wake: its session is interrupted",
	   "[defect]")
{
  reset ();
  W.on_wake = worker_ends_and_connection_goes_to_next_client;
  logtb_set_tran_index_interrupt (&T (), X, true);
  CHECK (W.sleeper_sids == std::vector<SESSION_ID> { S2 });
}

TEST_CASE ("B5 (develop) the woken worker takes a request of another session: that session is interrupted",
	   "[defect]")
{
  reset ();
  W.on_wake = worker_takes_request_of_s3;
  logtb_set_tran_index_interrupt (&T (), X, true);
  CHECK (W.sleeper_sids == std::vector<SESSION_ID> { S3 });
}

TEST_CASE ("B6 (residual) the worker moved on to another session's request before it is read: that session",
	   "[defect]")
{
  reset ();
  W.on_find = worker_takes_request_of_s3;
  logtb_set_tran_index_interrupt (&T (), X, true);
  CHECK (W.sleeper_sids == std::vector<SESSION_ID> { S3 });
}
