/* PR#8718 (CBRD-28283) a4a513237 -- the search keys of an index scan across its passes, src/query/scan_manager.c.
 *
 * A pass that stops before its last key's end (a MIN/MAX pass at its first row, the inner of a NL semi/anti join at
 * its first match) leaves the search keys made, and a multi-column key owns a buffer (midxkey).  base released them in
 * scan_reset_scan_block () and scan_switch_index_partition () (CBRD-28203) but not when a MIN/MAX scan turns around
 * (S_GO_BACKWARD): the second pass's first open overwrote them with db_make_null -- a leak per execution.  PR#8718
 * releases them at the first open of the next pass (scan_get_index_oidset (), curr_keyno == -1), drops the two
 * CBRD-28203 calls, makes scan_clear_index_key_vals () (scan_end_scan ()) release key_cnt keys whatever curr_keyno,
 * and sets key_cnt 0 where key_vals is dropped (scan_close_scan (), the open's failure).
 *
 * Every case drives the real callers and counts the key buffers alive: whichever way a scan goes from pass to pass
 * and ends, one pass's keys at most are alive, none after scan_end_scan (), none freed twice (#063: the only
 * allocations on these paths are the keys -- the OID buffer is the test's, no covering list, no multi range opt).
 *
 * The real code under test: scan_next_index_scan () (static: unstatic'd by the seam), call_get_next_index_oidset (),
 * scan_get_index_oidset (), check_key_vals () and its sorts, scan_reset_scan_block (), scan_switch_index_partition ()
 * (*btid_int_cached, so no root page fix), scan_end_scan (), scan_clear_index_key_vals (), scan_init_iss (),
 * scan_init_index_key_limit (), scan_close_scan (), scan_open_index_scan () up to its root page fix.
 *
 * Fakes:
 *  - scan_regu_key_to_index_key () (seam): the search key of KEY_RANGE i from kb::query[i], an IN list item: key1 a
 *    DB_TYPE_MIDXKEY owning a malloc'd buffer (need_clear) -- scan_dbvals_to_midxkey () -- key2 its clone
 *    (pr_clone_value (), a second buffer), range EQ_NA turned GE_LE, num_index_term 1, the bounds copied: the real
 *    one's outputs for an equality on the first column of a multi-column index.
 *  - btree_prepare_bts () / btree_range_scan (): one row per value (OID pageid = the value), all of a key range in
 *    one call, in the scan's direction; the range ends (BTREE_END_OF_SCAN).
 *  - scan_next_index_lookup_heap () (seam): the row qualifies (S_SUCCESS).
 *  - pr_midxkey_compare_resolved (): the integer comparison of the first column.
 *  - pr_clear_value (): the real one's NULL / MIDXKEY branches: frees the buffer when need_clear, the value NULL,
 *    need_clear false; a pointer under 4096 (a slot of a NULL key_vals) is counted, not dereferenced.
 *  - scan_switch_index_attr_caches () (seam): NO_ERROR -- the attribute caches move to the partition, no key.
 *  - heap_scancache_switch_class () / heap_scancache_end (): NO_ERROR.  btree_scan_clear_key (): the real one's
 *    clear of bts->cur_key (never set here).  btree_range_scan_free_matched_idx (): nothing.
 *  - pgbuf_fix_release (): NULL (the root page fix of a reopen fails).  logtb_find_current_isolation (): read
 *    committed.  css_get_max_conn (): 100 (MAX_NTRANS of the OID buffer reuse list scan_close_scan () returns to).
 *  - prm_Def: a zeroed table of the real size (asm label), read by scan_open_index_scan ()'s static initializer.
 * SCAN_ID / INDX_INFO / thread entry calloc'd, never constructed: opened and started as scan_open_index_scan () and
 * scan_start_scan () leave a non-covering serial index scan (curr_keyno -1, S_BEFORE, ahead_next INT_MAX).
 *
 * min_max_only_desc_pass is set where it exists (SFINAE); the cases here do not depend on the key order.
 */
#define CATCH_CONFIG_ENABLE_PAIR_STRINGMAKER
#include "catch.hpp"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "scan_manager.h"
#include "btree.h"
#include "btree_load.h"
#include "object_primitive.h"
#include "domain_rules.h"
#include "heap_file.h"
#include "page_buffer.h"
#include "system_parameter.h"
#include "log_impl.h"
#include "thread_entry.hpp"

/* static in scan_manager.c: unstatic'd by the seam (the real code runs) */
extern SCAN_CODE scan_next_index_scan (THREAD_ENTRY * thread_p, SCAN_ID * scan_id);

/* scan_open_index_scan ()'s static initializer reads oracle_style_empty_string: a zeroed table of the real size */
alignas (16) unsigned char ut_prm_def_bytes[sizeof (SYSPRM_PARAM) * (PRM_LAST_ID + 1)] __asm__ ("prm_Def");

namespace kb
{
  std::vector<int> query;	/* KEY_RANGE i: IN list item query[i] */
  std::set<void *> live;	/* midxkey buffers alive */
  int bad_frees;		/* a buffer freed that is not alive */
  int near_null_clears;		/* pr_clear_value () of a slot of a NULL key_vals */
  int keys_made;
  int root_fixes;

  cubthread::entry *thr;
  SCAN_ID *sid;
  INDX_INFO *indx;
  KEY_RANGE *ranges;
  KEY_VAL_RANGE *vals;
  int n_vals;
  DOMAIN_KEY_BOUND kbound;
  DOMAIN_KEY_RANGES kranges;
  BTREE_ISCAN_OID_LIST *oid_list;
  OID cls1, cls2;
  HFID hfid1, hfid2;
  BTID_INT btid_int;

  struct
  {
    int lo, hi;
    bool desc;
    INDX_SCAN_ID *isid;
  } cur;

  template < typename T > auto set_desc_pass_impl (T * isid, bool v, int)->decltype (isid->min_max_only_desc_pass = v,
										   bool ())
  {
    isid->min_max_only_desc_pass = v;
    return true;
  }
  template < typename T > bool set_desc_pass_impl (T *, bool, long)
  {
    return false;
  }
  bool set_desc_pass (INDX_SCAN_ID * isid, bool v)
  {
    return set_desc_pass_impl (isid, v, 0);
  }

  void make_key (DB_VALUE * v, int x)
  {
    DB_MIDXKEY m;
    char *buf = (char *) malloc (16);
    memset (buf, 0, 16);
    memcpy (buf, &x, sizeof (x));
    memset (&m, 0, sizeof (m));
    m.size = 16;
    m.ncolumns = 1;
    m.buf = buf;
    m.domain = NULL;
    m.min_max_val.position = -1;
    m.min_max_val.type = MIN_COLUMN;
    db_make_midxkey (v, &m);
    v->need_clear = true;
    live.insert (buf);
  }

  int key_int (const DB_VALUE * v)
  {
    int x;
    memcpy (&x, v->data.midxkey.buf, sizeof (x));
    return x;
  }

  /* what is left of the last case: its keys freed by hand (counted out) */
  void release ()
  {
    if (vals != nullptr)
      {
	for (int i = 0; i < n_vals; i++)
	  {
	    pr_clear_value (&vals[i].key1);
	    pr_clear_value (&vals[i].key2);
	  }
	delete[]vals;
	vals = nullptr;
      }
    free (ranges);
    ranges = nullptr;
  }

  /* a non-covering serial index scan of a multi-column index over `cb in (q...)`, opened and started */
  void setup (std::vector<int> q)
  {
    if (thr == nullptr)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));
	sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
	indx = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
      }
    release ();
    query = q;
    live.clear ();
    bad_frees = 0;
    near_null_clears = 0;
    keys_made = 0;
    root_fixes = 0;
    n_vals = (int) q.size ();

    memset (&kbound, 0, sizeof (kbound));
    memset (&kranges, 0, sizeof (kranges));
    ranges = (KEY_RANGE *) calloc (n_vals, sizeof (KEY_RANGE));
    vals = new KEY_VAL_RANGE[n_vals];
    for (int i = 0; i < n_vals; i++)
      {
	ranges[i].range = EQ_NA;
	ranges[i].key1_bound = &kbound;
	ranges[i].key2_bound = &kbound;
	vals[i].range = NA_NA;
	db_make_null (&vals[i].key1);
	db_make_null (&vals[i].key2);
	vals[i].is_truncated = false;
	vals[i].num_index_term = 0;
      }

    memset (indx, 0, sizeof (INDX_INFO));
    indx->range_type = R_KEYLIST;
    indx->key_info.key_cnt = n_vals;
    indx->key_info.key_ranges = ranges;
    indx->key_info.key_vals = vals;
    indx->key_ranges_compares = &kranges;
    indx->btid.root_pageid = 10;

    cls1.volid = 0;
    cls1.pageid = 1;
    cls1.slotid = 1;
    cls2 = cls1;
    cls2.pageid = 2;
    memset (&hfid1, 0, sizeof (hfid1));
    memset (&hfid2, 0, sizeof (hfid2));
    memset (&btid_int, 0, sizeof (btid_int));

    /* a fresh OID buffer each case: scan_close_scan () hands the scan's to the server-wide reuse list */
    oid_list = (BTREE_ISCAN_OID_LIST *) calloc (1, sizeof (BTREE_ISCAN_OID_LIST));
    oid_list->oidp = (OID *) calloc (64, sizeof (OID));
    oid_list->capacity = 64;
    oid_list->max_oid_cnt = 64;

    memset (sid, 0, sizeof (SCAN_ID));
    sid->type = S_INDX_SCAN;
    sid->status = S_STARTED;
    sid->position = S_BEFORE;
    sid->direction = S_FORWARD;
    sid->single_fetch = QPROC_NO_SINGLE_INNER;
    sid->s.isid.indx_info = indx;
    sid->s.isid.key_vals = vals;
    sid->s.isid.key_cnt = n_vals;
    sid->s.isid.curr_keyno = -1;
    sid->s.isid.curr_oidno = -1;
    sid->s.isid.key_limit_lower = -1;
    sid->s.isid.key_limit_upper = -1;
    sid->s.isid.cls_oid = cls1;
    sid->s.isid.oid_list = oid_list;
    sid->s.isid.curr_oidp = oid_list->oidp;
    sid->s.isid.ahead_next = INT_MAX;	/* scan_open_index_scan () */
    sid->s.isid.ahead_distance = 0;
    set_desc_pass (&sid->s.isid, false);
  }

  /* the next row of the scan: its value, 0 at S_END, -1 on error */
  int next ()
  {
    SCAN_CODE sc = scan_next_index_scan (thr, sid);
    if (sc == S_END)
      {
	return 0;
      }
    if (sc != S_SUCCESS)
      {
	return -1;
      }
    return sid->s.isid.curr_oidp->pageid;
  }

  /* the rest of the pass, to S_END */
  std::vector<int> to_end ()
  {
    std::vector<int> rows;
    for (int guard = 0; guard < 64; guard++)
      {
	int v = next ();
	if (v <= 0)
	  {
	    break;
	  }
	rows.push_back (v);
      }
    return rows;
  }

  /* qexec_intprt_fnc () turning a MIN/MAX scan around after the MIN pass took its row */
  void turn_around ()
  {
    indx->use_desc_index = !indx->use_desc_index;
    set_desc_pass (&sid->s.isid, !!indx->use_desc_index);
    /* bts_reset_scan () */
    sid->s.isid.bt_scan.is_scan_started = false;
    VPID_SET_NULL (&sid->s.isid.bt_scan.C_vpid);
    sid->position = S_GO_BACKWARD;
  }

  /* the inner scan of a NL join, for the next outer row (qexec_reset_sa_inner_scan_block ()) */
  SCAN_CODE rewind ()
  {
    return scan_reset_scan_block (thr, sid);
  }

  /* the inner scan of a HASH partitioned class moves to the next partition while started */
  int switch_partition (bool *started)
  {
    bool cached = true;

    sid->s.isid.scancache_inited = true;
    sid->s.isid.caches_inited = true;
    return scan_switch_index_partition (thr, sid, &cls2, &hfid2, &btid_int, &cached, false, started);
  }

  /* qexec_open_scan () reopening the closed scan; the root page fix fails */
  int reopen_failing_at_root ()
  {
    return scan_open_index_scan (thr, sid, false, false, false, S_SELECT, 0, 0, QPROC_NO_SINGLE_INNER, NULL, NULL,
				 NULL, indx, &cls1, &hfid1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0, NULL,
				 NULL, 0, NULL, NULL, 0, NULL, NULL, 0, NULL, NULL, false, 0, true);
  }
}

/* ---- fakes ---- */

int
scan_regu_key_to_index_key (THREAD_ENTRY * thread_p, KEY_RANGE * key_ranges, KEY_VAL_RANGE * key_val_range,
			    INDX_SCAN_ID * iscan_id, TP_DOMAIN * btree_domainp, VAL_DESCR * vd, int key_range_idx)
{
  (void) thread_p;
  (void) iscan_id;
  (void) btree_domainp;
  (void) vd;
  key_val_range->key1_bound = key_ranges->key1_bound;
  key_val_range->key2_bound = key_ranges->key2_bound;
  key_val_range->num_index_term = 1;
  kb::make_key (&key_val_range->key1, kb::query[key_range_idx]);
  kb::make_key (&key_val_range->key2, kb::query[key_range_idx]);	/* pr_clone_value () of key1 */
  kb::keys_made++;
  if (key_val_range->range == EQ_NA)
    {
      key_val_range->range = GE_LE;
    }
  return NO_ERROR;
}

int
btree_prepare_bts (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTID * btid, INDX_SCAN_ID * index_scan_id_p,
		   key_val_range * kv_range, FILTER_INFO * filter, const OID * match_class_oid,
		   DB_BIGINT * key_limit_upper, DB_BIGINT * key_limit_lower, bool need_to_check_null, void *bts_other)
{
  (void) thread_p;
  (void) btid;
  (void) filter;
  (void) match_class_oid;
  (void) key_limit_upper;
  (void) key_limit_lower;
  (void) need_to_check_null;
  (void) bts_other;
  kb::cur.lo = kb::key_int (&kv_range->key1);
  kb::cur.hi = kb::key_int (&kv_range->key2);
  kb::cur.desc = index_scan_id_p->indx_info->use_desc_index != 0;
  kb::cur.isid = index_scan_id_p;
  bts->use_desc_index = kb::cur.desc;
  return NO_ERROR;
}

int
btree_range_scan (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTREE_RANGE_SCAN_PROCESS_KEY_FUNC * key_func)
{
  int n = 0;

  (void) thread_p;
  (void) key_func;
  for (int k = 0; k <= kb::cur.hi - kb::cur.lo; k++)
    {
      int v = kb::cur.desc ? kb::cur.hi - k : kb::cur.lo + k;
      kb::cur.isid->oid_list->oidp[n].volid = 0;
      kb::cur.isid->oid_list->oidp[n].pageid = v;
      kb::cur.isid->oid_list->oidp[n].slotid = 1;
      n++;
    }
  kb::cur.isid->oid_list->oid_cnt = n;
  bts->n_oids_read_last_iteration = n;
  bts->C_vpid.pageid = NULL_PAGEID;
  bts->O_vpid.pageid = NULL_PAGEID;
  bts->is_scan_started = false;
  return NO_ERROR;
}

SCAN_CODE
scan_next_index_lookup_heap (THREAD_ENTRY * thread_p, SCAN_ID * scan_id, INDX_SCAN_ID * isidp,
			     FILTER_INFO * data_filter, TRAN_ISOLATION isolation)
{
  (void) thread_p;
  (void) scan_id;
  (void) isidp;
  (void) data_filter;
  (void) isolation;
  return S_SUCCESS;
}

DB_VALUE_COMPARE_RESULT
pr_midxkey_compare_resolved (DB_MIDXKEY * mul1, DB_MIDXKEY * mul2, int do_coercion, int total_order,
			     int num_index_term, int *start_colp, int *diff_column, bool * dom_is_desc,
			     int *result_size, PR_MIDXKEY_ELEMENT_COMPARE element_compare, const void *element_context)
{
  int a, b;

  (void) do_coercion;
  (void) total_order;
  (void) num_index_term;
  (void) start_colp;
  (void) dom_is_desc;
  (void) result_size;
  (void) element_compare;
  (void) element_context;
  memcpy (&a, mul1->buf, sizeof (a));
  memcpy (&b, mul2->buf, sizeof (b));
  if (diff_column != NULL)
    {
      *diff_column = (a == b) ? 1 : 0;
    }
  return a < b ? DB_LT : (a > b ? DB_GT : DB_EQ);
}

int
pr_clear_value (DB_VALUE * value)
{
  if (value == NULL)
    {
      return NO_ERROR;
    }
  if ((uintptr_t) value < 4096)
    {
      kb::near_null_clears++;	/* &key_vals[i].key1 of a NULL key_vals */
      return NO_ERROR;
    }
  if (DB_IS_NULL (value))
    {
      return NO_ERROR;
    }
  if (DB_VALUE_DOMAIN_TYPE (value) == DB_TYPE_MIDXKEY)
    {
      char *buf = value->data.midxkey.buf;
      if (buf != NULL)
	{
	  if (value->need_clear)
	    {
	      if (kb::live.erase (buf) == 1)
		{
		  free (buf);
		}
	      else
		{
		  kb::bad_frees++;
		}
	    }
	  value->data.midxkey.buf = NULL;
	}
    }
  PRIM_SET_NULL (value);
  value->need_clear = false;
  return NO_ERROR;
}

int
scan_switch_index_attr_caches (THREAD_ENTRY * thread_p, INDX_SCAN_ID * isidp, OID * cls_oid)
{
  (void) thread_p;
  (void) isidp;
  (void) cls_oid;
  return NO_ERROR;
}

int
heap_scancache_switch_class (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
			     const OID * class_oid, bool lock_held)
{
  (void) thread_p;
  (void) scan_cache;
  (void) hfid;
  (void) class_oid;
  (void) lock_held;
  return NO_ERROR;
}

int
heap_scancache_end (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  (void) thread_p;
  (void) scan_cache;
  return NO_ERROR;
}

void
btree_scan_clear_key (BTREE_SCAN * btree_scan)
{
  btree_clear_key_value (&btree_scan->clear_cur_key, &btree_scan->cur_key);
  btree_scan->is_cur_key_compressed = false;
}

void
btree_range_scan_free_matched_idx (BTREE_SCAN * bts)
{
  (void) bts;
}

PAGE_PTR
pgbuf_fix_release (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_FETCH_MODE fetch_mode,
		   PGBUF_LATCH_MODE requestmode, PGBUF_LATCH_CONDITION condition)
{
  (void) thread_p;
  (void) vpid;
  (void) fetch_mode;
  (void) requestmode;
  (void) condition;
  kb::root_fixes++;
  return NULL;
}

TRAN_ISOLATION
logtb_find_current_isolation (THREAD_ENTRY * thread_p)
{
  (void) thread_p;
  return TRAN_READ_COMMITTED;
}

int
css_get_max_conn (void)
{
  return 100;
}

/* ---- cases ---- */

TEST_CASE ("K1 a MIN/MAX scan turning around (S_GO_BACKWARD) over IN (1, 5, 9) of a multi-column index, not "
	   "covering: the MIN pass stops at its first row with its 6 key buffers, the second pass's first open "
	   "releases them and makes its own -- 6 alive, not 12; scan_end_scan () leaves none (base: 12, then 6 left)",
	   "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);	/* MIN */
  CHECK (kb::live.size () == 6);

  kb::turn_around ();
  CHECK (kb::next () > 0);	/* MAX (the key order is suite key_range_order's) */
  CHECK (kb::keys_made == 6);
  CHECK (kb::live.size () == 6);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K2 the inner scan of a NL semi/anti join rewound for three outer rows (scan_reset_scan_block ()), each "
	   "pass stopping at its first match: one pass's keys alive after each pass (6), none after scan_end_scan ()",
	   "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  for (int outer = 0; outer < 3; outer++)
    {
      if (outer > 0)
	{
	  REQUIRE (kb::rewind () == S_SUCCESS);
	}
      CHECK (kb::next () == 1);
      CHECK (kb::live.size () == 6);
    }
  CHECK (kb::keys_made == 9);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K3 boundary: a rewind after a stopped pass and then the end of the scan with no pass in between -- "
	   "scan_end_scan () releases the keys though curr_keyno is -1 (head: the end releases key_cnt keys whatever "
	   "curr_keyno; base: the rewind released them)", "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);
  REQUIRE (kb::rewind () == S_SUCCESS);
  CHECK (kb::sid->s.isid.curr_keyno == -1);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K4 the inner scan of a HASH partitioned class moving to the next partition while started "
	   "(scan_switch_index_partition ()) after a stopped pass: the next partition's pass releases the last one's "
	   "keys at its first open -- 6 alive -- and scan_end_scan () leaves none", "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);
  CHECK (kb::live.size () == 6);

  bool started = false;
  REQUIRE (kb::switch_partition (&started) == NO_ERROR);
  REQUIRE (started);
  CHECK (kb::sid->s.isid.curr_keyno == -1);
  CHECK (kb::sid->status == S_STARTED);

  CHECK (kb::next () == 1);
  CHECK (kb::keys_made == 6);
  CHECK (kb::live.size () == 6);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K5 boundary: a partition switch after a stopped pass and then the end of the scan with no pass on the "
	   "new partition: scan_end_scan () releases the keys (base: the switch released them)", "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);
  bool started = false;
  REQUIRE (kb::switch_partition (&started) == NO_ERROR);
  REQUIRE (started);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K6 a pass read to its last key's end (S_END) releases the keys itself: none alive before scan_end_scan "
	   "(), which then frees nothing twice; a rewind and a full second pass do the same", "[contract]")
{
  kb::setup ({ 9, 1, 5 });

  CHECK (kb::to_end () == std::vector<int> ({ 1, 5, 9 }));
  CHECK (kb::live.empty ());

  REQUIRE (kb::rewind () == S_SUCCESS);
  CHECK (kb::to_end () == std::vector<int> ({ 1, 5, 9 }));
  CHECK (kb::live.empty ());

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K7 the last pass stops inside the key list and no pass follows: scan_end_scan () releases its keys",
	   "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);
  CHECK (kb::next () == 5);
  CHECK (kb::live.size () == 6);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}

TEST_CASE ("K8 a closed scan whose reopen fails at the root page fix: the error's scan_end_scan () touches no key "
	   "slot -- scan_close_scan () dropped key_vals (base: key_cnt stays 3 and curr_keyno 1, the end reads 6 slots "
	   "of the NULL key_vals, which the real pr_clear_value () would crash on)", "[contract]")
{
  kb::setup ({ 1, 5, 9 });

  CHECK (kb::next () == 1);	/* the last pass stopped: curr_keyno inside the list */
  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::live.empty ());
  scan_close_scan (kb::thr, kb::sid);
  CHECK (kb::sid->status == S_CLOSED);
  CHECK (kb::sid->s.isid.key_vals == NULL);

  CHECK (kb::reopen_failing_at_root () == ER_FAILED);
  CHECK (kb::root_fixes == 1);
  CHECK (kb::sid->status == S_OPENED);

  scan_end_scan (kb::thr, kb::sid);
  CHECK (kb::near_null_clears == 0);
  CHECK (kb::live.empty ());
  CHECK (kb::bad_frees == 0);
}
