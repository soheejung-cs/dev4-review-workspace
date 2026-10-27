/* PR#8196 (CBRD-27361) 927a44c04 -- heap_page_collect_visible (), src/storage/heap_file.c.
 *
 * Since 927a44c04 a record the page does not answer for -- REC_RELOCATION, REC_BIGONE, a version too new for the
 * snapshot with a previous one in the log, a visible record of a representation the caller does not decode -- ends
 * the chunk before it: the rows before it go out (S_SUCCESS, *slot = the slot before the record) and the next call,
 * with nothing collected, answers S_DOESNT_FIT; the callers (qexec_batch_scan_chain (), scan_batch_iter_next (), the
 * px worker) then hand the page to the row path from that slot to the page's end.  Before 927a44c04 the record went
 * to an exception list fetched by OID and the rest of the page stayed in the batch.
 *
 * Fakes (read from the real code they stand for):
 *  - spage_next_record (): the next record whose slot is above *slotid, S_END past the last (slotted_page.c walk);
 *  - or_mvcc_get_header (): reads the header the test wrote into the record bytes -- all-visible (no INSID flag,
 *    insid MVCCID_ALL_VISIBLE) or an INSID the fake snapshot answers for, a previous-version LSA or none, a repid;
 *  - the snapshot function answers SATISFIED / TOO_OLD / TOO_NEW from that INSID;
 *  - pgbuf_get_vpid_ptr () gives the fake page's VPID (the watcher already holds it: no fix is made);
 *  - heap_page_is_bestspace () (seam) is false;
 *  - oid_Root_class_oid points at an OID no test record has. */
#include "catch.hpp"

#include "heap_file.h"
#include "slotted_page.h"
#include "page_buffer.h"
#include "object_representation_sr.h"
#include "object_representation_constants.h"
#include "mvcc.h"

#include <cstdint>
#include <cstring>
#include <vector>

/* both revisions' collectors, weak: the one this revision's TU defines is non-null.  head (927a44c04 and later): the
 * chunk ends before a record the page does not answer for; 462e6739d and earlier: such a record goes to an exception
 * list (exc, exc_cap, n_exc) and the chunk goes on. */
extern SCAN_CODE heap_page_collect_visible (THREAD_ENTRY * thread_p, const VPID * vpid, const OID * class_oid,
					    HEAP_SCANCACHE * scan_cache, PGSLOTID * slot, HEAP_PAGE_ROW * rows, int cap,
					    int *n_rows, const REPR_ID * repids, int n_repids, REPR_ID * unknown_repid)
  __attribute__ ((weak));
extern SCAN_CODE heap_page_collect_visible (THREAD_ENTRY * thread_p, const VPID * vpid, const OID * class_oid,
					    HEAP_SCANCACHE * scan_cache, PGSLOTID * slot, HEAP_PAGE_ROW * rows, int cap,
					    int *n_rows, const REPR_ID * repids, int n_repids, REPR_ID * unknown_repid,
					    OID * exc, int exc_cap, int *n_exc) __attribute__ ((weak));

namespace
{
  typedef SCAN_CODE (*z_collect_new_fn) (THREAD_ENTRY *, const VPID *, const OID *, HEAP_SCANCACHE *, PGSLOTID *,
					 HEAP_PAGE_ROW *, int, int *, const REPR_ID *, int, REPR_ID *);
  typedef SCAN_CODE (*z_collect_old_fn) (THREAD_ENTRY *, const VPID *, const OID *, HEAP_SCANCACHE *, PGSLOTID *,
					 HEAP_PAGE_ROW *, int, int *, const REPR_ID *, int, REPR_ID *, OID *, int, int *);
  const z_collect_new_fn z_new = heap_page_collect_visible;
  const z_collect_old_fn z_old = heap_page_collect_visible;

  enum z_vis
  { Z_ALL, Z_SAT, Z_OLD, Z_NEW, Z_NEW_PREV };

  struct z_hdr
  {
    int magic;
    int vis;
    int repid;
  };

  struct z_rec
  {
    PGSLOTID slot;
    INT16 type;
    z_hdr hdr;
  };

  struct z_page
  {
    VPID vpid;
    std::vector < z_rec > recs;
  };

  z_page z_make_page (int n_rows, int repid = 1)
  {
    z_page p;
    p.vpid.pageid = 77;
    p.vpid.volid = 0;
    for (int s = 1; s <= n_rows; s++)
      {
	z_rec r;
	r.slot = (PGSLOTID) s;
	r.type = REC_HOME;
	r.hdr.magic = 0x5a5a;
	r.hdr.vis = Z_ALL;
	r.hdr.repid = repid;
	p.recs.push_back (r);
      }
    return p;
  }

  z_rec *z_at (z_page & p, int slot)
  {
    for (auto & r:p.recs)
      {
	if (r.slot == slot)
	  {
	    return &r;
	  }
      }
    return NULL;
  }

  MVCC_SATISFIES_SNAPSHOT_RESULT z_snapshot (THREAD_ENTRY *, MVCC_REC_HEADER * h, MVCC_SNAPSHOT *)
  {
    switch ((int) (h->mvcc_ins_id - 1000))
      {
      case Z_SAT:
	return SNAPSHOT_SATISFIED;
      case Z_OLD:
	return TOO_OLD_FOR_SNAPSHOT;
      default:
	return TOO_NEW_FOR_SNAPSHOT;
      }
  }

  struct z_result
  {
    SCAN_CODE code;
    int n;
    PGSLOTID slot;
    REPR_ID unknown;
    int n_exc;			/* 462e6739d and earlier: the records listed for an OID fetch after the chunk */
    std::vector < int > rows;
    int rowpath_records;	/* records from the walk's slot to the page's end: what the row path is handed */
  };

  z_result z_collect (z_page & pg, PGSLOTID slot, int cap, const std::vector < REPR_ID > &repids)
  {
    static unsigned char snap_mem[sizeof (MVCC_SNAPSHOT) + 64];
    MVCC_SNAPSHOT *snap = (MVCC_SNAPSHOT *) (void *) (((uintptr_t) snap_mem + 63) & ~(uintptr_t) 63);
    HEAP_SCANCACHE sc;
    std::vector < HEAP_PAGE_ROW > rows ((size_t) cap);
    VPID vpid = pg.vpid;
    OID cls;
    z_result r;

    memset (&sc, 0, sizeof (sc));
    snap->snapshot_fnc = z_snapshot;	/* the only member the collector reads (never constructed: no mvcc.c here) */
    sc.mvcc_snapshot = snap;
    sc.mvcc_disabled_class = false;
    sc.page_watcher.pgptr = (PAGE_PTR) (void *) &pg;
    cls.pageid = 5;
    cls.slotid = 1;
    cls.volid = 0;
    r.slot = slot;
    r.n = -1;
    r.unknown = NULL_REPRID;
    r.n_exc = 0;
    if (z_new != NULL)
      {
	r.code =
	  z_new (NULL, &vpid, &cls, &sc, &r.slot, rows.data (), cap, &r.n, repids.empty ()? NULL : repids.data (),
		 (int) repids.size (), &r.unknown);
      }
    else
      {
	std::vector < OID > exc ((size_t) cap);

	REQUIRE (z_old != NULL);
	r.code =
	  z_old (NULL, &vpid, &cls, &sc, &r.slot, rows.data (), cap, &r.n, repids.empty ()? NULL : repids.data (),
		 (int) repids.size (), &r.unknown, exc.data (), cap, &r.n_exc);
      }
    for (int i = 0; i < r.n; i++)
      {
	r.rows.push_back (rows[i].oid.slotid);
      }
    r.rowpath_records = 0;
    for (auto & rec:pg.recs)
      {
	r.rowpath_records += (rec.slot > r.slot) ? 1 : 0;
      }
    return r;
  }
}

/* ---- fakes ---- */

OID z_root_oid = { -9, -9, -9 };
OID *oid_Root_class_oid = &z_root_oid;

SCAN_CODE
spage_next_record (PAGE_PTR pgptr, PGSLOTID * slotid, RECDES * recdes, int ispeeking)
{
  z_page *pg = (z_page *) (void *) pgptr;

  (void) ispeeking;
  for (auto & r:pg->recs)
    {
      if (r.slot > *slotid)
	{
	  *slotid = r.slot;
	  recdes->type = r.type;
	  recdes->data = (char *) &r.hdr;
	  recdes->length = (int) sizeof (r.hdr);
	  recdes->area_size = -1;
	  return S_SUCCESS;
	}
    }
  return S_END;
}

int
or_mvcc_get_header (RECDES * record, MVCC_REC_HEADER * h)
{
  const z_hdr *f = (const z_hdr *) (const void *) record->data;
  MVCC_REC_HEADER init = MVCC_REC_HEADER_INITIALIZER;

  REQUIRE (f->magic == 0x5a5a);
  *h = init;
  h->repid = f->repid;
  if (f->vis == Z_ALL)
    {
      h->mvcc_flag = 0;
      h->mvcc_ins_id = MVCCID_ALL_VISIBLE;
    }
  else
    {
      h->mvcc_flag = OR_MVCC_FLAG_VALID_INSID;
      h->mvcc_ins_id = (MVCCID) (1000 + f->vis);
      if (f->vis == Z_NEW_PREV)
	{
	  h->prev_version_lsa = LOG_LSA (12, 34);
	}
    }
  return NO_ERROR;
}

VPID *
pgbuf_get_vpid_ptr (PAGE_PTR pgptr)
{
  return &((z_page *) (void *) pgptr)->vpid;
}

bool
heap_page_is_bestspace (THREAD_ENTRY * thread_p, PAGE_PTR page_heap)
{
  (void) thread_p;
  (void) page_heap;
  return false;
}

/* ---- cases ---- */

TEST_CASE ("visible rows only: chunks of cap rows, then S_END with the rest", "[contract]")
{
  z_page pg = z_make_page (10);
  z_result a = z_collect (pg, 0, 4, {});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.n == 4);
  CHECK (a.slot == 4);
  z_result b = z_collect (pg, a.slot, 4, {});
  CHECK (b.code == S_SUCCESS);
  CHECK (b.n == 4);
  CHECK (b.slot == 8);
  z_result c = z_collect (pg, b.slot, 4, {});
  CHECK (c.code == S_END);
  CHECK (c.n == 2);
}

TEST_CASE ("a relocated record ends the chunk before it; the next call collects nothing and answers S_DOESNT_FIT",
	   "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  z_page pg = z_make_page (10);
  z_at (pg, 5)->type = REC_RELOCATION;
  z_result a = z_collect (pg, 0, 64, {});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.n == 4);
  CHECK (a.slot == 4);
  CHECK (a.rows == std::vector < int > ({1, 2, 3, 4}));
  z_result b = z_collect (pg, a.slot, 64, {});
  CHECK (b.code == S_DOESNT_FIT);
  CHECK (b.n == 0);
  CHECK (b.slot == 4);
  CHECK (b.rowpath_records == 6);	/* slots 5..10 go to the row path, the relocated one among them */
}

TEST_CASE ("an overflow record (REC_BIGONE) first on the page: S_DOESNT_FIT, nothing collected", "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  z_page pg = z_make_page (10);
  z_at (pg, 1)->type = REC_BIGONE;
  z_result a = z_collect (pg, 0, 64, {});
  CHECK (a.code == S_DOESNT_FIT);
  CHECK (a.n == 0);
  CHECK (a.slot == 0);
  CHECK (a.rowpath_records == 10);
}

TEST_CASE ("MVCC: TOO_OLD and TOO_NEW without a previous version are skipped; TOO_NEW with one ends the chunk",
	   "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  z_page pg = z_make_page (6);
  z_at (pg, 1)->hdr.vis = Z_SAT;
  z_at (pg, 2)->hdr.vis = Z_OLD;
  z_at (pg, 3)->hdr.vis = Z_NEW;
  z_at (pg, 4)->hdr.vis = Z_SAT;
  z_at (pg, 5)->hdr.vis = Z_NEW_PREV;
  z_at (pg, 6)->hdr.vis = Z_SAT;
  z_result a = z_collect (pg, 0, 64, {});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.rows == std::vector < int > ({1, 4}));
  CHECK (a.slot == 4);
  z_result b = z_collect (pg, a.slot, 64, {});
  CHECK (b.code == S_DOESNT_FIT);
  CHECK (b.n == 0);
}

TEST_CASE ("a visible record of a representation the caller does not decode ends the chunk and is reported",
	   "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  z_page pg = z_make_page (6, 1);
  z_at (pg, 4)->hdr.repid = 2;
  z_result a = z_collect (pg, 0, 64, {1});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.n == 3);
  CHECK (a.slot == 3);
  CHECK (a.unknown == 2);
  z_result b = z_collect (pg, a.slot, 64, {1});
  CHECK (b.code == S_DOESNT_FIT);
  CHECK (b.unknown == 2);
  /* learned: the same walk goes on through it */
  z_result c = z_collect (pg, a.slot, 64, {1, 2});
  CHECK (c.code == S_END);
  CHECK (c.rows == std::vector < int > ({4, 5, 6}));
  CHECK (c.unknown == NULL_REPRID);
}

TEST_CASE ("new-home, assign-address and unknown slots are skipped, not exceptions", "[contract]")
{
  z_page pg = z_make_page (6);
  z_at (pg, 2)->type = REC_NEWHOME;
  z_at (pg, 3)->type = REC_ASSIGN_ADDRESS;
  z_at (pg, 5)->type = REC_UNKNOWN;
  z_result a = z_collect (pg, 0, 64, {});
  CHECK (a.code == S_END);
  CHECK (a.rows == std::vector < int > ({1, 4, 6}));
}

TEST_CASE ("Z-01 counts: one relocated record at slot s of a 100-row page -- the batch takes the s-1 rows before "
	   "it, the row path the 101-s records from it", "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  for (int s : {2, 50, 100})
    {
      z_page pg = z_make_page (100);
      z_at (pg, s)->type = REC_RELOCATION;
      z_result a = z_collect (pg, 0, 1000, {});
      CHECK (a.code == S_SUCCESS);
      CHECK (a.n == s - 1);
      z_result b = z_collect (pg, a.slot, 1000, {});
      CHECK (b.code == S_DOESNT_FIT);
      CHECK (b.rowpath_records == 101 - s);
    }
}

TEST_CASE ("Z-01 counts: two relocated records (slots 30 and 60) -- the batch takes 29 rows, the row path 71 "
	   "records; the second record does not matter", "[contract]")
{
  if (z_new == NULL)
    {
      SUCCEED ("462e6739d and earlier: the exception list -- see the before/after case");
      return;
    }
  z_page pg = z_make_page (100);
  z_at (pg, 30)->type = REC_RELOCATION;
  z_at (pg, 60)->type = REC_RELOCATION;
  z_result a = z_collect (pg, 0, 1000, {});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.n == 29);
  z_result b = z_collect (pg, a.slot, 1000, {});
  CHECK (b.code == S_DOESNT_FIT);
  CHECK (b.rowpath_records == 71);
}

TEST_CASE ("Z-01 before/after: one relocated record at slot 5 of a 10-row page -- 462e6739d keeps 9 rows in the batch "
	   "and lists 1 for an OID fetch; 927a44c04 and later keep 4 and hand the 6 records from slot 5 to the row path",
	   "[contract]")
{
  z_page pg = z_make_page (10);
  z_at (pg, 5)->type = REC_RELOCATION;
  z_result a = z_collect (pg, 0, 64, {});
  if (z_new == NULL)
    {
      CHECK (a.code == S_END);
      CHECK (a.n == 9);
      CHECK (a.n_exc == 1);
    }
  else
    {
      CHECK (a.code == S_SUCCESS);
      CHECK (a.n == 4);
      z_result b = z_collect (pg, a.slot, 64, {});
      CHECK (b.code == S_DOESNT_FIT);
      CHECK (b.rowpath_records == 6);
    }
}
