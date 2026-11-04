/*
 * Contract suite -- CBRD-27845 (PR#8394): locator_check_foreign_key () (src/transaction/locator_sr.c, SERVER_MODE,
 * static; reached through hooks.cpp), the foreign key check every INSERT / UPDATE of a row runs.
 *
 * TO-BE of PR#8394 (body "AS-IS / TO-BE"): whatever the number of foreign keys, every class representation the check
 * fixes (heap_classrepr_get of the referenced class) is freed, and the pruning context of a partitioned referenced
 * class is cleared for each key -- nothing is left when the function returns, on the found path and on every exit
 * (ER_FK_INVALID, the log applier's go-on, a B-tree error, the error paths between the fix and the release).
 * Unchanged by the PR (body "Implementation"): error codes, the ER_FK_INVALID message, the lock (S_SELECT_WITH_LOCK),
 * the order of the checks, the index searched (the pruned partition's PK).
 * Second commit 196989e1a (self-review G1-01): the key value of a multi-column foreign key owns a buffer when the row
 * record is longer than DBVAL_BUFSIZE (heap_attrvalue_get_key allocates it); the error label clears the key value, so
 * the error exits between reading a key and finishing it (pruning, loading the pruning context, fixing the class
 * representation, the parent lookup's BTREE_ERROR_OCCURRED) free that buffer too, and the exits that cleared it
 * already (the end of a key, ER_FK_INVALID) do not free it twice (K27-K34, R3).
 *
 * Real code: locator_check_foreign_key only. Fakes (other TUs), each playing the branch of the real function named:
 *   heap_attrinfo_start_with_index (heap_file.c 13808-13990): the child's representation fixed in the attribute cache
 *       (last_classrepr / last_cacheindex), num_btids = n_indexes, has_single_col / has_multi_col; returns the number
 *       of single-column indexed attributes (1 when only multi-column); 0 with nothing fixed when there is no index
 *       attribute; ER_FAILED with nothing fixed when the child's representation cannot be had.
 *   heap_attrinfo_end (10997-11030): num_values -1 -> nothing; else the child's representation unfixed, num_values -1.
 *   heap_attrinfo_read_dbvalues: NO_ERROR, or the scripted error (er_set).
 *   heap_attrvalue_get_key (14682-14830): notes an entry with a db_value that is not NULL (the real one asserts
 *       DB_IS_NULL (db_value), 14692); NULL for the scripted index; *btid = index->btid; a multi-column index builds a
 *       MIDXKEY in the caller's db_value and returns it -- over buf when recdes->length <= DBVAL_BUFSIZE, else over a
 *       buffer it allocates (counted) with need_clear = true after db_make_midxkey (14740-14784: the size is the
 *       record's length, not the key's); a single-column one returns the attribute cache's value
 *       (heap_attrinfo_access) -- so only multi-column keys are `== &dbvalue` in the caller.
 *   heap_classrepr_get (2072-): *idx_incache = -1 first; the scripted failure returns NULL; else the class's cache
 *       entry is fixed (fcnt++) and its slot returned.
 *   heap_classrepr_free (1666-1720): *idx_incache < 0 -> a representation outside the cache; else fcnt-- on that slot
 *       (*idx_incache is not reset, as the real one). A slot that does not hold the pointer, or fcnt already 0, is
 *       counted as a free error (a double free, or a free of what was never fixed).
 *   partition_init_pruning_context (partition.c 2944-2970): the real body (fields reset, nothing released).
 *   partition_load_pruning_context (3029-3150): init first (3043), root_oid = class_oid; partitions from the cache
 *       (is_from_cache: the array stays the cache's) or read from the heap (owned by the context); the partition
 *       predicate unpacked (partition_pred / fp_cache_context: the xasl_stream.cpp allocations the debug server's
 *       resource tracker reports, PR body); partitions NULL -> "not partitioned, just return" NO_ERROR; a scripted
 *       failure after the partitions were read goes to error_return: the context cleared, error kept, returned.
 *   partition_clear_pruning_context (3160-3205): owned partitions freed (heap_clear_partition_info) unless
 *       is_from_cache, partitions / count reset; the predicate freed and NULLed (partition_free_partition_predicate;
 *       free_xasl_unpack_info takes the pointer by reference); the attribute info ended when is_attr_info_inited
 *       (heap_attrinfo_end: the root's representation unfixed). Idempotent, as the real one.
 *   partition_prune_unique_btid (4421-4466) with partition_attrinfo_get_key (4702-4795): the context's attribute info
 *       started once (heap_attrinfo_start: the root's representation fixed, is_attr_info_inited) -- a scripted failure
 *       of that start leaves it not started; a scripted failure after it leaves it started (the real cleanup does not
 *       clear a caller's context); the partition that holds the key (hash: v mod n, range: first upper bound > v)
 *       -> class_oid / class_hfid / btid of pcontext->partitions[pos + 1] and that partition's PK; none -> all three
 *       NULL, NO_ERROR (4438-4445).
 *   xbtree_find_unique: records the BTID, class OID, scan op, is_all_class_srch, the key pointer, and how many of this
 *       check's own fixes and pruning-context resources are live at that moment; FOUND when the (PK root page, key)
 *       row was scripted, else NOTFOUND; BTREE_ERROR_OCCURRED with er_set (ER_LK_UNILATERALLY_ABORTED) at the
 *       scripted search (a deadlock victim while locking the parent row).
 *   btree_multicol_key_has_null: the scripted NULL element of the key just read.
 *   pr_valstring: a malloc'ed print of the key (counted); db_private_free (_release | _debug): frees it (counted).
 *   pr_clear_value (object_primitive.c 1867-2023): NULL -> nothing; else MIDXKEY: a buf with need_clear is freed
 *       (db_private_free_and_init; a buffer the fake did not allocate, or one freed already, is a bad free), buf NULL;
 *       is_null = 1, need_clear = false (PRIM_SET_NULL) -- so a second clear of the same value does nothing.
 *   logtb_find_client_type: DB_CLIENT_TYPE_LOG_APPLIER when the scenario is the log applier (LOG_CHECK_LOG_APPLIER).
 *   er_set / er_errid: the ids, the two ER_FK_INVALID arguments (FK name, value string), the last id.
 * Tags: [contract]. Expectations: head b307f5819 (196989e1a + a develop merge that touches none of the files here
 * but locator_sr.c's two lines) 37/37. First commit 1cc4cb1a7 32/37 -- K27-K30 and R3 "(PR#8394 196989e1a)" FAIL:
 * the key buffer is left on those error exits. Base 226352283 (develop) 19/37 -- those five and the cases named
 * "(PR#8394 TO-BE)" and K9 "(PR#8394 설계)" FAIL: k-1 fixes (2(k-1) with partitioned parents: the class
 * representation and the pruning attribute info) and the pruning context (predicate unpack) of every key but the
 * last are left, on the found path and on the exits after a later key. The other cases (normal path, NULL keys,
 * non-FK indexes, first-key exits, R1's codes / order / messages / lock, K31-K34's no double free) PASS at all
 * three: the PR does not change them.
 * "(현재 동작 - develop 동일)" pins a behaviour the PR does not change (K25, K26) -- a later fix would show here.
 * Sensitivity (mut_*.diff, --patch on b307f5819): no per-key representation release -> the 13 first-commit cases of
 * base (K2 K3 K4 K5 K8 K9 K12 K13 K14 K16 K18 K24 R2); no per-key pruning context clear -> K3 K8 K9 K16 K24 R2; free
 * without NULLing (double free at the end) -> 18 FAIL; release after the search on the found path only -> K9 K13 R2
 * (the log applier's go-on leaves the fix); the key cleared on the B-tree error branch only, not at the error label
 * -> K27 K28 K29 R3 (each exit needs it); control ctl_flag_kept (the flag left true: the end clears a cleared
 * context, harmless as the real clear is idempotent) -> 0 FAIL. Running 1cc4cb1a7 is 196989e1a taken out.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "error_manager.h"
#include "memory_alloc.h"
#include "heap_file.h"
#include "partition_sr.h"
#include "btree.h"
#include "object_representation_sr.h"
#include "object_primitive.h"
#include "db_client_type.hpp"
#include "log_impl.h"
#include "thread_entry.hpp"
#include "xserver_interface.h"
#include "locator.h"
#include <cstdarg>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

/* hooks.cpp (end of locator_sr.c) */
extern int unitseam_locator_check_foreign_key (THREAD_ENTRY * thread_p, HFID * hfid, OID * class_oid, OID * inst_oid,
					       RECDES * recdes, RECDES * new_recdes, bool * is_cached,
					       LC_COPYAREA ** copyarea);

namespace
{
  const int MAXC = 10;
  const int SEARCH_ERROR = ER_LK_UNILATERALLY_ABORTED;

  struct fake_class
  {
    OID oid;
    OR_CLASSREP repr;		/* what heap_classrepr_get hands out (the cache entry's representation) */
    int fcnt;			/* the cache entry's fix count */
    bool get_fails;		/* heap_classrepr_get returns NULL */
    BTID pk;			/* the PK BTID a foreign key names (ref_class_pk_btid) */
    /* partitioned parent (repr.has_partition_info) */
    std::vector<OR_PARTITION> parts;	/* [0] master, [1..n] partitions: the heap_get_class_partitions layout */
    std::vector<BTID> part_pk;	/* [i] PK BTID of partition i (what partition_find_inherited_btid finds) */
    std::vector<int> range_upper;	/* range: partition i (1..n) holds v < range_upper[i-1]; empty = hash */
    bool from_cache;		/* the pruning context comes from the partition cache */
    bool load_unpartitioned;	/* load: partitions NULL, NO_ERROR */
    int load_error;
    int attr_start_error;
    int prune_error;
  };

  struct search_rec
  {
    BTID btid;
    OID class_oid;
    SCAN_OPERATION_TYPE op;
    bool all_class;
    bool key_is_read_key;	/* the key searched is the one heap_attrvalue_get_key returned */
    int key_id;
    int own_fixes;		/* this check's heap_classrepr_get fixes not yet freed */
    int pctx_live;		/* pruning-context resources live */
    int child_fcnt;		/* the child's cache entry fix count */
  };

  struct world
  {
    fake_class cls[MAXC];
    int ncls;
    int child;
    std::vector<OR_INDEX> indexes;
    std::vector<OR_FOREIGN_KEY> fks;	/* one per index; only the FK ones are linked */
    std::vector<DB_VALUE> attr_val;	/* per index: the single-column attribute value */
    std::vector<int> key_id;	/* per index: the key's identity (the INT, or a multi-column key's number) */
    std::vector<bool> multi_null;	/* per index: the multi-column key has a NULL element */
    std::set<std::pair<int, int>> rows;	/* (PK root page, key id) */
    int read_error;
    int get_key_fail_at;	/* index position; -1 none */
    bool applier;
    int search_error_at;	/* 1-based search number; 0 none */
    /* ledgers */
    int start_calls, read_calls, end_calls;
    int get_key_calls, get_key_dirty, has_null_calls;
    std::vector<int> get_key_order;
    int cur_index;
    DB_VALUE *cur_key;
    int get_calls, get_ok, free_calls, free_ok, free_errors;
    int load_calls, clear_calls, prune_calls;
    int pred_live, attr_live, parts_live;
    std::vector<BTID> prune_in_btid;
    std::vector<int> er_ids;
    std::vector<std::string> fk_msg_name, fk_msg_value;
    int last_er;
    int valstr_alloc, valstr_free;
    std::set<void *> valstr_live;
    int clear_value_calls;
    std::vector<search_rec> searches;
    /* multi-column key buffers (heap_attrvalue_get_key allocates one when the record is longer than DBVAL_BUFSIZE) */
    bool long_record;
    int keybuf_alloc, keybuf_free, keybuf_bad_free, keybuf_max_live;
    std::set<void *> keybuf_live;
  };

  world W;
  char g_pred_token, g_unpack_token;	/* stand-ins for the unpacked partition predicate */

  void
  reset ()
  {
    W = world ();
    W.get_key_fail_at = -1;
    W.cur_index = -1;
  }

  OID
  mk_oid (int page)
  {
    OID o;
    o.pageid = page;
    o.slotid = 1;
    o.volid = 0;
    return o;
  }

  BTID
  mk_btid (int page)
  {
    BTID b;
    b.vfid.fileid = page;
    b.vfid.volid = 0;
    b.root_pageid = page;
    return b;
  }

  fake_class *
  find_class (const OID * oid)
  {
    for (int i = 0; i < W.ncls; i++)
      {
	if (OID_EQ (&W.cls[i].oid, oid))
	  {
	    return &W.cls[i];
	  }
      }
    return NULL;
  }

  int
  slot_of (const fake_class * c)
  {
    return (int) (c - W.cls);
  }

  int
  pins_left ()
  {
    int n = 0;
    for (int i = 0; i < W.ncls; i++)
      {
	n += W.cls[i].fcnt;
      }
    return n;
  }

  int
  pctx_live ()
  {
    return W.pred_live + W.attr_live + W.parts_live;
  }

  int
  key_identity (const DB_VALUE * key)
  {
    return (key == W.cur_key && W.cur_index >= 0) ? W.key_id[W.cur_index] : -999999;
  }

  /* ---- world building ---- */
  int
  add_class ()
  {
    int c = W.ncls++;
    fake_class & k = W.cls[c];
    k.oid = mk_oid (100 + c);
    k.pk = mk_btid (500 + c);
    k.from_cache = true;
    return c;
  }

  int
  add_partitioned (int nparts, std::vector<int> range_upper = std::vector<int> ())
  {
    int c = add_class ();
    fake_class & k = W.cls[c];
    k.repr.has_partition_info = 1;
    k.parts.assign (nparts + 1, OR_PARTITION ());
    k.part_pk.assign (nparts + 1, BTID ());
    k.range_upper = range_upper;
    k.parts[0].class_oid = k.oid;
    k.parts[0].partition_type = range_upper.empty ()? DB_PARTITION_HASH : DB_PARTITION_RANGE;
    k.parts[0].rep_id = 1;
    for (int i = 1; i <= nparts; i++)
      {
	k.parts[i].class_oid = mk_oid (1000 + c * 10 + i);
	k.parts[i].class_hfid.vfid.fileid = 3000 + c * 10 + i;
	k.parts[i].class_hfid.vfid.volid = 0;
	k.parts[i].class_hfid.hpgid = 3000 + c * 10 + i;
	k.parts[i].partition_type = k.parts[0].partition_type;
	k.part_pk[i] = mk_btid (5000 + c * 10 + i);
      }
    return c;
  }

  /* partition (0-based) that holds v, -1 none */
  int
  partition_of (const fake_class & k, int v)
  {
    int n = (int) k.parts.size () - 1;
    if (k.range_upper.empty ())
      {
	return ((v % n) + n) % n;
      }
    for (int i = 0; i < n; i++)
      {
	if (v < k.range_upper[i])
	  {
	    return i;
	  }
      }
    return -1;
  }

  int
  add_index (BTREE_TYPE type, int n_atts)
  {
    int pos = (int) W.indexes.size ();
    OR_INDEX ix;
    memset (&ix, 0, sizeof (ix));
    ix.type = type;
    ix.n_atts = n_atts;
    ix.btid = mk_btid (700 + pos);
    W.indexes.push_back (ix);
    OR_FOREIGN_KEY fk;
    memset (&fk, 0, sizeof (fk));
    W.fks.push_back (fk);
    DB_VALUE v;
    db_make_null (&v);
    W.attr_val.push_back (v);
    W.key_id.push_back (0);
    W.multi_null.push_back (false);
    return pos;
  }

  int
  add_fk (int ref, const char *name, int n_atts = 1)
  {
    int pos = add_index (BTREE_FOREIGN_KEY, n_atts);
    W.fks[pos].fkname = const_cast < char *>(name);
    W.fks[pos].ref_class_oid = W.cls[ref].oid;
    W.fks[pos].ref_class_pk_btid = W.cls[ref].pk;
    W.fks[pos].self_btid = W.indexes[pos].btid;
    return pos;
  }

  void
  set_key (int pos, int v)
  {
    db_make_int (&W.attr_val[pos], v);
    W.key_id[pos] = v;
    W.multi_null[pos] = false;
  }

  void
  set_null (int pos)
  {
    db_make_null (&W.attr_val[pos]);
    W.key_id[pos] = -1;
    W.multi_null[pos] = true;
  }

  BTID
  pk_holding (int ref, int v, OID * holder)
  {
    fake_class & k = W.cls[ref];
    if (k.repr.has_partition_info && !k.load_unpartitioned)
      {
	int p = partition_of (k, v);
	if (p < 0)
	  {
	    BTID b;
	    BTID_SET_NULL (&b);
	    OID_SET_NULL (holder);
	    return b;
	  }
	*holder = k.parts[p + 1].class_oid;
	return k.part_pk[p + 1];
      }
    *holder = k.oid;
    return k.pk;
  }

  void
  add_row (int ref, int v)
  {
    OID holder;
    BTID b = pk_holding (ref, v, &holder);
    if (!BTID_IS_NULL (&b))
      {
	W.rows.insert (std::make_pair (b.root_pageid, v));
      }
  }

  void
  wire ()
  {
    for (size_t i = 0; i < W.indexes.size (); i++)
      {
	W.indexes[i].fk = (W.indexes[i].type == BTREE_FOREIGN_KEY) ? &W.fks[i] : NULL;
      }
    W.cls[W.child].repr.indexes = W.indexes.empty ()? NULL : W.indexes.data ();
    W.cls[W.child].repr.n_indexes = (int) W.indexes.size ();
  }

  alignas (cubthread::entry) unsigned char thr_storage[sizeof (cubthread::entry)];

  cubthread::entry *
  thr ()
  {
    cubthread::entry * t = reinterpret_cast < cubthread::entry * >(thr_storage);
    t->tran_index = 3;
    return t;
  }

  int
  run ()
  {
    wire ();
    OID inst = mk_oid (9000);
    static char data[DBVAL_BUFSIZE + 128];
    RECDES rec;
    rec.data = data;
    rec.length = W.long_record ? DBVAL_BUFSIZE + 64 : 16;
    rec.area_size = (int) sizeof (data);
    rec.type = REC_HOME;
    RECDES new_rec = rec;
    bool is_cached = false;
    LC_COPYAREA *copyarea = NULL;
    HFID hfid;
    HFID_SET_NULL (&hfid);
    return unitseam_locator_check_foreign_key (thr (), &hfid, &W.cls[W.child].oid, &inst, &rec, &new_rec, &is_cached,
					       &copyarea);
  }

  void
  unfix (int slot, const OR_CLASSREP * repr)
  {
    if (slot < 0 || slot >= W.ncls || repr != &W.cls[slot].repr || W.cls[slot].fcnt <= 0)
      {
	W.free_errors++;
	return;
      }
    W.cls[slot].fcnt--;
  }
}

/* ======================================================================================================== fakes */

/* ---- error manager ---- */
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  W.er_ids.push_back (err_id);
  W.last_er = err_id;
  if (err_id == ER_FK_INVALID && num_args == 2)
    {
      va_list ap;
      va_start (ap, num_args);
      const char *a1 = va_arg (ap, const char *);
      const char *a2 = va_arg (ap, const char *);
      va_end (ap);
      W.fk_msg_name.push_back (a1 != NULL ? a1 : "(null)");
      W.fk_msg_value.push_back (a2 != NULL ? a2 : "(null)");
    }
}

int
er_errid (void)
{
  return W.last_er;
}

/* ---- the key's print and its release ---- */
char *
pr_valstring (const DB_VALUE * val)
{
  std::string s;
  if (val == NULL || DB_IS_NULL (val))
    {
      s = "NULL";
    }
  else if (DB_VALUE_TYPE (val) == DB_TYPE_INTEGER)
    {
      s = std::to_string (db_get_int (val));
    }
  else
    {
      s = "{" + std::to_string (key_identity (val)) + "}";
    }
  char *p = (char *) malloc (s.size () + 1);
  memcpy (p, s.c_str (), s.size () + 1);
  W.valstr_alloc++;
  W.valstr_live.insert (p);
  return p;
}

static void
private_free (void *ptr)
{
  if (W.valstr_live.erase (ptr) == 1)
    {
      W.valstr_free++;
      free (ptr);
    }
}

#if !defined (NDEBUG)
void
db_private_free_debug (THREAD_ENTRY * thrd, void *ptr, bool rc_track, const char *caller_file, int caller_line)
{
  (void) thrd;
  (void) rc_track;
  (void) caller_file;
  (void) caller_line;
  private_free (ptr);
}
#else
void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  private_free (ptr);
}
#endif

int
pr_clear_value (DB_VALUE * value)
{
  W.clear_value_calls++;
  if (value == NULL || DB_IS_NULL (value))
    {
      return NO_ERROR;
    }
  if (DB_VALUE_DOMAIN_TYPE (value) == DB_TYPE_MIDXKEY)
    {
      char *midxkey_buf = value->data.midxkey.buf;
      if (midxkey_buf != NULL)
	{
	  if (value->need_clear)
	    {
	      /* db_private_free_and_init (NULL, midxkey_buf): a buffer heap_attrvalue_get_key allocated; anything
	       * else (the caller's stack buf, or one freed already) is a bad free */
	      if (W.keybuf_live.erase (midxkey_buf) == 1)
		{
		  W.keybuf_free++;
		  free (midxkey_buf);
		}
	      else
		{
		  W.keybuf_bad_free++;
		}
	    }
	  value->data.midxkey.buf = NULL;
	}
    }
  value->domain.general_info.is_null = 1;
  value->need_clear = false;
  return NO_ERROR;
}

/* ---- transaction table ---- */
int
logtb_find_client_type (int tran_index)
{
  (void) tran_index;
  return W.applier ? DB_CLIENT_TYPE_LOG_APPLIER : DB_CLIENT_TYPE_DEFAULT;
}

/* ---- heap: the child's attribute cache ---- */
int
heap_attrinfo_start_with_index (THREAD_ENTRY * thread_p, OID * class_oid, RECDES * class_recdes,
				HEAP_CACHE_ATTRINFO * attr_info, HEAP_IDX_ELEMENTS_INFO * idx_info)
{
  (void) thread_p;
  (void) class_recdes;
  W.start_calls++;
  idx_info->has_single_col = false;
  idx_info->has_multi_col = false;
  idx_info->num_btids = 0;
  attr_info->num_values = -1;
  fake_class *c = find_class (class_oid);
  if (c == NULL || c->get_fails)
    {
      return ER_FAILED;		/* heap_classrepr_get failed: nothing fixed */
    }
  idx_info->num_btids = c->repr.n_indexes;
  int nsingle = 0;
  for (int j = 0; j < c->repr.n_indexes; j++)
    {
      if (c->repr.indexes[j].n_atts == 1)
	{
	  idx_info->has_single_col = true;
	  nsingle++;
	}
      else if (c->repr.indexes[j].n_atts > 1)
	{
	  idx_info->has_multi_col = true;
	}
    }
  if (!idx_info->has_multi_col && nsingle == 0)
    {
      attr_info->num_values = -1;	/* empty attribute cache; the representation freed */
      return 0;
    }
  attr_info->class_oid = *class_oid;
  attr_info->last_cacheindex = slot_of (c);
  attr_info->read_cacheindex = -1;
  attr_info->last_classrepr = &c->repr;
  attr_info->read_classrepr = NULL;
  attr_info->num_values = nsingle;
  attr_info->values = NULL;
  c->fcnt++;
  return nsingle == 0 ? 1 : nsingle;
}

void
heap_attrinfo_end (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info)
{
  (void) thread_p;
  W.end_calls++;
  if (attr_info->num_values == -1)
    {
      return;
    }
  if (attr_info->last_classrepr != NULL)
    {
      unfix (attr_info->last_cacheindex, attr_info->last_classrepr);
      attr_info->last_classrepr = NULL;
    }
  attr_info->num_values = -1;
}

int
heap_attrinfo_read_dbvalues (THREAD_ENTRY * thread_p, const OID * inst_oid, RECDES * recdes,
			     HEAP_CACHE_ATTRINFO * attr_info)
{
  (void) thread_p;
  (void) inst_oid;
  (void) recdes;
  (void) attr_info;
  W.read_calls++;
  if (W.read_error != NO_ERROR)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, W.read_error, 0);
      return W.read_error;
    }
  return NO_ERROR;
}

DB_VALUE *
heap_attrvalue_get_key (THREAD_ENTRY * thread_p, int btid_index, HEAP_CACHE_ATTRINFO * idx_attrinfo,
			RECDES * recdes, BTID * btid, DB_VALUE * db_value, char *buf,
			FUNC_PRED_UNPACK_INFO * func_indx_preds, TP_DOMAIN ** key_domain)
{
  (void) thread_p;
  (void) recdes;
  (void) func_indx_preds;
  (void) key_domain;
  W.get_key_calls++;
  W.get_key_order.push_back (btid_index);
  if (!DB_IS_NULL (db_value))
    {
      W.get_key_dirty++;
    }
  if (idx_attrinfo->num_values == -1 || btid_index >= idx_attrinfo->last_classrepr->n_indexes)
    {
      return NULL;
    }
  if (btid_index == W.get_key_fail_at)
    {
      return NULL;
    }
  OR_INDEX *index = &idx_attrinfo->last_classrepr->indexes[btid_index];
  *btid = index->btid;
  W.cur_index = btid_index;
  if (index->n_atts > 1)
    {
      /* heap_file.c 14740-14784: the buffer is sized by the record (midxkey_size = recdes->length) and allocated
       * when that is over DBVAL_BUFSIZE (db_private_alloc, then need_clear = true after db_make_midxkey) */
      int midxkey_size = recdes != NULL ? recdes->length : 0;
      DB_MIDXKEY midxkey;
      memset (&midxkey, 0, sizeof (midxkey));
      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  midxkey.buf = (char *) malloc (midxkey_size);
	  W.keybuf_live.insert (midxkey.buf);
	  W.keybuf_alloc++;
	  if ((int) W.keybuf_live.size () > W.keybuf_max_live)
	    {
	      W.keybuf_max_live = (int) W.keybuf_live.size ();
	    }
	}
      else
	{
	  midxkey.buf = buf;
	}
      midxkey.size = 8;
      midxkey.ncolumns = index->n_atts;
      midxkey.min_max_val.position = -1;
      db_make_midxkey (db_value, &midxkey);
      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  db_value->need_clear = true;
	}
      W.cur_key = db_value;
      return db_value;
    }
  W.cur_key = &W.attr_val[btid_index];
  return W.cur_key;
}

/* ---- heap: the class representation cache ---- */
OR_CLASSREP *
heap_classrepr_get (THREAD_ENTRY * thread_p, const OID * class_oid, RECDES * class_recdes, REPR_ID reprid,
		    int *idx_incache)
{
  (void) thread_p;
  (void) class_recdes;
  (void) reprid;
  *idx_incache = -1;
  W.get_calls++;
  fake_class *c = find_class (class_oid);
  if (c == NULL || c->get_fails)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 0);
      return NULL;
    }
  c->fcnt++;
  W.get_ok++;
  *idx_incache = slot_of (c);
  return &c->repr;
}

int
heap_classrepr_free (OR_CLASSREP * classrep, int *idx_incache)
{
  W.free_calls++;
  int before = W.free_errors;
  unfix (*idx_incache, classrep);
  if (W.free_errors == before)
    {
      W.free_ok++;
    }
  return NO_ERROR;
}

/* ---- partition pruning ---- */
void
partition_init_pruning_context (PRUNING_CONTEXT * pinfo)
{
  OID_SET_NULL (&pinfo->root_oid);
  pinfo->thread_p = NULL;
  pinfo->partitions = NULL;
  pinfo->selected_partition = NULL;
  pinfo->spec = NULL;
  pinfo->vd = NULL;
  pinfo->count = 0;
  pinfo->fp_cache_context = NULL;
  pinfo->partition_pred = NULL;
  pinfo->attr_position = -1;
  pinfo->error_code = NO_ERROR;
  pinfo->scan_cache_list = NULL;
  pinfo->pruning_type = DB_PARTITIONED_CLASS;
  pinfo->is_attr_info_inited = false;
  pinfo->is_from_cache = false;
}

void
partition_clear_pruning_context (PRUNING_CONTEXT * pinfo)
{
  W.clear_calls++;
  if (!pinfo->is_from_cache && pinfo->partitions != NULL)
    {
      W.parts_live--;		/* heap_clear_partition_info */
    }
  pinfo->partitions = NULL;
  pinfo->selected_partition = NULL;
  pinfo->count = 0;
  bool had_pred = false;
  if (pinfo->partition_pred != NULL)
    {
      pinfo->partition_pred = NULL;
      had_pred = true;
    }
  if (pinfo->fp_cache_context != NULL)
    {
      pinfo->fp_cache_context = NULL;
      had_pred = true;
    }
  if (had_pred)
    {
      W.pred_live--;
    }
  if (pinfo->is_attr_info_inited)
    {
      fake_class *root = find_class (&pinfo->root_oid);
      if (root != NULL)
	{
	  unfix (slot_of (root), &root->repr);
	}
      W.attr_live--;
      pinfo->is_attr_info_inited = false;
    }
  pinfo->scan_cache_list = NULL;
}

int
partition_load_pruning_context (THREAD_ENTRY * thread_p, const OID * class_oid, int pruning_type,
				PRUNING_CONTEXT * pinfo)
{
  W.load_calls++;
  partition_init_pruning_context (pinfo);
  pinfo->pruning_type = pruning_type;
  pinfo->thread_p = thread_p;
  COPY_OID (&pinfo->root_oid, class_oid);
  fake_class *c = find_class (class_oid);
  if (c == NULL || c->load_unpartitioned)
    {
      return NO_ERROR;		/* partitions NULL: "Class is not partitioned, just return" */
    }
  pinfo->partitions = c->parts.data ();
  pinfo->count = (int) c->parts.size ();
  pinfo->is_from_cache = c->from_cache;
  if (!c->from_cache)
    {
      W.parts_live++;
    }
  if (c->load_error != NO_ERROR)
    {
      /* partition_load_partition_predicate failed: error_return */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, c->load_error, 0);
      partition_clear_pruning_context (pinfo);
      pinfo->error_code = c->load_error;
      return c->load_error;
    }
  pinfo->partition_pred = reinterpret_cast < func_pred * >(&g_pred_token);
  pinfo->fp_cache_context = reinterpret_cast < xasl_unpack_info * >(&g_unpack_token);
  W.pred_live++;
  pinfo->partition_type = (DB_PARTITION_TYPE) c->parts[0].partition_type;
  pinfo->root_repr_id = c->parts[0].rep_id;
  return NO_ERROR;
}

int
partition_prune_unique_btid (PRUNING_CONTEXT * pcontext, DB_VALUE * key, OID * class_oid, HFID * class_hfid,
			     BTID * btid)
{
  W.prune_calls++;
  W.prune_in_btid.push_back (*btid);
  fake_class *c = find_class (&pcontext->root_oid);
  if (c == NULL)
    {
      return ER_FAILED;
    }
  if (!pcontext->is_attr_info_inited)
    {
      if (c->attr_start_error != NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, c->attr_start_error, 0);
	  return c->attr_start_error;
	}
      c->fcnt++;		/* heap_attrinfo_start: the root's representation */
      W.attr_live++;
      pcontext->is_attr_info_inited = true;
    }
  if (c->prune_error != NO_ERROR)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, c->prune_error, 0);
      return c->prune_error;
    }
  int pos = partition_of (*c, key_identity (key));
  if (pos < 0)
    {
      OID_SET_NULL (class_oid);
      HFID_SET_NULL (class_hfid);
      BTID_SET_NULL (btid);
      return NO_ERROR;
    }
  COPY_OID (class_oid, &pcontext->partitions[pos + 1].class_oid);
  HFID_COPY (class_hfid, &pcontext->partitions[pos + 1].class_hfid);
  *btid = c->part_pk[pos + 1];
  return NO_ERROR;
}

/* ---- B-tree ---- */
int
btree_multicol_key_has_null (DB_VALUE * key)
{
  W.has_null_calls++;
  return (key == W.cur_key && W.cur_index >= 0 && W.multi_null[W.cur_index]) ? 1 : 0;
}

BTREE_SEARCH
xbtree_find_unique (THREAD_ENTRY * thread_p, BTID * btid, SCAN_OPERATION_TYPE scan_op_type, DB_VALUE * key,
		    OID * class_oid, OID * oid, bool is_all_class_srch)
{
  (void) thread_p;
  search_rec s;
  s.btid = *btid;
  s.class_oid = *class_oid;
  s.op = scan_op_type;
  s.all_class = is_all_class_srch;
  s.key_is_read_key = (key == W.cur_key);
  s.key_id = key_identity (key);
  s.own_fixes = W.get_ok - W.free_ok;
  s.pctx_live = pctx_live ();
  s.child_fcnt = W.cls[W.child].fcnt;
  W.searches.push_back (s);
  if ((int) W.searches.size () == W.search_error_at)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, SEARCH_ERROR, 0);
      return BTREE_ERROR_OCCURRED;
    }
  if (W.rows.count (std::make_pair (btid->root_pageid, s.key_id)) != 0)
    {
      *oid = mk_oid (7000 + s.key_id);
      return BTREE_KEY_FOUND;
    }
  return BTREE_KEY_NOTFOUND;
}

/* ======================================================================================================= checks */

/* nothing this call fixed or loaded is left (PR#8394 TO-BE) */
#define CHECK_NOTHING_LEFT() \
  do \
    { \
      CHECK (pins_left () == 0); \
      CHECK (W.get_ok == W.free_ok); \
      CHECK (W.free_errors == 0); \
      CHECK (W.pred_live == 0); \
      CHECK (W.attr_live == 0); \
      CHECK (W.parts_live == 0); \
      CHECK (W.valstr_alloc == W.valstr_free); \
      CHECK (W.keybuf_live.empty ()); \
      CHECK (W.keybuf_bad_free == 0); \
    } \
  while (0)

/* every multi-column key buffer was freed exactly once (196989e1a: the error exits too) */
#define CHECK_KEY_BUFFERS() \
  do \
    { \
      CHECK (W.keybuf_live.empty ()); \
      CHECK (W.keybuf_free == W.keybuf_alloc); \
      CHECK (W.keybuf_bad_free == 0); \
    } \
  while (0)

static int
fixes_of (int c)
{
  return W.cls[c].fcnt;
}

/* ======================================================================================================== cases */

TEST_CASE ("K1 정상: 일반 부모 FK 1개 - 부모 PK 를 S_SELECT_WITH_LOCK 으로 한 번 찾고 고정 1·해제 1", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p = add_class ();
  int f = add_fk (p, "fk_p");
  set_key (f, 7);
  add_row (p, 7);

  CHECK (run () == NO_ERROR);
  REQUIRE (W.searches.size () == 1);
  CHECK (BTID_IS_EQUAL (&W.searches[0].btid, &W.cls[p].pk));
  CHECK (OID_EQ (&W.searches[0].class_oid, &W.cls[p].oid));
  CHECK (W.searches[0].op == S_SELECT_WITH_LOCK);
  CHECK (W.searches[0].all_class == true);
  CHECK (W.searches[0].key_is_read_key);
  CHECK (W.searches[0].key_id == 7);
  CHECK (W.get_calls == 1);
  CHECK (W.free_ok == 1);
  CHECK (W.load_calls == 0);
  CHECK (W.start_calls == 1);
  CHECK (W.read_calls == 1);
  CHECK (W.end_calls == 1);
  CHECK (W.er_ids.empty ());
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K2 FK 2개(일반 부모 둘): 키마다 놓아 고정 2·해제 2, 첫 부모 고정도 0 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (p1, 1);
  add_row (p2, 2);

  CHECK (run () == NO_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (W.get_calls == 2);
  CHECK (W.free_ok == 2);
  CHECK (fixes_of (p1) == 0);
  CHECK (fixes_of (p2) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K3 FK k=1..6, 일반·분할 부모 번갈아: 고정 수 = 해제 수 = k, 문맥 읽기 = 분할 부모 수, 남는 것 없음 (PR#8394 TO-BE)",
	   "[contract]")
{
  for (int k = 1; k <= 6; k++)
    {
      INFO ("k = " << k);
      reset ();
      W.child = add_class ();
      int nparted = 0;
      static const char *names[] = { "fk1", "fk2", "fk3", "fk4", "fk5", "fk6" };
      for (int i = 0; i < k; i++)
	{
	  int p;
	  if (i % 2 == 1)
	    {
	      p = add_partitioned (3);
	      nparted++;
	    }
	  else
	    {
	      p = add_class ();
	    }
	  int f = add_fk (p, names[i]);
	  set_key (f, 10 + i);
	  add_row (p, 10 + i);
	}
      CHECK (run () == NO_ERROR);
      CHECK ((int) W.searches.size () == k);
      CHECK (W.get_calls == k);
      CHECK (W.free_ok == k);
      CHECK (W.load_calls == nparted);
      CHECK_NOTHING_LEFT ();
    }
}

TEST_CASE ("K4 같은 부모를 가리키는 FK 2개: 그 캐시 항목의 고정 수가 0 으로 돌아온다 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p = add_class ();
  int f1 = add_fk (p, "fk_a");
  int f2 = add_fk (p, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (p, 1);
  add_row (p, 2);

  CHECK (run () == NO_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (fixes_of (p) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K5 자기 참조 FK 2개(부모 = 자식): 탐색 중에도 자식 표현은 속성 캐시가 쥐고(index->fk 유효) 끝나면 고정 0 (PR#8394 TO-BE)",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  int f1 = add_fk (W.child, "fk_self_a");
  int f2 = add_fk (W.child, "fk_self_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (W.child, 1);
  add_row (W.child, 2);

  CHECK (run () == NO_ERROR);
  REQUIRE (W.searches.size () == 2);
  CHECK (W.searches[0].child_fcnt >= 1);
  CHECK (W.searches[1].child_fcnt >= 1);
  CHECK (fixes_of (W.child) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K6 NULL 키: 단일 열 NULL 값·다중 열 NULL 원소인 FK 는 고정·가지치기·탐색을 하지 않는다", "[contract]")
{
  reset ();
  W.child = add_class ();
  int ph = add_partitioned (3);
  int p2 = add_class ();
  int p3 = add_class ();
  int f1 = add_fk (ph, "fk_null_single");
  int f2 = add_fk (p2, "fk_null_multi", 2);
  int f3 = add_fk (p3, "fk_value");
  set_null (f1);
  W.key_id[f2] = 21;
  W.multi_null[f2] = true;
  set_key (f3, 3);
  add_row (p3, 3);

  CHECK (run () == NO_ERROR);
  CHECK (W.get_calls == 1);
  CHECK (W.load_calls == 0);
  REQUIRE (W.searches.size () == 1);
  CHECK (BTID_IS_EQUAL (&W.searches[0].btid, &W.cls[p3].pk));
  CHECK (W.has_null_calls == 1);
  CHECK (W.get_key_dirty == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K7 외래 키가 아닌 인덱스(PK·UNIQUE·일반)는 키를 읽지 않고 건너뛴다", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p = add_class ();
  add_index (BTREE_PRIMARY_KEY, 1);
  add_index (BTREE_UNIQUE, 1);
  add_index (BTREE_INDEX, 2);
  int f = add_fk (p, "fk_p");
  set_key (f, 4);
  add_row (p, 4);

  CHECK (run () == NO_ERROR);
  CHECK (W.get_key_order == std::vector<int> ({ f }));
  CHECK (W.get_calls == 1);
  CHECK (W.searches.size () == 1);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K8 분할 부모 FK 2개(hash·range - PR 본문 재현의 모양): 가지치기가 고른 분할의 OID·PK 로 찾고 술어·속성 정보 고정이 남지 않는다 (PR#8394 TO-BE)",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  int ph = add_partitioned (3);
  int pr = add_partitioned (2, std::vector<int> ({ 100, 200 }));
  int f1 = add_fk (ph, "fk_hash");
  int f2 = add_fk (pr, "fk_range");
  set_key (f1, 1);
  set_key (f2, 150);
  add_row (ph, 1);
  add_row (pr, 150);

  CHECK (run () == NO_ERROR);
  REQUIRE (W.searches.size () == 2);
  /* hash: 1 mod 3 = partition 1 -> partitions[2]; range: 150 < 200 -> partition 1 -> partitions[2] */
  CHECK (BTID_IS_EQUAL (&W.searches[0].btid, &W.cls[ph].part_pk[2]));
  CHECK (OID_EQ (&W.searches[0].class_oid, &W.cls[ph].parts[2].class_oid));
  CHECK (BTID_IS_EQUAL (&W.searches[1].btid, &W.cls[pr].part_pk[2]));
  CHECK (OID_EQ (&W.searches[1].class_oid, &W.cls[pr].parts[2].class_oid));
  REQUIRE (W.prune_in_btid.size () == 2);
  CHECK (BTID_IS_EQUAL (&W.prune_in_btid[0], &W.cls[ph].pk));	/* pruning starts from the FK's root PK */
  CHECK (BTID_IS_EQUAL (&W.prune_in_btid[1], &W.cls[pr].pk));
  CHECK (W.load_calls == 2);
  CHECK (fixes_of (ph) == 0);
  CHECK (fixes_of (pr) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K9 탐색 시점에는 그 키의 클래스 표현 고정·가지치기 문맥이 이미 풀려 있다 (PR#8394 설계 - 탐색 앞 해제)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int ph = add_partitioned (3);
  int p3 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (ph, "fk_b");
  int f3 = add_fk (p3, "fk_c");
  set_key (f1, 1);
  set_key (f2, 5);
  set_key (f3, 3);
  add_row (p1, 1);
  add_row (ph, 5);
  add_row (p3, 3);

  CHECK (run () == NO_ERROR);
  REQUIRE (W.searches.size () == 3);
  for (size_t i = 0; i < W.searches.size (); i++)
    {
      INFO ("search " << i);
      CHECK (W.searches[i].own_fixes == 0);
      CHECK (W.searches[i].pctx_live == 0);
    }
}

TEST_CASE ("K10 분할 부모에서 어느 분할도 키를 담지 못함(NULL BTID): 탐색 없이 ER_FK_INVALID, 메시지에 FK 이름·키, 남는 것 없음",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  int pr = add_partitioned (2, std::vector<int> ({ 100, 200 }));
  int f = add_fk (pr, "fk_range");
  set_key (f, 500);

  CHECK (run () == ER_FK_INVALID);
  CHECK (W.searches.empty ());
  CHECK (W.prune_calls == 1);
  CHECK (W.fk_msg_name == std::vector<std::string> ({ "fk_range" }));
  CHECK (W.fk_msg_value == std::vector<std::string> ({ "500" }));
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K11 ER_FK_INVALID(첫 키가 부모에 없음): 다음 키는 검사하지 않고 값 문자열을 놓으며 남는 것 없음", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  set_key (f1, 5);
  set_key (f2, 6);
  add_row (p2, 6);

  CHECK (run () == ER_FK_INVALID);
  CHECK (W.searches.size () == 1);
  CHECK (W.get_calls == 1);
  CHECK (W.get_key_order == std::vector<int> ({ f1 }));
  CHECK (W.fk_msg_name == std::vector<std::string> ({ "fk_a" }));
  CHECK (W.fk_msg_value == std::vector<std::string> ({ "5" }));
  CHECK (W.valstr_alloc == 1);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K12 ER_FK_INVALID(둘째 키가 부모에 없음): 첫 키의 고정도 남지 않는다 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  set_key (f1, 5);
  set_key (f2, 6);
  add_row (p1, 5);

  CHECK (run () == ER_FK_INVALID);
  CHECK (W.searches.size () == 2);
  CHECK (W.fk_msg_name == std::vector<std::string> ({ "fk_b" }));
  CHECK (fixes_of (p1) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K13 로그 적용기: 없는 키 뒤에도 다음 키를 검사해 ER_FK_INVALID, 다중 열 키값은 키마다 비우고 고정은 키마다 놓는다 (PR#8394 TO-BE)",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  W.applier = true;
  int p1 = add_class ();
  int p2 = add_class ();
  int p3 = add_class ();
  int f1 = add_fk (p1, "fk_a", 2);
  int f2 = add_fk (p2, "fk_b", 2);
  int f3 = add_fk (p3, "fk_c");
  W.key_id[f1] = 31;		/* absent */
  W.key_id[f2] = 32;
  add_row (p2, 32);
  set_key (f3, 33);		/* absent */

  CHECK (run () == ER_FK_INVALID);
  CHECK (W.searches.size () == 3);
  CHECK (W.fk_msg_name == std::vector<std::string> ({ "fk_a", "fk_c" }));
  CHECK (W.fk_msg_value == std::vector<std::string> ({ "{31}", "33" }));
  CHECK (W.get_key_dirty == 0);
  CHECK (fixes_of (p1) == 0);
  CHECK (fixes_of (p2) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K14 B-tree 오류(BTREE_ERROR_OCCURRED, 둘째 키): er_errid 를 돌려주고 멈추며 남는 것 없음 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  int p3 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  int f3 = add_fk (p3, "fk_c");
  set_key (f1, 1);
  set_key (f2, 2);
  set_key (f3, 3);
  add_row (p1, 1);
  add_row (p2, 2);
  add_row (p3, 3);
  W.search_error_at = 2;

  CHECK (run () == SEARCH_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (W.get_calls == 2);
  CHECK (W.fk_msg_name.empty ());
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K15 가지치기 문맥 읽기 실패(분할 부모, 첫 키): 그 오류를 돌려주고 끝 정리가 문맥(두 번째 정리 - 무해)·표현을 놓는다", "[contract]")
{
  reset ();
  W.child = add_class ();
  int ph = add_partitioned (3);
  W.cls[ph].load_error = ER_OUT_OF_VIRTUAL_MEMORY;
  int f = add_fk (ph, "fk_hash");
  set_key (f, 1);
  add_row (ph, 1);

  CHECK (run () == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (W.searches.empty ());
  CHECK (W.load_calls == 1);
  CHECK (W.clear_calls >= 2);	/* error_return's own clear, then the function end's */
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K16 가지치기 문맥 읽기 실패(분할 부모, 둘째 키): 그 오류, 첫 키의 고정·문맥도 남지 않는다 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int ph1 = add_partitioned (3);
  int ph2 = add_partitioned (2);
  W.cls[ph2].load_error = ER_OUT_OF_VIRTUAL_MEMORY;
  int f1 = add_fk (ph1, "fk_a");
  int f2 = add_fk (ph2, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (ph1, 1);
  add_row (ph2, 2);

  CHECK (run () == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (W.searches.size () == 1);
  CHECK (fixes_of (ph1) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K17 가지치기 실패(속성 정보를 연 뒤·열기 전): 그 오류를 돌려주고 끝 정리가 속성 정보 고정·문맥·표현을 놓는다", "[contract]")
{
  for (int variant = 0; variant < 2; variant++)
    {
      INFO ((variant == 0 ? "prune fails after heap_attrinfo_start" : "heap_attrinfo_start fails"));
      reset ();
      W.child = add_class ();
      int ph = add_partitioned (3);
      if (variant == 0)
	{
	  W.cls[ph].prune_error = ER_PARTITION_WORK_FAILED;
	}
      else
	{
	  W.cls[ph].attr_start_error = ER_PARTITION_WORK_FAILED;
	}
      int f = add_fk (ph, "fk_hash");
      set_key (f, 1);
      add_row (ph, 1);

      CHECK (run () == ER_PARTITION_WORK_FAILED);
      CHECK (W.searches.empty ());
      CHECK (fixes_of (ph) == 0);
      CHECK_NOTHING_LEFT ();
    }
}

TEST_CASE ("K18 부모 클래스 표현을 못 얻음(둘째 키): ER_FAILED, 첫 키의 고정도 남지 않는다 (PR#8394 TO-BE)", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  W.cls[p2].get_fails = true;
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (p1, 1);

  CHECK (run () == ER_FAILED);
  CHECK (W.searches.size () == 1);
  CHECK (W.get_calls == 2);
  CHECK (fixes_of (p1) == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K19 키를 못 읽음(heap_attrvalue_get_key NULL, 둘째 FK): ER_FAILED, 남는 것 없음", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int p2 = add_class ();
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (p2, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (p1, 1);
  W.get_key_fail_at = f2;

  CHECK (run () == ER_FAILED);
  CHECK (W.searches.size () == 1);
  CHECK (W.get_calls == 1);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K20 레코드 값을 못 읽음(heap_attrinfo_read_dbvalues 오류): 그 오류, FK 를 보지 않고 속성 캐시를 닫는다", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p = add_class ();
  int f = add_fk (p, "fk_p");
  set_key (f, 1);
  add_row (p, 1);
  W.read_error = ER_OUT_OF_VIRTUAL_MEMORY;

  CHECK (run () == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (W.get_key_calls == 0);
  CHECK (W.get_calls == 0);
  CHECK (W.end_calls == 1);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K21 다중 열 FK 2개(midxkey 가 호출자 dbvalue 로 돌아옴): 키마다 dbvalue 를 비워 다음 키 읽기가 NULL 에서 시작", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p1 = add_class ();
  int ph = add_partitioned (3);
  int f1 = add_fk (p1, "fk_m1", 2);
  int f2 = add_fk (ph, "fk_m2", 3);
  W.key_id[f1] = 11;
  W.key_id[f2] = 12;
  add_row (p1, 11);
  add_row (ph, 12);

  CHECK (run () == NO_ERROR);
  REQUIRE (W.searches.size () == 2);
  CHECK (W.searches[0].key_is_read_key);
  CHECK (W.searches[1].key_is_read_key);
  CHECK (W.searches[1].key_id == 12);
  CHECK (W.get_key_dirty == 0);
  CHECK (W.has_null_calls == 2);
}

TEST_CASE ("K22 색인 속성이 없는 표(heap_attrinfo_start_with_index = 0): FK 검사 없이 NO_ERROR, 고정 없음", "[contract]")
{
  reset ();
  W.child = add_class ();

  CHECK (run () == NO_ERROR);
  CHECK (W.start_calls == 1);
  CHECK (W.get_key_calls == 0);
  CHECK (W.get_calls == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K23 분할 표시가 있으나 가지치기 문맥에 분할이 없음(load 의 partitions NULL): 루트 PK·루트 OID 로 찾고 문맥은 정리", "[contract]")
{
  reset ();
  W.child = add_class ();
  int pu = add_partitioned (2);
  W.cls[pu].load_unpartitioned = true;
  int f = add_fk (pu, "fk_u");
  set_key (f, 4);
  add_row (pu, 4);

  CHECK (run () == NO_ERROR);
  CHECK (W.prune_calls == 0);
  REQUIRE (W.searches.size () == 1);
  CHECK (BTID_IS_EQUAL (&W.searches[0].btid, &W.cls[pu].pk));
  CHECK (OID_EQ (&W.searches[0].class_oid, &W.cls[pu].oid));
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K24 분할 부모 2개, 문맥을 캐시 밖에서 읽음(is_from_cache false - 분할 배열도 문맥 소유): 키마다 배열까지 놓는다 (PR#8394 TO-BE)",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  int ph1 = add_partitioned (3);
  int ph2 = add_partitioned (2);
  W.cls[ph1].from_cache = false;
  W.cls[ph2].from_cache = false;
  int f1 = add_fk (ph1, "fk_a");
  int f2 = add_fk (ph2, "fk_b");
  set_key (f1, 1);
  set_key (f2, 2);
  add_row (ph1, 1);
  add_row (ph2, 2);

  CHECK (run () == NO_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K25 (현재 동작 - develop 동일) 자식 표현을 못 얻어 heap_attrinfo_start_with_index 가 ER_FAILED: FK 검사 없이 NO_ERROR", "[contract]")
{
  reset ();
  W.child = add_class ();
  int p = add_class ();
  int f = add_fk (p, "fk_p");
  set_key (f, 1);
  W.cls[W.child].get_fails = true;

  CHECK (run () == NO_ERROR);
  CHECK (W.get_key_calls == 0);
  CHECK (W.searches.empty ());
  CHECK (W.end_calls == 0);
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K26 (현재 동작 - develop 동일) 로그 적용기: 앞 키의 ER_FK_INVALID 뒤 분할 부모 키의 문맥 읽기·가지치기가 error_code 를 NO_ERROR 로 덮는다",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  W.applier = true;
  int p1 = add_class ();
  int ph = add_partitioned (3);
  int f1 = add_fk (p1, "fk_a");
  int f2 = add_fk (ph, "fk_b");
  set_key (f1, 5);		/* absent */
  set_key (f2, 6);
  add_row (ph, 6);

  CHECK (run () == NO_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (W.fk_msg_name == std::vector<std::string> ({ "fk_a" }));
  CHECK (W.er_ids.size () == 1);	/* the ER_FK_INVALID stays set in the error area while NO_ERROR is returned */
}

/* ---- the key value of a multi-column foreign key (196989e1a): heap_attrvalue_get_key builds it in a buffer it
 * allocates when the record is longer than DBVAL_BUFSIZE; every exit must free that buffer once ---- */

TEST_CASE ("K27 버퍼를 가진 다중 열 키가 가지치기 실패로 나감(속성 정보를 연 뒤·열기 전): 키 버퍼를 놓는다 (PR#8394 196989e1a)", "[contract]")
{
  for (int variant = 0; variant < 2; variant++)
    {
      INFO ((variant == 0 ? "prune fails after heap_attrinfo_start" : "heap_attrinfo_start fails"));
      reset ();
      W.child = add_class ();
      W.long_record = true;
      int ph = add_partitioned (3);
      if (variant == 0)
	{
	  W.cls[ph].prune_error = ER_PARTITION_WORK_FAILED;
	}
      else
	{
	  W.cls[ph].attr_start_error = ER_PARTITION_WORK_FAILED;
	}
      int f = add_fk (ph, "fk_m", 2);
      W.key_id[f] = 1;
      add_row (ph, 1);

      CHECK (run () == ER_PARTITION_WORK_FAILED);
      CHECK (W.keybuf_alloc == 1);
      CHECK (W.searches.empty ());
      CHECK_KEY_BUFFERS ();
      CHECK_NOTHING_LEFT ();
    }
}

TEST_CASE ("K28 버퍼를 가진 다중 열 키가 가지치기 문맥 읽기 실패로 나감: 키 버퍼를 놓는다 (PR#8394 196989e1a)", "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int ph = add_partitioned (3);
  W.cls[ph].load_error = ER_OUT_OF_VIRTUAL_MEMORY;
  int f = add_fk (ph, "fk_m", 2);
  W.key_id[f] = 1;
  add_row (ph, 1);

  CHECK (run () == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (W.keybuf_alloc == 1);
  CHECK_KEY_BUFFERS ();
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K29 버퍼를 가진 다중 열 키가 부모 클래스 표현 고정 실패로 나감: 키 버퍼를 놓는다 (PR#8394 196989e1a)", "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int p = add_class ();
  W.cls[p].get_fails = true;
  int f = add_fk (p, "fk_m", 2);
  W.key_id[f] = 1;

  CHECK (run () == ER_FAILED);
  CHECK (W.keybuf_alloc == 1);
  CHECK_KEY_BUFFERS ();
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K30 버퍼를 가진 다중 열 키가 부모 PK 탐색의 BTREE_ERROR_OCCURRED(락 실패)로 나감: 키 버퍼를 놓는다 (PR#8394 196989e1a)",
	   "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int p = add_class ();
  int f = add_fk (p, "fk_m", 2);
  W.key_id[f] = 1;
  add_row (p, 1);
  W.search_error_at = 1;

  CHECK (run () == SEARCH_ERROR);
  CHECK (W.searches.size () == 1);
  CHECK (W.keybuf_alloc == 1);
  CHECK_KEY_BUFFERS ();
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K31 버퍼를 가진 다중 열 키 2개가 정상으로 끝남: 키마다 다음 키 전에 한 번씩 놓고 이중 해제 없음", "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int p1 = add_class ();
  int ph = add_partitioned (3);
  int f1 = add_fk (p1, "fk_m1", 2);
  int f2 = add_fk (ph, "fk_m2", 3);
  W.key_id[f1] = 11;
  W.key_id[f2] = 12;
  add_row (p1, 11);
  add_row (ph, 12);

  CHECK (run () == NO_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (W.keybuf_alloc == 2);
  CHECK (W.keybuf_max_live == 1);
  CHECK (W.get_key_dirty == 0);
  CHECK_KEY_BUFFERS ();
}

TEST_CASE ("K32 버퍼를 가진 다중 열 키의 ER_FK_INVALID 출구(일반·로그 적용기): 키 버퍼를 한 번만 놓는다", "[contract]")
{
  for (int variant = 0; variant < 2; variant++)
    {
      INFO ((variant == 0 ? "ER_FK_INVALID, goto error" : "log applier: ER_FK_INVALID, then the next key"));
      reset ();
      W.child = add_class ();
      W.long_record = true;
      W.applier = variant == 1;
      int p1 = add_class ();
      int p2 = add_class ();
      int f1 = add_fk (p1, "fk_m1", 2);
      int f2 = add_fk (p2, "fk_m2", 2);
      W.key_id[f1] = 21;	/* absent */
      W.key_id[f2] = 22;
      add_row (p2, 22);

      CHECK (run () == ER_FK_INVALID);
      CHECK (W.fk_msg_name.front () == "fk_m1");
      CHECK (W.keybuf_alloc == (variant == 0 ? 1 : 2));
      CHECK (W.keybuf_max_live == 1);
      CHECK_KEY_BUFFERS ();
    }
}

TEST_CASE ("K33 버퍼를 가진 다중 열 키에 NULL 원소: 검사 없이 키 버퍼를 한 번 놓는다", "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int p = add_class ();
  int f = add_fk (p, "fk_m", 2);
  W.key_id[f] = 31;
  W.multi_null[f] = true;

  CHECK (run () == NO_ERROR);
  CHECK (W.get_calls == 0);
  CHECK (W.searches.empty ());
  CHECK (W.keybuf_alloc == 1);
  CHECK_KEY_BUFFERS ();
  CHECK_NOTHING_LEFT ();
}

TEST_CASE ("K34 버퍼를 가진 다중 열 키를 마친 뒤 다음(단일 열) 키에서 오류로 나감: 이미 놓은 키 버퍼를 다시 놓지 않는다", "[contract]")
{
  reset ();
  W.child = add_class ();
  W.long_record = true;
  int p1 = add_class ();
  int p2 = add_class ();
  int f1 = add_fk (p1, "fk_m", 2);
  int f2 = add_fk (p2, "fk_s");
  W.key_id[f1] = 41;
  add_row (p1, 41);
  set_key (f2, 42);
  add_row (p2, 42);
  W.search_error_at = 2;

  CHECK (run () == SEARCH_ERROR);
  CHECK (W.searches.size () == 2);
  CHECK (W.keybuf_alloc == 1);
  CHECK (W.keybuf_max_live == 1);
  CHECK_KEY_BUFFERS ();
}

/* ---------------------------------------------------------------------------------------- generated scenarios */

namespace
{
  struct expect
  {
    int ret;
    bool ret_overwritten;	/* the log applier's ER_FK_INVALID was overwritten by a later partitioned key (K26) */
    std::vector<std::pair<int, int>> searches;	/* (BTID root page, class OID page) */
    std::vector<std::string> msg_name, msg_value;
  };

  std::string
  print_key (int pos)
  {
    return W.indexes[pos].n_atts > 1 ? "{" + std::to_string (W.key_id[pos]) + "}" : std::to_string (W.key_id[pos]);
  }

  /* the documented behaviour, read from the script before the run */
  expect
  oracle ()
  {
    expect e;
    int err = NO_ERROR;
    bool fk_invalid_seen = false;
    int nsearch = 0;
    e.ret_overwritten = false;
    bool has_single_col = false;
    for (size_t pos = 0; pos < W.indexes.size (); pos++)
      {
	has_single_col = has_single_col || W.indexes[pos].n_atts == 1;
      }
    if (W.read_error != NO_ERROR && has_single_col)
      {
	e.ret = W.read_error;	/* heap_attrinfo_read_dbvalues is called only when idx_info.has_single_col */
	return e;
      }
    for (size_t pos = 0; pos < W.indexes.size (); pos++)
      {
	if (W.indexes[pos].type != BTREE_FOREIGN_KEY)
	  {
	    continue;
	  }
	if ((int) pos == W.get_key_fail_at)
	  {
	    e.ret = ER_FAILED;
	    return e;
	  }
	bool has_null = W.indexes[pos].n_atts > 1 ? (bool) W.multi_null[pos] : DB_IS_NULL (&W.attr_val[pos]);
	if (has_null)
	  {
	    continue;
	  }
	fake_class *k = find_class (&W.fks[pos].ref_class_oid);
	if (k->get_fails)
	  {
	    e.ret = ER_FAILED;
	    return e;
	  }
	BTID b = k->pk;
	OID o = k->oid;
	if (k->repr.has_partition_info)
	  {
	    if (k->load_error != NO_ERROR)
	      {
		e.ret = k->load_error;
		return e;
	      }
	    err = NO_ERROR;	/* error_code = partition_load_pruning_context (...) */
	    if (!k->load_unpartitioned)
	      {
		if (k->attr_start_error != NO_ERROR)
		  {
		    e.ret = k->attr_start_error;
		    return e;
		  }
		if (k->prune_error != NO_ERROR)
		  {
		    e.ret = k->prune_error;
		    return e;
		  }
		b = pk_holding ((int) (k - W.cls), W.key_id[pos], &o);
	      }
	  }
	bool found = false;
	if (!BTID_IS_NULL (&b))
	  {
	    e.searches.push_back (std::make_pair (b.root_pageid, o.pageid));
	    nsearch++;
	    if (nsearch == W.search_error_at)
	      {
		e.ret = SEARCH_ERROR;
		return e;
	      }
	    found = W.rows.count (std::make_pair (b.root_pageid, W.key_id[pos])) != 0;
	  }
	if (!found)
	  {
	    e.msg_name.push_back (W.fks[pos].fkname);
	    e.msg_value.push_back (print_key ((int) pos));
	    err = ER_FK_INVALID;
	    fk_invalid_seen = true;
	    if (W.applier)
	      {
		continue;
	      }
	    e.ret = ER_FK_INVALID;
	    return e;
	  }
      }
    e.ret = err;
    e.ret_overwritten = fk_invalid_seen && err != ER_FK_INVALID;
    return e;
  }

  /* a random child: 1-5 foreign keys over plain / hash / range / owned-context / self parents, 0-2 other indexes,
   * NULL and multi-column keys, absent keys, the log applier, at most one scripted failure */
  void
  gen (std::mt19937 & rng)
  {
    auto pick = [&rng] (int n) { return (int) (rng () % (unsigned) n); };
    reset ();
    W.child = add_class ();
    int pa = add_class ();
    int pb = add_class ();
    int ph = add_partitioned (3);
    int pr = add_partitioned (2, std::vector<int> ({ 100, 200 }));
    int po = add_partitioned (4);
    W.cls[po].from_cache = false;
    int pu = add_partitioned (2);
    W.cls[pu].load_unpartitioned = true;
    int parents[] = { pa, pb, ph, pr, po, pu, W.child };
    static const char *names[] = { "fk0", "fk1", "fk2", "fk3", "fk4", "fk5", "fk6", "fk7", "fk8" };
    W.applier = pick (10) < 3;
    int nfk = 1 + pick (5);
    int nother = pick (3);
    int total = nfk + nother;
    std::vector<bool> is_fk (total, false);
    for (int i = 0; i < nfk; i++)
      {
	is_fk[i] = true;
      }
    std::shuffle (is_fk.begin (), is_fk.end (), rng);
    for (int i = 0; i < total; i++)
      {
	if (!is_fk[i])
	  {
	    static const BTREE_TYPE other[] = { BTREE_PRIMARY_KEY, BTREE_UNIQUE, BTREE_INDEX };
	    add_index (other[pick (3)], 1 + pick (2));
	    continue;
	  }
	int ref = parents[pick (7)];
	int n_atts = pick (4) == 0 ? 2 : 1;
	int pos = add_fk (ref, names[i], n_atts);
	int v = 1 + pick (250);
	if (pick (7) == 0)
	  {
	    if (n_atts > 1)
	      {
		W.key_id[pos] = v;
		W.multi_null[pos] = true;
	      }
	    else
	      {
		set_null (pos);
	      }
	    continue;
	  }
	if (n_atts > 1)
	  {
	    W.key_id[pos] = v;
	  }
	else
	  {
	    set_key (pos, v);
	  }
	if (pick (10) < 7)
	  {
	    add_row (ref, v);
	  }
      }
    switch (pick (12))
      {
      case 0:
	W.cls[parents[pick (6)]].get_fails = true;
	break;
      case 1:
	W.cls[ph].load_error = ER_OUT_OF_VIRTUAL_MEMORY;
	break;
      case 2:
	W.cls[pr].prune_error = ER_PARTITION_WORK_FAILED;
	break;
      case 3:
	W.cls[po].attr_start_error = ER_PARTITION_WORK_FAILED;
	break;
      case 4:
	W.search_error_at = 1 + pick (3);
	break;
      case 5:
	W.get_key_fail_at = pick (total);
	break;
      case 6:
	if (pick (3) == 0)
	  {
	    W.read_error = ER_OUT_OF_VIRTUAL_MEMORY;
	  }
	break;
      default:
	break;
      }
  }
}

TEST_CASE ("R1 생성 시나리오 3000개: 반환 코드·탐색 순서(분할 PK·OID)·ER_FK_INVALID 메시지·락 모드가 문서된 동작과 같다(PR 이 바꾸지 않는 것)",
	   "[contract]")
{
  std::mt19937 rng (8394);
  int mismatches = 0, overwritten = 0, ran = 0;
  int by_ret[6] = { 0 };	/* NO_ERROR, ER_FK_INVALID, ER_FAILED, search error, pruning errors, other */
  int with_applier = 0, with_partitioned_search = 0, with_multi = 0;
  for (int it = 0; it < 3000; it++)
    {
      gen (rng);
      wire ();
      expect e = oracle ();
      int ret = run ();
      ran++;
      by_ret[ret == NO_ERROR ? 0 : ret == ER_FK_INVALID ? 1 : ret == ER_FAILED ? 2 : ret == SEARCH_ERROR ? 3
	     : (ret == ER_PARTITION_WORK_FAILED || ret == ER_OUT_OF_VIRTUAL_MEMORY) ? 4 : 5]++;
      with_applier += W.applier ? 1 : 0;
      with_partitioned_search += W.prune_calls > 0 ? 1 : 0;
      with_multi += W.has_null_calls > 0 ? 1 : 0;
      bool ok = true;
      if (!e.ret_overwritten)
	{
	  ok = ok && ret == e.ret;
	}
      else
	{
	  overwritten++;
	}
      ok = ok && W.searches.size () == e.searches.size ();
      for (size_t i = 0; ok && i < W.searches.size (); i++)
	{
	  ok = W.searches[i].btid.root_pageid == e.searches[i].first && W.searches[i].class_oid.pageid == e.searches[i].second
	    && W.searches[i].op == S_SELECT_WITH_LOCK && W.searches[i].all_class && W.searches[i].key_is_read_key;
	}
      ok = ok && W.fk_msg_name == e.msg_name && W.fk_msg_value == e.msg_value;
      if (!ok)
	{
	  if (mismatches < 3)
	    {
	      UNSCOPED_INFO ("iteration " << it << ": ret " << ret << " expected " << e.ret << ", searches " <<
			     W.searches.size () << " expected " << e.searches.size () << ", msgs " << W.fk_msg_name.size ()
			     << " expected " << e.msg_name.size ());
	    }
	  mismatches++;
	}
    }
  WARN ("R1 coverage: ran " << ran << " - NO_ERROR " << by_ret[0] << ", ER_FK_INVALID " << by_ret[1] << ", ER_FAILED " << by_ret[2]
	<< ", B-tree error " << by_ret[3] << ", pruning/read errors " << by_ret[4] << ", other " << by_ret[5] <<
	"; log applier " << with_applier << ", pruned " << with_partitioned_search << ", multi-column " << with_multi <<
	"; log-applier overwrite (return code not compared) " << overwritten);
  CHECK (mismatches == 0);
}

TEST_CASE ("R2 생성 시나리오 3000개: 어떤 출구로 끝나도 고정·가지치기 문맥·값 문자열이 남지 않고 이중 해제가 없다 (PR#8394 TO-BE)",
	   "[contract]")
{
  std::mt19937 rng (8394);
  int leaks = 0, ran = 0, multi_fix = 0;
  for (int it = 0; it < 3000; it++)
    {
      gen (rng);
      (void) run ();
      ran++;
      if (W.get_ok >= 2)
	{
	  multi_fix++;
	}
      bool ok = pins_left () == 0 && W.get_ok == W.free_ok && W.free_errors == 0 && pctx_live () == 0
	&& W.valstr_alloc == W.valstr_free && W.get_key_dirty == 0;
      if (!ok)
	{
	  if (leaks < 3)
	    {
	      UNSCOPED_INFO ("iteration " << it << ": pins left " << pins_left () << ", get " << W.get_ok << " free " <<
			     W.free_ok << ", free errors " << W.free_errors << ", pruning context live " << pctx_live ()
			     << ", value strings " << W.valstr_alloc << "/" << W.valstr_free);
	    }
	  leaks++;
	}
    }
  WARN ("R2 coverage: ran " << ran << ", scenarios with two or more fixes " << multi_fix << ", leaving something " <<
	leaks);
  CHECK (leaks == 0);
}

TEST_CASE ("R3 생성 시나리오 3000개(긴 레코드 - 다중 열 키가 버퍼를 가짐): 어떤 출구로 끝나도 키 버퍼가 남지 않고 이중 해제가 없다 (PR#8394 196989e1a)",
	   "[contract]")
{
  std::mt19937 rng (8394);
  int leaks = 0, ran = 0, with_buffer = 0, error_exit_with_buffer = 0;
  for (int it = 0; it < 3000; it++)
    {
      gen (rng);
      W.long_record = true;
      int ret = run ();
      ran++;
      with_buffer += W.keybuf_alloc > 0 ? 1 : 0;
      if (W.keybuf_alloc > 0 && ret != NO_ERROR && ret != ER_FK_INVALID)
	{
	  error_exit_with_buffer++;
	}
      bool ok = W.keybuf_live.empty () && W.keybuf_free == W.keybuf_alloc && W.keybuf_bad_free == 0
	&& W.keybuf_max_live <= 1 && W.get_key_dirty == 0;
      if (!ok)
	{
	  if (leaks < 3)
	    {
	      UNSCOPED_INFO ("iteration " << it << ": ret " << ret << ", key buffers " << W.keybuf_alloc << " allocated, "
			     << W.keybuf_free << " freed, " << W.keybuf_bad_free << " bad frees, max live " <<
			     W.keybuf_max_live);
	    }
	  leaks++;
	}
    }
  WARN ("R3 coverage: ran " << ran << ", with a multi-column key buffer " << with_buffer <<
	", error exits (not ER_FK_INVALID) after a key buffer " << error_exit_with_buffer << ", leaving a buffer " <<
	leaks);
  CHECK (leaks == 0);
}
