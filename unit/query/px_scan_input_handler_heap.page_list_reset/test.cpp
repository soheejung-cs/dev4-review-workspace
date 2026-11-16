/* PR#8497 (CBRD-28043) GB-01, the parallel side (GA-01) -- a parallel heap scan's reset (manager::reset () -> open ()
 * -> input_handler_heap::init_on_main (), px_scan.cpp:2064 -> :1737) after the transaction's snapshot was built again
 * (READ COMMITTED: a nested statement, MVCC_SNAPSHOT.build_serial 7 -> 8) and a page was allocated since.
 *
 * Real code (the TU, px_scan_input_handler_heap.cpp): input_handler_heap::init_on_main (), initialize (),
 * get_next_vpid_with_fix (), finalize (); ftab_set (header); bit64_* of bit.c (--extra).
 * 3328953d3 init_on_main (thread_p, hfid, parallelism, page_list) splits the bitmap it is handed -- manager::open ()
 * hands it m_page_list, which set_page_list () set once at the scan's open (px_scan.cpp:496/530/564); develop's
 * init_on_main (thread_p, hfid, parallelism) took the snapshot and collected the bitmap itself every time.  The test
 * calls whichever the revision has (SFINAE) and, for 3328953d3, hands the open's bitmap again as manager::open () does.
 *
 * Fakes (other TUs):
 *  - logtb_get_mvcc_snapshot (), file_get_all_data_sectors (): count their calls; the file's data sectors now.
 *  - heap_page_list_fix_next () (3328953d3's walk): the real one's walk without the page fix (heap_file.c:2894-2961);
 *    records the page.
 *  - pgbuf_ordered_fix_release () / pgbuf_ordered_unfix () / heap_page_is_bestspace () (develop's walk): record the page,
 *    hold a dummy page pointer in the watcher / let it go; no bestspace page.
 *  - db_private_alloc_release () / db_private_free_release () = malloc / free with a live count; er_* one level.
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
#include "page_buffer.h"
#include "log_impl.h"
#include "mvcc.h"
#include "scan_manager.h"
#include "px_scan_input_handler_heap.hpp"
#include "thread_entry.hpp"

namespace gp
{
  cubthread::entry *thr;
  alignas (MVCC_SNAPSHOT) unsigned char snap_mem[sizeof (MVCC_SNAPSHOT)];
  MVCC_SNAPSHOT *snap = (MVCC_SNAPSHOT *) snap_mem;
  int snapshot_calls, collect_calls, live_blocks, err;
  std::vector<FILE_PARTIAL_SECTOR> file_sectors;
  std::vector<int> visited;
  char dummy_page[64];
  HFID heap_id;
  FILE_FTAB_COLLECTOR open_list;	/* the bitmap the scan's open collected (scan_heap_scan_page_list ()) */
  std::vector<FILE_PARTIAL_SECTOR> open_sectors;

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
      }
    file_sectors.push_back (s);
  }

  /* heap header 641 (sector 10), data pages 642, 643, 650, 704, 705, 706, 720 */
  void
  setup ()
  {
    if (thr == NULL)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));	/* never constructed: read as fields */
	thr->index = 1;
      }
    memset (snap_mem, 0, sizeof (snap_mem));
    snapshot_calls = collect_calls = live_blocks = 0;
    err = NO_ERROR;
    file_sectors.clear ();
    visited.clear ();
    heap_id.vfid.volid = 0;
    heap_id.vfid.fileid = 640;
    heap_id.hpgid = 641;
    add_sector (10, { 1, 2, 3, 10 });
    add_sector (11, { 0, 1, 2, 16 });
    /* the open collects after the statement's snapshot (build 7) */
    open_sectors = file_sectors;
    open_list.nsects = (int) open_sectors.size ();
    open_list.npages = 8;
    open_list.partsect_ftab = open_sectors.data ();
  }

  void
  allocate_page_768 ()
  {
    add_sector (12, { 0 });
  }
}

/* ---- fakes ---- */

template < typename S > auto
set_build (S * s, unsigned int b, int)->decltype (s->build_serial = b, void ())
{
  s->build_serial = b;
}

template < typename S > void
set_build (S *, unsigned int, long)
{
}

MVCC_SNAPSHOT *
logtb_get_mvcc_snapshot (THREAD_ENTRY * thread_p)
{
  (void) thread_p;
  gp::snapshot_calls++;
  return gp::snap;
}

int
file_get_all_data_sectors (THREAD_ENTRY * thread_p, const VFID * vfid, FILE_FTAB_COLLECTOR * collector_out)
{
  (void) thread_p;
  (void) vfid;
  gp::collect_calls++;
  collector_out->nsects = (int) gp::file_sectors.size ();
  collector_out->npages = 0;
  collector_out->partsect_ftab =
    (FILE_PARTIAL_SECTOR *) malloc (sizeof (FILE_PARTIAL_SECTOR) * (gp::file_sectors.size () + 1));
  gp::live_blocks++;
  for (size_t i = 0; i < gp::file_sectors.size (); i++)
    {
      collector_out->partsect_ftab[i] = gp::file_sectors[i];
      collector_out->npages += __builtin_popcountll (gp::file_sectors[i].page_bitmap);
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
      gp::visited.push_back (pageid);
      (*pgoffset)++;
      return S_SUCCESS;
    }
  return S_END;
}

int
pgbuf_ordered_fix_release (THREAD_ENTRY * thread_p, const VPID * req_vpid, PAGE_FETCH_MODE fetch_mode,
			   const PGBUF_LATCH_MODE request_mode, PGBUF_WATCHER * req_watcher)
{
  (void) thread_p;
  (void) fetch_mode;
  (void) request_mode;
  gp::visited.push_back (req_vpid->pageid);
  req_watcher->pgptr = (PAGE_PTR) gp::dummy_page;
  return NO_ERROR;
}

void
pgbuf_ordered_unfix (THREAD_ENTRY * thread_p, PGBUF_WATCHER * watcher_object)
{
  (void) thread_p;
  watcher_object->pgptr = NULL;
}

bool
heap_page_is_bestspace (THREAD_ENTRY * thread_p, PAGE_PTR page_heap)
{
  (void) thread_p;
  (void) page_heap;
  return false;
}

void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  gp::live_blocks++;
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  if (ptr != NULL)
    {
      gp::live_blocks--;
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
  gp::err = err_id;
}

int
er_errid (void)
{
  return gp::err;
}

void
er_clear (void)
{
  gp::err = NO_ERROR;
}

/* ---- the revision's init_on_main ---- */

template < typename H > auto
call_init_on_main (H & h, THREAD_ENTRY * t, HFID hfid, int par, const FILE_FTAB_COLLECTOR * pl,
		   int)->decltype (h.init_on_main (t, hfid, par, pl))
{
  return h.init_on_main (t, hfid, par, pl);	/* 3328953d3: the bitmap manager::open () hands over */
}

template < typename H > auto
call_init_on_main (H & h, THREAD_ENTRY * t, HFID hfid, int par, const FILE_FTAB_COLLECTOR * pl,
		   long)->decltype (h.init_on_main (t, hfid, par))
{
  (void) pl;
  return h.init_on_main (t, hfid, par);	/* develop: snapshot, then collect */
}

namespace gp
{
  /* one pass: every worker in turn, as the workers of the scan would (thread_local walk state per call) */
  std::vector<int>
  pass (parallel_scan::input_handler_heap & h, int workers)
  {
    std::vector<int> all;
    SCAN_ID *sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));

    for (int w = 0; w < workers; w++)
      {
	VPID vpid;
	SCAN_CODE c;
	int n = 0;

	visited.clear ();
	memset (&sid->s.hsid.scan_cache.page_watcher, 0, sizeof (PGBUF_WATCHER));
	REQUIRE (h.initialize (thr, &heap_id, sid) == NO_ERROR);
	while ((c = h.get_next_vpid_with_fix (thr, &vpid)) == S_SUCCESS && n++ < 1000)
	  {
	  }
	CHECK (c == S_END);
	h.finalize (thr);
	all.insert (all.end (), visited.begin (), visited.end ());
      }
    free (sid);
    std::sort (all.begin (), all.end ());
    return all;
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
}

using namespace gp;

TEST_CASE ("P0 control: an open split among 2 and among 3 workers walks every listed page exactly once", "[contract]")
{
  for (int workers : { 2, 3 })
    {
      setup ();
      parallel_scan::input_handler_heap h (nullptr, nullptr);
      set_build (snap, 7, 0);
      REQUIRE (call_init_on_main (h, thr, heap_id, workers, &open_list, 0) == NO_ERROR);
      std::vector<int> v = pass (h, workers);
      CAPTURE (workers, str (v));
      CHECK (v == std::vector<int> ({ 641, 642, 643, 650, 704, 705, 706, 720 }));
      CHECK (live_blocks == 0);
    }
}

TEST_CASE ("PR8497 GB-01 P1 (GA-01): after the snapshot was built again (build 7 -> 8) and page 768 was allocated, a "
	   "reset of the parallel scan (init_on_main () again) splits the open's bitmap -- no snapshot, no collection, page "
	   "768 is walked by no worker (PASS = defect present; develop took the snapshot and collected again at every "
	   "reset)", "[defect]")
{
  setup ();
  parallel_scan::input_handler_heap h (nullptr, nullptr);
  set_build (snap, 7, 0);
  REQUIRE (call_init_on_main (h, thr, heap_id, 2, &open_list, 0) == NO_ERROR);
  (void) pass (h, 2);
  int snapshots_at_open = snapshot_calls, collected_at_open = collect_calls;

  set_build (snap, 8, 0);	/* a nested statement rebuilt the READ COMMITTED snapshot */
  allocate_page_768 ();
  REQUIRE (call_init_on_main (h, thr, heap_id, 2, &open_list, 0) == NO_ERROR);
  std::vector<int> v = pass (h, 2);
  CAPTURE (str (v), snapshot_calls, collect_calls);
  CHECK (std::find (v.begin (), v.end (), 642) != v.end ());
  CHECK (std::find (v.begin (), v.end (), 768) == v.end ());
  CHECK (snapshot_calls == snapshots_at_open);
  CHECK (collect_calls == collected_at_open);
  CHECK (live_blocks == 0);
}

TEST_CASE ("P2 a reset with nothing changed walks the same pages again", "[contract]")
{
  setup ();
  parallel_scan::input_handler_heap h (nullptr, nullptr);
  set_build (snap, 7, 0);
  REQUIRE (call_init_on_main (h, thr, heap_id, 3, &open_list, 0) == NO_ERROR);
  std::vector<int> first = pass (h, 3);
  REQUIRE (call_init_on_main (h, thr, heap_id, 3, &open_list, 0) == NO_ERROR);
  std::vector<int> second = pass (h, 3);
  CAPTURE (str (first), str (second));
  CHECK (first == second);
  CHECK (live_blocks == 0);
}
