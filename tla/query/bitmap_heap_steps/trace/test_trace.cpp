/* TLA+ trace run for tla/query/bitmap_heap_steps (PR #8627, bitmap heap scan) -- skills/harness-review/tla.md §3-T.
 *
 * TU: src/query/parallel/px_scan/px_scan_input_handler_index.cpp (the barrier: bitmap_init_on_main (),
 * bitmap_leave_index_step (), bitmap_wait_index_step ()), with src/query/scan_manager.c linked as --extra (the real
 * scan_bitmap_marks: init (), claim_worker (), mark (), end_marks (), take_partition (), load (), take_over (),
 * close_worker (), ~scan_bitmap_marks ()).  Both built with instrument.patch and -DTLA_TRACE.
 *
 * The driver is px_scan_task.cpp's worker as of efca4544a, copied in its order (that TU needs the whole executor):
 *   loop () :1236-1241  enter_worker, claim_worker
 *   index step          leaf_slot_walker::process_oid () :485-489 -> mark () for each OID the worker's leaves give
 *   bitmap_heap_step () :1402-1411 end_marks () unless stopped, end_bitmap_index_step, bitmap_leave_index_step (),
 *                       bitmap_wait_index_step (), return on false
 *                       :1474-1517 take_partition () / load (part, 0) / the pages of heap_tbl (rows: none here --
 *                       drain_slot_oids is the executor)
 *                       :1520-1546 take_over () (no list here: no OID beyond the width)
 *   finalize ()         slot_iterator_index::finalize () :67-71 -> close_worker ()
 * UT_SCENARIO=intr: worker UT_FAIL_W stops in its index step after UT_FAIL_AFTER marks the way loop () does on an
 * error (:1313-1328: move the message, set ERROR_INTERRUPTED_FROM_WORKER_THREAD, break) -- it never leaves the
 * barrier, then finalize ().
 *
 * Fakes (other translation units): logtb_get_mvcc_snapshot () -> a snapshot (a statement's, taken);
 * file_get_all_data_sectors () -> two full sectors of volume 0 (sectid 0 and 1, every page allocated) -- the heap
 * the scan reads.  Everything else the two TUs call is unitseam's zero stub (prm_get_integer_value () = 0: the
 * table takes one page of memory, 174 entries; the two sectors make 2 partitions of 1 part each).
 * The OIDs: page 1 slot 1, page 2 slot 3 (sector 0 -> partition 0), page 65 slot 1, page 70 slot 2 (sector 1 ->
 * partition 1); a model OID is pageid * 1000 + slot.  Each worker marks a random subset (overlaps allowed: two
 * workers may give the same OID). */
#include "catch.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <thread>
#include <vector>

#include "px_scan_input_handler_index.hpp"
#include "file_manager.h"
#include "log_impl.h"
#include "mvcc.h"
#include "scan_manager.h"

/* ---- fakes ---- */
static MVCC_SNAPSHOT ut_snapshot;

MVCC_SNAPSHOT *
logtb_get_mvcc_snapshot (THREAD_ENTRY * thread_p)
{
  return &ut_snapshot;
}

int
file_get_all_data_sectors (THREAD_ENTRY * thread_p, const VFID * vfid, FILE_FTAB_COLLECTOR * collector_out)
{
  FILE_PARTIAL_SECTOR *s = (FILE_PARTIAL_SECTOR *) calloc (2, sizeof (FILE_PARTIAL_SECTOR));

  for (int i = 0; i < 2; i++)
    {
      s[i].vsid.volid = 0;
      s[i].vsid.sectid = i;
      s[i].page_bitmap = ~(FILE_ALLOC_BITMAP) 0;
    }
  collector_out->nsects = 2;
  collector_out->npages = 128;
  collector_out->partsect_ftab = s;
  return NO_ERROR;
}

/* private bitmap_init_on_main (): reached the legal way, through an explicit instantiation's member pointer */
using ut_init_fn = int (parallel_scan::input_handler_index::*) (THREAD_ENTRY *, SCAN_ID *, int);
ut_init_fn ut_get_init ();
template <ut_init_fn F> struct ut_rob
{
  friend ut_init_fn ut_get_init ()
  {
    return F;
  }
};
template struct ut_rob<&parallel_scan::input_handler_index::bitmap_init_on_main>;

static int
ut_env (const char *name, int dflt)
{
  const char *v = getenv (name);
  return v != nullptr ? atoi (v) : dflt;
}

TEST_CASE ("bitmap heap steps: N workers through the real barrier, partitions and take_over", "[trace]")
{
  const int nworkers = ut_env ("UT_WORKERS", 2);
  const bool intr_scenario = getenv ("UT_SCENARIO") != nullptr && strcmp (getenv ("UT_SCENARIO"), "intr") == 0;
  const int fail_w = ut_env ("UT_FAIL_W", 0);	/* the thread (0..N-1) that fails in intr */
  const int fail_after = ut_env ("UT_FAIL_AFTER", 1);
  const unsigned seed = (unsigned) ut_env ("UT_SEED", 1);
  const OID oids[] = { {1, 1, 0}, {2, 3, 0}, {65, 1, 0}, {70, 2, 0} };	/* pageid, slotid, volid */

  db_User_page_size = 16384;

  parallel_query::interrupt intr;
  intr.clear ();
  void *mem = calloc (1, sizeof (parallel_scan::input_handler_index));
  auto *handler = new (mem) parallel_scan::input_handler_index (&intr, nullptr);

  SCAN_ID *scan_id = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
  INDX_INFO *indx_info = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
  scan_id->s.isid.bitmap_planned = true;
  scan_id->s.isid.indx_info = indx_info;	/* coverage 0: not covering */
  REQUIRE ((handler->*ut_get_init ()) (nullptr, scan_id, nworkers) == NO_ERROR);
  REQUIRE (handler->bitmap_on ());

  /* each worker's OIDs: a random non-empty subset (two workers may give the same OID) */
  std::mt19937 rng (seed);
  std::vector<std::vector<OID>> mine (nworkers);
  for (int t = 0; t < nworkers; t++)
    {
      int mask = 1 + (int) (rng () % 15);
      for (int k = 0; k < 4; k++)
	{
	  if (mask & (1 << k))
	    {
	      mine[t].push_back (oids[k]);
	    }
	}
    }

  std::atomic<int> ready (0);
  std::atomic<bool> go (false);
  std::vector<std::thread> threads;

  for (int t = 0; t < nworkers; t++)
    {
      threads.emplace_back ([&, t] ()
      {
	scan_bitmap_marks &bm = handler->bitmap ();
	int hint = -1;

	ready++;
	while (!go.load ())
	  {
	  }
	/* loop (): claim_worker () */
	scan_bitmap_worker *w = bm.claim_worker ();
	bool stop = false;

	/* index step */
	for (size_t i = 0; i < mine[t].size (); i++)
	  {
	    if (intr_scenario && t == fail_w && (int) i == fail_after)
	      {
		/* loop ()'s error path: the interrupt, break -- no barrier leave */
		intr.set_code (parallel_query::interrupt::interrupt_code::ERROR_INTERRUPTED_FROM_WORKER_THREAD);
		stop = true;
		break;
	      }
	    if (intr.get_code () != parallel_query::interrupt::interrupt_code::NO_INTERRUPT)
	      {
		stop = true;	/* loop (): the interrupt check before the next page */
		break;
	      }
	    REQUIRE (bm.mark (nullptr, w, &mine[t][i], &hint) == NO_ERROR);
	  }
	if (intr_scenario && t == fail_w && !stop)
	  {
	    intr.set_code (parallel_query::interrupt::interrupt_code::ERROR_INTERRUPTED_FROM_WORKER_THREAD);
	    stop = true;
	  }

	if (!stop)
	  {
	    /* bitmap_heap_step () */
	    REQUIRE (bm.end_marks (nullptr, w) == NO_ERROR);
	    handler->bitmap_leave_index_step ();
	    if (handler->bitmap_wait_index_step ())
	      {
		std::uint32_t part;

		while (bm.take_partition (&part))
		  {
		    REQUIRE (bm.load (nullptr, w, part, 0) == NO_ERROR);
		    /* the pages of heap_tbl: rows go to the executor -- none here */
		  }
		int error_code = NO_ERROR;
		REQUIRE_FALSE (bm.take_over (nullptr, w, &error_code));
		REQUIRE (error_code == NO_ERROR);
	      }
	  }
	/* finalize () */
	bm.close_worker (nullptr, w);
      });
    }
  while (ready.load () < nworkers)
    {
    }
  go = true;
  for (auto &th : threads)
    {
      th.join ();
    }

  handler->~input_handler_index ();
  free (mem);
  free (indx_info);
  free (scan_id);
}
