/*
 * PR#8696 (CBRD-28231) -- pt_semantic_check_local () of src/parser/semantic_check.c (CS_MODE), its UNION /
 * INTERSECTION / DIFFERENCE case: pt_check_union_is_foldable () finds nothing to fold yet, the order by is checked,
 * then pt_semantic_type () types the set operation -- and that walk folds it (pt_eval_type () -> pt_fold_union ())
 * when an operand is known to be empty only after its CTE is folded. The case then infers the collations of the root
 * set operation from union_.arg1 / is_leaf_node.
 *
 * The contract the PR sets (semantic_check.c:11707-11712 at bb9575015): when the node pt_semantic_type () returns is
 * no longer a set operation -- the operand left, the NULL value, or the SELECT DISTINCT from a derived table of the
 * operand left -- nothing is left to check: the case stops, returns that node and reads no set-operation field of
 * it. A fold below the root leaves the root a set operation, which is still checked.
 * base (a617e19b7) goes on with the folded node read as a set operation (is_leaf_node, arg1): the client crash the PR
 * fixes. Here the collation step is a seam that records its call, so base shows the read as a call on a node that is
 * not a set operation instead of undefined behaviour.
 *
 * Real code: semantic_check.c (the TU: pt_semantic_check_local -- through hooks.cpp --, pt_check_union_is_foldable,
 * pt_fold_union, pt_fold_union_needs_derived, pt_set_operation_select_list, pt_orderby_matches_select_list,
 * pt_fold_union_collation, pt_fold_union_as_derived, pt_check_into_clause), parse_tree.c + parse_tree_cl.c
 * (parser_create_parser, parser_new_node, parser_copy_tree, parser_free_tree, pt_move_node), language_support.c +
 * intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c,
 * system_parameter.c.
 * Seams (same TU): pt_check_order_by (records, NO_ERROR), pt_get_select_list_coll_compat (records the query and the
 * column count, returns PT_UNION_COMP -- what it returns for columns without collation).
 * Fakes (suite.json "fakes"): pt_semantic_type (the type checking walk over the node kinds of these trees, in the
 * order pt_apply_union_stmt () walks them: WITH, arg1, arg2, then the set operation -- each query's post step is the
 * pt_eval_type () gate, each set operation's is pt_check_union_is_foldable () + pt_fold_union (), both real),
 * pt_false_where + pt_false_search_condition (type_checking.c:7125-7270 verbatim but the single-row test, which reads
 * the HAS_AGG flag only), pt_get_select_list (query_result.c:403: SELECT list; set operation: kept list or arg1's;
 * anything else NULL), mq_rewrite_query_as_derived (view_transform.c:6637: SELECT a_1.. FROM (copy of the query)
 * d3201), g_query_string / g_query_string_len, er_*, pt_parser_line_col, pt_free_dblink_remote_cols, locale side
 * tables, envvar_*, cubregex::get_engine_name.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "semantic_check.h"
#include "system_parameter.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "string_regex.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

/* the appendix (hooks.cpp, same TU as pt_semantic_check_local) */
PT_NODE *ut_semantic_check_local (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * top);

/* ---------------------------------------------------------------- fakes */

/* csql_grammar.y:464-466 -- the grammar's globals; parse_tree_cl.c:494-498 reads them with C linkage */
extern "C"
{
  char *g_query_string;
  int g_query_string_len;
}

/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* environment_variable.c: no CUBRID_* variable is set */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c envvar_trim_char body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* string_opfunc.c: calendar names of the built-in locales; not read here */
void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

/* db_date.c: AM/PM strings of date parsing; not read here */
void
db_date_locale_init (void)
{
}

/* scanner_support.c pt_parser_line_col () (C linkage): no lexer, position 0 */
extern "C" void
pt_parser_line_col (PT_NODE * node)
{
  if (node == NULL)
    {
      return;
    }
  node->line_number = 0;
  node->column_number = 0;
}

/* name_resolution.c: a parser that resolved no dblink table has no remote columns to free */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

namespace
{
  int g_er = NO_ERROR;
  int g_order_by_checks = 0;	/* pt_check_order_by () calls */
  int g_type_calls = 0;		/* pt_semantic_type () calls */
  int g_coll_calls = 0;		/* pt_get_select_list_coll_compat () calls: the collation inference of the root */
  PT_NODE *g_coll_query = NULL;	/* the node it was called on */
  int g_coll_cnt = -1;		/* the column count it was given */
}

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  g_er = err_id;
}

int
er_errid (void)
{
  return g_er;
}

void
er_clear (void)
{
  g_er = NO_ERROR;
}

void
er_stack_push (void)
{
}

void
er_stack_pop (void)
{
}

/* seam: semantic_check.c:15308 checks the ORDER BY items against the select list -- none here; records the call */
int
pt_check_order_by (PARSER_CONTEXT * parser, PT_NODE * query)
{
  g_order_by_checks++;
  return NO_ERROR;
}

/* seam: semantic_check.c:18601 pt_get_select_list_coll_compat () -- the first step of the collation inference of a
 * root set operation (semantic_check.c:11746). The real one walks union_.arg1 / arg2 of the node; columns without
 * collation (all of them here) leave PT_UNION_COMP. Records the node and the count instead, so a call on a node that
 * is not a set operation is seen rather than run. SEMAN_COMPATIBLE_INFO is local to the TU: the struct tag gives the
 * same symbol, and the enum PT_UNION_COMPATIBLE (also local) is returned as its int value. */
struct seman_compatible_info;
int
pt_get_select_list_coll_compat (PARSER_CONTEXT * parser, PT_NODE * query, seman_compatible_info * cinfo,
				int num_cinfo)
{
  g_coll_calls++;
  g_coll_query = query;
  g_coll_cnt = num_cinfo;
  return 1;			/* PT_UNION_COMP */
}

/* query_result.c:403 pt_get_select_list (): the select list of a SELECT (NULL for '*'), of a set operation the one it
 * kept or that of its first operand (the real one merges both operands' types), NULL for anything else (default:) */
PT_NODE *
pt_get_select_list (PARSER_CONTEXT * parser, PT_NODE * query)
{
  if (query == NULL)
    {
      return NULL;
    }
  if (query->node_type == PT_SELECT)
    {
      PT_NODE *list = query->info.query.q.select.list;
      if (list != NULL && list->node_type == PT_VALUE && list->type_enum == PT_TYPE_STAR)
	{
	  return NULL;
	}
      return list;
    }
  if (query->node_type == PT_UNION || query->node_type == PT_DIFFERENCE || query->node_type == PT_INTERSECTION)
    {
      return (query->info.query.q.union_.select_list != NULL ? query->info.query.q.union_.select_list
	      : pt_get_select_list (parser, query->info.query.q.union_.arg1));
    }
  return NULL;
}

/* type_checking.c:7253-7270 pt_false_search_condition () verbatim */
bool
pt_false_search_condition (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  while (node)
    {
      if (node->or_next == NULL
	  && (node->type_enum == PT_TYPE_NA || node->type_enum == PT_TYPE_NULL
	      || (node->node_type == PT_VALUE && node->type_enum == PT_TYPE_LOGICAL
		  && node->info.value.data_value.i == 0)))
	{
	  return true;
	}
      node = node->next;
    }
  return false;
}

/* type_checking.c:7125-7242 pt_false_where () verbatim, but pt_is_single_tuple () (xasl_generation.c:4261: no GROUP
 * BY and pt_has_aggregate ()) reads only the HAS_AGG flag -- no tree here has an aggregate */
bool
pt_false_where (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *from, *where = NULL;

  switch (node->node_type)
    {
    case PT_VALUE:
      where = node;
      break;

    case PT_SELECT:
      if (node->info.query.q.select.start_with
	  && pt_false_search_condition (parser, node->info.query.q.select.start_with))
	{
	  return true;
	}
      if (node->info.query.q.select.after_cb_filter
	  && pt_false_search_condition (parser, node->info.query.q.select.after_cb_filter))
	{
	  return true;
	}
      if (node->info.query.orderby_for && pt_false_search_condition (parser, node->info.query.orderby_for))
	{
	  return true;
	}
      if (node->info.query.q.select.group_by == NULL && PT_SELECT_INFO_IS_FLAGED (node, PT_SELECT_INFO_HAS_AGG))
	{
	  return false;
	}
      if (node->info.query.q.select.group_by
	  && pt_false_search_condition (parser, node->info.query.q.select.having))
	{
	  return true;
	}
      for (from = node->info.query.q.select.from; from; from = from->next)
	{
	  if (from->info.spec.join_type == PT_JOIN_LEFT_OUTER || from->info.spec.join_type == PT_JOIN_RIGHT_OUTER
	      || (from->next
		  && ((from->next->info.spec.join_type == PT_JOIN_LEFT_OUTER)
		      || (from->next->info.spec.join_type == PT_JOIN_RIGHT_OUTER))))
	    {
	      continue;
	    }
	  else if (from->info.spec.derived_table_type == PT_IS_SUBQUERY
		   || from->info.spec.derived_table_type == PT_IS_SET_EXPR)
	    {
	      if (PT_IS_FALSE_WHERE_VALUE (from->info.spec.derived_table))
		{
		  return true;
		}
	    }
	  else if (PT_SPEC_IS_CTE (from))
	    {
	      PT_NODE *cte = from->info.spec.cte_pointer;

	      CAST_POINTER_TO_NODE (cte);
	      if (cte && PT_IS_FALSE_WHERE_VALUE (cte->info.cte.non_recursive_part))
		{
		  return true;
		}
	    }
	}
      where = node->info.query.q.select.where;
      break;

    default:
      break;
    }

  return pt_false_search_condition (parser, where);
}

/* view_transform.c:6637 mq_rewrite_query_as_derived (): SELECT a_1, ... FROM (copy of the query) d3201 (a_1, ...),
 * each a_k of the type of the k-th column; the ids of the copy are not reset (mq_reset_ids_in_statement) */
PT_NODE *
mq_rewrite_query_as_derived (PARSER_CONTEXT * parser, PT_NODE * query)
{
  PT_NODE *range = parser_new_node (parser, PT_NAME);
  range->info.name.original = pt_append_string (parser, NULL, "d3201");
  PT_NODE *spec = parser_new_node (parser, PT_SPEC);
  spec->info.spec.derived_table = parser_copy_tree (parser, query);
  spec->info.spec.derived_table_type = PT_IS_SUBQUERY;
  spec->info.spec.range_var = range;
  spec->info.spec.id = (UINTPTR) spec;
  PT_NODE *new_query = parser_new_node (parser, PT_SELECT);
  new_query->info.query.q.select.from = spec;
  new_query->info.query.is_subquery = query->info.query.is_subquery;
  PT_NODE **head = &new_query->info.query.q.select.list;
  int i = 0;
  for (PT_NODE * col = pt_get_select_list (parser, spec->info.spec.derived_table); col != NULL; col = col->next)
    {
      char buf[16];
      snprintf (buf, sizeof (buf), "a_%d", ++i);
      PT_NODE *a = parser_new_node (parser, PT_NAME);
      a->info.name.original = pt_append_string (parser, NULL, buf);
      a->info.name.resolved = range->info.name.original;
      a->type_enum = col->type_enum;
      spec->info.spec.as_attr_list = parser_append_node (parser_copy_tree (parser, a), spec->info.spec.as_attr_list);
      *head = a;
      head = &a->next;
    }
  return new_query;
}

/* pt_to_false_subquery () (type_checking.c:7463-7491) for a query that is not on the outer side of a join, a sort
 * spec or INSERT's source: the NULL value marked is_false_where, keeping next, position, alias and hidden */
static PT_NODE *
ut_false_query_value (PARSER_CONTEXT * parser, PT_NODE * node)
{
  PT_NODE *next = node->next;
  int line = node->line_number, column = node->column_number, hidden = node->flag.is_hidden_column;
  const char *alias_print = node->alias_print;

  node->next = NULL;
  parser_free_tree (parser, node);
  node = parser_new_node (parser, PT_VALUE);
  node->line_number = line;
  node->column_number = column;
  node->alias_print = alias_print;
  node->type_enum = PT_TYPE_NULL;
  node->info.value.location = 0;
  node->info.value.is_false_where = true;
  node->flag.is_hidden_column = hidden;
  node->next = next;
  return node;
}

/* the type checking walk of pt_semantic_type () over the node kinds of these trees, post order as parser_walk_tree ()
 * goes (pt_apply_union_stmt (): WITH, arg1, arg2; pt_apply_with_clause () / pt_apply_cte (): the non-recursive part):
 * a query's post step is pt_eval_type ()'s gate (type_checking.c:8357-8362), a set operation's its fold
 * (type_checking.c:8435-8440) -- pt_check_union_is_foldable () and pt_fold_union () are the real ones */
static PT_NODE *
ut_type_walk (PARSER_CONTEXT * parser, PT_NODE * node)
{
  if (node == NULL)
    {
      return NULL;
    }
  if (PT_IS_UNION (node) || PT_IS_INTERSECTION (node) || PT_IS_DIFFERENCE (node))
    {
      if (node->info.query.with != NULL)
	{
	  for (PT_NODE * cte = node->info.query.with->info.with_clause.cte_definition_list; cte != NULL;
	       cte = cte->next)
	    {
	      cte->info.cte.non_recursive_part = ut_type_walk (parser, cte->info.cte.non_recursive_part);
	    }
	}
      node->info.query.q.union_.arg1 = ut_type_walk (parser, node->info.query.q.union_.arg1);
      node->info.query.q.union_.arg2 = ut_type_walk (parser, node->info.query.q.union_.arg2);
      STATEMENT_SET_FOLD fold = pt_check_union_is_foldable (parser, node);
      if (fold != STATEMENT_SET_FOLD_NOTHING)
	{
	  node = pt_fold_union (parser, node, fold);
	}
      return node;
    }
  if (node->node_type == PT_SELECT)
    {
      PT_MISC_TYPE s = node->info.query.is_subquery;
      if ((s == PT_IS_SUBQUERY || s == PT_IS_UNION_SUBQUERY || s == PT_IS_CTE_NON_REC_SUBQUERY
	   || s == PT_IS_CTE_REC_SUBQUERY) && pt_false_where (parser, node))
	{
	  return ut_false_query_value (parser, node);
	}
    }
  return node;
}

PT_NODE *
pt_semantic_type (PARSER_CONTEXT * parser, PT_NODE * tree, SEMANTIC_CHK_INFO * sc_info)
{
  g_type_calls++;
  return ut_type_walk (parser, tree);
}

/* ---------------------------------------------------------------- builders */
namespace
{
  bool boot ()
  {
    static bool done = false;
    if (!done)
      {
	done = true;
	lang_init_builtin ();
	if (lang_set_charset_lang ("en_US.utf8") != NO_ERROR)
	  {
	    return false;
	  }
      }
    g_er = NO_ERROR;
    g_order_by_checks = 0;
    g_type_calls = 0;
    g_coll_calls = 0;
    g_coll_query = NULL;
    g_coll_cnt = -1;
    g_query_string = NULL;
    g_query_string_len = 0;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  struct stmt
  {
    PARSER_CONTEXT *parser;

    stmt ()
    {
      parser = parser_create_parser ();
    }
    ~stmt ()
    {
      parser_free_parser (parser);
    }

    PT_NODE *name (const char *n, PT_TYPE_ENUM t = PT_TYPE_INTEGER)
    {
      PT_NODE *x = parser_new_node (parser, PT_NAME);
      x->info.name.original = pt_append_string (parser, NULL, n);
      x->info.name.resolved = "t";
      x->type_enum = t;
      return x;
    }

    /* the false constant the type checking leaves for WHERE 1 = 0 */
    PT_NODE *false_value ()
    {
      PT_NODE *w = parser_new_node (parser, PT_VALUE);
      w->type_enum = PT_TYPE_LOGICAL;
      w->info.value.data_value.i = 0;
      return w;
    }

    static PT_NODE *chain (std::initializer_list < PT_NODE * >nodes)
    {
      PT_NODE *head = NULL, *tail = NULL;
      for (PT_NODE * n:nodes)
	{
	  if (head == NULL)
	    {
	      head = n;
	    }
	  else
	    {
	      tail->next = n;
	    }
	  tail = n;
	}
      return head;
    }

    PT_NODE *entity (const char *table)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.entity_name = name (table, PT_TYPE_NONE);
      s->info.spec.range_var = name (table, PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* an operand: SELECT <list> FROM <from>, typed by its first column */
    PT_NODE *select (PT_NODE * list, PT_NODE * from)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = list;
      q->info.query.q.select.from = from;
      q->info.query.all_distinct = PT_ALL;
      q->type_enum = list->type_enum;
      return q;
    }

    /* WITH c AS (SELECT 1 x FROM db_root WHERE 1 = 0): its query, still a SELECT, has the false constant WHERE */
    PT_NODE *false_cte (const char *cte_name)
    {
      PT_NODE *q = select (name ("x"), entity ("db_root"));
      q->info.query.q.select.where = false_value ();
      q->info.query.is_subquery = PT_IS_CTE_NON_REC_SUBQUERY;
      PT_NODE *cte = parser_new_node (parser, PT_CTE);
      cte->info.cte.name = name (cte_name, PT_TYPE_NONE);
      cte->info.cte.non_recursive_part = q;
      return cte;
    }

    PT_NODE *with (PT_NODE * cte)
    {
      PT_NODE *w = parser_new_node (parser, PT_WITH_CLAUSE);
      w->info.with_clause.cte_definition_list = cte;
      return w;
    }

    PT_NODE *cte_ref (PT_NODE * cte)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.cte_pointer = cte;
      s->info.spec.range_var = name (cte->info.cte.name->info.name.original, PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* csql_grammar.y set operation rule: the operands become union subqueries */
    PT_NODE *set_op (PT_NODE_TYPE op, PT_NODE * a1, PT_NODE * a2, PT_MISC_TYPE all_distinct = PT_ALL)
    {
      PT_NODE *u = parser_new_node (parser, op);
      u->info.query.q.union_.arg1 = a1;
      u->info.query.q.union_.arg2 = a2;
      u->info.query.all_distinct = all_distinct;
      a1->info.query.is_subquery = PT_IS_UNION_SUBQUERY;
      a2->info.query.is_subquery = PT_IS_UNION_SUBQUERY;
      u->type_enum = a1->type_enum;
      return u;
    }

    PT_NODE *check (PT_NODE * node, PT_NODE * top)
    {
      return ut_semantic_check_local (parser, node, top);
    }
  };

  bool is_set_op (const PT_NODE * n)
  {
    return PT_IS_UNION (n) || PT_IS_INTERSECTION (n) || PT_IS_DIFFERENCE (n);
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("PR8696 S1 (control): a root UNION ALL that the type checking does not fold stays a set operation and "
	   "goes on to the collation inference of its columns", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *a1 = s.select (s.chain ({s.name ("x"), s.name ("y")}), s.entity ("t1"));
  PT_NODE *a2 = s.select (s.chain ({s.name ("id"), s.name ("v")}), s.entity ("t2"));
  PT_NODE *u = s.set_op (PT_UNION, a1, a2);
  PT_NODE *next = s.name ("next_stmt");
  u->next = next;

  PT_NODE *r = s.check (u, u);

  CHECK (g_type_calls == 1);
  REQUIRE (r == u);
  CHECK (r->info.query.q.union_.arg1 == a1);
  CHECK (r->info.query.q.union_.arg2 == a2);
  CHECK (g_coll_calls == 1);
  CHECK (g_coll_query == u);
  CHECK (g_coll_cnt == 2);
  CHECK (r->next == next);
  CHECK_FALSE (pt_has_error (s.parser));
}

TEST_CASE ("PR8696 S1b (control): a root DIFFERENCE ALL that the type checking does not fold is a set operation "
	   "too: it goes on to the collation inference (the stop is for a node that is none of the three)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *a1 = s.select (s.chain ({s.name ("x"), s.name ("y"), s.name ("z")}), s.entity ("t1"));
  PT_NODE *a2 = s.select (s.chain ({s.name ("id"), s.name ("v"), s.name ("w")}), s.entity ("t2"));
  PT_NODE *d = s.set_op (PT_DIFFERENCE, a1, a2);

  PT_NODE *r = s.check (d, d);

  CHECK (g_type_calls == 1);
  REQUIRE (r == d);
  CHECK (g_coll_calls == 1);
  CHECK (g_coll_query == d);
  CHECK (g_coll_cnt == 3);
  CHECK_FALSE (pt_has_error (s.parser));
}

TEST_CASE ("PR8696 S2: WITH c AS (... WHERE 1 = 0) (SELECT x FROM c) UNION ALL (SELECT id FROM t) -- the type "
	   "checking folds the CTE, then the operand, then the set operation into the operand left: that SELECT is "
	   "returned and no set-operation step runs on it", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *cte = s.false_cte ("c");
  PT_NODE *a1 = s.select (s.name ("x"), s.cte_ref (cte));
  PT_NODE *a2 = s.select (s.name ("id"), s.entity ("t"));
  PT_NODE *u = s.set_op (PT_UNION, a1, a2);
  PT_NODE *w = s.with (cte);
  u->info.query.with = w;
  PT_NODE *next = s.name ("next_stmt");
  u->next = next;

  /* nothing to fold before the type checking: the CTE is still a query */
  REQUIRE (pt_check_union_is_foldable (s.parser, u) == STATEMENT_SET_FOLD_NOTHING);

  PT_NODE *r = s.check (u, u);

  CHECK (g_type_calls == 1);
  REQUIRE (r != NULL);
  CHECK (r == a2);
  CHECK (r->node_type == PT_SELECT);
  CHECK (r->info.query.with == w);	/* the set operation's WITH went to the operand (pt_fold_union) */
  CHECK (r->next == next);
  CHECK (g_coll_calls == 0);
  CHECK_FALSE (pt_has_error (s.parser));
}

TEST_CASE ("PR8696 S3 (boundary): a UNION without ALL whose operand reads the folded CTE becomes a SELECT DISTINCT "
	   "from a derived table of the operand left (pt_fold_union_as_derived) -- a new node, returned as it is, no "
	   "set-operation step runs on it", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *cte = s.false_cte ("c");
  PT_NODE *a1 = s.select (s.name ("x"), s.cte_ref (cte));
  PT_NODE *a2 = s.select (s.name ("id"), s.entity ("t"));
  PT_NODE *u = s.set_op (PT_UNION, a1, a2, PT_DISTINCT);
  u->info.query.with = s.with (cte);

  PT_NODE *r = s.check (u, u);

  CHECK (g_type_calls == 1);
  REQUIRE (r != NULL);
  CHECK (r->node_type == PT_SELECT);
  CHECK (r->info.query.all_distinct == PT_DISTINCT);
  CHECK (r->info.query.flag.folded_distinct == 1);
  REQUIRE (r->info.query.q.select.from != NULL);
  CHECK (r->info.query.q.select.from->info.spec.derived_table_type == PT_IS_SUBQUERY);
  CHECK (g_coll_calls == 0);
  CHECK_FALSE (pt_has_error (s.parser));
}

TEST_CASE ("PR8696 S4 (boundary): (A UNION ALL B) UNION ALL C where B reads the folded CTE -- the inner set "
	   "operation folds into A, the root stays a set operation and is still checked (collation inference runs "
	   "once, on the root)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *cte = s.false_cte ("c");
  PT_NODE *a = s.select (s.chain ({s.name ("x"), s.name ("y")}), s.entity ("t1"));
  PT_NODE *b = s.select (s.chain ({s.name ("x"), s.name ("y")}), s.cte_ref (cte));
  PT_NODE *c = s.select (s.chain ({s.name ("id"), s.name ("v")}), s.entity ("t2"));
  PT_NODE *inner = s.set_op (PT_UNION, a, b);
  inner->info.query.q.union_.is_leaf_node = 1;	/* pt_mark_union_leaf_nodes () */
  PT_NODE *root = s.set_op (PT_UNION, inner, c);
  root->info.query.with = s.with (cte);

  PT_NODE *r = s.check (root, root);

  CHECK (g_type_calls == 1);
  REQUIRE (r == root);
  REQUIRE (is_set_op (r));
  CHECK (r->info.query.q.union_.arg1 == a);	/* the inner set operation folded into A */
  CHECK (r->info.query.q.union_.arg2 == c);
  CHECK (g_coll_calls == 1);
  CHECK (g_coll_query == root);
  CHECK (g_coll_cnt == 2);
  CHECK_FALSE (pt_has_error (s.parser));
}

TEST_CASE ("PR8696 S5 (boundary): a DIFFERENCE ALL whose second operand reads the folded CTE folds into its first "
	   "operand -- returned as it is, no set-operation step runs on it", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *cte = s.false_cte ("c");
  PT_NODE *a1 = s.select (s.name ("id"), s.entity ("t"));
  PT_NODE *a2 = s.select (s.name ("x"), s.cte_ref (cte));
  PT_NODE *u = s.set_op (PT_DIFFERENCE, a1, a2);
  u->info.query.with = s.with (cte);

  PT_NODE *r = s.check (u, u);

  CHECK (g_type_calls == 1);
  REQUIRE (r != NULL);
  CHECK (r == a1);
  CHECK (r->node_type == PT_SELECT);
  CHECK (g_coll_calls == 0);
  CHECK_FALSE (pt_has_error (s.parser));
}

/* kept last: base (a617e19b7) reads the NULL value as a set operation -- union_.arg1 falls on the value's
 * host_var_index (-1) bytes, 0xffffffff00000000 -- and dies in pt_get_select_list () (SIGSEGV), as the client did; the
 * cases before it still report */
TEST_CASE ("PR8696 S6 (boundary): an INTERSECTION ALL whose operand reads the CTE the type checking folds becomes the "
	   "NULL value -- returned as it is, no set-operation step runs on it", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *cte = s.false_cte ("c");
  PT_NODE *a1 = s.select (s.name ("x"), s.cte_ref (cte));
  PT_NODE *a2 = s.select (s.name ("id"), s.entity ("t"));
  PT_NODE *u = s.set_op (PT_INTERSECTION, a1, a2);
  u->info.query.with = s.with (cte);

  PT_NODE *r = s.check (u, u);

  CHECK (g_type_calls == 1);
  REQUIRE (r != NULL);
  CHECK (r->node_type == PT_VALUE);
  CHECK (r->type_enum == PT_TYPE_NULL);
  CHECK (r->info.value.is_false_where);
  CHECK (g_coll_calls == 0);
  CHECK_FALSE (pt_has_error (s.parser));
}
