/*
 * PR#8709 (CBRD-28298) -- db_to_number (), src/query/string_opfunc.c.
 *
 * TO_NUMBER (s, fmt) with a NULL format value (a column or a host variable) took strlen of no string and crashed;
 * a NULL literal format, folded by the parser through the same function, failed with ER_QSTR_INVALID_DATA_TYPE.
 * The PR returns NULL for both, in the user format branch only.
 *
 *   N1 a NULL VARCHAR format gives NULL and NO_ERROR (base: SIGSEGV, run in a child process)
 *   N2 a NULL of type NULL (the literal) gives NULL and NO_ERROR (base: ER_QSTR_INVALID_DATA_TYPE)
 *   N3 no user format: the format argument is not looked at, '12' gives 12 even when the argument is NULL
 *   N4 a user format '99.9' gives 12.5 for '12.5'
 *   N5 the source checks stay first: an empty source with a NULL format fails with ER_QSTR_EMPTY_STRING,
 *      a NULL source gives NULL
 *
 * Real code at THIS revision is compiled into the test binary (as unit/query/f_string_opfunc.string_append).
 */
#include "catch.hpp"
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#if __has_include("domain_rules.h")
#include "domain_rules.h"
#define UNITSEAM_HAS_CONVERTERS 1
#endif
#include "system_parameter.c"
#undef new
#include "language_support.c"
#undef new
#include "intl_support.c"
#undef new
#include "chartype.c"
#undef new
#include "mprec.c"
#undef new
#include "dtoa.c"
#undef new
#include "object_primitive.c"
#undef new
#if defined (UNITSEAM_HAS_CONVERTERS)
#include "object_domain_convert.cpp"
#undef new
#endif
#include "numeric_opfunc.c"
#undef new
#include "db_date.c"
#undef new
#include "db_macro.c"
#undef new
#include "string_opfunc.c"
#undef new
#undef cmp
#include "boot_sr.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

/* ---------------------------------------------------------------- fakes: what a booted server process has */

namespace cubthread
{
  std::size_t system_core_count (void)
  {
    return 8;
  }
}
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* DOWN: prm_get_value reads a session parameter's global value (no thread entry) */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

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
dk_deduplicate_key_attribute_initialized ()
{
}

static int fake_er_id = NO_ERROR;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  fake_er_id = err_id;
}

int
er_errid (void)
{
  return fake_er_id;
}

void
er_clear (void)
{
  fake_er_id = NO_ERROR;
}

/* memory_alloc.c: the private heap is malloc here; live blocks keep their size so a test can compare a recorded
 * capacity with the block it describes */
static std::unordered_map<void *, size_t> f_blocks;
static long f_allocs = 0;
static long f_allocs_4k = 0;

void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  void *p = malloc (size);
  if (p != NULL)
    {
      f_blocks[p] = size;
      f_allocs++;
      if (size >= 4096)
	{
	  f_allocs_4k++;
	}
    }
  return p;
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  if (ptr != NULL)
    {
      f_blocks.erase (ptr);
      free (ptr);
    }
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  void *p = realloc (ptr, size);
  if (p != NULL)
    {
      if (ptr != NULL)
	{
	  f_blocks.erase (ptr);
	}
      f_blocks[p] = size;
      f_allocs++;
    }
  return p;
}

struct fake_area
{
  size_t element_size;
};
AREA *
area_create (const char *, size_t element_size, size_t)
{
  return (AREA *) new fake_area { element_size };
}

void *
area_alloc (AREA * area)
{
  return calloc (1, ((fake_area *) area)->element_size);
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

static bool
boot_types ()
{
  static bool done = false;
  static bool ok = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      ok = tp_init () == NO_ERROR;
      lang_Initialized = true;
      ok = ok && lang_set_charset_lang ("en_US.utf8") == NO_ERROR;
    }
  return ok;
}

/* the number_lang argument as the parser builds it: the default number language, a user format or not */
static void
make_lang (DB_VALUE * lang, bool has_user_format)
{
  int flag = 0;
  REQUIRE (lang_set_flag_from_lang (NULL, has_user_format, false, &flag) == 0);
  db_make_int (lang, flag);
}

static int
to_number (const char *src, const DB_VALUE * fmt, bool has_user_format, DB_VALUE * res)
{
  DB_VALUE s, lang;
  if (src == NULL)
    {
      db_make_null (&s);
    }
  else
    {
      db_make_varchar (&s, DB_DEFAULT_PRECISION, src, (int) strlen (src), LANG_SYS_CODESET, LANG_SYS_COLLATION);
    }
  make_lang (&lang, has_user_format);
  er_clear ();
  db_make_null (res);
  return db_to_number (&s, fmt, &lang, res);
}

static void
null_varchar (DB_VALUE * v)
{
  db_value_domain_init (v, DB_TYPE_VARCHAR, DB_DEFAULT_PRECISION, 0);
  db_value_put_null (v);
}

static std::string
num_str (DB_VALUE * v)
{
  char buf[128];
  return numeric_db_value_print (v, buf);
}

TEST_CASE ("N1 a NULL VARCHAR format gives NULL and NO_ERROR", "[contract]")
{
  REQUIRE (boot_types ());
  fflush (NULL);
  pid_t pid = fork ();
  REQUIRE (pid >= 0);
  if (pid == 0)
    {
      /* die by the signal itself, not through Catch's handler that would write into the parent's report */
      signal (SIGSEGV, SIG_DFL);
      signal (SIGABRT, SIG_DFL);
      DB_VALUE f, r;
      null_varchar (&f);
      int rc = to_number ("12", &f, true, &r);
      _exit (rc == NO_ERROR && DB_IS_NULL (&r) ? 0 : 1);
    }
  int st = 0;
  waitpid (pid, &st, 0);
  INFO ("child status " << st << (WIFSIGNALED (st) ? " (signal)" : ""));
  CHECK (WIFEXITED (st));
  CHECK (WEXITSTATUS (st) == 0);
}

TEST_CASE ("N2 a NULL of type NULL (the folded literal) gives NULL and NO_ERROR", "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE f, r;
  db_make_null (&f);
  int rc = to_number ("12", &f, true, &r);
  CHECK (rc == NO_ERROR);
  CHECK (DB_IS_NULL (&r));
}

TEST_CASE ("N3 no user format: the format argument is not looked at", "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE f, r;
  db_make_null (&f);
  REQUIRE (to_number ("12", &f, false, &r) == NO_ERROR);
  CHECK (num_str (&r) == "12");
  pr_clear_value (&r);
  null_varchar (&f);
  REQUIRE (to_number ("12", &f, false, &r) == NO_ERROR);
  CHECK (num_str (&r) == "12");
  pr_clear_value (&r);
}

TEST_CASE ("N4 a user format '99.9' gives 12.5 for '12.5'", "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE f, r;
  db_make_varchar (&f, DB_DEFAULT_PRECISION, "99.9", 4, LANG_SYS_CODESET, LANG_SYS_COLLATION);
  REQUIRE (to_number ("12.5", &f, true, &r) == NO_ERROR);
  CHECK (num_str (&r) == "12.5");
  pr_clear_value (&r);
}

TEST_CASE ("N5 the source checks stay first: empty source fails, NULL source gives NULL, with a NULL format",
	   "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE f, r;
  null_varchar (&f);
  CHECK (to_number ("", &f, true, &r) == ER_QSTR_EMPTY_STRING);
  CHECK (to_number (NULL, &f, true, &r) == NO_ERROR);
  CHECK (DB_IS_NULL (&r));
}
