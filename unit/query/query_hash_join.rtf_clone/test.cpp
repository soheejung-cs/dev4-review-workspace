/* PR#8196 (CBRD-27361) G3G4-01 -- hash join runtime filter on a parallel worker's XASL clone,
 * src/query/query_hash_join.c (server mode).
 *
 * The leader's hjoin_rtf_build () puts the filter on the probe input's outermost scan node (target->rtf); a
 * parallel scan worker's initialize () binds the same filter on its clone of that node (hjoin_rtf_bind_clone,
 * px_scan_task.cpp:122); when the join's two inputs are lists, hjoin_rtf_release () frees the filter and resets
 * the leader tree's two input nodes only (query_hash_join.c:5043-5052). The worker's finalize () clears and
 * retires its clone to the xcache clone pool without touching rtf.
 * Claim under test: the retired clone keeps the freed pointer; when the same plan's next execution gets that
 * clone (a leader whose own clone is still out: another session on the same prepared statement, or two open
 * cursors) and hjoin_rtf_build () declines (first input > HJOIN_RTF_MAX_KEYS rows, calloc failure), the probe
 * node's rtf is never reset, so qexec_intprt_fnc () 12821 / row_body () 967 call hjoin_rtf_pass () on freed memory.
 *
 * Fakes: none for the functions under test -- hjoin_rtf_first_input / build (its decline branch) / pass /
 * bind_clone / release are the TU's own. hooks.cpp builds the filter the build's success path would leave
 * (the list scan it reads is a tuple layout, not reproduced). The XASL trees are calloc'd and shaped as
 * stream_to_xasl leaves them (status XASL_BUILD, rtf NULL); the probe node is one TARGET_CLASS spec whose
 * rest regu list fetches its key column into the value list's second cell, as hjoin_rtf_target_cell () wants.
 * The worker's finalize () is modelled by worker_retire (): hjoin_rtf_unbind_clone () (the fix, be6c64d40) and
 * status XASL_CLEARED (qexec_clear_xasl).  Since c503fcaa2's successor the filter counts nothing itself: the caller
 * of hjoin_rtf_pass () counts on its XASL node (rtf_rows / rtf_dropped), unbind merges a clone's into the node it
 * was bound from, and hjoin_rtf_release () copies the target node's into the join node for the trace. */
#include "catch.hpp"
#include "query_hash_join.h"
#include "xasl.h"
#include "regu_var.hpp"
#include "query_list.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "dbtype.h"

#include <cstring>
#include <cstdlib>
#include <vector>

HJOIN_RUNTIME_FILTER *unitseam_rtf_make (const INT64 * keys, int n, int val_index);
void unitseam_rtf_attach (XASL_NODE * hashjoin_xasl, XASL_NODE * target, HJOIN_RUNTIME_FILTER * rtf, DB_VALUE * cell);
INT64 unitseam_rtf_n_keys (const HJOIN_RUNTIME_FILTER * rtf);
INT64 unitseam_rtf_max_keys (void);

namespace
{
  /* an INTEGER domain without object_domain.c / object_primitive.c: the TU reads dom->type->id only */
  TP_DOMAIN *int_domain ()
  {
    static char type_buf[sizeof (PR_TYPE)];
    static TP_DOMAIN dom;
    static bool made = false;
    if (!made)
      {
	memset (type_buf, 0, sizeof (type_buf));
	reinterpret_cast<PR_TYPE *> (type_buf)->id = DB_TYPE_INTEGER;
	memset (&dom, 0, sizeof (dom));
	dom.type = reinterpret_cast<PR_TYPE *> (type_buf);
	made = true;
      }
    return &dom;
  }

  template <class T> T *zalloc ()
  {
    return (T *) calloc (1, sizeof (T));
  }

  /* the probe input: BUILDLIST over one class scan, two columns, the key in column 1 (= value list cell 1) */
  struct scan_node
  {
    XASL_NODE *xasl;
    DB_VALUE *cell[2];
  };

  scan_node make_probe ()
  {
    scan_node s;
    s.xasl = zalloc<XASL_NODE> ();
    s.xasl->type = BUILDLIST_PROC;
    s.xasl->val_list = zalloc<VAL_LIST> ();
    s.xasl->outptr_list = zalloc<OUTPTR_LIST> ();
    s.xasl->spec_list = zalloc<ACCESS_SPEC_TYPE> ();
    s.xasl->spec_list->type = TARGET_CLASS;
    QPROC_DB_VALUE_LIST *vtail = &s.xasl->val_list->valp;
    REGU_VARIABLE_LIST *otail = &s.xasl->outptr_list->valptrp;
    REGU_VARIABLE_LIST *rtail = &ACCESS_SPEC_CLS_SPEC (s.xasl->spec_list).cls_regu_list_rest;
    for (int i = 0; i < 2; i++)
      {
	s.cell[i] = zalloc<DB_VALUE> ();
	db_make_null (s.cell[i]);
	QPROC_DB_VALUE_LIST v = zalloc<qproc_db_value_list> ();
	v->val = s.cell[i];
	v->dom = int_domain ();
	*vtail = v;
	vtail = &v->next;
	s.xasl->val_list->val_cnt++;
	REGU_VARIABLE_LIST o = zalloc<regu_variable_list_node> ();
	o->value.type = TYPE_CONSTANT;
	o->value.value.dbvalptr = s.cell[i];
	o->value.domain = int_domain ();
	*otail = o;
	otail = &o->next;
	s.xasl->outptr_list->valptr_cnt++;
	REGU_VARIABLE_LIST r = zalloc<regu_variable_list_node> ();
	r->value.type = TYPE_ATTR_ID;
	r->value.vfetch_to = s.cell[i];
	r->value.domain = int_domain ();
	*rtail = r;
	rtail = &r->next;
      }
    return s;
  }

  /* the build input after it ran: a list of n rows, one INTEGER column */
  XASL_NODE *make_build (INT64 n_rows)
  {
    XASL_NODE *x = zalloc<XASL_NODE> ();
    x->type = BUILDLIST_PROC;
    x->list_id = zalloc<QFILE_LIST_ID> ();
    x->list_id->tuple_cnt = n_rows;
    x->list_id->type_list.type_cnt = 1;
    x->list_id->type_list.domp = (TP_DOMAIN **) calloc (1, sizeof (TP_DOMAIN *));
    x->list_id->type_list.domp[0] = int_domain ();
    return x;
  }

  /* INNER hash join, outer = probe (key column 1), inner = build (key column 0) */
  XASL_NODE *make_hj (XASL_NODE * outer, XASL_NODE * inner)
  {
    XASL_NODE *x = zalloc<XASL_NODE> ();
    x->type = HASHJOIN_PROC;
    HASHJOIN_PROC_NODE *hj = &x->proc.hashjoin;
    hj->outer.xasl = outer;
    hj->inner.xasl = inner;
    hj->merge_info.join_type = JOIN_INNER;
    hj->merge_info.ls_column_cnt = 1;
    hj->merge_info.ls_outer_column = (int *) calloc (1, sizeof (int));
    hj->merge_info.ls_outer_column[0] = 1;
    hj->merge_info.ls_inner_column = (int *) calloc (1, sizeof (int));
    hj->merge_info.ls_inner_column[0] = 0;
    return x;
  }

  /* what px_scan_task::finalize () leaves on the worker's clone before xcache_retire_clone (): the alias dropped and
   * the clone's counters merged into the leader's node (hjoin_rtf_unbind_clone ()), the subtree cleared (status
   * XASL_CLEARED) */
  void worker_retire (XASL_NODE * clone, XASL_NODE * orig)
  {
    hjoin_rtf_unbind_clone (clone, orig);
    clone->status = XASL_CLEARED;
  }

  /* what the row loop does with one row on its node (row_body () / qexec_intprt_fnc ()): count, look up, count */
  bool row_check (XASL_NODE * node, const DB_VALUE * key)
  {
    node->rtf_rows++;
    if (!hjoin_rtf_pass (node->rtf, key))
      {
	node->rtf_dropped++;
	return false;
      }
    return true;
  }

  DB_VALUE int_val (int v)
  {
    DB_VALUE d;
    db_make_int (&d, v);
    return d;
  }

  /* one execution on the leader tree with a worker clone of the probe: build (hand-made), bind, release, retire.
   * Returns the filter pointer the release freed. */
  struct scene
  {
    XASL_NODE *hj;
    scan_node probe, clone;
    HJOIN_RUNTIME_FILTER *rtf;
  };

  scene leader_run_with_worker (bool do_release = true)
  {
    scene s;
    static const INT64 keys[] = { 10, 20, 30 };
    s.probe = make_probe ();
    s.clone = make_probe ();
    s.hj = make_hj (s.probe.xasl, make_build (3));
    s.rtf = unitseam_rtf_make (keys, 3, 1);
    unitseam_rtf_attach (s.hj, s.probe.xasl, s.rtf, s.probe.cell[1]);
    hjoin_rtf_bind_clone (s.clone.xasl, s.probe.xasl);
    if (do_release)
      {
	hjoin_rtf_release (nullptr, s.hj);
	worker_retire (s.clone.xasl, s.probe.xasl);
      }
    return s;
  }
}

TEST_CASE ("hjoin_rtf_first_input: INNER join runs the build side first and targets the probe's scan node", "[contract]")
{
  scan_node probe = make_probe ();
  XASL_NODE *build = make_build (3);
  XASL_NODE *hj = make_hj (probe.xasl, build);
  XASL_NODE *target = nullptr;

  CHECK (hjoin_rtf_first_input (hj, &target) == build);
  CHECK (target == probe.xasl);
  CHECK (hj->proc.hashjoin.rtf_keys == -1);
}

TEST_CASE ("hjoin_rtf_build declines a first input above HJOIN_RTF_MAX_KEYS and leaves a fresh target's rtf NULL", "[contract]")
{
  scan_node probe = make_probe ();
  XASL_NODE *build = make_build (unitseam_rtf_max_keys () + 1);
  XASL_NODE *hj = make_hj (probe.xasl, build);
  XASL_NODE *target = nullptr;

  REQUIRE (hjoin_rtf_first_input (hj, &target) == build);
  CHECK (hjoin_rtf_build (nullptr, hj, build, target) == NO_ERROR);
  CHECK (probe.xasl->rtf == nullptr);
  CHECK (hj->proc.hashjoin.rtf == nullptr);
}

TEST_CASE ("hjoin_rtf_bind_clone: the worker clone shares the leader's filter on its own cell; hjoin_rtf_pass keeps build keys, drops the rest", "[contract]")
{
  scene s = leader_run_with_worker (false);

  CHECK (s.clone.xasl->rtf == s.rtf);
  CHECK (s.clone.xasl->rtf_val == s.clone.cell[1]);
  CHECK (s.clone.xasl->rtf_val != s.probe.cell[1]);
  CHECK (unitseam_rtf_n_keys (s.rtf) == 3);

  DB_VALUE k = int_val (20);
  CHECK (row_check (s.clone.xasl, &k));
  k = int_val (25);
  CHECK_FALSE (row_check (s.clone.xasl, &k));
  db_make_null (&k);
  CHECK_FALSE (row_check (s.clone.xasl, &k));
  /* the lookup counts nothing: the rows are on the clone's node, the leader's node and the join see none yet */
  CHECK (s.clone.xasl->rtf_rows == 3);
  CHECK (s.clone.xasl->rtf_dropped == 2);
  CHECK (s.probe.xasl->rtf_rows == 0);
  CHECK (s.hj->proc.hashjoin.rtf_rows == 0);

  worker_retire (s.clone.xasl, s.probe.xasl);
  CHECK (s.probe.xasl->rtf_rows == 3);
  CHECK (s.probe.xasl->rtf_dropped == 2);
  CHECK (s.clone.xasl->rtf_rows == 0);
  CHECK (s.clone.xasl->rtf_dropped == 0);
  hjoin_rtf_release (nullptr, s.hj);
  CHECK (s.hj->proc.hashjoin.rtf_rows == 3);
  CHECK (s.hj->proc.hashjoin.rtf_dropped == 2);
}

TEST_CASE ("hjoin_rtf_unbind_clone: two workers' counters (3/1, 5/2) merge into the leader's node beside its own row (9/3), which the release copies to the join", "[contract]")
{
  scene s = leader_run_with_worker (false);
  scan_node clone2 = make_probe ();

  hjoin_rtf_bind_clone (clone2.xasl, s.probe.xasl);
  REQUIRE (clone2.xasl->rtf == s.rtf);
  /* the leader's own row path counted 1 row before the workers finished */
  DB_VALUE k = int_val (10);
  CHECK (row_check (s.probe.xasl, &k));
  s.clone.xasl->rtf_rows = 3;
  s.clone.xasl->rtf_dropped = 1;
  clone2.xasl->rtf_rows = 5;
  clone2.xasl->rtf_dropped = 2;

  worker_retire (s.clone.xasl, s.probe.xasl);
  worker_retire (clone2.xasl, s.probe.xasl);
  CHECK (s.probe.xasl->rtf_rows == 9);
  CHECK (s.probe.xasl->rtf_dropped == 3);
  CHECK (s.clone.xasl->rtf == nullptr);
  CHECK (clone2.xasl->rtf == nullptr);
  CHECK (clone2.xasl->rtf_rows == 0);
  CHECK (clone2.xasl->rtf_dropped == 0);

  hjoin_rtf_release (nullptr, s.hj);
  CHECK (s.hj->proc.hashjoin.rtf_rows == 9);
  CHECK (s.hj->proc.hashjoin.rtf_dropped == 3);
  CHECK (s.hj->proc.hashjoin.rtf == nullptr);
  CHECK (s.probe.xasl->rtf == nullptr);
}

TEST_CASE ("hjoin_rtf_unbind_clone on a clone that was bound to no filter adds nothing", "[contract]")
{
  scan_node probe = make_probe (), clone = make_probe ();

  hjoin_rtf_bind_clone (clone.xasl, probe.xasl);	/* probe has no filter: the clone stays unbound */
  clone.xasl->rtf_rows = 7;	/* stale, as a retired clone could carry */
  worker_retire (clone.xasl, probe.xasl);
  CHECK (probe.xasl->rtf_rows == 0);
  CHECK (clone.xasl->rtf_rows == 0);
}

TEST_CASE ("hjoin_rtf_contains: a build key is found, a key no build row has is not (the batch leaf's lookup)", "[contract]")
{
  scene s = leader_run_with_worker (false);

  CHECK (unitseam_rtf_n_keys (s.rtf) == 3);
  CHECK (hjoin_rtf_contains (s.rtf, 10));
  CHECK (hjoin_rtf_contains (s.rtf, 20));
  CHECK (hjoin_rtf_contains (s.rtf, 30));
  CHECK_FALSE (hjoin_rtf_contains (s.rtf, 25));
  CHECK_FALSE (hjoin_rtf_contains (s.rtf, 0));
  CHECK_FALSE (hjoin_rtf_contains (s.rtf, -10));
  CHECK_FALSE (hjoin_rtf_is_date (s.rtf));
  /* the same answer the DB_VALUE lookup gives */
  DB_VALUE k = int_val (30);
  CHECK (hjoin_rtf_pass (s.rtf, &k));
  k = int_val (31);
  CHECK_FALSE (hjoin_rtf_pass (s.rtf, &k));
  hjoin_rtf_release (nullptr, s.hj);
}

TEST_CASE ("hjoin_rtf_release: the leader tree's probe node and the join forget the filter", "[contract]")
{
  scene s = leader_run_with_worker ();

  CHECK (s.probe.xasl->rtf == nullptr);
  CHECK (s.probe.xasl->rtf_val == nullptr);
  CHECK (s.hj->proc.hashjoin.rtf == nullptr);
}

TEST_CASE ("a worker clone retired to the pool carries no filter pointer after the leader's release (FAIL = defect)", "[contract]")
{
  scene s = leader_run_with_worker ();

  CHECK (s.clone.xasl->rtf == nullptr);
  CHECK (s.clone.xasl->rtf_val == nullptr);
}

TEST_CASE ("PR8196 G3G4-01: hjoin_rtf_release frees the filter but the worker clone keeps the freed pointer into the pool (PASS = defect present)", "[defect]")
{
  scene s = leader_run_with_worker ();

  CHECK (s.clone.xasl->rtf != nullptr);
  CHECK (s.clone.xasl->rtf == s.rtf);	/* the address hjoin_rtf_release () just free ()d */
  CHECK (s.clone.xasl->rtf_val == s.clone.cell[1]);
}

TEST_CASE ("PR8196 G3G4-01: next execution on the retired clone -- first_input targets it again, build declines (> HJOIN_RTF_MAX_KEYS), the dangling rtf survives to the row loop (PASS = defect present)", "[defect]")
{
  scene s = leader_run_with_worker ();
  /* the clone tree as xcache hands it to the next leader: its own hash join node never ran (rtf NULL, status
   * as unpacked), its probe node is the worker's retired clone, its build input will list more than the bound */
  XASL_NODE *clone_build = make_build (unitseam_rtf_max_keys () + 1);
  XASL_NODE *hj2 = make_hj (s.clone.xasl, clone_build);
  XASL_NODE *target = nullptr;

  REQUIRE (hjoin_rtf_first_input (hj2, &target) == clone_build);	/* the hj->rtf guard is on the join node, not the target */
  REQUIRE (target == s.clone.xasl);
  CHECK (hjoin_rtf_build (nullptr, hj2, clone_build, target) == NO_ERROR);
  CHECK (hj2->proc.hashjoin.rtf == nullptr);
  CHECK (s.clone.xasl->rtf == s.rtf);	/* qexec_intprt_fnc () 12821 / row_body () 967 would now call hjoin_rtf_pass () on this */
  CHECK (s.clone.xasl->rtf_val == s.clone.cell[1]);
}

TEST_CASE ("PR8196 G3G4-01: the next execution's own parallel workers inherit the dangling pointer through bind_clone (PASS = defect present)", "[defect]")
{
  scene s = leader_run_with_worker ();
  XASL_NODE *clone_build = make_build (unitseam_rtf_max_keys () + 1);
  XASL_NODE *hj2 = make_hj (s.clone.xasl, clone_build);
  XASL_NODE *target = nullptr;
  scan_node worker2 = make_probe ();

  REQUIRE (hjoin_rtf_first_input (hj2, &target) == clone_build);
  REQUIRE (hjoin_rtf_build (nullptr, hj2, clone_build, target) == NO_ERROR);
  hjoin_rtf_bind_clone (worker2.xasl, s.clone.xasl);	/* px_scan_task.cpp:122 with orig = the retired clone */
  CHECK (worker2.xasl->rtf == s.rtf);
}
