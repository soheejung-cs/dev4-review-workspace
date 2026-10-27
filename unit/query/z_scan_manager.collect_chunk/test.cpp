/* PR#8196 (CBRD-27361) 927a44c04 -- scan_batch_collect_chunk (), src/query/scan_manager.c.
 *
 * 927a44c04 made the collector learn a representation the plan has not met where the walk met it: the collector
 * (heap_page_collect_visible ()) ends the chunk before such a record and reports its representation; when nothing
 * was collected and the plan learns it, the walk goes on through that record as a row.  When rows WERE collected
 * before it (S_SUCCESS), the learn still runs at once (scan_manager.c:8125), while the chunk's rows are peeked
 * pointers into the page the scan holds -- and expr_batch_plan_learn_repr () reads the class record on a cache miss
 * (heap_classrepr_get (), an ordered fix of a root-class heap page that may let the scan's page go and fix it again).
 * Review C-07 (revised for 2381ce437): learn only with nothing collected; the next call meets the record first.
 *
 * Fakes (read from the real code they stand for):
 *  - heap_page_collect_visible (): the head walk restricted to representations -- the records after *slot in slot
 *    order; a record whose repid is not in repids (n_repids > 0) ends the chunk (S_SUCCESS with rows, S_DOESNT_FIT
 *    without), *slot = the slot before it, its repid in *unknown_repid when that is still NULL_REPRID; cap rows end
 *    a chunk with S_SUCCESS; the page's end is S_END (heap_file.c:8215-8358).  *n_rows is written when it returns,
 *    as there.
 *  - expr_batch_plan_gave_up / _fallback_why / _repids: the plan's state as test globals.
 *  - expr_batch_plan_learn_repr (): notes how many rows the chunk holds when it is called (batch->n_rows), then
 *    accepts the repid (NO_ERROR, now among the repids) when it is learnable, else ER_FAILED (expr_batch.c:5519). */
#include "catch.hpp"

#include "scan_manager.h"
#include "heap_file.h"
#include "expr_batch.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
  struct z_rec
  {
    PGSLOTID slot;
    REPR_ID repid;
  };

  std::vector < z_rec > z_page;
  std::vector < REPR_ID > z_known;
  std::vector < REPR_ID > z_learnable;
  bool z_gave_up;
  int z_collect_calls;
  int z_learn_calls;
  int z_rows_held_at_learn;
  EXPR_BATCH *z_batch;
  HEAP_PAGE_ROW z_rows[256];
  SCAN_ID *z_sid;

  void z_reset (std::vector < z_rec > page, std::vector < REPR_ID > known, std::vector < REPR_ID > learnable)
  {
    z_page = page;
    z_known = known;
    z_learnable = learnable;
    z_gave_up = false;
    z_collect_calls = 0;
    z_learn_calls = 0;
    z_rows_held_at_learn = -1;
  }

  std::vector < z_rec > z_rows_of (int from, int to, REPR_ID repid)
  {
    std::vector < z_rec > v;
    for (int s = from; s <= to; s++)
      {
	v.push_back ({(PGSLOTID) s, repid});
      }
    return v;
  }

  SCAN_CODE z_run (PGSLOTID * slot, int cap)
  {
    static EXPR_BATCH batch;
    VPID vpid;

    if (z_sid == NULL)
      {
	z_sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));	/* never constructed: the collector reads plain fields */
      }
    memset (z_sid, 0, sizeof (SCAN_ID));
    memset (&batch, 0, sizeof (batch));
    batch.rows = z_rows;
    batch.cap = cap;
    z_batch = &batch;
    z_sid->type = S_HEAP_SCAN;
    z_sid->s.hsid.batch = &batch;
    z_sid->s.hsid.batch_plan = (EXPR_BATCH_PLAN *) (void *) &batch;	/* a token: the fakes do not read it */
    vpid.pageid = 77;
    vpid.volid = 0;
    return scan_batch_collect_chunk (NULL, z_sid, &vpid, slot);
  }

  std::vector < int > z_collected ()
  {
    std::vector < int > v;
    for (int i = 0; i < z_batch->n_rows; i++)
      {
	v.push_back (z_batch->rows[i].oid.slotid);
      }
    return v;
  }
}

/* ---- fakes ---- */

SCAN_CODE
heap_page_collect_visible (THREAD_ENTRY * thread_p, const VPID * vpid, const OID * class_oid,
			   HEAP_SCANCACHE * scan_cache, PGSLOTID * slot, HEAP_PAGE_ROW * rows, int cap, int *n_rows,
			   const REPR_ID * repids, int n_repids, REPR_ID * unknown_repid)
{
  int n = 0;

  (void) thread_p;
  (void) vpid;
  (void) class_oid;
  (void) scan_cache;
  z_collect_calls++;
  for (auto & r:z_page)
    {
      bool known = false;

      if (r.slot <= *slot)
	{
	  continue;
	}
      for (int i = 0; i < n_repids; i++)
	{
	  known = known || (repids[i] == r.repid);
	}
      if (n_repids > 0 && !known)
	{
	  if (unknown_repid != NULL && *unknown_repid == NULL_REPRID)
	    {
	      *unknown_repid = r.repid;
	    }
	  *slot = r.slot - 1;
	  *n_rows = n;
	  return (n > 0) ? S_SUCCESS : S_DOESNT_FIT;
	}
      memset (&rows[n], 0, sizeof (rows[n]));
      rows[n].oid.slotid = r.slot;
      n++;
      if (n == cap)
	{
	  *slot = r.slot;
	  *n_rows = n;
	  return S_SUCCESS;
	}
    }
  *n_rows = n;
  return S_END;
}

bool
expr_batch_plan_gave_up (const EXPR_BATCH_PLAN * plan)
{
  (void) plan;
  return z_gave_up;
}

const char *
expr_batch_fallback_why (const EXPR_BATCH_PLAN * plan)
{
  (void) plan;
  return "the batch gave up";
}

const REPR_ID *
expr_batch_plan_repids (const EXPR_BATCH_PLAN * plan, int *n)
{
  (void) plan;
  *n = (int) z_known.size ();
  return z_known.empty ()? NULL : z_known.data ();
}

int
expr_batch_plan_learn_repr (THREAD_ENTRY * thread_p, EXPR_BATCH_PLAN * plan, REPR_ID repid)
{
  (void) thread_p;
  (void) plan;
  z_learn_calls++;
  z_rows_held_at_learn = z_batch->n_rows;
  for (REPR_ID r:z_learnable)
    {
      if (r == repid)
	{
	  z_known.push_back (repid);
	  return NO_ERROR;
	}
    }
  return ER_FAILED;
}

/* ---- cases ---- */

TEST_CASE ("the plan gave up: S_DOESNT_FIT without a collection, the plan's cause on the scan", "[contract]")
{
  PGSLOTID slot = 0;
  z_reset (z_rows_of (1, 5, 1), {1}, {});
  z_gave_up = true;
  CHECK (z_run (&slot, 64) == S_DOESNT_FIT);
  CHECK (z_collect_calls == 0);
  CHECK (z_batch->n_rows == 0);
  CHECK (z_sid->s.hsid.batch_why_not != NULL);
}

TEST_CASE ("an unknown representation first on the page, learnable: the walk goes on through that record",
	   "[contract]")
{
  PGSLOTID slot = 0;
  std::vector < z_rec > page = z_rows_of (2, 5, 1);
  page.insert (page.begin (), {1, 2});
  z_reset (page, {1}, {2});
  CHECK (z_run (&slot, 64) == S_END);
  CHECK (z_collected () == std::vector < int > ({1, 2, 3, 4, 5}));
  CHECK (z_learn_calls == 1);
  CHECK (z_rows_held_at_learn == 0);
  CHECK (z_collect_calls == 2);
}

TEST_CASE ("an unknown representation first on the page, not learnable: S_DOESNT_FIT, nothing collected",
	   "[contract]")
{
  PGSLOTID slot = 0;
  std::vector < z_rec > page = z_rows_of (2, 5, 1);
  page.insert (page.begin (), {1, 2});
  z_reset (page, {1}, {});
  CHECK (z_run (&slot, 64) == S_DOESNT_FIT);
  CHECK (z_batch->n_rows == 0);
  CHECK (z_learn_calls == 1);
}

TEST_CASE ("rows, then an unknown representation: the chunk is the rows before it, and the next collection gives "
	   "the rest of the page as rows once the representation is learned", "[contract]")
{
  PGSLOTID slot = 0;
  std::vector < z_rec > page = z_rows_of (1, 3, 1);
  page.push_back ({4, 2});
  for (z_rec r:z_rows_of (5, 6, 1))
    {
      page.push_back (r);
    }
  z_reset (page, {1}, {2});
  CHECK (z_run (&slot, 64) == S_SUCCESS);
  CHECK (z_collected () == std::vector < int > ({1, 2, 3}));
  CHECK (slot == 3);
  CHECK (z_run (&slot, 64) == S_END);
  CHECK (z_collected () == std::vector < int > ({4, 5, 6}));
}

TEST_CASE ("C-07 (2381ce437): rows collected, then a record of an unknown representation -- the representation is "
	   "learned (a class-record read) while the chunk's peeked rows are held (PASS = defect present)", "[defect]")
{
  PGSLOTID slot = 0;
  std::vector < z_rec > page = z_rows_of (1, 3, 1);
  page.push_back ({4, 2});
  for (z_rec r:z_rows_of (5, 6, 1))
    {
      page.push_back (r);
    }
  z_reset (page, {1}, {2});
  CHECK (z_run (&slot, 64) == S_SUCCESS);
  CHECK (z_learn_calls == 1);
  CHECK (z_rows_held_at_learn == 3);
}
