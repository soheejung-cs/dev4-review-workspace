/* CBRD-28163 -- pgbuf_timed_sleep_error_handling () (src/storage/page_buffer.c, SERVER_MODE): a page latch waiter that
 * gave up (latch timeout, or interrupt) takes itself off the BCB wait queue; when it was at the front it hands the READ
 * latch to the READ waiters behind it (fcnt += their request_fix_count, pgbuf_wakeup ()).
 *
 * Contract (rule #028 form): when the function returns, the atomic latch's waiter_exists == (a waiter whose
 * request_latch_mode is PGBUF_LATCH_READ or PGBUF_LATCH_WRITE is left on the queue).  While the flag is set,
 * pgbuf_latch_bcb_upon_fix () blocks the READ request of a non-holder on a READ-latched page (cae32d077 :7046-7058) and
 * pgbuf_lockfree_fix_ro () stays off; the flag is cleared again only when the latch drains (pgbuf_unlatch_bcb_upon_unfix ()
 * -> pgbuf_wakeup_reader_writer (), :7521 / :8265).  A FLUSH waiter does not set it (:7719, :9635).
 *   head cae32d077 (branch CBRD-28163): the three exits meet at `end:`, which clears the flag when no blocked reader /
 *        writer is left -- the check pgbuf_wakeup_reader_writer () ends with (:8265-8268).
 *   base bb71348d6 (develop): takes the waiter off the queue and leaves the flag as it was.
 *
 * Tags (unit/README.md, tools/harness/unit_regress.py):
 *   [contract] passes on head and base: the queue, the latch mode and fix count, who is woken, the BCB mutex on return,
 *              and the flag where a blocked reader / writer is left (d, e, f2, h).
 *   [defect]   asserts base's behaviour, PASS = defect reproduced: the flag left set with no blocked reader / writer on the
 *              queue (a, b, c, e2, f, g) -- PASS on base, FAIL on head (the expansion shows head's flag).
 *              R1 is a different, develop-same defect of the grant loop, outside CBRD-28163's change: PASS on both.
 *
 * Real code: pgbuf_timed_sleep_error_handling () and what it calls in the TU -- the atomic latch helpers, PGBUF_BCB_LOCK
 * (pthread_mutex_lock: pgbuf_Monitor_locks is false), pgbuf_is_exist_blocked_reader_writer () (head) and pgbuf_wakeup ()
 * (STATIC_INLINE -- unitseam's --seam turns `static` into weak and cannot take `STATIC_INLINE`, so it runs as it is:
 * resume_status = THREAD_PGBUF_RESUMED, pthread_cond_signal () of the zeroed wakeup_cond, the entry unlock).  Who was
 * woken is read from resume_status and from the entry lock calls.
 * Fakes: cubthread::entry::lock () / unlock () (thread_entry.cpp:397-407: pthread_mutex_lock / unlock of th_entry_lock) --
 * the same mutex calls plus a count per entry.  In R1 one entry's lock () also plays that waiter's own thread giving up
 * just before the lock: pgbuf_timed_sleep () sets its request_latch_mode to PGBUF_NO_LATCH (:8005 interrupt, :8035
 * timeout).
 * Inputs follow the caller, pgbuf_timed_sleep (): the thread that gives up has set its own request_latch_mode to
 * PGBUF_NO_LATCH before the call (:8005, :8035); the call is made without the BCB mutex and returns holding it (the caller
 * unlocks it, :8010 / er_set_return) -- give_up () checks both and unlocks.  A blocked thread carries the request
 * pgbuf_block_bcb () gave it (:7733-7734) and resume_status THREAD_PGBUF_SUSPENDED (:7991).  The BCB and the thread
 * entries are calloc'd (a zeroed pthread mutex / cond is the default one on Linux), never constructed.  The BCB is READ
 * latched by one other thread (fcnt 1) with waiter_exists set, as the writer or reader that queued left it.
 */
#include "catch.hpp"

#include <cstdlib>
#include <map>
#include <pthread.h>
#include <vector>

#include "error_code.h"
#include "page_buffer.h"
#include "thread_entry.hpp"

struct pgbuf_bcb;		/* PGBUF_BCB is page_buffer.c's */

/* ---- hooks.cpp (appended to page_buffer.c) ---- */
extern pgbuf_bcb *unit_bcb_new (void);
extern void unit_bcb_free (pgbuf_bcb * bcb);
extern void unit_bcb_set_latch (pgbuf_bcb * bcb, int latch_mode, int fcnt, bool waiter_exists);
extern int unit_bcb_latch_mode (pgbuf_bcb * bcb);
extern int unit_bcb_fcnt (pgbuf_bcb * bcb);
extern bool unit_bcb_waiter_exists (pgbuf_bcb * bcb);
extern void unit_bcb_set_queue (pgbuf_bcb * bcb, cubthread::entry ** entries, int n);
extern int unit_bcb_queue (pgbuf_bcb * bcb, cubthread::entry ** out, int max);
extern int unit_tseh (cubthread::entry * thrd_entry, pgbuf_bcb * bcb);
extern bool unit_bcb_mutex_held (pgbuf_bcb * bcb);
extern void unit_bcb_unlock (pgbuf_bcb * bcb);

/* ---- fakes ---- */
namespace ut
{
  struct entry_locks
  {
    int lock = 0;
    int unlock = 0;
  };
  std::map<const cubthread::entry *, entry_locks> locks;
  cubthread::entry *gives_up_on_lock = NULL;	/* R1 */

  void reset ()
  {
    locks.clear ();
    gives_up_on_lock = NULL;
  }

  /* read without inserting */
  entry_locks counts (const cubthread::entry * t)
  {
    std::map<const cubthread::entry *, entry_locks>::const_iterator it = locks.find (t);
    return it == locks.end () ? entry_locks () : it->second;
  }

  /* entry locks taken in all */
  int lock_calls ()
  {
    int n = 0;
    for (const auto &kv : locks)
      {
	n += kv.second.lock;
      }
    return n;
  }
}

/* thread_entry.cpp:397-407 */
void
cubthread::entry::lock (void)
{
  pthread_mutex_lock (&th_entry_lock);
  ut::locks[this].lock++;
  if (this == ut::gives_up_on_lock)
    {
      /* R1: this waiter's own thread gave up after the grant loop read its request_latch_mode without the entry lock
       * and before the loop got the lock -- pgbuf_timed_sleep () (page_buffer.c:8005, :8035) */
      request_latch_mode = PGBUF_NO_LATCH;
      ut::gives_up_on_lock = NULL;
    }
}

void
cubthread::entry::unlock (void)
{
  ut::locks[this].unlock++;
  pthread_mutex_unlock (&th_entry_lock);
}

/* ---- the BCB and its queue ---- */
namespace
{
  const int MAXQ = 8;

  struct world
  {
    pgbuf_bcb *bcb;
    std::vector<cubthread::entry *> entries;

    world (int latch_mode, int fcnt, bool waiter_exists)
    {
      ut::reset ();
      bcb = unit_bcb_new ();
      unit_bcb_set_latch (bcb, latch_mode, fcnt, waiter_exists);
    }

    ~world ()
    {
      for (cubthread::entry * t : entries)
	{
	  free (t);
	}
      unit_bcb_free (bcb);
      ut::reset ();
    }

    /* a thread blocked in pgbuf_timed_sleep () */
    cubthread::entry *waiter (int index, int request_latch_mode, int request_fix_count)
    {
      cubthread::entry *t = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));	/* never constructed */

      t->index = index;
      t->request_latch_mode = request_latch_mode;
      t->request_fix_count = request_fix_count;
      t->resume_status = THREAD_PGBUF_SUSPENDED;
      entries.push_back (t);
      return t;
    }

    /* the thread that gives up: it asked for 'requested', and pgbuf_timed_sleep () has set PGBUF_NO_LATCH */
    cubthread::entry *giving_up (int index, int requested)
    {
      cubthread::entry *t = waiter (index, requested, 1);

      t->request_latch_mode = PGBUF_NO_LATCH;
      return t;
    }

    void queue (std::vector<cubthread::entry *> q)
    {
      unit_bcb_set_queue (bcb, q.data (), (int) q.size ());
    }

    std::vector<cubthread::entry *> queue_now ()
    {
      cubthread::entry *out[MAXQ];
      int n = unit_bcb_queue (bcb, out, MAXQ);

      REQUIRE (n <= MAXQ);	/* no cycle */
      return std::vector<cubthread::entry *> (out, out + n);
    }

    /* the contract's right side, read from the queue here -- not through the TU's pgbuf_is_exist_blocked_reader_writer () */
    bool blocked_reader_writer_left ()
    {
      for (cubthread::entry * t : queue_now ())
	{
	  if (t->request_latch_mode == PGBUF_LATCH_READ || t->request_latch_mode == PGBUF_LATCH_WRITE)
	    {
	      return true;
	    }
	}
      return false;
    }

    /* the call as pgbuf_timed_sleep () makes it (page_buffer.c:8008-8011, :8038) */
    void give_up (cubthread::entry * x)
    {
      ut::entry_locks before = ut::counts (x);

      REQUIRE_FALSE (unit_bcb_mutex_held (bcb));
      int rc = unit_tseh (x, bcb);
      CHECK (rc == NO_ERROR);
      REQUIRE (unit_bcb_mutex_held (bcb));
      unit_bcb_unlock (bcb);
      CHECK (x->next_wait_thrd == NULL);
      /* the giving-up thread's own entry is not locked by its own call */
      CHECK (ut::counts (x).lock == before.lock);
      CHECK (ut::counts (x).unlock == before.unlock);
    }
  };

  bool woken (const cubthread::entry * t)
  {
    return t->resume_status == THREAD_PGBUF_RESUMED;
  }

  /* the entry lock was taken and given back as often, and is free now */
  bool entry_lock_balanced (cubthread::entry * t, int times)
  {
    if (ut::counts (t).lock != times || ut::counts (t).unlock != times)
      {
	return false;
      }
    if (pthread_mutex_trylock (&t->th_entry_lock) != 0)
      {
	return false;
      }
    pthread_mutex_unlock (&t->th_entry_lock);
    return true;
  }
}

/* ---- a: [X(WRITE)] -- case 3, nobody behind ---- */
TEST_CASE ("CBRD-28163 a: [X(WRITE)] on a READ latch -- X leaves, the queue is empty, the READ latch (fcnt 1) stays",
	   "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  w.queue ({x});

  w.give_up (x);

  CHECK (w.queue_now ().empty ());
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK (ut::lock_calls () == 0);	/* nobody granted */
}

TEST_CASE ("CBRD-28163 a (base defect, PASS = reproduced): [X(WRITE)] -- waiter_exists stays set with no blocked "
	   "reader or writer left", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  w.queue ({x});

  w.give_up (x);

  REQUIRE_FALSE (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- b: [X(READ)] -- a reader queued behind the flag, gives up ---- */
TEST_CASE ("CBRD-28163 b: [X(READ)] on a READ latch -- X leaves, the queue is empty, the READ latch (fcnt 1) stays",
	   "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_READ);
  w.queue ({x});

  w.give_up (x);

  CHECK (w.queue_now ().empty ());
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK (ut::lock_calls () == 0);
}

TEST_CASE ("CBRD-28163 b (base defect, PASS = reproduced): [X(READ)] -- waiter_exists stays set with no blocked "
	   "reader or writer left", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_READ);
  w.queue ({x});

  w.give_up (x);

  REQUIRE_FALSE (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- c: [X(WRITE), Y(READ)] -- case 3 hands the READ latch to Y ---- */
TEST_CASE ("CBRD-28163 c: [X(WRITE), Y(READ, 2 fixes)] -- Y gets the READ latch (fcnt 1 -> 3), is woken and leaves "
	   "the queue, which is empty", "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_READ, 2);
  w.queue ({x, y});

  w.give_up (x);

  CHECK (w.queue_now ().empty ());
  CHECK (y->next_wait_thrd == NULL);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1 + 2);
  CHECK (woken (y));
  CHECK (entry_lock_balanced (y, 1));	/* thread_lock_entry () in the loop, thread_unlock_entry () in pgbuf_wakeup () */
}

TEST_CASE ("CBRD-28163 c (base defect, PASS = reproduced): [X(WRITE), Y(READ)] -- after Y is granted, waiter_exists "
	   "stays set with no blocked reader or writer left", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_READ, 2);
  w.queue ({x, y});

  w.give_up (x);

  REQUIRE (woken (y));
  REQUIRE_FALSE (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- d: [X(WRITE), Y(WRITE)] -- a writer stays ---- */
TEST_CASE ("CBRD-28163 d: [X(WRITE), Y(WRITE)] -- Y stays blocked at the front, waiter_exists stays set", "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_WRITE, 1);
  w.queue ({x, y});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  CHECK (q[0] == y);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK_FALSE (woken (y));
  CHECK (ut::lock_calls () == 0);
  CHECK (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));	/* a new READ must not overtake the writer */
}

/* ---- e: [Y(WRITE), X(READ)] -- case 2, X not at the front ---- */
TEST_CASE ("CBRD-28163 e: [Y(WRITE), X(READ)] (X not at the front) -- X is unlinked, [Y] stays, waiter_exists stays set",
	   "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_WRITE, 1);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_READ);
  w.queue ({y, x});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  CHECK (q[0] == y);
  CHECK (y->next_wait_thrd == NULL);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK_FALSE (woken (y));
  CHECK (ut::lock_calls () == 0);
  CHECK (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- e2: [F(FLUSH), X(WRITE)] -- case 2 with only a flush waiter left (added: the case-2 exit with nothing blocked) */
TEST_CASE ("CBRD-28163 e2: [F(FLUSH), X(WRITE)] (X not at the front) -- X is unlinked, [F] stays, not woken",
	   "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *f = w.waiter (3, PGBUF_LATCH_FLUSH, 0);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  w.queue ({f, x});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  CHECK (q[0] == f);
  CHECK (f->next_wait_thrd == NULL);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK_FALSE (woken (f));
  CHECK (ut::lock_calls () == 0);
}

TEST_CASE ("CBRD-28163 e2 (base defect, PASS = reproduced): [F(FLUSH), X(WRITE)] -- the case-2 exit leaves "
	   "waiter_exists set with only a flush waiter left", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *f = w.waiter (3, PGBUF_LATCH_FLUSH, 0);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  w.queue ({f, x});

  w.give_up (x);

  REQUIRE (w.queue_now ().size () == 1);
  REQUIRE_FALSE (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- f: [X(WRITE), F(FLUSH)] -- case 3 stops at a flush waiter ---- */
TEST_CASE ("CBRD-28163 f: [X(WRITE), F(FLUSH)] -- the grant loop stops at F, [F] stays, not woken, fcnt 1",
	   "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *f = w.waiter (3, PGBUF_LATCH_FLUSH, 0);
  w.queue ({x, f});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  CHECK (q[0] == f);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK_FALSE (woken (f));
  CHECK (ut::lock_calls () == 0);
}

TEST_CASE ("CBRD-28163 f (base defect, PASS = reproduced): [X(WRITE), F(FLUSH)] -- waiter_exists stays set with only "
	   "a flush waiter left (flush waiters never set it, page_buffer.c:7719 / :9635)", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *f = w.waiter (3, PGBUF_LATCH_FLUSH, 0);
  w.queue ({x, f});

  w.give_up (x);

  REQUIRE (w.queue_now ().size () == 1);
  REQUIRE_FALSE (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- f2: [X(WRITE), F(FLUSH), Y(READ)] -- a reader blocked behind the flush waiter (added) ---- */
TEST_CASE ("CBRD-28163 f2: [X(WRITE), F(FLUSH), Y(READ)] -- the loop stops at F, Y stays blocked behind it, "
	   "waiter_exists stays set", "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *f = w.waiter (3, PGBUF_LATCH_FLUSH, 0);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_READ, 2);
  w.queue ({x, f, y});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 2);
  CHECK (q[0] == f);
  CHECK (q[1] == y);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK_FALSE (woken (f));
  CHECK_FALSE (woken (y));
  CHECK (ut::lock_calls () == 0);
  CHECK (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));	/* the check walks past the flush waiter */
}

/* ---- g: [] -- case 1, another thread took X off already ---- */
TEST_CASE ("CBRD-28163 g: [] (X already off the queue, case 1) -- NO_ERROR with the BCB mutex held, nothing else "
	   "changes", "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_READ);
  w.queue ({});

  w.give_up (x);

  CHECK (w.queue_now ().empty ());
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1);
  CHECK (ut::lock_calls () == 0);
}

TEST_CASE ("CBRD-28163 g (base defect, PASS = reproduced): [] (case 1) -- the early exit leaves waiter_exists set "
	   "on an empty queue", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_READ);
  w.queue ({});

  w.give_up (x);

  REQUIRE (w.queue_now ().empty ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- h: [X(WRITE), Y1(READ), Y2(READ), W(WRITE)] -- two readers granted, the writer stays (added) ---- */
TEST_CASE ("CBRD-28163 h: [X(WRITE), Y1(READ, 2), Y2(READ, 3), W(WRITE)] -- Y1 and Y2 get the READ latch (fcnt 1 -> 6) "
	   "and are woken, W stays blocked, waiter_exists stays set", "[contract]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *y1 = w.waiter (2, PGBUF_LATCH_READ, 2);
  cubthread::entry *y2 = w.waiter (3, PGBUF_LATCH_READ, 3);
  cubthread::entry *wr = w.waiter (4, PGBUF_LATCH_WRITE, 1);
  w.queue ({x, y1, y2, wr});

  w.give_up (x);

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  CHECK (q[0] == wr);
  CHECK (unit_bcb_latch_mode (w.bcb) == PGBUF_LATCH_READ);
  CHECK (unit_bcb_fcnt (w.bcb) == 1 + 2 + 3);
  CHECK (woken (y1));
  CHECK (woken (y2));
  CHECK_FALSE (woken (wr));
  CHECK (entry_lock_balanced (y1, 1));
  CHECK (entry_lock_balanced (y2, 1));
  CHECK (ut::counts (wr).lock == 0);
  CHECK (w.blocked_reader_writer_left ());
  CHECK (unit_bcb_waiter_exists (w.bcb));
}

/* ---- R1: the grant loop's fix count, when the reader gives up in between (develop same) ---- */
TEST_CASE ("R1 (develop same, outside CBRD-28163's change; PASS = reproduced): [X(WRITE), Y(READ, 2)] and Y gives up "
	   "between the grant loop's unlocked read of its request and its entry lock -- the loop has added Y's 2 fixes, "
	   "and they stay on the BCB after Y's own call though Y holds nothing", "[defect]")
{
  world w (PGBUF_LATCH_READ, 1, true);
  cubthread::entry *x = w.giving_up (1, PGBUF_LATCH_WRITE);
  cubthread::entry *y = w.waiter (2, PGBUF_LATCH_READ, 2);
  w.queue ({x, y});
  ut::gives_up_on_lock = y;

  w.give_up (x);		/* X's pgbuf_timed_sleep (): the loop sees Y READ, adds 2, locks Y, finds NO_LATCH, stops */

  std::vector<cubthread::entry *> q = w.queue_now ();
  REQUIRE (q.size () == 1);
  REQUIRE (q[0] == y);		/* left for Y's own call */
  REQUIRE_FALSE (woken (y));
  REQUIRE (entry_lock_balanced (y, 1));

  w.give_up (y);		/* Y's pgbuf_timed_sleep (), once X's caller let the BCB mutex go; it then returns ER_FAILED */

  REQUIRE (w.queue_now ().empty ());
  REQUIRE_FALSE (woken (y));
  CHECK (unit_bcb_fcnt (w.bcb) == 1 + 2);	/* the one real holder's fix plus Y's 2 that nobody will unfix */
}
