/*
 * PR#8393 (CBRD-27849) -- pt_set_host_variables () of src/parser/parse_dbi.c (CS_MODE): one execution's values cast
 * into the host variable slots of a compiled statement (db_push_values / EXECUTE ... USING / PL/CSQL static SQL).
 *
 * The contract read off the function (parse_dbi.c:3336-3410 of 2837872ec):
 *   - a value is cast into the slot's expected domain (tp_value_cast_preserve_domain); a CHAR(n) cast of a string
 *     makes a new buffer (string_opfunc.c db_char_string_coerce -> qstr_coerce, need_clear on)
 *   - a VARCHAR value bound to a CHAR(n) position keeps its original VARCHAR value in the slot once the cast accepts
 *     it (so the comparison is the VARCHAR one, as a literal CAST(... AS VARCHAR) is); the cast copy is released
 *     first -- before 2837872ec it was overwritten without being freed, at every execution
 *   - any other value keeps the cast result (CHAR value into CHAR, VARCHAR into VARCHAR)
 *   - the slot of the previous execution is cleared at the top of the loop, so a slot holds one owned value
 *
 * Real code: parse_dbi.c (the TU), object_domain.c (tp_value_cast_preserve_domain, tp_Char_domain /
 * tp_String_domain), object_primitive.c (pr_clear_value / pr_clone_value), string_opfunc.c (db_char_string_coerce,
 * qstr_coerce), db_macro.c (db_make_string / db_make_char), memory_alloc.c (db_private_alloc -> malloc in CS_MODE),
 * language_support.c + intl_support.c + chartype.c (en_US.utf8), domain_rules.c, system_parameter.c.
 * Fakes: pt_is_reference_to_reusable_oid (parse_evaluate.c:636 -- 0 for a value that is not an OID), the locale side
 * tables lang_init_builtin fills (init_builtin_calendar_names, db_date_locale_init), envvar_get / envvar_trim_char,
 * cubregex::get_engine_name.
 * Fakes also: db_ws_alloc / db_ws_realloc / db_ws_free (quick_fit.c, the workspace heap of db_private_alloc in
 * CS_MODE) as malloc / realloc / free counting the live blocks -- a leaked cast copy is one more live block per
 * execution.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "dbi.h"
#include "error_manager.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "string_regex.hpp"
#include <cstdlib>
#include <cstring>
#include <string>

/* ---------------------------------------------------------------- fakes */

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
db_date_locale_init (void)
{
}

/* quick_fit.c db_ws_alloc / db_ws_realloc / db_ws_free: the workspace heap db_private_alloc (NULL thread) uses in
 * CS_MODE -- each block a malloc, counted while it lives (the real ones take it from the Lea heap of the workspace) */
namespace
{
  long g_live_blocks = 0;
}

void *
db_ws_alloc (size_t size)
{
  void *p = size > 0 ? malloc (size) : NULL;
  if (p != NULL)
    {
      g_live_blocks++;
    }
  return p;
}

void *
db_ws_realloc (void *ptr, size_t size)
{
  if (ptr == NULL)
    {
      return db_ws_alloc (size);
    }
  return realloc (ptr, size);
}

void
db_ws_free (void *ptr)
{
  if (ptr != NULL)
    {
      g_live_blocks--;
      free (ptr);
    }
}

/* parse_evaluate.c pt_is_reference_to_reusable_oid (): 1 only for an OID / object of a reusable-OID class, else 0 */
int
pt_is_reference_to_reusable_oid (DB_VALUE * val)
{
  return 0;
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
    return lang_charset () == INTL_CODESET_UTF8;
  }

  long heap_in_use ()
  {
    return g_live_blocks;
  }

  /* a compiled statement of one host variable position of the given expected domain (what pt_bind_values_to_hostvars
   * and the type check leave: host_var_count, host_variables zeroed, host_var_expected_domains) */
  struct stmt
  {
    PARSER_CONTEXT *parser;
    TP_DOMAIN domain;

    explicit stmt (const TP_DOMAIN & base, int precision)
    {
      domain = base;
      domain.precision = precision;
      domain.codeset = LANG_SYS_CODESET;
      domain.collation_id = LANG_SYS_COLLATION;
      parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
      parser->host_var_count = 1;
      parser->host_variables = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
      db_make_null (&parser->host_variables[0]);
      parser->host_var_expected_domains = (TP_DOMAIN **) calloc (1, sizeof (TP_DOMAIN *));
      parser->host_var_expected_domains[0] = &domain;
    }
    ~stmt ()
    {
      pr_clear_value (&parser->host_variables[0]);
      free (parser->host_variables);
      free (parser->host_var_expected_domains);
      free (parser);
    }
    DB_VALUE *slot ()
    {
      return &parser->host_variables[0];
    }
  };

  void make_value (DB_VALUE * v, const std::string & s, bool as_char)
  {
    if (as_char)
      {
	db_make_char (v, (int) s.size (), s.c_str (), (int) s.size (), LANG_SYS_CODESET, LANG_SYS_COLLATION);
      }
    else
      {
	db_make_varchar (v, DB_MAX_VARCHAR_PRECISION, s.c_str (), (int) s.size (), LANG_SYS_CODESET,
			 LANG_SYS_COLLATION);
      }
  }

  /* executes n times with the same value; returns how many more workspace blocks live after the last execution than
   * after the first */
  long growth_over (stmt & st, DB_VALUE * v, int n)
  {
    pt_set_host_variables (st.parser, 1, v);
    long before = heap_in_use ();
    for (int i = 1; i < n; i++)
      {
	pt_set_host_variables (st.parser, 1, v);
      }
    return heap_in_use () - before;
  }
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("SH1 a VARCHAR value bound to a CHAR(n) position keeps the VARCHAR value in the slot", "[contract]")
{
  REQUIRE (boot ());
  stmt st (tp_Char_domain, 256);
  DB_VALUE v;
  std::string s (250, 'a');
  make_value (&v, s, false);

  pt_set_host_variables (st.parser, 1, &v);
  CHECK (st.parser->flag.set_host_var == 1);
  CHECK (DB_VALUE_DOMAIN_TYPE (st.slot ()) == DB_TYPE_VARCHAR);
  CHECK (db_get_string_size (st.slot ()) == 250);
  CHECK (std::string (db_get_string (st.slot ()), 250) == s);
  /* the slot owns its copy: not the caller's buffer */
  CHECK (db_get_string (st.slot ()) != db_get_string (&v));
}

TEST_CASE ("SH2 repeated executions of a VARCHAR value at a CHAR(n) position keep the heap flat", "[contract]")
{
  REQUIRE (boot ());
  stmt st (tp_Char_domain, 2000);
  DB_VALUE v;
  std::string s (2000, 'a');
  make_value (&v, s, false);

  /* before the fix: one cast copy (4 x 2000 + 1 bytes in utf8) left per execution */
  long growth = growth_over (st, &v, 2000);
  INFO ("live workspace blocks grew by " << growth << " over 1,999 executions");
  CHECK (growth == 0);
}

TEST_CASE ("SH3 a CHAR value bound to a CHAR(n) position keeps the cast CHAR(n) value, heap flat", "[contract]")
{
  REQUIRE (boot ());
  stmt st (tp_Char_domain, 8);
  DB_VALUE v;
  make_value (&v, "abc", true);

  pt_set_host_variables (st.parser, 1, &v);
  CHECK (DB_VALUE_DOMAIN_TYPE (st.slot ()) == DB_TYPE_CHAR);
  CHECK (db_get_string_size (st.slot ()) == 8);	/* padded to CHAR(8) */
  CHECK (growth_over (st, &v, 2000) == 0);
}

TEST_CASE ("SH4 a VARCHAR value bound to a VARCHAR(n) position keeps the cast value, heap flat", "[contract]")
{
  REQUIRE (boot ());
  stmt st (tp_String_domain, 2000);
  DB_VALUE v;
  std::string s (2000, 'b');
  make_value (&v, s, false);

  pt_set_host_variables (st.parser, 1, &v);
  CHECK (DB_VALUE_DOMAIN_TYPE (st.slot ()) == DB_TYPE_VARCHAR);
  CHECK (std::string (db_get_string (st.slot ()), db_get_string_size (st.slot ())) == s);
  CHECK (growth_over (st, &v, 2000) == 0);
}

TEST_CASE ("SH5 a NULL value at a CHAR(n) position leaves a NULL slot", "[contract]")
{
  REQUIRE (boot ());
  stmt st (tp_Char_domain, 8);
  DB_VALUE v;
  db_make_null (&v);

  pt_set_host_variables (st.parser, 1, &v);
  CHECK (st.parser->flag.set_host_var == 1);
  CHECK (DB_IS_NULL (st.slot ()));
}
