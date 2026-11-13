/*
 * PR#8428 (CBRD-27961) -- pt_fold_union () of src/parser/semantic_check.c (CS_MODE): a set operation whose arm has an
 * always-false WHERE folds to the other arm (STATEMENT_SET_FOLD_AS_ARG1/ARG2), to a derived table around that arm when
 * both carry ORDER BY/LIMIT (mq_rewrite_query_as_derived), or to a NULL value (STATEMENT_SET_FOLD_AS_NULL). Callers:
 * pt_semantic_check_local () (semantic_check.c:11631) and pt_eval_type () (type_checking.c:8406).
 *
 * Where the statement text lives (csql_grammar.y, parse_tree_cl.c):
 *   - parser_new_node () gives every node g_query_string (the start of the statement being parsed, set when the
 *     statement starts, csql_grammar.y:1844) and g_query_string_len (parse_tree_cl.c:2501-2502)
 *   - the statement's end action puts the statement's own length on the top node only and then sets
 *     g_query_string_len to it (csql_grammar.y:1921-1922): the other nodes of statement k carry the length of statement
 *     k-1 (0 for the first), and parser_main () does not reset the globals when it returns (csql_grammar.y:23955-23957
 *     reset them only at the start of the next parse)
 *   - a driver session compiles the statement text again for the types of its bind values and reads it from the
 *     compiled statement node (db_vdb.c:2988-2989); a length <= 0 is ER_QPROC_INVALID_XASLNODE (db_vdb.c:3069-3074)
 *
 * The contract the PR sets (semantic_check.c:19120-19123, 19220-19221): the node that takes the place of the set
 * operation -- the remaining arm, the derived table made around it, or the NULL value -- takes the set operation's
 * sql_user_text and sql_user_text_len, as it takes its line, column, alias and next.
 *
 * Real code: semantic_check.c (the TU: pt_fold_union), parse_tree.c + parse_tree_cl.c (parser_create_parser,
 * parser_new_node, pt_init_*, parser_free_tree, pt_move_node, parser_free_parser), language_support.c + intl_support.c +
 * chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Seam: pt_check_order_by (semantic_check.c:15234, same TU) -- counts the calls, NO_ERROR. Fakes (suite.json "fakes"):
 * g_query_string / g_query_string_len (csql_grammar.y:464-466, C linkage as parse_tree_cl.c:494-498 declares them),
 * mq_rewrite_query_as_derived (view_transform.c:6617-6656: a new PT_SELECT made by parser_new_node over a PT_SPEC whose
 * derived table is the query), cubregex::get_engine_name, envvar_get / envvar_trim_char, init_builtin_calendar_names,
 * db_date_locale_init, pt_parser_line_col, pt_free_dblink_remote_cols, er_*.
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

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
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

/* scanner_support.c pt_parser_line_col () (C linkage): no lexer, position 0 -- the cases set the positions they check */
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
  PT_NODE *g_order_by_checked = NULL;	/* the query it last checked */
  int g_derived_calls = 0;	/* mq_rewrite_query_as_derived () calls */
  bool g_derived_fails = false;
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

/* seam: semantic_check.c:15234 checks the ORDER BY items against the select list; the fold calls it once on the node
 * that replaces the set operation (semantic_check.c:19207) -- here it records that call */
int
pt_check_order_by (PARSER_CONTEXT * parser, PT_NODE * query)
{
  g_order_by_checks++;
  g_order_by_checked = query;
  return NO_ERROR;
}

/* view_transform.c:6617-6656 mq_rewrite_query_as_derived (): the derived table spec (parser_new_node PT_SPEC, derived
 * table = the query -- the real one copies it) and a new PT_SELECT made by parser_new_node over it; the select list
 * that names the derived columns is left out (the fold does not read it). On failure the real one leaves a parser
 * error (PT_INTERNAL_ERROR) and returns NULL. */
PT_NODE *
mq_rewrite_query_as_derived (PARSER_CONTEXT * parser, PT_NODE * query)
{
  g_derived_calls++;
  if (g_derived_fails)
    {
      parser->flag.has_internal_error = 1;
      return NULL;
    }
  PT_NODE *spec = parser_new_node (parser, PT_SPEC);
  spec->info.spec.derived_table = query;
  spec->info.spec.derived_table_type = PT_IS_SUBQUERY;
  spec->info.spec.id = (UINTPTR) spec;
  PT_NODE *new_query = parser_new_node (parser, PT_SELECT);
  new_query->info.query.q.select.from = spec;
  new_query->info.query.is_subquery = query->info.query.is_subquery;
  return new_query;
}

/* query_result.c:403 pt_get_select_list (): the select list of a SELECT, else the one a set operation kept or that of
 * its first operand (PR#8489 fourth commit: the fold reads the select list that names the columns of the set
 * operation, and of its empty operand) -- the arms here are SELECTs without one */
PT_NODE *
pt_get_select_list (PARSER_CONTEXT * parser, PT_NODE * query)
{
  if (query == NULL)
    {
      return NULL;
    }
  if (query->node_type == PT_SELECT)
    {
      return query->info.query.q.select.list;
    }
  if (query->node_type == PT_UNION || query->node_type == PT_DIFFERENCE || query->node_type == PT_INTERSECTION)
    {
      return (query->info.query.q.union_.select_list != NULL ? query->info.query.q.union_.select_list
	      : pt_get_select_list (parser, query->info.query.q.union_.arg1));
    }
  return NULL;
}

/* type_checking.c:7125 pt_false_where (): an operand folded into a value is empty; the SELECTs here have no WHERE */
bool
pt_false_where (PARSER_CONTEXT * parser, PT_NODE * node)
{
  return node != NULL && node->node_type == PT_VALUE;
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
    g_order_by_checked = NULL;
    g_derived_calls = 0;
    g_derived_fails = false;
    g_query_string = NULL;
    g_query_string_len = 0;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  /* the text one parser_parse_string () call parses: two statements, as a driver's "a; b" or a csql line; the first
   * is the longer, so the second's arms (which carry the first's length) reach past the end of the text */
  char g_text[] =
    "select 'a first statement longer than the next one' as x from db_root where 1 = 1;"
    "select c from t where c > ? union all select c from t where 1 = 0";
  const int g_len1 = (int) (strchr (g_text, ';') - g_text);	/* the first statement, up to the ';' */
  char *const g_stmt2 = g_text + g_len1 + 1;
  const int g_len2 = (int) strlen (g_stmt2);

  /* the text of another parser (a view's query spec parsed while the statement compiles -- sm_virtual_queries () on a
   * cache miss calls parser_parse_string (), and parser_main () leaves the globals on that text) */
  char g_other[] = "select a from t2";

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

    /* csql_grammar.y:1844 the statement starts: g_query_string at its first byte, g_query_string_len kept */
    void start (char *text)
    {
      g_query_string = text;
    }

    /* csql_grammar.y:1919-1922 the statement ends: the top node gets the length, and so does g_query_string_len */
    void end (PT_NODE * top, int len)
    {
      top->sql_user_text_len = len;
      g_query_string_len = len;
    }

    PT_NODE *ival (int x)
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_INTEGER;
      v->info.value.data_value.i = x;
      return v;
    }

    PT_NODE *select (int line)
    {
      PT_NODE *s = parser_new_node (parser, PT_SELECT);
      s->line_number = line;
      s->column_number = 1;
      return s;
    }

    /* csql_grammar.y set operation rule: PT_UNION over two queries (all) */
    PT_NODE *union_all (PT_NODE * a1, PT_NODE * a2)
    {
      PT_NODE *u = parser_new_node (parser, PT_UNION);
      u->info.query.q.union_.arg1 = a1;
      u->info.query.q.union_.arg2 = a2;
      u->info.query.all_distinct = PT_ALL;
      a1->info.query.is_subquery = PT_IS_UNION_SUBQUERY;
      a2->info.query.is_subquery = PT_IS_UNION_SUBQUERY;
      u->line_number = 7;
      u->column_number = 3;
      u->alias_print = "u_alias";
      return u;
    }
  };
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("F1 fold to the second arm: the arm that replaces the statement takes its text and length",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (2);
  PT_NODE *u = s.union_all (a1, a2);
  s.end (u, g_len1);
  PT_NODE *next = s.select (9);
  u->next = next;

  /* the arms carry the first statement's start and no length of their own */
  REQUIRE (a2->sql_user_text == g_text);
  REQUIRE (a2->sql_user_text_len == 0);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG2);
  REQUIRE (r == a2);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == g_len1);
  CHECK (r->line_number == 7);
  CHECK (r->column_number == 3);
  CHECK (strcmp (r->alias_print, "u_alias") == 0);
  CHECK (r->next == next);
  CHECK (g_order_by_checks == 1);
  CHECK (g_derived_calls == 0);
}

TEST_CASE ("F2 fold to the first arm: the same for STATEMENT_SET_FOLD_AS_ARG1", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (2);
  PT_NODE *u = s.union_all (a1, a2);
  s.end (u, g_len1);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r == a1);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == g_len1);
  CHECK (r->next == NULL);
}

TEST_CASE ("F3 the second statement of a text: its arms carry the first statement's length, the fold gives the "
	   "statement's own", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  /* statement 1 parsed and ended */
  s.start (g_text);
  PT_NODE *first = s.select (1);
  s.end (first, g_len1);
  /* statement 2 */
  s.start (g_stmt2);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (1);
  PT_NODE *u = s.union_all (a1, a2);
  s.end (u, g_len2);

  REQUIRE (a1->sql_user_text == g_stmt2);
  REQUIRE (a1->sql_user_text_len == g_len1);	/* the previous statement's length: develop sends that many bytes */
  REQUIRE (g_len1 > g_len2);	/* ... which runs past the end of this, the last statement */

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r == a1);
  CHECK (r->sql_user_text == g_stmt2);
  CHECK (r->sql_user_text_len == g_len2);
}

TEST_CASE ("F4 a set operation that is a subquery: it and its arms carry the same text and length, nothing changes",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (1);
  PT_NODE *u = s.union_all (a1, a2);	/* not the top node: the statement end gives its length to another node */
  PT_NODE *top = s.select (1);
  s.end (top, g_len1);

  REQUIRE (u->sql_user_text_len == 0);
  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG2);
  REQUIRE (r == a2);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == 0);
}

TEST_CASE ("F5 fold to NULL: the NULL value takes the set operation's text, not the text the globals point at",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (1);
  PT_NODE *u = s.union_all (a1, a2);
  s.end (u, g_len1);
  /* another parse has run since (a view's query spec): parser_new_node () would stamp that text */
  g_query_string = g_other;
  g_query_string_len = (int) strlen (g_other);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_NULL);
  REQUIRE (r != NULL);
  REQUIRE (r->node_type == PT_VALUE);
  CHECK (r->type_enum == PT_TYPE_NULL);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == g_len1);
  CHECK (r->line_number == 7);
}

namespace
{
  /* the fold makes a derived table when the arm has LIMIT (or ORDERBY_NUM) and the set operation has LIMIT or ORDER BY
   * (semantic_check.c:19167) */
  PT_NODE *derived_case (stmt & s, PT_NODE ** arm, PT_NODE ** u_limit, PT_NODE ** u_order)
  {
    s.start (g_stmt2);
    PT_NODE *a1 = s.select (1);
    a1->info.query.limit = s.ival (2);
    PT_NODE *a2 = s.select (1);
    PT_NODE *u = s.union_all (a1, a2);
    *u_limit = u->info.query.limit = s.ival (1);
    u->info.query.flag.rewrite_limit = 1;
    *u_order = u->info.query.order_by = parser_new_node (s.parser, PT_SORT_SPEC);
    s.end (u, g_len2);
    *arm = a1;
    return u;
  }
}

TEST_CASE ("F6 fold to a derived table, nothing parsed since the statement: the new node has the statement's text "
	   "either way (the globals still point at it)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *arm, *u_limit, *u_order;
  PT_NODE *u = derived_case (s, &arm, &u_limit, &u_order);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r != NULL);
  REQUIRE (g_derived_calls == 1);
  REQUIRE (r != arm);
  REQUIRE (r->node_type == PT_SELECT);
  CHECK (r->info.query.q.select.from->info.spec.derived_table == arm);
  CHECK (r->sql_user_text == g_stmt2);
  CHECK (r->sql_user_text_len == g_len2);
}

TEST_CASE ("F7 fold to a derived table after another parse: the new node takes the set operation's text, not the "
	   "globals'", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *arm, *u_limit, *u_order;
  PT_NODE *u = derived_case (s, &arm, &u_limit, &u_order);
  g_query_string = g_other;
  g_query_string_len = (int) strlen (g_other);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r != NULL);
  REQUIRE (r != arm);
  CHECK (r->sql_user_text == g_stmt2);
  CHECK (r->sql_user_text_len == g_len2);
}

TEST_CASE ("F8 fold to a derived table: the set operation's ORDER BY, LIMIT and rewrite flag move to the new node, "
	   "which the ORDER BY check sees", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *arm, *u_limit, *u_order;
  PT_NODE *u = derived_case (s, &arm, &u_limit, &u_order);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r != NULL);
  CHECK (r->info.query.limit == u_limit);
  CHECK (r->info.query.flag.rewrite_limit == 1);
  CHECK (r->info.query.order_by == u_order);
  CHECK (g_order_by_checks == 1);
  CHECK (g_order_by_checked == r);
  CHECK (arm->info.query.limit != NULL);	/* the arm keeps its own LIMIT inside the derived table */
}

TEST_CASE ("F11 a UNION without ALL folds to a SELECT DISTINCT from a derived table of the operand left, which "
	   "takes the set operation's text and LIMIT (PR#8489 fourth commit)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (2);
  PT_NODE *u = s.union_all (a1, a2);
  u->info.query.all_distinct = PT_DISTINCT;
  PT_NODE *u_limit = u->info.query.limit = s.ival (1);
  s.end (u, g_len1);

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG2);
  REQUIRE (r != NULL);
  REQUIRE (g_derived_calls == 1);
  REQUIRE (r->node_type == PT_SELECT);
  CHECK (r->info.query.all_distinct == PT_DISTINCT);
  CHECK (r->info.query.q.select.from->info.spec.derived_table == a2);
  CHECK (a2->info.query.all_distinct != PT_DISTINCT);	/* the block of the operand stays as it is */
  CHECK (r->info.query.limit == u_limit);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == g_len1);
  CHECK (g_order_by_checks == 1);
}

TEST_CASE ("F9 nothing to fold: the set operation comes back as it was, next relinked", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  s.start (g_text);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (1);
  PT_NODE *u = s.union_all (a1, a2);
  s.end (u, g_len1);
  PT_NODE *next = s.select (9);
  u->next = next;

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_NOTHING);
  REQUIRE (r == u);
  CHECK (r->sql_user_text == g_text);
  CHECK (r->sql_user_text_len == g_len1);
  CHECK (r->next == next);
  CHECK (r->info.query.q.union_.arg1 == a1);
  CHECK (g_order_by_checks == 0);
}

TEST_CASE ("F10 the derived table cannot be made: NULL with the parser error left", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *arm, *u_limit, *u_order;
  PT_NODE *u = derived_case (s, &arm, &u_limit, &u_order);
  g_derived_fails = true;

  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  CHECK (r == NULL);
  CHECK (pt_has_error (s.parser));
}

TEST_CASE ("O1 a set operation whose text differs from its arm's: the fold keeps the set operation's (the node it "
	   "replaces), not the arm's", "[contract]")
{
  /* the shape of a set operation made after parsing: pt_union () (parser_support.c:258-263) takes the globals through
   * parser_new_node (), while each arm is a parser_copy_tree () of the statement (view_transform.c
   * mq_substitute_subquery_in_statement) and keeps the statement's text -- the fold now takes the set operation's */
  REQUIRE (boot ());
  stmt s;
  s.start (g_stmt2);
  PT_NODE *a1 = s.select (1);
  PT_NODE *a2 = s.select (1);
  a1->sql_user_text_len = g_len2;
  a2->sql_user_text_len = g_len2;
  g_query_string = g_other;
  g_query_string_len = (int) strlen (g_other);
  PT_NODE *u = s.union_all (a1, a2);

  REQUIRE (u->sql_user_text == g_other);
  PT_NODE *r = pt_fold_union (s.parser, u, STATEMENT_SET_FOLD_AS_ARG1);
  REQUIRE (r == a1);
  CHECK (r->sql_user_text == g_other);
  CHECK (r->sql_user_text_len == (int) strlen (g_other));
}
