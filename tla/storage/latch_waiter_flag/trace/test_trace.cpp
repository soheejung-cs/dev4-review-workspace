/* TLA+ trace run for tla/storage/latch_waiter_flag (CBRD-28163, PR #8587) -- skills/harness-review/tla.md §3-T.
 *
 * Three threads latch one BCB through the real page_buffer.c code built with instrument.patch and -DTLA_TRACE:
 *   H (index 0) READ-fixes first (the model's Init: the page READ-latched by H), then
 *   r1 (index 1) and r2 (index 2) fix with the kinds UT_K1 / UT_K2 (R or W), wait in the real pgbuf_block_bcb () /
 *   pgbuf_timed_sleep (), give up by page latch timeout (UT_TIMEOUT_MS) or are woken by the real wakeup code,
 *   and unfix what they got.  H unfixes after UT_DH_US microseconds (or, in UT_SCENARIO=tc, once r2 is queued or
 *   holds -- the TC cbrd_28163 shape: r1 gives up first, then r2 asks).
 * Each instrumented critical section writes one NDJSON line (tla_trace.h) to $TLA_TRACE_DIR; trace_to_tla.py merges
 * them and TraceCheck.tla / Trace.tla (Specula) replay them against LatchWaiterFlag.tla.
 *
 * Fakes (other translation units): cubthread::entry::lock () / unlock () (thread_entry.cpp: the th_entry_lock mutex),
 * thread_suspend_timeout_wakeup_and_unlock_entry () (thread_entry.cpp:567 -- the same pthread_cond_timedwait loop,
 * without the statistics of thread_prepare_suspension / resumption), logtb_is_current_active () = true (a running
 * transaction: the timeout is final), timeval_add_msec () / timeval_to_timespec () (porting.c).  Nothing in
 * page_buffer.c is faked.  Thread entries are calloc'd (zeroed mutex / cond = Linux defaults); each thread has its own
 * holder anchor; wait_msecs_override > 0 so pgbuf_timed_sleep () waits pgbuf_latch_timeout_msecs. */
#include "catch.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "error_code.h"
#include "page_buffer.h"
#include "thread_entry.hpp"

struct pgbuf_bcb;

extern pgbuf_bcb *ut_bcb_new (void);
extern void ut_set_latch_timeout (int msecs);
extern void ut_thread_init (cubthread::entry * th);
extern int ut_fix (cubthread::entry * th, pgbuf_bcb * bcb, int read);
extern int ut_unfix (cubthread::entry * th, pgbuf_bcb * bcb);
extern bool ut_queued (pgbuf_bcb * bcb, cubthread::entry * th);

/* ---- fakes ---- */
void
cubthread::entry::lock (void)
{
  pthread_mutex_lock (&th_entry_lock);
}

void
cubthread::entry::unlock (void)
{
  pthread_mutex_unlock (&th_entry_lock);
}

int
thread_suspend_timeout_wakeup_and_unlock_entry (cubthread::entry * thread_p, struct timespec *time_p,
						 thread_resume_suspend_status suspended_reason)
{
  int error = NO_ERROR;

  while (thread_p->resume_status == suspended_reason)
    {
      int r = pthread_cond_timedwait (&thread_p->wakeup_cond, &thread_p->th_entry_lock, time_p);
      if (r != 0 && r != ETIMEDOUT)
	{
	  return ER_CSS_PTHREAD_COND_TIMEDWAIT;
	}
      if (r == ETIMEDOUT)
	{
	  error = ER_CSS_PTHREAD_COND_TIMEDOUT;
	  break;
	}
    }
  thread_p->unlock ();
  return error;
}

bool
logtb_is_current_active (THREAD_ENTRY *)
{
  return true;
}

int
timeval_add_msec (struct timeval *added_time, const struct timeval *start_time, int msec)
{
  long usec = start_time->tv_usec + (long) msec * 1000;
  added_time->tv_sec = start_time->tv_sec + usec / 1000000;
  added_time->tv_usec = usec % 1000000;
  return NO_ERROR;
}

int
timeval_to_timespec (struct timespec *to, const struct timeval *from)
{
  to->tv_sec = from->tv_sec;
  to->tv_nsec = from->tv_usec * 1000;
  return NO_ERROR;
}

/* ---- the run ---- */
namespace
{
  int env_int (const char *k, int d)
  {
    const char *v = getenv (k);
    return v ? atoi (v) : d;
  }

  cubthread::entry *new_entry (int index)
  {
    cubthread::entry *th = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));
    th->index = index;
    th->tran_index = index + 1;
    th->wait_msecs_override = 60000;
    th->request_latch_mode = PGBUF_NO_LATCH;
    ut_thread_init (th);
    return th;
  }
}

TEST_CASE ("trace: H holds READ, r1 and r2 fix, wait, give up or are woken, unfix")
{
  const bool tc = getenv ("UT_SCENARIO") && strcmp (getenv ("UT_SCENARIO"), "tc") == 0;
  const int k1_read = env_int ("UT_K1_READ", 0), k2_read = env_int ("UT_K2_READ", 1);
  const int d1 = env_int ("UT_D1_US", 0), d2 = env_int ("UT_D2_US", 0), dh = env_int ("UT_DH_US", 5000);
  ut_set_latch_timeout (env_int ("UT_TIMEOUT_MS", 3));

  pgbuf_bcb *bcb = ut_bcb_new ();
  cubthread::entry *h = new_entry (0), *r1 = new_entry (1), *r2 = new_entry (2);
  std::atomic<int> go (0), r1_done (0), r2_done (0), r2_fixed (0);

  REQUIRE (ut_fix (h, bcb, 1) == NO_ERROR);	/* H: idle grant, READ */

  std::thread t1 ([&] ()
  {
    while (!go.load ()) { }
    usleep (d1);
    if (ut_fix (r1, bcb, k1_read) == NO_ERROR)
      {
	ut_unfix (r1, bcb);
      }
    r1_done = 1;
  });
  std::thread t2 ([&] ()
  {
    while (!go.load ()) { }
    if (tc)
      {
	while (!r1_done.load ()) { }	/* TC: session C asks after session B gave up */
      }
    usleep (d2);
    int rc = ut_fix (r2, bcb, k2_read);
    r2_fixed = (rc == NO_ERROR) ? 1 : 2;
    if (rc == NO_ERROR)
      {
	ut_unfix (r2, bcb);
      }
    r2_done = 1;
  });

  go = 1;
  if (tc)
    {
      while (!r2_fixed.load () && !ut_queued (bcb, r2)) { usleep (100); }
      usleep (dh);
    }
  else
    {
      usleep (dh);
    }
  ut_unfix (h, bcb);	/* H: the scan ends */
  t1.join ();
  t2.join ();

  /* Each entry lock is released as often as it is taken. A second unlock of a default mutex leaves its user count at
   * -1 and destroying it fails with EBUSY -- the shutdown assertion in cubthread::entry::clear_resources () that the
   * page latch timeout branch of pgbuf_timed_sleep () reached before it took the lock again (CI run 44049957988). */
  CHECK (pthread_mutex_destroy (&h->th_entry_lock) == 0);
  CHECK (pthread_mutex_destroy (&r1->th_entry_lock) == 0);
  CHECK (pthread_mutex_destroy (&r2->th_entry_lock) == 0);
  SUCCEED ("traced");
}
