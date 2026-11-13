/* PR#8490 (CBRD-28031) 35a151f37 -- pgbuf_read_ahead (), src/storage/page_buffer.c: the pages it hands the OS.
 *
 * The real code under test: pgbuf_read_ahead () with the real hash function and pgbuf_is_temporary_volume ().  The
 * test fills the pool's hash table through hooks.cpp (appended to page_buffer.c: pgbuf_Pool is static there).
 *
 * Fakes:
 *  - fileio_read_ahead_hint (): records (descriptor, first page, pages, page size) -- the posix_fadvise range.
 *  - fileio_get_volume_descriptor (): 100 + volid.
 *  - xdisk_get_purpose (): DB_TEMPORARY_DATA_PURPOSE for the volumes the case names, else permanent.  log_Gl is
 *    zeroed (LOG_RESTARTED), so pgbuf_is_temporary_volume () asks it (page_buffer.c:6185-6194).
 *  - db_Io_page_size (IO_PAGESIZE): 16 KiB.
 */
#include "catch.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "page_buffer.h"
#include "file_io.h"
#include "storage_common.h"
#include "xserver_interface.h"

/* hooks.cpp */
extern void unit_pgbuf_reset (void);
extern void unit_pgbuf_put (short volid, int pageid);
extern unsigned int unit_pgbuf_hash (short volid, int pageid);
extern int unit_pgbuf_chain_length (short volid, int pageid);

namespace pb
{
  struct range
  {
    int fd;
    int page;
    int n;
    size_t page_size;
  };
  std::vector<range> hints;
  std::set<short> temp_volumes;
  int purpose_calls;

  void
  reset ()
  {
    hints.clear ();
    temp_volumes.clear ();
    purpose_calls = 0;
    unit_pgbuf_reset ();
  }

  VPID
  vp (short volid, int pageid)
  {
    VPID v;
    v.volid = volid;
    v.pageid = pageid;
    return v;
  }

  void
  read_ahead (std::vector<VPID> pages)
  {
    pgbuf_read_ahead (NULL, pages.data (), (int) pages.size ());
  }

  /* the hints as (volume, first page, pages) */
  bool
  hints_are (std::vector<std::vector<int>> want)
  {
    if (hints.size () != want.size ())
      {
	return false;
      }
    for (size_t i = 0; i < want.size (); i++)
      {
	if (hints[i].fd != 100 + want[i][0] || hints[i].page != want[i][1] || hints[i].n != want[i][2]
	    || hints[i].page_size != (size_t) IO_PAGESIZE)
	  {
	    return false;
	  }
      }
    return true;
  }

  std::string
  dump ()
  {
    std::string s;
    char b[64];
    for (const range &r : hints)
      {
	snprintf (b, sizeof (b), "(vol %d, page %d, %d) ", r.fd - 100, r.page, r.n);
	s += b;
      }
    return s;
  }
}

/* ---- fakes ---- */

PGLENGTH db_Io_page_size = 16 * 1024;

void
fileio_read_ahead_hint (int vol_fd, PAGEID page_id, int num_pages, size_t page_size)
{
  pb::hints.push_back ({ vol_fd, page_id, num_pages, page_size });
}

int
fileio_get_volume_descriptor (VOLID volid)
{
  return 100 + volid;
}

DISK_VOLPURPOSE
xdisk_get_purpose (THREAD_ENTRY * thread_p, VOLID volid)
{
  (void) thread_p;
  pb::purpose_calls++;
  return pb::temp_volumes.count (volid) ? DB_TEMPORARY_DATA_PURPOSE : DB_PERMANENT_DATA_PURPOSE;
}

/* ---- cases ---- */

TEST_CASE ("P1 consecutive pages of a volume in ascending order go to the OS as one range of IO_PAGESIZE pages", "[contract]")
{
  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11), pb::vp (0, 12), pb::vp (0, 13), pb::vp (0, 14) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 5} }));
}

TEST_CASE ("P2 a page in the buffer is not told and ends the range: the pages before it and after it are two ranges", "[contract]")
{
  pb::reset ();
  unit_pgbuf_put (0, 12);
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11), pb::vp (0, 12), pb::vp (0, 13), pb::vp (0, 14) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 2}, {0, 13, 2} }));

  /* every page in the buffer: nothing told */
  pb::reset ();
  unit_pgbuf_put (0, 10);
  unit_pgbuf_put (0, 11);
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11) });
  CHECK (pb::hints.empty ());

  /* a BCB of another page in the same hash chain (2^20 pages apart) does not hide the page */
  pb::reset ();
  unit_pgbuf_put (0, 10 + (1 << 20));
  REQUIRE (unit_pgbuf_hash (0, 10) == unit_pgbuf_hash (0, 10 + (1 << 20)));
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 2} }));
}

TEST_CASE ("P3 a page of a temporary volume is never told and ends the range; the next permanent page starts a new one", "[contract]")
{
  pb::reset ();
  pb::temp_volumes.insert (3);
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11), pb::vp (3, 5), pb::vp (3, 6), pb::vp (0, 12), pb::vp (0, 13) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 2}, {0, 12, 2} }));
  CHECK (pb::purpose_calls > 0);
}

TEST_CASE ("P4 a gap, a volume change or a descending step ends the range", "[contract]")
{
  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 11), pb::vp (0, 13), pb::vp (0, 14) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 2}, {0, 13, 2} }));

  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (1, 11), pb::vp (1, 12) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 1}, {1, 11, 2} }));

  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 14), pb::vp (0, 13), pb::vp (0, 12) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 14, 1}, {0, 13, 1}, {0, 12, 1} }));
}

TEST_CASE ("P5 the last range is told at the end of the list; one page alone is a range of one; no page, no hint", "[contract]")
{
  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 7) });
  CHECK (pb::hints_are ({ {0, 7, 1} }));

  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 30), pb::vp (0, 7), pb::vp (0, 8), pb::vp (0, 9) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 30, 1}, {0, 7, 3} }));

  pb::reset ();
  pgbuf_read_ahead (NULL, NULL, 0);
  CHECK (pb::hints.empty ());
}

TEST_CASE ("P6 (current behavior) a page listed twice is told twice: once in a row (10 10 11 -> 10:1, 10:2), or after another page (5 6 5 -> 5:2, 5:1)", "[contract]")
{
  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 10), pb::vp (0, 10), pb::vp (0, 11) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 10, 1}, {0, 10, 2} }));

  pb::reset ();
  pb::read_ahead ({ pb::vp (0, 5), pb::vp (0, 6), pb::vp (0, 5) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 5, 2}, {0, 5, 1} }));
}

TEST_CASE ("P7 a hash chain walked 16 BCBs (PGBUF_READ_AHEAD_MAX_CHAIN_STEPS) without the page counts as found: not told, the range ends; 15 others do not", "[contract]")
{
  pb::reset ();
  for (int k = 1; k <= 16; k++)
    {
      unit_pgbuf_put (0, 20 + (k << 20));
    }
  REQUIRE (unit_pgbuf_chain_length (0, 20) == 16);
  pb::read_ahead ({ pb::vp (0, 19), pb::vp (0, 20), pb::vp (0, 21) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 19, 1}, {0, 21, 1} }));

  pb::reset ();
  for (int k = 1; k <= 15; k++)
    {
      unit_pgbuf_put (0, 20 + (k << 20));
    }
  REQUIRE (unit_pgbuf_chain_length (0, 20) == 15);
  pb::read_ahead ({ pb::vp (0, 19), pb::vp (0, 20), pb::vp (0, 21) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 19, 3} }));

  /* the page itself behind 15 others is found at the 16th */
  pb::reset ();
  unit_pgbuf_put (0, 20);
  for (int k = 1; k <= 15; k++)
    {
      unit_pgbuf_put (0, 20 + (k << 20));
    }
  pb::read_ahead ({ pb::vp (0, 19), pb::vp (0, 20), pb::vp (0, 21) });
  CAPTURE (pb::dump ());
  CHECK (pb::hints_are ({ {0, 19, 1}, {0, 21, 1} }));
}
