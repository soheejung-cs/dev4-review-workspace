/* PR#8718 (CBRD-28283) a4a513237 -- scan_get_index_oidset (), src/query/scan_manager.c: the order in which a pass of
 * an index scan reads the key ranges of an IN list (R_KEYLIST) or an OR of ranges (R_RANGELIST).
 *
 * An index MIN/MAX scan (ACCESS_SPEC_FLAG_ONLY_MIN_MAX_SCAN) takes MIN or MAX from the first row of a pass.  The pass
 * that reads the MIN/MAX column descending takes MAX, so it has to read the key ranges from the last one.  PR#8718
 * marks that pass with INDX_SCAN_ID::min_max_only_desc_pass (qexec_open_scan () sets it, qexec_intprt_fnc () flips it
 * with use_desc_index) and scan_get_index_oidset () reverses the key list at the first open of the pass, as it does for
 * a skipped descending ORDER BY / GROUP BY.  base reads 1, 5, 9 in both passes: MAX of `cb in (1, 5, 9)` came out 1.
 *
 * The real code under test: scan_get_index_oidset () (static: the seam only unstatics it), check_key_vals (),
 * eliminate_duplicated_keys (), merge_key_ranges (), reverse_key_list (), scan_key_compare (), compare_val_op ().
 *
 * Fakes:
 *  - scan_regu_key_to_index_key () (seam): the search key of KEY_RANGE i from kr::query[i] -- an IN list item is an
 *    equality (EQ_NA: key2 is key1's clone, range turned GE_LE), a BETWEEN a GE_LE range with key1 > key2 empty
 *    (NA_NA); num_index_term 1, the bounds copied from the KEY_RANGE -- as the real one leaves them.  A multi-column
 *    index's key is a DB_TYPE_MIDXKEY that owns a malloc'd buffer (need_clear), a single-column one an INTEGER.
 *  - btree_prepare_bts () / btree_range_scan (): the B-tree holds one row per integer value (minus kr::no_rows); a call
 *    reads every row of the key range in the scan's direction (indx_info->use_desc_index) into the OID buffer (OID
 *    pageid = the value) and ends the range (BTREE_END_OF_SCAN true).
 *  - domain_key_ranges_element_compare () / pr_midxkey_compare_resolved (): the integer comparison of the two bounds'
 *    first column (the comparisons XASL generation decides for INTEGER columns).
 *  - pr_clear_value (): the real one's NULL / INTEGER / MIDXKEY branches (object_primitive.c pr_clear_value ()):
 *    frees the midxkey buffer when need_clear, sets the value NULL and need_clear false.  kr::live counts the buffers.
 * SCAN_ID / INDX_INFO / thread entry are calloc'd, never constructed: S_INDX_SCAN, curr_keyno -1 (scan_start_scan ()),
 * key_vals NULL-made as the XASL unpack leaves them, no key limit, no covering, no multi range opt, no OID order.
 *
 * min_max_only_desc_pass is new in PR#8718: kr::set_desc_pass () sets it where the member exists (SFINAE), so the
 * suite compiles on base too, where the MAX pass cases fail (that is the CBRD-28283 defect).
 */
#define CATCH_CONFIG_ENABLE_PAIR_STRINGMAKER
#include "catch.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include "scan_manager.h"
#include "btree.h"
#include "object_primitive.h"
#include "domain_rules.h"
#include "thread_entry.hpp"

/* static in scan_manager.c: unstatic'd by the seam (the real code runs) */
extern int scan_get_index_oidset (THREAD_ENTRY * thread_p, SCAN_ID * s_id, DB_BIGINT * key_limit_upper,
				  DB_BIGINT * key_limit_lower);

namespace kr
{
  struct bound
  {
    int lo, hi;
    RANGE range;		/* EQ_NA: an IN list item (lo), GE_LE: BETWEEN lo AND hi */
  };

  struct probe
  {
    int lo, hi;
    bool desc;
  };

  std::vector<bound> query;	/* KEY_RANGE i of the scan */
  bool multi_column;		/* midxkey keys with a buffer (multi-column index) or INTEGER keys */
  std::set<int> no_rows;	/* values the table has no row with */
  std::vector<probe> probes;	/* the key ranges the B-tree was asked for, in order */
  std::set<void *> live;	/* midxkey buffers alive */
  int bad_frees;		/* a buffer freed that is not alive */
  int near_null_clears;		/* pr_clear_value () of a slot of a NULL key_vals */
  int keys_made;

  cubthread::entry *thr;
  SCAN_ID *sid;
  INDX_INFO *indx;
  KEY_RANGE *ranges;
  KEY_VAL_RANGE *vals;
  int n_vals;
  DOMAIN_KEY_BOUND kbound;
  DOMAIN_KEY_RANGES kranges;
  BTREE_ISCAN_OID_LIST oid_list;
  OID oid_area[256];

  /* the probe the next btree_range_scan () reads */
  probe cur;
  INDX_SCAN_ID *cur_isid;

  /* INDX_SCAN_ID::min_max_only_desc_pass where it exists (PR#8718); false: the revision has no such member */
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

  int key_int (const DB_VALUE * v)
  {
    if (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_MIDXKEY)
      {
	int x;
	memcpy (&x, v->data.midxkey.buf, sizeof (x));
	return x;
      }
    return db_get_int (v);
  }

  void make_key (DB_VALUE * v, int x)
  {
    if (multi_column)
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
    else
      {
	db_make_int (v, x);
      }
  }

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

  /* an index scan of `range_type` over the key ranges `q`, opened and started: curr_keyno -1 */
  void setup (RANGE_TYPE range_type, std::vector<bound> q, bool multi)
  {
    if (thr == nullptr)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));
	sid = (SCAN_ID *) calloc (1, sizeof (SCAN_ID));
	indx = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
      }
    release ();
    query = q;
    multi_column = multi;
    no_rows.clear ();
    probes.clear ();
    live.clear ();
    bad_frees = 0;
    near_null_clears = 0;
    keys_made = 0;
    n_vals = (int) q.size ();

    memset (&kbound, 0, sizeof (kbound));
    memset (&kranges, 0, sizeof (kranges));
    ranges = (KEY_RANGE *) calloc (n_vals, sizeof (KEY_RANGE));
    vals = new KEY_VAL_RANGE[n_vals];
    for (int i = 0; i < n_vals; i++)
      {
	ranges[i].range = q[i].range;
	ranges[i].key1_bound = &kbound;
	ranges[i].key2_bound = &kbound;
	vals[i].range = NA_NA;
	db_make_null (&vals[i].key1);
	db_make_null (&vals[i].key2);
	vals[i].is_truncated = false;
	vals[i].num_index_term = 0;
      }

    memset (indx, 0, sizeof (INDX_INFO));
    indx->range_type = range_type;
    indx->key_info.key_cnt = n_vals;
    indx->key_info.key_ranges = ranges;
    indx->key_info.key_vals = vals;
    indx->key_ranges_compares = &kranges;

    memset (sid, 0, sizeof (SCAN_ID));
    sid->type = S_INDX_SCAN;
    sid->status = S_STARTED;
    sid->position = S_BEFORE;
    sid->direction = S_FORWARD;
    sid->s.isid.indx_info = indx;
    sid->s.isid.key_vals = vals;
    sid->s.isid.key_cnt = n_vals;
    sid->s.isid.curr_keyno = -1;
    sid->s.isid.curr_oidno = -1;
    sid->s.isid.key_limit_lower = -1;
    sid->s.isid.key_limit_upper = -1;
    sid->s.isid.cls_oid.volid = 0;
    sid->s.isid.cls_oid.pageid = 1;
    sid->s.isid.cls_oid.slotid = 1;
    memset (&oid_list, 0, sizeof (oid_list));
    oid_list.oidp = oid_area;
    oid_list.capacity = 256;
    oid_list.max_oid_cnt = 256;
    sid->s.isid.oid_list = &oid_list;
    sid->s.isid.curr_oidp = oid_area;
  }

  /* one scan_get_index_oidset () call: the rows it read (values), -1 on error */
  std::vector<int> call ()
  {
    std::vector<int> rows;
    if (scan_get_index_oidset (thr, sid, NULL, NULL) != NO_ERROR)
      {
	rows.push_back (-1);
	return rows;
      }
    for (int i = 0; i < sid->s.isid.oids_count; i++)
      {
	rows.push_back (sid->s.isid.oid_list->oidp[i].pageid);
      }
    return rows;
  }

  /* the rest of the pass: calls until the scan is past its last key (scan_get_index_oidset () moves curr_keyno past
   * key_cnt when it reads the last key's end), every row in the order read */
  std::vector<int> drain (std::vector<int> rows = {})
  {
    for (int guard = 0; guard < 64; guard++)
      {
	if (sid->s.isid.curr_keyno > sid->s.isid.key_cnt)
	  {
	    break;
	  }
	std::vector<int> r = call ();
	rows.insert (rows.end (), r.begin (), r.end ());
      }
    return rows;
  }

  /* a new pass of the same scan, as S_GO_BACKWARD (scan_next_index_scan ()) or a rewind (scan_reset_scan_block ())
   * starts it: the first open makes the keys afresh */
  void new_pass ()
  {
    sid->s.isid.curr_keyno = -1;
    sid->s.isid.curr_oidno = -1;
    sid->s.isid.oids_count = 0;
  }

  /* qexec_intprt_fnc () turning a MIN/MAX scan around: use_desc_index and min_max_only_desc_pass flipped together */
  void turn_around (bool desc_pass_now)
  {
    indx->use_desc_index = !indx->use_desc_index;
    set_desc_pass (&sid->s.isid, desc_pass_now);
  }

  std::vector<std::pair<int, int>> probe_bounds ()
  {
    std::vector<std::pair<int, int>> v;
    for (const probe &p : probes)
      {
	v.push_back (std::make_pair (p.lo, p.hi));
      }
    return v;
  }

  bool all_desc (bool d)
  {
    for (const probe &p : probes)
      {
	if (p.desc != d)
	  {
	    return false;
	  }
      }
    return true;
  }
}

/* ---- fakes ---- */

int
scan_regu_key_to_index_key (THREAD_ENTRY * thread_p, KEY_RANGE * key_ranges, KEY_VAL_RANGE * key_val_range,
			    INDX_SCAN_ID * iscan_id, TP_DOMAIN * btree_domainp, VAL_DESCR * vd, int key_range_idx)
{
  const kr::bound & b = kr::query[key_range_idx];

  (void) thread_p;
  (void) iscan_id;
  (void) btree_domainp;
  (void) vd;
  key_val_range->key1_bound = key_ranges->key1_bound;
  key_val_range->key2_bound = key_ranges->key2_bound;
  key_val_range->num_index_term = 1;
  kr::make_key (&key_val_range->key1, b.lo);
  /* an equality's key2 is key1's clone (pr_clone_value ()), a range's its upper bound */
  kr::make_key (&key_val_range->key2, b.range == EQ_NA ? b.lo : b.hi);
  kr::keys_made++;
  if (key_val_range->range == EQ_NA)
    {
      key_val_range->range = GE_LE;
    }
  else if (key_val_range->range == GE_LE && b.lo > b.hi)
    {
      key_val_range->range = NA_NA;	/* key1 > key2: an empty range, as the real one marks it */
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
  kr::cur.lo = kr::key_int (&kv_range->key1);
  kr::cur.hi = kr::key_int (&kv_range->key2);
  kr::cur.desc = index_scan_id_p->indx_info->use_desc_index != 0;
  kr::cur_isid = index_scan_id_p;
  kr::probes.push_back (kr::cur);
  bts->use_desc_index = kr::cur.desc;
  return NO_ERROR;
}

int
btree_range_scan (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTREE_RANGE_SCAN_PROCESS_KEY_FUNC * key_func)
{
  int n = 0;

  (void) thread_p;
  (void) key_func;
  for (int k = 0; k <= kr::cur.hi - kr::cur.lo; k++)
    {
      int v = kr::cur.desc ? kr::cur.hi - k : kr::cur.lo + k;
      if (kr::no_rows.count (v) != 0)
	{
	  continue;
	}
      kr::cur_isid->oid_list->oidp[n].volid = 0;
      kr::cur_isid->oid_list->oidp[n].pageid = v;
      kr::cur_isid->oid_list->oidp[n].slotid = 1;
      n++;
    }
  kr::cur_isid->oid_list->oid_cnt = n;
  bts->n_oids_read_last_iteration = n;
  /* the range's end: BTREE_END_OF_SCAN () */
  bts->C_vpid.pageid = NULL_PAGEID;
  bts->O_vpid.pageid = NULL_PAGEID;
  bts->is_scan_started = false;
  return NO_ERROR;
}

DB_VALUE_COMPARE_RESULT
domain_key_ranges_element_compare (const void *context, int column, DB_VALUE * value1, DB_VALUE * value2,
				   int do_coercion, int total_order, bool * can_compare)
{
  int a = db_get_int (value1), b = db_get_int (value2);

  (void) context;
  (void) column;
  (void) do_coercion;
  (void) total_order;
  if (can_compare != NULL)
    {
      *can_compare = true;
    }
  return a < b ? DB_LT : (a > b ? DB_GT : DB_EQ);
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
      kr::near_null_clears++;	/* &key_vals[i].key1 of a NULL key_vals */
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
	      if (kr::live.erase (buf) == 1)
		{
		  free (buf);
		}
	      else
		{
		  kr::bad_frees++;
		}
	    }
	  value->data.midxkey.buf = NULL;
	}
    }
  PRIM_SET_NULL (value);
  value->need_clear = false;
  return NO_ERROR;
}

/* ---- cases ---- */

typedef std::vector<std::pair<int, int>> bounds;

TEST_CASE ("R1 an ascending pass (MIN, min_max_only_desc_pass off) of an IN list (9, 1, 5) reads the key ranges from "
	   "the smallest: its first call reads 1 only, the pass 1, 5, 9; the last key's end releases the keys",
	   "[contract]")
{
  kr::setup (R_KEYLIST, { {9, 9, EQ_NA}, {1, 1, EQ_NA}, {5, 5, EQ_NA} }, false);
  kr::set_desc_pass (&kr::sid->s.isid, false);

  std::vector<int> first = kr::call ();
  REQUIRE (first == std::vector<int> ({ 1 }));	/* MIN */
  std::vector<int> rows = kr::drain (first);
  CHECK (rows == std::vector<int> ({ 1, 5, 9 }));
  CHECK (kr::probe_bounds () == bounds ({ {1, 1}, {5, 5}, {9, 9} }));
  CHECK (kr::all_desc (false));
  CHECK (kr::keys_made == 3);
  CHECK (kr::sid->s.isid.curr_keyno == 4);	/* past key_cnt: the end of the scan */
  for (int i = 0; i < 3; i++)
    {
      CHECK (DB_IS_NULL (&kr::vals[i].key1));
      CHECK (DB_IS_NULL (&kr::vals[i].key2));
    }
}

TEST_CASE ("R2 a MAX pass (min_max_only_desc_pass on, use_desc_index on) of the IN list (9, 1, 5) reads the key "
	   "ranges from the largest: its first call reads 9 only -- the MAX -- and the next calls go on down the "
	   "reversed list, 5 then 1 (base: 1 first, MAX 1 -- CBRD-28283)", "[contract]")
{
  kr::setup (R_KEYLIST, { {9, 9, EQ_NA}, {1, 1, EQ_NA}, {5, 5, EQ_NA} }, false);
  kr::indx->use_desc_index = 1;
  kr::set_desc_pass (&kr::sid->s.isid, true);

  std::vector<int> first = kr::call ();
  CHECK (first == std::vector<int> ({ 9 }));	/* MAX */
  std::vector<int> rows = kr::drain (first);
  CHECK (rows == std::vector<int> ({ 9, 5, 1 }));
  CHECK (kr::probe_bounds () == bounds ({ {9, 9}, {5, 5}, {1, 1} }));
  CHECK (kr::all_desc (true));
  CHECK (kr::keys_made == 3);	/* the reversal is decided once, at the first open */
}

TEST_CASE ("R3 a MAX pass over an OR of ranges of a multi-column index (cb between 7 and 9 or cb between 1 and 3, "
	   "R_RANGELIST, midxkey keys with buffers) reads 7..9 first, descending: first row 9, then 1..3 as 3, 2, 1; "
	   "no key buffer is left after the last key's end (base: 3 first)", "[contract]")
{
  kr::setup (R_RANGELIST, { {1, 3, GE_LE}, {7, 9, GE_LE} }, true);
  kr::indx->use_desc_index = 1;
  kr::set_desc_pass (&kr::sid->s.isid, true);

  std::vector<int> first = kr::call ();
  CHECK (first == std::vector<int> ({ 9, 8, 7 }));
  CHECK (kr::live.size () == 4);	/* key1 and key2 of the two ranges */
  std::vector<int> rows = kr::drain (first);
  CHECK (rows == std::vector<int> ({ 9, 8, 7, 3, 2, 1 }));
  CHECK (kr::probe_bounds () == bounds ({ {7, 9}, {1, 3} }));
  CHECK (kr::live.empty ());
  CHECK (kr::bad_frees == 0);
}

TEST_CASE ("R4 boundary: a MAX pass over ranges that merge (between 1 and 3, between 8 and 9, between 2 and 5 -> "
	   "[1,5], [8,9]) reverses the merged list, not the original count: [8,9] then [1,5]; the dropped third entry "
	   "stays NULL and owns no buffer (base: [1,5] first)", "[contract]")
{
  kr::setup (R_RANGELIST, { {1, 3, GE_LE}, {8, 9, GE_LE}, {2, 5, GE_LE} }, true);
  kr::indx->use_desc_index = 1;
  kr::set_desc_pass (&kr::sid->s.isid, true);

  std::vector<int> first = kr::call ();
  CHECK (first == std::vector<int> ({ 9, 8 }));
  CHECK (kr::sid->s.isid.key_cnt == 2);
  CHECK (DB_IS_NULL (&kr::vals[2].key1));
  CHECK (DB_IS_NULL (&kr::vals[2].key2));
  CHECK_FALSE (kr::vals[2].key1.need_clear);
  CHECK (kr::live.size () == 4);	/* the merge released the inner bounds 3 and 2 */
  std::vector<int> rows = kr::drain (first);
  CHECK (kr::probe_bounds () == bounds ({ {8, 9}, {1, 5} }));
  CHECK (rows == std::vector<int> ({ 9, 8, 5, 4, 3, 2, 1 }));
  CHECK (kr::live.empty ());
  CHECK (kr::bad_frees == 0);
}

TEST_CASE ("R5 boundary: a MAX pass over an IN list with a duplicate (5, 1, 5, 9) reads 9, 5, 1 -- the duplicate "
	   "eliminated before the reversal, its keys released (multi-column keys)", "[contract]")
{
  kr::setup (R_KEYLIST, { {5, 5, EQ_NA}, {1, 1, EQ_NA}, {5, 5, EQ_NA}, {9, 9, EQ_NA} }, true);
  kr::indx->use_desc_index = 1;
  kr::set_desc_pass (&kr::sid->s.isid, true);

  std::vector<int> first = kr::call ();
  CHECK (first == std::vector<int> ({ 9 }));
  CHECK (kr::sid->s.isid.key_cnt == 3);
  CHECK (kr::live.size () == 6);
  std::vector<int> rows = kr::drain (first);
  CHECK (rows == std::vector<int> ({ 9, 5, 1 }));
  CHECK (kr::live.empty ());
  CHECK (kr::bad_frees == 0);
}

TEST_CASE ("R6 boundary: a MAX pass whose last key range has no row (IN (1, 5, 9), no row with 9) skips it in the "
	   "same call and takes 5, the largest value with a row (base: 1)", "[contract]")
{
  kr::setup (R_KEYLIST, { {1, 1, EQ_NA}, {5, 5, EQ_NA}, {9, 9, EQ_NA} }, false);
  kr::no_rows.insert (9);
  kr::indx->use_desc_index = 1;
  kr::set_desc_pass (&kr::sid->s.isid, true);

  std::vector<int> first = kr::call ();
  CHECK (first == std::vector<int> ({ 5 }));
  CHECK (kr::probe_bounds () == bounds ({ {9, 9}, {5, 5} }));
}

TEST_CASE ("R7 the reversals that predate PR#8718 stay: a skipped descending ORDER BY or GROUP BY reverses the list "
	   "(9, 5, 1) with min_max_only_desc_pass off; a descending ORDER BY that is not skipped, or a descending "
	   "index scan that is no MIN/MAX pass, does not (1, 5, 9)", "[contract]")
{
  std::vector<kr::bound> q = { {9, 9, EQ_NA}, {1, 1, EQ_NA}, {5, 5, EQ_NA} };

  SECTION ("order by desc, skipped")
  {
    kr::setup (R_KEYLIST, q, false);
    kr::indx->use_desc_index = 1;
    kr::indx->orderby_desc = 1;
    kr::indx->orderby_skip = 1;
    CHECK (kr::drain () == std::vector<int> ({ 9, 5, 1 }));
  }
  SECTION ("group by desc, skipped")
  {
    kr::setup (R_KEYLIST, q, false);
    kr::indx->use_desc_index = 1;
    kr::indx->groupby_desc = 1;
    kr::indx->groupby_skip = 1;
    CHECK (kr::drain () == std::vector<int> ({ 9, 5, 1 }));
  }
  SECTION ("order by desc, not skipped")
  {
    kr::setup (R_KEYLIST, q, false);
    kr::indx->use_desc_index = 1;
    kr::indx->orderby_desc = 1;
    CHECK (kr::drain () == std::vector<int> ({ 1, 5, 9 }));
  }
  SECTION ("descending index scan (USE_DESC_IDX), no MIN/MAX")
  {
    kr::setup (R_KEYLIST, q, false);
    kr::indx->use_desc_index = 1;
    kr::set_desc_pass (&kr::sid->s.isid, false);
    CHECK (kr::drain () == std::vector<int> ({ 1, 5, 9 }));
  }
}

TEST_CASE ("R8 a MIN/MAX scan as qexec_intprt_fnc () drives it: pass 1 ascending takes 1 from its first row; the turn "
	   "(use_desc_index and min_max_only_desc_pass flipped, curr_keyno -1 as S_GO_BACKWARD sets it) makes pass 2 take "
	   "9; restored for the next execution, a new scan's first pass takes 1 again -- each first open decides the "
	   "order afresh from the keys it makes (base: pass 2 takes 1)", "[contract]")
{
  kr::setup (R_KEYLIST, { {9, 9, EQ_NA}, {1, 1, EQ_NA}, {5, 5, EQ_NA} }, false);
  kr::set_desc_pass (&kr::sid->s.isid, false);

  std::vector<int> p1 = kr::call ();
  CHECK (p1 == std::vector<int> ({ 1 }));	/* MIN */

  kr::turn_around (true);
  kr::new_pass ();
  std::vector<int> p2 = kr::call ();
  CHECK (p2 == std::vector<int> ({ 9 }));	/* MAX */

  kr::turn_around (false);	/* restore index scan direction */
  kr::new_pass ();
  std::vector<int> p3 = kr::call ();
  CHECK (p3 == std::vector<int> ({ 1 }));
  CHECK (kr::keys_made == 9);	/* three first opens, three keys each */
}
