/*
 * PR#8750 (CBRD-28372) G1 -- parser_walk_tree () / parser_walk_leaves () walk-mode contract (src/parser/parse_tree_cl.c
 * pt_walk_private ()).  Trees of PT_EXPR (arg1, arg2) and PT_NAME nodes built with parser_new_node (); the node id is
 * line_number.  pre / post record the ids they are called on.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "memory_alloc.h"
#include "string_regex.hpp"
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}
const char *envvar_get (const char *) { return NULL; }
void envvar_trim_char (char *, const int) { }
void init_builtin_calendar_names (LANG_LOCALE_DATA *) { }
void db_date_locale_init (void) { }
void pt_free_dblink_remote_cols (PARSER_CONTEXT *) { }
namespace { int g_er = NO_ERROR; }
void er_set (int, const char *, const int, int err_id, int, ...) { g_er = err_id; }
int er_errid (void) { return g_er; }
void er_clear (void) { g_er = NO_ERROR; }

namespace
{
  const int UT_SKIP = PT_LIST_WALK + 1;	/* PT_SKIP_WALK at head; an unknown mode at base */
  PARSER_CONTEXT *g_parser = NULL;

  PARSER_CONTEXT *boot ()
  {
    if (g_parser == NULL)
      {
	lang_init_builtin ();
	REQUIRE (lang_set_charset_lang ("en_US.utf8") == NO_ERROR);
	g_parser = parser_create_parser ();
	REQUIRE (g_parser != NULL);
      }
    return g_parser;
  }

  PT_NODE *nm (int id)
  {
    PT_NODE *n = parser_new_node (g_parser, PT_NAME);
    n->info.name.original = "x";
    n->line_number = id;
    return n;
  }
  PT_NODE *ex (int id, PT_NODE * a1, PT_NODE * a2)
  {
    PT_NODE *n = parser_new_node (g_parser, PT_EXPR);
    n->info.expr.op = PT_PLUS;
    n->info.expr.arg1 = a1;
    n->info.expr.arg2 = a2;
    n->line_number = id;
    return n;
  }

  struct rec
  {
    std::vector<int> pre, post;
    std::map<int, int> pre_mode;	/* mode the pre sets at id */
    std::map<int, int> post_mode;	/* mode the post sets at id */
    int null_at = -1;		/* pre returns NULL at id */
    std::map<int, int> post_seen;	/* mode the post finds at id */
  };

  PT_NODE *pre (PARSER_CONTEXT *, PT_NODE * node, void *arg, int *cw)
  {
    rec *r = (rec *) arg;
    r->pre.push_back (node->line_number);
    auto it = r->pre_mode.find (node->line_number);
    if (it != r->pre_mode.end ())
      {
	*cw = it->second;
      }
    if (node->line_number == r->null_at)
      {
	return NULL;
      }
    return node;
  }
  PT_NODE *post (PARSER_CONTEXT *, PT_NODE * node, void *arg, int *cw)
  {
    rec *r = (rec *) arg;
    r->post.push_back (node->line_number);
    r->post_seen[node->line_number] = *cw;
    auto it = r->post_mode.find (node->line_number);
    if (it != r->post_mode.end ())
      {
	*cw = it->second;
      }
    return node;
  }
  typedef std::vector<int> V;
}

/* 1 = (2 = (3, 4), 5 = (6, 7)), next 8 = (9, 10) */
static PT_NODE *tree1 ()
{
  PT_NODE *root = ex (1, ex (2, nm (3), nm (4)), ex (5, nm (6), nm (7)));
  root->next = ex (8, nm (9), nm (10));
  return root;
}

TEST_CASE ("W1 [contract] a PT_LIST_WALK set at a leaf applies to it only: the sibling after it is walked under", "[contract]")
{
  boot ();
  rec r;
  r.pre_mode[2] = PT_LIST_WALK;
  parser_walk_tree (g_parser, tree1 (), pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 5, 6, 7, 8, 9, 10}));
  CHECK (r.post == V ({2, 6, 7, 5, 9, 10, 8, 1}));
}

TEST_CASE ("W2 [contract] PT_LEAF_WALK at the head of a list: its children and not its next list; the rest of the tree goes on", "[contract]")
{
  boot ();
  rec r;
  r.pre_mode[3] = PT_LEAF_WALK;
  PT_NODE *t = tree1 ();
  t->info.expr.arg1->info.expr.arg1->next = nm (11);	/* 3 -> 11 */
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
}

TEST_CASE ("W3 [contract] PT_SKIP_WALK: no children, no or_next, no next of the node; the walk goes on", "[contract]")
{
  boot ();
  rec r;
  r.pre_mode[2] = UT_SKIP;
  PT_NODE *t = tree1 ();
  t->info.expr.arg1->or_next = nm (12);	/* 2 or_next 12 */
  t->info.expr.arg1->next = nm (13);	/* 2 next 13 */
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 5, 6, 7, 8, 9, 10}));
  CHECK (r.post_seen[1] == PT_CONTINUE_WALK);
}

TEST_CASE ("W4 [contract] STOP at a child ends the walk: no pre after it; the entered nodes get their post with STOP", "[contract]")
{
  boot ();
  rec r;
  r.pre_mode[3] = PT_STOP_WALK;
  parser_walk_tree (g_parser, tree1 (), pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3}));
  CHECK (r.post == V ({3, 2, 1}));
  CHECK (r.post_seen[1] == PT_STOP_WALK);
}

TEST_CASE ("W5 [contract] STOP inside an or_next list: the rest of the or_next list and the next list are not entered", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = tree1 ();
  PT_NODE *o1 = nm (20), *o2 = nm (21);
  t->info.expr.arg1->or_next = o1;	/* 2 or_next 20 or_next 21 */
  o1->or_next = o2;
  t->info.expr.arg1->next = nm (22);	/* 2 next 22 */
  r.pre_mode[20] = PT_STOP_WALK;
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 4, 20}));
  CHECK (r.post == V ({3, 4, 20, 2, 1}));
}

TEST_CASE ("W6 [contract] STOP inside a next list: the nodes after it are not entered; the posts run backwards", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = tree1 ();
  t->next->next = nm (30);	/* 1 -> 8 -> 30 */
  r.pre_mode[8] = PT_STOP_WALK;
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 4, 5, 6, 7, 8}));
  CHECK (r.post == V ({3, 4, 2, 6, 7, 5, 8, 1}));
}

TEST_CASE ("W7 [contract] pre STOP + post CONTINUE at the same node: its subtree and its own lists are skipped, the parent's next child is walked", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = tree1 ();
  t->info.expr.arg1->next = nm (40);	/* 2 next 40 */
  r.pre_mode[2] = PT_STOP_WALK;
  r.post_mode[2] = PT_CONTINUE_WALK;
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 5, 6, 7, 8, 9, 10}));
}

TEST_CASE ("W8 [contract] a STOP below resumed only by an ancestor's post: the ancestor's own next list stays skipped", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = tree1 ();
  t->info.expr.arg1->next = nm (41);	/* 2 next 41 */
  r.pre_mode[3] = PT_STOP_WALK;
  r.post_mode[2] = PT_CONTINUE_WALK;	/* 2 resumes, 3 does not */
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 5, 6, 7, 8, 9, 10}));
}

TEST_CASE ("W9 [contract] a mode a post sets other than STOP does not reach the nodes after it", "[contract]")
{
  boot ();
  rec r;
  r.post_mode[2] = PT_LIST_WALK;
  parser_walk_tree (g_parser, tree1 (), pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
}

TEST_CASE ("W10 [contract] a post that sets STOP ends the walk after it", "[contract]")
{
  boot ();
  rec r;
  r.post_mode[2] = PT_STOP_WALK;
  parser_walk_tree (g_parser, tree1 (), pre, &r, post, &r);
  CHECK (r.pre == V ({1, 2, 3, 4}));
  CHECK (r.post == V ({3, 4, 2, 1}));
}

TEST_CASE ("W11 [contract] pre returning NULL: the node is cut, its post not called; the mode it set does not reach the next sibling", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = tree1 ();
  r.pre_mode[2] = PT_LIST_WALK;
  r.null_at = 2;
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (t->info.expr.arg1 == NULL);
  CHECK (r.pre == V ({1, 2, 5, 6, 7, 8, 9, 10}));
  CHECK (r.post == V ({6, 7, 5, 9, 10, 8, 1}));
}

TEST_CASE ("W12 [contract] pre returning NULL as the last child leaves its mode for the parent's post (only STOP is read)", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *t = ex (1, nm (2), nm (3));
  r.pre_mode[3] = PT_LIST_WALK;
  r.null_at = 3;
  parser_walk_tree (g_parser, t, pre, &r, post, &r);
  CHECK (r.post_seen[1] == PT_LIST_WALK);
}

TEST_CASE ("W13 [contract] parser_walk_leaves: a STOP under the first or_next root does not walk the next root", "[contract]")
{
  boot ();
  rec r;
  PT_NODE *a = ex (1, nm (2), nm (3));
  PT_NODE *b = ex (4, nm (5), nm (6));
  a->or_next = b;
  r.pre_mode[2] = PT_STOP_WALK;
  parser_walk_leaves (g_parser, a, pre, &r, post, &r);
  CHECK (r.pre == V ({2}));
}
