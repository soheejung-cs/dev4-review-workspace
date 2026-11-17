/* ---- unitseam appendix (CBRD-28163 suite): one BCB and its wait queue, from inside page_buffer.c ----
 *
 * PGBUF_BCB, the atomic latch helpers (get_latch / get_fcnt / get_waiter_exists) and pgbuf_timed_sleep_error_handling ()
 * are this TU's statics.  The test builds one calloc'd BCB and a wait queue of calloc'd thread entries, calls the real
 * function through unit_tseh (), and reads the BCB back.  Nothing here copies engine logic: the accessors only set and
 * read fields.  A zero-filled pthread mutex is the default (PTHREAD_MUTEX_INITIALIZER) mutex on Linux / glibc, so the
 * BCB mutex works without pthread_mutex_init ().
 */
#include <errno.h>

PGBUF_BCB *
unit_bcb_new (void)
{
  return (PGBUF_BCB *) calloc (1, sizeof (PGBUF_BCB));
}

void
unit_bcb_free (PGBUF_BCB * bcb)
{
  free (bcb);
}

void
unit_bcb_set_latch (PGBUF_BCB * bcb, int latch_mode, int fcnt, bool waiter_exists)
{
  PGBUF_ATOMIC_LATCH_IMPL impl;

  impl.raw = 0;
  impl.impl.latch_mode = (PGBUF_LATCH_MODE) latch_mode;
  impl.impl.waiter_exists = waiter_exists ? 1 : 0;
  impl.impl.fcnt = fcnt;
  bcb->atomic_latch.store (impl.raw);
}

int
unit_bcb_latch_mode (PGBUF_BCB * bcb)
{
  return (int) get_latch (&bcb->atomic_latch);
}

int
unit_bcb_fcnt (PGBUF_BCB * bcb)
{
  return get_fcnt (&bcb->atomic_latch);
}

bool
unit_bcb_waiter_exists (PGBUF_BCB * bcb)
{
  return get_waiter_exists (&bcb->atomic_latch);
}

/* bcb->next_wait_thrd -> entries[0] -> ... -> entries[n - 1] -> NULL */
void
unit_bcb_set_queue (PGBUF_BCB * bcb, THREAD_ENTRY ** entries, int n)
{
  int i;

  bcb->next_wait_thrd = n > 0 ? entries[0] : NULL;
  for (i = 0; i < n; i++)
    {
      entries[i]->next_wait_thrd = (i + 1 < n) ? entries[i + 1] : NULL;
    }
}

/* the queue as it is now; more than max entries (a cycle) returns max + 1 */
int
unit_bcb_queue (PGBUF_BCB * bcb, THREAD_ENTRY ** out, int max)
{
  THREAD_ENTRY *t;
  int n = 0;

  for (t = bcb->next_wait_thrd; t != NULL; t = t->next_wait_thrd)
    {
      if (n == max)
	{
	  return max + 1;
	}
      out[n++] = t;
    }
  return n;
}

int
unit_tseh (THREAD_ENTRY * thrd_entry, PGBUF_BCB * bcb)
{
  return pgbuf_timed_sleep_error_handling (thrd_entry, bcb);
}

/* the BCB mutex is held (by anyone): trylock of a held default mutex returns EBUSY */
bool
unit_bcb_mutex_held (PGBUF_BCB * bcb)
{
  int r = pthread_mutex_trylock (&bcb->mutex);

  if (r == 0)
    {
      pthread_mutex_unlock (&bcb->mutex);
      return false;
    }
  return r == EBUSY;
}

void
unit_bcb_unlock (PGBUF_BCB * bcb)
{
  PGBUF_BCB_UNLOCK (bcb);
}
