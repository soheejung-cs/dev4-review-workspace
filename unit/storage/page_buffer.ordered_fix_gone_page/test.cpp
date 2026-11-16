/* PR#8497 (CBRD-28043) GD-01 -- pgbuf_ordered_fix () with OLD_PAGE_MAYBE_DEALLOCATED while the thread holds another
 * ordered page, and heap_page_list_fix_next () (heap_file.c, new in 3328953d3) that takes its ER_PB_BAD_PAGEID for
 * "deallocated since the bitmap" and walks on.
 *
 * Real code: pgbuf_ordered_fix () / pgbuf_ordered_unfix () / pgbuf_get_page_ptype () and the holder bookkeeping of
 * page_buffer.c (the TU), heap_page_list_fix_next () and heap_page_is_bestspace () of heap_file.c (--extra; absent in
 * develop, so the L / W cases are registered only where the function is linked), bit64_* of bit.c (--extra).
 *
 * Fakes (what they copy is written in hooks.cpp's head comment):
 *  - pgbuf_fix_release () / pgbuf_unfix () (seams, weak in the TU; pgbuf_fix_debug () / pgbuf_unfix_debug () on a debug
 *    build tree): hooks.cpp's unit_pgbuf_fix () / unit_pgbuf_unfix () -- one other thread per page holding a latch,
 *    and what that thread does before it lets the page go.  On a debug build W3 and O4 hit an assert (false) the code
 *    has on purpose (a non-heap page listed; a PREVENT_DEALLOC page found deallocated): run it with '~W3*,~O4*'.
 *  - spage_get_record (): slot 0 of a heap page is a HEAP_CHAIN; its flags carry HEAP_PAGE_FLAG_BESTSPACE (0x1,
 *    heap_file.c:231) for the pages the case marks.  R0 checks the replica against the real heap_page_is_bestspace ().
 *  - er_set / er_errid / er_errid_if_has_error / er_clear: one error level, severity kept (error_manager.c:1868-1901:
 *    er_errid_if_has_error () ignores warnings and notifications).
 *  - fileio_get_volume_label (): a fixed label (an er_set argument only).
 *
 * The layout: volume 0.  Heap A: file header page 64, heap header page 65 (its pages' ordered group is VPID 0|65).
 * Heap B: file header page 320, heap header page 321 (group 0|321).  pgbuf_compare_hold_vpid_for_sort () orders by the
 * group first (page_buffer.c:12933-12953), so every page of heap A sorts before every page of heap B -- page 520 of
 * heap A before page 330 of heap B.  GD-01 as the refutation placed it (refute.GD.json): the innermost fixed sequential
 * scan of a join holds its page of heap B while a select-list correlated subquery (dptr) walks heap A, whose group
 * sorts first.
 */
#include "catch.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "storage_common.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "lock_manager.h"
#include "page_buffer.h"
#include "file_manager.h"
#include "slotted_page.h"
#include "heap_file.h"
#include "thread_entry.hpp"

/* ---- hooks.cpp (appended to page_buffer.c) ---- */
extern void unit_pgbuf_reset (THREAD_ENTRY * thread_p);
extern void unit_pgbuf_add_page (short volid, int pageid, int ptype);
extern void unit_pgbuf_set_other (short volid, int pageid, int other, int then);
extern void unit_pgbuf_set_ptype (short volid, int pageid, int ptype);
extern int unit_pgbuf_ptype (short volid, int pageid);
extern int unit_pgbuf_count (short volid, int pageid, int which);
extern int unit_pgbuf_avoid_dealloc (short volid, int pageid);
extern PAGE_PTR unit_pgbuf_page_ptr (short volid, int pageid);
extern int unit_pgbuf_held (THREAD_ENTRY * thread_p, int *pageids, int *fix_counts, int *watch_counts, int max);
extern void unit_pgbuf_interrupt_next_fix (void);
extern int unit_pgbuf_unknown_fixes (void);
extern PAGE_PTR unit_pgbuf_fix (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode,
				PGBUF_LATCH_MODE request_mode, PGBUF_LATCH_CONDITION condition);
extern void unit_pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr);

enum
{ OTHER_NONE = 0, OTHER_READ, OTHER_WRITE, OTHER_WRITER_WAITS };
enum
{ THEN_RELEASE = 0, THEN_DEALLOC, THEN_REUSE_FTAB };
enum
{ CNT_FIXES = 0, CNT_FIX_CALLS, CNT_COND_REFUSED, CNT_WAITED };

/* new in 3328953d3: weak, so the suite links where it is absent (develop) and the L / W cases are not registered */
extern SCAN_CODE heap_page_list_fix_next (THREAD_ENTRY * thread_p, const HFID * hfid,
					  const FILE_PARTIAL_SECTOR * sector, size_t * pgoffset,
					  PGBUF_WATCHER * watcher, VPID * vpid_out) __attribute__ ((weak));

/* ---- fakes ---- */
namespace ut
{
  int err_id = NO_ERROR;
  int err_sev = ER_WARNING_SEVERITY;
  std::set<PAGE_PTR> bestspace;

  /* heap_file.c:280-288 */
  struct heap_chain_replica
  {
    OID class_oid;
    VPID prev_vpid;
    VPID next_vpid;
    MVCCID max_mvccid;
    INT32 flags;
  };
}

#if defined (NDEBUG)
PAGE_PTR
pgbuf_fix_release (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode,
		   PGBUF_LATCH_MODE request_mode, PGBUF_LATCH_CONDITION condition)
{
  return unit_pgbuf_fix (thread_p, vpid, fetch_mode, request_mode, condition);
}

void
pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr)
{
  unit_pgbuf_unfix (thread_p, pgptr);
}
#else /* a debug build tree (--build-tree .../build_x86_64_debug): the debug names, seamed the same way */
PAGE_PTR
pgbuf_fix_debug (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode, PGBUF_LATCH_MODE request_mode,
		 PGBUF_LATCH_CONDITION condition, const char *caller_file, int caller_line, const char *caller_func)
{
  (void) caller_file;
  (void) caller_line;
  (void) caller_func;
  return unit_pgbuf_fix (thread_p, vpid, fetch_mode, request_mode, condition);
}

void
pgbuf_unfix_debug (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, const char *caller_file, int caller_line,
		   const char *caller_func)
{
  (void) caller_file;
  (void) caller_line;
  (void) caller_func;
  unit_pgbuf_unfix (thread_p, pgptr);
}
#endif

void
er_set (int severity, const char *, const int, int err_id, int, ...)
{
  ut::err_id = err_id;
  ut::err_sev = severity;
}

int
er_errid (void)
{
  return ut::err_id;
}

int
er_errid_if_has_error (void)
{
  return (ut::err_sev == ER_FATAL_ERROR_SEVERITY || ut::err_sev == ER_ERROR_SEVERITY
	  || ut::err_sev == ER_SYNTAX_ERROR_SEVERITY) ? ut::err_id : NO_ERROR;
}

void
er_clear (void)
{
  ut::err_id = NO_ERROR;
  ut::err_sev = ER_WARNING_SEVERITY;
}

char *
fileio_get_volume_label (VOLID volid, bool is_peek)
{
  static char label[] = "unit_volume";
  (void) volid;
  (void) is_peek;
  return label;
}

SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };
PGLENGTH db_Io_page_size = 16 * 1024;

SCAN_CODE
spage_get_record (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, PGSLOTID slotid, RECDES * recdes, int ispeeking)
{
  static ut::heap_chain_replica chains[2];
  bool bs = ut::bestspace.count (pgptr) > 0;

  (void) thread_p;
  (void) ispeeking;
  if (slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
    {
      return S_DOESNT_EXIST;
    }
  memset (&chains[bs], 0, sizeof (chains[bs]));
  chains[bs].flags = bs ? 0x1 : 0;
  recdes->data = (char *) &chains[bs];
  recdes->length = (int) sizeof (ut::heap_chain_replica);
  recdes->area_size = recdes->length;
  recdes->type = REC_HOME;
  return S_SUCCESS;
}

/* ---- the layout ---- */
namespace gd
{
  cubthread::entry *thr;
  HFID hfid_a;			/* group 0|65: sorts first */
  HFID hfid_b;			/* group 0|321 */

  HFID
  hfid_of (int fileid, int hpgid)
  {
    HFID h;
    h.vfid.volid = 0;
    h.vfid.fileid = fileid;
    h.hpgid = hpgid;
    return h;
  }

  VPID
  vp (int pageid)
  {
    VPID v;
    v.volid = 0;
    v.pageid = pageid;
    return v;
  }

  FILE_PARTIAL_SECTOR
  sector_of (int sectid, std::vector<int> offsets)
  {
    FILE_PARTIAL_SECTOR s;
    s.vsid.volid = 0;
    s.vsid.sectid = sectid;
    s.page_bitmap = 0;
    for (int o : offsets)
      {
	s.page_bitmap |= ((UINT64) 1) << o;
      }
    return s;
  }

  /* heap A's data pages are in sector 8 (pages 512-575), heap B's in sector 5 (320-383): page 520 of heap A has a
   * higher page number than page 330 of heap B and still sorts first, by its group */
  void
  setup ()
  {
    if (thr == NULL)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));	/* never constructed: read as fields */
	thr->index = 1;
      }
    thr->wait_msecs_override = LK_INFINITE_WAIT;
    /* the parameters this path reads, typed as a booted server has them (a debug build asserts the type): error log
     * debug off, page validation level 0 */
    prm_Def[PRM_ID_ER_LOG_DEBUG].datatype = PRM_BOOLEAN;
    prm_Def[PRM_ID_PB_DEBUG_PAGE_VALIDATION_LEVEL].datatype = PRM_INTEGER;
    unit_pgbuf_reset (thr);
    er_clear ();
    ut::bestspace.clear ();
    hfid_a = hfid_of (64, 65);
    hfid_b = hfid_of (320, 321);
    for (int p : { 65, 515, 520, 521, 530, 575 })
      {
	unit_pgbuf_add_page (0, p, PAGE_HEAP);
      }
    for (int p : { 321, 330, 331, 340 })
      {
	unit_pgbuf_add_page (0, p, PAGE_HEAP);
      }
    unit_pgbuf_add_page (0, 64, PAGE_FTAB);
    unit_pgbuf_add_page (0, 320, PAGE_FTAB);
  }

  std::vector<int>
  held ()
  {
    int ids[16], fixes[16], watches[16];
    int n = unit_pgbuf_held (thr, ids, fixes, watches, 16);
    std::vector<int> v (ids, ids + std::min (n, 16));
    std::sort (v.begin (), v.end ());
    return v;
  }

  std::string
  held_str ()
  {
    int ids[16], fixes[16], watches[16];
    int n = unit_pgbuf_held (thr, ids, fixes, watches, 16);
    std::string s = "held:";
    char b[64];
    for (int i = 0; i < std::min (n, 16); i++)
      {
	snprintf (b, sizeof (b), " %d(fix %d, watch %d)", ids[i], fixes[i], watches[i]);
	s += b;
      }
    return s;
  }

  /* the inner scan's page under an ordered watcher of this thread: a plain ordered fix (develop and head) */
  void
  hold (PGBUF_WATCHER * w, const HFID * hfid, int pageid)
  {
    VPID v = vp (pageid);
    PGBUF_INIT_WATCHER (w, PGBUF_ORDERED_HEAP_NORMAL, hfid);
    int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_PREVENT_DEALLOC, PGBUF_LATCH_READ, w);
    REQUIRE (rc == NO_ERROR);
    REQUIRE (w->pgptr != NULL);
  }

  /* the inner scan's page as head's sector walk fixes it (heap_page_list_fix_next ()) */
  void
  hold_listed (PGBUF_WATCHER * w, const HFID * hfid, int sectid, int offset)
  {
    FILE_PARTIAL_SECTOR s = sector_of (sectid, { offset });
    size_t off = 0;
    VPID got;

    PGBUF_INIT_WATCHER (w, PGBUF_ORDERED_HEAP_NORMAL, hfid);
    REQUIRE (heap_page_list_fix_next (thr, hfid, &s, &off, w, &got) == S_SUCCESS);
    REQUIRE (w->pgptr != NULL);
    REQUIRE (got.pageid == sectid * DISK_SECTOR_NPAGES + offset);
  }

  bool
  unfixed (const PGBUF_WATCHER * w)
  {
    return w->page_was_unfixed != 0;
  }

  bool
  holds (const PGBUF_WATCHER * w, int pageid)
  {
    return w->pgptr != NULL && w->pgptr == unit_pgbuf_page_ptr (0, pageid);
  }
}

using namespace gd;

#define L2_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = (heap_page_list_fix_next != NULL) \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* ================================================================ R0: the fake's record against the real reader */

TEST_CASE ("R0 guard: the HEAP_CHAIN the fake spage_get_record () returns is heap_file.c's -- the real "
	   "heap_page_is_bestspace () reads HEAP_PAGE_FLAG_BESTSPACE from it", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  hold (&w, &hfid_a, 520);
  CHECK_FALSE (heap_page_is_bestspace (thr, w.pgptr));
  ut::bestspace.insert (w.pgptr);
  CHECK (heap_page_is_bestspace (thr, w.pgptr));
  pgbuf_ordered_unfix (thr, &w);
  CHECK (held ().empty ());
}

/* ================================================================ O: pgbuf_ordered_fix () -- the same in head and base */

TEST_CASE ("O1 control: holding the inner page 330 (heap B), an ordered fix of page 520 (heap A, sorts first) with "
	   "OLD_PAGE_MAYBE_DEALLOCATED that another thread's write latch refuses lets page 330 go, waits, and fixes both "
	   "back -- NO_ERROR, page 330 re-fixed with page_was_unfixed, no dealloc mark left", "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_b, 330);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_RELEASE);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  VPID v = vp (520);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_MAYBE_DEALLOCATED, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 1);
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (rc == NO_ERROR);
  CHECK (holds (&w_sub, 520));
  CHECK (holds (&w_in, 330));
  CHECK (unfixed (&w_in));
  CHECK (held () == std::vector<int> ({ 330, 520 }));
  CHECK (unit_pgbuf_avoid_dealloc (0, 330) == 0);
}

TEST_CASE ("PR8497 GD-01 O2 (pgbuf_ordered_fix (), develop same): holding the inner page 330 (heap B), an ordered fix "
	   "of page 520 (heap A, sorts first) with OLD_PAGE_MAYBE_DEALLOCATED is refused, lets page 330 go, waits, and "
	   "finds page 520 deallocated by file_perm_dealloc () -- it returns ER_PB_BAD_PAGEID as a warning with page 330 "
	   "not fixed again and the thread holding no page (PASS = defect present)", "[defect]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_b, 330);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  VPID v = vp (520);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_MAYBE_DEALLOCATED, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 1);
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (rc == ER_PB_BAD_PAGEID);
  CHECK (er_errid () == ER_PB_BAD_PAGEID);
  CHECK (er_errid_if_has_error () == NO_ERROR);	/* a warning */
  CHECK (w_sub.pgptr == nullptr);
  CHECK (unfixed (&w_in));
  CHECK (w_in.pgptr == nullptr);	/* the defect: the held page is not fixed again */
  CHECK (unit_pgbuf_count (0, 330, CNT_FIXES) == 0);
  CHECK (held ().empty ());
  CHECK (unit_pgbuf_avoid_dealloc (0, 330) == 0);
}

TEST_CASE ("PR8497 GD-01 O3 (pgbuf_ordered_fix (), develop same): the same, page 520 deallocated and allocated again as "
	   "the file's table page while the fix waits (pgbuf_ordered_unfix_reused_page (), page_buffer.c:13632-13636) -- "
	   "ER_PB_BAD_PAGEID, page 330 not fixed again (PASS = defect present)", "[defect]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_b, 330);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_REUSE_FTAB);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  VPID v = vp (520);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_MAYBE_DEALLOCATED, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (unit_pgbuf_ptype (0, 520) == PAGE_FTAB);
  CHECK (rc == ER_PB_BAD_PAGEID);
  CHECK (er_errid_if_has_error () == NO_ERROR);
  CHECK (w_sub.pgptr == nullptr);
  CHECK (unfixed (&w_in));
  CHECK (w_in.pgptr == nullptr);
  CHECK (held ().empty ());
}

TEST_CASE ("O4 develop's chain-walk fix mode (OLD_PAGE_PREVENT_DEALLOC, base heap_next_internal () heap_file.c:7673) in "
	   "the same interleaving: the deallocated page comes back as an error-severity ER_PB_BAD_PAGEID, which the chain "
	   "walk turns into S_ERROR (base heap_file.c:7678-7689) -- the page is not passed over as deallocated",
	   "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_b, 330);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  VPID v = vp (520);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_PREVENT_DEALLOC, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (rc == ER_PB_BAD_PAGEID);
  CHECK (er_errid_if_has_error () == ER_PB_BAD_PAGEID);	/* an error: no caller reads it as "skip" */
  CHECK (w_sub.pgptr == nullptr);
}

TEST_CASE ("O5 develop's chain-walk fix mode with ordinary contention: OLD_PAGE_PREVENT_DEALLOC marks page 520 before "
	   "the refused conditional try, waits, fixes it and page 330 back -- NO_ERROR, both marks gone", "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_b, 330);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_RELEASE);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  VPID v = vp (520);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_PREVENT_DEALLOC, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 1);
  CHECK (rc == NO_ERROR);
  CHECK (holds (&w_sub, 520));
  CHECK (holds (&w_in, 330));
  CHECK (unfixed (&w_in));
  CHECK (unit_pgbuf_avoid_dealloc (0, 520) == 0);
  CHECK (unit_pgbuf_avoid_dealloc (0, 330) == 0);
}

TEST_CASE ("O6 group order: holding page 520 of heap A (group 0|65), a fix of page 330 of heap B (group 0|321) that is "
	   "refused and then finds the page deallocated keeps page 520 fixed -- a held page that sorts before the request "
	   "is never let go", "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold (&w_in, &hfid_a, 520);
  unit_pgbuf_set_other (0, 330, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_b);
  VPID v = vp (330);
  int rc = pgbuf_ordered_fix (thr, &v, OLD_PAGE_MAYBE_DEALLOCATED, PGBUF_LATCH_READ, &w_sub);
  CAPTURE (rc, held_str ());
  CHECK (unit_pgbuf_count (0, 330, CNT_COND_REFUSED) == 1);
  CHECK (rc == ER_PB_BAD_PAGEID);
  CHECK (holds (&w_in, 520));
  CHECK_FALSE (unfixed (&w_in));
  CHECK (held () == std::vector<int> ({ 520 }));
}

/* ================================================================ L: heap_page_list_fix_next () under another held page
 * (registered where it is linked: new in 3328953d3) */

L2_CASE (l1_defect,
	 "PR8497 GD-01 L1: the innermost fixed scan holds page 330 of heap B; the select-list subquery's sector walk "
	 "(heap A, sorts first) asks heap_page_list_fix_next () for page 520, the conditional latch is refused (another "
	 "thread's write latch), page 330 is let go, and page 520 is deallocated while the fix waits -- the function "
	 "passes on to page 521 and returns S_SUCCESS while the inner scan's watcher holds no page (PASS = defect "
	 "present)", "[defect]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold_listed (&w_in, &hfid_b, 5, 10);	/* page 330 */
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });	/* pages 520, 521, 530 */
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, off, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 1);
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 521);
  CHECK (holds (&w_sub, 521));
  CHECK (er_errid () == NO_ERROR);	/* the warning was cleared */
  CHECK (unfixed (&w_in));
  CHECK (w_in.pgptr == nullptr);	/* the defect: the inner scan's page is not held */
  CHECK (unit_pgbuf_count (0, 330, CNT_FIXES) == 0);
  CHECK (held () == std::vector<int> ({ 521 }));
}

L2_CASE (l1b_defect,
	 "PR8497 GD-01 L1b: the same with the deallocating writer queued behind another reader of page 520 "
	 "(waiter_exists refuses a new reader, page_buffer.c:7029-7037) -- S_SUCCESS on page 521, page 330 not held "
	 "(PASS = defect present)", "[defect]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold_listed (&w_in, &hfid_b, 5, 10);
  unit_pgbuf_set_other (0, 520, OTHER_WRITER_WAITS, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 1);
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 521);
  CHECK (unfixed (&w_in));
  CHECK (w_in.pgptr == nullptr);
  CHECK (held () == std::vector<int> ({ 521 }));
}

L2_CASE (l2_defect,
	 "PR8497 GD-01 L2: the same with page 520 deallocated and allocated again as the file's table page while the fix "
	 "waits -- S_SUCCESS on page 521, page 330 not held (PASS = defect present)", "[defect]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold_listed (&w_in, &hfid_b, 5, 10);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_REUSE_FTAB);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 521);
  CHECK (er_errid () == NO_ERROR);
  CHECK (unfixed (&w_in));
  CHECK (w_in.pgptr == nullptr);
  CHECK (held () == std::vector<int> ({ 521 }));
}

L2_CASE (l3_contract,
	 "L3 control: the same walk when the other thread lets page 520 go as it was -- S_SUCCESS on page 520, page 330 "
	 "fixed again with page_was_unfixed (the outer scan's LSA check reads it again, scan_manager.c:5736)",
	 "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold_listed (&w_in, &hfid_b, 5, 10);
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_RELEASE);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 520);
  CHECK (off == 9);
  CHECK (holds (&w_sub, 520));
  CHECK (holds (&w_in, 330));
  CHECK (unfixed (&w_in));
  CHECK (held () == std::vector<int> ({ 330, 520 }));
}

L2_CASE (l4_contract,
	 "L4 group order: the inner scan holds page 520 of heap A and the subquery walks heap B (sorts after): page 330 "
	 "refused, then deallocated while the fix waits -- the walk passes on to page 331 and page 520 stays held, never "
	 "let go", "[contract]")
{
  setup ();
  PGBUF_WATCHER w_in, w_sub;
  hold_listed (&w_in, &hfid_a, 8, 8);	/* page 520 */
  unit_pgbuf_set_other (0, 330, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_b);
  FILE_PARTIAL_SECTOR s = sector_of (5, { 10, 11 });	/* pages 330, 331 */
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_b, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (unit_pgbuf_count (0, 330, CNT_COND_REFUSED) == 1);
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 331);
  CHECK (holds (&w_in, 520));
  CHECK_FALSE (unfixed (&w_in));
  CHECK (held () == std::vector<int> ({ 331, 520 }));
}

L2_CASE (l5_contract,
	 "L5 no other page held: a listed page another thread deallocates while the fix waits is passed over -- "
	 "S_SUCCESS on the next listed page, the warning cleared (the PR's skip)", "[contract]")
{
  setup ();
  PGBUF_WATCHER w_sub;
  unit_pgbuf_set_other (0, 520, OTHER_WRITE, THEN_DEALLOC);
  PGBUF_INIT_WATCHER (&w_sub, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });
  size_t off = 0;
  VPID got;
  VPID_SET_NULL (&got);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w_sub, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (unit_pgbuf_count (0, 520, CNT_COND_REFUSED) == 0);	/* nothing held: an unconditional fix */
  CHECK (unit_pgbuf_count (0, 520, CNT_WAITED) == 1);
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 521);
  CHECK (off == 10);
  CHECK (er_errid () == NO_ERROR);
  CHECK (held () == std::vector<int> ({ 521 }));
}

/* ================================================================ W: heap_page_list_fix_next () contract (no other page) */

L2_CASE (w1_contract,
	 "W1 the walk: the listed pages of the sector in bit order, each fixed once; *pgoffset past the page returned; "
	 "S_END past the last listed page with that page still held", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 3, 9, 63 });	/* pages 515, 521, 575 */
  size_t off = 0;
  VPID got;
  std::vector<int> pages, offs;
  SCAN_CODE code;

  while ((code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got)) == S_SUCCESS)
    {
      pages.push_back (got.pageid);
      offs.push_back ((int) off);
      CHECK (holds (&w, got.pageid));
      CHECK (held () == std::vector<int> ({ got.pageid }));
      if (pages.size () > 4)
	{
	  break;
	}
    }
  CHECK (code == S_END);
  CHECK (pages == std::vector<int> ({ 515, 521, 575 }));
  CHECK (offs == std::vector<int> ({ 4, 10, 64 }));
  CHECK (off == 64);
  CHECK (holds (&w, 575));
  CHECK (unit_pgbuf_count (0, 515, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_count (0, 521, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_count (0, 575, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_unknown_fixes () == 0);
}

L2_CASE (w2_contract,
	 "W2 the file header page (vfid.fileid) is passed over without a fix; the heap header page (hpgid) is returned "
	 "like a data page", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_b);
  FILE_PARTIAL_SECTOR s = sector_of (5, { 0, 1, 10 });	/* pages 320 (file header), 321 (heap header), 330 */
  size_t off = 0;
  VPID got;
  SCAN_CODE c1 = heap_page_list_fix_next (thr, &hfid_b, &s, &off, &w, &got);
  int p1 = got.pageid;
  SCAN_CODE c2 = heap_page_list_fix_next (thr, &hfid_b, &s, &off, &w, &got);
  int p2 = got.pageid;
  CHECK (c1 == S_SUCCESS);
  CHECK (p1 == 321);
  CHECK (c2 == S_SUCCESS);
  CHECK (p2 == 330);
  CHECK (unit_pgbuf_count (0, 320, CNT_FIX_CALLS) == 0);
}

L2_CASE (w3_contract,
	 "W3 a bestspace page (HEAP_PAGE_FLAG_BESTSPACE) and a page of another ordered type (PAGE_OVERFLOW; the release "
	 "build passes its assert) are fixed, let go and passed over -- the next heap page is returned, nothing else "
	 "held", "[contract]")
{
  setup ();
  ut::bestspace.insert (unit_pgbuf_page_ptr (0, 520));
  unit_pgbuf_set_ptype (0, 521, PAGE_OVERFLOW);
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });	/* 520 bestspace, 521 overflow, 530 */
  size_t off = 0;
  VPID got;
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 530);
  CHECK (off == 19);
  CHECK (unit_pgbuf_count (0, 520, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_count (0, 521, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_count (0, 520, CNT_FIXES) == 0);
  CHECK (unit_pgbuf_count (0, 521, CNT_FIXES) == 0);
  CHECK (held () == std::vector<int> ({ 530 }));
}

L2_CASE (w4_contract,
	 "W4 a listed page deallocated since the bitmap (PAGE_UNKNOWN, nobody on it) or reused as the file's table page "
	 "(PAGE_FTAB) is passed over and the ER_PB_BAD_PAGEID warning cleared", "[contract]")
{
  setup ();
  unit_pgbuf_set_ptype (0, 520, PAGE_UNKNOWN);
  unit_pgbuf_set_ptype (0, 521, PAGE_FTAB);
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 8, 9, 18 });
  size_t off = 0;
  VPID got;
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got);
  CAPTURE ((int) code, got.pageid, held_str ());
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 530);
  CHECK (er_errid () == NO_ERROR);
  CHECK (unit_pgbuf_count (0, 520, CNT_FIX_CALLS) == 1);
  CHECK (unit_pgbuf_count (0, 521, CNT_FIX_CALLS) == 1);
  CHECK (held () == std::vector<int> ({ 530 }));
}

L2_CASE (w5_contract,
	 "W5 the held page is let go before the next page is fixed: the next fix holds nothing else, so a busy page is "
	 "waited for unconditionally, not refused (heap_file.c:2912-2915 comment: holding it while waiting deadlocks "
	 "with vacuum)", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 3, 9 });	/* 515, 521 */
  size_t off = 0;
  VPID got;
  REQUIRE (heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got) == S_SUCCESS);
  REQUIRE (got.pageid == 515);
  unit_pgbuf_set_other (0, 521, OTHER_WRITE, THEN_RELEASE);
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got);
  CHECK (code == S_SUCCESS);
  CHECK (got.pageid == 521);
  CHECK (unit_pgbuf_count (0, 521, CNT_COND_REFUSED) == 0);
  CHECK (unit_pgbuf_count (0, 521, CNT_WAITED) == 1);
  CHECK (unit_pgbuf_count (0, 515, CNT_FIXES) == 0);
  CHECK (held () == std::vector<int> ({ 521 }));
}

L2_CASE (w6_contract,
	 "W6 an interrupt during the fix is S_ERROR with ER_INTERRUPTED kept and *pgoffset left on the page; the page "
	 "held before was already let go", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 3, 9 });
  size_t off = 0;
  VPID got;
  REQUIRE (heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got) == S_SUCCESS);
  unit_pgbuf_interrupt_next_fix ();
  SCAN_CODE code = heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got);
  CHECK (code == S_ERROR);
  CHECK (er_errid () == ER_INTERRUPTED);
  CHECK (off == 9);
  CHECK (w.pgptr == nullptr);
  CHECK (held ().empty ());
}

L2_CASE (w7_contract,
	 "W7 (current behavior) S_END keeps the held page only when no listed page is left to try: with nothing left it "
	 "is still held, but when the pages left were all passed over it was let go at the first of them", "[contract]")
{
  setup ();
  PGBUF_WATCHER w;
  PGBUF_INIT_WATCHER (&w, PGBUF_ORDERED_HEAP_NORMAL, &hfid_a);
  FILE_PARTIAL_SECTOR s = sector_of (8, { 3 });
  size_t off = 0;
  VPID got;
  REQUIRE (heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got) == S_SUCCESS);
  CHECK (heap_page_list_fix_next (thr, &hfid_a, &s, &off, &w, &got) == S_END);
  CHECK (holds (&w, 515));

  unit_pgbuf_set_ptype (0, 520, PAGE_UNKNOWN);
  FILE_PARTIAL_SECTOR s2 = sector_of (8, { 8 });
  size_t off2 = 0;
  CHECK (heap_page_list_fix_next (thr, &hfid_a, &s2, &off2, &w, &got) == S_END);
  CHECK (w.pgptr == nullptr);
  CHECK (held ().empty ());
  CHECK (off2 == 64);
}
