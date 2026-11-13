/* PR#8490 (CBRD-28031) 35a151f37 -- scan_index_read_ahead (), src/query/scan_manager.c: which heap pages an index
 * scan tells the OS about, a window ahead of its lookups.
 *
 * The real code under test: scan_index_read_ahead () (the window), call_get_next_index_oidset () (where a new OID
 * buffer starts its windows: scan_manager.c:5156-5169) and scan_next_index_scan () (the serial lookups: a window is
 * told when the lookup is half a window from ahead_next, scan_manager.c:6369-6372).  The last two are static: the
 * seam only unstatics them (no fake).
 *
 * Fakes:
 *  - scan_get_index_oidset (): hands the next scripted OID buffer (oids_count, oid_list->oidp), then none;
 *    BTREE_END_OF_SCAN stays false so the scan asks for the next buffer (scan_manager.c:6265-6327).
 *  - scan_next_index_lookup_heap (): records the lookup (curr_oidno) and qualifies it.
 *  - pgbuf_read_ahead (): records the pages told and the lookup in progress (curr_oidno) when told.
 *  - logtb_find_current_isolation (): read committed.
 * SCAN_ID is calloc'd and never constructed; the scan reads plain fields.
 */
#include "catch.hpp"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "scan_manager.h"
#include "btree.h"
#include "log_impl.h"
#include "page_buffer.h"
#include "thread_entry.hpp"

/* static in scan_manager.c: unstatic'd by the seam */
extern SCAN_CODE scan_next_index_scan (THREAD_ENTRY * thread_p, SCAN_ID * scan_id);
extern SCAN_CODE call_get_next_index_oidset (THREAD_ENTRY * thread_p, SCAN_ID * scan_id, INDX_SCAN_ID * isidp,
					     bool should_go_to_next_value);

namespace sm
{
  struct told
  {
    int curr;			/* the lookup in progress (curr_oidno) */
    std::vector<VPID> vpids;
  };

  std::vector<std::vector<VPID>> buffers;	/* the OID buffers the B-tree hands, each OID's heap page */
  size_t next_buffer;
  std::vector<told> calls;
  std::vector<int> lookups;
  SCAN_ID *sid;
  INDX_INFO *indx;
  BTREE_ISCAN_OID_LIST oid_list;
  OID oid_area[1024];
  cubthread::entry *thr;
  int b_no;			/* lookups so far in the current buffer */

  std::vector<VPID>
  distinct_pages (int n, int base)
  {
    std::vector<VPID> v;
    for (int i = 0; i < n; i++)
      {
	VPID p;
	p.volid = 0;
	p.pageid = base + i;
	v.push_back (p);
      }
    return v;
  }

  void
  setup (std::vector<std::vector<VPID>> bufs, QPROC_SINGLE_FETCH single_fetch)
  {
    if (thr == nullptr)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));
	sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
	indx = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
      }
    buffers = bufs;
    next_buffer = 0;
    calls.clear ();
    lookups.clear ();
    memset (sid, 0, sizeof (SCAN_ID));
    memset (indx, 0, sizeof (INDX_INFO));
    indx->range_type = R_RANGE;
    sid->type = S_INDX_SCAN;
    sid->position = S_BEFORE;
    sid->direction = S_FORWARD;
    sid->single_fetch = single_fetch;
    sid->s.isid.indx_info = indx;
    sid->s.isid.cls_oid.volid = 0;
    sid->s.isid.cls_oid.pageid = 1;
    sid->s.isid.cls_oid.slotid = 1;
    sid->s.isid.key_limit_lower = -1;
    sid->s.isid.key_limit_upper = -1;
    memset (&oid_list, 0, sizeof (oid_list));
    oid_list.oidp = oid_area;
    oid_list.capacity = 1024;
    oid_list.max_oid_cnt = 1024;
    sid->s.isid.oid_list = &oid_list;
    sid->s.isid.ahead_next = INT_MAX;	/* scan_open_index_scan () */
    sid->s.isid.ahead_distance = 0;
  }

  /* the serial scan: scan_next_index_scan () until it ends */
  int
  run ()
  {
    int n = 0;
    SCAN_CODE sc;
    while ((sc = scan_next_index_scan (thr, sid)) == S_SUCCESS)
      {
	n++;
      }
    REQUIRE (sc == S_END);
    return n;
  }

  /* a told window as OID indexes of the buffer [first, last] (by page, pages distinct) */
  struct window
  {
    int curr;
    int first_page;
    int n;
  };

  std::vector<window>
  windows ()
  {
    std::vector<window> v;
    for (const told &t : calls)
      {
	v.push_back ({ t.curr, t.vpids.empty () ? -1 : t.vpids[0].pageid, (int) t.vpids.size () });
      }
    return v;
  }
}

/* ---- fakes ---- */

int
scan_get_index_oidset (THREAD_ENTRY * thread_p, SCAN_ID * s_id, DB_BIGINT * key_limit_upper,
		       DB_BIGINT * key_limit_lower)
{
  INDX_SCAN_ID *isidp = &s_id->s.isid;

  (void) thread_p;
  (void) key_limit_upper;
  (void) key_limit_lower;
  isidp->oids_count = 0;
  if (sm::next_buffer < sm::buffers.size ())
    {
      const std::vector<VPID> &b = sm::buffers[sm::next_buffer++];
      if (isidp->oid_list != nullptr)
	{
	  for (size_t i = 0; i < b.size (); i++)
	    {
	      isidp->oid_list->oidp[i].volid = b[i].volid;
	      isidp->oid_list->oidp[i].pageid = b[i].pageid;
	      isidp->oid_list->oidp[i].slotid = (short) (i + 1);
	    }
	  isidp->oid_list->oid_cnt = (int) b.size ();
	}
      isidp->oids_count = (int) b.size ();
    }
  return NO_ERROR;
}

SCAN_CODE
scan_next_index_lookup_heap (THREAD_ENTRY * thread_p, SCAN_ID * scan_id, INDX_SCAN_ID * isidp,
			     FILTER_INFO * data_filter, TRAN_ISOLATION isolation)
{
  (void) thread_p;
  (void) scan_id;
  (void) data_filter;
  (void) isolation;
  sm::lookups.push_back (isidp->curr_oidno);
  return S_SUCCESS;
}

void
pgbuf_read_ahead (THREAD_ENTRY * thread_p, const VPID * vpids, int count)
{
  sm::told t;

  (void) thread_p;
  t.curr = sm::sid->s.isid.curr_oidno;
  t.vpids.assign (vpids, vpids + count);
  sm::calls.push_back (t);
}

TRAN_ISOLATION
logtb_find_current_isolation (THREAD_ENTRY * thread_p)
{
  (void) thread_p;
  return TRAN_READ_COMMITTED;
}

/* ---- cases ---- */

TEST_CASE ("S1 serial, 600 OIDs on distinct pages: the first window holds 8 OIDs, each next one twice the one before up to 256, told when half the current one is looked up; the last one is cut at the buffer's end", "[contract]")
{
  sm::setup ({ sm::distinct_pages (600, 1000) }, QPROC_NO_SINGLE_INNER);
  REQUIRE (sm::run () == 600);

  /* (lookup in progress, first OID told, OIDs told): windows [0,8) [8,20) [20,44) [44,92) [92,188) [188,380)
   * [380,508) [508,600) -- curr + distance = 8, 16, 32, 64, 128, 256, 256, then the buffer's end */
  std::vector<sm::window> want = {
    {0, 1000, 8}, {4, 1008, 12}, {12, 1020, 24}, {28, 1044, 48}, {60, 1092, 96}, {124, 1188, 192},
    {252, 1380, 128}, {380, 1508, 92}
  };
  std::vector<sm::window> got = sm::windows ();
  REQUIRE (got.size () == want.size ());
  for (size_t i = 0; i < want.size (); i++)
    {
      CAPTURE (i);
      CHECK (got[i].curr == want[i].curr);
      CHECK (got[i].first_page == want[i].first_page);
      CHECK (got[i].n == want[i].n);
    }
}

TEST_CASE ("S2 serial, buffers of 700, 5 and 300 OIDs: every OID's page told once, before or at its lookup, never more than 256 OIDs ahead; each buffer starts its windows over from 8", "[contract]")
{
  sm::setup ({ sm::distinct_pages (700, 10000), sm::distinct_pages (5, 20000), sm::distinct_pages (300, 30000) },
	     QPROC_NO_SINGLE_INNER);
  REQUIRE (sm::run () == 1005);

  std::map<int, int> told_at;	/* page -> lookup in progress when told */
  for (const sm::told &t : sm::calls)
    {
      for (const VPID &v : t.vpids)
	{
	  CHECK (told_at.count (v.pageid) == 0);	/* told once */
	  told_at[v.pageid] = t.curr;
	  int index = v.pageid % 10000;
	  CHECK (index >= t.curr);	/* never a page already looked up */
	  CHECK (index - t.curr < 256);	/* at most 256 OIDs ahead */
	}
    }
  CHECK (told_at.size () == 1005);
  /* the first window of each buffer: 8 OIDs from its first lookup (5 for the 5-OID buffer) */
  int starts = 0;
  for (const sm::told &t : sm::calls)
    {
      if (t.curr == 0)
	{
	  starts++;
	  int expect = (t.vpids[0].pageid / 10000 == 2) ? 5 : 8;
	  CHECK ((int) t.vpids.size () == expect);
	}
    }
  CHECK (starts == 3);
}

TEST_CASE ("S3 QPROC_SINGLE_INNER (NL semi/anti join inner): the first lookup tells nothing; from the second one the windows are 2, 4, 8, ... OIDs from the lookup", "[contract]")
{
  sm::setup ({ sm::distinct_pages (50, 1000) }, QPROC_SINGLE_INNER);
  REQUIRE (call_get_next_index_oidset (sm::thr, sm::sid, &sm::sid->s.isid, true) == S_SUCCESS);
  CHECK (sm::sid->s.isid.ahead_next == 1);
  CHECK (sm::sid->s.isid.ahead_distance == 1);

  /* one lookup (the join matched at once): nothing told */
  sm::setup ({ sm::distinct_pages (50, 1000) }, QPROC_SINGLE_INNER);
  REQUIRE (scan_next_index_scan (sm::thr, sm::sid) == S_SUCCESS);
  CHECK (sm::calls.empty ());

  /* the whole buffer: [1,3) [3,6) [6,12) [12,24) [24,48) [48,50) */
  sm::setup ({ sm::distinct_pages (50, 1000) }, QPROC_SINGLE_INNER);
  REQUIRE (sm::run () == 50);
  std::vector<sm::window> want = { {1, 1001, 2}, {2, 1003, 3}, {4, 1006, 6}, {8, 1012, 12}, {16, 1024, 24},
  {32, 1048, 2}
  };
  std::vector<sm::window> got = sm::windows ();
  REQUIRE (got.size () == want.size ());
  for (size_t i = 0; i < want.size (); i++)
    {
      CAPTURE (i);
      CHECK (got[i].curr == want[i].curr);
      CHECK (got[i].first_page == want[i].first_page);
      CHECK (got[i].n == want[i].n);
    }
}

TEST_CASE ("S4 a window cut short by the buffer's end does not make the next one larger: with more OIDs to come (a worker's buffer) the next window is 16, not 32; without, the buffer's windows end", "[contract]")
{
  INDX_SCAN_ID *isidp;

  sm::setup ({}, QPROC_NO_SINGLE_INNER);
  isidp = &sm::sid->s.isid;
  for (int i = 0; i < 40; i++)
    {
      isidp->oid_list->oidp[i].volid = 0;
      isidp->oid_list->oidp[i].pageid = 1000 + i;
      isidp->oid_list->oidp[i].slotid = 1;
    }
  /* after the first window [0,8): the lookup at 4 asks for a 16-OID window but the buffer holds 10 */
  isidp->oids_count = 10;
  isidp->curr_oidno = 4;
  isidp->ahead_next = 8;
  isidp->ahead_distance = 8;
  scan_index_read_ahead (sm::thr, isidp, true);
  REQUIRE (sm::calls.size () == 1);
  CHECK (sm::calls[0].vpids.size () == 2);	/* [8,10) */
  CHECK (isidp->ahead_next == 10);
  CHECK (isidp->ahead_distance == 8);	/* not 16 */

  /* the worker evaluated more keys: 40 OIDs; the lookup at 6 is half a window (4) from 10 */
  isidp->oids_count = 40;
  isidp->curr_oidno = 6;
  scan_index_read_ahead (sm::thr, isidp, true);
  REQUIRE (sm::calls.size () == 2);
  CHECK (sm::calls[1].vpids.size () == 12);	/* [10,22): 6 + 16 */
  CHECK (sm::calls[1].vpids[0].pageid == 1010);
  CHECK (isidp->ahead_next == 22);
  CHECK (isidp->ahead_distance == 16);

  /* the same cut window in a serial buffer (no more OIDs): nothing more is told for this buffer */
  isidp->oids_count = 10;
  isidp->curr_oidno = 4;
  isidp->ahead_next = 8;
  isidp->ahead_distance = 8;
  scan_index_read_ahead (sm::thr, isidp, false);
  CHECK (isidp->ahead_next == INT_MAX);
  CHECK (isidp->ahead_distance == 8);
}

TEST_CASE ("S5 OIDs on the same page next to each other are told as one page; a page met again after another is told again (current behavior), as is the same page id of another volume", "[contract]")
{
  INDX_SCAN_ID *isidp;
  int pages[8] = { 5, 5, 6, 6, 5, 7, 7, 8 };

  sm::setup ({}, QPROC_NO_SINGLE_INNER);
  isidp = &sm::sid->s.isid;
  for (int i = 0; i < 8; i++)
    {
      isidp->oid_list->oidp[i].volid = (i == 7) ? 1 : 0;
      isidp->oid_list->oidp[i].pageid = pages[i];
      isidp->oid_list->oidp[i].slotid = (short) (i + 1);
    }
  isidp->oid_list->oidp[6].pageid = 8;	/* 5 5 6 6 5 7 8 8(vol 1) */
  isidp->oids_count = 8;
  isidp->curr_oidno = 0;
  isidp->ahead_next = 0;
  isidp->ahead_distance = 0;
  scan_index_read_ahead (sm::thr, isidp, false);
  REQUIRE (sm::calls.size () == 1);
  const std::vector<VPID> &v = sm::calls[0].vpids;
  REQUIRE (v.size () == 6);
  int want[6][2] = { {0, 5}, {0, 6}, {0, 5}, {0, 7}, {0, 8}, {1, 8} };
  for (int i = 0; i < 6; i++)
    {
      CAPTURE (i);
      CHECK (v[i].volid == want[i][0]);
      CHECK (v[i].pageid == want[i][1]);
    }
}

TEST_CASE ("S6 a window whose one page is the page being looked up now is not told (the window still moves on); a one-page window ahead of the lookup, or the lookup's page with another, is told", "[contract]")
{
  INDX_SCAN_ID *isidp;

  sm::setup ({}, QPROC_NO_SINGLE_INNER);
  isidp = &sm::sid->s.isid;
  for (int i = 0; i < 40; i++)
    {
      isidp->oid_list->oidp[i].volid = 0;
      isidp->oid_list->oidp[i].pageid = (i < 8) ? 5 : 9;
      isidp->oid_list->oidp[i].slotid = (short) (i + 1);
    }
  isidp->oids_count = 40;

  /* [0,8) all on page 5, the page of OID 0 being looked up */
  isidp->curr_oidno = 0;
  isidp->ahead_next = 0;
  isidp->ahead_distance = 0;
  scan_index_read_ahead (sm::thr, isidp, false);
  CHECK (sm::calls.empty ());
  CHECK (isidp->ahead_next == 8);
  CHECK (isidp->ahead_distance == 8);

  /* [8,20) all on page 9, ahead of the lookup at 4 */
  isidp->curr_oidno = 4;
  scan_index_read_ahead (sm::thr, isidp, false);
  REQUIRE (sm::calls.size () == 1);
  REQUIRE (sm::calls[0].vpids.size () == 1);
  CHECK (sm::calls[0].vpids[0].pageid == 9);

  /* a fresh window from the lookup at 6 (page 5) that reaches page 9 */
  isidp->curr_oidno = 6;
  isidp->ahead_next = 0;
  isidp->ahead_distance = 0;
  scan_index_read_ahead (sm::thr, isidp, false);
  REQUIRE (sm::calls.size () == 2);
  REQUIRE (sm::calls[1].vpids.size () == 2);
  CHECK (sm::calls[1].vpids[0].pageid == 5);
  CHECK (sm::calls[1].vpids[1].pageid == 9);
}

TEST_CASE ("S7 a new OID buffer starts its windows (ahead_next 0, distance 0) only for 2+ OIDs in an OID list, a heap lookup per OID (not covered), not grouped, not multi-range-optimized, not count-only; otherwise nothing is told (ahead_next INT_MAX)", "[contract]")
{
  struct variant
  {
    const char *name;
    int n_oids;
    int expect_next;
  };
  variant vs[] = {
    {"plain", 5, 0},
    {"one OID", 1, INT_MAX},
    {"covered", 5, INT_MAX},
    {"grouped", 5, INT_MAX},
    {"count only", 5, INT_MAX},
    {"multi range opt", 5, INT_MAX},
    {"no OID list", 5, INT_MAX},
  };
  static QFILE_LIST_ID list_token;

  for (const variant &v : vs)
    {
      CAPTURE (v.name);
      sm::setup ({ sm::distinct_pages (v.n_oids, 1000) }, QPROC_NO_SINGLE_INNER);
      INDX_SCAN_ID *isidp = &sm::sid->s.isid;
      if (strcmp (v.name, "covered") == 0)
	{
	  isidp->indx_cov.list_id = &list_token;
	}
      else if (strcmp (v.name, "grouped") == 0)
	{
	  sm::sid->grouped = 1;
	}
      else if (strcmp (v.name, "count only") == 0)
	{
	  isidp->need_count_only = true;
	}
      else if (strcmp (v.name, "multi range opt") == 0)
	{
	  isidp->multi_range_opt.use = true;
	  isidp->multi_range_opt.cnt = 5;
	}
      else if (strcmp (v.name, "no OID list") == 0)
	{
	  isidp->oid_list = nullptr;
	}
      isidp->ahead_next = 77;
      isidp->ahead_distance = 77;
      CHECK (call_get_next_index_oidset (sm::thr, sm::sid, isidp, true) == S_SUCCESS);
      CHECK (isidp->ahead_next == v.expect_next);
      CHECK (isidp->ahead_distance == 0);
    }

  /* QPROC_SINGLE_INNER with one OID: nothing told either */
  sm::setup ({ sm::distinct_pages (1, 1000) }, QPROC_SINGLE_INNER);
  CHECK (call_get_next_index_oidset (sm::thr, sm::sid, &sm::sid->s.isid, true) == S_SUCCESS);
  CHECK (sm::sid->s.isid.ahead_next == INT_MAX);
  CHECK (sm::sid->s.isid.ahead_distance == 0);
}
