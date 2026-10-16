/*
 * PR#8022 (CBRD-27510) -- comparisons and collations resolved before any row (src/query/domain_rules.c, server mode;
 * new in the PR, so there is no base to run against: the contract is the row-time function each rule says it keeps).
 *
 *   K1  a comparison resolved from the two keys (domain_resolve_comparison + domain_compare_values) answers what
 *       tp_value_compare_with_error answers on the values: result, comparability, error - 41 values, every pair
 *   K2  the key pair table's cell is the comparison computed for the two keys (domain_compute_comparison, static)
 *   K3  domain_compare_by_type_pair (collections, JSON scalars, partition bounds) answers as tp_value_compare_with_error
 *       for values whose codeset is their collation's
 *   K4  argument order: comparing (b, a) reverses (a, b), with the same comparability and error (a value whose
 *       codeset is not its collation's is asymmetric in tp_value_compare_with_error itself: K1 covers it)
 *   K5  domain_value_domain gives the cached domain tp_domain_resolve_value gives (the optdebug cross-check, run in
 *       release) - garbage values included
 *   K6  a character result's collation (domain_resolve_character, a LEAVE node): CONCAT / REPLACE merge in operand
 *       order (LANG_RT_COMMON_COLL), FIRST takes the source's, SUBSTRING_INDEX rejects a delimiter that does not
 *       merge, a branch node with two domains is the resolver's (-1383 here, merged by the caller)
 *   K7  branch merge and pick (ELT, CASE over binds): the merged collation, CHAR stays CHAR, -1150 when they do not
 *   K8  common value (NVL, COALESCE ...): tp_infer_common_domain folded left to right, either order
 *   K9  set-operation / CTE column (LIST_COLUMN): a NULL branch takes the other's domain, a variable string keeps the
 *       first, any other difference is -456 - qfile_unify_types' rule (list_file.c, a9940d67a)
 *   F2  [defect] (latent) for a string whose codeset is not its collation's, domain_compare_by_type_pair copies the
 *       table cell of the key it does not have and answers otherwise than tp_value_compare_with_error
 *
 * Real code at THIS revision compiled into the test binary: system_parameter.c, language_support.c, intl_support.c,
 * chartype.c, mprec.c, dtoa.c, object_primitive.c, object_domain.c, object_domain_convert.cpp, numeric_opfunc.c,
 * db_date.c, db_macro.c, string_opfunc.c. hooks.cpp exposes domain_compute_comparison (static in the TU).
 */
#include "catch.hpp"
/* first: mprec.h (via the sources below) defines cmp, a member name of DOMAIN_COMPARE */
#include "domain_rules.h"
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
#include "object_domain.c"
#undef new
#include "object_domain_convert.cpp"
#undef new
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
#include <string>
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

/* ---------------------------------------------------------------- hooks.cpp (in domain_rules.c) */
bool unitseam_compare_equal (const DOMAIN_COMPARE * a, const DOMAIN_COMPARE * b);
int unitseam_compute_comparison (const DOMAIN_COMPARE_KEY * lhs, const DOMAIN_COMPARE_KEY * rhs,
				 DOMAIN_COMPARE * result);

/* ---------------------------------------------------------------- values */

struct named_value
{
  std::string name;
  DB_VALUE v;
  bool garbage;			/* a string or ENUM whose codeset is not its collation's, or a precision no column has */
};

static std::vector<named_value> g_values;

static void
add (const char *name, const DB_VALUE & v, bool garbage = false)
{
  g_values.push_back ({name, v, garbage});
}

static DB_VALUE
numeric (const char *text, int p, int s)
{
  DB_VALUE src, dst;
  db_make_string (&src, text);
  db_make_null (&dst);
  REQUIRE (tp_value_cast (&src, &dst, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0), false)
	   == DOMAIN_COMPATIBLE);
  return dst;
}

static DB_VALUE
varchar (const char *s, int codeset, int collation, int precision = DB_MAX_VARCHAR_PRECISION)
{
  DB_VALUE v;
  db_make_varchar (&v, precision, s, (int) strlen (s), codeset, collation);
  return v;
}

static DB_VALUE
fixed_char (const char *s, int precision, int codeset, int collation)
{
  DB_VALUE v;
  db_make_char (&v, precision, s, (int) strlen (s), codeset, collation);
  return v;
}

static void
build_values ()
{
  if (!g_values.empty ())
    {
      return;
    }
  static unsigned char bits16[] = { 0x12, 0x34 }, bits8[] = { 0x12 };
  DB_VALUE v;
  db_make_null (&v);
  add ("NULL", v);
  db_make_short (&v, 12);
  add ("SHORT 12", v);
  db_make_int (&v, 12);
  add ("INT 12", v);
  db_make_int (&v, -3);
  add ("INT -3", v);
  db_make_bigint (&v, 12);
  add ("BIGINT 12", v);
  db_make_bigint (&v, 1099511627776LL);
  add ("BIGINT 2^40", v);
  db_make_float (&v, 1.5f);
  add ("FLOAT 1.5", v);
  db_make_double (&v, 12.0);
  add ("DOUBLE 12", v);
  db_make_double (&v, 12.5);
  add ("DOUBLE 12.5", v);
  db_make_double (&v, 1e300);
  add ("DOUBLE 1e300", v);
  add ("NUMERIC(4,2) 12.00", numeric ("12.00", 4, 2));
  add ("NUMERIC(2,1) 1.5", numeric ("1.5", 2, 1));
  add ("NUMERIC(38,0) 1e37", numeric ("10000000000000000000000000000000000000", 38, 0));
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, 12.0);
  add ("MONETARY 12", v);
  add ("VARCHAR iso_bin '12'", varchar ("12", INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY));
  add ("VARCHAR utf8_bin '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_en_ci '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI));
  add ("VARCHAR utf8_bin 'abc'", varchar ("abc", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_en_ci 'ABC'", varchar ("ABC", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI));
  add ("VARCHAR utf8_en_cs 'abc'", varchar ("abc", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CS));
  add ("VARCHAR iso_en_ci 'ABC'", varchar ("ABC", INTL_CODESET_ISO88591, LANG_COLL_ISO_EN_CI));
  add ("VARCHAR euckr_bin 'abc'", varchar ("abc", INTL_CODESET_KSC5601_EUC, LANG_COLL_EUCKR_BINARY));
  add ("VARCHAR utf8_bin '1a'", varchar ("1a", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin ''", varchar ("", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin ' 12 '", varchar (" 12 ", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '2020-01-02'", varchar ("2020-01-02", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '10:11:12'", varchar ("10:11:12", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '2020-01-02 10:11:12'", varchar ("2020-01-02 10:11:12", INTL_CODESET_UTF8,
							   LANG_COLL_UTF8_BINARY));
  add ("CHAR(5) iso_bin 'ab'", fixed_char ("ab", 5, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY));
  add ("CHAR(5) utf8_bin 'ab'", fixed_char ("ab", 5, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("CHAR(2) utf8_bin '12'", fixed_char ("12", 2, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  db_make_date (&v, 1, 2, 2020);
  add ("DATE 2020-01-02", v);
  db_make_time (&v, 10, 11, 12);
  add ("TIME 10:11:12", v);
  {
    DB_DATETIME dt;
    db_datetime_encode (&dt, 1, 2, 2020, 10, 11, 12, 0);
    db_make_datetime (&v, &dt);
    add ("DATETIME 2020-01-02 10:11:12", v);
  }
  db_make_enumeration (&v, 2, "abc", 3, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  add ("ENUM(2,'abc') utf8_bin", v);
  db_make_enumeration (&v, 1, "ABC", 3, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  add ("ENUM(1,'ABC') iso_bin", v);
  db_make_bit (&v, 16, (DB_CONST_C_BIT) bits16, 16);
  add ("BIT(16) x1234", v);
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) bits8, 8);
  add ("VARBIT x12", v);
  /* garbage: no column gives a value these (a bind's string takes the session collation's codeset,
   * lang_get_client_charset/_collation language_support.c:2767; a DBLINK value is cast into the local
   * column's domain, dblink_scan.c:1391), but nothing in a DB_VALUE prevents them */
  add ("VARCHAR cs=utf8 coll=iso_bin '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_ISO_BINARY), true);
  add ("VARCHAR cs=iso coll=utf8_bin 'ab'", varchar ("ab", INTL_CODESET_ISO88591, LANG_COLL_UTF8_BINARY), true);
  add ("VARCHAR coll=200 (unregistered) 'ab'", varchar ("ab", INTL_CODESET_UTF8, 200), true);
  add ("VARCHAR prec=-1 'ab'", varchar ("ab", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY,
					TP_FLOATING_PRECISION_VALUE), true);
  add ("VARCHAR prec=0 ''", varchar ("", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY, 0), true);
  add ("CHAR prec=-1 'ab'", fixed_char ("ab", TP_FLOATING_PRECISION_VALUE, INTL_CODESET_UTF8,
					LANG_COLL_UTF8_BINARY), true);
  add ("CHAR(1) 'ab' (longer than its precision)", fixed_char ("ab", 1, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY),
       true);
  db_make_varbit (&v, TP_FLOATING_PRECISION_VALUE, (DB_CONST_C_BIT) bits8, 8);
  add ("VARBIT prec=-1", v, true);
  db_make_enumeration (&v, 3, "ab", 2, INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI);
  add ("ENUM(3,'ab') utf8_en_ci", v);
}

/* codeset != its collation's codeset: the keys the type pair table has no exact row for */
static bool
codeset_off_collation (const DB_VALUE * v)
{
  DOMAIN_COMPARE_KEY key;
  domain_compare_key_of_value (v, &key);
  return TP_TYPE_HAS_COLLATION (key.type) && key.collation >= 0 && key.collation < LANG_MAX_COLLATIONS
    && key.codeset != lang_get_collation (key.collation)->codeset;
}

struct outcome
{
  int result;
  bool can_compare;
  int error;
  bool operator== (const outcome & o) const
  {
    return result == o.result && can_compare == o.can_compare && error == o.error;
  }
};

static std::string
show (const outcome & o)
{
  static const char *names[] = { "EQ", "LT", "GT", "UNK" };
  const int r = o.result;
  return std::string (r == DB_EQ ? names[0] : r == DB_LT ? names[1] : r == DB_GT ? names[2] : names[3]) + "/"
    + (o.can_compare ? "comparable" : "not comparable") + "/er " + std::to_string (o.error);
}

static outcome
reference (const DB_VALUE * a, const DB_VALUE * b)
{
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = tp_value_compare_with_error (a, b, 1, 0, &o.can_compare);
  o.error = er_errid ();
  return o;
}

/* what a resolved comparison gives at the row: DIRECT, CONVERT, COLLATIONS, RANK compare in domain_compare_values; a
 * NULL or OBJECT side (VALUES, OBJECT) is tp_value_compare_with_error's by design */
static outcome
resolved (const DB_VALUE * a, const DB_VALUE * b, int *method)
{
  DOMAIN_COMPARE_KEY ka, kb;
  DOMAIN_COMPARE c;
  domain_compare_key_of_value (a, &ka);
  domain_compare_key_of_value (b, &kb);
  REQUIRE (domain_resolve_comparison (&ka, &kb, &c) == NO_ERROR);
  *method = c.method;
  if (c.method != DOMAIN_COMPARE_DIRECT && c.method != DOMAIN_COMPARE_CONVERT && c.method != DOMAIN_COMPARE_COLLATIONS
      && c.method != DOMAIN_COMPARE_RANK)
    {
      return reference (a, b);
    }
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = domain_compare_values (&c, a, b, 0, &o.can_compare);
  o.error = er_errid ();
  return o;
}

static outcome
by_type_pair (const DB_VALUE * a, const DB_VALUE * b)
{
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = domain_compare_by_type_pair (a, b, 1, 0, &o.can_compare);
  o.error = er_errid ();
  return o;
}

static const TP_DOMAIN *
vc_domain (int precision, int collation)
{
  return tp_domain_resolve (DB_TYPE_VARCHAR, NULL, precision, 0, NULL, collation);
}

static const TP_DOMAIN *
char_domain (int precision, int collation)
{
  return tp_domain_resolve (DB_TYPE_CHAR, NULL, precision, 0, NULL, collation);
}

static const TP_DOMAIN *
type_domain (DB_TYPE type)
{
  return tp_domain_resolve_default (type);
}

static DOMAIN_OPERAND
operand (const TP_DOMAIN * d)
{
  return DOMAIN_OPERAND { d, TP_DOMAIN_TYPE (d), -1, false };
}

/* "TYPE(precision,scale) coll=c" or the error code */
static std::string
domain_text (int error, const TP_DOMAIN * d)
{
  if (error != NO_ERROR)
    {
      return "error " + std::to_string (error);
    }
  if (d == NULL)
    {
      return "no domain";
    }
  char buf[128];
  snprintf (buf, sizeof (buf), "%s(%d,%d) coll=%d", pr_type_name (TP_DOMAIN_TYPE (d)), d->precision, d->scale,
	    TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (d)) ? d->collation_id : -1);
  return buf;
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("K1 a comparison resolved from the keys answers what tp_value_compare_with_error answers, every pair",
	   "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  std::vector<std::string> residues;
  for (const named_value & a : g_values)
    {
      for (const named_value & b : g_values)
	{
	  int method = -1;
	  const outcome want = reference (&a.v, &b.v), got = resolved (&a.v, &b.v, &method);
	  INFO (a.name << " vs " << b.name << ": tp_value_compare_with_error " << show (want) << ", resolved (method "
		<< method << ") " << show (got));
	  if (want.can_compare && want.error != NO_ERROR)
	    {
	      /* the cast tp_value_compare_with_error coerces with publishes an error its converter wrote on a
	       * conversion it calls compatible (tp_value_convert_integer_to_time: db_time_decode of a negative
	       * remainder, object_domain_convert.cpp:2079); the resolved converter runs without publishing.
	       * Same result and comparability; the error is a residue nobody reads (can_compare is true) */
	      CHECK (got.result == want.result);
	      CHECK (got.can_compare == want.can_compare);
	      residues.push_back (a.name + " vs " + b.name + " er " + std::to_string (want.error));
	      continue;
	    }
	  CHECK (got == want);
	}
    }
  const std::vector<std::string> expected_residues = {
    "INT -3 vs TIME 10:11:12 er -787", "TIME 10:11:12 vs INT -3 er -787"
  };
  CHECK (residues == expected_residues);
}

TEST_CASE ("K2 the key pair table's cell is the comparison computed for its two keys", "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  int pairs = 0;
  for (const named_value & a : g_values)
    {
      for (const named_value & b : g_values)
	{
	  DOMAIN_COMPARE_KEY ka, kb;
	  DOMAIN_COMPARE table, computed;
	  domain_compare_key_of_value (&a.v, &ka);
	  domain_compare_key_of_value (&b.v, &kb);
	  REQUIRE (domain_resolve_comparison (&ka, &kb, &table) == NO_ERROR);
	  REQUIRE (unitseam_compute_comparison (&ka, &kb, &computed) == NO_ERROR);
	  INFO (a.name << " vs " << b.name << " method " << (int) table.method << " / " << (int) computed.method);
	  /* every field, the operator functions included (the table's cells have none: the load and resolve_domains set
	   * them on the copies they keep, domain_compare_set_operator_functions) */
	  CHECK (unitseam_compare_equal (&table, &computed));
	  pairs++;
	}
    }
  CHECK (pairs == (int) (g_values.size () * g_values.size ()));
}

TEST_CASE ("K3 domain_compare_by_type_pair answers as tp_value_compare_with_error for values whose codeset is their "
	   "collation's", "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  for (const named_value & a : g_values)
    {
      for (const named_value & b : g_values)
	{
	  if (codeset_off_collation (&a.v) || codeset_off_collation (&b.v))
	    {
	      continue;		/* F2 */
	    }
	  const outcome want = reference (&a.v, &b.v), got = by_type_pair (&a.v, &b.v);
	  INFO (a.name << " vs " << b.name << ": tp_value_compare_with_error " << show (want) << ", by type pair "
		<< show (got));
	  if (want.can_compare && want.error != NO_ERROR)
	    {
	      CHECK (got.result == want.result);	/* the -787 residue of K1 */
	      continue;
	    }
	  CHECK (got == want);
	}
    }
}

TEST_CASE ("K4 argument order: a resolved comparison of (b, a) reverses (a, b), same comparability and error "
	   "(values whose codeset is their collation's)",
	   "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  for (size_t i = 0; i < g_values.size (); i++)
    {
      for (size_t j = i + 1; j < g_values.size (); j++)
	{
	  const named_value & a = g_values[i], &b = g_values[j];
	  if (codeset_off_collation (&a.v) || codeset_off_collation (&b.v))
	    {
	      /* a string whose codeset is not its collation's meets LANG_RT_COMMON_COLL's order (one coercible binary
	       * collation yields to ISO binary only as the second operand) and the codeset test in either order:
	       * tp_value_compare_with_error itself answers (a, b) and (b, a) differently there, and K1 holds the
	       * resolved comparison to it */
	      continue;
	    }
	  int m1, m2;
	  const outcome ab = resolved (&a.v, &b.v, &m1), ba = resolved (&b.v, &a.v, &m2);
	  const int reversed = ba.result == DB_LT ? DB_GT : ba.result == DB_GT ? DB_LT : ba.result;
	  INFO (a.name << " vs " << b.name << ": " << show (ab) << " / reversed order " << show (ba));
	  CHECK (ab.result == reversed);
	  CHECK (ab.can_compare == ba.can_compare);
	  CHECK (ab.error == ba.error);
	}
    }
}

TEST_CASE ("K5 domain_value_domain gives the cached domain tp_domain_resolve_value gives, garbage included",
	   "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  for (const named_value & a : g_values)
    {
      INFO (a.name);
      CHECK (domain_value_domain (&a.v) == tp_domain_resolve_value (&a.v, NULL));
    }
}

TEST_CASE ("K6 a character result's collation: merge in operand order, the source's, or the branches' one domain",
	   "[contract]")
{
  REQUIRE (boot_types ());
  /* a node the compiler typed VARCHAR and left its collation to the values (LEAVE) */
  const TP_DOMAIN *leave = tp_domain_resolve_default_w_coll (DB_TYPE_VARCHAR, LANG_COLL_UTF8_BINARY,
							      TP_DOMAIN_COLL_LEAVE);
  REQUIRE (TP_DOMAIN_COLLATION_FLAG (leave) == TP_DOMAIN_COLL_LEAVE);
  const TP_DOMAIN *integer = type_domain (DB_TYPE_INTEGER), *date = type_domain (DB_TYPE_DATE);
  struct
  {
    const char *what;
    int opcode;
    std::vector<const TP_DOMAIN *> operands;
    const char *expected;
  } cases[] = {
    /* db_string_concatenate: LANG_RT_COMMON_COLL left to right, precisions added */
    {"CONCAT utf8_bin(10), utf8_bin(20)", T_CONCAT, {vc_domain (10, 1), vc_domain (20, 1)}, "character varying(30,0) coll=1"},
    /* both binary (coercible): the second wins only when it is ISO binary - so the order decides */
    {"CONCAT utf8_bin, iso_bin", T_CONCAT, {vc_domain (10, 1), vc_domain (10, 0)}, "character varying(20,0) coll=0"},
    {"CONCAT iso_bin, utf8_bin", T_CONCAT, {vc_domain (10, 0), vc_domain (10, 1)}, "error -1150"},
    /* a binary collation yields to a non-binary one, either order */
    {"CONCAT utf8_en_ci, utf8_bin", T_CONCAT, {vc_domain (10, 5), vc_domain (10, 1)}, "character varying(20,0) coll=5"},
    {"CONCAT utf8_bin, utf8_en_ci", T_CONCAT, {vc_domain (10, 1), vc_domain (10, 5)}, "character varying(20,0) coll=5"},
    {"CONCAT utf8_en_ci, utf8_en_cs", T_CONCAT, {vc_domain (10, 5), vc_domain (10, 4)}, "error -1150"},
    /* an operand without a collation takes no part; none at all is NULL */
    {"CONCAT utf8_en_ci(10), INT", T_CONCAT, {vc_domain (10, 5), integer}, "character varying(10,0) coll=5"},
    {"CONCAT INT, INT", T_CONCAT, {integer, integer}, "*NULL*(0,0) coll=-1"},
    {"REPLACE utf8_en_ci, utf8_en_cs, utf8_en_ci", T_REPLACE, {vc_domain (10, 5), vc_domain (10, 4), vc_domain (10, 5)},
     "error -1150"},
    /* the cut string keeps its collation and precision */
    {"SUBSTRING utf8_en_ci(10), INT", T_SUBSTRING, {vc_domain (10, 5), integer}, "character varying(10,0) coll=5"},
    {"UPPER CHAR(5) utf8_en_ci", T_UPPER, {char_domain (5, 5)}, "character(5,0) coll=5"},
    /* the source's collation once the delimiter merges with it */
    {"SUBSTRING_INDEX utf8_en_ci, utf8_bin, INT", T_SUBSTRING_INDEX, {vc_domain (10, 5), vc_domain (10, 1), integer},
     "character varying(1073741823,0) coll=5"},
    {"SUBSTRING_INDEX utf8_en_ci, utf8_en_cs, INT", T_SUBSTRING_INDEX, {vc_domain (10, 5), vc_domain (10, 4), integer},
     "error -1150"},
    /* a row-picked branch: one domain, or the resolver's (-1383 here; the late-binding node step merges them) */
    {"CASE utf8_bin(10), utf8_bin(10)", T_CASE, {vc_domain (10, 1), vc_domain (10, 1)}, "character varying(10,0) coll=1"},
    {"CASE utf8_bin(10), utf8_en_ci(10)", T_CASE, {vc_domain (10, 1), vc_domain (10, 5)}, "error -1383"},
    /* a common value is cast to the compiled type, collations merged */
    {"NVL CHAR(5) utf8_bin, VARCHAR utf8_en_ci", T_NVL, {char_domain (5, 1), vc_domain (10, 5)},
     "character varying(1073741823,0) coll=5"},
    {"DATE_FORMAT DATE, utf8_en_ci", T_DATE_FORMAT, {date, vc_domain (10, 5)}, "character varying(1073741823,0) coll=5"},
    {"SPACE INT (LANG_SYS)", T_SPACE, {integer}, "character varying(1073741823,0) coll=1"},
  };
  for (auto & c : cases)
    {
      std::vector<DOMAIN_OPERAND> ops;
      for (const TP_DOMAIN * d : c.operands)
	{
	  ops.push_back (operand (d));
	}
      RESOLVED_DOMAIN r;
      const int e = domain_resolve_character (c.opcode, ops.data (), (int) ops.size (), leave, &r);
      INFO (c.what);
      CHECK (domain_text (e, r.domain) == c.expected);
    }
}

TEST_CASE ("K7 branch merge and pick: merged collation, CHAR stays CHAR, -1150 when the branches do not merge",
	   "[contract]")
{
  REQUIRE (boot_types ());
  const TP_DOMAIN *integer = type_domain (DB_TYPE_INTEGER);
  struct
  {
    const char *what;
    std::vector<const TP_DOMAIN *> branches;
    const char *expected;
  } merges[] = {
    {"VARCHAR utf8_bin, VARCHAR utf8_en_ci", {vc_domain (10, 1), vc_domain (10, 5)}, "character varying(1073741823,0) coll=5"},
    {"CHAR(5) utf8_bin, VARCHAR utf8_en_ci", {char_domain (5, 1), vc_domain (10, 5)}, "character(-1,0) coll=5"},
    {"VARCHAR utf8_en_ci, VARCHAR utf8_en_cs", {vc_domain (10, 5), vc_domain (10, 4)}, "error -1150"},
    {"VARCHAR iso_bin, VARCHAR utf8_bin", {vc_domain (10, 0), vc_domain (10, 1)}, "error -1150"},
    {"VARCHAR utf8_bin, VARCHAR iso_bin", {vc_domain (10, 1), vc_domain (10, 0)}, "character varying(1073741823,0) coll=0"},
    {"INT, INT", {integer, integer}, "*NULL*(0,0) coll=-1"},
  };
  for (auto & c : merges)
    {
      std::vector<DOMAIN_OPERAND> ops;
      for (const TP_DOMAIN * d : c.branches)
	{
	  ops.push_back (operand (d));
	}
      RESOLVED_DOMAIN r;
      const int e = domain_resolve_branch_merge (ops.data (), (int) ops.size (), &r);
      INFO (c.what);
      CHECK (domain_text (e, r.domain) == c.expected);
      if (e == NO_ERROR && TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (r.domain)))
	{
	  /* the converters a picked value takes into the merged domain: [0] from VARCHAR, [1] from CHAR */
	  CHECK (r.conv[0] == tp_value_find_converter (DB_TYPE_VARCHAR, r.domain, DOMAIN_CONVERT_ASSIGN));
	  CHECK (r.conv[1] == tp_value_find_converter (DB_TYPE_CHAR, r.domain, DOMAIN_CONVERT_ASSIGN));
	}
    }
  /* ELT (index, VARCHAR(10) utf8_bin, CHAR(5) utf8_en_ci): the index names the branch; none, or one without a string,
   * is NULL at every row (qdata_elt) */
  std::vector<DOMAIN_OPERAND> elt = { operand (integer), operand (vc_domain (10, 1)), operand (char_domain (5, 5)) };
  const char *picks[] = { "*NULL*(0,0) coll=-1", "*NULL*(0,0) coll=-1", "character varying(10,0) coll=1",
    "character(5,0) coll=5", "*NULL*(0,0) coll=-1"
  };
  for (int branch = -1; branch <= 3; branch++)
    {
      RESOLVED_DOMAIN r;
      const int e = domain_resolve_branch_pick (elt.data (), 3, branch, &r);
      INFO ("pick " << branch);
      CHECK (domain_text (e, r.domain) == picks[branch + 1]);
    }
}

TEST_CASE ("K8 common value: tp_infer_common_domain folded left to right gives the same domain either order",
	   "[contract]")
{
  REQUIRE (boot_types ());
  const TP_DOMAIN *integer = type_domain (DB_TYPE_INTEGER), *bigint = type_domain (DB_TYPE_BIGINT);
  const TP_DOMAIN *n52 = tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 5, 2, NULL, 0);
  const TP_DOMAIN *n104 = tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 10, 4, NULL, 0);
  struct
  {
    const char *what;
    const TP_DOMAIN *a, *b;
    const char *ab, *ba;
  } cases[] = {
    {"CHAR(5) utf8_bin, VARCHAR(10) utf8_bin", char_domain (5, 1), vc_domain (10, 1), "character varying(10,0) coll=1",
     "character varying(10,0) coll=1"},
    {"INT, BIGINT", integer, bigint, "bigint(19,0) coll=-1", "bigint(19,0) coll=-1"},
    /* a NUMERIC common value is the default NUMERIC: the value operation fixes precision and scale */
    {"INT, NUMERIC(5,2)", integer, n52, "numeric(40,0) coll=-1", "numeric(40,0) coll=-1"},
    {"NUMERIC(5,2), NUMERIC(10,4)", n52, n104, "numeric(40,0) coll=-1", "numeric(40,0) coll=-1"},
    {"INT, VARCHAR(10)", integer, vc_domain (10, 1), "character varying(1073741823,0) coll=1",
     "character varying(1073741823,0) coll=1"},
    {"INT, DOUBLE", integer, type_domain (DB_TYPE_DOUBLE), "double(15,0) coll=-1", "double(15,0) coll=-1"},
    {"DATE, DATETIME", type_domain (DB_TYPE_DATE), type_domain (DB_TYPE_DATETIME), "datetime(23,3) coll=-1",
     "datetime(23,3) coll=-1"},
    {"NULL, INT", &tp_Null_domain, integer, "integer(10,0) coll=-1", "integer(10,0) coll=-1"},
    /* two collations of one type: the first operand's (tp_infer_common_domain), unlike K6's NVL merge, which is a
     * LEAVE node's rule */
    {"VARCHAR(10) utf8_bin, VARCHAR(10) utf8_en_ci", vc_domain (10, 1), vc_domain (10, 5),
     "character varying(10,0) coll=1", "character varying(10,0) coll=5"},
  };
  for (auto & c : cases)
    {
      DOMAIN_OPERAND ab[2] = { operand (c.a), operand (c.b) }, ba[2] = { operand (c.b), operand (c.a) };
      RESOLVED_DOMAIN r1, r2;
      bool late1 = true, late2 = true;
      const int e1 = domain_resolve (DOMAIN_CTX_COMMON_VALUE, T_NVL, ab, 2, NULL, &r1, &late1);
      const int e2 = domain_resolve (DOMAIN_CTX_COMMON_VALUE, T_NVL, ba, 2, NULL, &r2, &late2);
      INFO (c.what);
      CHECK (!late1);
      CHECK (!late2);
      CHECK (domain_text (e1, r1.domain) == c.ab);
      CHECK (domain_text (e2, r2.domain) == c.ba);
    }
}

TEST_CASE ("K9 set-operation column: a NULL branch takes the other's domain, a variable string keeps the first, "
	   "any other difference is -456 (qfile_unify_types' rule)", "[contract]")
{
  REQUIRE (boot_types ());
  const TP_DOMAIN *integer = type_domain (DB_TYPE_INTEGER);
  struct
  {
    const char *what;
    const TP_DOMAIN *a, *b;
    const char *expected;
  } cases[] = {
    {"NULL, INT", &tp_Null_domain, integer, "integer(10,0) coll=-1"},
    {"INT, INT", integer, integer, "integer(10,0) coll=-1"},
    {"VARCHAR(10), VARCHAR(20)", vc_domain (10, 1), vc_domain (20, 1), "character varying(10,0) coll=1"},
    {"VARCHAR(10) utf8_bin, VARCHAR(10) utf8_en_ci", vc_domain (10, 1), vc_domain (10, 5),
     "character varying(10,0) coll=1"},
    /* pr_is_variable_type (DB_TYPE_CHAR) holds: CHAR is a variable string type there, as in qfile_unify_types */
    {"CHAR(5), CHAR(10)", char_domain (5, 1), char_domain (10, 1), "character(5,0) coll=1"},
    {"NUMERIC(5,2), NUMERIC(10,4)", tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 5, 2, NULL, 0),
     tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 10, 4, NULL, 0), "error -456"},
    {"INT, BIGINT", integer, type_domain (DB_TYPE_BIGINT), "error -456"},
  };
  for (auto & c : cases)
    {
      DOMAIN_OPERAND ops[2] = { operand (c.a), operand (c.b) };
      RESOLVED_DOMAIN r;
      bool late = true;
      const int e = domain_resolve (DOMAIN_CTX_LIST_COLUMN, 0, ops, 2, NULL, &r, &late);
      INFO (c.what);
      CHECK (domain_text (e, r.domain) == c.expected);
    }
}

TEST_CASE ("PR8022 F2: (latent) for a string whose codeset is not its collation's, domain_compare_by_type_pair "
	   "answers otherwise than tp_value_compare_with_error (PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  build_values ();
  /* domain_resolve_comparison takes a table cell only for a key the table has exactly
   * (domain_key_pair_index_exact, domain_rules.c:2274); domain_compare_by_type_pair (domain_rules.c:2346) and
   * domain_compare_row_entry (domain_rules.c:2314) look the key up with domain_key_pair_index, which ignores the
   * codeset: the cell they copy was computed for the collation's own codeset */
  int differ = 0, compared = 0;
  for (const named_value & a : g_values)
    {
      for (const named_value & b : g_values)
	{
	  if (!codeset_off_collation (&a.v) && !codeset_off_collation (&b.v))
	    {
	      continue;
	    }
	  const outcome want = reference (&a.v, &b.v), got = by_type_pair (&a.v, &b.v);
	  compared++;
	  if (!(got == want) && !(want.can_compare && want.error != NO_ERROR))
	    {
	      differ++;
	    }
	}
    }
  INFO ("pairs with a codeset-off-collation side: " << compared << ", answered otherwise: " << differ);
  CHECK (differ > 0);
  /* the two directions of it */
  const named_value *utf8_bin_12 = NULL, *off_12 = NULL, *en_ci_12 = NULL, *off_ab = NULL;
  for (const named_value & v : g_values)
    {
      utf8_bin_12 = v.name == "VARCHAR utf8_bin '12'" ? &v : utf8_bin_12;
      off_12 = v.name == "VARCHAR cs=utf8 coll=iso_bin '12'" ? &v : off_12;
      en_ci_12 = v.name == "VARCHAR utf8_en_ci '12'" ? &v : en_ci_12;
      off_ab = v.name == "VARCHAR cs=iso coll=utf8_bin 'ab'" ? &v : off_ab;
    }
  REQUIRE (utf8_bin_12 != NULL);
  REQUIRE (off_12 != NULL);
  REQUIRE (en_ci_12 != NULL);
  REQUIRE (off_ab != NULL);
  /* equal strings the reference compares: the pair says the collations do not merge */
  CHECK (show (reference (&utf8_bin_12->v, &off_12->v)) == "EQ/comparable/er 0");
  CHECK (show (by_type_pair (&utf8_bin_12->v, &off_12->v)) == "UNK/not comparable/er -1150");
  /* collations the reference refuses to merge: the pair compares them, unknown and without an error */
  CHECK (show (reference (&en_ci_12->v, &off_ab->v)) == "UNK/not comparable/er -1150");
  CHECK (show (by_type_pair (&en_ci_12->v, &off_ab->v)) == "UNK/comparable/er 0");
}
