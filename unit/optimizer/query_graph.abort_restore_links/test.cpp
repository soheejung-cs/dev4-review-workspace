/*
 * PR#8664 (CBRD-28204) -- the parse tree links the optimizer cuts for the moment and qo_abort () puts back,
 * src/optimizer/query_graph.c (CS_MODE), reached through the appendix hooks (hooks.cpp).
 *
 *   qo_expr_segs (env, expr, result)        cuts expr->next around its walk (the PR records the cut with
 *                                           qo_detach_link (), so qo_abort () puts it back)
 *   qo_discover_sort_limit_nodes (env)      cuts the next link of the ORDER BY column of the select list by hand
 *                                           (save_next) and calls qo_expr_segs () on it while it is cut
 *
 * A QO_ASSERT that fails inside qo_expr_segs () is the real lookup_node ()'s QO_ASSERT (env, aux->node_type ==
 * PT_NAME) -- the reach the PR body names for converting qo_expr_segs () (set_seg_expr () -> lookup_node ()).
 * The seam lookup_node () stands for it: armed, it calls qo_abort () as the failed QO_ASSERT would; unarmed it
 * returns NULL ("this attr resolves to an enclosing scope", set_seg_node () adds nothing).
 *
 * Real code: query_graph.c (the TU: qo_expr_segs, set_seg_expr, set_seg_node, qo_seg_nodes,
 * qo_discover_sort_limit_nodes, qo_abort and on the head qo_detach_link / qo_reattach_link), the bitset
 * (query_bitset.c #included below).
 * Fakes: parser_walk_tree (pre / post on the node and the rest of its next list -- the names here have no
 * children), pt_continue_walk (parse_tree_cl.c body), pt_get_select_list (query_result.c: a PT_SELECT's list),
 * pt_has_analytic / pt_has_aggregate (a column name has neither), pt_get_query_limit_value (a usable LIMIT,
 * NO_ERROR), er_set (one level).
 *
 * Tags: [contract] expected behaviour; [defect] reproduces a defect (PASS = the defect is there).
 *
 * scene: SELECT c1, c2, c3 FROM o, i ... ORDER BY 1 LIMIT 10 -- one partition of two nodes, no term (so no
 * after-join / other term stops the search), the ORDER BY position 1 = c1.
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "optimizer.h"
#include "query_graph.h"
#include "error_manager.h"
#include <cstdlib>
#include <vector>

#include "query_bitset.c"

void unitseam_expr_segs (QO_ENV * env, PT_NODE * expr, BITSET * result, int *aborted);
void unitseam_sort_limit_nodes (QO_ENV * env, int *aborted);

/* ---------------------------------------------------------------- fakes */

namespace
{
  bool g_abort_armed = false;
  int g_lookups = 0;
  int g_er_sets = 0;
  std::vector < PT_NODE * >g_walked;
  std::vector < void *>g_alloc;

  void *zalloc (size_t n)
  {
    void *p = calloc (1, n);
    g_alloc.push_back (p);
    return p;
  }

  void free_all ()
  {
  for (void *p:g_alloc)
      {
	free (p);
      }
    g_alloc.clear ();
  }
}

/* error_manager: one level; qo_abort () er_set ()s ER_FAILED_ASSERTION before its longjmp */
void
er_set (int, const char *, const int, int, int, ...)
{
  g_er_sets++;
}

/* query_graph.c lookup_node () (seam): the real one QO_ASSERTs aux->node_type == PT_NAME for a node that is not a
 * function index expression; armed, the fake fails that assertion (qo_abort () does not return) */
QO_NODE *
lookup_node (PT_NODE * attr, QO_ENV * env, PT_NODE ** entity)
{
  g_lookups++;
  if (g_abort_armed)
    {
      qo_abort (env, __FILE__, __LINE__);
    }
  *entity = NULL;
  return NULL;
}

/* parse_tree_cl.c parser_walk_tree (): pre, then post, on the node and on the rest of its next list; the PT_NAME
 * nodes here have no children. The walk records what it visited -- a cut link shows as the list not followed. */
PT_NODE *
parser_walk_tree (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE_WALK_FUNCTION pre_function, void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  for (PT_NODE * n = node; n != NULL; n = n->next)
    {
      int cw = PT_CONTINUE_WALK;
      g_walked.push_back (n);
      if (pre_function != NULL)
	{
	  (void) pre_function (parser, n, pre_argument, &cw);
	}
      if (cw == PT_STOP_WALK)
	{
	  break;
	}
      if (post_function != NULL)
	{
	  (void) post_function (parser, n, post_argument, &cw);
	}
    }
  return node;
}

/* parse_tree_cl.c:1281 pt_continue_walk () body */
PT_NODE *
pt_continue_walk (PARSER_CONTEXT * parser, PT_NODE * tree, void *arg, int *continue_walk)
{
  *continue_walk = PT_CONTINUE_WALK;
  return tree;
}

/* query_result.c pt_get_select_list (): a PT_SELECT gives its list (no PT_NODE_LIST, no star here) */
PT_NODE *
pt_get_select_list (PARSER_CONTEXT * parser, PT_NODE * query)
{
  return (query != NULL && query->node_type == PT_SELECT) ? query->info.query.q.select.list : NULL;
}

/* parser_support.c: a column name holds no analytic and no aggregate function */
bool
pt_has_analytic (PARSER_CONTEXT * parser, PT_NODE * node)
{
  return false;
}

bool
pt_has_aggregate (PARSER_CONTEXT * parser, PT_NODE * node)
{
  return false;
}

/* parser_support.c pt_get_query_limit_value (): a usable LIMIT (the value is not read on the paths taken here) */
int
pt_get_query_limit_value (PARSER_CONTEXT * parser, PT_NODE * query, DB_VALUE * limit_val, bool add_offset)
{
  return NO_ERROR;
}

/* ---------------------------------------------------------------- scenes */

namespace
{
  PT_NODE *pt (PT_NODE_TYPE t)
  {
    PT_NODE *n = (PT_NODE *) zalloc (sizeof (PT_NODE));
    n->node_type = t;
    return n;
  }

  PT_NODE *name ()
  {
    PT_NODE *n = pt (PT_NAME);
    n->info.name.meta_class = PT_NORMAL;
    n->type_enum = PT_TYPE_INTEGER;
    return n;
  }

  struct scene
  {
    PARSER_CONTEXT *parser;
    QO_ENV *env;
    PT_NODE *tree, *c1, *c2, *c3;

    scene ()
    {
      free_all ();
      g_abort_armed = false;
      g_lookups = g_er_sets = 0;
      g_walked.clear ();

      parser = (PARSER_CONTEXT *) zalloc (sizeof (PARSER_CONTEXT));	/* no error message: pt_has_error () false */
      env = (QO_ENV *) zalloc (sizeof (QO_ENV));
      env->parser = parser;

      /* SELECT c1, c2, c3 FROM o, i ORDER BY 1 LIMIT 10 */
      tree = pt (PT_SELECT);
      tree->info.query.all_distinct = PT_ALL;
      c1 = name ();
      c2 = name ();
      c3 = name ();
      c1->next = c2;
      c2->next = c3;
      tree->info.query.q.select.list = c1;
      PT_NODE *ob = pt (PT_SORT_SPEC);
      ob->info.sort_spec.pos_descr.pos_no = 1;
      tree->info.query.order_by = ob;
      env->pt_tree = tree;

      /* two nodes in one partition, no term */
      env->nnodes = env->Nnodes = 2;
      env->nodes = (QO_NODE *) zalloc (2 * sizeof (QO_NODE));
      env->npartitions = 1;
      env->partitions = (QO_PARTITION *) zalloc (sizeof (QO_PARTITION));
      bitset_init (&env->partitions[0].nodes, env);
      bitset_add (&env->partitions[0].nodes, 0);
      bitset_add (&env->partitions[0].nodes, 1);
      env->nterms = 0;
    }

    ~scene ()
    {
      bitset_delset (&env->partitions[0].nodes);
    }
  };
}

/* ---------------------------------------------------------------- qo_expr_segs (): the PR's change */

TEST_CASE ("PR8664 K0: control -- qo_expr_segs () walks the expression alone (next cut during the walk) and puts the "
	   "link back", "[contract]")
{
  scene s;
  BITSET segs;
  int aborted = -1;

  bitset_init (&segs, s.env);
  unitseam_expr_segs (s.env, s.c1, &segs, &aborted);

  CHECK (aborted == 0);
  CHECK (g_walked.size () == 1);	/* c1 only: c2, c3 not followed */
  CHECK (s.c1->next == s.c2);
  CHECK (s.c2->next == s.c3);
  CHECK (QO_ENV_TMP_BITSET (s.env) == NULL);
  bitset_delset (&segs);
}

TEST_CASE ("PR8664 K1: an abort inside qo_expr_segs ()'s walk (lookup_node ()'s QO_ASSERT) leaves expr->next as it "
	   "was -- qo_abort () puts back the link qo_expr_segs () cut (head: the PR's fix; base: the link stays cut)",
	   "[contract]")
{
  scene s;
  BITSET segs;
  int aborted = -1;

  bitset_init (&segs, s.env);
  g_abort_armed = true;
  unitseam_expr_segs (s.env, s.c1, &segs, &aborted);

  CHECK (aborted == 1);
  CHECK (g_er_sets == 1);
  CHECK (s.c1->next == s.c2);
  bitset_delset (&segs);
}

/* ---------------------------------------------------------------- qo_discover_sort_limit_nodes () */

TEST_CASE ("PR8664 C0: control -- qo_discover_sort_limit_nodes () reads the ORDER BY column c1 alone and leaves the "
	   "select list whole (no abort; every node is needed, so the search is abandoned)", "[contract]")
{
  scene s;
  int aborted = -1;

  unitseam_sort_limit_nodes (s.env, &aborted);

  CHECK (aborted == 0);
  CHECK (g_lookups == 1);	/* qo_expr_segs () on c1 reached lookup_node () */
  CHECK (g_walked.size () == 1);	/* c1 alone: the walk did not follow c2 */
  CHECK (s.c1->next == s.c2);
  CHECK (s.c2->next == s.c3);
  CHECK (s.env->use_sort_limit == QO_SL_INVALID);
}

TEST_CASE ("PR8664 F2: an abort inside qo_expr_segs () on the ORDER BY column leaves the select list cut after that "
	   "column -- qo_discover_sort_limit_nodes () cut c1->next by hand (save_next), qo_expr_segs () recorded the "
	   "NULL it found, and qo_abort () puts back only that NULL: SELECT c1, c2, c3 runs as SELECT c1 "
	   "(PASS = the defect is there)", "[defect]")
{
  scene s;
  int aborted = -1;

  g_abort_armed = true;
  unitseam_sort_limit_nodes (s.env, &aborted);

  REQUIRE (aborted == 1);
  CHECK (g_er_sets == 1);
  CHECK (s.tree->info.query.q.select.list == s.c1);
  CHECK (s.c1->next == NULL);	/* c2 and c3 are gone from the select list */
}
