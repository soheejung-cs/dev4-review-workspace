/*
 * PR#8196 (CBRD-27361) -- the hash merge of a GROUP BY's leftovers, src/query/query_executor.c (server), group K
 * review (head 2381ce437).
 *
 * qexec_gby_hash_merge () folds the unsorted list and the partial list by key-hash partitions.  When the order of the
 * groups reaches the result (agg_hash_respect_order = yes, the default, and the block is not a plain DISTINCT) the
 * job's two sort lists are opened before the first partition (qexec_gby_merge_job_sort_lists ()); the merge hands them
 * to rows_out / partials_out only after every partition went through, and end: frees only those two.  An error before
 * that -- a split whose next page fails because the query was interrupted, a partition's fold error -- leaves the two
 * lists with nobody to free them: their QFILE_LIST_IDs (malloc) leak and m_qlist_count stays one up each, which
 * qexec_execute_query () asserts on at its end (debug).  K-01.
 *
 * qexec_gby_merge_sample_groups () reads up to 65,536 rows of the unsorted list; its loop stops on anything but
 * S_SUCCESS, so a read error (S_ERROR) ends the sample as the list's end would: NO_ERROR, and the truncated sample's
 * distinct count taken for the whole list.  K-02.
 *
 * Fakes (test.cpp): qfile_open_list () callocs a list id and counts it live, qfile_free_list_id () takes a live one
 * back (a list id it did not hand out is only noted), qfile_destroy_list () / qfile_close_list () do nothing to the
 * count (the real destroy clears the id; the free that follows is what returns it), qfile_open_list_scan () /
 * qfile_scan_list_next () walk a list's tuple_cnt fake tuples (k_scan_limit cuts it, then k_scan_end), the tuple a
 * 16-byte record whose length word is set, qfile_add_tuple_to_list () counts a tuple into the list and fails at call
 * k_add_fail_at with ER_INTERRUPTED, fetch_val_list () is NO_ERROR, qdata_hash_agg_hkey () hashes the fake tuple's
 * ordinal into one of k_groups keys, qdata_agg_hkey_eq () is true (the hash decides: one key per hash),
 * qdata_key_sketch_estimate () = k_groups, mht_count () = 0, compute_parallel_degree () = 1 (no workers),
 * qdata_evaluate_aggregate_list () / qdata_save_agg_hentry_to_list () NO_ERROR, qfile_copy_list_id () a struct copy,
 * db_create_ostk_heap () / db_ostk_alloc () / db_destroy_ostk_heap () a malloc'd chunk list, db_private_*_release ()
 * malloc / free, er_set () / er_errid () / er_clear () record the code, prm_Def the two parameters the merge reads.
 * Real: everything of query_executor.c the merge reaches (sample, hash merge, part_of, open/free lists and parts,
 * split, partition, fold, rows, partials, mtable_*, collapse, arena wrappers, qexec_build_agg_hkey).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "memory_alloc.h"
#include "memory_hash.h"
#include "storage_common.h"
#include "query_list.h"
#include "list_file.h"
#include "fetch.h"
#include "query_aggregate.hpp"
#include "xasl_aggregate.hpp"
#include "px_parallel.hpp"

#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

/* ---------------------------------------------------------------- fakes */

static int g_errid = NO_ERROR;
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  g_errid = err_id;
}

int
er_errid (void)
{
  return g_errid;
}

void
er_clear (void)
{
  g_errid = NO_ERROR;
}

SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };
static UINT64 g_max_agg_hash_size = 32 * 1024;
static bool
boot_params ()
{
  prm_Def[PRM_ID_AGG_HASH_RESPECT_ORDER].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_AGG_HASH_RESPECT_ORDER].value.v.b = true;	/* the default */
  prm_Def[PRM_ID_MAX_AGG_HASH_SIZE].datatype = PRM_BIGINT;
  prm_Def[PRM_ID_MAX_AGG_HASH_SIZE].value.v.bi = g_max_agg_hash_size;
  prm_Def[PRM_ID_ORACLE_STYLE_EMPTY_STRING].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_STYLE_EMPTY_STRING].value.v.b = false;
  return true;
}
static const bool params_booted = boot_params ();

PGLENGTH db_User_page_size = 16384;
PGLENGTH db_Io_page_size = 16384;

void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  return malloc (size > 0 ? size : 1);
}

void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  free (ptr);
}

void *
db_private_realloc_release (THREAD_ENTRY * thrd, void *ptr, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  return realloc (ptr, size > 0 ? size : 1);
}

/* the obstack heap (memory_alloc.c is not linked): a list of malloc'd blocks, all freed at destroy */
struct k_ostk
{
  std::vector<void *> blocks;
};

HL_HEAPID
db_create_ostk_heap (int chunk_size)
{
  (void) chunk_size;
  return (HL_HEAPID) new k_ostk;
}

void
db_destroy_ostk_heap (HL_HEAPID heap_id)
{
  k_ostk *h = (k_ostk *) heap_id;
  for (void *p : h->blocks)
    {
      free (p);
    }
  delete h;
}

void *
db_ostk_alloc (HL_HEAPID heap_id, size_t size)
{
  void *p = calloc (1, size);
  ((k_ostk *) heap_id)->blocks.push_back (p);
  return p;
}

/* list ids handed out and taken back */
static std::set<const QFILE_LIST_ID *> g_live;
long k_opened = 0;
int k_open_fail_at = 0;

long
k_live_count (void)
{
  return (long) g_live.size ();
}

void
k_live_reset (void)
{
  g_live.clear ();
  k_opened = 0;
  k_open_fail_at = 0;
  g_errid = NO_ERROR;
}

int
k_is_live (const QFILE_LIST_ID * l)
{
  return g_live.count (l) ? 1 : 0;
}

QFILE_LIST_ID *
qfile_open_list (THREAD_ENTRY * thread_p, QFILE_TUPLE_VALUE_TYPE_LIST * type_list, SORT_LIST * sort_list,
		 QUERY_ID query_id, int flag, QFILE_LIST_ID * existing_list_id)
{
  (void) thread_p;
  (void) sort_list;
  (void) existing_list_id;
  if (k_open_fail_at > 0 && k_opened + 1 == k_open_fail_at)
    {
      k_opened++;
      g_errid = ER_OUT_OF_VIRTUAL_MEMORY;
      return NULL;
    }
  QFILE_LIST_ID *l = (QFILE_LIST_ID *) calloc (1, sizeof (QFILE_LIST_ID));
  l->type_list.type_cnt = type_list->type_cnt;
  l->type_list.hdr_size = (flag & QFILE_FLAG_BACKWARD) ? QFILE_TUPLE_HDR_SIZE_BACKWARD : QFILE_TUPLE_HDR_SIZE_FORWARD;
  l->query_id = query_id;
  g_live.insert (l);
  k_opened++;
  return l;
}

int
qfile_tpl_descr_alloc_values (QFILE_TUPLE_DESCRIPTOR * tuple_descr_p, int n)
{
  (void) tuple_descr_p;
  (void) n;
  return NO_ERROR;
}

void
qfile_close_list (THREAD_ENTRY * thread_p, QFILE_LIST_ID * list_id)
{
  (void) thread_p;
  (void) list_id;
}

void
qfile_destroy_list (THREAD_ENTRY * thread_p, QFILE_LIST_ID * list_id)
{
  (void) thread_p;
  (void) list_id;
}

void
qfile_free_list_id (QFILE_LIST_ID * list_id)
{
  if (g_live.erase (list_id))
    {
      free (list_id);
    }
}

int
qfile_copy_list_id (QFILE_LIST_ID * dest_list_id, const QFILE_LIST_ID * src_list_id, bool is_include_sort_list,
		    QFILE_DEPENDENT_MODE dep_mode)
{
  (void) is_include_sort_list;
  (void) dep_mode;
  memcpy ((void *) dest_list_id, (const void *) src_list_id, sizeof (QFILE_LIST_ID));
  return NO_ERROR;
}

int k_add_fail_at = 0;
int k_add_calls = 0;

int
qfile_add_tuple_to_list (THREAD_ENTRY * thread_p, QFILE_LIST_ID * list_id, QFILE_TUPLE tpl)
{
  (void) thread_p;
  (void) tpl;
  k_add_calls++;
  if (k_add_fail_at > 0 && k_add_calls == k_add_fail_at)
    {
      g_errid = ER_INTERRUPTED;	/* qfile_allocate_new_page (): the query was interrupted */
      return ER_FAILED;
    }
  list_id->tuple_cnt++;
  return NO_ERROR;
}

/* scans: a list's tuple_cnt fake tuples; each tuple a 16-byte record, its first word the length */
int k_scan_end = S_END;
int k_scan_limit = -1;
static long g_ordinal = 0;	/* the fake tuples' running number: what the fake key hash reads */
static long g_cur_ordinal = 0;
static char g_tuple[16];

int
qfile_open_list_scan (QFILE_LIST_ID * list_id, QFILE_LIST_SCAN_ID * s_id)
{
  memset ((void *) s_id, 0, sizeof (*s_id));
  s_id->list_id.tuple_cnt = list_id->tuple_cnt;
  s_id->curr_tplno = 0;
  return NO_ERROR;
}

SCAN_CODE
qfile_scan_list_next (THREAD_ENTRY * thread_p, QFILE_LIST_SCAN_ID * s_id, QFILE_TUPLE_RECORD * tplrec, int peek)
{
  (void) thread_p;
  (void) peek;
  if ((k_scan_limit >= 0 && s_id->curr_tplno >= k_scan_limit) || s_id->curr_tplno >= s_id->list_id.tuple_cnt)
    {
      return (SCAN_CODE) k_scan_end;
    }
  s_id->curr_tplno++;
  g_cur_ordinal = g_ordinal++;
  QFILE_PUT_TUPLE_LENGTH (g_tuple, (int) sizeof (g_tuple), false);
  tplrec->tpl = g_tuple;
  tplrec->size = 0;
  return S_SUCCESS;
}

void
qfile_close_scan (THREAD_ENTRY * thread_p, QFILE_LIST_SCAN_ID * s_id)
{
  (void) thread_p;
  (void) s_id;
}

int
fetch_val_list (THREAD_ENTRY * thread_p, regu_variable_list_node * regu_list, val_descr * vd, OID * class_oid,
		OID * obj_oid, QFILE_TUPLE_RECORD * tplrec, int peek)
{
  (void) thread_p;
  (void) regu_list;
  (void) vd;
  (void) class_oid;
  (void) obj_oid;
  (void) tplrec;
  (void) peek;
  return NO_ERROR;
}

double k_groups = 100;

unsigned int
qdata_hash_agg_hkey (const void *key, unsigned int ht_size)
{
  (void) key;
  (void) ht_size;
  /* the row's key: its ordinal modulo the group count, spread (one hash per key) */
  unsigned int g = (unsigned int) (g_cur_ordinal % (long) (k_groups > 1 ? k_groups : 1));
  return g * 2654435761u + 0x9e3779b9u;
}

int
qdata_agg_hkey_eq (const void *key1, const void *key2)
{
  (void) key1;
  (void) key2;
  return 1;			/* the hash decides: one key per hash in these scenarios */
}

double
qdata_key_sketch_estimate (const unsigned char *regs)
{
  (void) regs;
  return k_groups;
}

unsigned int
mht_count (const MHT_TABLE * ht)
{
  (void) ht;
  return 0;
}

namespace parallel_query
{
  UINT32 compute_parallel_degree (parallel_type type, UINT64 num_pages, int hint_degree) noexcept
  {
    (void) type;
    (void) num_pages;
    (void) hint_degree;
    return 1;
  }
}

int
qdata_evaluate_aggregate_list (cubthread::entry * thread_p, cubxasl::aggregate_list_node * agg_list, val_descr * vd,
			       cubxasl::aggregate_accumulator * alt_acc_list, bool use_desc_index)
{
  (void) thread_p;
  (void) agg_list;
  (void) vd;
  (void) alt_acc_list;
  (void) use_desc_index;
  return NO_ERROR;
}

int
qdata_save_agg_hentry_to_list (cubthread::entry * thread_p, cubquery::aggregate_hash_key * key,
			       cubquery::aggregate_hash_value * value, DB_VALUE * temp_dbval_array,
			       qfile_list_id * list_id)
{
  (void) thread_p;
  (void) key;
  (void) value;
  (void) temp_dbval_array;
  list_id->tuple_cnt++;
  return NO_ERROR;
}

/* ---------------------------------------------------------------- hooks (hooks.cpp, in the TU) */

int k_sample_scenario (int rows, int end_code, int *rc, double *groups);
int k_hash_merge_scenario (int rows, int pages, double groups, int add_fail_at, int *rc, int *result, long *opened,
			   long *live, int *part_list_live);
int k_open_parts_scenario (int n, int *rc, long *opened, long *live);

/* ---------------------------------------------------------------- cases */

TEST_CASE ("sample: a list read to its end -- the distinct keys of the whole list, NO_ERROR", "[contract]")
{
  int rc = -1;
  double groups = 0;

  k_scan_limit = -1;
  REQUIRE (k_sample_scenario (10, S_END, &rc, &groups) == 0);
  CHECK (rc == NO_ERROR);
  CHECK (groups == 10.0);
}

TEST_CASE ("K-02 defect: a read error (S_ERROR) after 10 rows ends the sample as the list's end -- NO_ERROR, and the 10 rows' distinct keys taken for the whole list (PASS = defect present)", "[defect]")
{
  int rc = -1;
  double groups = 0;

  REQUIRE (k_sample_scenario (10, S_ERROR, &rc, &groups) == 0);
  CHECK (rc == NO_ERROR);
  CHECK (groups == 10.0);
}

TEST_CASE ("open_parts: the k-th list failing to open, free_parts gives back every list that opened", "[contract]")
{
  int rc = 0;
  long opened = 0, live = -1;

  for (int fail_at = 1; fail_at <= 8; fail_at++)
    {
      k_live_reset ();
      k_open_fail_at = fail_at;
      /* k_open_parts_scenario () resets the count; set the failure after its reset through the global */
      REQUIRE (k_open_parts_scenario (4, &rc, &opened, &live) == 0);
      CHECK (live == 0);
    }
}

TEST_CASE ("hash merge, order reaching the result (collapse): a run without error leaves only what it hands over -- ctx->part_list_id, the collapsed partials", "[contract]")
{
  int rc = -1, result = -1, part_list_live = -1;
  long opened = 0, live = -1;

  /* 3,000 rows of 300 keys (fold 10 >= 3), 40 pages: several partitions at a 32 KB budget */
  REQUIRE (k_hash_merge_scenario (3000, 40, 300, 0, &rc, &result, &opened, &live, &part_list_live) == 0);
  CHECK (rc == NO_ERROR);
  CHECK (result == 2);		/* QEXEC_GBY_MERGE_COLLAPSED */
  CHECK (opened > 2);		/* the two sort lists and the partitions' */
  CHECK (live == 1);
  CHECK (part_list_live == 1);
}

TEST_CASE ("K-01 defect: the split's 3rd tuple write fails (interrupt) -- the job's two sort lists, opened before the first partition, are never freed (PASS = defect present)", "[defect]")
{
  int rc = -1, result = -1, part_list_live = -1;
  long opened = 0, live = -1;

  REQUIRE (k_hash_merge_scenario (3000, 40, 300, 3, &rc, &result, &opened, &live, &part_list_live) == 0);
  CHECK (rc != NO_ERROR);
  CHECK (er_errid () == ER_INTERRUPTED);
  CHECK (opened > 2);
  CHECK (live == 2);
}

TEST_CASE ("K-01 defect: a later partition's write fails -- the same two lists are left (PASS = defect present)", "[defect]")
{
  int rc = -1, result = -1, part_list_live = -1;
  long opened = 0, live = -1;

  /* past the split's 3,000 writes: the collapse writes of the first partitions, or the second level's */
  REQUIRE (k_hash_merge_scenario (3000, 40, 300, 3005, &rc, &result, &opened, &live, &part_list_live) == 0);
  CHECK (rc != NO_ERROR);
  CHECK (live == 2);
}
