/*
 * PR#8669 (CBRD-28295) -- pt_check_cast_op () of src/parser/semantic_check.c (CS_MODE) when its operand is itself a
 * CAST. Callers: pt_eval_expr_type () (type_checking.c:10892, type evaluation) and pt_semantic_check_local ()
 * (semantic_check.c:11975, semantic check).
 *
 * A COLLATE modifier is parsed into a CAST flagged PT_EXPR_INFO_CAST_COLL_MODIFIER with no cast type
 * (csql_grammar.y:25918-25925); type evaluation gives it one from its operand (type_checking.c:10852-10883). On a
 * non-string operand it gets none and an error is set, but inside a collection constructor, an IN list or = ANY {...}
 * the type walk goes on (pt_eval_type () does not stop at errors, type_checking.c:8177) and the outer CAST is checked
 * with that operand. develop read arg1->info.expr.cast_type->type_enum there (NULL -- SIGSEGV).
 *
 * The contract the PR sets (semantic_check.c:1174-1187): an operand CAST without a cast type makes the check fail
 * (false); the error already set is kept, and with none an internal 'null cast type' error is set. An operand CAST with
 * a cast type is read as before.
 *
 * Real code: semantic_check.c (the TU: pt_check_cast_op), parse_tree.c + parse_tree_cl.c (parser_create_parser,
 * parser_new_node, pt_record_error, pt_internal_error, parser_free_parser), language_support.c + intl_support.c +
 * chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Fakes: the same as semantic_check.fold_union_text (the TU needs them to link; none is read by these cases).
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

    PT_NODE *dt (PT_TYPE_ENUM t)
    {
      PT_NODE *d = parser_new_node (parser, PT_DATA_TYPE);
      d->type_enum = t;
      return d;
    }

    /* an INT column reference: type INTEGER, no data type (primitive) */
    PT_NODE *int_name ()
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->type_enum = PT_TYPE_INTEGER;
      n->info.name.original = "c1";
      return n;
    }

    /* csql_grammar.y:25918-25925: (arg COLLATE utf8_bin) -- a flagged CAST, no cast type yet */
    PT_NODE *coll_cast (PT_NODE * arg)
    {
      PT_NODE *c = parser_new_node (parser, PT_EXPR);
      c->info.expr.op = PT_CAST;
      c->info.expr.arg1 = arg;
      c->info.expr.cast_type = NULL;
      PT_EXPR_INFO_SET_FLAG (c, PT_EXPR_INFO_CAST_COLL_MODIFIER);
      PT_SET_NODE_COLL_MODIFIER (c, LANG_COLL_UTF8_BINARY);
      return c;
    }

    /* CAST (arg AS t) */
    PT_NODE *cast (PT_NODE * arg, PT_TYPE_ENUM t)
    {
      PT_NODE *c = parser_new_node (parser, PT_EXPR);
      c->info.expr.op = PT_CAST;
      c->info.expr.arg1 = arg;
      c->info.expr.cast_type = dt (t);
      return c;
    }
  };
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("K1 CAST ((c1 COLLATE utf8_bin) AS CHAR) after the COLLATE error: false, the error already set is kept",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *inner = s.coll_cast (s.int_name ());
  PT_NODE *outer = s.cast (inner, PT_TYPE_CHAR);
  /* the error type evaluation set on the inner CAST ('cast ' operator is not defined on type integer) */
  pt_record_error (s.parser, 0, 1, 1, "'cast ' operator is not defined on type integer.", NULL);
  PT_NODE *first = s.parser->error_msgs;
  REQUIRE (first != NULL);

  CHECK_FALSE (pt_check_cast_op (s.parser, outer));
  CHECK (s.parser->error_msgs == first);
  CHECK (first->next == NULL);
  CHECK_FALSE (s.parser->flag.has_internal_error);
}

TEST_CASE ("K2 an operand CAST without a cast type and no error set: false with an internal error", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *outer = s.cast (s.coll_cast (s.int_name ()), PT_TYPE_VARCHAR);
  REQUIRE_FALSE (pt_has_error (s.parser));

  CHECK_FALSE (pt_check_cast_op (s.parser, outer));
  CHECK (pt_has_error (s.parser));
}

TEST_CASE ("K3 an operand CAST with a cast type is read as before: CAST (CAST (c1 AS INTEGER) AS VARCHAR) is valid",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *outer = s.cast (s.cast (s.int_name (), PT_TYPE_INTEGER), PT_TYPE_VARCHAR);

  CHECK (pt_check_cast_op (s.parser, outer));
  CHECK_FALSE (pt_has_error (s.parser));
}
