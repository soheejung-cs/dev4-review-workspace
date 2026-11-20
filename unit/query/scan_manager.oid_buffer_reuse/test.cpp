/* PR#8668 (CBRD-28138) -- the index scan OID buffer an index scan takes from the server-wide reuse list
 * (scan_Iscan_oid_buf_list): its OID count limit (max_oid_cnt) must stay within its capacity after
 * index_scan_oid_buffer_size changes.
 *
 * The real code under test: scan_index_alloc_oid_buffer () (the parallel worker's way in, SERVER_MODE, extern) and
 * through it scan_alloc_iscan_oid_buf_list (), scan_alloc_oid_list (), scan_free_oid_list () and
 * scan_free_iscan_oid_buf_list () (static; reached through the appended hooks).  In develop the caller set
 * max_oid_cnt = ISCAN_OID_BUFFER_COUNT whatever the buffer it got; in the PR the allocation sets it, cut to the
 * capacity, and replaces a reused buffer smaller than the parameter asks.
 *
 * Fakes:
 *  - prm_Def: a zeroed table of the real layout named prm_Def (asm label: its members are const, so it cannot be
 *    defined as an array of SYSPRM_PARAM), the index_scan_oid_buffer_pages / _size entries floats in pages, not
 *    session parameters -- prm_get_float_value () is inline and reads them as the server does.
 *  - db_Io_page_size / db_User_page_size: 16K pages (ISCAN_OID_BUFFER_SIZE / ISCAN_OID_BUFFER_MIN_CAPACITY).
 *  - css_get_max_conn (): 100 (MAX_NTRANS bounds the reuse list).
 */
#include "catch.hpp"

#include <cstring>

#include "scan_manager.h"
#include "btree.h"
#include "storage_common.h"
#include "system_parameter.h"
#include "file_io.h"

/* storage_common.c's UT_RESERVED_SIZE_IN_PAGE (not in a header) */
#define UT_RESERVED_SIZE_IN_PAGE (sizeof (FILEIO_PAGE_RESERVED) + sizeof (FILEIO_PAGE_WATERMARK))

alignas (16) unsigned char ut_prm_def_bytes[sizeof (SYSPRM_PARAM) * (PRM_LAST_ID + 1)] __asm__ ("prm_Def");
PGLENGTH db_Io_page_size = 16384;
PGLENGTH db_User_page_size = 16384 - UT_RESERVED_SIZE_IN_PAGE;

int
css_get_max_conn (void)
{
  return 100;
}

extern int ut_oidbuf_pool_count (void);
extern BTREE_ISCAN_OID_LIST *ut_oidbuf_pool_head (void);
extern void ut_oidbuf_release (BTREE_ISCAN_OID_LIST * oid_list);

namespace oidbuf
{
  SYSPRM_PARAM *
  prm (PARAM_ID id)
  {
    return &((SYSPRM_PARAM *) ut_prm_def_bytes)[id];
  }

  /* index_scan_oid_buffer_size in pages: the server keeps it in index_scan_oid_buffer_pages too (the pair of
   * system_parameter.c, set by its duplicate functions), and ISCAN_OID_BUFFER_SIZE reads that one */
  void
  set_pages (float pages)
  {
    const PARAM_ID ids[2] = { PRM_ID_BT_OID_NBUFFERS, PRM_ID_BT_OID_BUFFER_SIZE };

    for (PARAM_ID id : ids)
      {
	SYSPRM_PARAM *p = prm (id);

	p->id = id;
	p->datatype = PRM_FLOAT;
	p->static_flag = 0;
	p->value.v.f = pages;
      }
  }

  int
  oid_count (float pages)
  {
    return (int) (16384 * pages) / OR_OID_SIZE;
  }

  /* a scan's way to its OID buffer: what the parallel worker calls (scan_index_alloc_oid_buffer ()) */
  BTREE_ISCAN_OID_LIST *
  take (void)
  {
    INDX_SCAN_ID isid;

    memset (&isid, 0, sizeof (isid));
    REQUIRE (scan_index_alloc_oid_buffer (&isid) == NO_ERROR);
    REQUIRE (isid.oid_list != nullptr);
    CHECK (isid.curr_oidp == isid.oid_list->oidp);
    return isid.oid_list;
  }
}

TEST_CASE ("[contract] C1 a buffer reused after the parameter grew: the limit stays within the buffer, and the scan gets a buffer the parameter's size", "[oidbuf]")
{
  oidbuf::set_pages (4.0f);	/* 64K: 8192 OIDs */
  BTREE_ISCAN_OID_LIST *l = oidbuf::take ();
  CHECK (l->capacity == oidbuf::oid_count (4.0f));
  ut_oidbuf_release (l);

  oidbuf::set_pages (16.0f);	/* 256K: 32768 OIDs */
  BTREE_ISCAN_OID_LIST *m = oidbuf::take ();
  CHECK (m->max_oid_cnt <= m->capacity);
  CHECK (m->capacity >= oidbuf::oid_count (16.0f));
  CHECK (m->max_oid_cnt == oidbuf::oid_count (16.0f));
  CHECK (m->oid_cnt == 0);
  ut_oidbuf_release (m);
}

TEST_CASE ("[contract] C2 after the parameter is lowered, a larger reused buffer is kept and the limit is the parameter's", "[oidbuf]")
{
  oidbuf::set_pages (16.0f);
  BTREE_ISCAN_OID_LIST *l = oidbuf::take ();
  REQUIRE (l->capacity >= oidbuf::oid_count (16.0f));
  ut_oidbuf_release (l);

  oidbuf::set_pages (1.0f);	/* 16K: 2048 OIDs */
  BTREE_ISCAN_OID_LIST *m = oidbuf::take ();
  CHECK (m == l);		/* the same buffer, not replaced */
  CHECK (m->max_oid_cnt == oidbuf::oid_count (1.0f));
  CHECK (m->max_oid_cnt <= m->capacity);
  ut_oidbuf_release (m);
}

TEST_CASE ("[contract] C3 a parameter below the minimum capacity (two pages): the limit is the parameter's, within the buffer", "[oidbuf]")
{
  oidbuf::set_pages (0.5f);	/* 8K: 1024 OIDs, under ISCAN_OID_BUFFER_MIN_CAPACITY */
  BTREE_ISCAN_OID_LIST *m = oidbuf::take ();
  CHECK (m->max_oid_cnt == oidbuf::oid_count (0.5f));
  CHECK (m->max_oid_cnt <= m->capacity);
  CHECK (m->capacity >= (2 * (16384 - UT_RESERVED_SIZE_IN_PAGE)) / OR_OID_SIZE);
  ut_oidbuf_release (m);
}

TEST_CASE ("[contract] C4 a reused buffer too small for the parameter is replaced, not leaked: the reuse list loses it and gets the new one back", "[oidbuf]")
{
  oidbuf::set_pages (2.0f);	/* 32K: 4096 OIDs */
  BTREE_ISCAN_OID_LIST *small = oidbuf::take ();
  /* make sure the list's head is this small buffer */
  ut_oidbuf_release (small);
  REQUIRE (ut_oidbuf_pool_head () == small);
  int before = ut_oidbuf_pool_count ();

  oidbuf::set_pages (8.0f);	/* 128K: 16384 OIDs */
  BTREE_ISCAN_OID_LIST *m = oidbuf::take ();
  if (small->capacity < oidbuf::oid_count (8.0f))
    {
      CHECK (m->capacity >= oidbuf::oid_count (8.0f));
    }
  CHECK (m->max_oid_cnt <= m->capacity);
  CHECK (ut_oidbuf_pool_count () == before - 1);
  ut_oidbuf_release (m);
  CHECK (ut_oidbuf_pool_count () == before);
}
