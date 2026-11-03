/*
 * Contract suite -- CBRD-27871 (PR#8376): btree_get_next_key_info (src/storage/btree.c, SERVER_MODE) and the
 * restart-from-root request (bts->force_restart_from_root) of a descending key info scan.
 *   - a restart raised by the resume of a later call is met before any key is read: the scan locates again (resume
 *     again) and the row is the key it then lands on -- never slot 0 of the leaf it let go;
 *   - a restart raised by the first start is met by starting again (btree_prepare_bts is not called again);
 *   - a leaf still fixed when the restart is raised is unfixed (cur_key kept) before the scan locates again;
 *   - a scan the new locate ends is S_END.
 * Real code: btree_get_next_key_info, btree_range_scan_start, btree_range_scan_resume (the flag handling of the
 * start and resume is part of what is tested, so they are NOT faked -- PR#8376 depends on how a start meets a request
 * left set, which is PR#8363's change).
 * Fakes (seams), each playing the branch of the real function named below:
 *   btree_prepare_bts -- counts; leaves the scan as BTREE_INIT_SCAN + key_range NULL would (lower_key NULL).
 *   btree_find_lower_bound_leaf -- (start, lower_key NULL) fixes the rightmost leaf, C_vpid = it, slot = key count
 *       (btree.c @abc0ad106 18850-18935, use_desc_index branch). After 10 starts it ends the scan (the empty-index
 *       branch: end_scan) so that a start that never ends is seen as a count, not a hang.
 *   btree_locate_key_from_saved_leaf -- (resume) *leaf_page = NULL first as the real one; a saved leaf_vpid -> the
 *       saved leaf unchanged (leaf_unchanged); a NULL leaf_vpid (resume met a restart) -> the key looked up from the
 *       root on the script's relocation leaf (found).
 *   btree_range_scan_advance_over_filtered_keys -- scripted per call:
 *       OK(slot)     positions on slot of C_page, key verified, cur_key = a copied INT;
 *       RESTART      btree_range_scan_descending_fix_prev_leaf, previous leaf deallocated (btree.c 29560-29567): C_page
 *                    unfixed and NULL, slot 0, flag raised;
 *       RESTART_HELD the same with the current leaf fixed again and the key not found in it (BTREE_KEY_NOTFOUND,
 *                    btree.c 29666-29672): C_page still fixed, slot 0, flag raised;
 *       END          no previous leaf: end_scan, C_page still fixed.
 *   btree_record_get_num_oids, btree_key_find_first_visible_row(_from_all_ovf), btree_leaf_is_flaged -- the key's
 *       record reads; they note whether a page was fixed when the key was read.
 *   pgbuf_unfix, pgbuf_get_lsa -- a fix/unfix ledger over fake pages (double unfix and an unfix of a page not fixed are
 *       counted as errors).
 * Tags: [contract]. Expectations at base 1802aa581 (develop): R1 R1b R2 R4 R5 FAIL (the request is not met: the row is
 * slot 0 of the let-go leaf, or the start's NULL position). At head abc0ad106 without PR#8363: R2 FAIL (the start
 * meets the request left set only with PR#8363 -- PR#8376 body says so; run with --patch pr8363.diff to see it close).
 */
#include "catch.hpp"
#include "config.h"
#include "btree.h"
#include "btree_load.h"
#include "scan_manager.h"
#include "access_spec.hpp"
#include "system_parameter.h"
#include "dbtype.h"
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <string>

/* system parameters: the head path logs through btree_log_if_enabled (prm_get_bool_value inline reads prm_Def). */
SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };

/* ---- fake pages and the latch ledger ---- */
enum
{ NPAGES = 8, PAGE_BYTES = 256 };
static char g_page_mem[NPAGES][PAGE_BYTES];
static LOG_LSA g_lsa[NPAGES];
static int g_fixed[NPAGES];	/* fix count per page */
static int g_unfix_calls;
static int g_latch_errors;	/* unfix of a page not fixed */
static int
page_no (PAGE_PTR p)
{
  for (int i = 0; i < NPAGES; i++)
    {
      if (p == g_page_mem[i])
	{
	  return i;
	}
    }
  return -1;
}

static PAGE_PTR
fix_page (int no)
{
  g_fixed[no]++;
  return g_page_mem[no];
}

static int
fixed_total ()
{
  int n = 0;
  for (int i = 0; i < NPAGES; i++)
    {
      n += g_fixed[i];
    }
  return n;
}

void
pgbuf_unfix (THREAD_ENTRY * thread_p, PAGE_PTR pgptr)
{
  (void) thread_p;
  g_unfix_calls++;
  int no = page_no (pgptr);
  if (no < 0 || g_fixed[no] <= 0)
    {
      g_latch_errors++;
      return;
    }
  g_fixed[no]--;
}

LOG_LSA *
pgbuf_get_lsa (PAGE_PTR pgptr)
{
  int no = page_no (pgptr);
  return &g_lsa[no < 0 ? 0 : no];
}

/* ---- DB_VALUE helpers of object/ (keys here are INT, the key info strings are constants): clear = make NULL,
 * clone = bit copy -- what the real ones do for values that own no memory ---- */
int
pr_clear_value (DB_VALUE * value)
{
  if (value != NULL)
    {
      value->need_clear = false;
      db_make_null (value);
    }
  return NO_ERROR;
}

int
pr_clone_value (const DB_VALUE * src, DB_VALUE * dest)
{
  *dest = *src;
  dest->need_clear = false;
  return NO_ERROR;
}

int
db_value_domain_init (DB_VALUE * value, const DB_TYPE type, const int precision, const int scale)
{
  /* the type-independent part of db_macro.c's (compat/db_macro.c 172-180) */
  value->data.ch.info.codeset = 0;
  value->domain.general_info.type = type;
  value->domain.numeric_info.precision = precision;
  value->domain.numeric_info.scale = scale;
  value->domain.numeric_info.is_value_negative = false;
  value->need_clear = false;
  value->domain.general_info.is_null = 1;
  return NO_ERROR;
}

/* ---- script ---- */
enum step_kind
{ OK, RESTART, RESTART_HELD, END };
struct step
{
  step_kind kind;
  int slot;
};

static std::vector<step> g_adv;	/* per advance call */
static size_t g_adv_i;
static int g_relocate_page;	/* leaf the key is found on when a resume looks it up from the root */
static int g_relocate_slot;
static int g_start_page;	/* rightmost leaf */
static int g_start_keys;

static int g_prepare_calls, g_start_calls, g_resume_calls, g_advance_calls;
static int g_resume_with_page_fixed;	/* a resume entered while the scan still had a page fixed */
static int g_advance_without_page;	/* an advance called with C_page NULL (the real one asserts it) */
static int g_key_read_without_page;	/* the key's record read with no page fixed */
static int g_slot0_key_reads;	/* the key's record read at slot 0 (the leaf header slot, not a key) */
static std::vector<int> g_unfixes_before_resume;	/* g_unfix_calls seen by each resume on entry */

static int
key_value_for (int page, int slot)
{
  return page * 1000 + slot;
}

static void
position (BTREE_SCAN * bts, int slot)
{
  bts->slot_id = slot;
  bts->key_status = BTS_KEY_IS_VERIFIED;
  btree_clear_key_value (&bts->clear_cur_key, &bts->cur_key);
  db_make_int (&bts->cur_key, key_value_for (page_no (bts->C_page), slot));
  bts->is_cur_key_copied = true;
  bts->is_cur_key_compressed = false;
}

int
btree_prepare_bts (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTID * btid, INDX_SCAN_ID * index_scan_id_p,
		   key_val_range * kv_range, FILTER_INFO * filter, const OID * match_class_oid, DB_BIGINT * key_limit_upper,
		   DB_BIGINT * key_limit_lower, bool need_to_check_null, void *bts_other)
{
  (void) thread_p;
  (void) btid;
  (void) index_scan_id_p;
  (void) kv_range;
  (void) filter;
  (void) match_class_oid;
  (void) key_limit_upper;
  (void) key_limit_lower;
  (void) need_to_check_null;
  (void) bts_other;
  g_prepare_calls++;
  bts->key_range.lower_key = NULL;
  bts->key_range.upper_key = NULL;
  bts->key_range.range = INF_INF;
  bts->is_btid_int_valid = true;
  return NO_ERROR;
}

int
btree_find_lower_bound_leaf (THREAD_ENTRY * thread_p, BTREE_SCAN * bts, BTREE_STATS * stat_info_p)
{
  (void) thread_p;
  (void) stat_info_p;
  g_start_calls++;
  if (g_start_calls > 10)
    {
      /* bounded: the empty-index branch (end_scan, no page) */
      bts->end_scan = true;
      return NO_ERROR;
    }
  bts->C_page = fix_page (g_start_page);
  bts->C_vpid.volid = 0;
  bts->C_vpid.pageid = 100 + g_start_page;
  bts->slot_id = g_start_keys;
  return NO_ERROR;
}

int
btree_locate_key_from_saved_leaf (THREAD_ENTRY * thread_p, BTID_INT * btid_int, DB_VALUE * key,
				  const LOG_LSA * leaf_lsa, VPID * leaf_vpid, PAGE_PTR * leaf_page, INT16 * slot_id,
				  bool * leaf_unchanged, bool * found)
{
  (void) thread_p;
  (void) btid_int;
  (void) key;
  (void) leaf_lsa;
  g_resume_calls++;
  g_unfixes_before_resume.push_back (g_unfix_calls);
  if (fixed_total () != 0)
    {
      g_resume_with_page_fixed++;
    }
  *leaf_page = NULL;
  *leaf_unchanged = false;
  *found = false;
  if (!VPID_ISNULL (leaf_vpid))
    {
      /* saved leaf unchanged: stay on it, slot as saved */
      *leaf_page = fix_page (leaf_vpid->pageid - 100);
      *leaf_unchanged = true;
      return NO_ERROR;
    }
  /* looked up from the root */
  *leaf_page = fix_page (g_relocate_page);
  leaf_vpid->volid = 0;
  leaf_vpid->pageid = 100 + g_relocate_page;
  *slot_id = g_relocate_slot;
  *found = true;
  return NO_ERROR;
}

int
btree_range_scan_advance_over_filtered_keys (THREAD_ENTRY * thread_p, BTREE_SCAN * bts)
{
  (void) thread_p;
  g_advance_calls++;
  if (bts->C_page == NULL)
    {
      g_advance_without_page++;
      return ER_FAILED;
    }
  step s = g_adv_i < g_adv.size ()? g_adv[g_adv_i] : step { OK, bts->slot_id - 1 };
  g_adv_i++;
  switch (s.kind)
    {
    case OK:
      position (bts, s.slot);
      break;
    case RESTART:
      bts->slot_id = 0;
      pgbuf_unfix (thread_p, bts->C_page);
      bts->C_page = NULL;
      bts->force_restart_from_root = true;
      break;
    case RESTART_HELD:
      bts->slot_id = 0;
      bts->force_restart_from_root = true;
      break;
    case END:
      bts->end_scan = true;
      break;
    }
  return NO_ERROR;
}

int
btree_record_get_num_oids (THREAD_ENTRY * thread_p, BTID_INT * btid_int, RECDES * rec, int after_key_offset,
			   BTREE_NODE_TYPE node_type)
{
  (void) thread_p;
  (void) btid_int;
  (void) rec;
  (void) after_key_offset;
  (void) node_type;
  if (fixed_total () == 0)
    {
      g_key_read_without_page++;
    }
  return 1;
}

static BTREE_SCAN *g_bts;	/* the scan under test, for the record-read fakes to see its slot */

BTREE_SEARCH
btree_key_find_first_visible_row (THREAD_ENTRY * thread_p, BTID_INT * btid_int, RECDES * rec, int offset,
				  BTREE_NODE_TYPE node_type, OID * oid, OID * class_oid, int max_oids)
{
  (void) thread_p;
  (void) btid_int;
  (void) rec;
  (void) offset;
  (void) node_type;
  (void) max_oids;
  if (g_bts != NULL && g_bts->slot_id == 0)
    {
      g_slot0_key_reads++;
      return BTREE_KEY_NOTFOUND;
    }
  oid->volid = 0;
  oid->pageid = 500;
  oid->slotid = g_bts != NULL ? g_bts->slot_id : 1;
  OID_SET_NULL (class_oid);
  return BTREE_KEY_FOUND;
}

BTREE_SEARCH
btree_key_find_first_visible_row_from_all_ovf (THREAD_ENTRY * thread_p, BTID_INT * btid_int, VPID * first_ovfl_vpid,
					       OID * oid, OID * class_oid)
{
  (void) thread_p;
  (void) btid_int;
  (void) first_ovfl_vpid;
  (void) class_oid;
  OID_SET_NULL (oid);
  return BTREE_KEY_NOTFOUND;
}

bool
btree_leaf_is_flaged (RECDES * recp, short record_flag)
{
  (void) recp;
  (void) record_flag;
  return false;
}

/* ---- harness ---- */
struct row
{
  SCAN_CODE code;
  int pageid, slotid, key;
  bool oid_null;
};

static DB_VALUE g_vals[BTREE_KEY_INFO_COUNT];
static DB_VALUE *g_key_info[BTREE_KEY_INFO_COUNT];

struct scan
{
  BTREE_SCAN *bts;
  INDX_INFO indx;
  INDX_SCAN_ID isid;
  BTID btid;
  OID cls;
};

static scan *
mk_scan ()
{
  scan *s = (scan *) calloc (1, sizeof (scan));
  s->bts = (BTREE_SCAN *) calloc (1, sizeof (BTREE_SCAN));
  BTREE_SCAN *bts = s->bts;
  VPID_SET_NULL (&bts->C_vpid);
  VPID_SET_NULL (&bts->P_vpid);
  bts->C_page = NULL;
  bts->P_page = NULL;
  bts->is_cur_key_compressed = false;
  bts->common_prefix_size = COMMON_PREFIX_UNKNOWN;
  db_make_null (&bts->cur_key);
  bts->clear_cur_key = false;
  VPID_SET_NULL (&bts->leaf_rec_info.ovfl);
  LSA_SET_NULL (&bts->cur_leaf_lsa);
  s->indx.use_desc_index = 1;
  s->isid.indx_info = &s->indx;
  g_bts = bts;
  return s;
}

static void
reset (std::vector<step> adv, int start_page, int start_keys, int relocate_page, int relocate_slot)
{
  memset (g_fixed, 0, sizeof (g_fixed));
  memset (g_lsa, 0, sizeof (g_lsa));
  g_unfix_calls = g_latch_errors = 0;
  g_adv = adv;
  g_adv_i = 0;
  g_start_page = start_page;
  g_start_keys = start_keys;
  g_relocate_page = relocate_page;
  g_relocate_slot = relocate_slot;
  g_prepare_calls = g_start_calls = g_resume_calls = g_advance_calls = 0;
  g_resume_with_page_fixed = g_advance_without_page = g_key_read_without_page = g_slot0_key_reads = 0;
  g_unfixes_before_resume.clear ();
  for (int i = 0; i < BTREE_KEY_INFO_COUNT; i++)
    {
      db_make_null (&g_vals[i]);
      g_key_info[i] = &g_vals[i];
    }
}

static row
next (scan * s)
{
  row r;
  r.code = btree_get_next_key_info (NULL, &s->btid, s->bts, 1, &s->cls, &s->isid, g_key_info);
  r.pageid = db_get_int (g_key_info[BTREE_KEY_INFO_PAGEID]);
  r.slotid = db_get_int (g_key_info[BTREE_KEY_INFO_SLOTID]);
  r.key = DB_IS_NULL (g_key_info[BTREE_KEY_INFO_KEY]) ? -1 : db_get_int (g_key_info[BTREE_KEY_INFO_KEY]);
  r.oid_null = OID_ISNULL (db_get_oid (g_key_info[BTREE_KEY_INFO_FIRST_OID]));
  return r;
}

TEST_CASE ("R3 no restart request: one start, then one resume per call, rows are the keys positioned on", "[contract]")
{
  /* leaf 1 has 9 keys; desc: slot 9, then 8 */
  reset ({{OK, 9}, {OK, 8}}, 1, 9, 2, 0);
  scan *s = mk_scan ();
  row r1 = next (s);
  CHECK (r1.code == S_SUCCESS);
  CHECK (r1.pageid == 101);
  CHECK (r1.slotid == 9);
  CHECK (r1.key == key_value_for (1, 9));
  CHECK_FALSE (r1.oid_null);
  CHECK (fixed_total () == 0);
  row r2 = next (s);
  CHECK (r2.code == S_SUCCESS);
  CHECK (r2.slotid == 8);
  CHECK (r2.key == key_value_for (1, 8));
  CHECK (g_prepare_calls == 1);
  CHECK (g_start_calls == 1);
  CHECK (g_resume_calls == 1);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
  CHECK (g_key_read_without_page == 0);
  CHECK (g_slot0_key_reads == 0);
}

TEST_CASE ("R1 a resume returns with a restart and its leaf let go: the scan resumes again from the root and the row is the key it lands on, not slot 0", "[contract]")
{
  /* call 1: start on leaf 1 slot 1 (its last key in desc order). call 2: the resume steps to the previous leaf, which
   * is deallocated -> restart, C_page NULL; the next resume finds the key on leaf 3 and advances to slot 5. */
  reset ({{OK, 1}, {RESTART, 0}, {OK, 5}}, 1, 1, 3, 6);
  scan *s = mk_scan ();
  row r1 = next (s);
  REQUIRE (r1.code == S_SUCCESS);
  CHECK (r1.slotid == 1);
  row r2 = next (s);
  CHECK (r2.code == S_SUCCESS);
  CHECK (g_resume_calls == 2);
  CHECK (r2.pageid == 103);
  CHECK (r2.slotid == 5);
  CHECK (r2.key == key_value_for (3, 5));
  CHECK (r2.slotid != 0);
  CHECK_FALSE (r2.oid_null);
  CHECK (g_key_read_without_page == 0);
  CHECK (g_slot0_key_reads == 0);
  CHECK (g_advance_without_page == 0);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
  CHECK_FALSE (s->bts->force_restart_from_root);
}

TEST_CASE ("R1b two restarts in a row on the resume path: three resumes, then the row", "[contract]")
{
  reset ({{OK, 1}, {RESTART, 0}, {RESTART, 0}, {OK, 4}}, 1, 1, 3, 5);
  scan *s = mk_scan ();
  REQUIRE (next (s).code == S_SUCCESS);
  row r2 = next (s);
  CHECK (r2.code == S_SUCCESS);
  CHECK (g_resume_calls == 3);
  CHECK (r2.pageid == 103);
  CHECK (r2.slotid == 4);
  CHECK (g_key_read_without_page == 0);
  CHECK (g_slot0_key_reads == 0);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
}

TEST_CASE ("R2 the first start returns with a restart (C_vpid NULL, not started): the scan starts again, btree_prepare_bts once, the row is the key (needs PR#8363's clear on entry)", "[contract]")
{
  /* start 1: rightmost leaf 2 (3 keys), the advance steps to the previous leaf, deallocated -> restart.
   * start 2: positions on slot 3. */
  reset ({{RESTART, 0}, {OK, 3}}, 2, 3, 4, 0);
  scan *s = mk_scan ();
  row r1 = next (s);
  CHECK (r1.code == S_SUCCESS);
  CHECK (g_prepare_calls == 1);
  CHECK (g_start_calls == 2);
  CHECK (g_resume_calls == 0);
  CHECK (r1.pageid == 102);
  CHECK (r1.slotid == 3);
  CHECK (r1.key == key_value_for (2, 3));
  CHECK (g_key_read_without_page == 0);
  CHECK (g_slot0_key_reads == 0);
  CHECK (s->bts->is_scan_started);
  CHECK_FALSE (s->bts->force_restart_from_root);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
}

TEST_CASE ("R4 a restart raised with the leaf still fixed: it is unfixed once before the resume is called again, latches balance", "[contract]")
{
  reset ({{OK, 1}, {RESTART_HELD, 0}, {OK, 5}}, 1, 1, 3, 6);
  scan *s = mk_scan ();
  REQUIRE (next (s).code == S_SUCCESS);
  int unfix_before = g_unfix_calls;
  row r2 = next (s);
  CHECK (r2.code == S_SUCCESS);
  CHECK (g_resume_calls == 2);
  CHECK (g_resume_with_page_fixed == 0);
  CHECK (g_unfixes_before_resume.size () >= 2);
  if (g_unfixes_before_resume.size () >= 2)
    {
      /* between the first resume's entry and the second's: exactly the held leaf's unfix */
      CHECK (g_unfixes_before_resume[1] - g_unfixes_before_resume[0] == 1);
    }
  CHECK (r2.pageid == 103);
  CHECK (r2.slotid == 5);
  CHECK (g_slot0_key_reads == 0);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
  CHECK (g_unfix_calls - unfix_before == 2);	/* the held leaf, then the relocated leaf at the end: label */
}

TEST_CASE ("R5 the resume called again ends the scan: S_END, latches balance, C_vpid reset", "[contract]")
{
  reset ({{OK, 1}, {RESTART, 0}, {END, 0}}, 1, 1, 3, 1);
  scan *s = mk_scan ();
  REQUIRE (next (s).code == S_SUCCESS);
  row r2 = next (s);
  CHECK (r2.code == S_END);
  CHECK (g_resume_calls == 2);
  CHECK (g_key_read_without_page == 0);
  CHECK (g_slot0_key_reads == 0);
  CHECK (fixed_total () == 0);
  CHECK (g_latch_errors == 0);
  CHECK (VPID_ISNULL (&s->bts->C_vpid));
}
