/*
 * PR#8022 (CBRD-27510) -- conversion boundaries of the casts (object_domain.c) and of the converters they call
 * (object_domain_convert.cpp, new in the PR), server mode, at both sides of the PR.
 *
 * The PR moved every value conversion out of tp_value_cast_internal into named type-pair converters
 * (tp_value_find_converter). The target TU is object_domain.c, which exists at every revision, so the same cases
 * run before the PR (a9940d67a), at its merge (be683e04c) and at develop head: what a cast answers for a
 * value at a boundary - status, error and value - is the contract.
 *
 *   C1  string -> INT / BIGINT / SHORT / DOUBLE / NUMERIC(4,2): parse failure, spaces, exponent, overflow
 *   C2  number -> number narrowing: rounding (half away from zero), overflow, NUMERIC precision / scale
 *   C3  tp_value_coerce_strict: a fraction is refused, an integral value converts, a string parses strictly
 *   C4  strict BIGINT at 2^63 is refused (a9940d67a..be683e04c wrap it to INT64_MIN; 16eb8474d refuses it)
 *   C5  string truncation: explicit OVERFLOW, force keeps the prefix
 *   C6  NUMERIC + - * / result precision and scale (numeric_opfunc.c)
 *   C7  (head only) tp_value_find_converter, every (src, dst, mode), equals the policy matrix written in
 *       object_domain_convert.cpp above tp_value_find_converter, with the server-build object cells: six before
 *       CBRD-27583 (PR#8213), five from it - the server converts OID <- OID too. Which one is decided at compile
 *       time: whether this server-mode TU has the OID converter (unitseam_oid_probe below)
 *   D1  [defect] DOUBLE / MONETARY / string in [2^31 - 0.5, 2^31) cast to INT wraps to INT_MIN (16eb8474d, #8087;
 *       fixed in e07b3b09a, #8253 - a history case since)
 *
 * Real code at THIS revision is compiled into the test binary (the test's -I resolves to the worktree):
 * system_parameter.c (prm_Def defaults), language_support.c, intl_support.c, chartype.c, mprec.c, dtoa.c,
 * object_primitive.c, object_domain_convert.cpp (when present), numeric_opfunc.c, db_date.c, db_macro.c,
 * string_opfunc.c. Faked: error manager, private allocator, area allocator and a few boot-time globals (below).
 */
#include "catch.hpp"
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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

/* ---------------------------------------------------------------- fakes: what a booted server process has */

/* system_parameter.c's static initializers (prm_Def's parallelism defaults, the regex engine keyword table) */
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

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this. DOWN makes prm_get_value (system_parameter.c:10829) read a
 * session parameter's global value - its default here - instead of a session's copy, which needs a thread entry */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c:494 body */
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

/* tp_init () before 5de66400a (#8104) names the deduplicate-key attributes; nothing under test reads them */
void
dk_deduplicate_key_attribute_initialized ()
{
}

/* error_manager.c: the last error id er_set raised, as er_errid () reads it; er_clear () resets it */
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

/* memory_alloc.c: the private heap of a thread is malloc here */
void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  free (ptr);
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  return realloc (ptr, size);
}

/* area_alloc.c: tp_init's domain area hands out zeroed element-sized blocks */
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

/* The boot order of a server (boot_restart_server): built-in collations, the type module, then the database's
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset (tp_apply_sys_charset) */
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

/* ---------------------------------------------------------------- values and domains from short specs */

static DB_VALUE
string_value (const char *text)
{
  DB_VALUE v;
  db_make_varchar (&v, DB_MAX_VARCHAR_PRECISION, text, (int) strlen (text), INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  return v;
}

static TP_DOMAIN *
numeric_domain (int precision, int scale)
{
  return tp_domain_resolve (DB_TYPE_NUMERIC, NULL, precision, scale, NULL, 0);
}

/* "VC:text" VARCHAR utf8_bin, "I:n" INT, "BI:n" BIGINT, "D:x" DOUBLE, "F:x" FLOAT, "M:x" MONETARY (dollar),
 * "N:p:s:text" NUMERIC(p,s) parsed from text */
static DB_VALUE
make_value (const std::string & spec)
{
  DB_VALUE v;
  db_make_null (&v);
  const size_t colon = spec.find (':');
  const std::string kind = spec.substr (0, colon), rest = spec.substr (colon + 1);
  if (kind == "VC")
    {
      /* the text outlives the value: specs are string literals of the tables below */
      v = string_value (spec.c_str () + colon + 1);
    }
  else if (kind == "I")
    {
      db_make_int (&v, (int) strtol (rest.c_str (), NULL, 10));
    }
  else if (kind == "BI")
    {
      db_make_bigint (&v, strtoll (rest.c_str (), NULL, 10));
    }
  else if (kind == "D")
    {
      db_make_double (&v, strtod (rest.c_str (), NULL));
    }
  else if (kind == "F")
    {
      db_make_float (&v, strtof (rest.c_str (), NULL));
    }
  else if (kind == "M")
    {
      db_make_monetary (&v, DB_CURRENCY_DOLLAR, strtod (rest.c_str (), NULL));
    }
  else if (kind == "N")
    {
      int p = 0, s = 0, n = 0;
      sscanf (rest.c_str (), "%d:%d:%n", &p, &s, &n);
      DB_VALUE text = string_value (rest.c_str () + n);
      REQUIRE (tp_value_cast (&text, &v, numeric_domain (p, s), false) == DOMAIN_COMPATIBLE);
    }
  else
    {
      FAIL ("bad value spec " << spec);
    }
  return v;
}

/* "INT", "SHORT", "BIGINT", "DOUBLE", "FLOAT", "TIME", "N:p:s", "VC:n" (VARCHAR(n) utf8_bin) */
static TP_DOMAIN *
make_domain (const std::string & spec)
{
  if (spec == "INT")
    {
      return tp_domain_resolve_default (DB_TYPE_INTEGER);
    }
  if (spec == "SHORT")
    {
      return tp_domain_resolve_default (DB_TYPE_SHORT);
    }
  if (spec == "BIGINT")
    {
      return tp_domain_resolve_default (DB_TYPE_BIGINT);
    }
  if (spec == "DOUBLE")
    {
      return tp_domain_resolve_default (DB_TYPE_DOUBLE);
    }
  if (spec == "FLOAT")
    {
      return tp_domain_resolve_default (DB_TYPE_FLOAT);
    }
  if (spec == "TIME")
    {
      return tp_domain_resolve_default (DB_TYPE_TIME);
    }
  int p = 0, s = 0;
  if (sscanf (spec.c_str (), "N:%d:%d", &p, &s) == 2)
    {
      return numeric_domain (p, s);
    }
  if (sscanf (spec.c_str (), "VC:%d", &p) == 1)
    {
      return tp_domain_resolve (DB_TYPE_VARCHAR, NULL, p, 0, NULL, LANG_COLL_UTF8_BINARY);
    }
  FAIL ("bad domain spec " << spec);
  return NULL;
}

static std::string
format_value (const DB_VALUE * v)
{
  char buf[256];
  if (DB_IS_NULL (v))
    {
      return "NULL";
    }
  switch (DB_VALUE_TYPE (v))
    {
    case DB_TYPE_INTEGER:
      snprintf (buf, sizeof (buf), "INT %d", db_get_int (v));
      break;
    case DB_TYPE_SHORT:
      snprintf (buf, sizeof (buf), "SHORT %d", db_get_short (v));
      break;
    case DB_TYPE_BIGINT:
      snprintf (buf, sizeof (buf), "BIGINT %lld", (long long) db_get_bigint (v));
      break;
    case DB_TYPE_DOUBLE:
      snprintf (buf, sizeof (buf), "DOUBLE %.17g", db_get_double (v));
      break;
    case DB_TYPE_FLOAT:
      snprintf (buf, sizeof (buf), "FLOAT %.9g", db_get_float (v));
      break;
    case DB_TYPE_NUMERIC:
      {
	char digits[NUMERIC_MAX_STRING_SIZE];
	numeric_db_value_print (v, digits);
	snprintf (buf, sizeof (buf), "NUMERIC(%d,%d) %s", DB_VALUE_PRECISION (v), DB_VALUE_SCALE (v), digits);
      }
      break;
    case DB_TYPE_VARCHAR:
    case DB_TYPE_CHAR:
      snprintf (buf, sizeof (buf), "STR(%d) '%.*s'", DB_VALUE_PRECISION (v), db_get_string_size (v), db_get_string (v));
      break;
    case DB_TYPE_TIME:
      {
	int h, m, s;
	db_time_decode (db_get_time (v), &h, &m, &s);
	snprintf (buf, sizeof (buf), "TIME %d:%d:%d", h, m, s);
      }
      break;
    default:
      snprintf (buf, sizeof (buf), "type %d", (int) DB_VALUE_TYPE (v));
      break;
    }
  return buf;
}

static const char *
status_name (int s)
{
  switch (s)
    {
    case DOMAIN_COMPATIBLE:
      return "COMPAT";
    case DOMAIN_INCOMPATIBLE:
      return "INCOMPAT";
    case DOMAIN_OVERFLOW:
      return "OVERFLOW";
    case DOMAIN_ERROR:
      return "ERROR";
    default:
      return "OTHER";
    }
}

/* modes: 0 tp_value_cast (explicit), 1 tp_value_coerce (implicit), 2 tp_value_cast_force, 3 tp_value_coerce_strict.
 * The outcome reads "status|er_errid|value"; a strict coercion that fails gives "FAILED|er|-": its callers clear the
 * destination without reading it (btree.c:19174, domain_resolve.c:2074), so what is left there is not part of it. */
static std::string
convert (const std::string & value_spec, const std::string & domain_spec, int mode)
{
  DB_VALUE src = make_value (value_spec), dst;
  TP_DOMAIN *domain = make_domain (domain_spec);
  db_make_null (&dst);
  er_clear ();
  int s;
  switch (mode)
    {
    case 0:
      s = tp_value_cast (&src, &dst, domain, false);
      break;
    case 1:
      s = tp_value_coerce (&src, &dst, domain);
      break;
    case 2:
      s = tp_value_cast_force (&src, &dst, domain, false);
      break;
    default:
      s = tp_value_coerce_strict (&src, &dst, domain);
      break;
    }
  const int error = er_errid ();
  std::string out;
  if (mode == 3)
    {
      out = std::string (s == NO_ERROR ? "OK" : "FAILED") + "|" + std::to_string (error) + "|"
	+ (s == NO_ERROR ? format_value (&dst) : "-");
    }
  else
    {
      out = std::string (status_name (s)) + "|" + std::to_string (error) + "|" + format_value (&dst);
    }
  pr_clear_value (&dst);
  return out;
}

struct conversion_case
{
  const char *value;
  const char *domain;
  int mode;
  const char *expected;
};

/* UNITSEAM_CAPTURE=1 prints every row as an initializer (how the tables were made: at a9940d67a, then checked
 * equal at be683e04c) */
static void
check_conversions (const conversion_case * cases, size_t n)
{
  const bool capture = getenv ("UNITSEAM_CAPTURE") != NULL;
  for (size_t i = 0; i < n; i++)
    {
      const conversion_case & c = cases[i];
      const std::string got = convert (c.value, c.domain, c.mode);
      if (capture)
	{
	  printf ("  {\"%s\", \"%s\", %d, \"%s\"},\n", c.value, c.domain, c.mode, got.c_str ());
	  continue;
	}
      INFO ("cast " << c.value << " -> " << c.domain << " mode " << c.mode);
      CHECK (got == c.expected);
    }
}

struct numeric_op_case
{
  const char *left;
  char op;
  const char *right;
  const char *expected;		/* "rc|er_errid|value" */
};

static const conversion_case c1_string_to_number[] = {
  {"VC:12", "INT", 0, "COMPAT|0|INT 12"},
  {"VC:12", "INT", 1, "COMPAT|0|INT 12"},
  {"VC:12", "INT", 2, "COMPAT|0|INT 12"},
  {"VC:12", "BIGINT", 0, "COMPAT|0|BIGINT 12"},
  {"VC:12", "BIGINT", 1, "COMPAT|0|BIGINT 12"},
  {"VC:12", "BIGINT", 2, "COMPAT|0|BIGINT 12"},
  {"VC:12", "DOUBLE", 0, "COMPAT|0|DOUBLE 12"},
  {"VC:12", "DOUBLE", 1, "COMPAT|0|DOUBLE 12"},
  {"VC:12", "DOUBLE", 2, "COMPAT|0|DOUBLE 12"},
  {"VC:12", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC:12", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC:12", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC: 12 ", "INT", 0, "COMPAT|0|INT 12"},
  {"VC: 12 ", "INT", 1, "COMPAT|0|INT 12"},
  {"VC: 12 ", "INT", 2, "COMPAT|0|INT 12"},
  {"VC: 12 ", "BIGINT", 0, "COMPAT|0|BIGINT 12"},
  {"VC: 12 ", "BIGINT", 1, "COMPAT|0|BIGINT 12"},
  {"VC: 12 ", "BIGINT", 2, "COMPAT|0|BIGINT 12"},
  {"VC: 12 ", "DOUBLE", 0, "COMPAT|0|DOUBLE 12"},
  {"VC: 12 ", "DOUBLE", 1, "COMPAT|0|DOUBLE 12"},
  {"VC: 12 ", "DOUBLE", 2, "COMPAT|0|DOUBLE 12"},
  {"VC: 12 ", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC: 12 ", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC: 12 ", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) 12.00"},
  {"VC:12a", "INT", 0, "INCOMPAT|0|NULL"},
  {"VC:12a", "INT", 1, "INCOMPAT|0|NULL"},
  {"VC:12a", "INT", 2, "INCOMPAT|0|NULL"},
  {"VC:12a", "BIGINT", 0, "INCOMPAT|0|NULL"},
  {"VC:12a", "BIGINT", 1, "INCOMPAT|0|NULL"},
  {"VC:12a", "BIGINT", 2, "INCOMPAT|0|NULL"},
  {"VC:12a", "DOUBLE", 0, "INCOMPAT|0|NULL"},
  {"VC:12a", "DOUBLE", 1, "INCOMPAT|0|NULL"},
  {"VC:12a", "DOUBLE", 2, "INCOMPAT|0|NULL"},
  {"VC:12a", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:12a", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:12a", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:", "INT", 0, "COMPAT|0|INT 0"},
  {"VC:", "INT", 1, "COMPAT|0|INT 0"},
  {"VC:", "INT", 2, "COMPAT|0|INT 0"},
  {"VC:", "BIGINT", 0, "COMPAT|0|BIGINT 0"},
  {"VC:", "BIGINT", 1, "COMPAT|0|BIGINT 0"},
  {"VC:", "BIGINT", 2, "COMPAT|0|BIGINT 0"},
  {"VC:", "DOUBLE", 0, "COMPAT|0|DOUBLE 0"},
  {"VC:", "DOUBLE", 1, "COMPAT|0|DOUBLE 0"},
  {"VC:", "DOUBLE", 2, "COMPAT|0|DOUBLE 0"},
  {"VC:", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:1e3", "INT", 0, "COMPAT|0|INT 1000"},
  {"VC:1e3", "INT", 1, "COMPAT|0|INT 1000"},
  {"VC:1e3", "INT", 2, "COMPAT|0|INT 1000"},
  {"VC:1e3", "BIGINT", 0, "COMPAT|0|BIGINT 1000"},
  {"VC:1e3", "BIGINT", 1, "COMPAT|0|BIGINT 1000"},
  {"VC:1e3", "BIGINT", 2, "COMPAT|0|BIGINT 1000"},
  {"VC:1e3", "DOUBLE", 0, "COMPAT|0|DOUBLE 1000"},
  {"VC:1e3", "DOUBLE", 1, "COMPAT|0|DOUBLE 1000"},
  {"VC:1e3", "DOUBLE", 2, "COMPAT|0|DOUBLE 1000"},
  {"VC:1e3", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:1e3", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:1e3", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:12.5", "INT", 0, "COMPAT|0|INT 13"},
  {"VC:12.5", "INT", 1, "COMPAT|0|INT 13"},
  {"VC:12.5", "INT", 2, "COMPAT|0|INT 13"},
  {"VC:12.5", "BIGINT", 0, "COMPAT|0|BIGINT 13"},
  {"VC:12.5", "BIGINT", 1, "COMPAT|0|BIGINT 13"},
  {"VC:12.5", "BIGINT", 2, "COMPAT|0|BIGINT 13"},
  {"VC:12.5", "DOUBLE", 0, "COMPAT|0|DOUBLE 12.5"},
  {"VC:12.5", "DOUBLE", 1, "COMPAT|0|DOUBLE 12.5"},
  {"VC:12.5", "DOUBLE", 2, "COMPAT|0|DOUBLE 12.5"},
  {"VC:12.5", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.50"},
  {"VC:12.5", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.50"},
  {"VC:12.5", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) 12.50"},
  {"VC:-12.5", "INT", 0, "COMPAT|0|INT -13"},
  {"VC:-12.5", "INT", 1, "COMPAT|0|INT -13"},
  {"VC:-12.5", "INT", 2, "COMPAT|0|INT -13"},
  {"VC:-12.5", "BIGINT", 0, "COMPAT|0|BIGINT -13"},
  {"VC:-12.5", "BIGINT", 1, "COMPAT|0|BIGINT -13"},
  {"VC:-12.5", "BIGINT", 2, "COMPAT|0|BIGINT -13"},
  {"VC:-12.5", "DOUBLE", 0, "COMPAT|0|DOUBLE -12.5"},
  {"VC:-12.5", "DOUBLE", 1, "COMPAT|0|DOUBLE -12.5"},
  {"VC:-12.5", "DOUBLE", 2, "COMPAT|0|DOUBLE -12.5"},
  {"VC:-12.5", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) -12.50"},
  {"VC:-12.5", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) -12.50"},
  {"VC:-12.5", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) -12.50"},
  {"VC:-0.5", "INT", 0, "COMPAT|0|INT -1"},
  {"VC:-0.5", "INT", 1, "COMPAT|0|INT -1"},
  {"VC:-0.5", "INT", 2, "COMPAT|0|INT -1"},
  {"VC:-0.5", "BIGINT", 0, "COMPAT|0|BIGINT -1"},
  {"VC:-0.5", "BIGINT", 1, "COMPAT|0|BIGINT -1"},
  {"VC:-0.5", "BIGINT", 2, "COMPAT|0|BIGINT -1"},
  {"VC:-0.5", "DOUBLE", 0, "COMPAT|0|DOUBLE -0.5"},
  {"VC:-0.5", "DOUBLE", 1, "COMPAT|0|DOUBLE -0.5"},
  {"VC:-0.5", "DOUBLE", 2, "COMPAT|0|DOUBLE -0.5"},
  {"VC:-0.5", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) -0.50"},
  {"VC:-0.5", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) -0.50"},
  {"VC:-0.5", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) -0.50"},
  {"VC:2147483647", "INT", 0, "COMPAT|0|INT 2147483647"},
  {"VC:2147483647", "INT", 1, "COMPAT|0|INT 2147483647"},
  {"VC:2147483647", "INT", 2, "COMPAT|0|INT 2147483647"},
  {"VC:2147483647", "BIGINT", 0, "COMPAT|0|BIGINT 2147483647"},
  {"VC:2147483647", "BIGINT", 1, "COMPAT|0|BIGINT 2147483647"},
  {"VC:2147483647", "BIGINT", 2, "COMPAT|0|BIGINT 2147483647"},
  {"VC:2147483647", "DOUBLE", 0, "COMPAT|0|DOUBLE 2147483647"},
  {"VC:2147483647", "DOUBLE", 1, "COMPAT|0|DOUBLE 2147483647"},
  {"VC:2147483647", "DOUBLE", 2, "COMPAT|0|DOUBLE 2147483647"},
  {"VC:2147483647", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:2147483647", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:2147483647", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "INT", 1, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "INT", 2, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "BIGINT", 0, "COMPAT|0|BIGINT 2147483648"},
  {"VC:2147483648", "BIGINT", 1, "COMPAT|0|BIGINT 2147483648"},
  {"VC:2147483648", "BIGINT", 2, "COMPAT|0|BIGINT 2147483648"},
  {"VC:2147483648", "DOUBLE", 0, "COMPAT|0|DOUBLE 2147483648"},
  {"VC:2147483648", "DOUBLE", 1, "COMPAT|0|DOUBLE 2147483648"},
  {"VC:2147483648", "DOUBLE", 2, "COMPAT|0|DOUBLE 2147483648"},
  {"VC:2147483648", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:2147483648", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:-2147483648", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"VC:-2147483648", "INT", 1, "COMPAT|0|INT -2147483648"},
  {"VC:-2147483648", "INT", 2, "COMPAT|0|INT -2147483648"},
  {"VC:-2147483648", "BIGINT", 0, "COMPAT|0|BIGINT -2147483648"},
  {"VC:-2147483648", "BIGINT", 1, "COMPAT|0|BIGINT -2147483648"},
  {"VC:-2147483648", "BIGINT", 2, "COMPAT|0|BIGINT -2147483648"},
  {"VC:-2147483648", "DOUBLE", 0, "COMPAT|0|DOUBLE -2147483648"},
  {"VC:-2147483648", "DOUBLE", 1, "COMPAT|0|DOUBLE -2147483648"},
  {"VC:-2147483648", "DOUBLE", 2, "COMPAT|0|DOUBLE -2147483648"},
  {"VC:-2147483648", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:-2147483648", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:-2147483648", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "INT", 1, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "INT", 2, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "BIGINT", 0, "COMPAT|0|BIGINT -2147483649"},
  {"VC:-2147483649", "BIGINT", 1, "COMPAT|0|BIGINT -2147483649"},
  {"VC:-2147483649", "BIGINT", 2, "COMPAT|0|BIGINT -2147483649"},
  {"VC:-2147483649", "DOUBLE", 0, "COMPAT|0|DOUBLE -2147483649"},
  {"VC:-2147483649", "DOUBLE", 1, "COMPAT|0|DOUBLE -2147483649"},
  {"VC:-2147483649", "DOUBLE", 2, "COMPAT|0|DOUBLE -2147483649"},
  {"VC:-2147483649", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:-2147483649", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "INT", 1, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "INT", 2, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "BIGINT", 0, "COMPAT|0|BIGINT 9223372036854775807"},
  {"VC:9223372036854775807", "BIGINT", 1, "COMPAT|0|BIGINT 9223372036854775807"},
  {"VC:9223372036854775807", "BIGINT", 2, "COMPAT|0|BIGINT 9223372036854775807"},
  {"VC:9223372036854775807", "DOUBLE", 0, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775807", "DOUBLE", 1, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775807", "DOUBLE", 2, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775807", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775807", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "INT", 1, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "INT", 2, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "BIGINT", 0, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "BIGINT", 1, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "BIGINT", 2, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "DOUBLE", 0, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775808", "DOUBLE", 1, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775808", "DOUBLE", 2, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"VC:9223372036854775808", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:9223372036854775808", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:99999", "INT", 0, "COMPAT|0|INT 99999"},
  {"VC:99999", "INT", 1, "COMPAT|0|INT 99999"},
  {"VC:99999", "INT", 2, "COMPAT|0|INT 99999"},
  {"VC:99999", "BIGINT", 0, "COMPAT|0|BIGINT 99999"},
  {"VC:99999", "BIGINT", 1, "COMPAT|0|BIGINT 99999"},
  {"VC:99999", "BIGINT", 2, "COMPAT|0|BIGINT 99999"},
  {"VC:99999", "DOUBLE", 0, "COMPAT|0|DOUBLE 99999"},
  {"VC:99999", "DOUBLE", 1, "COMPAT|0|DOUBLE 99999"},
  {"VC:99999", "DOUBLE", 2, "COMPAT|0|DOUBLE 99999"},
  {"VC:99999", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:99999", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:99999", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:abc", "INT", 0, "INCOMPAT|0|NULL"},
  {"VC:abc", "INT", 1, "INCOMPAT|0|NULL"},
  {"VC:abc", "INT", 2, "INCOMPAT|0|NULL"},
  {"VC:abc", "BIGINT", 0, "INCOMPAT|0|NULL"},
  {"VC:abc", "BIGINT", 1, "INCOMPAT|0|NULL"},
  {"VC:abc", "BIGINT", 2, "INCOMPAT|0|NULL"},
  {"VC:abc", "DOUBLE", 0, "INCOMPAT|0|NULL"},
  {"VC:abc", "DOUBLE", 1, "INCOMPAT|0|NULL"},
  {"VC:abc", "DOUBLE", 2, "INCOMPAT|0|NULL"},
  {"VC:abc", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:abc", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:abc", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:1e400", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:1e400", "INT", 1, "OVERFLOW|0|NULL"},
  {"VC:1e400", "INT", 2, "OVERFLOW|0|NULL"},
  {"VC:1e400", "BIGINT", 0, "INCOMPAT|0|NULL"},
  {"VC:1e400", "BIGINT", 1, "INCOMPAT|0|NULL"},
  {"VC:1e400", "BIGINT", 2, "INCOMPAT|0|NULL"},
  {"VC:1e400", "DOUBLE", 0, "OVERFLOW|0|NULL"},
  {"VC:1e400", "DOUBLE", 1, "OVERFLOW|0|NULL"},
  {"VC:1e400", "DOUBLE", 2, "OVERFLOW|0|NULL"},
  {"VC:1e400", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:1e400", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:1e400", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:12.345", "INT", 0, "COMPAT|0|INT 12"},
  {"VC:12.345", "INT", 1, "COMPAT|0|INT 12"},
  {"VC:12.345", "INT", 2, "COMPAT|0|INT 12"},
  {"VC:12.345", "BIGINT", 0, "COMPAT|0|BIGINT 12"},
  {"VC:12.345", "BIGINT", 1, "COMPAT|0|BIGINT 12"},
  {"VC:12.345", "BIGINT", 2, "COMPAT|0|BIGINT 12"},
  {"VC:12.345", "DOUBLE", 0, "COMPAT|0|DOUBLE 12.345000000000001"},
  {"VC:12.345", "DOUBLE", 1, "COMPAT|0|DOUBLE 12.345000000000001"},
  {"VC:12.345", "DOUBLE", 2, "COMPAT|0|DOUBLE 12.345000000000001"},
  {"VC:12.345", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"VC:12.345", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"VC:12.345", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"VC:123.456", "INT", 0, "COMPAT|0|INT 123"},
  {"VC:123.456", "INT", 1, "COMPAT|0|INT 123"},
  {"VC:123.456", "INT", 2, "COMPAT|0|INT 123"},
  {"VC:123.456", "BIGINT", 0, "COMPAT|0|BIGINT 123"},
  {"VC:123.456", "BIGINT", 1, "COMPAT|0|BIGINT 123"},
  {"VC:123.456", "BIGINT", 2, "COMPAT|0|BIGINT 123"},
  {"VC:123.456", "DOUBLE", 0, "COMPAT|0|DOUBLE 123.456"},
  {"VC:123.456", "DOUBLE", 1, "COMPAT|0|DOUBLE 123.456"},
  {"VC:123.456", "DOUBLE", 2, "COMPAT|0|DOUBLE 123.456"},
  {"VC:123.456", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"VC:123.456", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"VC:123.456", "N:4:2", 2, "OVERFLOW|0|NULL"},
  {"VC:0x10", "INT", 0, "COMPAT|0|INT 16"},
  {"VC:0x10", "INT", 1, "COMPAT|0|INT 16"},
  {"VC:0x10", "INT", 2, "COMPAT|0|INT 16"},
  {"VC:0x10", "BIGINT", 0, "COMPAT|0|BIGINT 16"},
  {"VC:0x10", "BIGINT", 1, "COMPAT|0|BIGINT 16"},
  {"VC:0x10", "BIGINT", 2, "COMPAT|0|BIGINT 16"},
  {"VC:0x10", "DOUBLE", 0, "COMPAT|0|DOUBLE 16"},
  {"VC:0x10", "DOUBLE", 1, "COMPAT|0|DOUBLE 16"},
  {"VC:0x10", "DOUBLE", 2, "COMPAT|0|DOUBLE 16"},
  {"VC:0x10", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:0x10", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:0x10", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:+7", "INT", 0, "COMPAT|0|INT 7"},
  {"VC:+7", "INT", 1, "COMPAT|0|INT 7"},
  {"VC:+7", "INT", 2, "COMPAT|0|INT 7"},
  {"VC:+7", "BIGINT", 0, "COMPAT|0|BIGINT 7"},
  {"VC:+7", "BIGINT", 1, "COMPAT|0|BIGINT 7"},
  {"VC:+7", "BIGINT", 2, "COMPAT|0|BIGINT 7"},
  {"VC:+7", "DOUBLE", 0, "COMPAT|0|DOUBLE 7"},
  {"VC:+7", "DOUBLE", 1, "COMPAT|0|DOUBLE 7"},
  {"VC:+7", "DOUBLE", 2, "COMPAT|0|DOUBLE 7"},
  {"VC:+7", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 7.00"},
  {"VC:+7", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 7.00"},
  {"VC:+7", "N:4:2", 2, "COMPAT|0|NUMERIC(4,2) 7.00"},
  {"VC:--1", "INT", 0, "INCOMPAT|0|NULL"},
  {"VC:--1", "INT", 1, "INCOMPAT|0|NULL"},
  {"VC:--1", "INT", 2, "INCOMPAT|0|NULL"},
  {"VC:--1", "BIGINT", 0, "INCOMPAT|0|NULL"},
  {"VC:--1", "BIGINT", 1, "INCOMPAT|0|NULL"},
  {"VC:--1", "BIGINT", 2, "INCOMPAT|0|NULL"},
  {"VC:--1", "DOUBLE", 0, "INCOMPAT|0|NULL"},
  {"VC:--1", "DOUBLE", 1, "INCOMPAT|0|NULL"},
  {"VC:--1", "DOUBLE", 2, "INCOMPAT|0|NULL"},
  {"VC:--1", "N:4:2", 0, "INCOMPAT|0|NULL"},
  {"VC:--1", "N:4:2", 1, "INCOMPAT|0|NULL"},
  {"VC:--1", "N:4:2", 2, "INCOMPAT|0|NULL"},
  {"VC:99999", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"VC:99999", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"VC:99999", "SHORT", 2, "OVERFLOW|0|NULL"},
  {"VC:12", "SHORT", 0, "COMPAT|0|SHORT 12"},
  {"VC:12", "SHORT", 1, "COMPAT|0|SHORT 12"},
  {"VC:12", "SHORT", 2, "COMPAT|0|SHORT 12"},
  {"VC:-32769", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"VC:-32769", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"VC:-32769", "SHORT", 2, "OVERFLOW|0|NULL"},
  {"VC:32767", "SHORT", 0, "COMPAT|0|SHORT 32767"},
  {"VC:32767", "SHORT", 1, "COMPAT|0|SHORT 32767"},
  {"VC:32767", "SHORT", 2, "COMPAT|0|SHORT 32767"},
  {"VC:-32768", "SHORT", 0, "COMPAT|0|SHORT -32768"},
  {"VC:-32768", "SHORT", 1, "COMPAT|0|SHORT -32768"},
  {"VC:-32768", "SHORT", 2, "COMPAT|0|SHORT -32768"},
};
static const conversion_case c2_number_narrowing[] = {
  {"D:2.5", "INT", 0, "COMPAT|0|INT 3"},
  {"D:2.5", "INT", 1, "COMPAT|0|INT 3"},
  {"D:-2.5", "INT", 0, "COMPAT|0|INT -3"},
  {"D:-2.5", "INT", 1, "COMPAT|0|INT -3"},
  {"D:2.0", "INT", 0, "COMPAT|0|INT 2"},
  {"D:2.0", "INT", 1, "COMPAT|0|INT 2"},
  {"D:0.49999999999999994", "INT", 0, "COMPAT|0|INT 1"},
  {"D:0.49999999999999994", "INT", 1, "COMPAT|0|INT 1"},
  {"D:1e10", "INT", 0, "OVERFLOW|0|NULL"},
  {"D:1e10", "INT", 1, "OVERFLOW|0|NULL"},
  {"D:-2147483648.4", "INT", 0, "OVERFLOW|0|NULL"},
  {"D:-2147483648.4", "INT", 1, "OVERFLOW|0|NULL"},
  {"D:-2147483648.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"D:-2147483648.5", "INT", 1, "OVERFLOW|0|NULL"},
  {"F:1.5", "INT", 0, "COMPAT|0|INT 2"},
  {"F:1.5", "INT", 1, "COMPAT|0|INT 2"},
  {"F:2147483648", "INT", 0, "OVERFLOW|0|NULL"},
  {"F:2147483648", "INT", 1, "OVERFLOW|0|NULL"},
  {"BI:2147483648", "INT", 0, "OVERFLOW|0|NULL"},
  {"BI:2147483648", "INT", 1, "OVERFLOW|0|NULL"},
  {"BI:-2147483649", "INT", 0, "OVERFLOW|0|NULL"},
  {"BI:-2147483649", "INT", 1, "OVERFLOW|0|NULL"},
  {"M:12.5", "INT", 0, "COMPAT|0|INT 13"},
  {"M:12.5", "INT", 1, "COMPAT|0|INT 13"},
  {"D:32767.5", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"D:32767.5", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"D:32767.4", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"D:32767.4", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"D:-32768.5", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"D:-32768.5", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"BI:40000", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"BI:40000", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"N:6:1:32767.5", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"N:6:1:32767.5", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"N:6:1:-32768.5", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"N:6:1:-32768.5", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"N:3:1:12.5", "SHORT", 0, "COMPAT|0|SHORT 13"},
  {"N:3:1:12.5", "SHORT", 1, "COMPAT|0|SHORT 13"},
  {"N:3:1:-12.5", "SHORT", 0, "COMPAT|0|SHORT -13"},
  {"N:3:1:-12.5", "SHORT", 1, "COMPAT|0|SHORT -13"},
  {"D:9223372036854774784", "BIGINT", 0, "COMPAT|0|BIGINT 9223372036854774784"},
  {"D:9223372036854774784", "BIGINT", 1, "COMPAT|0|BIGINT 9223372036854774784"},
  {"D:9223372036854775808", "BIGINT", 0, "OVERFLOW|0|NULL"},
  {"D:9223372036854775808", "BIGINT", 1, "OVERFLOW|0|NULL"},
  {"N:19:0:9223372036854775807", "BIGINT", 0, "COMPAT|0|BIGINT 9223372036854775807"},
  {"N:19:0:9223372036854775807", "BIGINT", 1, "COMPAT|0|BIGINT 9223372036854775807"},
  {"N:19:0:9223372036854775808", "BIGINT", 0, "OVERFLOW|0|NULL"},
  {"N:19:0:9223372036854775808", "BIGINT", 1, "OVERFLOW|0|NULL"},
  {"N:19:0:-9223372036854775808", "BIGINT", 0, "COMPAT|0|BIGINT -9223372036854775808"},
  {"N:19:0:-9223372036854775808", "BIGINT", 1, "COMPAT|0|BIGINT -9223372036854775808"},
  {"N:20:1:9223372036854775807.5", "BIGINT", 0, "OVERFLOW|0|NULL"},
  {"N:20:1:9223372036854775807.5", "BIGINT", 1, "OVERFLOW|0|NULL"},
  {"N:38:0:12345678901234567890123456789012345678", "BIGINT", 0, "OVERFLOW|0|NULL"},
  {"N:38:0:12345678901234567890123456789012345678", "BIGINT", 1, "OVERFLOW|0|NULL"},
  {"N:11:1:2147483647.4", "INT", 0, "OVERFLOW|0|NULL"},
  {"N:11:1:2147483647.4", "INT", 1, "OVERFLOW|0|NULL"},
  {"N:11:1:2147483647.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"N:11:1:2147483647.5", "INT", 1, "OVERFLOW|0|NULL"},
  {"N:11:1:-2147483648.4", "INT", 0, "OVERFLOW|0|NULL"},
  {"N:11:1:-2147483648.4", "INT", 1, "OVERFLOW|0|NULL"},
  {"N:11:1:-2147483648.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"N:11:1:-2147483648.5", "INT", 1, "OVERFLOW|0|NULL"},
  {"N:10:0:2147483647", "INT", 0, "COMPAT|0|INT 2147483647"},
  {"N:10:0:2147483647", "INT", 1, "COMPAT|0|INT 2147483647"},
  {"N:10:0:2147483648", "INT", 0, "OVERFLOW|0|NULL"},
  {"N:10:0:2147483648", "INT", 1, "OVERFLOW|0|NULL"},
  {"N:3:1:12.5", "INT", 0, "COMPAT|0|INT 13"},
  {"N:3:1:12.5", "INT", 1, "COMPAT|0|INT 13"},
  {"N:3:1:-12.5", "INT", 0, "COMPAT|0|INT -13"},
  {"N:3:1:-12.5", "INT", 1, "COMPAT|0|INT -13"},
  {"N:5:3:99.995", "INT", 0, "COMPAT|0|INT 100"},
  {"N:5:3:99.995", "INT", 1, "COMPAT|0|INT 100"},
  {"N:5:3:12.345", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"N:5:3:12.345", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"N:5:3:99.995", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"N:5:3:99.995", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"N:5:3:99.994", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 99.99"},
  {"N:5:3:99.994", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 99.99"},
  {"N:5:3:-99.995", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"N:5:3:-99.995", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"N:4:3:0.005", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 0.01"},
  {"N:4:3:0.005", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 0.01"},
  {"N:38:0:12345678901234567890123456789012345678", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"N:38:0:12345678901234567890123456789012345678", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"I:99", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 99.00"},
  {"I:99", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 99.00"},
  {"I:100", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"I:100", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"I:-100", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"I:-100", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"D:12.345", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"D:12.345", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.35"},
  {"D:12.355", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 12.36"},
  {"D:12.355", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 12.36"},
  {"D:99.995", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"D:99.995", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"D:123.456", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"D:123.456", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"F:0.1", "N:4:2", 0, "COMPAT|0|NUMERIC(4,2) 0.10"},
  {"F:0.1", "N:4:2", 1, "COMPAT|0|NUMERIC(4,2) 0.10"},
  {"BI:9223372036854775807", "N:4:2", 0, "OVERFLOW|0|NULL"},
  {"BI:9223372036854775807", "N:4:2", 1, "OVERFLOW|0|NULL"},
  {"D:1e30", "N:38:5", 0, "COMPAT|0|NUMERIC(38,5) 1000000000000000000000000000000.00000"},
  {"D:1e30", "N:38:5", 1, "COMPAT|0|NUMERIC(38,5) 1000000000000000000000000000000.00000"},
  {"D:0.1", "N:38:5", 0, "COMPAT|0|NUMERIC(38,5) 0.10000"},
  {"D:0.1", "N:38:5", 1, "COMPAT|0|NUMERIC(38,5) 0.10000"},
  {"D:-0.005", "N:38:5", 0, "COMPAT|0|NUMERIC(38,5) -0.00500"},
  {"D:-0.005", "N:38:5", 1, "COMPAT|0|NUMERIC(38,5) -0.00500"},
  {"BI:9223372036854775807", "N:38:5", 0, "COMPAT|0|NUMERIC(38,5) 9223372036854775807.00000"},
  {"BI:9223372036854775807", "N:38:5", 1, "COMPAT|0|NUMERIC(38,5) 9223372036854775807.00000"},
  {"N:5:3:12.345", "N:38:5", 0, "COMPAT|0|NUMERIC(38,5) 12.34500"},
  {"N:5:3:12.345", "N:38:5", 1, "COMPAT|0|NUMERIC(38,5) 12.34500"},
  {"N:5:3:12.345", "DOUBLE", 0, "COMPAT|0|DOUBLE 12.345000000000001"},
  {"N:5:3:12.345", "DOUBLE", 1, "COMPAT|0|DOUBLE 12.345000000000001"},
  {"N:5:3:12.345", "FLOAT", 0, "COMPAT|0|FLOAT 12.3450003"},
  {"N:5:3:12.345", "FLOAT", 1, "COMPAT|0|FLOAT 12.3450003"},
  {"N:38:0:12345678901234567890123456789012345678", "DOUBLE", 0, "COMPAT|0|DOUBLE 1.2345678901234568e+37"},
  {"N:38:0:12345678901234567890123456789012345678", "DOUBLE", 1, "COMPAT|0|DOUBLE 1.2345678901234568e+37"},
  {"N:38:0:12345678901234567890123456789012345678", "FLOAT", 0, "COMPAT|0|FLOAT 1.23456784e+37"},
  {"N:38:0:12345678901234567890123456789012345678", "FLOAT", 1, "COMPAT|0|FLOAT 1.23456784e+37"},
  {"N:19:0:9223372036854775807", "DOUBLE", 0, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"N:19:0:9223372036854775807", "DOUBLE", 1, "COMPAT|0|DOUBLE 9.2233720368547758e+18"},
  {"N:19:0:9223372036854775807", "FLOAT", 0, "COMPAT|0|FLOAT 9.22337204e+18"},
  {"N:19:0:9223372036854775807", "FLOAT", 1, "COMPAT|0|FLOAT 9.22337204e+18"},
  {"I:-3", "TIME", 0, "COMPAT|-787|TIME 0:0:-1"},
  {"I:-3", "TIME", 1, "COMPAT|-787|TIME 0:0:-1"},
  {"I:90000", "TIME", 0, "COMPAT|0|TIME 1:0:0"},
  {"I:90000", "TIME", 1, "COMPAT|0|TIME 1:0:0"},
  {"D:3661.5", "TIME", 0, "COMPAT|0|TIME 1:1:2"},
  {"D:3661.5", "TIME", 1, "COMPAT|0|TIME 1:1:2"},
};
static const conversion_case c3_strict[] = {
  {"D:2.5", "INT", 3, "FAILED|0|-"},
  {"D:2.0", "INT", 3, "OK|0|INT 2"},
  {"D:-2.0", "INT", 3, "OK|0|INT -2"},
  {"D:2147483647.0", "INT", 3, "OK|0|INT 2147483647"},
  {"D:-2147483648.0", "INT", 3, "OK|0|INT -2147483648"},
  {"D:2147483648.0", "INT", 3, "FAILED|0|-"},
  {"F:1.5", "INT", 3, "FAILED|0|-"},
  {"F:2.0", "INT", 3, "OK|0|INT 2"},
  {"VC:12", "INT", 3, "OK|0|INT 12"},
  {"VC:12.5", "INT", 3, "FAILED|0|-"},
  {"VC:12a", "INT", 3, "FAILED|0|-"},
  {"VC: 12 ", "INT", 3, "OK|0|INT 12"},
  {"VC:2147483647", "INT", 3, "OK|0|INT 2147483647"},
  {"VC:2147483648", "INT", 3, "FAILED|0|-"},
  {"N:3:1:12.5", "INT", 3, "FAILED|0|-"},
  {"N:3:1:12.0", "INT", 3, "FAILED|0|-"},
  {"BI:2147483648", "INT", 3, "FAILED|0|-"},
  {"BI:12", "INT", 3, "OK|0|INT 12"},
  {"M:12.0", "INT", 3, "OK|0|INT 12"},
  {"M:12.5", "INT", 3, "FAILED|0|-"},
  {"I:12", "N:4:2", 3, "OK|0|NUMERIC(4,2) 12.00"},
  {"D:12.5", "N:4:2", 3, "FAILED|0|-"},
  {"VC:12.345", "N:4:2", 3, "OK|0|NUMERIC(4,2) 12.35"},
  {"VC:123.456", "N:4:2", 3, "FAILED|0|-"},
  {"N:5:3:12.345", "N:4:2", 3, "OK|0|NUMERIC(5,3) 12.345"},
  {"N:5:3:12.340", "N:4:2", 3, "OK|0|NUMERIC(5,3) 12.340"},
  {"I:5", "DOUBLE", 3, "OK|0|DOUBLE 5"},
  {"VC:abc", "DOUBLE", 3, "FAILED|0|-"},
  {"VC:1e3", "DOUBLE", 3, "OK|0|DOUBLE 1000"},
  {"N:5:3:12.345", "DOUBLE", 3, "FAILED|0|-"},
  {"I:5", "VC:3", 3, "FAILED|0|-"},
  {"VC:abc", "VC:3", 3, "FAILED|0|-"},
  {"BI:-9223372036854775808", "BIGINT", 3, "OK|0|BIGINT -9223372036854775808"},
  {"D:-9223372036854775808", "BIGINT", 3, "OK|0|BIGINT -9223372036854775808"},
  {"VC:-9223372036854775808", "BIGINT", 3, "OK|0|BIGINT -9223372036854775808"},
};
static const conversion_case c4_strict_bigint_bound[] = {
  {"VC:9223372036854775806", "BIGINT", 3, "FAILED|0|-"},
  {"VC:9223372036854775807", "BIGINT", 3, "FAILED|0|-"},
  {"VC:9223372036854775808", "BIGINT", 3, "FAILED|0|-"},
  {"D:9223372036854775808", "BIGINT", 3, "FAILED|0|-"},
};
static const conversion_case c5_truncation[] = {
  {"VC:abcdef", "VC:3", 0, "OVERFLOW|0|NULL"},
  {"VC:abcdef", "VC:3", 1, "OVERFLOW|0|NULL"},
  {"VC:abcdef", "VC:3", 2, "COMPAT|0|STR(3) 'abc'"},
  {"VC:abc", "VC:3", 0, "COMPAT|0|STR(3) 'abc'"},
  {"VC:abc", "VC:3", 1, "COMPAT|0|STR(3) 'abc'"},
  {"VC:abc", "VC:3", 2, "COMPAT|0|STR(3) 'abc'"},
  {"I:12345", "VC:3", 0, "OVERFLOW|0|NULL"},
  {"I:12345", "VC:3", 1, "INCOMPAT|0|NULL"},
  {"I:12345", "VC:3", 2, "OVERFLOW|0|NULL"},
  {"I:123", "VC:3", 0, "COMPAT|0|STR(3) '123'"},
  {"I:123", "VC:3", 1, "INCOMPAT|0|NULL"},
  {"I:123", "VC:3", 2, "COMPAT|0|STR(3) '123'"},
  {"D:1.5", "VC:3", 0, "COMPAT|0|STR(3) '1.5'"},
  {"D:1.5", "VC:3", 1, "INCOMPAT|0|NULL"},
  {"D:1.5", "VC:3", 2, "COMPAT|0|STR(3) '1.5'"},
};
static const conversion_case d1_int_wrap[] = {
  {"D:2147483647.5", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"D:2147483647.5", "INT", 1, "COMPAT|0|INT -2147483648"},
  {"D:2147483647.5", "INT", 2, "COMPAT|0|INT -2147483648"},
  {"D:2147483647.999", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"D:2147483647.999", "INT", 1, "COMPAT|0|INT -2147483648"},
  {"D:2147483647.999", "INT", 2, "COMPAT|0|INT -2147483648"},
  {"M:2147483647.5", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"M:2147483647.5", "INT", 1, "COMPAT|0|INT -2147483648"},
  {"M:2147483647.5", "INT", 2, "COMPAT|0|INT -2147483648"},
  {"VC:2147483647.5", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"VC:2147483647.5", "INT", 1, "COMPAT|0|INT -2147483648"},
  {"VC:2147483647.5", "INT", 2, "COMPAT|0|INT -2147483648"},
};
static const numeric_op_case c6_numeric_ops[] = {
  {"N:4:2:12.34", '+', "N:4:3:1.234", "0|0|NUMERIC(40,0) 13.574"},
  {"N:4:3:1.234", '+', "N:4:2:12.34", "0|0|NUMERIC(40,0) 13.574"},
  {"N:38:0:99999999999999999999999999999999999999", '+', "N:38:0:99999999999999999999999999999999999999", "0|0|NUMERIC(40,0) 199999999999999999999999999999999999998"},
  {"N:4:2:12.34", '+', "N:11:10:0.0000000001", "0|0|NUMERIC(40,0) 12.3400000001"},
  {"N:1:0:3", '+', "N:4:2:12.34", "0|0|NUMERIC(40,0) 15.34"},
  {"N:38:8:123456789012345678901234567890.12345678", '+', "N:38:8:123456789012345678901234567890.12345678", "0|0|NUMERIC(40,0) 246913578024691357802469135780.24691356"},
  {"N:38:8:123456789012345678901234567890.12345678", '+', "N:4:2:12.34", "0|0|NUMERIC(40,0) 123456789012345678901234567902.46345678"},
  {"N:3:1:-0.5", '+', "N:1:0:3", "0|0|NUMERIC(40,0) 2.5"},
  {"N:10:0:2147483647", '+', "N:10:0:2147483647", "0|0|NUMERIC(40,0) 4294967294"},
  {"N:4:2:12.34", '+', "N:3:1:-0.5", "0|0|NUMERIC(40,0) 11.84"},
  {"N:4:2:12.34", '-', "N:4:3:1.234", "0|0|NUMERIC(40,0) 11.106"},
  {"N:4:3:1.234", '-', "N:4:2:12.34", "0|0|NUMERIC(40,0) -11.106"},
  {"N:38:0:99999999999999999999999999999999999999", '-', "N:38:0:99999999999999999999999999999999999999", "0|0|NUMERIC(40,0) 0"},
  {"N:4:2:12.34", '-', "N:11:10:0.0000000001", "0|0|NUMERIC(40,0) 12.3399999999"},
  {"N:1:0:3", '-', "N:4:2:12.34", "0|0|NUMERIC(40,0) -9.34"},
  {"N:38:8:123456789012345678901234567890.12345678", '-', "N:38:8:123456789012345678901234567890.12345678", "0|0|NUMERIC(40,0) 0.00000000"},
  {"N:38:8:123456789012345678901234567890.12345678", '-', "N:4:2:12.34", "0|0|NUMERIC(40,0) 123456789012345678901234567877.78345678"},
  {"N:3:1:-0.5", '-', "N:1:0:3", "0|0|NUMERIC(40,0) -3.5"},
  {"N:10:0:2147483647", '-', "N:10:0:2147483647", "0|0|NUMERIC(40,0) 0"},
  {"N:4:2:12.34", '-', "N:3:1:-0.5", "0|0|NUMERIC(40,0) 12.84"},
  {"N:4:2:12.34", '*', "N:4:3:1.234", "0|0|NUMERIC(40,0) 15.22756"},
  {"N:4:3:1.234", '*', "N:4:2:12.34", "0|0|NUMERIC(40,0) 15.22756"},
  {"N:38:0:99999999999999999999999999999999999999", '*', "N:38:0:99999999999999999999999999999999999999", "-427|0|NULL"},
  {"N:4:2:12.34", '*', "N:11:10:0.0000000001", "0|0|NUMERIC(40,0) 0.000000001234"},
  {"N:1:0:3", '*', "N:4:2:12.34", "0|0|NUMERIC(40,0) 37.02"},
  {"N:38:8:123456789012345678901234567890.12345678", '*', "N:38:8:123456789012345678901234567890.12345678", "-427|0|NULL"},
  {"N:38:8:123456789012345678901234567890.12345678", '*', "N:4:2:12.34", "0|0|NUMERIC(40,0) 1523456776412345677641234567764.1234566652"},
  {"N:3:1:-0.5", '*', "N:1:0:3", "0|0|NUMERIC(40,0) -1.5"},
  {"N:10:0:2147483647", '*', "N:10:0:2147483647", "0|0|NUMERIC(40,0) 4611686014132420609"},
  {"N:4:2:12.34", '*', "N:3:1:-0.5", "0|0|NUMERIC(40,0) -6.170"},
  {"N:4:2:12.34", '/', "N:4:3:1.234", "0|0|NUMERIC(40,0) 10.000000000"},
  {"N:4:3:1.234", '/', "N:4:2:12.34", "0|0|NUMERIC(40,0) 0.100000000"},
  {"N:38:0:99999999999999999999999999999999999999", '/', "N:38:0:99999999999999999999999999999999999999", "0|0|NUMERIC(40,0) 1.00"},
  {"N:4:2:12.34", '/', "N:11:10:0.0000000001", "0|0|NUMERIC(40,0) 123400000000.0000000000"},
  {"N:1:0:3", '/', "N:4:2:12.34", "0|0|NUMERIC(40,0) 0.243111831"},
  {"N:38:8:123456789012345678901234567890.12345678", '/', "N:38:8:123456789012345678901234567890.12345678", "0|0|NUMERIC(40,0) 1.00000000"},
  {"N:38:8:123456789012345678901234567890.12345678", '/', "N:4:2:12.34", "0|0|NUMERIC(40,0) 10004602026932388889889349099.68585549"},
  {"N:3:1:-0.5", '/', "N:1:0:3", "0|0|NUMERIC(40,0) -0.166666667"},
  {"N:10:0:2147483647", '/', "N:10:0:2147483647", "0|0|NUMERIC(40,0) 1.000000000"},
  {"N:4:2:12.34", '/', "N:3:1:-0.5", "0|0|NUMERIC(40,0) -24.680000000"},
};

/* ---------------------------------------------------------------- NUMERIC arithmetic (numeric_opfunc.c) */

static std::string
numeric_op (const numeric_op_case & c)
{
  DB_VALUE a = make_value (c.left), b = make_value (c.right), r;
  db_make_null (&r);
  er_clear ();
  int rc = c.op == '+' ? numeric_db_value_add (&a, &b, &r) : c.op == '-' ? numeric_db_value_sub (&a, &b, &r)
    : c.op == '*' ? numeric_db_value_mul (&a, &b, &r) : numeric_db_value_div (&a, &b, &r);
  return std::to_string (rc) + "|" + std::to_string (er_errid ()) + "|" + format_value (&r);
}

/* ---------------------------------------------------------------- the conversion policy matrix (head) */

#if defined (UNITSEAM_HAS_CONVERTERS)
/* object_domain_convert.cpp, the comment above tp_value_find_converter at 4adcd4090 (row = dst, column = src, DB_TYPE
 * order): '.' no mode converts, '=' same type (nullptr), 'o' one converter in every mode, 'c' every mode with the strict
 * converter for COMPARE / OPERAND, 'a' ASSIGN and IMPLICIT only; upper case: IMPLICIT refuses it as well. */
struct policy_row
{
  int dst;
  const char *name;
  const char *cells;
};
static const policy_row policy_matrix[] = {
  {0, "NULL", "=........................................"},
  {1, "INTEGER", ".=ccc........c....c...c..c.....c...o....o"},
  {2, "FLOAT", ".c=cc........c....c...c..c.....c...o....o"},
  {3, "DOUBLE", ".cc=c........c....c...c..c.....c...o....o"},
  {4, "VARCHAR", ".OOOo.....OOOO....O...OOOo.....OO.AoOOOOO"},
  {5, "OBJECT", ".....o..........o..oo...................."},
  {6, "SET", "......ooo................................"},
  {7, "MULTISET", "......ooo................................"},
  {8, "SEQUENCE", "......ooo................................"},
  {9, "ELO", ".........=..............................."},
  {10, "TIME", ".aaac.....=a.a....a......c.....aa..aaaaao"},
  {11, "TIMESTAMP", ".aaac......=ca....a...a..c.....ac..acccco"},
  {12, "DATE", "....c......c=............c......c..acccco"},
  {13, "MONETARY", ".cccc........=....c...c..c.....c...o....o"},
  {14, "VARIABLE", "........................................."},
  {15, "SUB", "........................................."},
  {16, "POINTER", "................=........................"},
  {17, "ERROR", ".................=......................."},
  {18, "SHORT", ".cccc........c....=...c..c.....c...o....o"},
  {19, "VOBJ", ".....o.............oo...................."},
  {20, "OID", "....................o...................."},
  {21, "DB_VALUE", "........................................."},
  {22, "NUMERIC", ".cccc........c....c...c..c.....c...o....o"},
  {23, "BIT", "....O..................ooO.......A.o....o"},
  {24, "VARBIT", "....O..................ooO.......A.o....o"},
  {25, "CHAR", ".OOOo.....OOOO....O...OOOo.....OO.AoOOOOO"},
  {26, "NCHAR_DEPRECATED", "........................................."},
  {27, "VARNCHAR_DEPRECATED", "........................................."},
  {28, "RESULTSET", "............................=............"},
  {29, "MIDXKEY", "........................................."},
  {30, "TABLE", "........................................."},
  {31, "BIGINT", ".cccc........c....c...c..c.....=...o....o"},
  {32, "DATETIME", "....c......cc............c......=..acccco"},
  {33, "BLOB", "....A..................AAA.......=.A....O"},
  {34, "CLOB", "....A....................A........=A....O"},
  {35, "ENUMERATION", ".oooo.....oooo....o...oooo.....ooAAoooooo"},
  {36, "TIMESTAMPTZ", ".aaac......cca....a...a..c.....ac..a=ccco"},
  {37, "TIMESTAMPLTZ", ".aaac......cca....a...a..c.....ac..ac=cco"},
  {38, "DATETIMETZ", "....c......cc............c......c..acc=co"},
  {39, "DATETIMELTZ", "....c......cc............c......c..accc=o"},
  {40, "JSON", ".oooO.............o...o..O.....o........o"},
};

/* CBRD-27583 (PR#8213) opened OID <- OID on the server too ("as tp_value_cast_internal clones an OID into an OID
 * domain"): its converter tp_value_convert_oid_to_oid, static in object_domain_convert.cpp (included above), is
 * compiled into a server-mode TU only from that revision - before it, the function sits under
 * #if !defined (SERVER_MODE). Unqualified lookup of the name below finds the static function where it is compiled
 * (an exact match beats the ellipsis) and the fallback otherwise: the revision, decided at compile time. */
namespace unitseam_oid_probe
{
  struct absent
  {
  };
  absent tp_value_convert_oid_to_oid (...);
}
namespace unitseam_oid_probe_use
{
  using namespace unitseam_oid_probe;
  constexpr bool server_converts_oid =
    !std::is_same<decltype (tp_value_convert_oid_to_oid (nullptr, nullptr, nullptr, nullptr)),
		  unitseam_oid_probe::absent>::value;
}

/* The comment's server cells, '.' there: "the server build differs in the six object cells OBJECT <- OBJECT, POINTER,
 * VOBJ, OID and VOBJ <- OBJECT, OID <- OID" before CBRD-27583; "in the five object cells OBJECT <- OBJECT, POINTER,
 * VOBJ, OID and VOBJ <- OBJECT" from it (OID <- OID is 'o' on the server as well) */
static bool
server_object_cell (int dst, int src, bool server_converts_oid)
{
  return (dst == DB_TYPE_OBJECT && (src == DB_TYPE_OBJECT || src == DB_TYPE_POINTER || src == DB_TYPE_VOBJ
				    || src == DB_TYPE_OID))
    || (dst == DB_TYPE_VOBJ && src == DB_TYPE_OBJECT)
    || (!server_converts_oid && dst == DB_TYPE_OID && src == DB_TYPE_OID);
}

static char
policy_cell (DB_TYPE src, const TP_DOMAIN * dst)
{
  const TP_VALUE_CONVERTER incompatible = tp_value_find_converter (DB_TYPE_INTEGER, NULL, DOMAIN_CONVERT_ASSIGN);
  TP_VALUE_CONVERTER m[4];
  for (int k = 0; k < 4; k++)
    {
      m[k] = tp_value_find_converter (src, dst, (DOMAIN_CONVERT_MODE) k);
    }
  const bool assign = m[DOMAIN_CONVERT_ASSIGN] != incompatible, implicit = m[DOMAIN_CONVERT_IMPLICIT] != incompatible;
  const bool compare = m[DOMAIN_CONVERT_COMPARE] != incompatible, operand = m[DOMAIN_CONVERT_OPERAND] != incompatible;
  if (m[DOMAIN_CONVERT_ASSIGN] == nullptr)
    {
      return '=';
    }
  if (!assign && !implicit && !compare && !operand)
    {
      return '.';
    }
  if (assign && compare && operand && m[DOMAIN_CONVERT_ASSIGN] == m[DOMAIN_CONVERT_COMPARE]
      && m[DOMAIN_CONVERT_COMPARE] == m[DOMAIN_CONVERT_OPERAND])
    {
      return implicit ? 'o' : 'O';
    }
  if (assign && compare && operand)
    {
      return implicit ? 'c' : 'C';
    }
  if (assign && !compare && !operand)
    {
      return implicit ? 'a' : 'A';
    }
  return '?';
}
#endif

/* ---------------------------------------------------------------- cases */

TEST_CASE ("C1 string to number: parse failure, spaces, exponent and overflow keep their status, error and value",
	   "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (c1_string_to_number, sizeof (c1_string_to_number) / sizeof (c1_string_to_number[0]));
}

TEST_CASE ("C2 number narrowing: rounding half away from zero, overflow, NUMERIC precision and scale", "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (c2_number_narrowing, sizeof (c2_number_narrowing) / sizeof (c2_number_narrowing[0]));
}

TEST_CASE ("C3 strict coercion: a fraction or a refused target fails, an integral value converts", "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (c3_strict, sizeof (c3_strict) / sizeof (c3_strict[0]));
}

TEST_CASE ("C4 strict BIGINT at 2^63 is refused (a9940d67a and be683e04c wrap it to INT64_MIN; 16eb8474d refuses)",
	   "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (c4_strict_bigint_bound, sizeof (c4_strict_bigint_bound) / sizeof (c4_strict_bigint_bound[0]));
}

TEST_CASE ("C5 string truncation: explicit and implicit overflow, force keeps the prefix", "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (c5_truncation, sizeof (c5_truncation) / sizeof (c5_truncation[0]));
}

TEST_CASE ("C6 NUMERIC + - * / result precision and scale", "[contract]")
{
  REQUIRE (boot_types ());
  const bool capture = getenv ("UNITSEAM_CAPTURE") != NULL;
  for (const numeric_op_case & c : c6_numeric_ops)
    {
      const std::string got = numeric_op (c);
      if (capture)
	{
	  printf ("  {\"%s\", '%c', \"%s\", \"%s\"},\n", c.left, c.op, c.right, got.c_str ());
	  continue;
	}
      INFO (c.left << " " << c.op << " " << c.right);
      CHECK (got == c.expected);
    }
}

TEST_CASE ("C7 tp_value_find_converter over every (src, dst, mode) is the policy matrix its comment writes "
	   "(server build: its object cells '.' - six, five once the server converts OID <- OID, CBRD-27583)",
	   "[contract]")
{
#if defined (UNITSEAM_HAS_CONVERTERS)
  REQUIRE (boot_types ());
  const bool server_oid = unitseam_oid_probe_use::server_converts_oid;
  const char *revision = server_oid ? "server converts OID <- OID (CBRD-27583): five server object cells '.'"
    : "no OID <- OID converter on the server (before CBRD-27583): six server object cells '.'";
  WARN ("C7 revision: " << revision);
  INFO ("C7 revision: " << revision);
  int cells = 0;
  for (const policy_row & row : policy_matrix)
    {
      const TP_DOMAIN *dst = tp_domain_resolve_default ((DB_TYPE) row.dst);
      if (dst == NULL || (int) TP_DOMAIN_TYPE (dst) != row.dst)
	{
	  /* DB_VALUE, the two deprecated national types and TABLE have no domain of their own type: no caller can
	   * ask for them (tp_value_find_converter reads the type off the domain) */
	  continue;
	}
      for (int src = 0; row.cells[src] != '\0'; src++)
	{
	  const char want = server_object_cell (row.dst, src, server_oid) ? '.' : row.cells[src];
	  INFO ("dst " << row.name << " (" << row.dst << ") src " << src << " documented '" << row.cells[src] << "'");
	  CHECK (policy_cell ((DB_TYPE) src, dst) == want);
	  cells++;
	}
    }
  CHECK (cells == 37 * 41);
#else
  FAIL ("absent at this revision: object_domain_convert.cpp (tp_value_find_converter) is new in PR#8022");
#endif
}

TEST_CASE ("PR8087 F1: DOUBLE, MONETARY and string in [2^31 - 0.5, 2^31) cast to INT wrap to INT_MIN instead of "
	   "overflowing (PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  /* 16eb8474d (#8087) made tp_numeric_overflow<INTEGER> compare a floating value against 2^31 (OR_CHECK_INT_OVERFLOW_
   * FROM_FP) before tp_value_convert_number rounds it (ROUND: x + .5): 2147483647.5 passes, rounds to 2^31, and the
   * narrowing static_cast<int> - undefined - gives INT_MIN on x86. OR_CHECK_ASSIGN_OVERFLOW guards BIGINT and
   * FLOAT -> INT only. Before it (a9940d67a, be683e04c) every row here was OVERFLOW. */
  check_conversions (d1_int_wrap, sizeof (d1_int_wrap) / sizeof (d1_int_wrap[0]));
}

/* ---------------------------------------------------------------- PR#8253 (CBRD-27660), merged as e07b3b09a */

/* What this PR guarantees: a value that rounds out of the INTEGER range overflows, a NaN overflows into SMALLINT */
static const conversion_case pr8253_contract[] = {
  {"D:2147483647.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"D:2147483647.5", "INT", 1, "OVERFLOW|0|NULL"},
  {"D:2147483647.5", "INT", 2, "OVERFLOW|0|NULL"},
  {"D:2147483647.999", "INT", 0, "OVERFLOW|0|NULL"},
  {"M:2147483647.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:2147483647.5", "INT", 0, "OVERFLOW|0|NULL"},
  {"VC:2147483647.5", "INT", 1, "OVERFLOW|0|NULL"},
  {"D:-2147483647.5", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"VC:-2147483648", "INT", 0, "COMPAT|0|INT -2147483648"},
  {"VC:32767", "SHORT", 0, "COMPAT|0|SHORT 32767"},
  {"VC:-32768", "SHORT", 0, "COMPAT|0|SHORT -32768"},
  {"D:32766.5", "SHORT", 0, "COMPAT|0|SHORT 32767"},
  {"D:nan", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"D:nan", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"D:nan", "SHORT", 2, "OVERFLOW|0|NULL"},
  {"D:nan", "SHORT", 3, "FAILED|0|-"},
  {"F:nan", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"M:nan", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"VC:nan", "SHORT", 0, "OVERFLOW|0|NULL"},
  {"VC:nan", "SHORT", 1, "OVERFLOW|0|NULL"},
  {"D:nan", "INT", 0, "OVERFLOW|0|NULL"},
  {"D:nan", "BIGINT", 0, "OVERFLOW|0|NULL"},
};

TEST_CASE ("PR8253 C8: a value rounding out of INT overflows, a NaN overflows into SHORT, exact limits convert",
	   "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (pr8253_contract, sizeof (pr8253_contract) / sizeof (pr8253_contract[0]));
}

/* ROUND (x) is x + .5 in double: in [2^52, 2^53) the spacing is 1, so x + .5 is a tie and rounds to even */
static const conversion_case pr8253_f2_bigint_tie[] = {
  {"D:4503599627370497", "BIGINT", 0, "COMPAT|0|BIGINT 4503599627370498"},
  {"D:4503599627370497", "BIGINT", 1, "COMPAT|0|BIGINT 4503599627370498"},
  {"D:-4503599627370497", "BIGINT", 0, "COMPAT|0|BIGINT -4503599627370498"},
  {"M:4503599627370497", "BIGINT", 0, "COMPAT|0|BIGINT 4503599627370498"},
};

TEST_CASE ("PR8253 F2: DOUBLE and MONETARY odd integers in [2^52, 2^53) cast to BIGINT come out one too large "
	   "(PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  check_conversions (pr8253_f2_bigint_tie, sizeof (pr8253_f2_bigint_tie) / sizeof (pr8253_f2_bigint_tie[0]));
}

/* tp_value_convert_double_to_time / _monetary_to_time: OR_CHECK_INT_OVERFLOW passes a NaN, (int) ROUND (NaN) is
 * undefined (x86: INT_MIN), the negative remainder fails db_time_encode_core, which writes -1, and the converter
 * ignores that and answers COMPATIBLE; FLOAT checks with the _FROM_FP form and overflows */
static const conversion_case pr8253_f3_nan_time[] = {
  {"D:nan", "TIME", 0, "COMPAT|-787|TIME 0:0:-1"},
  {"M:nan", "TIME", 0, "COMPAT|-787|TIME 0:0:-1"},
};

TEST_CASE ("PR8253 F3: a DOUBLE or MONETARY NaN cast to TIME gives an invalid TIME instead of overflowing "
	   "(PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  check_conversions (pr8253_f3_nan_time, sizeof (pr8253_f3_nan_time) / sizeof (pr8253_f3_nan_time[0]));
}

static const conversion_case pr8253_f3_control[] = {
  {"F:nan", "TIME", 0, "OVERFLOW|0|NULL"},
  {"D:nan", "TIME", 0, "OVERFLOW|0|NULL"},
  {"M:nan", "TIME", 0, "OVERFLOW|0|NULL"},
  {"D:3661.4", "TIME", 0, "COMPAT|0|TIME 1:1:1"},
};

TEST_CASE ("PR8253 C10: a FLOAT, DOUBLE or MONETARY NaN overflows into TIME, a DOUBLE in range converts", "[contract]")
{
  REQUIRE (boot_types ());
  check_conversions (pr8253_f3_control, sizeof (pr8253_f3_control) / sizeof (pr8253_f3_control[0]));
}

/* every number type: a negative value gives the same invalid TIME (the time converters ignore the encode error) */
static const conversion_case pr8253_f4_negative_time[] = {
  {"I:-1", "TIME", 0, "COMPAT|-787|TIME 0:0:-1"},
  {"D:-1", "TIME", 0, "COMPAT|-787|TIME 0:0:-1"},
};

TEST_CASE ("PR8253 F4: a negative number cast to TIME gives an invalid TIME (PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  check_conversions (pr8253_f4_negative_time, sizeof (pr8253_f4_negative_time) / sizeof (pr8253_f4_negative_time[0]));
}
