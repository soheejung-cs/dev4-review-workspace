/* ---- unitseam appendix (PR#8497 GD-01 suite): a page buffer of a few pages, from inside page_buffer.c ----
 *
 * pgbuf_Pool, PGBUF_BCB, PGBUF_HOLDER and the holder helpers are this TU's.  The test fixes pages through the real
 * pgbuf_ordered_fix () and heap_page_list_fix_next (); only the bottom of a fix (pgbuf_fix_release ()) and of an unfix
 * (pgbuf_unfix ()) are the test's -- test.cpp's strong definitions call unit_pgbuf_fix () / unit_pgbuf_unfix () below.
 *
 * What unit_pgbuf_fix () copies from pgbuf_fix_release () (page_buffer.c, 3328953d3 line numbers):
 *   - the interrupt check (2351-2360): ER_INTERRUPTED, NULL
 *   - a zero wait turns an unconditional request conditional (2327-2337)
 *   - OLD_PAGE_PREVENT_DEALLOC marks the BCB before the latch is tried (2476-2479) and unmarks it once the latch is
 *     held (2564-2568); a refused conditional request leaves the mark (2491-2514)
 *   - the latch, as pgbuf_latch_bcb_upon_fix () decides it (7021-7091): another thread's write latch blocks every
 *     request, its read latch a write request, and a writer queued behind a reader blocks a new reader that does not
 *     hold the page yet (waiter_exists, 7024-7037); a conditional request is refused without an error unless the wait
 *     is LK_ZERO_WAIT (7205-7238); an unconditional one blocks until the other thread lets the page go
 *   - the holder: this thread's holder entry of the BCB gets one more fix, or a new entry from the thread's list
 *     (pgbuf_find_thrd_holder (), pgbuf_allocate_thrd_holder_entry () -- this TU's own functions, 7106-7176)
 *   - a deallocated page (ptype PAGE_UNKNOWN, 2574-2608): OLD_PAGE_MAYBE_DEALLOCATED sets ER_PB_BAD_PAGEID as a
 *     warning, unfixes and returns NULL; OLD_PAGE and OLD_PAGE_PREVENT_DEALLOC set it as an error
 * What the other thread does before it lets a blocked page go is the case's script (unit_pgbuf_set_other ()):
 *   UNIT_THEN_DEALLOC     file_perm_dealloc () at postpone: write latch (file_manager.c:6530), pgbuf_dealloc_page ()
 *                         sets PAGE_UNKNOWN (page_buffer.c:16011-16014) -- it checks no dealloc mark and no waiter
 *   UNIT_THEN_REUSE_FTAB  the page deallocated, then allocated again as the file's table page (ptype PAGE_FTAB)
 *   UNIT_THEN_RELEASE     the page let go as it was
 * unit_pgbuf_unfix () is pgbuf_unfix ()'s holder half: pgbuf_unlatch_thrd_holder () (6779-6829, this TU's).
 */

#define UNIT_PAGES_MAX 48
#define UNIT_THREADS 4

enum unit_other
{
  UNIT_OTHER_NONE = 0,
  UNIT_OTHER_READ,
  UNIT_OTHER_WRITE,
  UNIT_OTHER_WRITER_WAITS
};

enum unit_then
{
  UNIT_THEN_RELEASE = 0,
  UNIT_THEN_DEALLOC,
  UNIT_THEN_REUSE_FTAB
};

struct unit_page
{
  PGBUF_BCB *bcb;
  PGBUF_IOPAGE_BUFFER *iop;
  int other;
  int then;
  int fixes;			/* this thread's fixes now */
  int fix_calls;		/* fix requests on the page */
  int cond_refused;		/* conditional requests refused */
  int waited;			/* unconditional requests that waited for the other thread */
};

static unit_page unit_pages[UNIT_PAGES_MAX];
static int unit_npages;
static PGBUF_BUFFER_HASH *unit_hash_table;
static PGBUF_HOLDER_ANCHOR *unit_anchors;
static bool unit_interrupt_next_fix;
static int unit_unknown_fix;	/* fix requests of pages not on the test's disk */

void unit_pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr);

static unit_page *
unit_find (short volid, int pageid)
{
  int i;

  for (i = 0; i < unit_npages; i++)
    {
      if (unit_pages[i].bcb->vpid.volid == volid && unit_pages[i].bcb->vpid.pageid == pageid)
	{
	  return &unit_pages[i];
	}
    }
  return NULL;
}

void
unit_pgbuf_reset (THREAD_ENTRY * thread_p)
{
  int i;

  if (unit_hash_table == NULL)
    {
      unit_hash_table = (PGBUF_BUFFER_HASH *) calloc (PGBUF_HASH_SIZE, sizeof (PGBUF_BUFFER_HASH));
    }
  for (i = 0; i < unit_npages; i++)
    {
      unit_hash_table[PGBUF_HASH_VALUE (&unit_pages[i].bcb->vpid)].hash_next = NULL;
      free (unit_pages[i].bcb);
      free (unit_pages[i].iop);
    }
  memset (unit_pages, 0, sizeof (unit_pages));
  unit_npages = 0;

  /* the holder entries: every set the last case allocated, then fresh anchors for every thread index */
  while (pgbuf_Pool.free_holder_set != NULL)
    {
      PGBUF_HOLDER_SET *next = pgbuf_Pool.free_holder_set->next_set;
      free (pgbuf_Pool.free_holder_set);
      pgbuf_Pool.free_holder_set = next;
    }
  pgbuf_Pool.free_index = -1;	/* the first allocation takes a new set (pgbuf_allocate_thrd_holder_entry ()) */
  free (unit_anchors);
  unit_anchors = (PGBUF_HOLDER_ANCHOR *) calloc (UNIT_THREADS, sizeof (PGBUF_HOLDER_ANCHOR));
  pgbuf_Pool.thrd_holder_info = unit_anchors;
  pgbuf_Pool.buf_hash_table = unit_hash_table;
  pgbuf_Pool.check_for_interrupts = false;
  thread_p->m_holder_anchor = NULL;
  unit_interrupt_next_fix = false;
  unit_unknown_fix = 0;
}

/* a page of the test's disk, in the buffer: its BCB at the head of its hash chain (pgbuf_insert_into_hash_chain ()) */
void
unit_pgbuf_add_page (short volid, int pageid, int ptype)
{
  unit_page *p;
  unsigned int h;
  size_t sz = offsetof (PGBUF_IOPAGE_BUFFER, iopage) + sizeof (FILEIO_PAGE) + 64 * 1024;

  if (unit_npages >= UNIT_PAGES_MAX || unit_find (volid, pageid) != NULL)
    {
      abort ();
    }
  p = &unit_pages[unit_npages++];
  p->bcb = (PGBUF_BCB *) calloc (1, sizeof (PGBUF_BCB));
  p->iop = (PGBUF_IOPAGE_BUFFER *) calloc (1, sz);
  p->bcb->vpid.volid = volid;
  p->bcb->vpid.pageid = pageid;
  p->bcb->iopage_buffer = p->iop;
  p->iop->bcb = p->bcb;
  p->iop->iopage.prv.volid = volid;
  p->iop->iopage.prv.pageid = pageid;
  p->iop->iopage.prv.ptype = (unsigned char) ptype;
  h = PGBUF_HASH_VALUE (&p->bcb->vpid);
  p->bcb->hash_next = unit_hash_table[h].hash_next;
  unit_hash_table[h].hash_next = p->bcb;
}

/* another thread on the page, and what it does before it lets the page go to a request that waits for it */
void
unit_pgbuf_set_other (short volid, int pageid, int other, int then)
{
  unit_page *p = unit_find (volid, pageid);

  p->other = other;
  p->then = then;
}

void
unit_pgbuf_set_ptype (short volid, int pageid, int ptype)
{
  unit_find (volid, pageid)->iop->iopage.prv.ptype = (unsigned char) ptype;
}

int
unit_pgbuf_ptype (short volid, int pageid)
{
  return unit_find (volid, pageid)->iop->iopage.prv.ptype;
}

/* counters: 0 fixes now, 1 fix requests, 2 conditional requests refused, 3 requests that waited */
int
unit_pgbuf_count (short volid, int pageid, int which)
{
  unit_page *p = unit_find (volid, pageid);

  switch (which)
    {
    case 0:
      return p->fixes;
    case 1:
      return p->fix_calls;
    case 2:
      return p->cond_refused;
    default:
      return p->waited;
    }
}

/* the BCB's avoid-deallocation count (pgbuf_bcb_register_avoid_deallocation ()) */
int
unit_pgbuf_avoid_dealloc (short volid, int pageid)
{
  return unit_find (volid, pageid)->bcb->count_fix_and_avoid_dealloc & PGBUF_BCB_AVOID_DEALLOC_MASK;
}

PAGE_PTR
unit_pgbuf_page_ptr (short volid, int pageid)
{
  PAGE_PTR pgptr;
  unit_page *p = unit_find (volid, pageid);

  CAST_BFPTR_TO_PGPTR (pgptr, p->bcb);
  return pgptr;
}

/* the thread's holder list, as pgbuf_ordered_fix () walks it: page ids, fix and watch counts */
int
unit_pgbuf_held (THREAD_ENTRY * thread_p, int *pageids, int *fix_counts, int *watch_counts, int max)
{
  PGBUF_HOLDER *holder;
  int n = 0;

  for (holder = pgbuf_Pool.thrd_holder_info[thread_p->index].thrd_hold_list; holder != NULL; holder = holder->thrd_link)
    {
      if (n < max)
	{
	  pageids[n] = holder->bufptr->vpid.pageid;
	  fix_counts[n] = holder->fix_count;
	  watch_counts[n] = holder->watch_count;
	}
      n++;
    }
  return n;
}

void
unit_pgbuf_interrupt_next_fix (void)
{
  unit_interrupt_next_fix = true;
}

int
unit_pgbuf_unknown_fixes (void)
{
  return unit_unknown_fix;
}

PAGE_PTR
unit_pgbuf_fix (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode, PGBUF_LATCH_MODE request_mode,
		PGBUF_LATCH_CONDITION condition)
{
  unit_page *p = unit_find (vpid->volid, vpid->pageid);
  PGBUF_HOLDER *holder;
  PAGE_PTR pgptr;
  bool blocked;

  if (p == NULL)
    {
      /* not a page of the test's disk: the case asked for a page it did not lay out */
      unit_unknown_fix++;
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_PB_BAD_PAGEID, 2, vpid->pageid, "unit");
      return NULL;
    }
  p->fix_calls++;

  /* interrupt check */
  if (unit_interrupt_next_fix)
    {
      unit_interrupt_next_fix = false;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
      return NULL;
    }

  /* a zero wait makes an unconditional request conditional */
  if (condition == PGBUF_UNCONDITIONAL_LATCH)
    {
      int wait_msecs = pgbuf_find_current_wait_msecs (thread_p);

      if (wait_msecs == LK_ZERO_WAIT || wait_msecs == LK_FORCE_ZERO_WAIT)
	{
	  condition = PGBUF_CONDITIONAL_LATCH;
	}
    }

  if (fetch_mode == OLD_PAGE_PREVENT_DEALLOC)
    {
      pgbuf_bcb_register_avoid_deallocation (p->bcb);
    }

  holder = pgbuf_find_thrd_holder (thread_p, p->bcb);
  blocked = (p->other == UNIT_OTHER_WRITE || (p->other == UNIT_OTHER_READ && request_mode == PGBUF_LATCH_WRITE)
	     || (p->other == UNIT_OTHER_WRITER_WAITS && holder == NULL));
  if (blocked)
    {
      if (condition == PGBUF_CONDITIONAL_LATCH)
	{
	  p->cond_refused++;
	  if (pgbuf_find_current_wait_msecs (thread_p) == LK_ZERO_WAIT)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_LK_PAGE_TIMEOUT, 0);
	    }
	  return NULL;
	}

      /* blocked until the other thread lets the page go */
      p->waited++;
      if (p->then == UNIT_THEN_DEALLOC)
	{
	  p->iop->iopage.prv.ptype = (unsigned char) PAGE_UNKNOWN;
	  p->iop->iopage.prv.pflag = 0;
	}
      else if (p->then == UNIT_THEN_REUSE_FTAB)
	{
	  p->iop->iopage.prv.ptype = (unsigned char) PAGE_FTAB;
	}
      p->other = UNIT_OTHER_NONE;
    }

  if (holder != NULL)
    {
      holder->fix_count++;
    }
  else
    {
      holder = pgbuf_allocate_thrd_holder_entry (thread_p);
      holder->fix_count = 1;
      holder->bufptr = p->bcb;
    }
  p->fixes++;

  if (fetch_mode == OLD_PAGE_PREVENT_DEALLOC)
    {
      pgbuf_bcb_unregister_avoid_deallocation (p->bcb);
    }

  CAST_BFPTR_TO_PGPTR (pgptr, p->bcb);
  if (p->iop->iopage.prv.ptype == PAGE_UNKNOWN)
    {
      switch (fetch_mode)
	{
	case NEW_PAGE:
	case OLD_PAGE_DEALLOCATED:
	case OLD_PAGE_IF_IN_BUFFER:
	case RECOVERY_PAGE:
	  break;
	case OLD_PAGE_MAYBE_DEALLOCATED:
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_PB_BAD_PAGEID, 2, vpid->pageid, "unit");
	  unit_pgbuf_unfix (thread_p, pgptr);
	  return NULL;
	default:
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PB_BAD_PAGEID, 2, vpid->pageid, "unit");
	  unit_pgbuf_unfix (thread_p, pgptr);
	  return NULL;
	}
    }
  return pgptr;
}

void
unit_pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr)
{
  PGBUF_BCB *bcb;
  unit_page *p;

  if (pgptr == NULL)
    {
      return;
    }
  CAST_PGPTR_TO_BFPTR (bcb, pgptr);
  p = unit_find (bcb->vpid.volid, bcb->vpid.pageid);
  (void) pgbuf_unlatch_thrd_holder (thread_p, bcb, NULL);
  p->fixes--;
}
