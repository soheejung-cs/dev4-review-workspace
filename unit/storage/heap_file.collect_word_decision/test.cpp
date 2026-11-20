/* PR#8614 (CBRD-28188) -- heap_page_collect_visible (), src/storage/heap_file.c: the first-word decision.
 *
 * The PR answers most REC_HOME records from their first word (representation + MVCC flags) before the header is
 * decoded:
 *  - the word has neither INSID nor DELID and its representation is the one the walk expects (repids[0], then the
 *    last row's): a row, no snapshot_fnc ();
 *  - the page precedes the snapshot -- slot 0 is a HEAP_CHAIN (length == sizeof (HEAP_CHAIN)), the snapshot function
 *    is mvcc_satisfies_snapshot and MVCC_ID_PRECEDES (chain->max_mvccid, snapshot->lowest_active_mvccid) -- then
 *    an INSID-only word of the expected representation is a row and a DELID word is skipped, no snapshot_fnc ();
 *  - any other record is decided from its header as before (snapshot_fnc () for a non all-visible header, then the
 *    representation search).
 * The base (406d2d87a) decodes every header and calls snapshot_fnc () for each non all-visible one: the outcome is the
 * same, the snapshot_fnc () count is not.  Cases that check the count say so; they FAIL on the base by design.
 *
 * Fakes (each read from the real code it stands for):
 *  - mvcc_satisfies_snapshot (): the real symbol (mvcc.c is not linked), so `snapshot_fnc == mvcc_satisfies_snapshot`
 *    is the real comparison.  It follows mvcc.c mvcc_satisfies_snapshot () branch by branch, with
 *    mvcc_is_id_in_snapshot () for a snapshot whose highest_completed_mvccid == lowest_active_mvccid (an id is in the
 *    snapshot iff id >= lowest_active_mvccid, no active-list lookup) and a transaction with no MVCCID of its own
 *    (MVCC_IS_REC_INSERTED_BY_ME / DELETED_BY_ME never true).  It counts its calls.
 *  - w_other_snapshot (): the same answers under another address (a snapshot function that is not
 *    mvcc_satisfies_snapshot, e.g. mvcc_satisfies_dirty), counted apart.
 *  - spage_get_record (): slot 0 only -- the page's chain bytes (or HEAP_HDR_STATS bytes on the heap header page),
 *    PEEK, S_SUCCESS (slotted_page.c returns the slot's record as it is).
 *  - spage_next_record (): the next record whose slot is above *slotid, S_END past the last (slotted_page.c walk).
 *  - pgbuf_get_vpid_ptr (): the fake page's VPID, equal to the one asked (the watcher already holds the page).
 *  - mvcc_header_size_lookup[8]: object_representation.c's table, copied (object_representation.c is not linked).
 *  - oid_Root_class_oid: an OID on another page.
 * Record bytes are a real MVCC header: OR_PUT_INT (repid | flags << 24), OR_PUT_INT (chn), OR_PUT_BIGINT (insid)
 * and (delid) where the flags say, the previous version's LOG_LSA raw (heap_page_row_mvcc_header () reads it so),
 * then 8 bytes of payload.  The header is decoded by the TU's own heap_page_row_mvcc_header ().
 * MVCC_SNAPSHOT is never constructed (no mvcc.c): lowest_active_mvccid, highest_completed_mvccid and snapshot_fnc
 * are written, the members the collector and the fake read.
 * Release flags (-DNDEBUG): heap_page_check_word_decision () and the or_mvcc_get_header () cross-check are compiled
 * out.  Under a debug build the fake snapshot answers what the word decided, so the debug asserts hold as well
 * (perfmon_is_perf_tracking_and_active () reads a zero global: false). */
#include "catch.hpp"

#include "heap_file.h"
#include "slotted_page.h"
#include "page_buffer.h"
#include "object_representation.h"
#include "object_representation_constants.h"
#include "mvcc.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

/* hooks.cpp (appended to heap_file.c) */
extern size_t w_heap_chain_size (void);
extern size_t w_heap_hdr_stats_size (void);
extern void w_heap_chain_fill (void *buf, MVCCID max_mvccid, bool bestspace);

namespace
{
  const MVCCID W_LOWEST = 1000;	/* the snapshot's lowest active MVCCID */
  const int W_INS = OR_MVCC_FLAG_VALID_INSID;
  const int W_DEL = OR_MVCC_FLAG_VALID_DELID;
  const int W_PREV = OR_MVCC_FLAG_VALID_PREV_VERSION;

  int w_calls;			/* mvcc_satisfies_snapshot () */
  int w_other_calls;		/* w_other_snapshot () */

  struct w_rec
  {
    PGSLOTID slot;
    INT16 type;
    int hdr_len;		/* the header bytes written: what OR_HEADER_SIZE () of the record is */
    std::vector < char >bytes;
  };

  struct w_page
  {
    VPID vpid;
    std::vector < char >slot0;
    std::vector < w_rec > recs;
  };

  /* an ordinary page whose chain says max_mvccid; header_page: the heap header page (slot 0 = HEAP_HDR_STATS, zeros --
   * misread as a chain it would say max_mvccid 0, which precedes any snapshot) */
  w_page w_make_page (MVCCID max_mvccid, bool bestspace = false, bool header_page = false)
  {
    w_page p;

    p.vpid.volid = 0;
    p.vpid.pageid = 77;
    if (header_page)
      {
	p.slot0.assign (w_heap_hdr_stats_size (), 0);
      }
    else
      {
	p.slot0.assign (w_heap_chain_size (), 0);
	w_heap_chain_fill (p.slot0.data (), max_mvccid, bestspace);
      }
    return p;
  }

  /* a REC_HOME record with a real MVCC header */
  void w_add (w_page & p, int repid, int flags, MVCCID insid = 0, MVCCID delid = 0, INT16 type = REC_HOME)
  {
    w_rec r;
    char buf[64];
    char *q = buf;

    memset (buf, 0x5c, sizeof (buf));
    r.slot = (PGSLOTID) (p.recs.size () + 1);
    r.type = type;
    OR_PUT_INT (q, (repid & OR_MVCC_REPID_MASK) | (flags << OR_MVCC_FLAG_SHIFT_BITS));
    q += OR_INT_SIZE;
    OR_PUT_INT (q, 7);		/* CHN */
    q += OR_INT_SIZE;
    if (flags & W_INS)
      {
	INT64 v = (INT64) insid;
	OR_PUT_BIGINT (q, &v);
	q += OR_MVCCID_SIZE;
      }
    if (flags & W_DEL)
      {
	INT64 v = (INT64) delid;
	OR_PUT_BIGINT (q, &v);
	q += OR_MVCCID_SIZE;
      }
    if (flags & W_PREV)
      {
	LOG_LSA lsa (12, 34);
	memcpy (q, &lsa, sizeof (lsa));
	q += OR_MVCC_PREV_VERSION_LSA_SIZE;
      }
    r.hdr_len = (int) (q - buf);
    r.bytes.assign (buf, q + 8);
    p.recs.push_back (r);
  }

  /* mvcc_satisfies_snapshot () for this test's snapshots (see the head comment) */
  bool w_in_snapshot (MVCCID id, const MVCC_SNAPSHOT * s)
  {
    return !MVCC_ID_PRECEDES (id, s->lowest_active_mvccid);
  }

  MVCC_SATISFIES_SNAPSHOT_RESULT w_answer (const MVCC_REC_HEADER * h, const MVCC_SNAPSHOT * s)
  {
    if (!MVCC_IS_HEADER_DELID_VALID (h))
      {
	if (!MVCC_IS_FLAG_SET (h, OR_MVCC_FLAG_VALID_INSID))
	  {
	    return SNAPSHOT_SATISFIED;
	  }
	return w_in_snapshot (h->mvcc_ins_id, s) ? TOO_NEW_FOR_SNAPSHOT : SNAPSHOT_SATISFIED;
      }
    if (w_in_snapshot (h->mvcc_ins_id, s))
      {
	return TOO_NEW_FOR_SNAPSHOT;
      }
    return w_in_snapshot (h->mvcc_del_id, s) ? SNAPSHOT_SATISFIED : TOO_OLD_FOR_SNAPSHOT;
  }

  MVCC_SATISFIES_SNAPSHOT_RESULT w_other_snapshot (THREAD_ENTRY *, MVCC_REC_HEADER * h, MVCC_SNAPSHOT * s)
  {
    w_other_calls++;
    return w_answer (h, s);
  }

  struct w_out
  {
    SCAN_CODE code;
    int n;
    PGSLOTID slot;
    REPR_ID unknown;
    std::vector < int >rows;
    std::vector < int >hdr;	/* rows[].hdr_size */
    std::vector < int >want_hdr;	/* the header bytes the test wrote for each row */
    int calls;			/* snapshot_fnc () calls, whichever function */
  };

  w_out w_collect (w_page & pg, PGSLOTID slot, int cap, const std::vector < REPR_ID > &repids, bool other_fnc = false,
		   MVCCID lowest = W_LOWEST);
}

/* ---- fakes ---- */

MVCC_SATISFIES_SNAPSHOT_RESULT
mvcc_satisfies_snapshot (THREAD_ENTRY * thread_p, MVCC_REC_HEADER * rec_header, MVCC_SNAPSHOT * snapshot)
{
  (void) thread_p;
  w_calls++;
  return w_answer (rec_header, snapshot);
}

int mvcc_header_size_lookup[8] = {
  OR_MVCC_REP_SIZE + OR_CHN_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE + OR_MVCCID_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCC_PREV_VERSION_LSA_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE + OR_MVCC_PREV_VERSION_LSA_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE + OR_MVCC_PREV_VERSION_LSA_SIZE,
  OR_MVCC_REP_SIZE + OR_CHN_SIZE + OR_MVCCID_SIZE + OR_MVCCID_SIZE + OR_MVCC_PREV_VERSION_LSA_SIZE
};

OID w_root_oid = { 9, 9, 0 };
OID *oid_Root_class_oid = &w_root_oid;

SCAN_CODE
spage_get_record (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, PGSLOTID slotid, RECDES * recdes, int ispeeking)
{
  w_page *pg = (w_page *) (void *) pgptr;

  (void) thread_p;
  (void) ispeeking;
  REQUIRE (slotid == HEAP_HEADER_AND_CHAIN_SLOTID);
  recdes->data = pg->slot0.data ();
  recdes->length = (int) pg->slot0.size ();
  recdes->area_size = -1;
  recdes->type = REC_HOME;
  return S_SUCCESS;
}

SCAN_CODE
spage_next_record (PAGE_PTR pgptr, PGSLOTID * slotid, RECDES * recdes, int ispeeking)
{
  w_page *pg = (w_page *) (void *) pgptr;

  (void) ispeeking;
  for (auto & r:pg->recs)
    {
      if (r.slot > *slotid)
	{
	  *slotid = r.slot;
	  recdes->type = r.type;
	  recdes->data = r.bytes.data ();
	  recdes->length = (int) r.bytes.size ();
	  recdes->area_size = -1;
	  return S_SUCCESS;
	}
    }
  return S_END;
}

VPID *
pgbuf_get_vpid_ptr (PAGE_PTR pgptr)
{
  return &((w_page *) (void *) pgptr)->vpid;
}

namespace
{
  w_out w_collect (w_page & pg, PGSLOTID slot, int cap, const std::vector < REPR_ID > &repids, bool other_fnc,
		   MVCCID lowest)
  {
    static unsigned char snap_mem[sizeof (MVCC_SNAPSHOT) + 64];
    MVCC_SNAPSHOT *snap = (MVCC_SNAPSHOT *) (void *) (((uintptr_t) snap_mem + 63) & ~(uintptr_t) 63);
    HEAP_SCANCACHE sc;
    std::vector < HEAP_PAGE_ROW > rows ((size_t) cap);
    VPID vpid = pg.vpid;
    OID cls;
    w_out r;

    memset (&sc, 0, sizeof (sc));
    snap->lowest_active_mvccid = lowest;
    snap->highest_completed_mvccid = lowest;
    snap->snapshot_fnc = other_fnc ? w_other_snapshot : mvcc_satisfies_snapshot;
    sc.mvcc_snapshot = snap;
    sc.mvcc_disabled_class = false;
    sc.page_watcher.pgptr = (PAGE_PTR) (void *) &pg;
    cls.volid = 0;
    cls.pageid = 5;
    cls.slotid = 1;
    r.slot = slot;
    r.n = -1;
    r.unknown = NULL_REPRID;
    w_calls = 0;
    w_other_calls = 0;
    r.code = heap_page_collect_visible (NULL, &vpid, &cls, &sc, &r.slot, rows.data (), cap, &r.n,
					repids.empty ()? NULL : repids.data (), (int) repids.size (), &r.unknown);
    r.calls = w_calls + w_other_calls;
    for (int i = 0; i < r.n; i++)
      {
	r.rows.push_back (rows[i].oid.slotid);
	r.hdr.push_back (rows[i].hdr_size);
	for (auto & rec:pg.recs)
	  {
	    if (rec.slot == rows[i].oid.slotid)
	      {
		r.want_hdr.push_back (rec.hdr_len);
	      }
	  }
      }
    return r;
  }

  typedef std::vector < int >ints;
}

/* ---- 1. an ordinary page (max MVCCID not before the snapshot) ---- */

TEST_CASE ("C1 ordinary page: a record without MVCC ids of repids[0] is a row with no snapshot_fnc (); an INSID "
	   "record goes to snapshot_fnc () and is a row when committed before the snapshot, a DELID one is skipped "
	   "when its delete committed before it (snapshot_fnc () counts: same on the base)", "[contract]")
{
  w_page pg = w_make_page (2000);	/* max 2000 >= lowest 1000 */

  w_add (pg, 1, 0);		/* slot 1: no MVCC id */
  w_out a = w_collect (pg, 0, 64, {1});
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1}));
  CHECK (a.calls == 0);

  w_add (pg, 1, W_INS, 500);	/* slot 2: inserted by 500 (committed) */
  w_add (pg, 1, W_INS | W_DEL, 400, 600);	/* slot 3: deleted by 600 (committed) */
  w_add (pg, 1, W_INS, 1500);	/* slot 4: inserted by 1500 (active): TOO_NEW, no previous version */
  w_add (pg, 1, 0);		/* slot 5 */
  w_out b = w_collect (pg, 0, 64, {1});
  CHECK (b.code == S_END);
  CHECK (b.rows == ints ({1, 2, 5}));
  CHECK (b.calls == 3);		/* slots 2, 3, 4 */

  /* no repids: the representation is not looked at, the flags still are */
  w_out c = w_collect (pg, 0, 64, {});
  CHECK (c.rows == ints ({1, 2, 5}));
  CHECK (c.calls == 3);
}

/* ---- 2. a page that precedes the snapshot ---- */

TEST_CASE ("C2 page precedes the snapshot (max 900 < lowest 1000): an INSID-only record is a row and a DELID record "
	   "is skipped, neither through snapshot_fnc () (base: one call each)", "[contract]")
{
  w_page pg = w_make_page (900);

  w_add (pg, 1, W_INS, 500);	/* slot 1: row */
  w_add (pg, 1, W_INS | W_DEL, 400, 600);	/* slot 2: deleted -- skipped */
  w_add (pg, 1, W_DEL, 0, 700);	/* slot 3: DELID only (insert all-visible) -- skipped */
  w_add (pg, 1, W_INS | W_PREV, 800);	/* slot 4: an update's new version, committed -- row */
  w_add (pg, 1, 0);		/* slot 5: row */
  w_add (pg, 1, W_INS | W_DEL | W_PREV, 300, 850);	/* slot 6: deleted -- skipped */
  w_out a = w_collect (pg, 0, 64, {1});
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1, 4, 5}));
  CHECK (a.calls == 0);

  w_out b = w_collect (pg, 0, 64, {});
  CHECK (b.rows == ints ({1, 4, 5}));
  CHECK (b.calls == 0);
}

TEST_CASE ("C2b the same records on an ordinary page give the same rows through snapshot_fnc (): the word decision "
	   "changes the call count, not the outcome", "[contract]")
{
  w_page pg = w_make_page (1000);	/* max == lowest: ordinary */

  w_add (pg, 1, W_INS, 500);
  w_add (pg, 1, W_INS | W_DEL, 400, 600);
  w_add (pg, 1, W_DEL, 0, 700);
  w_add (pg, 1, W_INS | W_PREV, 800);
  w_add (pg, 1, 0);
  w_add (pg, 1, W_INS | W_DEL | W_PREV, 300, 850);
  w_out a = w_collect (pg, 0, 64, {1});
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1, 4, 5}));
  CHECK (a.calls == 5);
}

/* ---- 3. where the page decision does not hold ---- */

TEST_CASE ("C3a boundary: max MVCCID == lowest active does not precede (MVCC_ID_PRECEDES is strict): the INSID "
	   "record goes to snapshot_fnc (); max == lowest - 1 does", "[contract]")
{
  w_page eq = w_make_page (W_LOWEST);
  w_add (eq, 1, W_INS, 500);
  w_out a = w_collect (eq, 0, 64, {1});
  CHECK (a.rows == ints ({1}));
  CHECK (a.calls == 1);

  w_page below = w_make_page (W_LOWEST - 1);
  w_add (below, 1, W_INS, 500);
  w_out b = w_collect (below, 0, 64, {1});
  CHECK (b.rows == ints ({1}));
  CHECK (b.calls == 0);		/* base: 1 */
}

TEST_CASE ("C3b a snapshot function other than mvcc_satisfies_snapshot never takes the page decision: on a page "
	   "that precedes the snapshot the INSID and DELID records go to it", "[contract]")
{
  w_page pg = w_make_page (900);

  w_add (pg, 1, W_INS, 500);
  w_add (pg, 1, W_INS | W_DEL, 400, 600);
  w_add (pg, 1, 0);
  w_out a = w_collect (pg, 0, 64, {1}, true);
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1, 3}));
  CHECK (a.calls == 2);
  CHECK (w_other_calls == 2);
  CHECK (w_calls == 0);
}

TEST_CASE ("C3c the heap header page (slot 0 is HEAP_HDR_STATS, its length not sizeof (HEAP_CHAIN)) is never judged: "
	   "its zero bytes read as a chain would say max 0 < lowest, yet the INSID record goes to snapshot_fnc ()",
	   "[contract]")
{
  REQUIRE (w_heap_hdr_stats_size () != w_heap_chain_size ());
  w_page pg = w_make_page (0, false, true);

  w_add (pg, 1, W_INS, 500);
  w_add (pg, 1, W_INS | W_DEL, 400, 600);
  w_add (pg, 1, 0);
  w_out a = w_collect (pg, 0, 64, {1});
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1, 3}));
  CHECK (a.calls == 2);
}

TEST_CASE ("C3d a chain with the bestspace flag: S_END, nothing collected, no snapshot_fnc ()", "[contract]")
{
  w_page pg = w_make_page (900, true);

  w_add (pg, 1, W_INS, 500);
  w_add (pg, 1, 0);
  w_out a = w_collect (pg, 0, 64, {1});
  CHECK (a.code == S_END);
  CHECK (a.n == 0);
  CHECK (a.calls == 0);
}

/* ---- 4. representations ---- */

TEST_CASE ("C4a another representation inside repids is a row after its header is read; the walk then expects it: "
	   "a record of repids[0] after it is found by the search again", "[contract]")
{
  w_page pg = w_make_page (2000);

  w_add (pg, 1, 0);		/* slot 1: repids[0] */
  w_add (pg, 2, 0);		/* slot 2: repids[1] -- header, search */
  w_add (pg, 2, 0);		/* slot 3: the word now answers */
  w_add (pg, 1, 0);		/* slot 4: back to repids[0] -- header, search */
  w_add (pg, 2, W_INS, 500);	/* slot 5 */
  REPR_ID unk = NULL_REPRID;
  w_out a = w_collect (pg, 0, 64, {1, 2});
  CHECK (a.code == S_END);
  CHECK (a.rows == ints ({1, 2, 3, 4, 5}));
  CHECK (a.unknown == unk);
  CHECK (a.calls == 1);		/* slot 5 */
}

TEST_CASE ("C4b a visible record of a representation outside repids ends the chunk (is_exception): first on the page "
	   "S_DOESNT_FIT, after rows S_SUCCESS at the slot before it; *unknown_repid gets it", "[contract]")
{
  w_page first = w_make_page (2000);
  w_add (first, 3, 0);
  w_add (first, 1, 0);
  w_out a = w_collect (first, 0, 64, {1, 2});
  CHECK (a.code == S_DOESNT_FIT);
  CHECK (a.n == 0);
  CHECK (a.slot == 0);
  CHECK (a.unknown == 3);

  w_page later = w_make_page (2000);
  w_add (later, 1, 0);
  w_add (later, 2, 0);
  w_add (later, 3, W_INS, 500);	/* visible (committed) -- then the representation */
  w_add (later, 1, 0);
  w_out b = w_collect (later, 0, 64, {1, 2});
  CHECK (b.code == S_SUCCESS);
  CHECK (b.rows == ints ({1, 2}));
  CHECK (b.slot == 2);
  CHECK (b.unknown == 3);
  CHECK (b.calls == 1);
}

TEST_CASE ("C4c on a page that precedes the snapshot the representation still counts for rows: an INSID record of "
	   "repids[1] is read from its header (snapshot_fnc () once), one outside repids ends the chunk; a DELID "
	   "record outside repids is skipped by its word (deleted, not an exception -- the base skips it as TOO_OLD)",
	   "[contract]")
{
  w_page pg = w_make_page (900);

  w_add (pg, 1, W_INS, 500);	/* slot 1: word answers */
  w_add (pg, 3, W_INS | W_DEL, 400, 600);	/* slot 2: deleted, repid 3 -- skipped by the word */
  w_add (pg, 2, W_INS, 500);	/* slot 3: repids[1] -- header, snapshot_fnc (), search */
  w_add (pg, 2, W_INS, 510);	/* slot 4: word answers (repid 2 now expected) */
  w_add (pg, 3, W_INS, 520);	/* slot 5: outside -- exception */
  w_out a = w_collect (pg, 0, 64, {1, 2});
  CHECK (a.code == S_SUCCESS);
  CHECK (a.rows == ints ({1, 3, 4}));
  CHECK (a.slot == 4);
  CHECK (a.unknown == 3);
  CHECK (a.calls == 2);		/* slots 3 and 5 (base: 5 -- every record has an MVCC id) */
}

/* ---- 5. hdr_size ---- */

TEST_CASE ("C5 rows[].hdr_size is the header the first word's flags describe -- the bytes the test wrote: 8, 16, 24, "
	   "16 (PREV only), 24 (INSID+PREV), 32 (INSID+DELID+PREV, deleter active) -- on both page kinds",
	   "[contract]")
{
  for (MVCCID max : {(MVCCID) 900, (MVCCID) 2000})
    {
      w_page pg = w_make_page (max);

      w_add (pg, 1, 0);
      w_add (pg, 1, W_INS, 500);
      w_add (pg, 1, W_PREV);
      w_add (pg, 1, W_INS | W_PREV, 500);
      if (max >= W_LOWEST)
	{
	  /* deleted by an active transaction: still visible (SNAPSHOT_SATISFIED) -- only on an ordinary page */
	  w_add (pg, 1, W_INS | W_DEL, 500, 1500);
	  w_add (pg, 1, W_INS | W_DEL | W_PREV, 500, 1600);
	}
      w_add (pg, 2, W_INS, 500);	/* another representation, header read */
      w_out a = w_collect (pg, 0, 64, {1, 2});
      CHECK (a.code == S_END);
      CHECK (a.n == (int) pg.recs.size ());
      CHECK (a.hdr == a.want_hdr);
      CHECK (a.hdr.size () == pg.recs.size ());
    }
}

/* ---- properties over generated pages ---- */

namespace
{
  /* the row path's answer, record by record (heap_next_1page () / heap_scan_get_visible_version () as the collector's
   * head comment states it): skipped slot types, non-REC_HOME = the chunk ends, snapshot_fnc () for a non all-visible
   * header -- TOO_OLD skipped, TOO_NEW skipped without a previous version and the chunk ends with one -- then a
   * representation outside repids ends the chunk */
  w_out w_reference (w_page & pg, PGSLOTID slot, int cap, const std::vector < REPR_ID > &repids, int *calls_needed)
  {
    static unsigned char snap_mem[sizeof (MVCC_SNAPSHOT) + 64];
    MVCC_SNAPSHOT *snap = (MVCC_SNAPSHOT *) (void *) (((uintptr_t) snap_mem + 63) & ~(uintptr_t) 63);
    w_out r;

    snap->lowest_active_mvccid = W_LOWEST;
    snap->highest_completed_mvccid = W_LOWEST;
    r.n = 0;
    r.unknown = NULL_REPRID;
    r.slot = slot;
    *calls_needed = 0;
    for (auto & rec:pg.recs)
      {
	if (rec.slot <= slot)
	  {
	    continue;
	  }
	if (rec.type == REC_NEWHOME || rec.type == REC_ASSIGN_ADDRESS || rec.type == REC_UNKNOWN)
	  {
	    continue;
	  }
	bool exc = (rec.type != REC_HOME);
	if (!exc)
	  {
	    const char *p = rec.bytes.data ();
	    int w = OR_GET_INT (p);
	    MVCC_REC_HEADER h = MVCC_REC_HEADER_INITIALIZER;
	    int off = 8;

	    h.repid = w & OR_MVCC_REPID_MASK;
	    h.mvcc_flag = (char) ((w >> OR_MVCC_FLAG_SHIFT_BITS) & OR_MVCC_FLAG_MASK);
	    h.mvcc_ins_id = MVCCID_ALL_VISIBLE;
	    if (h.mvcc_flag & W_INS)
	      {
		INT64 v;
		OR_GET_BIGINT (p + off, &v);
		h.mvcc_ins_id = (MVCCID) v;
		off += 8;
	      }
	    if (h.mvcc_flag & W_DEL)
	      {
		INT64 v;
		OR_GET_BIGINT (p + off, &v);
		h.mvcc_del_id = (MVCCID) v;
	      }
	    if (!MVCC_IS_HEADER_ALL_VISIBLE (&h))
	      {
		MVCC_SATISFIES_SNAPSHOT_RESULT res = w_answer (&h, snap);

		(*calls_needed)++;
		if (res == TOO_OLD_FOR_SNAPSHOT)
		  {
		    continue;
		  }
		if (res != SNAPSHOT_SATISFIED)
		  {
		    if (!(h.mvcc_flag & W_PREV))
		      {
			continue;
		      }
		    exc = true;
		  }
	      }
	    if (!exc && !repids.empty ())
	      {
		bool found = false;
		for (REPR_ID id:repids)
		  {
		    found = found || (id == h.repid);
		  }
		if (!found)
		  {
		    exc = true;
		    r.unknown = h.repid;
		  }
	      }
	  }
	if (exc)
	  {
	    r.slot = rec.slot - 1;
	    r.code = r.n > 0 ? S_SUCCESS : S_DOESNT_FIT;
	    return r;
	  }
	r.rows.push_back (rec.slot);
	r.n++;
	if (r.n == cap)
	  {
	    r.slot = rec.slot;
	    r.code = S_SUCCESS;
	    return r;
	  }
      }
    r.code = S_END;
    return r;
  }

  /* a generated page: precedes = every id below max < lowest; otherwise ids on both sides of lowest and max = the
   * largest id or more.  one_repid: every record of repid 1 */
  w_page w_generate (std::mt19937 & g, bool precedes, bool one_repid, int n)
  {
    std::uniform_int_distribution < int >pick (0, 99);
    MVCCID top = precedes ? 900 : 1900;
    std::uniform_int_distribution < int >idd (4, (int) top - 1);
    w_page pg = w_make_page (top);

    for (int i = 0; i < n; i++)
      {
	int k = pick (g);
	int flags = 0;
	int repid = one_repid ? 1 : 1 + (pick (g) % 4);	/* 4 = outside {1, 2, 3} */
	INT16 type = REC_HOME;

	if (k < 25)
	  {
	    flags = 0;
	  }
	else if (k < 50)
	  {
	    flags = W_INS;
	  }
	else if (k < 65)
	  {
	    flags = W_INS | W_DEL;
	  }
	else if (k < 75)
	  {
	    flags = W_INS | W_PREV;
	  }
	else if (k < 82)
	  {
	    flags = W_DEL;
	  }
	else if (k < 88)
	  {
	    flags = W_INS | W_DEL | W_PREV;
	  }
	else if (k < 92)
	  {
	    flags = W_PREV;
	  }
	else if (k < 95)
	  {
	    type = (pick (g) & 1) ? REC_NEWHOME : REC_ASSIGN_ADDRESS;
	  }
	else if (k < 97)
	  {
	    type = REC_RELOCATION;
	  }
	else
	  {
	    flags = W_INS;
	  }
	MVCCID a = (MVCCID) idd (g), b = (MVCCID) idd (g);
	MVCCID ins = a < b ? a : b, del = a < b ? b : a;
	w_add (pg, repid, flags, ins, del, type);
      }
    return pg;
  }
}

TEST_CASE ("P1 generated pages (2,000; both page kinds; one or several representations; chunks of 1..64): chunk by "
	   "chunk the collector answers what the row path's record-by-record decision answers -- code, rows, slot, "
	   "unknown representation (outcome: same on the base)", "[contract]")
{
  std::mt19937 g (8614);
  int mismatches = 0;

  for (int t = 0; t < 2000; t++)
    {
      bool precedes = (t & 1) != 0;
      bool one_repid = (t & 2) != 0;
      w_page pg = w_generate (g, precedes, one_repid, 1 + (int) (g () % 80));
      std::vector < REPR_ID > repids = one_repid ? std::vector < REPR_ID > ({1}) : std::vector < REPR_ID > ({1, 2, 3});
      int cap = 1 + (int) (g () % 64);
      PGSLOTID slot = 0;

      for (int step = 0; step < 200; step++)
	{
	  int need;
	  w_out want = w_reference (pg, slot, cap, repids, &need);
	  w_out got = w_collect (pg, slot, cap, repids);
	  bool same = got.code == want.code && got.rows == want.rows && got.unknown == want.unknown
	    && (got.code == S_END || got.slot == want.slot) && got.hdr == got.want_hdr;

	  if (!same)
	    {
	      mismatches++;
	      break;
	    }
	  if (got.code != S_SUCCESS)
	    {
	      break;
	    }
	  /* the next chunk from where this one ended (S_DOESNT_FIT when an exception ended it, checked as well) */
	  slot = got.slot;
	}
    }
  CHECK (mismatches == 0);
}

TEST_CASE ("P2 generated pages of one representation that precede the snapshot: snapshot_fnc () is never called; on "
	   "ordinary pages it is called exactly as often as the row path decodes a non all-visible header (base: "
	   "that often on both kinds)", "[contract]")
{
  std::mt19937 g (28188);
  int called_on_precedes = 0, differs = 0, pages_with_ids = 0;

  for (int t = 0; t < 2000; t++)
    {
      bool precedes = (t & 1) != 0;
      w_page pg = w_generate (g, precedes, true, 1 + (int) (g () % 80));
      int need;
      w_out want = w_reference (pg, 0, 1000, {1}, &need);
      w_out got = w_collect (pg, 0, 1000, {1});

      REQUIRE (got.rows == want.rows);
      if (precedes)
	{
	  pages_with_ids += (need > 0) ? 1 : 0;
	  called_on_precedes += got.calls;
	}
      else
	{
	  differs += (got.calls != need) ? 1 : 0;
	}
    }
  CHECK (pages_with_ids > 500);	/* the generator does put MVCC ids on such pages */
  CHECK (called_on_precedes == 0);
  CHECK (differs == 0);
}
