/* ---- unitseam appendix for the TLA+ trace run (tla/storage/latch_waiter_flag/trace) ----
 * page_buffer.c's statics, reached from the test: one calloc'd BCB, per-thread holder anchors, and the real entry points
 * of a page latch -- pgbuf_latch_bcb_upon_fix () and pgbuf_unlatch_bcb_upon_unfix () called the way pgbuf_fix () /
 * pgbuf_unfix () call them (BCB mutex taken first).  Nothing here copies engine logic: the setup only fills fields. */

PGBUF_BCB *
ut_bcb_new (void)
{
  PGBUF_BCB *bcb = (PGBUF_BCB *) calloc (1, sizeof (PGBUF_BCB));
  bcb->vpid.volid = 0;
  bcb->vpid.pageid = 705;	/* the TC's page; flags 0 = no LRU zone: unfix skips the LRU bookkeeping (outside the model) */
  return bcb;
}

void
ut_set_latch_timeout (int msecs)
{
  pgbuf_latch_timeout_msecs = msecs;
}

/* pgbuf_allocate_thrd_holder_entry () takes holders from the thread's anchor; give each thread its own */
void
ut_thread_init (THREAD_ENTRY * th)
{
  PGBUF_HOLDER_ANCHOR *a = (PGBUF_HOLDER_ANCHOR *) calloc (1, sizeof (PGBUF_HOLDER_ANCHOR));
  int i;

  for (i = 0; i < 4; i++)
    {
      PGBUF_HOLDER *h = (PGBUF_HOLDER *) calloc (1, sizeof (PGBUF_HOLDER));
      h->next_holder = a->thrd_free_list;
      a->thrd_free_list = h;
      a->num_free_cnt++;
    }
  th->m_holder_anchor = a;
}

/* pgbuf_fix () → PGBUF_BCB_LOCK, pgbuf_latch_bcb_upon_fix () (unconditional) */
int
ut_fix (THREAD_ENTRY * th, PGBUF_BCB * bcb, int read)
{
  bool is_latch_wait = false;

  PGBUF_BCB_LOCK (bcb);
  return pgbuf_latch_bcb_upon_fix (th, bcb, read ? PGBUF_LATCH_READ : PGBUF_LATCH_WRITE, 0, PGBUF_UNCONDITIONAL_LATCH,
				   &is_latch_wait);
}

/* pgbuf_unfix (): drop the holder's fix, then PGBUF_BCB_LOCK and pgbuf_unlatch_bcb_upon_unfix () */
int
ut_unfix (THREAD_ENTRY * th, PGBUF_BCB * bcb)
{
  PGBUF_HOLDER *holder = pgbuf_find_thrd_holder (th, bcb);
  int holder_status = NO_ERROR;

  if (holder != NULL)
    {
      holder->fix_count--;
      if (holder->fix_count == 0)
	{
	  holder_status = pgbuf_remove_thrd_holder (th, holder);
	}
    }
  PGBUF_BCB_LOCK (bcb);
  return pgbuf_unlatch_bcb_upon_unfix (th, bcb, holder_status);
}

/* test orchestration only (not traced): is th on the wait queue / does the BCB look like this */
bool
ut_queued (PGBUF_BCB * bcb, THREAD_ENTRY * th)
{
  THREAD_ENTRY *t;
  bool found = false;

  PGBUF_BCB_LOCK (bcb);
  for (t = bcb->next_wait_thrd; t != NULL; t = t->next_wait_thrd)
    {
      found = found || t == th;
    }
  PGBUF_BCB_UNLOCK (bcb);
  return found;
}
