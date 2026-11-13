/* PR#8490 (CBRD-28031) 35a151f37 -- the parallel index scan worker's look-ahead and an error seen only once.
 *
 * TU: src/query/parallel/px_scan/index/px_scan_index_leaf_slot_walker.cpp (server), with the real
 * overflow_drain_fsm (px_scan_index_overflow_drain_fsm.cpp) and the real scan_index_read_ahead () (scan_manager.c)
 * linked as --extra.  The test drives one leaf the way a worker does: px_scan_task.cpp's loop checks the interrupt,
 * slot_iterator_index::set_page () then next_qualified_slot_with_peek () (px_scan_slot_iterator_index.cpp:62-92)
 * until S_END/S_ERROR, the loop checks again, slot_iterator_index::finalize ().
 *
 * 35a151f37 evaluates the keys after the one being looked up ahead of the lookups (process_oid -> read_ahead ->
 * evaluate_ahead -> next_key (ahead = true)); an error met there is dropped (stop_at_error: er_clear ()) on the
 * premise that the lookups read that key again and meet it again (walker.cpp:552-553).  A user cancel is seen once:
 * whoever checks it clears the transaction's flag (logtb_is_interrupted (thread_p, true, ..),
 * log_tran_table.c:3092-3094).  Review G1-ahead-interrupt-lost (the overflow key's pgbuf_fix) and G23-01 (the key
 * filter's SLEEP) say the cancel is lost there.  4255a337d (base) reads every key in scan_next_slot and raises it.
 *
 * Fakes (read from the code they stand for):
 *  - the transaction's interrupt flag: every engine call below that checks it does what pgbuf_fix () does
 *    (page_buffer.c:2350-2358: logtb_is_interrupted (thread_p, true, ..) -> the flag is cleared, ER_INTERRUPTED,
 *    failure) or db_sleep () (arithmetic.c:5121-5124, the same check).  A "cancel" sets the flag right before one
 *    chosen check (by its number, or the first read / filter of a key, or a lookup).
 *  - btree_read_record (): an overflow key (BTREE_LEAF_RECORD_OVERFLOW_KEY) fixes two overflow pages,
 *    overflow_get_length () and overflow_get () (btree.c:2197, 2211), each a check; on failure the key is NULL and
 *    ER_FAILED (btree.c:2199, 4855-4858).  Every key is a COPY that owns memory (clear_key = true, "we always clear
 *    overflow keys" btree.c:4861); in-page keys fix nothing.  A key may fail every time with a fixed error.
 *  - eval_key_filter (): SLEEP keys make one check per evaluation (db_sleep); a key may be false, or fail every time.
 *  - heap_get_visible_version (): the heap page's pgbuf_fix () is one check; otherwise the row is visible.
 *  - btree_record_process_objects (): the key's leaf OIDs in order (no MVCC snapshot: all visible).
 *  - spage_get_record / btree_node_number_of_keys / btree_leaf_record_is_fence: the leaf's key records, no fence.
 *  - btree_compare_search_key / btree_compare_key_to_bound: integer order of the key's slot number.
 *  - pr_clear_value (): frees the key's buffer when need_clear, then NULL (object_primitive.c:1856-2012); each COPY
 *    key carries a token so a second clear of a copy (double free) and a key never cleared (leak) are seen.
 *  - leaf_page_dispatcher::release_leaf () lets the leaf go; ::signal_chain_ended () records the signal (the leaf is
 *    the frontier: kept) -- px_scan_index_leaf_page_dispatcher.cpp:255-271, 641-655.
 *  - pgbuf_read_ahead (): records the pages told and how many lookups were done when they were told.
 *  - er_set / er_errid / er_clear: one error per thread.
 */
#include "catch.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "px_scan_index_leaf_slot_walker.hpp"
#include "px_scan_index_overflow_drain_fsm.hpp"
#include "px_scan_input_handler_index.hpp"

#include "btree.h"
#include "btree_load.h"
#include "error_manager.h"
#include "heap_file.h"
#include "log_impl.h"
#include "object_primitive.h"
#include "page_buffer.h"
#include "query_evaluator.h"
#include "scan_manager.h"
#include "slotted_page.h"
#include "thread_entry.hpp"

/* 35a151f37's read-ahead; absent (NULL) where the revision has none */
extern void scan_index_read_ahead (THREAD_ENTRY * thread_p, INDX_SCAN_ID * isidp, bool more) __attribute__ ((weak));

namespace wk
{
  const int TOKEN_BASE = 4096;	/* a key value is slot + token * TOKEN_BASE */

  struct fkey
  {
    int slot;
    int n_oids;
    bool ovf_key;		/* the key itself is in overflow pages: a read fixes two pages */
    bool sleep;			/* the key filter calls SLEEP: an interrupt check per evaluation */
    bool filtered;		/* the key filter is false */
    int filter_err;		/* the key filter fails with this error every time */
    int read_err;		/* the key's read fails with this error every time */
  };

  std::vector<fkey> keys;
  int upper;			/* the range's upper bound (INF_LE), 0: INF_INF */
  bool key_filter;

  /* the transaction's interrupt flag and its checks */
  bool tran_interrupt;
  int checks;
  int cancel_at_check;
  int cancel_at_read_of;
  int cancel_at_filter_of;
  int cancel_at_lookup;
  std::string consumed_by;
  int consumed;

  int err;

  std::map<int, int> reads_of;
  std::map<int, int> filters_of;
  int lookup_calls;
  std::vector<OID> looked_up;
  std::vector<int> looked_up_index;	/* INDX_SCAN_ID::curr_oidno at the lookup (35a151f37: its buffer index) */
  struct hint
  {
    int lookups_done;
    std::vector<VPID> vpids;
  };
  std::vector<hint> hints;
  int signals;
  int signal_idx;
  int rows_at_signal;
  int rows;
  int leaf_releases;
  int unfixes;

  std::set<int> live_tokens;
  int next_token;
  int double_clears;

  /* scan state */
  SCAN_ID *sid;
  INDX_INFO *indx;
  OID oid_area[4096];
  BTREE_ISCAN_OID_LIST oid_list;
  char leaf_page[64];
  char pred_token;
  cubthread::entry *thr;
  alignas (parallel_scan::input_handler_index) unsigned char handler_area[sizeof (parallel_scan::input_handler_index)];
  parallel_scan::input_handler_index *handler;

  OID
  oid_of (int slot, int j)
  {
    OID o;
    o.volid = 0;
    o.pageid = 100 + 16 * slot + j;	/* every OID on its own heap page */
    o.slotid = 1;
    return o;
  }

  /* logtb_get_check_interrupt () && logtb_is_interrupted (thread_p, true, ..): true clears the flag */
  bool
  interrupted (const char *who)
  {
    checks++;
    if (cancel_at_check == checks)
      {
	tran_interrupt = true;
      }
    if (tran_interrupt)
      {
	tran_interrupt = false;
	consumed_by = who;
	consumed++;
	return true;
      }
    return false;
  }

  DB_LOGICAL
  dummy_eval (THREAD_ENTRY *, const PRED_EXPR *, val_descr *, OID *)
  {
    return V_TRUE;
  }
}

/* ---- private members: explicit instantiation may name them (C++ [temp.explicit]) ---- */
namespace wk_access
{
  template <typename Tag, typename Tag::type M>
  struct rob
  {
    friend typename Tag::type get (Tag)
    {
      return M;
    }
  };
  struct handler_ranges
  {
    typedef parallel_index_scan::key_range_list parallel_scan::input_handler_index::*type;
    friend type get (handler_ranges);
  };
  template struct rob<handler_ranges, &parallel_scan::input_handler_index::m_ranges>;
  struct ranges_vector
  {
    typedef std::vector<key_val_range> parallel_index_scan::key_range_list::*type;
    friend type get (ranges_vector);
  };
  template struct rob<ranges_vector, &parallel_index_scan::key_range_list::m_key_val_ranges>;
}

/* ---- fakes ---- */

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  wk::err = err_id;
}

int
er_errid (void)
{
  return wk::err;
}

void
er_clear (void)
{
  wk::err = NO_ERROR;
}

bool
logtb_set_tran_index_interrupt (THREAD_ENTRY * thread_p, int tran_index, bool set)
{
  /* log_tran_table.c:2932-2992: tdes->interrupt = set (the notification and the sleepers' wake-up are not modeled) */
  (void) thread_p;
  (void) tran_index;
  wk::tran_interrupt = set;
  return true;
}

SCAN_CODE
spage_get_record (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, PGSLOTID slotid, RECDES * recdes, int ispeeking)
{
  (void) thread_p;
  (void) pgptr;
  (void) ispeeking;
  if (slotid < 1 || slotid > (int) wk::keys.size ())
    {
      return S_DOESNT_EXIST;
    }
  recdes->data = (char *) &wk::keys[slotid - 1];
  recdes->length = sizeof (wk::fkey);
  recdes->type = REC_HOME;
  return S_SUCCESS;
}

bool
btree_leaf_record_is_fence (RECDES * recp)
{
  (void) recp;
  return false;
}

int
btree_node_number_of_keys (THREAD_ENTRY * thread_p, PAGE_PTR page_ptr)
{
  (void) thread_p;
  (void) page_ptr;
  return (int) wk::keys.size ();
}

int
btree_read_record (THREAD_ENTRY * thread_p, BTID_INT * btid, PAGE_PTR pgptr, RECDES * rec, DB_VALUE * key,
		   void *rec_header, BTREE_NODE_TYPE node_type, bool * clear_key, int *offset, int copy, BTREE_SCAN * bts)
{
  const wk::fkey *k = (const wk::fkey *) rec->data;
  LEAF_REC *leaf = (LEAF_REC *) rec_header;
  int n;

  (void) thread_p;
  (void) btid;
  (void) pgptr;
  (void) node_type;
  (void) copy;
  (void) bts;
  VPID_SET_NULL (&leaf->ovfl);
  leaf->key_len = 0;
  *offset = 0;
  *clear_key = false;
  db_make_null (key);
  n = ++wk::reads_of[k->slot];
  if (k->read_err != NO_ERROR)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, k->read_err, 0);
      return k->read_err;
    }
  if (k->ovf_key)
    {
      if (n == 1 && wk::cancel_at_read_of == k->slot)
	{
	  wk::tran_interrupt = true;	/* the user cancels while the worker reads this key for the first time */
	}
      for (int fix = 0; fix < 2; fix++)
	{
	  /* overflow_get_length (), overflow_get (): a pgbuf_fix () each */
	  if (wk::interrupted ("overflow key read"))
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
	      return ER_FAILED;
	    }
	}
    }
  int token = ++wk::next_token;
  db_make_int (key, k->slot + token * wk::TOKEN_BASE);
  key->need_clear = true;
  wk::live_tokens.insert (token);
  *clear_key = true;
  return NO_ERROR;
}

int
btree_record_process_objects (THREAD_ENTRY * thread_p, BTID_INT * btid_int, BTREE_NODE_TYPE node_type,
			      RECDES * record, int after_key_offset, bool * stop, BTREE_PROCESS_OBJECT_FUNCTION * func,
			      void *args)
{
  const wk::fkey *k = (const wk::fkey *) record->data;

  (void) node_type;
  (void) after_key_offset;
  for (int j = 0; j < k->n_oids; j++)
    {
      OID oid = wk::oid_of (k->slot, j);
      OID class_oid;
      BTREE_MVCC_INFO mvcc_info;
      int r;

      memset (&class_oid, 0, sizeof (class_oid));
      memset (&mvcc_info, 0, sizeof (mvcc_info));
      r = func (thread_p, btid_int, record, nullptr, &oid, &class_oid, &mvcc_info, stop, args);
      if (r != NO_ERROR)
	{
	  return r;
	}
      if (*stop)
	{
	  break;
	}
    }
  return NO_ERROR;
}

DB_VALUE_COMPARE_RESULT
btree_compare_search_key (const BTID_INT * btid, DB_VALUE * bound_key, const DOMAIN_KEY_BOUND * bound, DB_VALUE * key,
			  int *start_colp)
{
  int b = db_get_int (bound_key) % wk::TOKEN_BASE, k = db_get_int (key) % wk::TOKEN_BASE;

  (void) btid;
  (void) bound;
  (void) start_colp;
  return b < k ? DB_LT : (b == k ? DB_EQ : DB_GT);
}

DB_VALUE_COMPARE_RESULT
btree_compare_key_to_bound (const BTID_INT * btid, DB_VALUE * key, DB_VALUE * bound_key, const DOMAIN_KEY_BOUND * bound,
			    int *start_colp)
{
  int b = db_get_int (bound_key) % wk::TOKEN_BASE, k = db_get_int (key) % wk::TOKEN_BASE;

  (void) btid;
  (void) bound;
  (void) start_colp;
  return k < b ? DB_LT : (k == b ? DB_EQ : DB_GT);
}

DB_LOGICAL
eval_key_filter (THREAD_ENTRY * thread_p, DB_VALUE * value, int prefix_size, DB_VALUE * prefix_value,
		 FILTER_INFO * filter)
{
  int slot = db_get_int (value) % wk::TOKEN_BASE;
  const wk::fkey & k = wk::keys[slot - 1];
  int n = ++wk::filters_of[slot];

  (void) thread_p;
  (void) prefix_size;
  (void) prefix_value;
  (void) filter;
  if (k.sleep)
    {
      if (n == 1 && wk::cancel_at_filter_of == slot)
	{
	  wk::tran_interrupt = true;	/* the user cancels while the key filter's SLEEP waits on this key */
	}
      if (wk::interrupted ("key filter SLEEP"))
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
	  return V_ERROR;
	}
    }
  if (k.filter_err != NO_ERROR)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, k.filter_err, 0);
      return V_ERROR;
    }
  return k.filtered ? V_FALSE : V_TRUE;
}

SCAN_CODE
heap_get_visible_version (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid, RECDES * recdes,
			  HEAP_SCANCACHE * scan_cache, int ispeeking, int old_chn)
{
  (void) thread_p;
  (void) class_oid;
  (void) recdes;
  (void) scan_cache;
  (void) ispeeking;
  (void) old_chn;
  wk::lookup_calls++;
  if (wk::cancel_at_lookup == wk::lookup_calls)
    {
      wk::tran_interrupt = true;
    }
  if (wk::interrupted ("heap fix"))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
      return S_ERROR;
    }
  wk::looked_up.push_back (*oid);
  wk::looked_up_index.push_back (wk::sid->s.isid.curr_oidno);
  return S_SUCCESS;
}

int
pr_clear_value (DB_VALUE * value)
{
  if (value == NULL)
    {
      return NO_ERROR;
    }
  if (!DB_IS_NULL (value) && value->need_clear)
    {
      int token = db_get_int (value) / wk::TOKEN_BASE;
      if (wk::live_tokens.erase (token) == 0)
	{
	  wk::double_clears++;
	}
    }
  db_make_null (value);
  value->need_clear = false;
  return NO_ERROR;
}

void
pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr)
{
  (void) thread_p;
  (void) pgptr;
  wk::unfixes++;
}

void
pgbuf_read_ahead (THREAD_ENTRY * thread_p, const VPID * vpids, int count)
{
  wk::hint h;

  (void) thread_p;
  h.lookups_done = (int) wk::looked_up.size ();
  h.vpids.assign (vpids, vpids + count);
  wk::hints.push_back (h);
}

namespace parallel_index_scan
{
  void
  leaf_page_dispatcher::release_leaf (THREAD_ENTRY * thread_p, PAGE_PTR & held_leaf)
  {
    (void) thread_p;
    wk::leaf_releases++;
    held_leaf = nullptr;
  }

  void
  leaf_page_dispatcher::signal_chain_ended (THREAD_ENTRY * thread_p, int last_local_idx, PAGE_PTR & held_leaf)
  {
    (void) thread_p;
    (void) held_leaf;
    wk::signals++;
    wk::signal_idx = last_local_idx;
    wk::rows_at_signal = wk::rows;
  }
}

/* ---- the scan ---- */

namespace wk
{
  fkey
  plain (int slot)
  {
    fkey k;
    memset (&k, 0, sizeof (k));
    k.slot = slot;
    k.n_oids = 1;
    return k;
  }

  void
  leaf (int n, bool ovf_key, bool sleep)
  {
    keys.clear ();
    for (int i = 1; i <= n; i++)
      {
	fkey k = plain (i);
	k.ovf_key = ovf_key;
	k.sleep = sleep;
	keys.push_back (k);
      }
    upper = 0;
    key_filter = sleep;
  }

  void
  reset ()
  {
    tran_interrupt = false;
    checks = 0;
    cancel_at_check = cancel_at_read_of = cancel_at_filter_of = cancel_at_lookup = 0;
    consumed_by.clear ();
    consumed = 0;
    err = NO_ERROR;
    reads_of.clear ();
    filters_of.clear ();
    lookup_calls = 0;
    looked_up.clear ();
    looked_up_index.clear ();
    hints.clear ();
    signals = 0;
    signal_idx = -1;
    rows_at_signal = -1;
    rows = 0;
    leaf_releases = 0;
    unfixes = 0;
    live_tokens.clear ();
    next_token = 0;
    double_clears = 0;
  }

  void
  setup_scan ()
  {
    if (thr == nullptr)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));	/* never constructed: read as plain fields */
	sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
	indx = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
	handler = new (handler_area) parallel_scan::input_handler_index (nullptr, nullptr);	/* never destroyed */
      }
    memset (sid, 0, sizeof (SCAN_ID));
    memset (indx, 0, sizeof (INDX_INFO));
    indx->coverage = 0;
    indx->use_desc_index = 0;
    indx->func_idx_col_id = -1;
    sid->type = S_INDX_SCAN;
    sid->s.isid.indx_info = indx;
    memset (&oid_list, 0, sizeof (oid_list));
    oid_list.oidp = oid_area;
    oid_list.capacity = 4096;
    oid_list.max_oid_cnt = 2048;
    sid->s.isid.oid_list = &oid_list;
    if (key_filter)
      {
	sid->s.isid.key_pred.pr_eval_fnc = dummy_eval;
	sid->s.isid.key_pred.pred_expr = (PRED_EXPR *) (void *) &pred_token;
      }

    key_val_range r = {};
    db_make_null (&r.key1);
    db_make_null (&r.key2);
    r.key1_bound = nullptr;
    r.key2_bound = nullptr;
    if (upper > 0)
      {
	r.range = INF_LE;
	db_make_int (&r.key2, upper);
      }
    else
      {
	r.range = INF_INF;
      }
    std::vector<key_val_range> &ranges = (handler->*get (wk_access::handler_ranges ())).*get (wk_access::ranges_vector ());
    ranges.assign (1, r);
  }

  struct outcome
  {
    SCAN_CODE end;
    int err;
    int rows;
  };

  SCAN_CODE
  next_row (parallel_index_scan::leaf_slot_walker &walker, parallel_index_scan::overflow_drain_fsm &fsm)
  {
    /* slot_iterator_index::next_qualified_slot_with_peek () */
    if (!fsm.is_idle ())
      {
	SCAN_CODE sc = fsm.drain_next_oid (thr);
	if (sc != S_END)
	  {
	    return sc;
	  }
	if (!walker.has_page ())
	  {
	    return S_END;
	  }
      }
    return walker.scan_next_slot (thr);
  }

  outcome
  run ()
  {
    outcome o = { S_END, NO_ERROR, 0 };
    parallel_index_scan::leaf_slot_walker walker;
    parallel_index_scan::overflow_drain_fsm fsm;
    SCAN_CODE sc = S_END;

    setup_scan ();
    fsm.wire_owner (&walker);
    walker.wire (sid, nullptr, handler, &fsm);

    /* the worker loop's check, then the leaf (px_scan_task.cpp:1212-1230, 1295-1324) */
    if (interrupted ("worker loop"))
      {
	er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
	sc = S_ERROR;
      }
    else
      {
	walker.set_range_idx (0);
	fsm.cleanup_on_reset (thr);
	walker.set_page (thr, (PAGE_PTR) leaf_page, NULL_SLOTID);
	while ((sc = next_row (walker, fsm)) == S_SUCCESS)
	  {
	    rows++;
	  }
	if (sc == S_END && interrupted ("worker loop"))
	  {
	    /* the next loop's check: the cancel is recorded as the worker's error */
	    er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_INTERRUPTED, 0);
	    sc = S_ERROR;
	  }
      }
    o.end = sc;
    o.err = er_errid ();
    o.rows = rows;

    /* slot_iterator_index::finalize () */
    fsm.cleanup_on_reset (thr);
    walker.cleanup_on_reset (thr);
    walker.unwire ();
    return o;
  }

  std::vector<OID>
  expected_oids ()
  {
    std::vector<OID> v;
    for (const fkey &k : keys)
      {
	if (upper > 0 && k.slot > upper)
	  {
	    break;
	  }
	if (k.filtered)
	  {
	    continue;
	  }
	for (int j = 0; j < k.n_oids; j++)
	  {
	    v.push_back (oid_of (k.slot, j));
	  }
      }
    return v;
  }

  bool
  same_oids (const std::vector<OID> &a, const std::vector<OID> &b)
  {
    if (a.size () != b.size ())
      {
	return false;
      }
    for (size_t i = 0; i < a.size (); i++)
      {
	if (a[i].pageid != b[i].pageid || a[i].volid != b[i].volid || a[i].slotid != b[i].slotid)
	  {
	    return false;
	  }
      }
    return true;
  }
}

static void
check_balanced ()
{
  /* every key value read is cleared once; the leaf is let go once at finalize */
  CHECK (wk::live_tokens.size () == 0);
  CHECK (wk::double_clears == 0);
  CHECK (wk::leaf_releases == 1);
  CHECK (wk::unfixes == 0);
}

/* ---- cases ---- */

TEST_CASE ("W1 no cancel: every qualified OID looked up once in key order, S_END, no error; the trace counters are develop's (read_keys counts filtered keys, also those the look-ahead skipped at the leaf's end)", "[contract]")
{
  wk::reset ();
  wk::leaf (20, false, false);
  wk::key_filter = true;
  for (int s : {4, 9, 15, 19, 20})
    {
      wk::keys[s - 1].filtered = true;
    }
  wk::keys[7 - 1].n_oids = 3;
  wk::keys[12 - 1].n_oids = 2;
  wk::outcome o = wk::run ();

  CHECK (o.end == S_END);
  CHECK (o.err == NO_ERROR);
  CHECK (o.rows == 18);
  CHECK (wk::same_oids (wk::looked_up, wk::expected_oids ()));
  CHECK (wk::sid->scan_stats.read_keys == 20);
  CHECK (wk::sid->scan_stats.qualified_keys == 15);
  CHECK (wk::sid->scan_stats.key_qualified_rows == 18);
  CHECK (wk::sid->scan_stats.read_rows == 18);
  CHECK (wk::sid->scan_stats.data_qualified_rows == 18);
  CHECK (wk::signals == 0);
  for (auto &r : wk::reads_of)
    {
      CHECK (r.second == 1);	/* a key the look-ahead queued is not read again */
    }
  check_balanced ();
}

TEST_CASE ("W2 a cancel seen by the lookups' own read of the first key (an overflow key) ends the scan: S_ERROR, ER_INTERRUPTED, no row, the range's end signaled", "[contract]")
{
  wk::reset ();
  wk::leaf (10, true, false);
  wk::cancel_at_read_of = 1;
  wk::outcome o = wk::run ();

  CHECK (wk::consumed == 1);
  CHECK (o.end == S_ERROR);
  CHECK (o.err == ER_INTERRUPTED);
  CHECK (o.rows == 0);
  CHECK (wk::signals == 1);
  check_balanced ();
}

TEST_CASE ("W3 a cancel seen by the heap fix of the third lookup ends the scan: S_ERROR, ER_INTERRUPTED after two rows", "[contract]")
{
  wk::reset ();
  wk::leaf (10, false, false);
  wk::cancel_at_lookup = 3;
  wk::outcome o = wk::run ();

  CHECK (wk::consumed == 1);
  CHECK (wk::consumed_by == "heap fix");
  CHECK (o.end == S_ERROR);
  CHECK (o.err == ER_INTERRUPTED);
  CHECK (o.rows == 2);
  check_balanced ();
}

TEST_CASE ("W4 a key filter that fails on key 5 every time: the error is raised when the lookups reach key 5 -- rows of keys 1-4, S_ERROR with that error, the range's end signaled once", "[contract]")
{
  wk::reset ();
  wk::leaf (10, false, false);
  wk::key_filter = true;
  wk::keys[5 - 1].filter_err = ER_QPROC_ZERO_DIVIDE;
  wk::outcome o = wk::run ();

  CHECK (o.end == S_ERROR);
  CHECK (o.err == ER_QPROC_ZERO_DIVIDE);
  CHECK (o.rows == 4);
  CHECK (wk::signals == 1);
  CHECK (wk::rows_at_signal == 4);
  check_balanced ();
}

TEST_CASE ("W5 a key read that fails on key 5 every time (an overflow key page): rows of keys 1-4, S_ERROR with that error, the range's end signaled once", "[contract]")
{
  wk::reset ();
  wk::leaf (10, true, false);
  wk::keys[5 - 1].read_err = ER_PB_BAD_PAGEID;
  wk::outcome o = wk::run ();

  CHECK (o.end == S_ERROR);
  CHECK (o.err == ER_PB_BAD_PAGEID);
  CHECK (o.rows == 4);
  CHECK (wk::signals == 1);
  CHECK (wk::rows_at_signal == 4);
  check_balanced ();
}

TEST_CASE ("W6 the range ends inside the leaf (upper bound 6, key 3 filtered): rows of keys 1,2,4,5,6, S_END, the end signaled once after the last row with range index 1; read_keys 7 as develop counts it", "[contract]")
{
  wk::reset ();
  wk::leaf (10, false, false);
  wk::key_filter = true;
  wk::keys[3 - 1].filtered = true;
  wk::upper = 6;
  wk::outcome o = wk::run ();

  CHECK (o.end == S_END);
  CHECK (o.err == NO_ERROR);
  CHECK (o.rows == 5);
  CHECK (wk::same_oids (wk::looked_up, wk::expected_oids ()));
  CHECK (wk::signals == 1);
  CHECK (wk::rows_at_signal == 5);
  CHECK (wk::signal_idx == 1);
  CHECK (wk::sid->scan_stats.read_keys == 7);
  CHECK (wk::sid->scan_stats.qualified_keys == 5);
  check_balanced ();
}

TEST_CASE ("W7 read-ahead (35a151f37; passes trivially where scan_index_read_ahead is absent): each looked-up OID's heap page is told before or at its lookup unless it opens a fresh buffer, none twice, none never looked up", "[contract]")
{
  if (scan_index_read_ahead == nullptr)
    {
      SUCCEED ("no read-ahead in this revision");
      return;
    }
  wk::reset ();
  wk::leaf (20, false, false);
  wk::key_filter = true;
  for (int s : {4, 9, 15, 19, 20})
    {
      wk::keys[s - 1].filtered = true;
    }
  wk::keys[7 - 1].n_oids = 3;
  wk::keys[12 - 1].n_oids = 2;
  wk::outcome o = wk::run ();
  REQUIRE (o.end == S_END);

  std::map<int, int> told_at;	/* page -> lookups done when told */
  std::map<int, int> told_count;
  for (const wk::hint &h : wk::hints)
    {
      for (const VPID &v : h.vpids)
	{
	  told_count[v.pageid]++;
	  if (told_at.find (v.pageid) == told_at.end ())
	    {
	      told_at[v.pageid] = h.lookups_done;
	    }
	}
    }
  std::set<int> looked;
  int told = 0;
  for (size_t i = 0; i < wk::looked_up.size (); i++)
    {
      int page = wk::looked_up[i].pageid;
      looked.insert (page);
      if (told_at.count (page))
	{
	  told++;
	  CHECK (told_at[page] <= (int) i);	/* told before (or at) its own lookup */
	}
      else
	{
	  CHECK (wk::looked_up_index[i] == 0);	/* only the first OID of a fresh buffer may go untold */
	}
    }
  for (auto &t : told_count)
    {
      CHECK (t.second == 1);
      CHECK (looked.count (t.first) == 1);
    }
  fprintf (stderr, "W7: %d lookups, %d told before or at their lookup, %d read-ahead calls\n", (int) wk::looked_up.size (),
	   told, (int) wk::hints.size ());
  CHECK (told > 0);
  check_balanced ();
}

TEST_CASE ("PR8490 G1-ahead-interrupt-lost: a cancel first seen by the read of overflow key 3 of 12, which the look-ahead reads, is lost -- every row comes back, S_END, no error (PASS = defect present)", "[defect]")
{
  wk::reset ();
  wk::leaf (12, true, false);
  wk::cancel_at_read_of = 3;
  wk::outcome o = wk::run ();

  CHECK (wk::consumed == 1);
  CHECK (wk::consumed_by == "overflow key read");
  CHECK (o.end == S_END);
  CHECK (o.err == NO_ERROR);
  CHECK (o.rows == 12);
  CHECK (wk::reads_of[3] == 2);	/* the look-ahead's read met the cancel; the lookups' read did not */
  check_balanced ();
}

TEST_CASE ("PR8490 G23-01: a cancel first seen by the key filter's SLEEP on key 3 of 12, which the look-ahead evaluates, is lost -- every row comes back, S_END, no error (PASS = defect present)", "[defect]")
{
  wk::reset ();
  wk::leaf (12, false, true);
  wk::cancel_at_filter_of = 3;
  wk::outcome o = wk::run ();

  CHECK (wk::consumed == 1);
  CHECK (wk::consumed_by == "key filter SLEEP");
  CHECK (o.end == S_END);
  CHECK (o.err == NO_ERROR);
  CHECK (o.rows == 12);
  CHECK (wk::filters_of[3] == 2);
  check_balanced ();
}

TEST_CASE ("PR8490 G1/G23-01 sweep: a cancel landing at each interrupt check of a 16-key leaf (overflow keys, SLEEP key filter, heap fixes, the worker loop) -- some are lost (PASS = defect present; the counts go to stderr)", "[defect]")
{
  wk::reset ();
  wk::leaf (16, true, true);
  wk::outcome clean = wk::run ();
  REQUIRE (clean.end == S_END);
  REQUIRE (clean.rows == 16);
  const int total = wk::checks;

  int lost = 0, honored = 0, other = 0;
  std::map<std::string, int> lost_by;
  for (int n = 1; n <= total; n++)
    {
      wk::reset ();
      wk::leaf (16, true, true);
      wk::cancel_at_check = n;
      wk::outcome o = wk::run ();
      if (o.end == S_ERROR && o.err == ER_INTERRUPTED)
	{
	  honored++;
	}
      else if (o.end == S_END && o.err == NO_ERROR && wk::consumed == 1)
	{
	  lost++;
	  lost_by[wk::consumed_by]++;
	}
      else
	{
	  other++;
	}
      CHECK (wk::live_tokens.size () == 0);
      CHECK (wk::double_clears == 0);
    }
  fprintf (stderr, "sweep: %d checks, honored %d, lost %d (", total, honored, lost);
  for (auto &l : lost_by)
    {
      fprintf (stderr, "%s %d; ", l.first.c_str (), l.second);
    }
  fprintf (stderr, "), other %d\n", other);
  CHECK (other == 0);
  CHECK (lost > 0);
}
