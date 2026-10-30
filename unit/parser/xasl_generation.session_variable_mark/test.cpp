/*
 * PR#8344 (CBRD-27144) -- pt_mark_peekable_session_variables () of src/parser/xasl_generation.c (CS_MODE), reached
 * through its one caller pt_type_session_variables () (parser.h), which mq_translate () runs on a statement compiled
 * at every execution (view_transform.c:10198-10205).
 *
 * The contract read off the walker and the caller (xasl_generation.c:7647-7674, 7793-7795):
 *   - a read `@v` (PT_EVALUATE_VARIABLE) gets PT_EXPR_INFO_PEEK_SESSION_VARIABLE unless one of the statement's
 *     assignments `@v := ...` (PT_DEFINE_VARIABLE, collected by pt_collect_session_variables) names the same variable
 *   - the names compare case-insensitively (intl_identifier_casecmp): `@V` is `@v`
 *   - the scope is the whole statement: an assignment in another query block, a subquery or the other side of a
 *     UNION keeps every read of that variable unmarked
 *   - only reads are marked: an assignment node, an operator over a variable and the other expressions keep their
 *     flags; the mark does not depend on the session having the variable (an undefined one is still marked -- the
 *     optimizer then finds no value, query_planner.session_variable_peek K4)
 *   - the walk follows every list the tree walker follows (next, or_next, subqueries)
 *
 * Real code: xasl_generation.c (the TU: pt_type_session_variables, the three walkers, pt_session_variable_name /
 * _index / _expr_domain / _start_domain / _stored_domain / _same_type), parse_tree.c + parse_tree_cl.c
 * (parser_create_parser, parser_new_node, parser_alloc, pt_append_bytes / _string, parser_walk_tree),
 * language_support.c + intl_support.c + chartype.c (intl_identifier_casecmp under en_US.utf8), object_domain.c +
 * object_primitive.c + domain_rules.c (tp_Null_domain, domain_value_domain of an INTEGER session value), db_macro.c,
 * memory_alloc.c, system_parameter.c.
 * Fakes (see suite.json "fakes"): db_get_variable (the session), er_* (the error context), pt_value_to_db (the
 * PT_TYPE_CHAR branch), pt_parser_line_col (no lexer), init_builtin_calendar_names / db_date_locale_init
 * (locale side tables nothing here reads), envvar_get / envvar_trim_char, cubregex::get_engine_name.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "dbi.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "memory_alloc.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "string_regex.hpp"
#include "tz_support.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined (PT_EXPR_INFO_PEEK_SESSION_VARIABLE)
#define UT_HEAD_HAS_MARK 1
#else
/* base: the bit the PR gives the mark (parse_tree.h 0x8000000) is free there; no code sets it */
#define UT_HEAD_HAS_MARK 0
#define PT_EXPR_INFO_PEEK_SESSION_VARIABLE 134217728
#endif

/* ---------------------------------------------------------------- fakes */

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

/* string_opfunc.c: lang_init_builtin () gives each built-in locale its calendar names; identifier comparison does not
 * read them */
void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

/* db_date.c: lang_set_charset_lang () refreshes the local AM/PM strings of date parsing; not read here */
void
db_date_locale_init (void)
{
}

/* scanner_support.c pt_parser_line_col () (C linkage, csql_grammar_scan.h): parser_new_node () stamps the lexer's
 * token position on a node; there is no lexer here, so the position is 0 */
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

/* name_resolution.c pt_free_dblink_remote_cols (): parser_free_parser () frees the dblink column lists; a parser that
 * resolved no dblink table has none (parser->dblink_remote NULL), and the real loop does nothing */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

/* error_context.cpp: a base level and a stack of levels; push makes a new empty level on top (err_id NO_ERROR), pop
 * drops the top, er_set / er_errid / er_clear work on the current level (the top, else the base) */
namespace
{
  int g_er_base = NO_ERROR;
  int g_er_stack[64];
  int g_er_depth = 0;

  int &er_current ()
  {
    return g_er_depth > 0 ? g_er_stack[g_er_depth - 1] : g_er_base;
  }
}

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  er_current () = err_id;
}

int
er_errid (void)
{
  return er_current ();
}

void
er_clear (void)
{
  er_current () = NO_ERROR;
}

void
er_stack_push (void)
{
  REQUIRE (g_er_depth < 64);
  g_er_stack[g_er_depth++] = NO_ERROR;
}

void
er_stack_pop (void)
{
  REQUIRE (g_er_depth > 0);
  g_er_depth--;
}

/* parse_dbi.c pt_value_to_db (): NULL for no node or not a constant (pt_is_const); for a PT_VALUE the node's own
 * DB_VALUE, made on the first call and kept (db_value_is_initialized) -- pt_db_value_initialize's PT_TYPE_CHAR branch:
 * db_make_char over the string's bytes, TP_FLOATING_PRECISION_VALUE, LANG_COERCIBLE_CODESET / LANG_COERCIBLE_COLL.
 * A session variable's name is such a PT_VALUE (csql_grammar.y session_variable). */
DB_VALUE *
pt_value_to_db (PARSER_CONTEXT * parser, PT_NODE * value)
{
  if (value == NULL || !pt_is_const (value) || value->node_type != PT_VALUE)
    {
      return NULL;
    }
  DB_VALUE *db_value = &value->info.value.db_value;
  if (value->info.value.db_value_is_initialized)
    {
      return db_value;
    }
  PARSER_VARCHAR *str = value->info.value.data_value.str;
  switch (value->type_enum)
    {
    case PT_TYPE_CHAR:
      db_make_char (db_value, TP_FLOATING_PRECISION_VALUE, (char *) str->bytes, str->length, LANG_COERCIBLE_CODESET,
		    LANG_COERCIBLE_COLL);
      break;
    default:
      return NULL;
    }
  value->info.value.db_value_is_initialized = true;
  return db_value;
}

/* db_admin.c db_get_variable () -> csession_get_variable () (CS_MODE) -> session.c session_get_variable (): a variable
 * the session has (name compared by intl_identifier_casecmp) -> NO_ERROR and its value; otherwise the server's
 * ER_SES_VARIABLE_NOT_FOUND in the client's error area and ER_FAILED, the value untouched. The session here holds
 * INTEGER values (`SET @v = 0`), which is all the typing needs. */
namespace
{
  struct session_var
  {
    const char *name;
    int value;
  };
  std::vector < session_var > g_session;
  int g_requests = 0;
}

int
db_get_variable (DB_VALUE * name, DB_VALUE * value)
{
  const char *name_str = db_get_string (name);
  g_requests++;
  for (const session_var & v:g_session)
    {
      if (intl_identifier_casecmp (v.name, name_str) == 0)
	{
	  db_make_int (value, v.value);
	  return NO_ERROR;
	}
    }
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SES_VARIABLE_NOT_FOUND, 1, name_str);
  return ER_FAILED;
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
	/* lang_init () would also load user locales; lang_set_charset_lang () only asserts lang_Initialized */
	if (lang_set_charset_lang ("en_US.utf8") != NO_ERROR)
	  {
	    return false;
	  }
      }
    g_session.clear ();
    g_requests = 0;
    g_er_base = NO_ERROR;
    g_er_depth = 0;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  /* one statement, its nodes made by the parser as csql_grammar.y makes them (parser_new_node, then the fields the
   * grammar action sets) */
  struct stmt
  {
    PARSER_CONTEXT *parser;
    std::vector < PT_NODE * >reads;	/* every PT_EVALUATE_VARIABLE made, in order */
    std::vector < PT_NODE * >defines;	/* every PT_DEFINE_VARIABLE made */
    std::vector < PT_NODE * >others;	/* every other PT_EXPR made */

      stmt ()
    {
      parser = parser_create_parser ();
    }
     ~stmt ()
    {
      parser_free_parser (parser);
    }

    /* csql_grammar.y session_variable: a CHAR PT_VALUE over the identifier's bytes */
    PT_NODE *name_value (const char *name)
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_CHAR;
      v->info.value.string_type = ' ';
      v->info.value.data_value.str = pt_append_bytes (parser, NULL, name, (int) strlen (name));
      v->info.value.text = (const char *) v->info.value.data_value.str->bytes;
      return v;
    }

    /* parser_make_expression () */
    PT_NODE *expr (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL, PT_NODE * a3 = NULL)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->info.expr.arg3 = a3;
      if (op != PT_EVALUATE_VARIABLE && op != PT_DEFINE_VARIABLE)
	{
	  others.push_back (e);
	}
      return e;
    }

    /* session_variable_expression: `@name` */
    PT_NODE *read (const char *name)
    {
      PT_NODE *e = expr (PT_EVALUATE_VARIABLE, name_value (name));
      e->flag.do_not_fold = 1;
      reads.push_back (e);
      return e;
    }

    /* session_variable_definition: `@name := value` */
    PT_NODE *assign (const char *name, PT_NODE * value)
    {
      PT_NODE *e = expr (PT_DEFINE_VARIABLE, name_value (name), value);
      e->flag.do_not_fold = 1;
      defines.push_back (e);
      return e;
    }

    PT_NODE *col (const char *name)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.resolved = "t";
      n->type_enum = PT_TYPE_INTEGER;
      return n;
    }

    PT_NODE *int_value (int x)
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_INTEGER;
      v->info.value.data_value.i = x;
      return v;
    }

    PT_NODE *spec (const char *table)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, table);
      s->info.spec.entity_name = n;
      return s;
    }

    PT_NODE *select (PT_NODE * list, PT_NODE * from, PT_NODE * where = NULL)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = list;
      q->info.query.q.select.from = from;
      q->info.query.q.select.where = where;
      return q;
    }

    PT_NODE *union_all (PT_NODE * a, PT_NODE * b)
    {
      PT_NODE *u = parser_new_node (parser, PT_UNION);
      u->info.query.q.union_.arg1 = a;
      u->info.query.q.union_.arg2 = b;
      u->info.query.all_distinct = PT_ALL;
      return u;
    }

    /* `count(pad)` of the examples: an aggregate over a column (its node kind does not matter to the walk) */
    PT_NODE *count_pad ()
    {
      PT_NODE *f = parser_new_node (parser, PT_FUNCTION);
      f->info.function.function_type = PT_COUNT;
      f->info.function.arg_list = col ("pad");
      return f;
    }

    /* a WHERE list of conjuncts (next-linked, as the CNF the rewrites leave) */
    static PT_NODE *conj (std::initializer_list < PT_NODE * >terms)
    {
      PT_NODE *head = NULL, *tail = NULL;
      for (PT_NODE * t:terms)
	{
	  if (head == NULL)
	    {
	      head = t;
	    }
	  else
	    {
	      tail->next = t;
	    }
	  tail = t;
	}
      return head;
    }

    int type (PT_NODE * statement)
    {
      return pt_type_session_variables (parser, statement);
    }
  };

  bool marked (PT_NODE * e)
  {
    return PT_EXPR_INFO_IS_FLAGED (e, PT_EXPR_INFO_PEEK_SESSION_VARIABLE) != 0;
  }

  /* the reads of one variable (by the spelling given when made) */
  std::vector < PT_NODE * >reads_of (const stmt & s, const char *name)
  {
    std::vector < PT_NODE * >out;
    for (PT_NODE * r:s.reads)
      {
	if (intl_identifier_casecmp ((const char *) r->info.expr.arg1->info.value.data_value.str->bytes, name) == 0)
	  {
	    out.push_back (r);
	  }
      }
    return out;
  }

  bool no_mark_outside_reads (const stmt & s)
  {
    for (PT_NODE * e:s.defines)
      {
	if (marked (e))
	  {
	    return false;
	  }
      }
    for (PT_NODE * e:s.others)
      {
	if (marked (e))
	  {
	    return false;
	  }
      }
    return true;
  }
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("PR8344 M1: control -- `select count(pad) from t where c1 > @v`: the statement does not assign @v, the "
	   "read is marked (develop marks nothing)", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  stmt s;
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), s.expr (PT_GT, s.col ("c1"), s.read ("v")));

  REQUIRE (s.type (q) == NO_ERROR);
  CHECK (marked (s.reads[0]));
  CHECK (no_mark_outside_reads (s));
  /* the typing asked the session once for the variable, on its own error level */
  CHECK (g_requests == 1);
  CHECK (g_er_depth == 0);
}

TEST_CASE ("PR8344 M2: `where c1 > @v and (@v := @v) is not null` -- the statement assigns @v: no read of @v is "
	   "marked, in the predicate or in the assignment", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  stmt s;
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("v")),
				 s.expr (PT_IS_NOT_NULL, s.assign ("v", s.read ("v"))) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (s.reads.size () == 2);
  CHECK_FALSE (marked (s.reads[0]));
  CHECK_FALSE (marked (s.reads[1]));
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M3: the scope is the statement -- an assignment only in the other UNION ALL block keeps the first "
	   "block's read unmarked", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  stmt s;
  /* select count(pad) from t where c1 > @v union all select @v := @v from db_root */
  PT_NODE *first = s.select (s.count_pad (), s.spec ("t"), s.expr (PT_GT, s.col ("c1"), s.read ("v")));
  PT_NODE *second = s.select (s.assign ("v", s.read ("v")), s.spec ("db_root"));
  PT_NODE *q = s.union_all (first, second);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (s.reads.size () == 2);
  CHECK_FALSE (marked (s.reads[0]));
  CHECK_FALSE (marked (s.reads[1]));
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M3 boundary: an assignment inside a subquery of the WHERE keeps the outer read unmarked; a "
	   "subquery's read of a variable no one assigns is marked", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  g_session.push_back ({ "w", 7 });
  stmt s;
  /* select count(pad) from t
   *   where c1 > @v and c1 < (select @v := @v from db_root) and c1 <> (select @w from db_root) */
  PT_NODE *sub_assign = s.select (s.assign ("v", s.read ("v")), s.spec ("db_root"));
  PT_NODE *sub_read = s.select (s.read ("w"), s.spec ("db_root"));
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("v")),
				 s.expr (PT_LT, s.col ("c1"), sub_assign),
				 s.expr (PT_NE, s.col ("c1"), sub_read) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  for (PT_NODE * r:reads_of (s, "v"))
    {
      CHECK_FALSE (marked (r));
    }
  REQUIRE (reads_of (s, "w").size () == 1);
  CHECK (marked (reads_of (s, "w")[0]));
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M4: names compare case-insensitively -- `@V` is the `@v` the statement assigns; the read of @w the "
	   "assignment takes its value from is marked", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  g_session.push_back ({ "w", 5 });
  stmt s;
  /* where c1 > @V and (@v := @w) is not null */
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("V")),
				 s.expr (PT_IS_NOT_NULL, s.assign ("v", s.read ("w"))) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (s.reads.size () == 2);
  CHECK_FALSE (marked (s.reads[0]));
  CHECK (marked (s.reads[1]));
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M5: two variables, one assigned -- `where c1 > @a and c1 < @b and (@B := @b) is not null`: @a "
	   "marked, every read of @b not", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "a", 1 });
  g_session.push_back ({ "b", 2 });
  stmt s;
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("a")),
				 s.expr (PT_LT, s.col ("c1"), s.read ("b")),
				 s.expr (PT_IS_NOT_NULL, s.assign ("B", s.read ("b"))) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (reads_of (s, "a").size () == 1);
  CHECK (marked (reads_of (s, "a")[0]));
  REQUIRE (reads_of (s, "b").size () == 2);
  for (PT_NODE * r:reads_of (s, "b"))
    {
      CHECK_FALSE (marked (r));
    }
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M5 boundary: two assignments of two variables -- `(@a := @a) is not null and (@b := @b) is not null` "
	   "with reads of both in the predicate: no read of either is marked, whatever the assignment's place in the list",
	   "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "a", 1 });
  g_session.push_back ({ "b", 2 });
  g_session.push_back ({ "c", 3 });
  stmt s;
  /* where c1 > @a and c1 < @b and c1 <> @c and (@a := @a) is not null and (@b := @b) is not null */
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("a")),
				 s.expr (PT_LT, s.col ("c1"), s.read ("b")),
				 s.expr (PT_NE, s.col ("c1"), s.read ("c")),
				 s.expr (PT_IS_NOT_NULL, s.assign ("a", s.read ("a"))),
				 s.expr (PT_IS_NOT_NULL, s.assign ("b", s.read ("b"))) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (reads_of (s, "a").size () == 2);
  REQUIRE (reads_of (s, "b").size () == 2);
  for (PT_NODE * r:reads_of (s, "a"))
    {
      CHECK_FALSE (marked (r));
    }
  for (PT_NODE * r:reads_of (s, "b"))
    {
      CHECK_FALSE (marked (r));
    }
  REQUIRE (reads_of (s, "c").size () == 1);
  CHECK (marked (reads_of (s, "c")[0]));
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M6 boundary: a variable the session does not have is still marked -- the mark is about the "
	   "statement's assignments, not the value", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  /* drop variable @u; select count(pad) from t where c1 > @u */
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), s.expr (PT_GT, s.col ("c1"), s.read ("u")));

  REQUIRE (s.type (q) == NO_ERROR);
  CHECK (marked (s.reads[0]));
  CHECK (g_requests == 1);
  /* the failed request left no error behind (pt_session_start_domain pushes and pops) */
  CHECK (er_errid () == NO_ERROR);
  CHECK (g_er_depth == 0);
}

TEST_CASE ("PR8344 M7 boundary: the walk follows OR lists (or_next) and the select list -- every read of an "
	   "unassigned variable there is marked", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  g_session.push_back ({ "w", 9 });
  stmt s;
  /* select @w, count(pad) from t where (c1 > @v or c1 < @w) */
  PT_NODE *disj = s.expr (PT_GT, s.col ("c1"), s.read ("v"));
  disj->or_next = s.expr (PT_LT, s.col ("c1"), s.read ("w"));
  PT_NODE *list = s.read ("w");
  list->next = s.count_pad ();
  PT_NODE *q = s.select (list, s.spec ("t"), disj);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (s.reads.size () == 3);
  for (PT_NODE * r:s.reads)
    {
      CHECK (marked (r));
    }
  CHECK (no_mark_outside_reads (s));
}

TEST_CASE ("PR8344 M8 boundary: a statement without session variables is left as it was", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.int_value (0)),
				 s.expr (PT_LT, s.col ("c1"), s.int_value (10)) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  CHECK (s.reads.empty ());
  CHECK (no_mark_outside_reads (s));
  CHECK (g_requests == 0);
  CHECK (s.parser->session_variable_count == 0);
}

TEST_CASE ("PR8344 M9 develop: the typing the walk follows is develop's -- one slot per variable whatever its "
	   "spelling, assigned exactly for the assigned one, the session's INTEGER type", "[contract]")
{
  REQUIRE (boot ());
  g_session.push_back ({ "v", 0 });
  g_session.push_back ({ "w", 3 });
  stmt s;
  /* where c1 > @v and c1 < @W and (@V := @v) is not null */
  PT_NODE *where = stmt::conj ({ s.expr (PT_GT, s.col ("c1"), s.read ("v")),
				 s.expr (PT_LT, s.col ("c1"), s.read ("W")),
				 s.expr (PT_IS_NOT_NULL, s.assign ("V", s.read ("v"))) });
  PT_NODE *q = s.select (s.count_pad (), s.spec ("t"), where);

  REQUIRE (s.type (q) == NO_ERROR);
  REQUIRE (s.parser->session_variable_count == 2);
  int v = -1, w = -1;
  for (int i = 0; i < s.parser->session_variable_count; i++)
    {
      if (intl_identifier_casecmp (s.parser->session_variable_names[i], "v") == 0)
	{
	  v = i;
	}
      if (intl_identifier_casecmp (s.parser->session_variable_names[i], "w") == 0)
	{
	  w = i;
	}
    }
  REQUIRE (v >= 0);
  REQUIRE (w >= 0);
  CHECK (s.parser->session_variable_assigned[v]);
  CHECK_FALSE (s.parser->session_variable_assigned[w]);
  CHECK (TP_DOMAIN_TYPE (s.parser->session_variable_domains[v]) == DB_TYPE_INTEGER);
  CHECK (TP_DOMAIN_TYPE (s.parser->session_variable_domains[w]) == DB_TYPE_INTEGER);
}
