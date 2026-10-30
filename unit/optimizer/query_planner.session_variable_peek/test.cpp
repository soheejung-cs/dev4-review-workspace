/*
 * PR#8344 (CBRD-27144) -- qo_session_variable_value () of src/optimizer/query_planner.c (CS_MODE): the value a
 * histogram probe estimates a session variable read with. Reached through appendix hooks (hooks.cpp), together with
 * its two consumers -- qo_between_range_arg_value () (the PR's PC_OTHER branch, K8) and qo_equal_selectivity ()
 * (`attr = @v`, K9) -- and the release in qo_env_free () (query_graph.c, K6).
 *
 * The contract read off the function and its header comment (query_planner.c:13624-13684, query_graph.h:900-911):
 *   - only a PT_EVALUATE_VARIABLE read the parser marked (PT_EXPR_INFO_PEEK_SESSION_VARIABLE) is estimated with its
 *     value; anything else gives NULL and does not ask the session (K1, K7)
 *   - the session is asked once per variable and block (QO_ENV), the name compared case-insensitively
 *     (intl_identifier_casecmp) -- `@v` and `@V` are one variable (K2)
 *   - each variable gets its own entry, never moved: a probe holds one value while it asks for another (the two
 *     bounds of a BETWEEN) (K3)
 *   - a variable the session cannot give (never set) keeps the default estimate -- NULL, no error left behind (the
 *     error stack is pushed and popped around the request) -- and is not asked again in the block (K4); so does an
 *     entry that cannot be allocated, which is not kept (K10)
 *   - a NULL value gives NULL (K5)
 *   - qo_env_free () releases every entry and its value (K6)
 * `... develop` cases hold on base too (they are registered on every revision); the others only where the PR's mark
 * exists (UT_HAS_PEEK).
 *
 * Real code: query_planner.c (the TU), query_graph.c (qo_env_free), query_bitset.c, object_primitive.c
 * (pr_clear_value), memory_alloc.c, system_parameter.c, language_support.c + intl_support.c + chartype.c
 * (intl_identifier_casecmp under en_US.utf8, as a booted client has it), db_macro.c.
 * Seam: qo_index_cardinality (the `return 0` exits -> DEFAULT_EQUAL_SELECTIVITY).
 * Fakes (see suite.json "fakes"): db_get_variable (the session), er_* (the error context), pt_value_to_db (the
 * PT_TYPE_CHAR branch), pt_is_function_index_expression (verbatim), histogram_get_equal_selectivity (its exits),
 * db_ws_alloc / db_ws_free (quick_fit.c's utility-thread branch: malloc / free), tp_apply_sys_charset (type module
 * not initialized), init_builtin_calendar_names / db_date_locale_init, envvar_get / envvar_trim_char,
 * cubregex::get_engine_name.
 * malloc / calloc / realloc / free are interposed only to list the blocks allocated inside a window (K6) and to fail
 * one allocation of an entry's size (K10).
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "query_bitset.h"
#include "dbi.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "memory_alloc.h"
#include "quick_fit.h"
#include "object_primitive.h"
#include "string_regex.hpp"
#include "tz_support.h"
#include "object_domain.h"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#if defined (PT_EXPR_INFO_PEEK_SESSION_VARIABLE)
/* a revision with the PR: qo_session_variable_value () and the mark exist -- every case is registered */
#define UT_HAS_PEEK 1
#else
/* base: no qo_session_variable_value (); only the between-range cases develop shares are registered. The bit the PR
 * gives the mark (parse_tree.h 0x8000000) is free there */
#define UT_HAS_PEEK 0
#define PT_EXPR_INFO_PEEK_SESSION_VARIABLE 134217728
#endif

/* ---------------------------------------------------------------- allocation window (K6) */
extern "C" void *__libc_malloc (size_t);
extern "C" void *__libc_calloc (size_t, size_t);
extern "C" void *__libc_realloc (void *, size_t);
extern "C" void __libc_free (void *);

namespace
{
  bool g_track = false;
  void *g_live[4096];
  int g_nlive = 0;
  size_t g_fail_size = 0;	/* the next malloc of this size returns NULL (K10), then 0 again */

  void track_add (void *p)
  {
    if (g_track && p != NULL && g_nlive < 4096)
      {
	g_live[g_nlive++] = p;
      }
  }

  void track_del (void *p)
  {
    for (int i = 0; p != NULL && i < g_nlive; i++)
      {
	if (g_live[i] == p)
	  {
	    g_live[i] = g_live[--g_nlive];
	    return;
	  }
      }
  }
}

extern "C" void *
malloc (size_t n) __THROW
{
  if (g_fail_size != 0 && n == g_fail_size)
    {
      g_fail_size = 0;
      return NULL;
    }
  void *p = __libc_malloc (n);
  track_add (p);
  return p;
}

extern "C" void *
calloc (size_t a, size_t b) __THROW
{
  void *p = __libc_calloc (a, b);
  track_add (p);
  return p;
}

extern "C" void *
realloc (void *p, size_t n) __THROW
{
  track_del (p);
  void *q = __libc_realloc (p, n);
  track_add (q);
  return q;
}

extern "C" void
free (void *p) __THROW
{
  track_del (p);
  __libc_free (p);
}

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

/* object_domain.c tp_apply_sys_charset (): the branch of a type module not initialized (tp_Domain_area NULL) --
 * nothing here reads a cached string domain */
void
tp_apply_sys_charset (void)
{
}

/* db_date.c: lang_set_charset_lang () refreshes the local AM/PM strings of date parsing; not read here */
void
db_date_locale_init (void)
{
}

/* quick_fit.c db_ws_alloc / db_ws_free: the DB_IS_UTILITY_THREAD () branch (malloc / free); memory_alloc.c's CS_MODE
 * db_private_alloc / db_private_free call them */
void *
db_ws_alloc (size_t size)
{
  return malloc (size);
}

void
db_ws_free (void *ptr)
{
  if (ptr)
    {
      free (ptr);
    }
}

/* error_context.cpp: a base level and a stack of levels; push makes a new empty level on top (err_id NO_ERROR), pop
 * drops the top, er_set / er_errid / er_clear work on the current level (the top, else the base). Fixed storage so
 * the K6 window sees no allocation of its own. */
namespace
{
  int g_er_base = NO_ERROR;
  int g_er_stack[64];
  int g_er_depth = 0;
  int g_er_pushes = 0;
  int g_er_pops = 0;

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
  g_er_pushes++;
}

void
er_stack_pop (void)
{
  REQUIRE (g_er_depth > 0);
  g_er_depth--;
  g_er_pops++;
}

/* parse_dbi.c pt_value_to_db (): NULL for no node or not a constant (pt_is_const); for a PT_VALUE the node's own
 * DB_VALUE, made on the first call and kept (db_value_is_initialized) -- pt_db_value_initialize's PT_TYPE_CHAR /
 * PT_TYPE_VARCHAR branches: db_make_char / db_make_varchar over the string's bytes, TP_FLOATING_PRECISION_VALUE,
 * LANG_COERCIBLE_CODESET / LANG_COERCIBLE_COLL (no data_type). A session variable's name is such a PT_VALUE
 * (csql_grammar.y session_variable); host variables and PT_PARAMETER names do not reach here. */
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
    case PT_TYPE_VARCHAR:
      db_make_varchar (db_value, TP_FLOATING_PRECISION_VALUE, (char *) str->bytes, str->length,
		       LANG_COERCIBLE_CODESET, LANG_COERCIBLE_COLL);
      break;
    default:
      return NULL;
    }
  value->info.value.db_value_is_initialized = true;
  return db_value;
}

/* parser_support.c pt_is_function_index_expression () verbatim (qo_classify asks it of every PT_EXPR) */
int
pt_is_function_index_expression (PT_NODE * node)
{
  if (node == NULL || node->node_type != PT_EXPR)
    {
      return false;
    }
  if (!PT_EXPR_INFO_IS_FLAGED (node, PT_EXPR_INFO_FUNCTION_INDEX))
    {
      return false;
    }
  return true;
}

/* db_admin.c db_get_variable () -> csession_get_variable () (CS_MODE) -> session.c session_get_variable ():
 *   found (name compared by intl_identifier_casecmp) -> NO_ERROR and the value unpacked into the caller's DB_VALUE
 *     (or_unpack_db_value: a string is a copy the caller owns, need_clear set);
 *   not found -> the server's er_set (ER_SES_VARIABLE_NOT_FOUND) reaches the client's error area, ER_FAILED comes
 *     back and the value is left as it was (the client returns before unpacking).
 * Counts the requests and keeps the names asked, in the spelling the reader sent. */
namespace
{
  enum var_kind
  { V_INT, V_STRING, V_NULL, V_TYPED_NULL };

  struct session_var
  {
    const char *name;
    var_kind kind;
    int i;
    const char *s;
  };

  session_var g_session[16];
  int g_nsession = 0;
  int g_requests = 0;
  const char *g_asked[64];

  void session_reset ()
  {
    g_nsession = 0;
    g_requests = 0;
  }

  void session_set (const char *name, var_kind kind, int i = 0, const char *s = NULL)
  {
    for (int k = 0; k < g_nsession; k++)
      {
	if (intl_identifier_casecmp (g_session[k].name, name) == 0)
	  {
	    g_session[k] = session_var { name, kind, i, s };
	    return;
	  }
      }
    g_session[g_nsession++] = session_var { name, kind, i, s };
  }
}

int
db_get_variable (DB_VALUE * name, DB_VALUE * value)
{
  const char *name_str = db_get_string (name);
  if (g_requests < 64)
    {
      g_asked[g_requests] = name_str;
    }
  g_requests++;
  for (int k = 0; k < g_nsession; k++)
    {
      const session_var & v = g_session[k];
      if (intl_identifier_casecmp (v.name, name_str) != 0)
	{
	  continue;
	}
      switch (v.kind)
	{
	case V_INT:
	  db_make_int (value, v.i);
	  break;
	case V_STRING:
	  {
	    int len = (int) strlen (v.s);
	    char *buf = (char *) db_private_alloc (NULL, len + 1);
	    memcpy (buf, v.s, len + 1);
	    db_make_varchar (value, DB_MAX_VARCHAR_PRECISION, buf, len, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
	    value->need_clear = true;
	  }
	  break;
	case V_NULL:
	  db_make_null (value);
	  break;
	case V_TYPED_NULL:
	  /* a NULL that keeps its variable's type (the session stores the domain of the last value) */
	  db_value_domain_init (value, DB_TYPE_INTEGER, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
	  break;
	}
      return NO_ERROR;
    }
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SES_VARIABLE_NOT_FOUND, 1, name_str);
  return ER_FAILED;
}

/* histogram_cl.cpp histogram_get_equal_selectivity (): no value (rhs NULL) or no histogram on the column -> not
 * priced (success false); a key the column's histogram cannot take (histogram_extract_key / the kind check: a string
 * against a numeric column) -> not priced; otherwise the histogram's estimate. The column `c1` here has an INTEGER
 * histogram that gives g_hist_sel; the probe's value is recorded. */
namespace
{
  int g_hist_probes = 0;
  DB_VALUE *g_hist_value = NULL;
  double g_hist_sel = 0.25;
  int g_icard_calls = 0;
}

void
histogram_get_equal_selectivity (PT_NODE * lhs, DB_VALUE * rhs_db_value, double *selectivity, bool *success)
{
  g_hist_probes++;
  g_hist_value = rhs_db_value;
  if (rhs_db_value == NULL || lhs == NULL || lhs->node_type != PT_NAME || lhs->info.name.original == NULL
      || strcmp (lhs->info.name.original, "c1") != 0)
    {
      *success = false;
      return;
    }
  DB_TYPE t = DB_VALUE_DOMAIN_TYPE (rhs_db_value);
  if (t != DB_TYPE_INTEGER && t != DB_TYPE_BIGINT && t != DB_TYPE_SHORT)
    {
      *success = false;
      return;
    }
  *selectivity = g_hist_sel;
  *success = true;
}

/* seam qo_index_cardinality (query_planner.c, static): the `return 0` exits -- the column has no node/segment in this
 * environment, no NDV and no index statistics -- so the caller takes DEFAULT_EQUAL_SELECTIVITY */
INT64
qo_index_cardinality (QO_ENV * env, PT_NODE * attr)
{
  g_icard_calls++;
  return 0;
}

/* ---------------------------------------------------------------- hooks.cpp (appended to query_planner.c) */
int unitseam_has_session_peek (void);
DB_VALUE *unitseam_session_variable_value (QO_ENV * env, PT_NODE * node);
int unitseam_session_peek_count (QO_ENV * env);
DB_VALUE *unitseam_between_range_arg_value (QO_ENV * env, PT_NODE * arg);
double unitseam_equal_selectivity (QO_ENV * env, PT_NODE * expr);

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
    session_reset ();
    g_er_base = NO_ERROR;
    g_er_depth = 0;
    g_er_pushes = g_er_pops = 0;
    g_hist_probes = 0;
    g_hist_value = NULL;
    g_icard_calls = 0;
    g_fail_size = 0;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  PT_NODE *mk (PT_NODE_TYPE t)
  {
    PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
    n->node_type = t;
    n->type_enum = PT_TYPE_NONE;
    return n;
  }

  /* `@name`'s name as csql_grammar.y session_variable makes it: a CHAR PT_VALUE over the identifier's bytes */
  PT_NODE *name_value (const char *name, PT_TYPE_ENUM type = PT_TYPE_CHAR)
  {
    PT_NODE *v = mk (PT_VALUE);
    size_t n = strlen (name);
    PARSER_VARCHAR *s = (PARSER_VARCHAR *) calloc (1, offsetof (PARSER_VARCHAR, bytes) + n + 1);
    s->length = (int) n;
    memcpy (s->bytes, name, n);
    v->type_enum = type;
    v->info.value.string_type = ' ';
    v->info.value.data_value.str = s;
    v->info.value.text = (const char *) s->bytes;
    return v;
  }

  /* a read `@name` (session_variable_expression: PT_EVALUATE_VARIABLE over the name, do_not_fold), marked or not */
  PT_NODE *read_var (const char *name, bool marked)
  {
    PT_NODE *e = mk (PT_EXPR);
    e->info.expr.op = PT_EVALUATE_VARIABLE;
    e->info.expr.arg1 = name_value (name);
    e->flag.do_not_fold = 1;
    if (marked)
      {
	PT_EXPR_INFO_SET_FLAG (e, PT_EXPR_INFO_PEEK_SESSION_VARIABLE);
      }
    return e;
  }

  PT_NODE *int_const (int x)
  {
    PT_NODE *v = mk (PT_VALUE);
    v->type_enum = PT_TYPE_INTEGER;
    v->info.value.data_value.i = x;
    db_make_int (&v->info.value.db_value, x);
    v->info.value.db_value_is_initialized = true;
    return v;
  }

  PT_NODE *column (const char *name)
  {
    PT_NODE *n = mk (PT_NAME);
    n->info.name.original = name;
    n->info.name.resolved = "t";
    n->type_enum = PT_TYPE_INTEGER;
    return n;
  }

  /* `col = rhs` as the optimizer sees it (qo_converse_sarg_terms has put the column on the left) */
  PT_NODE *equality (PT_NODE * col, PT_NODE * rhs)
  {
    PT_NODE *e = mk (PT_EXPR);
    e->info.expr.op = PT_EQ;
    e->info.expr.arg1 = col;
    e->info.expr.arg2 = rhs;
    return e;
  }

  PT_NODE *host_var (int index)
  {
    PT_NODE *h = mk (PT_HOST_VAR);
    h->info.host_var.index = index;
    h->info.host_var.var_type = PT_HOST_IN;
    return h;
  }

  /* qo_env_new () (query_graph.c, static) as far as qo_env_free () reads it: no segments, nodes, terms, partitions,
   * subqueries or FK info, the three bitsets initialized, a NULL limit value, no planner, no session values */
  QO_ENV *new_env (PARSER_CONTEXT * parser, PT_NODE * select)
  {
    QO_ENV *env = (QO_ENV *) calloc (1, sizeof (QO_ENV));
    env->parser = parser;
    env->pt_tree = select;
    bitset_init (&(env->final_segs), env);
    bitset_init (&(env->fake_terms), env);
    bitset_init (&QO_ENV_SORT_LIMIT_NODES (env), env);
    db_make_null (&QO_ENV_LIMIT_VALUE (env));
    return env;
  }

  struct scene
  {
    PARSER_CONTEXT *parser;
    PT_NODE *select;
    QO_ENV *env;

    scene ()
    {
      parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
      parser->host_variables = (DB_VALUE *) calloc (2, sizeof (DB_VALUE));
      parser->host_var_count = 2;
      db_make_int (&parser->host_variables[0], 7);
      db_make_null (&parser->host_variables[1]);
      select = mk (PT_SELECT);
      env = new_env (parser, select);
    }

    ~scene ()
    {
      qo_env_free (env);
    }

    DB_VALUE *peek (PT_NODE * n)
    {
      return unitseam_session_variable_value (env, n);
    }
  };

}

/* ---------------------------------------------------------------- cases */

#if UT_HAS_PEEK

TEST_CASE ("PR8344 K1: a read the parser did not mark, or no read at all, gives NULL without asking the session",
	   "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  scene s;

  CHECK (s.peek (read_var ("v", false)) == NULL);
  CHECK (s.peek (NULL) == NULL);
  CHECK (s.peek (int_const (5)) == NULL);
  CHECK (s.peek (host_var (0)) == NULL);
  /* the mark on another operator: only PT_EVALUATE_VARIABLE is a read */
  PT_NODE *plus = mk (PT_EXPR);
  plus->info.expr.op = PT_PLUS;
  plus->info.expr.arg1 = name_value ("v");
  PT_EXPR_INFO_SET_FLAG (plus, PT_EXPR_INFO_PEEK_SESSION_VARIABLE);
  CHECK (s.peek (plus) == NULL);
  /* an assignment `@v := ...` carries the name the same way and is not a read either */
  PT_NODE *define = mk (PT_EXPR);
  define->info.expr.op = PT_DEFINE_VARIABLE;
  define->info.expr.arg1 = name_value ("v");
  define->info.expr.arg2 = int_const (1);
  PT_EXPR_INFO_SET_FLAG (define, PT_EXPR_INFO_PEEK_SESSION_VARIABLE);
  CHECK (s.peek (define) == NULL);

  CHECK (g_requests == 0);
  CHECK (unitseam_session_peek_count (s.env) == 0);
}

TEST_CASE ("PR8344 K2: a marked read gives the variable's value; `@V` in the same block is the same entry and the "
	   "session is asked once", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  scene s;

  DB_VALUE *a = s.peek (read_var ("v", true));
  REQUIRE (a != NULL);
  CHECK (DB_VALUE_DOMAIN_TYPE (a) == DB_TYPE_INTEGER);
  CHECK (db_get_int (a) == 5);
  CHECK (g_requests == 1);
  CHECK (strcmp (g_asked[0], "v") == 0);

  /* another read node, the name in another case: the identifier comparison folds case */
  DB_VALUE *b = s.peek (read_var ("V", true));
  CHECK (b == a);
  DB_VALUE *c = s.peek (read_var ("v", true));
  CHECK (c == a);
  CHECK (g_requests == 1);
  CHECK (unitseam_session_peek_count (s.env) == 1);
}

TEST_CASE ("PR8344 K2 boundary: the value is the one the block first saw -- a later change of the session does not "
	   "reach the block, the next block asks again", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  {
    scene s;
    DB_VALUE *a = s.peek (read_var ("v", true));
    REQUIRE (a != NULL);
    session_set ("v", V_INT, 99990);
    DB_VALUE *b = s.peek (read_var ("v", true));
    CHECK (b == a);
    CHECK (db_get_int (b) == 5);
    CHECK (g_requests == 1);
  }
  {
    scene s2;
    DB_VALUE *c = s2.peek (read_var ("v", true));
    REQUIRE (c != NULL);
    CHECK (db_get_int (c) == 99990);
    CHECK (g_requests == 2);
  }
}

TEST_CASE ("PR8344 K3: two variables get two entries; the first value stays where it was after the second request "
	   "(the two bounds of a BETWEEN)", "[contract]")
{
  REQUIRE (boot ());
  session_set ("lo", V_INT, 10);
  session_set ("hi", V_STRING, 0, "zebra");
  scene s;

  DB_VALUE *lo = s.peek (read_var ("lo", true));
  REQUIRE (lo != NULL);
  DB_VALUE *hi = s.peek (read_var ("hi", true));
  REQUIRE (hi != NULL);
  CHECK (lo != hi);
  /* the first pointer still holds the first value */
  CHECK (DB_VALUE_DOMAIN_TYPE (lo) == DB_TYPE_INTEGER);
  CHECK (db_get_int (lo) == 10);
  CHECK (DB_VALUE_DOMAIN_TYPE (hi) == DB_TYPE_VARCHAR);
  CHECK (strcmp (db_get_string (hi), "zebra") == 0);
  CHECK (unitseam_session_peek_count (s.env) == 2);

  /* more variables after them do not move the earlier entries either */
  session_set ("x1", V_INT, 1);
  session_set ("x2", V_INT, 2);
  session_set ("x3", V_INT, 3);
  for (const char *n : { "x1", "x2", "x3" })
    {
      CHECK (s.peek (read_var (n, true)) != NULL);
    }
  CHECK (s.peek (read_var ("LO", true)) == lo);
  CHECK (s.peek (read_var ("Hi", true)) == hi);
  CHECK (db_get_int (lo) == 10);
  CHECK (strcmp (db_get_string (hi), "zebra") == 0);
  CHECK (g_requests == 5);
  CHECK (unitseam_session_peek_count (s.env) == 5);
}

TEST_CASE ("PR8344 K4: a variable the session cannot give -> NULL, the caller's error is the one it had before, the "
	   "error stack is balanced, and the block does not ask again", "[contract]")
{
  REQUIRE (boot ());
  scene s;

  /* no error before */
  CHECK (s.peek (read_var ("missing", true)) == NULL);
  CHECK (g_requests == 1);
  CHECK (er_errid () == NO_ERROR);
  CHECK (g_er_depth == 0);
  CHECK (g_er_pushes == 1);
  CHECK (g_er_pops == 1);

  /* asked once: the failure is kept too */
  CHECK (s.peek (read_var ("MISSING", true)) == NULL);
  CHECK (g_requests == 1);
  CHECK (unitseam_session_peek_count (s.env) == 1);

  /* an error the compilation already had survives the request of another variable */
  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_FAILED, 0);
  CHECK (s.peek (read_var ("other_missing", true)) == NULL);
  CHECK (g_requests == 2);
  CHECK (er_errid () == ER_FAILED);
  CHECK (g_er_depth == 0);
  CHECK (g_er_pushes == g_er_pops);
}

TEST_CASE ("PR8344 K4 boundary: the request runs on its own error level -- the not-found error is set there, not "
	   "on the caller's", "[contract]")
{
  REQUIRE (boot ());
  scene s;
  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_FAILED, 0);
  /* a session variable with a value: success leaves the caller's error as well */
  session_set ("v", V_INT, 1);
  CHECK (s.peek (read_var ("v", true)) != NULL);
  CHECK (er_errid () == ER_FAILED);
  CHECK (s.peek (read_var ("nope", true)) == NULL);
  CHECK (er_errid () == ER_FAILED);
  CHECK (g_er_pushes == 2);
  CHECK (g_er_pops == 2);
}

TEST_CASE ("PR8344 K5: a NULL value (untyped, or a NULL that keeps the variable's type) gives NULL and is asked once",
	   "[contract]")
{
  REQUIRE (boot ());
  session_set ("n", V_NULL);
  session_set ("tn", V_TYPED_NULL);
  scene s;

  CHECK (s.peek (read_var ("n", true)) == NULL);
  CHECK (s.peek (read_var ("tn", true)) == NULL);
  CHECK (s.peek (read_var ("N", true)) == NULL);
  CHECK (s.peek (read_var ("TN", true)) == NULL);
  CHECK (g_requests == 2);
  CHECK (unitseam_session_peek_count (s.env) == 2);
  CHECK (er_errid () == NO_ERROR);
}

TEST_CASE ("PR8344 K6: qo_env_free () releases every entry and value the block asked for -- int, string, NULL, "
	   "not found", "[contract]")
{
  REQUIRE (boot ());
  session_set ("i", V_INT, 42);
  session_set ("s", V_STRING, 0, "a string the session copies to the client");
  session_set ("n", V_NULL);
  /* the reads and the parser are made before the window: they belong to the parse tree, not to the block */
  PARSER_CONTEXT *parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
  PT_NODE *select = mk (PT_SELECT);
  PT_NODE *reads[] = {
    read_var ("i", true), read_var ("s", true), read_var ("n", true), read_var ("gone", true),
    read_var ("I", true), read_var ("S", true), read_var ("v_unmarked", false)
  };
  DB_VALUE *got[7];
  int n_entries;

  g_nlive = 0;
  g_track = true;
  QO_ENV *env = new_env (parser, select);
  for (int k = 0; k < 7; k++)
    {
      got[k] = unitseam_session_variable_value (env, reads[k]);
    }
  n_entries = unitseam_session_peek_count (env);
  qo_env_free (env);
  g_track = false;

  CHECK (got[0] != NULL);
  CHECK (got[1] != NULL);
  CHECK (got[2] == NULL);
  CHECK (got[3] == NULL);
  CHECK (got[4] == got[0]);
  CHECK (got[5] == got[1]);
  CHECK (got[6] == NULL);
  CHECK (n_entries == 4);
  CHECK (g_requests == 4);
  /* every block the window allocated (the environment, the four entries, the string copy) was freed */
  CHECK (g_nlive == 0);
}

TEST_CASE ("PR8344 K7 boundary: a read whose name is not a CHAR value is not estimated and does not ask the session",
	   "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  scene s;
  PT_NODE *r = read_var ("v", true);
  r->info.expr.arg1 = name_value ("v", PT_TYPE_VARCHAR);
  CHECK (s.peek (r) == NULL);
  PT_NODE *r2 = read_var ("v", true);
  r2->info.expr.arg1 = NULL;
  CHECK (s.peek (r2) == NULL);
  CHECK (g_requests == 0);
  CHECK (unitseam_session_peek_count (s.env) == 0);
}

#endif /* UT_HAS_PEEK */

TEST_CASE ("PR8344 K8 develop: a between-range bound that is a host variable or a constant is its value; a session "
	   "variable read the parser did not mark, or no bound, is no value -- and nothing asks the session", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 99990);
  scene s;

  DB_VALUE *hv = unitseam_between_range_arg_value (s.env, host_var (0));
  CHECK (hv == &s.parser->host_variables[0]);
  PT_NODE *k = int_const (3);
  CHECK (unitseam_between_range_arg_value (s.env, k) == &k->info.value.db_value);
  CHECK (unitseam_between_range_arg_value (s.env, NULL) == NULL);
  CHECK (unitseam_between_range_arg_value (s.env, read_var ("v", false)) == NULL);
  CHECK (g_requests == 0);
}

#if UT_HAS_PEEK
TEST_CASE ("PR8344 K8: a between-range bound that is a marked read is its variable's value -- the entry "
	   "qo_session_variable_value () keeps (`col > @v` reaches the histogram as a range)", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 99990);
  scene s;

  DB_VALUE *b = unitseam_between_range_arg_value (s.env, read_var ("v", true));
  REQUIRE (b != NULL);
  CHECK (db_get_int (b) == 99990);
  CHECK (b == s.peek (read_var ("V", true)));
  CHECK (g_requests == 1);
  /* the unmarked read beside it stays no value, and asks nothing */
  CHECK (unitseam_between_range_arg_value (s.env, read_var ("v", false)) == NULL);
  CHECK (g_requests == 1);
}
#endif /* UT_HAS_PEEK */

TEST_CASE ("PR8344 K9 develop: `c1 = @v` with a read the parser did not mark keeps the default -- no histogram probe, "
	   "the index cardinality, else DEFAULT_EQUAL_SELECTIVITY; the session is not asked", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  scene s;

  double sel = unitseam_equal_selectivity (s.env, equality (column ("c1"), read_var ("v", false)));
  CHECK (sel == DEFAULT_EQUAL_SELECTIVITY);
  CHECK (g_hist_probes == 0);
  CHECK (g_icard_calls == 1);
  CHECK (g_requests == 0);
  CHECK (s.env->sel_hist_fallback);
  CHECK_FALSE (s.env->sel_hist_used);
}

#if UT_HAS_PEEK
TEST_CASE ("PR8344 K9: `c1 = @v` with a marked read probes the column's histogram with the variable's value and takes "
	   "its estimate; a value the histogram cannot take, or none, falls back to the default", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  session_set ("s", V_STRING, 0, "5");
  {
    scene s;
    PT_NODE *r = read_var ("v", true);
    double sel = unitseam_equal_selectivity (s.env, equality (column ("c1"), r));
    CHECK (sel == g_hist_sel);
    CHECK (g_hist_probes == 1);
    REQUIRE (g_hist_value != NULL);
    CHECK (g_hist_value == unitseam_session_variable_value (s.env, r));
    CHECK (db_get_int (g_hist_value) == 5);
    CHECK (g_icard_calls == 0);
    CHECK (s.env->sel_hist_used);
  }
  g_hist_probes = 0;
  g_icard_calls = 0;
  {
    /* a string value against the INTEGER column's histogram: not priced, the default path */
    scene s;
    double sel = unitseam_equal_selectivity (s.env, equality (column ("c1"), read_var ("s", true)));
    CHECK (sel == DEFAULT_EQUAL_SELECTIVITY);
    CHECK (g_hist_probes == 1);
    CHECK (g_icard_calls == 1);
    CHECK (s.env->sel_hist_fallback);
  }
  g_hist_probes = 0;
  g_icard_calls = 0;
  {
    /* a variable the session does not have: no probe at all, the default path */
    scene s;
    double sel = unitseam_equal_selectivity (s.env, equality (column ("c1"), read_var ("undefined", true)));
    CHECK (sel == DEFAULT_EQUAL_SELECTIVITY);
    CHECK (g_hist_probes == 0);
    CHECK (g_icard_calls == 1);
    CHECK (er_errid () == NO_ERROR);
  }
}
#endif /* UT_HAS_PEEK */

#if UT_HAS_PEEK
TEST_CASE ("PR8344 K10 boundary: an entry that cannot be allocated -> NULL without an error and nothing kept; the next "
	   "read of the variable asks the session", "[contract]")
{
  REQUIRE (boot ());
  session_set ("v", V_INT, 5);
  scene s;

  g_fail_size = sizeof (QO_SESSION_VAR_PEEK);
  DB_VALUE *a = s.peek (read_var ("v", true));
  bool failed_once = (g_fail_size == 0);
  g_fail_size = 0;
  REQUIRE (failed_once);
  CHECK (a == NULL);
  CHECK (g_requests == 0);
  CHECK (er_errid () == NO_ERROR);
  CHECK (unitseam_session_peek_count (s.env) == 0);

  DB_VALUE *b = s.peek (read_var ("v", true));
  REQUIRE (b != NULL);
  CHECK (db_get_int (b) == 5);
  CHECK (g_requests == 1);
  CHECK (unitseam_session_peek_count (s.env) == 1);
}
#endif /* UT_HAS_PEEK */
