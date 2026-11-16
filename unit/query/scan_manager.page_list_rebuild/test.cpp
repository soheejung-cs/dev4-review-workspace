/* PR#8497 (CBRD-28043) GB-01, the serial side -- an open heap scan that is rewound (scan_reset_scan_block (), the
 * inner scan of a nested loop at every outer row) or restarted (scan_start_scan ()) after the transaction's snapshot
 * was built again (READ COMMITTED: a nested statement, MVCC_SNAPSHOT.build_serial 7 -> 8).
 *
 * Real code (the TU, scan_manager.c): scan_start_scan (), scan_reset_scan_block (), scan_next_scan () down the row path
 * (scan_handle_single_scan () -> scan_next_scan_local () -> scan_next_heap_scan () -> scan_next_heap_scan_rows ()),
 * and in 3328953d3 scan_heap_scan_page_list () / scan_heap_scan_set_page_list () / scan_heap_next_listed_row () /
 * scan_heap_next_listed_page ().  bit64_count_ones () of bit.c (--extra).
 *
 * Fakes (other TUs):
 *  - logtb_get_mvcc_snapshot (): one snapshot object, as the transaction descriptor keeps one and rebuilds it in place
 *    (log_tran_table.c:4441-4443, build_serial++ at mvcc_table.cpp:336); the case sets the build.  The object is never
 *    constructed (mvcc.c is not linked); build_serial is written where the member exists (3328953d3).
 *  - heap_scancache_start (): records the snapshot it is handed.
 *  - file_get_all_data_sectors (): the heap file's data sectors now, in a malloc'd array (counts its calls).
 *  - heap_page_list_fix_next () (3328953d3): the real one's walk without the page fix -- from *pgoffset, the next set bit
 *    of the sector, the file header page passed over, *pgoffset past the page returned, S_END past the last bit
 *    (heap_file.c:2894-2961); records the page.
 *  - heap_next_1page (): S_END -- the page holds no row this test needs.
 *  - heap_next () (develop's walk): with a NULL OID it walks the heap's page links from the header -- the pages now
 *    linked, a page allocated since included (develop heap_file.c: a new page is linked when allocated) -- and records
 *    them; S_END.
 *  - oid_Root_class_oid (oid.c:78, a booted process's root class OID): an OID no class of the test has.
 *  - disk_compare_vsids () = disk_manager.c:6226-6240, mvcc_is_mvcc_disabled_class () = false,
 *    db_private_alloc_release () / db_private_free_release () = malloc / free with a live count, er_* one level.
 */
#include "catch.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "storage_common.h"
#include "error_manager.h"
#include "memory_alloc.h"
#include "file_manager.h"
#include "heap_file.h"
#include "log_impl.h"
#include "mvcc.h"
#include "scan_manager.h"
#include "xasl.h"
#include "thread_entry.hpp"

/* new in 3328953d3: weak, so the suite links on develop and the cases that need them are not registered there */
extern int scan_heap_scan_page_list (THREAD_ENTRY * thread_p, struct access_spec_node *spec, const OID * cls_oid,
				     const HFID * hfid, bool grouped, const FILE_FTAB_COLLECTOR ** page_list,
				     int *num_data_pages) __attribute__ ((weak));
extern void scan_heap_scan_set_page_list (SCAN_ID * scan_id, const FILE_FTAB_COLLECTOR * page_list)
  __attribute__ ((weak));
extern void scan_heap_scan_page_list_free (THREAD_ENTRY * thread_p, struct access_spec_node *spec)
  __attribute__ ((weak));

/* members that exist in 3328953d3 only */
template < typename S > auto
set_build (S * s, unsigned int b, int)->decltype (s->build_serial = b, void ())
{
  s->build_serial = b;
}

template < typename S > void
set_build (S *, unsigned int, long)
{
}

template < typename H > auto
sector_order_of (H * h, int)->decltype ((int) h->sector_order)
{
  return h->sector_order ? 1 : 0;
}

template < typename H > int
sector_order_of (H *, long)
{
  return -1;			/* no such member: develop follows the page links */
}

namespace gb
{
  cubthread::entry *thr;
  alignas (MVCC_SNAPSHOT) unsigned char snap_mem[sizeof (MVCC_SNAPSHOT)];
  MVCC_SNAPSHOT *snap = (MVCC_SNAPSHOT *) snap_mem;
  unsigned int build;
  int snapshot_calls, collect_calls, live_blocks;
  MVCC_SNAPSHOT *cache_snapshot;
  std::vector<FILE_PARTIAL_SECTOR> file_sectors;	/* the heap file's data sectors now */
  std::vector<int> links;	/* the heap's page links now (develop's walk) */
  std::vector<int> visited;
  int heap_next_from_start;
  int err;

  HFID heap_id;
  OID cls;

  void
  add_sector (int sectid, std::vector<int> offsets)
  {
    FILE_PARTIAL_SECTOR s;
    s.vsid.volid = 0;
    s.vsid.sectid = sectid;
    s.page_bitmap = 0;
    for (int o : offsets)
      {
	s.page_bitmap |= ((UINT64) 1) << o;
	links.push_back (sectid * DISK_SECTOR_NPAGES + o);
      }
    file_sectors.push_back (s);
  }

  /* the heap at the open: file header page 640 (sector 10, offset 0; not in the data bitmap), heap header page 641,
   * data pages 642, 643, 650 and 704, 705 (sector 11) */
  void
  setup ()
  {
    if (thr == NULL)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));	/* never constructed: read as fields */
	thr->index = 1;
      }
    memset (snap_mem, 0, sizeof (snap_mem));
    build = 7;
    snapshot_calls = collect_calls = 0;
    cache_snapshot = NULL;
    file_sectors.clear ();
    links.clear ();
    visited.clear ();
    heap_next_from_start = 0;
    err = NO_ERROR;
    heap_id.vfid.volid = 0;
    heap_id.vfid.fileid = 640;
    heap_id.hpgid = 641;
    cls.volid = 0;
    cls.pageid = 7;
    cls.slotid = 3;
    add_sector (10, { 1, 2, 3, 10 });
    add_sector (11, { 0, 1 });
  }

  /* a page allocated after the open, in a new sector: another transaction's committed rows are on it */
  void
  allocate_page_768 ()
  {
    add_sector (12, { 0 });
  }

  SCAN_ID *
  new_scan ()
  {
    SCAN_ID *sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
    sid->type = S_HEAP_SCAN;
    sid->grouped = false;
    sid->fixed = true;
    sid->direction = S_FORWARD;
    sid->single_fetch = QPROC_NO_SINGLE_INNER;
    sid->s.hsid.hfid = heap_id;
    sid->s.hsid.cls_oid = cls;
    sid->s.hsid.caches_inited = true;	/* the attribute caches are not this test's */
    return sid;
  }

  /* one pass of the scan to its end: the pages it asked for */
  std::vector<int>
  pass (SCAN_ID * sid)
  {
    SCAN_CODE c;
    int n = 0;
    visited.clear ();
    while ((c = scan_next_scan (thr, sid)) == S_SUCCESS && n++ < 1000)
      {
      }
    CHECK (c == S_END);
    return visited;
  }

  bool
  has (const std::vector<int> &v, int p)
  {
    return std::find (v.begin (), v.end (), p) != v.end ();
  }

  std::string
  str (const std::vector<int> &v)
  {
    std::string s;
    for (int p : v)
      {
	s += " " + std::to_string (p);
      }
    return s;
  }

  /* the open as qexec_open_scan () does it: the spec's bitmap (3328953d3), handed to the scan; develop has none */
  ACCESS_SPEC_TYPE *
  open (SCAN_ID * sid)
  {
    ACCESS_SPEC_TYPE *spec = (ACCESS_SPEC_TYPE *) calloc (1, sizeof (ACCESS_SPEC_TYPE));
    if (scan_heap_scan_page_list != NULL)
      {
	const FILE_FTAB_COLLECTOR *pl = NULL;
	int npages = -1;
	REQUIRE (scan_heap_scan_page_list (thr, spec, &cls, &heap_id, false, &pl, &npages) == NO_ERROR);
	REQUIRE (pl != NULL);
	scan_heap_scan_set_page_list (sid, pl);
      }
    return spec;
  }

  void
  close (ACCESS_SPEC_TYPE * spec, SCAN_ID * sid)
  {
    if (scan_heap_scan_page_list_free != NULL)
      {
	scan_heap_scan_page_list_free (thr, spec);
      }
    free (spec);
    free (sid);
  }
}

/* ---- fakes ---- */

static OID unit_root_oid = { 1, 1, 0 };	/* oid.c:78: the root class; no class of this test */
OID *oid_Root_class_oid = &unit_root_oid;

MVCC_SNAPSHOT *
logtb_get_mvcc_snapshot (THREAD_ENTRY * thread_p)
{
  (void) thread_p;
  gb::snapshot_calls++;
  set_build (gb::snap, gb::build, 0);
  return gb::snap;
}

int
heap_scancache_start (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid, const OID * class_oid,
		      int cache_last_fix_page, MVCC_SNAPSHOT * mvcc_snapshot, bool copy_to_local_cache)
{
  (void) thread_p;
  (void) scan_cache;
  (void) hfid;
  (void) class_oid;
  (void) cache_last_fix_page;
  (void) copy_to_local_cache;
  gb::cache_snapshot = mvcc_snapshot;
  return NO_ERROR;
}

int
file_get_all_data_sectors (THREAD_ENTRY * thread_p, const VFID * vfid, FILE_FTAB_COLLECTOR * collector_out)
{
  (void) thread_p;
  (void) vfid;
  gb::collect_calls++;
  collector_out->nsects = (int) gb::file_sectors.size ();
  collector_out->npages = 0;
  collector_out->partsect_ftab =
    (FILE_PARTIAL_SECTOR *) malloc (sizeof (FILE_PARTIAL_SECTOR) * (gb::file_sectors.size () + 1));
  gb::live_blocks++;
  for (size_t i = 0; i < gb::file_sectors.size (); i++)
    {
      collector_out->partsect_ftab[i] = gb::file_sectors[i];
      collector_out->npages += __builtin_popcountll (gb::file_sectors[i].page_bitmap);
    }
  return NO_ERROR;
}

SCAN_CODE
heap_page_list_fix_next (THREAD_ENTRY * thread_p, const HFID * hfid, const FILE_PARTIAL_SECTOR * sector,
			 size_t * pgoffset, PGBUF_WATCHER * watcher, VPID * vpid_out)
{
  (void) thread_p;
  (void) watcher;
  for (; *pgoffset < DISK_SECTOR_NPAGES; (*pgoffset)++)
    {
      int pageid;

      if ((sector->page_bitmap & (((UINT64) 1) << *pgoffset)) == 0)
	{
	  continue;
	}
      pageid = SECTOR_FIRST_PAGEID (sector->vsid.sectid) + (int) *pgoffset;
      if (sector->vsid.volid == hfid->vfid.volid && pageid == hfid->vfid.fileid)
	{
	  continue;
	}
      vpid_out->volid = sector->vsid.volid;
      vpid_out->pageid = pageid;
      gb::visited.push_back (pageid);
      (*pgoffset)++;
      return S_SUCCESS;
    }
  return S_END;
}

SCAN_CODE
heap_next_1page (THREAD_ENTRY * thread_p, const HFID * hfid, const VPID * vpid, OID * class_oid, OID * next_oid,
		 RECDES * recdes, HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  (void) thread_p;
  (void) hfid;
  (void) vpid;
  (void) class_oid;
  (void) next_oid;
  (void) recdes;
  (void) scan_cache;
  (void) ispeeking;
  return S_END;
}

SCAN_CODE
heap_next (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
	   HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  (void) thread_p;
  (void) hfid;
  (void) class_oid;
  (void) recdes;
  (void) scan_cache;
  (void) ispeeking;
  if (OID_ISNULL (next_oid))
    {
      gb::heap_next_from_start++;
      for (int p : gb::links)
	{
	  if (p != gb::heap_id.vfid.fileid)
	    {
	      gb::visited.push_back (p);
	    }
	}
    }
  return S_END;
}

int
disk_compare_vsids (const void *first, const void *second)
{
  const VSID *a = (const VSID *) first;
  const VSID *b = (const VSID *) second;
  if (a->volid != b->volid)
    {
      return a->volid > b->volid ? 1 : -1;
    }
  return (int) (a->sectid - b->sectid);
}

bool
mvcc_is_mvcc_disabled_class (const OID * class_oid)
{
  (void) class_oid;
  return false;
}

void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  gb::live_blocks++;
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  if (ptr != NULL)
    {
      gb::live_blocks--;
      free (ptr);
    }
}

void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  gb::err = err_id;
}

int
er_errid (void)
{
  return gb::err;
}

void
er_clear (void)
{
  gb::err = NO_ERROR;
}

using namespace gb;

#define HEAD_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = (scan_heap_scan_page_list != NULL) \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* ================================================================ */

TEST_CASE ("B0 control: a pass of the open scan reads every page of the heap once (the bitmap's pages in 3328953d3, the "
	   "linked pages in develop) under the snapshot scan_start_scan () hands the scan cache", "[contract]")
{
  setup ();
  SCAN_ID *sid = new_scan ();
  ACCESS_SPEC_TYPE *spec = open (sid);
  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  std::vector<int> v = pass (sid);
  CAPTURE (str (v), sector_order_of (&sid->s.hsid, 0));
  CHECK (v == std::vector<int> ({ 641, 642, 643, 650, 704, 705 }));
  CHECK (cache_snapshot == snap);
  close (spec, sid);
  CHECK (live_blocks == 0);
}

TEST_CASE ("PR8497 GB-01 B1: after the snapshot was built again (build 7 -> 8) and a page was allocated (768, rows "
	   "another transaction committed), the rewound pass of the open scan (scan_reset_scan_block (), a nested loop's "
	   "inner scan at the next outer row) walks the open's bitmap -- page 768 is not read and nothing is collected "
	   "(PASS = defect present; develop follows the links from the header and reads it)", "[defect]")
{
  setup ();
  SCAN_ID *sid = new_scan ();
  ACCESS_SPEC_TYPE *spec = open (sid);
  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  (void) pass (sid);
  int collected_at_open = collect_calls;

  build = 8;			/* a nested statement rebuilt the READ COMMITTED snapshot (network_sr.c:936-938) */
  (void) logtb_get_mvcc_snapshot (thr);
  allocate_page_768 ();

  REQUIRE (scan_reset_scan_block (thr, sid) == S_SUCCESS);
  std::vector<int> v = pass (sid);
  CAPTURE (str (v), sector_order_of (&sid->s.hsid, 0), heap_next_from_start);
  CHECK (has (v, 642));
  CHECK_FALSE (has (v, 768));
  CHECK (collect_calls == collected_at_open);
  close (spec, sid);
}

TEST_CASE ("PR8497 GB-01 B2: the same for a restart (scan_start_scan (), a scan block started again) -- it takes the "
	   "rebuilt snapshot for the scan cache and still walks the open's bitmap: page 768 is not read (PASS = defect "
	   "present)", "[defect]")
{
  setup ();
  SCAN_ID *sid = new_scan ();
  ACCESS_SPEC_TYPE *spec = open (sid);
  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  (void) pass (sid);
  int collected_at_open = collect_calls;
  int snapshots_before = snapshot_calls;

  build = 8;
  allocate_page_768 ();

  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  CHECK (snapshot_calls == snapshots_before + 1);	/* the restart asked for the (rebuilt) snapshot */
  CHECK (cache_snapshot == snap);
  std::vector<int> v = pass (sid);
  CAPTURE (str (v), sector_order_of (&sid->s.hsid, 0));
  CHECK (has (v, 642));
  CHECK_FALSE (has (v, 768));
  CHECK (collect_calls == collected_at_open);
  close (spec, sid);
}

TEST_CASE ("B3 a rewind and a restart read the heap from its first page again: every page of the open is read in the "
	   "next pass", "[contract]")
{
  setup ();
  SCAN_ID *sid = new_scan ();
  ACCESS_SPEC_TYPE *spec = open (sid);
  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  std::vector<int> first = pass (sid);
  REQUIRE (scan_reset_scan_block (thr, sid) == S_SUCCESS);
  std::vector<int> second = pass (sid);
  REQUIRE (scan_start_scan (thr, sid) == NO_ERROR);
  std::vector<int> third = pass (sid);
  CAPTURE (str (first), str (second), str (third));
  CHECK (first == second);
  CHECK (first == third);
  close (spec, sid);
}

HEAD_CASE (b4_contract,
	   "B4 (3328953d3) the open's rule: a reopen under the same build hands back the kept bitmap without collecting, a "
	   "reopen under a rebuilt snapshot collects again and lists page 768 (scan_manager.c:5449-5472)", "[contract]")
{
  setup ();
  ACCESS_SPEC_TYPE *spec = (ACCESS_SPEC_TYPE *) calloc (1, sizeof (ACCESS_SPEC_TYPE));
  const FILE_FTAB_COLLECTOR *pl = NULL;
  int npages = -1;

  REQUIRE (scan_heap_scan_page_list (thr, spec, &cls, &heap_id, false, &pl, &npages) == NO_ERROR);
  CHECK (collect_calls == 1);
  CHECK (npages == 6);		/* 641 (heap header), 642, 643, 650, 704, 705 */
  allocate_page_768 ();
  REQUIRE (scan_heap_scan_page_list (thr, spec, &cls, &heap_id, false, &pl, &npages) == NO_ERROR);
  CHECK (collect_calls == 1);	/* same build: the kept bitmap */
  CHECK (npages == 6);
  build = 8;
  REQUIRE (scan_heap_scan_page_list (thr, spec, &cls, &heap_id, false, &pl, &npages) == NO_ERROR);
  CHECK (collect_calls == 2);	/* rebuilt snapshot: collected again */
  CHECK (npages == 7);
  CHECK (pl->nsects == 3);
  scan_heap_scan_page_list_free (thr, spec);
  free (spec);
  CHECK (live_blocks == 0);
}
