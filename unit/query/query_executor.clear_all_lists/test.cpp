/*
 * PR#8436 (CBRD-27959) -- qexec_clear_all_lists (), src/query/query_executor.c (server).
 *
 * qexec_intprt_fnc () calls it once per row for the block it runs (query_executor.c:15053 at 482608da0). develop
 * (953e83256, :3755-3784) walked the block's next as well. The next of a block is its sibling in the parent's list:
 * the operands of a set operation are next to each other in the parent's aptr list (pt_set_aptr () /
 * pt_append_xasl (), xasl_generation.c), and the parallel subquery executor runs those operands as jobs at once
 * (query_executor.c:22060 add_job, px_query_executor.cpp run_jobs), so the per-row clear of one operand freed the
 * list files of the next operand's correlated subqueries while another thread was writing them. The PR clears the
 * block it gets and the blocks under it only.
 *
 * Fakes: qfile_close_list () does nothing; qfile_destroy_list () records the list id it gets -- qexec_clear_xasl_head ()
 * calls the two for a list id that is not result-cached (query_executor.c:1563-1567); xcache_uses_clones () is false,
 * so a cleared head ends XASL_CLEARED (:1584-1599). Real: qexec_clear_all_lists (), qexec_clear_head_lists (),
 * qexec_clear_scan_all_lists (), qexec_clear_xasl_head ().
 *
 * XASL: nodes calloc'd (regu_alloc + regu_init leave the rest zero: option Q_ALL, scan_op_type S_SELECT, parallelism
 * -1), status XASL_SUCCESS (an executed block), each with a list id of its own. The fixture is the issue's query: two
 * operands of a UNION ALL, each a join whose IN subquery, correlated with the outer table, sits on the dptr_list of
 * the inner scan (scan_ptr) and is linked to the IN term's regu variable -- the stack in the PR body,
 * qexec_clear_all_lists () -> qexec_clear_scan_all_lists () -> qexec_clear_head_lists () -> qexec_clear_xasl_head ().
 * Around it, the other branches of the function: the block's own dptr_list with a zero correlation-level subquery
 * after a correlated one (qexec_clear_head_lists () skips it), a bptr_list of two fetch blocks with a dptr each (the
 * PR replaced the call on the list by a loop over its elements), an fptr_list fetch block with a dptr, and a second
 * scan in the scan_ptr chain with a dptr.
 */
#include "catch.hpp"
#include "xasl.h"
#include "query_list.h"
#include "list_file.h"
#include "xasl_cache.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

void unitseam_clear_all_lists (THREAD_ENTRY * thread_p, XASL_NODE * xasl);

/* ---------------------------------------------------------------- fakes */
static std::vector<const QFILE_LIST_ID *> g_destroyed;

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
  g_destroyed.push_back (list_id);
}

bool
xcache_uses_clones (void)
{
  return false;
}

/* ---------------------------------------------------------------- fixture */
static XASL_NODE *
xnode (PROC_TYPE type, int flag = 0)
{
  XASL_NODE *x = static_cast<XASL_NODE *> (calloc (1, sizeof (XASL_NODE)));
  x->type = type;
  x->flag = flag;
  x->option = Q_ALL;
  x->scan_op_type = S_SELECT;
  x->parallelism = -1;
  x->status = XASL_SUCCESS;
  x->list_id = static_cast<QFILE_LIST_ID *> (calloc (1, sizeof (QFILE_LIST_ID)));
  return x;
}

struct operand
{
  XASL_NODE *blk;		/* the operand: a BUILDLIST over the outer table */
  XASL_NODE *scan;		/* blk->scan_ptr: the inner table of the join */
  XASL_NODE *in_subq;		/* scan->dptr_list: the IN subquery, correlated with the outer table */
  XASL_NODE *corr;		/* blk->dptr_list: a correlated subquery of the block itself */
  XASL_NODE *zero_corr;		/* corr->next: a zero correlation-level subquery (skipped) */
  XASL_NODE *fetch1, *fetch2;	/* blk->bptr_list: two fetch blocks */
  XASL_NODE *fetch1_subq, *fetch2_subq;	/* their dptr_list */
  XASL_NODE *pfetch, *pfetch_subq;	/* blk->fptr_list and its dptr */
  XASL_NODE *scan2, *scan2_subq;	/* scan->scan_ptr: a second scan with a dptr */
};

/* full = every branch of the function; otherwise only the join with its IN subquery */
static operand
make_operand (bool full)
{
  operand o;

  memset (&o, 0, sizeof (o));
  o.blk = xnode (BUILDLIST_PROC);
  o.scan = xnode (SCAN_PROC);
  o.in_subq = xnode (BUILDLIST_PROC, XASL_LINK_TO_REGU_VARIABLE);
  o.blk->scan_ptr = o.scan;
  o.scan->dptr_list = o.in_subq;
  if (!full)
    {
      return o;
    }

  o.corr = xnode (BUILDVALUE_PROC, XASL_LINK_TO_REGU_VARIABLE);
  o.zero_corr = xnode (BUILDVALUE_PROC, XASL_ZERO_CORR_LEVEL);
  o.corr->next = o.zero_corr;
  o.blk->dptr_list = o.corr;

  o.fetch1 = xnode (OBJFETCH_PROC);
  o.fetch2 = xnode (OBJFETCH_PROC);
  o.fetch1_subq = xnode (BUILDLIST_PROC);
  o.fetch2_subq = xnode (BUILDLIST_PROC);
  o.fetch1->dptr_list = o.fetch1_subq;
  o.fetch2->dptr_list = o.fetch2_subq;
  o.fetch1->next = o.fetch2;
  o.blk->bptr_list = o.fetch1;

  o.pfetch = xnode (OBJFETCH_PROC);
  o.pfetch_subq = xnode (BUILDLIST_PROC);
  o.pfetch->dptr_list = o.pfetch_subq;
  o.blk->fptr_list = o.pfetch;

  o.scan2 = xnode (SCAN_PROC);
  o.scan2_subq = xnode (BUILDLIST_PROC);
  o.scan2->dptr_list = o.scan2_subq;
  o.scan->scan_ptr = o.scan2;
  return o;
}

static unsigned char g_thread_buf[sizeof (cubthread::entry)] __attribute__ ((aligned (64)));

static void
clear_all_lists (XASL_NODE * xasl)
{
  memset (g_thread_buf, 0, sizeof (g_thread_buf));
  g_destroyed.clear ();
  unitseam_clear_all_lists ((THREAD_ENTRY *) g_thread_buf, xasl);
}

static int
destroyed (const XASL_NODE * x)
{
  return (int) std::count (g_destroyed.begin (), g_destroyed.end (), x->list_id);
}

/* a cleared head: its list destroyed once and its status back to an initial one */
static bool
cleared_once (const XASL_NODE * x)
{
  return destroyed (x) == 1 && x->status == XASL_CLEARED;
}

static bool
untouched (const XASL_NODE * x)
{
  return destroyed (x) == 0 && x->status == XASL_SUCCESS;
}

/* ---------------------------------------------------------------- cases */
TEST_CASE ("the block's correlated subqueries are cleared once each: its dptr, each scan's dptr, and the dptr under each bptr and fptr block", "[contract]")
{
  operand a1 = make_operand (true);

  clear_all_lists (a1.blk);
  CHECK (cleared_once (a1.in_subq));
  CHECK (cleared_once (a1.scan2_subq));
  CHECK (cleared_once (a1.corr));
  CHECK (cleared_once (a1.fetch1_subq));
  CHECK (cleared_once (a1.fetch2_subq));
  CHECK (cleared_once (a1.pfetch_subq));
  CHECK (g_destroyed.size () == 6);
}

TEST_CASE ("a zero correlation-level subquery on the dptr_list stays, and so do the heads of the block, its scans and its fetch blocks", "[contract]")
{
  operand a1 = make_operand (true);

  clear_all_lists (a1.blk);
  CHECK (untouched (a1.zero_corr));
  CHECK (untouched (a1.blk));
  CHECK (untouched (a1.scan));
  CHECK (untouched (a1.scan2));
  CHECK (untouched (a1.fetch1));
  CHECK (untouched (a1.fetch2));
  CHECK (untouched (a1.pfetch));
}

TEST_CASE ("the next block -- the next operand of a set operation -- is left as it is: its subqueries keep their lists", "[contract]")
{
  operand a1 = make_operand (true);
  operand a2 = make_operand (true);

  a1.blk->next = a2.blk;
  clear_all_lists (a1.blk);
  CHECK (untouched (a2.in_subq));
  CHECK (untouched (a2.scan2_subq));
  CHECK (untouched (a2.corr));
  CHECK (untouched (a2.fetch1_subq));
  CHECK (untouched (a2.fetch2_subq));
  CHECK (untouched (a2.pfetch_subq));
  /* the first operand's own subqueries are cleared as without a next */
  CHECK (cleared_once (a1.in_subq));
  CHECK (g_destroyed.size () == 6);
}

TEST_CASE ("the last operand (no next) clears its own subqueries and leaves the operand before it alone", "[contract]")
{
  operand a1 = make_operand (false);
  operand a2 = make_operand (false);

  a1.blk->next = a2.blk;
  clear_all_lists (a2.blk);
  CHECK (cleared_once (a2.in_subq));
  CHECK (untouched (a1.in_subq));
  CHECK (g_destroyed.size () == 1);
}

TEST_CASE ("a block with nothing under it destroys no list", "[contract]")
{
  XASL_NODE *x = xnode (BUILDLIST_PROC);

  clear_all_lists (x);
  CHECK (g_destroyed.empty ());
  CHECK (untouched (x));
}

TEST_CASE ("develop 953e83256: the per-row clear of the first operand also clears the IN subquery of the next operand (PASS = defect present)", "[defect]")
{
  operand a1 = make_operand (false);
  operand a2 = make_operand (false);

  a1.blk->next = a2.blk;
  clear_all_lists (a1.blk);
  CHECK (cleared_once (a1.in_subq));
  CHECK (cleared_once (a2.in_subq));
}

TEST_CASE ("develop 953e83256: only the operand after carries a subquery, and the first operand's per-row clear still frees it (PASS = defect present)", "[defect]")
{
  XASL_NODE *a1 = xnode (BUILDLIST_PROC);
  XASL_NODE *a1_scan = xnode (SCAN_PROC);
  operand a2 = make_operand (false);

  a1->scan_ptr = a1_scan;
  a1->next = a2.blk;
  clear_all_lists (a1);
  CHECK (cleared_once (a2.in_subq));
}
