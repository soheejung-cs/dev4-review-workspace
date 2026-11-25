/* PR#8718 (CBRD-28283) a4a513237 -- qexec_open_scan (), src/query/query_executor.c: which pass of an index MIN/MAX
 * scan is marked to read its key ranges from the last (INDX_SCAN_ID::min_max_only_desc_pass).
 *
 * The MIN/MAX aggregates take MAX from the first row of a pass when use_desc_index != part_key_descending
 * (qdata_evaluate_aggregate_list (), the PT_MAX branch), MIN when they are equal: part_key_descending is the index
 * column's own order (XASL generation: the index scan's sort list), so the pass that reads the column descending --
 * a descending scan of an ascending column, or an ascending scan of a descending one -- takes MAX.  scan_get_index_oidset ()
 * keeps the key ranges in the values' ascending order (scan_key_compare () does not turn a descending column around),
 * so that pass has to read them from the last.  qexec_open_scan () marks it after scan_open_index_scan (), which
 * leaves the mark false, and before the parallel open, which copies the scan id.
 *
 * The real code under test: qexec_open_scan () (static: unstatic'd by the seam), qexec_set_expr_share_spec (),
 * qexec_spec_is_for_update ().
 *
 * Fakes:
 *  - scan_open_index_scan (): what qexec_open_scan () reads of the real one's result -- type S_INDX_SCAN, the mark
 *    false (scan_manager.c, "isidp->min_max_only_desc_pass = false") -- and NO_ERROR; records the arguments.
 *  - scan_open_parallel_index_scan (): records the mark it is handed, leaves the scan serial (S_INDX_SCAN, the
 *    fallback qexec_open_scan () handles), NO_ERROR.
 *  - mvcc_is_mvcc_disabled_class (): false (a user class).
 * ACCESS_SPEC_TYPE / XASL_NODE / INDX_INFO / AGGREGATE_TYPE / QFILE_LIST_ID calloc'd, never constructed: TARGET_CLASS,
 * ACCESS_METHOD_INDEX, not partitioned, no predicate, no FOR UPDATE, not cached; a BUILDVALUE block whose agg_list
 * carries part_key_descending; the list file without a temp file (no reopen).
 *
 * min_max_only_desc_pass is new in PR#8718: read where the member exists (SFINAE), false where it does not (base).
 */
#include "catch.hpp"

#include <cstdlib>
#include <cstring>

#include "query_executor.h"
#include "xasl.h"
#include "xasl_aggregate.hpp"
#include "scan_manager.h"
#include "px_scan.hpp"
#include "mvcc.h"
#include "thread_entry.hpp"

/* static in query_executor.c: unstatic'd by the seam (the real code runs) */
extern int qexec_open_scan (THREAD_ENTRY * thread_p, ACCESS_SPEC_TYPE * curr_spec, VAL_LIST * val_list,
			    VAL_DESCR * vd, bool force_select_lock, int fixed, int grouped, bool iscan_oid_order,
			    SCAN_ID * s_id, QUERY_ID query_id, SCAN_OPERATION_TYPE scan_op_type,
			    bool scan_immediately_stop, bool * p_mvcc_select_lock_needed, XASL_NODE * xasl,
			    bool stmt_scoped_locks);

namespace mm
{
  cubthread::entry *thr;
  ACCESS_SPEC_TYPE *spec;
  XASL_NODE *xasl;
  INDX_INFO *indx;
  AGGREGATE_TYPE *agg;
  QFILE_LIST_ID *list_id;

  int opens;			/* scan_open_index_scan () calls */
  bool open_min_max_arg;	/* its min_max_optimzied_scan */
  int parallel_opens;
  bool mark_at_parallel_open;

  template < typename T > auto get_impl (const T * isid, int)->decltype (isid->min_max_only_desc_pass, bool ())
  {
    return isid->min_max_only_desc_pass;
  }
  template < typename T > bool get_impl (const T *, long)
  {
    return false;
  }
  bool mark (const INDX_SCAN_ID * isid)
  {
    return get_impl (isid, 0);
  }

  template < typename T > auto set_impl (T * isid, bool v, int)->decltype (isid->min_max_only_desc_pass = v, bool ())
  {
    isid->min_max_only_desc_pass = v;
    return true;
  }
  template < typename T > bool set_impl (T *, bool, long)
  {
    return false;
  }
  void set_mark (INDX_SCAN_ID * isid, bool v)
  {
    (void) set_impl (isid, v, 0);
  }

  /* an index spec of a BUILDVALUE block (MIN/MAX only when min_max), scanned with use_desc_index over a column whose
   * own order is part_key_descending */
  void setup (bool min_max, bool use_desc_index, bool part_key_descending)
  {
    if (thr == nullptr)
      {
	thr = (cubthread::entry *) calloc (1, sizeof (cubthread::entry));
	spec = (ACCESS_SPEC_TYPE *) calloc (1, sizeof (ACCESS_SPEC_TYPE));
	xasl = (XASL_NODE *) calloc (1, sizeof (XASL_NODE));
	indx = (INDX_INFO *) calloc (1, sizeof (INDX_INFO));
	agg = (AGGREGATE_TYPE *) calloc (1, sizeof (AGGREGATE_TYPE));
	list_id = (QFILE_LIST_ID *) calloc (1, sizeof (QFILE_LIST_ID));
      }
    memset ((void *) spec, 0, sizeof (ACCESS_SPEC_TYPE));
    memset ((void *) xasl, 0, sizeof (XASL_NODE));
    memset ((void *) indx, 0, sizeof (INDX_INFO));
    memset ((void *) agg, 0, sizeof (AGGREGATE_TYPE));
    memset ((void *) list_id, 0, sizeof (QFILE_LIST_ID));
    opens = 0;
    parallel_opens = 0;
    open_min_max_arg = false;
    mark_at_parallel_open = false;

    indx->range_type = R_KEYLIST;
    indx->use_desc_index = use_desc_index ? 1 : 0;

    spec->type = TARGET_CLASS;
    spec->access = ACCESS_METHOD_INDEX;
    spec->indexptr = indx;
    spec->pruning_type = DB_NOT_PARTITIONED_CLASS;
    spec->s.cls_node.cls_oid.volid = 0;
    spec->s.cls_node.cls_oid.pageid = 1;
    spec->s.cls_node.cls_oid.slotid = 1;
    if (min_max)
      {
	ACCESS_SPEC_SET_FLAG (spec, ACCESS_SPEC_FLAG_ONLY_MIN_MAX_SCAN);
      }

    agg->flag.min_max_optimized = min_max;
    agg->flag.part_key_descending = part_key_descending;

    xasl->type = BUILDVALUE_PROC;
    xasl->spec_list = spec;
    xasl->list_id = list_id;
    xasl->proc.buildvalue.agg_list = agg;
  }

  int open ()
  {
    bool lock_needed = false;
    return qexec_open_scan (thr, spec, NULL, NULL, false, 0, 0, false, &spec->s_id, 0, S_SELECT, false,
			    &lock_needed, xasl, false);
  }

  /* the open of a MIN/MAX spec: the mark the parallel open saw and the one the scan keeps */
  bool marked (bool use_desc_index, bool part_key_descending)
  {
    setup (true, use_desc_index, part_key_descending);
    REQUIRE (open () == NO_ERROR);
    REQUIRE (opens == 1);
    REQUIRE (parallel_opens == 1);
    CHECK (mark_at_parallel_open == mark (&spec->s_id.s.isid));
    return mark (&spec->s_id.s.isid);
  }
}

/* ---- fakes ---- */

int
scan_open_index_scan (THREAD_ENTRY * thread_p, SCAN_ID * scan_id, bool mvcc_select_lock_needed,
		      bool upddel_target_scan, bool click_counter_scan, SCAN_OPERATION_TYPE scan_op_type, int fixed,
		      int grouped, QPROC_SINGLE_FETCH single_fetch, DB_VALUE * join_dbval, val_list_node * val_list,
		      val_descr * vd, indx_info * indx_info, OID * cls_oid, HFID * hfid,
		      regu_variable_list_node * regu_list_key, PRED_EXPR * pr_key, regu_variable_list_node * regu_list_pred,
		      PRED_EXPR * pr, regu_variable_list_node * regu_list_rest, PRED_EXPR * pr_range,
		      regu_variable_list_node * regu_list_range, valptr_list_node * output_val_list,
		      regu_variable_list_node * regu_val_list, int num_attrs_key, ATTR_ID * attrids_key,
		      HEAP_CACHE_ATTRINFO * cache_key, int num_attrs_pred, ATTR_ID * attrids_pred,
		      HEAP_CACHE_ATTRINFO * cache_pred, int num_attrs_rest, ATTR_ID * attrids_rest,
		      HEAP_CACHE_ATTRINFO * cache_rest, int num_attrs_range, ATTR_ID * attrids_range,
		      HEAP_CACHE_ATTRINFO * cache_range, bool iscan_oid_order, QUERY_ID query_id,
		      bool min_max_optimzied_scan)
{
  scan_id->type = S_INDX_SCAN;
  scan_id->status = S_OPENED;
  mm::set_mark (&scan_id->s.isid, false);	/* the real open leaves it false */
  mm::opens++;
  mm::open_min_max_arg = min_max_optimzied_scan;
  return NO_ERROR;
}

int
scan_open_parallel_index_scan (THREAD_ENTRY * thread_p, SCAN_ID * scan_id, VAL_DESCR * vd, ACCESS_SPEC_TYPE * spec,
			       OID * class_oid, HFID * class_hfid, XASL_NODE * xasl, QUERY_ID query_id)
{
  mm::parallel_opens++;
  mm::mark_at_parallel_open = mm::mark (&scan_id->s.isid);
  return NO_ERROR;		/* stays S_INDX_SCAN: the serial fallback */
}

bool
mvcc_is_mvcc_disabled_class (const OID * class_oid)
{
  (void) class_oid;
  return false;
}

/* ---- cases ---- */

TEST_CASE ("P1 a MIN/MAX only index scan is marked exactly when its pass reads the column descending "
	   "(use_desc_index != part_key_descending), the pass that takes MAX: ascending index (0,0) no -- MIN first; "
	   "USE_DESC_IDX on it (1,0) yes; descending index read forward (0,1) yes; USE_DESC_IDX on it (1,1) no; the "
	   "parallel open sees the same mark (base: no mark)", "[contract]")
{
  CHECK_FALSE (mm::marked (false, false));
  CHECK (mm::open_min_max_arg);
  CHECK (mm::marked (true, false));
  CHECK (mm::marked (false, true));
  CHECK_FALSE (mm::marked (true, true));
}

TEST_CASE ("P2 boundary: a spec that is no MIN/MAX only scan is never marked, whatever the directions, and its "
	   "block's agg_list is not read -- a BUILDLIST block has none", "[contract]")
{
  for (int d = 0; d < 4; d++)
    {
      mm::setup (false, (d & 1) != 0, (d & 2) != 0);
      mm::xasl->type = BUILDLIST_PROC;
      mm::xasl->proc.buildvalue.agg_list = NULL;
      mm::set_mark (&mm::spec->s_id.s.isid, true);	/* a mark left from before */
      REQUIRE (mm::open () == NO_ERROR);
      CHECK_FALSE (mm::mark (&mm::spec->s_id.s.isid));
      CHECK_FALSE (mm::mark_at_parallel_open);
      CHECK_FALSE (mm::open_min_max_arg);
    }
}

TEST_CASE ("P3 the mark is decided at each open from the directions of that open: a stale mark of the scan id "
	   "(the XASL clone's spec keeps it between executions) is replaced, and a use_desc_index left turned around "
	   "gives the mark of its own direction", "[contract]")
{
  mm::setup (true, false, false);
  mm::set_mark (&mm::spec->s_id.s.isid, true);
  REQUIRE (mm::open () == NO_ERROR);
  CHECK_FALSE (mm::mark (&mm::spec->s_id.s.isid));

  mm::indx->use_desc_index = 1;	/* left turned around */
  REQUIRE (mm::open () == NO_ERROR);
  CHECK (mm::mark (&mm::spec->s_id.s.isid));
}
