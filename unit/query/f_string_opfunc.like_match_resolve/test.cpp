/* PR#8196 (CBRD-27361) -- qstr_like_match_resolve () and its instances, src/query/string_opfunc.c.
 *
 * The page batch's LIKE leaf resolves a matcher once per (codeset, collation) and calls it per row
 * (expr_batch.c eb_raw_kid_str ()); the row path calls db_string_like () -> qstr_eval_like (), which dispatches to the
 * same byte-lockstep instance per call.  The contract: the resolved matcher answers what qstr_like_match () (the row
 * path's matcher, escape NULL) answers, and it is a lockstep instance exactly where qstr_eval_like () would take one.
 * A third case pins the develop property both paths rely on: a lockstep instance answers as the generic loop of
 * qstr_eval_like () (the collation's kind cleared for the reference call).
 *
 * Fakes (what a booted server has): envvar_get () (no CUBRID_* variable, lang_init_builtin () reads CUBRID_MSG_LANG),
 * er_set ()/er_errid ()/er_clear () (last error id).  Built-in collations are the real ones (language_support.c,
 * intl_support.c linked as extra TUs); the rest of the engine is stubbed and never called. */
#include "catch.hpp"

#include "string_opfunc.h"
#include "language_support.h"
#include "intl_support.h"
#include "dbtype_def.h"

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes */
#include "environment_variable.h"	/* C linkage */

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin () reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c body */
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

static int f_er_id = NO_ERROR;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  f_er_id = err_id;
}

int
er_errid (void)
{
  return f_er_id;
}

void
er_clear (void)
{
  f_er_id = NO_ERROR;
}

namespace
{
  bool f_boot ()
  {
    static bool done = false;
    if (!done)
      {
	done = true;
	lang_init_builtin ();
      }
    return lang_get_collation (LANG_COLL_UTF8_BINARY) != NULL;
  }

  bool f_is_instance_codeset (LANG_LOCKSTEP_KIND kind, INTL_CODESET cs)
  {
    return (kind == LANG_LOCKSTEP_UTF8 && cs == INTL_CODESET_UTF8)
      || (kind == LANG_LOCKSTEP_SB && cs == INTL_CODESET_ISO88591)
      || (kind == LANG_LOCKSTEP_BINARY && cs == INTL_CODESET_BINARY);
  }

  /* deterministic xorshift */
  struct f_rng
  {
    uint64_t s;
    uint32_t next ()
    {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      return (uint32_t) (s >> 11);
    }
  };

  /* characters of a codeset's alphabet: well-formed for UTF-8 (the column stores well-formed UTF-8) */
  std::vector < std::string > f_alphabet (INTL_CODESET cs)
  {
    std::vector < std::string > a = { "a", "b", "s", "p", " " };
    a.push_back (std::string (1, '\0'));
    if (cs == INTL_CODESET_UTF8)
      {
	a.push_back ("\xEA\xB0\x80");	/* U+AC00 */
	a.push_back ("\xC3\xA9");	/* U+00E9 */
      }
    else
      {
	a.push_back ("\xE9");
	a.push_back ("\xEA");
      }
    return a;
  }

  std::string f_word (f_rng & r, const std::vector < std::string > &alpha, int max_chars, bool wild)
  {
    std::string w;
    int n = (int) (r.next () % (unsigned) (max_chars + 1));
    for (int i = 0; i < n; i++)
      {
	uint32_t k = r.next () % 10;
	if (wild && k == 0)
	  {
	    w += '%';
	  }
	else if (wild && k == 1)
	  {
	    w += '_';
	  }
	else
	  {
	    w += alpha[r.next () % alpha.size ()];
	  }
      }
    return w;
  }

  std::vector < std::pair < std::string, std::string > > f_fixed_cases ()
  {
    return {
      {"", ""}, {"", "%"}, {"a", ""}, {"a", "%"}, {"abc", "a%"}, {"abc", "%c"}, {"abc", "%b%"}, {"abc", "_b_"},
      {"abc  ", "abc"}, {"abc  ", "%c"}, {"ab c", "ab%c"}, {"special requests", "%special%requests%"},
      {"specialrequests", "%special%requests%"}, {"requests special", "%special%requests%"},
      {"aab", "%ab"}, {"aaab", "%aab"}, {"ab", "a_b"}, {"a b", "a_b"}, {"abab", "%ab%ab"}, {"abab", "%ab%ab%ab"},
      {"x  ", "x"}, {"x  ", "x_"}, {"PROMO BRUSHED", "PROMO%"}, {"STANDARD BRASS", "%BRASS"},
      {"STANDARD BRASS ", "%BRASS"}, {"%", "%"}, {"_", "_"}, {"a%b", "a%b"}
    };
  }
}

TEST_CASE ("f_ resolve: a lockstep instance exactly where qstr_eval_like () dispatches to one, else qstr_like_match",
	   "[contract]")
{
  REQUIRE (f_boot ());
  const INTL_CODESET sets[] = { INTL_CODESET_ISO88591, INTL_CODESET_UTF8, INTL_CODESET_BINARY, INTL_CODESET_KSC5601_EUC };
  int n_specific = 0;

  for (int coll = 0; coll <= LANG_COLL_BUILTIN_MAX; coll++)
    {
      const LANG_COLLATION *lc = lang_get_collation (coll);
      REQUIRE (lc != NULL);
      for (INTL_CODESET cs:sets)
	{
	  QSTR_LIKE_MATCH_FN fn = qstr_like_match_resolve (cs, coll);
	  INFO ("collation " << coll << " kind " << (int) lc->byte_lockstep_kind << " codeset " << (int) cs);
	  REQUIRE (fn != NULL);
	  CHECK ((fn != qstr_like_match) == f_is_instance_codeset (lc->byte_lockstep_kind, cs));
	  n_specific += (fn != qstr_like_match) ? 1 : 0;
	}
    }
  /* iso88591_bin, utf8_bin and binary at least take an instance */
  CHECK (n_specific >= 3);
}

TEST_CASE ("f_ resolve: the resolved matcher answers as qstr_like_match () (the row path), every built-in collation",
	   "[contract]")
{
  REQUIRE (f_boot ());
  long compared = 0;

  for (int coll = 0; coll <= LANG_COLL_BUILTIN_MAX; coll++)
    {
      const LANG_COLLATION *lc = lang_get_collation (coll);
      INTL_CODESET cs = lc->codeset;
      QSTR_LIKE_MATCH_FN fn = qstr_like_match_resolve (cs, coll);
      std::vector < std::string > alpha = f_alphabet (cs);
      f_rng r = { 0x9E3779B97F4A7C15ULL + (uint64_t) coll };
      std::vector < std::pair < std::string, std::string > > cases = f_fixed_cases ();

      for (int i = 0; i < 4000; i++)
	{
	  cases.push_back ( {
			   f_word (r, alpha, 10, false), f_word (r, alpha, 6, true)});
	}
      for (const auto & c:cases)
	{
	  int a = fn (c.first.data (), (int) c.first.size (), c.second.data (), (int) c.second.size (), cs, coll);
	  int b = qstr_like_match (c.first.data (), (int) c.first.size (), c.second.data (), (int) c.second.size (), cs,
				   coll);
	  INFO ("collation " << coll << " target [" << c.first << "] pattern [" << c.second << "]");
	  REQUIRE (a == b);
	  compared++;
	}
    }
  CHECK (compared > 40000);
}

TEST_CASE ("f_ resolve: a lockstep instance answers as the generic loop it stands in for (develop property)",
	   "[contract]")
{
  REQUIRE (f_boot ());
  long compared = 0, differ = 0;
  std::string first_diff;

  for (int coll = 0; coll <= LANG_COLL_BUILTIN_MAX; coll++)
    {
      LANG_COLLATION *lc = lang_get_collation (coll);
      INTL_CODESET cs = lc->codeset;
      LANG_LOCKSTEP_KIND kind = lc->byte_lockstep_kind;

      if (!f_is_instance_codeset (kind, cs))
	{
	  continue;
	}
      QSTR_LIKE_MATCH_FN fn = qstr_like_match_resolve (cs, coll);
      std::vector < std::string > alpha = f_alphabet (cs);
      f_rng r = { 0xD1B54A32D192ED03ULL + (uint64_t) coll };
      std::vector < std::pair < std::string, std::string > > cases = f_fixed_cases ();

      for (int i = 0; i < 4000; i++)
	{
	  cases.push_back ( {
			   f_word (r, alpha, 10, false), f_word (r, alpha, 6, true)});
	}
      for (const auto & c:cases)
	{
	  int a = fn (c.first.data (), (int) c.first.size (), c.second.data (), (int) c.second.size (), cs, coll);
	  int b;

	  lc->byte_lockstep_kind = LANG_LOCKSTEP_NONE;	/* the generic loop for the reference */
	  b = qstr_like_match (c.first.data (), (int) c.first.size (), c.second.data (), (int) c.second.size (), cs, coll);
	  lc->byte_lockstep_kind = kind;
	  compared++;
	  if (a != b)
	    {
	      differ++;
	      if (first_diff.empty ())
		{
		  first_diff = "collation " + std::to_string (coll) + " target [" + c.first + "] pattern [" + c.second
		    + "] lockstep " + std::to_string (a) + " generic " + std::to_string (b);
		}
	    }
	}
    }
  INFO (first_diff);
  CHECK (compared > 10000);
  CHECK (differ == 0);
}
