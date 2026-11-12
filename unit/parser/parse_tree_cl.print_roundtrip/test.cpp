/*
 * PR#8242 (CBRD-27478) G3 -- the hint block pt_print_delete () / pt_print_update () / pt_print_select ()
 * (src/parser/parse_tree_cl.c) print, read back by the scanner.
 *
 * Texts printed from a DELETE / UPDATE tree are parsed again:
 *   - a trigger action: execute_statement.c get_activity_info () stores parser_print_tree_with_quotes () of the
 *     action, trigger_manager.c compiles it with pt_compile_trigger_stmt () -> parser_parse_string_use_sys_charset ()
 *   - a dblink DML: parser_support.c prints the statement with pt_print_bytes () and sends it to the remote server
 * and the same print, with CUSTOM_PRINT_4_SHA_COMPUTE | PT_PRINT_LOWER, is the XASL cache key of a server-side
 * DELETE / UPDATE (execute_statement.c do_prepare_delete / do_prepare_update).
 *
 * Re-parse = what the scanner does with the printed hint comment:
 *   csql_lexer.l BLOCK_HINT: "/*+" starts it, the body up to "*" "/" is collected behind a "+", parser_check_hint ()
 *   runs pt_check_hint () over it and a comment with no hint is not an SQL_HINT; csql_grammar.y hint_list then runs
 *   pt_get_hint () on the statement node.
 *
 * Real code: scanner_support.c (the TU: pt_initialize_hint, pt_check_hint, find_hint_token, read_hint_args,
 * get_hint_args_func, pt_get_hint, this_parser), parse_tree.c + parse_tree_cl.c (parser_create_parser,
 * parser_new_node, parser_print_tree, parser_print_tree_with_quotes, pt_print_delete / _update / _select / _spec /
 * _name), language_support.c + intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c,
 * db_macro.c, memory_alloc.c, system_parameter.c.
 * The hint table is csql_grammar.y parser_hint_table of the revision (copied; the two CBRD-27478 entries only where
 * parse_tree.h defines their bits).
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "csql_grammar_scan.h"
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

/* ---------------------------------------------------------------- fakes (as unit/parser/xasl_generation.session_variable_mark) */

namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

const char *
envvar_get (const char *)
{
  return NULL;
}

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

void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

void
db_date_locale_init (void)
{
}

void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

namespace
{
  int g_er = NO_ERROR;
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

/* ---------------------------------------------------------------- the hint table of csql_grammar.y */

#define UT_HINT(key, type) {key, NULL, type, 0, false}
static PT_HINT ut_hint_table[] = {
  UT_HINT ("ORDERED", PT_HINT_ORDERED),
  UT_HINT ("LEADING", PT_HINT_LEADING),
  UT_HINT ("NO_INDEX_SS", PT_HINT_NO_INDEX_SS),
  UT_HINT ("INDEX_SS", PT_HINT_INDEX_SS),
  UT_HINT ("USE_NL", PT_HINT_USE_NL),
  UT_HINT ("USE_IDX", PT_HINT_USE_IDX),
  UT_HINT ("USE_MERGE", PT_HINT_USE_MERGE),
  UT_HINT ("RECOMPILE", PT_HINT_RECOMPILE),
  UT_HINT ("LOCK_TIMEOUT", PT_HINT_LK_TIMEOUT),
  UT_HINT ("NO_LOGGING", PT_HINT_NO_LOGGING),
  UT_HINT ("QUERY_CACHE", PT_HINT_QUERY_CACHE),
  UT_HINT ("SQL_CACHE", PT_HINT_QUERY_CACHE),
  UT_HINT ("QUERY_NO_CACHE", PT_HINT_QUERY_NO_CACHE),
  UT_HINT ("SQL_NO_CACHE", PT_HINT_QUERY_NO_CACHE),
  UT_HINT ("REEXECUTE", PT_HINT_REEXECUTE),
  UT_HINT ("JDBC_CACHE", PT_HINT_JDBC_CACHE),
  UT_HINT ("USE_DESC_IDX", PT_HINT_USE_IDX_DESC),
  UT_HINT ("NO_COVERING_IDX", PT_HINT_NO_COVERING_IDX),
  UT_HINT ("INSERT_EXECUTION_MODE", PT_HINT_INSERT_MODE),
  UT_HINT ("NO_DESC_IDX", PT_HINT_NO_IDX_DESC),
  UT_HINT ("NO_MULTI_RANGE_OPT", PT_HINT_NO_MULTI_RANGE_OPT),
  UT_HINT ("USE_UPDATE_IDX", PT_HINT_USE_UPDATE_IDX),
  UT_HINT ("USE_INSERT_IDX", PT_HINT_USE_INSERT_IDX),
  UT_HINT ("NO_SORT_LIMIT", PT_HINT_NO_SORT_LIMIT),
  UT_HINT ("NO_HASH_AGGREGATE", PT_HINT_NO_HASH_AGGREGATE),
  UT_HINT ("NO_HASH_LIST_SCAN", PT_HINT_NO_HASH_LIST_SCAN),
  UT_HINT ("NO_PUSH_PRED", PT_HINT_NO_PUSH_PRED),
  UT_HINT ("NO_MERGE", PT_HINT_NO_MERGE),
  UT_HINT ("NO_UNNEST", PT_HINT_NO_UNNEST),
  UT_HINT ("NO_SUBQUERY_CACHE", PT_HINT_NO_SUBQUERY_CACHE),
  UT_HINT ("NO_PARALLEL_SCAN", PT_HINT_NO_PARALLEL_SCAN),
  UT_HINT ("NO_PARALLEL_SUBQUERY", PT_HINT_NO_PARALLEL_SUBQUERY),
  UT_HINT ("NO_PARALLEL_HASH_JOIN", PT_HINT_NO_PARALLEL_HASH_JOIN),
  UT_HINT ("PARALLEL", PT_HINT_PARALLEL),
  UT_HINT ("DBLINK_NO_PUSH_DOWN_SUBQ", PT_HINT_DBLINK_NO_PUSH_DOWN_SUBQ),
#if defined (PT_HINT_ROW_SNAPSHOT)
  UT_HINT ("ROW_SNAPSHOT", PT_HINT_ROW_SNAPSHOT),
  UT_HINT ("NO_ROW_SNAPSHOT", PT_HINT_NO_ROW_SNAPSHOT),
#endif
  UT_HINT ("NO_ELIMINATE_JOIN", PT_HINT_NO_ELIMINATE_JOIN),
  UT_HINT ("SKIP_UPDATE_NULL", PT_HINT_SKIP_UPDATE_NULL),
  UT_HINT ("NO_INDEX_LS", PT_HINT_NO_INDEX_LS),
  UT_HINT ("INDEX_LS", PT_HINT_INDEX_LS),
  UT_HINT ("SELECT_RECORD_INFO", PT_HINT_SELECT_RECORD_INFO),
  UT_HINT ("SELECT_PAGE_INFO", PT_HINT_SELECT_PAGE_INFO),
  UT_HINT ("SELECT_KEY_INFO", PT_HINT_SELECT_KEY_INFO),
  UT_HINT ("SELECT_BTREE_NODE_INFO", PT_HINT_SELECT_BTREE_NODE_INFO),
  UT_HINT ("USE_SBR", PT_HINT_USE_SBR),
  UT_HINT ("NO_SUPPLEMENTAL_LOG", PT_HINT_NO_SUPPLEMENTAL_LOG),
  UT_HINT ("USE_HASH", PT_HINT_USE_HASH),
  UT_HINT ("NO_USE_HASH", PT_HINT_NO_USE_HASH),
  UT_HINT ("INLINE", PT_HINT_INLINE_CTE),
  UT_HINT ("MATERIALIZE", PT_HINT_MATERIALIZE_CTE),
  UT_HINT ("BIND_SENSITIVE", PT_HINT_BIND_SENSITIVE),
  {NULL, NULL, (PT_HINT_ENUM) - 1, 0, false}
};

/* ---------------------------------------------------------------- helpers */
namespace
{
  PARSER_CONTEXT *g_parser = NULL;

  PARSER_CONTEXT *boot ()
  {
    static bool done = false;
    if (!done)
      {
	done = true;
	lang_init_builtin ();
	REQUIRE (lang_set_charset_lang ("en_US.utf8") == NO_ERROR);
	g_parser = parser_create_parser ();
	REQUIRE (g_parser != NULL);
	this_parser = g_parser;
	pt_initialize_hint (g_parser, ut_hint_table);
      }
    this_parser = g_parser;
    g_parser->custom_print = 0;
    return g_parser;
  }

  PT_NODE *name (PARSER_CONTEXT * parser, const char *s, PT_MISC_TYPE meta)
  {
    PT_NODE *n = parser_new_node (parser, PT_NAME);
    n->info.name.original = pt_append_string (parser, NULL, s);
    n->info.name.meta_class = meta;
    return n;
  }

  /* csql_grammar.y: a class spec `a` (entity_name PT_NAME, meta_class PT_CLASS) */
  PT_NODE *spec (PARSER_CONTEXT * parser, const char *s)
  {
    PT_NODE *sp = parser_new_node (parser, PT_SPEC);
    sp->info.spec.entity_name = name (parser, s, PT_CLASS);
    sp->info.spec.id = (UINTPTR) sp;
    return sp;
  }

  /* DELETE target FROM a, b (csql_grammar.y delete statement: target_classes, spec) */
  PT_NODE *delete_stmt (PARSER_CONTEXT * parser, const char *target, PT_HINT_ENUM hint)
  {
    PT_NODE *d = parser_new_node (parser, PT_DELETE);
    d->info.delete_.target_classes = name (parser, target, PT_CLASS);
    d->info.delete_.spec = spec (parser, "a");
    d->info.delete_.spec->next = spec (parser, "b");
    d->info.delete_.hint = hint;
    return d;
  }

  /* UPDATE a SET (no assignment needed for the hint block) */
  PT_NODE *update_stmt (PARSER_CONTEXT * parser, PT_HINT_ENUM hint)
  {
    PT_NODE *u = parser_new_node (parser, PT_UPDATE);
    u->info.update.spec = spec (parser, "a");
    u->info.update.hint = hint;
    return u;
  }

  /* SELECT 1 FROM a, b */
  PT_NODE *select_stmt (PARSER_CONTEXT * parser, PT_HINT_ENUM hint)
  {
    PT_NODE *s = parser_new_node (parser, PT_SELECT);
    PT_NODE *v = parser_new_node (parser, PT_VALUE);
    v->type_enum = PT_TYPE_INTEGER;
    v->info.value.data_value.i = 1;
    s->info.query.q.select.list = v;
    s->info.query.q.select.from = spec (parser, "a");
    s->info.query.q.select.from->next = spec (parser, "b");
    s->info.query.q.select.hint = hint;
    return s;
  }

  /* the printed hint comment as csql_lexer.l BLOCK_HINT hands it on: "+" and the body up to the closing star-slash */
  std::string hint_comment (const std::string & printed)
  {
    size_t s = printed.find ("/*+");
    REQUIRE (s != std::string::npos);
    size_t e = printed.find ("*/", s + 3);
    REQUIRE (e != std::string::npos);
    return "+" + printed.substr (s + 3, e - (s + 3));
  }

  /* scanner: parser_check_hint (pt_check_hint) then hint_list (pt_get_hint) on a fresh node of the same type;
   * the hint bits the re-parsed statement gets */
  PT_HINT_ENUM reparse (PARSER_CONTEXT * parser, PT_NODE_TYPE type, const std::string & printed)
  {
    std::string c = hint_comment (printed);
    std::vector < char >buf (c.begin (), c.end ());
    buf.push_back ('\0');
    PT_HINT_ENUM found = PT_HINT_NONE;
    pt_check_hint (buf.data (), ut_hint_table, &found);
    if (found == PT_HINT_NONE)
      {
	return PT_HINT_NONE;	/* not an SQL_HINT: an ordinary comment */
      }
    PT_NODE *node = parser_new_node (parser, type);
    pt_get_hint (buf.data (), ut_hint_table, node);
    switch (type)
      {
      case PT_DELETE:
	return node->info.delete_.hint;
      case PT_UPDATE:
	return node->info.update.hint;
      case PT_SELECT:
	return (PT_HINT_ENUM) (node->info.query.q.select.hint | node->info.query.hint);
      default:
	return PT_HINT_NONE;
      }
  }

  std::string print_trigger_text (PARSER_CONTEXT * parser, PT_NODE * stmt)
  {
    const char *s = parser_print_tree_with_quotes (parser, stmt);
    REQUIRE (s != NULL);
    return s;
  }

  /* the body of the XASL cache key text of a server-side DELETE (do_prepare_delete: PT_NODE_PRINT_TO_ALIAS with
   * CUSTOM_PRINT_4_SHA_COMPUTE | PT_PRINT_LOWER) -- PT_PRINT_USER / _HOST_VAR_COUNT / _DBLINK_INFO only append
   * suffixes that are the same for two statements of one session, so they are left out here */
  std::string print_key_body (PARSER_CONTEXT * parser, PT_NODE * stmt)
  {
    unsigned int save = parser->custom_print;
    parser->custom_print |= PT_CONVERT_RANGE | PT_PRINT_QUOTES | PT_PRINT_LOWER;
    const char *s = parser_print_tree (parser, stmt);
    parser->custom_print = save;
    REQUIRE (s != NULL);
    return s;
  }

  struct named_hint
  {
    const char *name;
    PT_HINT_ENUM bit;
  };

  /* hints pt_get_hint () puts on a DELETE / UPDATE without arguments (scanner_support.c) */
  const named_hint upddel_noarg[] = {
    {"ORDERED", PT_HINT_ORDERED},
    {"NO_LOGGING", PT_HINT_NO_LOGGING},
    {"BIND_SENSITIVE", PT_HINT_BIND_SENSITIVE},
    {"USE_DESC_IDX", PT_HINT_USE_IDX_DESC},
    {"NO_COVERING_IDX", PT_HINT_NO_COVERING_IDX},
    {"NO_DESC_IDX", PT_HINT_NO_IDX_DESC},
    {"NO_MULTI_RANGE_OPT", PT_HINT_NO_MULTI_RANGE_OPT},
    {"NO_SORT_LIMIT", PT_HINT_NO_SORT_LIMIT},
    {"USE_SBR", PT_HINT_USE_SBR},
    {"NO_SUPPLEMENTAL_LOG", PT_HINT_NO_SUPPLEMENTAL_LOG},
    {"NO_PARALLEL_HASH_JOIN", PT_HINT_NO_PARALLEL_HASH_JOIN},
  };

  /* hints pt_get_hint () puts on a SELECT without arguments */
  const named_hint select_noarg[] = {
    {"ORDERED", PT_HINT_ORDERED},
    {"NO_INDEX_SS", PT_HINT_NO_INDEX_SS},
    {"USE_DESC_IDX", PT_HINT_USE_IDX_DESC},
    {"NO_COVERING_IDX", PT_HINT_NO_COVERING_IDX},
    {"NO_DESC_IDX", PT_HINT_NO_IDX_DESC},
    {"NO_MULTI_RANGE_OPT", PT_HINT_NO_MULTI_RANGE_OPT},
    {"NO_SORT_LIMIT", PT_HINT_NO_SORT_LIMIT},
    {"NO_HASH_AGGREGATE", PT_HINT_NO_HASH_AGGREGATE},
    {"NO_HASH_LIST_SCAN", PT_HINT_NO_HASH_LIST_SCAN},
    {"NO_PUSH_PRED", PT_HINT_NO_PUSH_PRED},
    {"NO_MERGE", PT_HINT_NO_MERGE},
    {"NO_UNNEST", PT_HINT_NO_UNNEST},
    {"NO_SUBQUERY_CACHE", PT_HINT_NO_SUBQUERY_CACHE},
    {"NO_PARALLEL_SCAN", PT_HINT_NO_PARALLEL_SCAN},
    {"NO_PARALLEL_SUBQUERY", PT_HINT_NO_PARALLEL_SUBQUERY},
    {"NO_PARALLEL_HASH_JOIN", PT_HINT_NO_PARALLEL_HASH_JOIN},
    {"NO_ELIMINATE_JOIN", PT_HINT_NO_ELIMINATE_JOIN},
    {"NO_INDEX_LS", PT_HINT_NO_INDEX_LS},
  };

  /* the hints among `others` that, printed together with `h` in a statement of `type`, do not both read back */
  std::vector < std::string > lost_partners (PT_NODE_TYPE type, PT_HINT_ENUM h, const named_hint * others, size_t n)
  {
    PARSER_CONTEXT *parser = boot ();
    std::vector < std::string > lost;
    for (size_t i = 0; i < n; i++)
      {
	if (others[i].bit == h)
	  {
	    continue;
	  }
	PT_HINT_ENUM both = (PT_HINT_ENUM) (others[i].bit | h);
	PT_NODE *stmt = (type == PT_DELETE ? delete_stmt (parser, "a", both)
			 : type == PT_UPDATE ? update_stmt (parser, both) : select_stmt (parser, both));
	std::string printed = print_trigger_text (parser, stmt);
	PT_HINT_ENUM back = reparse (parser, type, printed);
	if ((back & both) != both)
	  {
	    lost.push_back (std::string (others[i].name) + " => " + printed);
	  }
      }
    return lost;
  }

  std::vector < std::string > names_only (const std::vector < std::string > &v)
  {
    std::vector < std::string > out;
    for (const std::string & s:v)
      {
	out.push_back (s.substr (0, s.find (" => ")));
      }
    return out;
  }
}

/* ================================================================ G3-01: separator between printed hints */

TEST_CASE ("G3 control: each DELETE / UPDATE hint read back alone (printer and scanner agree)", "[contract]")
{
  PARSER_CONTEXT *parser = boot ();
  for (const named_hint & h:upddel_noarg)
    {
      INFO (h.name);
      std::string d = print_trigger_text (parser, delete_stmt (parser, "a", h.bit));
      CHECK ((reparse (parser, PT_DELETE, d) & h.bit) == h.bit);
      std::string u = print_trigger_text (parser, update_stmt (parser, h.bit));
      CHECK ((reparse (parser, PT_UPDATE, u) & h.bit) == h.bit);
    }
}

TEST_CASE ("G3 control: LOCK_TIMEOUT(n) followed directly by the next hint still reads back (')' ends a token)",
	   "[contract]")
{
  PARSER_CONTEXT *parser = boot ();
  PT_NODE *d = delete_stmt (parser, "a", (PT_HINT_ENUM) (PT_HINT_LK_TIMEOUT | PT_HINT_NO_PARALLEL_HASH_JOIN));
  d->info.delete_.waitsecs_hint = name (parser, "5", PT_HINT_NAME);
  std::string printed = print_trigger_text (parser, d);
  INFO (printed);
  PT_HINT_ENUM back = reparse (parser, PT_DELETE, printed);
  CHECK ((back & PT_HINT_NO_PARALLEL_HASH_JOIN) != 0);
  CHECK ((back & PT_HINT_LK_TIMEOUT) != 0);
}

TEST_CASE ("G3 (develop) NO_PARALLEL_HASH_JOIN printed right after NO_LOGGING / BIND_SENSITIVE: both hints are lost "
	   "on re-parse (PASS = defect present)", "[defect]")
{
  std::vector < std::string > d = lost_partners (PT_DELETE, PT_HINT_NO_PARALLEL_HASH_JOIN, upddel_noarg,
						 sizeof (upddel_noarg) / sizeof (upddel_noarg[0]));
  std::vector < std::string > u = lost_partners (PT_UPDATE, PT_HINT_NO_PARALLEL_HASH_JOIN, upddel_noarg,
						 sizeof (upddel_noarg) / sizeof (upddel_noarg[0]));
  for (const std::string & s:d)
    {
      UNSCOPED_INFO ("DELETE: " << s);
    }
  for (const std::string & s:u)
    {
      UNSCOPED_INFO ("UPDATE: " << s);
    }
  CHECK (names_only (d) == std::vector < std::string > ({"NO_LOGGING", "BIND_SENSITIVE"}));
  CHECK (names_only (u) == std::vector < std::string > ({"NO_LOGGING", "BIND_SENSITIVE"}));
}

#if defined (PT_HINT_ROW_SNAPSHOT)
TEST_CASE ("G3 control: SELECT prints ROW_SNAPSHOT / NO_ROW_SNAPSHOT so that every other SELECT hint reads back "
	   "with it", "[contract]")
{
  size_t n = sizeof (select_noarg) / sizeof (select_noarg[0]);
  std::vector < std::string > a = lost_partners (PT_SELECT, PT_HINT_ROW_SNAPSHOT, select_noarg, n);
  std::vector < std::string > b = lost_partners (PT_SELECT, PT_HINT_NO_ROW_SNAPSHOT, select_noarg, n);
  for (const std::string & s:a)
    {
      UNSCOPED_INFO (s);
    }
  for (const std::string & s:b)
    {
      UNSCOPED_INFO (s);
    }
  CHECK (a.empty ());
  CHECK (b.empty ());

  PARSER_CONTEXT *parser = boot ();
  PT_HINT_ENUM both = (PT_HINT_ENUM) (PT_HINT_ROW_SNAPSHOT | PT_HINT_NO_ROW_SNAPSHOT);
  std::string printed = print_trigger_text (parser, select_stmt (parser, both));
  INFO (printed);
  CHECK ((reparse (parser, PT_SELECT, printed) & both) == both);
}

TEST_CASE ("G3 control: DELETE / UPDATE with ROW_SNAPSHOT alone, or after a hint printed with a trailing space, "
	   "read back", "[contract]")
{
  PARSER_CONTEXT *parser = boot ();
  const PT_HINT_ENUM hs[] = { PT_HINT_ROW_SNAPSHOT, PT_HINT_NO_ROW_SNAPSHOT,
    (PT_HINT_ENUM) (PT_HINT_ROW_SNAPSHOT | PT_HINT_NO_ROW_SNAPSHOT),
    (PT_HINT_ENUM) (PT_HINT_ORDERED | PT_HINT_ROW_SNAPSHOT),
    (PT_HINT_ENUM) (PT_HINT_USE_SBR | PT_HINT_NO_ROW_SNAPSHOT),
    (PT_HINT_ENUM) (PT_HINT_NO_PARALLEL_HASH_JOIN | PT_HINT_ROW_SNAPSHOT)
  };
  for (PT_HINT_ENUM h:hs)
    {
      std::string d = print_trigger_text (parser, delete_stmt (parser, "a", h));
      INFO (d);
      CHECK ((reparse (parser, PT_DELETE, d) & h) == h);
      std::string u = print_trigger_text (parser, update_stmt (parser, h));
      INFO (u);
      CHECK ((reparse (parser, PT_UPDATE, u) & h) == h);
    }
}

TEST_CASE ("G3-01 ROW_SNAPSHOT / NO_ROW_SNAPSHOT printed right after NO_LOGGING / BIND_SENSITIVE in a DELETE or "
	   "UPDATE: glued into one word, both hints are lost on re-parse (PASS = defect present)", "[defect]")
{
  size_t n = sizeof (upddel_noarg) / sizeof (upddel_noarg[0]);
  const std::vector < std::string > expect ({"NO_LOGGING", "BIND_SENSITIVE"});
  for (PT_HINT_ENUM h: {PT_HINT_ROW_SNAPSHOT, PT_HINT_NO_ROW_SNAPSHOT})
    {
      std::vector < std::string > d = lost_partners (PT_DELETE, h, upddel_noarg, n);
      std::vector < std::string > u = lost_partners (PT_UPDATE, h, upddel_noarg, n);
      for (const std::string & s:d)
	{
	  UNSCOPED_INFO ("DELETE: " << s);
	}
      for (const std::string & s:u)
	{
	  UNSCOPED_INFO ("UPDATE: " << s);
	}
      CHECK (names_only (d) == expect);
      CHECK (names_only (u) == expect);
    }

  /* the two words of the report: the printed text and what the re-parsed statement keeps */
  PARSER_CONTEXT *parser = boot ();
  std::string d = print_trigger_text (parser, delete_stmt (parser, "a",
							   (PT_HINT_ENUM) (PT_HINT_NO_LOGGING | PT_HINT_ROW_SNAPSHOT)));
  INFO (d);
  CHECK (d.find ("NO_LOGGINGROW_SNAPSHOT") != std::string::npos);
  CHECK (reparse (parser, PT_DELETE, d) == PT_HINT_NONE);

  std::string u = print_trigger_text (parser, update_stmt (parser,
							   (PT_HINT_ENUM) (PT_HINT_BIND_SENSITIVE |
									   PT_HINT_NO_ROW_SNAPSHOT)));
  INFO (u);
  CHECK (u.find ("BIND_SENSITIVENO_ROW_SNAPSHOT") != std::string::npos);
  CHECK (reparse (parser, PT_UPDATE, u) == PT_HINT_NONE);
}
#endif /* PT_HINT_ROW_SNAPSHOT */

/* ================================================================ G3-02: pt_print_delete () target list */

TEST_CASE ("G3 control: a multi-table DELETE without hint arguments prints its own target", "[contract]")
{
  PARSER_CONTEXT *parser = boot ();
  std::string a = print_key_body (parser, delete_stmt (parser, "a", PT_HINT_ORDERED));
  std::string b = print_key_body (parser, delete_stmt (parser, "b", PT_HINT_ORDERED));
  INFO (a);
  INFO (b);
  CHECK (a != b);
}

TEST_CASE ("G3-02 (develop) pt_print_delete () prints the last hint argument where the target list goes: DELETE a "
	   "and DELETE b FROM a, b with USE_NL(b) / LOCK_TIMEOUT(5) print the same text (PASS = defect present)",
	   "[defect]")
{
  PARSER_CONTEXT *parser = boot ();

  PT_NODE *da = delete_stmt (parser, "a", PT_HINT_USE_NL);
  da->info.delete_.use_nl_hint = name (parser, "b", PT_HINT_NAME);
  PT_NODE *db = delete_stmt (parser, "b", PT_HINT_USE_NL);
  db->info.delete_.use_nl_hint = name (parser, "b", PT_HINT_NAME);
  std::string ka = print_key_body (parser, da);
  std::string kb = print_key_body (parser, db);
  INFO (ka);
  INFO (kb);
  CHECK (ka == kb);

  PT_NODE *ta = delete_stmt (parser, "a", PT_HINT_LK_TIMEOUT);
  ta->info.delete_.waitsecs_hint = name (parser, "5", PT_HINT_NAME);
  PT_NODE *tb = delete_stmt (parser, "b", PT_HINT_LK_TIMEOUT);
  tb->info.delete_.waitsecs_hint = name (parser, "5", PT_HINT_NAME);
  std::string la = print_key_body (parser, ta);
  std::string lb = print_key_body (parser, tb);
  INFO (la);
  INFO (lb);
  CHECK (la == lb);
}

TEST_CASE ("G3-02 (develop) pt_print_delete () with a WITH clause prints the WITH clause again where the target list "
	   "goes: WITH c ... DELETE a / DELETE b FROM a, b print the same text (PASS = defect present)", "[defect]")
{
  PARSER_CONTEXT *parser = boot ();
  std::string k[2];
  const char *targets[2] = { "a", "b" };
  for (int i = 0; i < 2; i++)
    {
      /* csql_grammar.y: opt_with_clause delete_stmt -> delete_.with = PT_WITH_CLAUSE (cte_definition_list PT_CTE) */
      PT_NODE *cte = parser_new_node (parser, PT_CTE);
      cte->info.cte.name = name (parser, "c", PT_CLASS);
      cte->info.cte.non_recursive_part = select_stmt (parser, PT_HINT_NONE);
      PT_NODE *with = parser_new_node (parser, PT_WITH_CLAUSE);
      with->info.with_clause.cte_definition_list = cte;
      PT_NODE *d = delete_stmt (parser, targets[i], PT_HINT_NONE);
      d->info.delete_.with = with;
      k[i] = print_key_body (parser, d);
      INFO (k[i]);
    }
  INFO (k[0]);
  INFO (k[1]);
  CHECK (k[0] == k[1]);
  CHECK (k[0].find ("delete with ") != std::string::npos);
}
