/*
 * PR#8237 (CBRD-27701) -- the element hash of the sampled index prefix distinct estimate (src/storage/btree.c, server
 * mode): btree_get_stats_prefix_value_hash (const DB_VALUE *), new in the PR. btree_get_stats_midxkey () mixes it into
 * the prefix hash reservoir ("hash = hash * 1099511628211ULL + val_hash", level 0 = the element hash itself), and
 * btree_get_stats_duj1_estimate () counts distinct reservoir hashes. The PR's contract (its body and the function's
 * comment): values equal under tp_value_compare () hash alike, distinct values hash apart.
 *
 * hooks.cpp gives unitseam_prefix_value_hash (): this function where the revision has it, and before it the element
 * hash the same call site used - mht_valhash (&elem, 0xFFFFFFFEU) on its one local DB_VALUE (btree.c:6850
 * @89722cf4e) - so the same cases run on the base and show what the PR changed. Equality is decided as the reservoir's
 * caller decides it: tp_value_compare (a, b, 0, 1) (btree_get_stats_midxkey).
 *
 *   C1  fixed NUMERIC(10,2): equal values hash alike, distinct values apart
 *   C2  float NUMERIC (string path): 1.0 = 1.00, 15 = 15.0 hash alike; 1.5 and 15 apart; zero and negative forms
 *   C3  FLOAT / DOUBLE / MONETARY: -0.0 = 0.0 alike, fractions of one integer part apart
 *   C4  BIGINT: two values whose high and low 32-bit words xor alike hash apart
 *   C5  ENUM: index 1 and 2 apart, one index (two DB_VALUEs) alike
 *   C6  BIT / VARBIT: values whose first byte is 0 hash apart
 *   C7  DATETIMETZ / TIMESTAMPTZ: the same UTC instant in another time zone hashes alike (server mode ignores tz_id)
 *   C8  VARCHAR / CHAR: values equal under the collation hash alike (utf8_en_ci 'ABC' = 'abc'), distinct ones apart
 *       - no two values that differ only in trailing spaces (that is R1)
 *   C9  SHORT / INT / DATE / TIME / TIMESTAMP / DATETIME / OID: equal iff same hash
 *   R1  [defect] VARCHAR 'a' / 'a ' (and '' / ' ' / '  ') compare unequal with ignore_trailing_space=no (default) but
 *       hash alike - mht_get_hash_number () drops trailing ASCII spaces before the collation hash
 *   R1d [defect] its effect: Duj1 of 2,048 strings each with a trailing-space twin (4,096 distinct) is 2,048
 *   R2  [defect] float NUMERIC 100 as (coefficient 1, scale -2, precision 1) and (100, 0, 3) - also 10^39 as
 *       (p39, s-1)/(p40, s0) and 0 with scale -2 - compare equal but hash apart: a negative scale is multiplied away
 *       by the normalization but the original scale is mixed in
 *   R2d [defect] its effect: Duj1 of 2,048 values each in both forms (2,048 distinct) is 4,096
 *   R2r [defect] reach: float NUMERIC subtraction (the SQL '-') of two 42-digit literals gives that precision-1 scale -2
 *       form, and a cast into the float NUMERIC column domain (NUMERIC without (p,s)) keeps it
 *   O1  [defect] (found here) euckr_bin: leading spaces and a trailing EUC-KR space pair do not change the hash
 *       (lang_mht2str_ko) though the comparison sees them
 *   O2  [defect] (found here) the 32-bit PJW-style collation hash (lang_mht2str_byte / mht_2str_pseudo_key) gives
 *       distinct 'YYYY-MM-DD' strings one hash ('2015-01-20' = '2025-01-10'): 4,096 days from 2015-01-01 -> fewer
 *       distinct hashes, and the Duj1 estimate drops with them; develop's byte hash did not collide on them
 *
 * Mutation (5-1): fix_r1.diff (the R1 proposal as filed: the trailing-space count xor-ed into the string hash),
 * fix_r1b.diff (count and string hash both through FNV-1a from the offset basis), fix_r2.diff (left_scale for a
 * negative scale) - run with unitseam --patch: /home/cubrid/dev/utils/harness-out/8237/3a53a044b/unit/ (summary.md).
 *
 * Real code at THIS revision linked into the test binary (suite.json extra): memory_hash.c (mht_get_hash_number,
 * mht_valhash), system_parameter.c (prm_Def defaults: ignore_trailing_space=false), language_support.c,
 * intl_support.c, chartype.c, mprec.c, dtoa.c, object_primitive.c, object_domain.c, object_domain_convert.cpp,
 * numeric_opfunc.c, db_date.c, db_macro.c, string_opfunc.c, oid.c. Fakes: only what a booted server process has
 * (below).
 */
#include "catch.hpp"
#include "dbtype.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "numeric_opfunc.h"
#include "language_support.h"
#include "intl_support.h"
#include "system_parameter.h"
#include "boot_sr.h"
#include "error_manager.h"
#include "memory_alloc.h"
#include "area_alloc.h"
#include "environment_variable.h"
#include "string_regex.hpp"
#include "string_opfunc.h"
#include "db_date.h"
#include "oid.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
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

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this; DOWN keeps prm_get_value (system_parameter.c) on the global
 * values - the prm_Def defaults - instead of a session's copy, which needs a thread entry */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

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

/* memory_alloc.c: a thread's private heap is malloc here */
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
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset */
static void
boot_types ()
{
  static bool done = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      REQUIRE (tp_init () == NO_ERROR);
      /* lang_init () would also load user locales; lang_set_charset_lang () only asserts lang_Initialized (static) */
      REQUIRE (lang_set_charset_lang ("en_US.utf8") == NO_ERROR);
    }
  er_clear ();
}

/* ---------------------------------------------------------------- hooks.cpp (appended to btree.c) */
UINT64 unitseam_prefix_value_hash (const DB_VALUE * value);
bool unitseam_prefix_hash_is_legacy (void);
INT64 unitseam_duj1_estimate (const UINT64 * hashes, int n, INT64 total_keys);
int unitseam_duj1_sample_capacity (void);

static const char *
impl ()
{
  return unitseam_prefix_hash_is_legacy ()? "legacy mht_valhash (&elem, 0xFFFFFFFEU) (this revision has no "
    "btree_get_stats_prefix_value_hash)" : "btree_get_stats_prefix_value_hash ()";
}

static UINT64
H (const DB_VALUE & v)
{
  return unitseam_prefix_value_hash (&v);
}

/* the reservoir's caller decides equality so (btree_get_stats_midxkey: tp_value_compare (.., .., 0, 1)) */
static bool
EQ (const DB_VALUE & a, const DB_VALUE & b)
{
  return tp_value_compare (&a, &b, 0, 1) == DB_EQ;
}

/* ---------------------------------------------------------------- values */

struct nv
{
  std::string name;
  DB_VALUE v;
};

/* every pair: compare EQ <=> same hash. Returns the pairs that break it, one per line. */
static std::string
inconsistencies (const std::vector < nv > &vals)
{
  std::ostringstream out;
  for (size_t i = 0; i < vals.size (); i++)
    {
      for (size_t j = i + 1; j < vals.size (); j++)
	{
	  bool eq = EQ (vals[i].v, vals[j].v);
	  bool same = H (vals[i].v) == H (vals[j].v);
	  if (eq != same)
	    {
	      out << vals[i].name << " vs " << vals[j].name << ": compare " << (eq ? "EQ" : "NE") << ", hash "
		<< (same ? "same" : "different") << "\n";
	    }
	}
    }
  return out.str ();
}

static DB_VALUE
varchar (const char *s, int coll)
{
  DB_VALUE v;
  db_make_varchar (&v, DB_MAX_VARCHAR_PRECISION, s, (int) strlen (s), lang_get_collation (coll)->codeset, coll);
  return v;
}

static DB_VALUE
fixed_char (const char *s, int precision, int coll)
{
  DB_VALUE v;
  db_make_char (&v, precision, s, (int) strlen (s), lang_get_collation (coll)->codeset, coll);
  return v;
}

/* fixed NUMERIC(p,s) as a column holds it: the string cast into the column's domain */
static DB_VALUE
fixed_numeric (const char *text, int p, int s)
{
  DB_VALUE src, dst;
  db_make_string (&src, text);
  db_make_null (&dst);
  REQUIRE (tp_value_cast (&src, &dst, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0), false)
	   == DOMAIN_COMPATIBLE);
  return dst;
}

/* float NUMERIC the way a literal or CAST (.. AS NUMERIC) makes it (numeric_coerce_string_to_num, numeric_opfunc.c) */
static DB_VALUE
float_numeric (const char *text)
{
  DB_VALUE v;
  REQUIRE (numeric_coerce_string_to_num (text, (int) strlen (text), INTL_CODESET_UTF8, &v) == NO_ERROR);
  bool is_float = false;
  int p = 0, s = 0;
  db_get_numeric_precision_and_scale (&v, &p, &s, &is_float);
  REQUIRE (is_float);
  return v;
}

/* float NUMERIC with a given coefficient, precision and scale (db_make_numeric (.., true), the form every float
 * NUMERIC producer ends in) */
static DB_VALUE
float_numeric_raw (const char *coefficient, int precision, int scale, bool negative = false)
{
  unsigned char buf[DB_NUMERIC_BUF_SIZE];
  DB_VALUE v;
  numeric_coerce_dec_str_to_num (coefficient, buf, NULL);
  REQUIRE (db_make_numeric (&v, buf, precision, scale, DB_NUMERIC_BUF_SIZE, negative, true) == NO_ERROR);
  return v;
}

static std::string
pscale (const DB_VALUE & v)
{
  bool is_float = false;
  int p = 0, s = 0;
  db_get_numeric_precision_and_scale (&v, &p, &s, &is_float);
  /* numeric_coerce_num_to_dec_str: optional '-', then the coefficient zero-padded to 256 digits */
  char dec[NUMERIC_MAX_STRING_SIZE];
  numeric_coerce_num_to_dec_str (&v, dec);
  const char *c = dec + (dec[0] == '-' ? 1 : 0);
  while (*c == '0' && c[1] != '\0')
    {
      c++;
    }
  std::ostringstream o;
  o << "(" << (dec[0] == '-' ? "-" : "") << "coefficient " << c << ", precision " << p << ", scale " << s << ", "
    << (is_float ? "float" : "fixed") << ")";
  return o.str ();
}

/* ================================================================ contract */

TEST_CASE ("PR8237 C1: fixed NUMERIC(10,2) - equal values hash alike, distinct values apart", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  std::vector < nv > vals = {
    {"12.00 from '12'", fixed_numeric ("12", 10, 2)},
    {"12.00 from '12.000'", fixed_numeric ("12.000", 10, 2)},
    {"12.01", fixed_numeric ("12.01", 10, 2)},
    {"13.00", fixed_numeric ("13", 10, 2)},
    {"-12.00", fixed_numeric ("-12", 10, 2)},
    {"0.00", fixed_numeric ("0", 10, 2)},
    {"0.01", fixed_numeric ("0.01", 10, 2)},
    {"1200.00", fixed_numeric ("1200", 10, 2)},
    {"99999999.99", fixed_numeric ("99999999.99", 10, 2)},
    {"-99999999.99", fixed_numeric ("-99999999.99", 10, 2)},
  };
  CHECK (EQ (vals[0].v, vals[1].v));
  CHECK (inconsistencies (vals) == std::string ());
  /* the PR's note: mht_valhash () stops at the first zero byte, so every small fixed NUMERIC hashed alike */
  CHECK (H (vals[0].v) != H (vals[2].v));
  CHECK (H (vals[0].v) != H (vals[4].v));
}

TEST_CASE ("PR8237 C2: float NUMERIC - 1.0 = 1.00 and 15 = 15.0 hash alike, 1.5 and 15 apart", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  DB_VALUE one_0 = float_numeric ("1.0"), one_00 = float_numeric ("1.00"), one = float_numeric ("1");
  DB_VALUE f15 = float_numeric ("15"), f15_0 = float_numeric ("15.0"), f1_5 = float_numeric ("1.5");
  WARN ("C2 1.0 " << pscale (one_0) << ", 1.00 " << pscale (one_00) << ", 15 " << pscale (f15) << ", 15.0 "
	<< pscale (f15_0) << ", 1.5 " << pscale (f1_5));

  REQUIRE (EQ (one_0, one_00));
  REQUIRE (EQ (f15, f15_0));
  REQUIRE_FALSE (EQ (f1_5, f15));
  CHECK (H (one_0) == H (one_00));
  CHECK (H (one_0) == H (one));
  CHECK (H (f15) == H (f15_0));
  CHECK (H (f1_5) != H (f15));

  std::vector < nv > vals = {
    {"1", one}, {"1.0", one_0}, {"1.00", one_00},
    {"15", f15}, {"15.0", f15_0}, {"15.000", float_numeric ("15.000")},
    {"1.5", f1_5}, {"1.50", float_numeric ("1.50")}, {"0.15", float_numeric ("0.15")}, {"150", float_numeric ("150")},
    {"150.0", float_numeric ("150.0")}, {"-1.5", float_numeric ("-1.5")}, {"-1.50", float_numeric ("-1.50")},
    {"-15", float_numeric ("-15")},
    {"0", float_numeric ("0")}, {"0.0", float_numeric ("0.0")}, {"0.000", float_numeric ("0.000")},
    {"0.001", float_numeric ("0.001")}, {"0.0010", float_numeric ("0.0010")},
    /* 40 significant digits: precision 40 with positive scale is normalized too */
    {"1.000000000000000000000000000000000000000", float_numeric ("1.000000000000000000000000000000000000000")},
    {"1234567890123456789012345678901234567890", float_numeric ("1234567890123456789012345678901234567890")},
    {"123456789012345678901234567890123456789.0",
     float_numeric ("123456789012345678901234567890123456789.0")},
    /* 42 digits: precision 40, scale -2 (determine_prec_scale Case 1-B) - left as it is */
    {"100000000000000000000000000000000000000100", float_numeric ("100000000000000000000000000000000000000100")},
    {"100000000000000000000000000000000000000000", float_numeric ("100000000000000000000000000000000000000000")},
    {"1e-30 as 0.000...001", float_numeric ("0.000000000000000000000000000001")},
    {"1e-30 as 0.000...0010", float_numeric ("0.0000000000000000000000000000010")},
  };
  CHECK (inconsistencies (vals) == std::string ());
}

TEST_CASE ("PR8237 C3: FLOAT, DOUBLE, MONETARY - -0.0 = 0.0 hash alike, fractions of one integer part apart",
	   "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  std::vector < nv > vals;
  DB_VALUE v;
  db_make_double (&v, -0.0);
  vals.push_back ({"DOUBLE -0.0", v});
  db_make_double (&v, 0.0);
  vals.push_back ({"DOUBLE 0.0", v});
  db_make_double (&v, 1.5);
  vals.push_back ({"DOUBLE 1.5", v});
  db_make_double (&v, 1.25);
  vals.push_back ({"DOUBLE 1.25", v});
  db_make_double (&v, -1.5);
  vals.push_back ({"DOUBLE -1.5", v});
  db_make_double (&v, 1e300);
  vals.push_back ({"DOUBLE 1e300", v});
  REQUIRE (EQ (vals[0].v, vals[1].v));
  CHECK (H (vals[0].v) == H (vals[1].v));
  CHECK (H (vals[2].v) != H (vals[3].v));
  CHECK (inconsistencies (vals) == std::string ());

  std::vector < nv > fl;
  db_make_float (&v, -0.0f);
  fl.push_back ({"FLOAT -0.0", v});
  db_make_float (&v, 0.0f);
  fl.push_back ({"FLOAT 0.0", v});
  db_make_float (&v, 1.5f);
  fl.push_back ({"FLOAT 1.5", v});
  db_make_float (&v, 1.25f);
  fl.push_back ({"FLOAT 1.25", v});
  CHECK (H (fl[0].v) == H (fl[1].v));
  CHECK (inconsistencies (fl) == std::string ());

  std::vector < nv > mo;
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, -0.0);
  mo.push_back ({"MONETARY -0.0", v});
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, 0.0);
  mo.push_back ({"MONETARY 0.0", v});
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, 1.5);
  mo.push_back ({"MONETARY 1.5", v});
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, 1.25);
  mo.push_back ({"MONETARY 1.25", v});
  CHECK (H (mo[0].v) == H (mo[1].v));
  CHECK (inconsistencies (mo) == std::string ());
}

TEST_CASE ("PR8237 C4: BIGINT - two values whose high and low words xor alike hash apart", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  std::vector < nv > vals;
  DB_VALUE v;
  db_make_bigint (&v, 0);
  vals.push_back ({"0", v});
  db_make_bigint (&v, 0x0000000100000001LL);	/* 1 ^ 1 = 0 */
  vals.push_back ({"2^32+1", v});
  db_make_bigint (&v, 5);
  vals.push_back ({"5", v});
  db_make_bigint (&v, 0x0000000700000002LL);	/* 7 ^ 2 = 5 */
  vals.push_back ({"7*2^32+2", v});
  db_make_bigint (&v, -1);
  vals.push_back ({"-1", v});
  db_make_bigint (&v, 0x7FFFFFFFFFFFFFFFLL);
  vals.push_back ({"INT64_MAX", v});
  CHECK (H (vals[0].v) != H (vals[1].v));
  CHECK (H (vals[2].v) != H (vals[3].v));
  CHECK (inconsistencies (vals) == std::string ());
}

TEST_CASE ("PR8237 C5: ENUM - index 1 and 2 hash apart, one index alike", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  DB_VALUE e1, e1b, e2, e3;
  db_make_enumeration (&e1, 1, "red", 3, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_enumeration (&e1b, 1, "red", 3, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_enumeration (&e2, 2, "green", 5, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  db_make_enumeration (&e3, 3, "blue", 4, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  REQUIRE (EQ (e1, e1b));
  CHECK (H (e1) == H (e1b));
  CHECK (H (e1) != H (e2));
  CHECK (inconsistencies ({{"ENUM 1", e1}, {"ENUM 1 (another DB_VALUE)", e1b}, {"ENUM 2", e2}, {"ENUM 3", e3}})
	 == std::string ());
}

TEST_CASE ("PR8237 C6: BIT, VARBIT - values whose first byte is 0 hash apart", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  static const unsigned char b00ff[] = { 0x00, 0xFF }, b0012[] = { 0x00, 0x12 }, b00ffb[] = { 0x00, 0xFF };
  static const unsigned char b0001[] = { 0x00, 0x01 }, b0002[] = { 0x00, 0x02 }, b00[] = { 0x00 };
  static const unsigned char b80[] = { 0x80 };
  DB_VALUE v;
  std::vector < nv > vb;
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b00ff, 16);
  vb.push_back ({"VARBIT X'00FF'", v});
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b0012, 16);
  vb.push_back ({"VARBIT X'0012'", v});
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b00ffb, 16);
  vb.push_back ({"VARBIT X'00FF' (another buffer)", v});
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b00, 8);
  vb.push_back ({"VARBIT X'00'", v});
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b80, 1);
  vb.push_back ({"VARBIT B'1'", v});
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) b80, 2);
  vb.push_back ({"VARBIT B'10'", v});
  REQUIRE_FALSE (EQ (vb[0].v, vb[1].v));
  CHECK (H (vb[0].v) != H (vb[1].v));
  CHECK (H (vb[0].v) == H (vb[2].v));
  CHECK (inconsistencies (vb) == std::string ());

  std::vector < nv > bb;
  db_make_bit (&v, 16, (DB_CONST_C_BIT) b0001, 16);
  bb.push_back ({"BIT(16) X'0001'", v});
  db_make_bit (&v, 16, (DB_CONST_C_BIT) b0002, 16);
  bb.push_back ({"BIT(16) X'0002'", v});
  db_make_bit (&v, 16, (DB_CONST_C_BIT) b00ff, 16);
  bb.push_back ({"BIT(16) X'00FF'", v});
  CHECK (H (bb[0].v) != H (bb[1].v));
  CHECK (inconsistencies (bb) == std::string ());
}

TEST_CASE ("PR8237 C7: DATETIMETZ, TIMESTAMPTZ - the same UTC instant in another time zone hashes alike",
	   "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  DB_DATETIME dt, dt2;
  REQUIRE (db_datetime_encode (&dt, 10, 27, 2026, 9, 30, 0, 0) == NO_ERROR);
  REQUIRE (db_datetime_encode (&dt2, 10, 27, 2026, 9, 30, 0, 1) == NO_ERROR);
  DB_DATETIMETZ a = { dt, 1 }, b = { dt, 7 }, c = { dt2, 1 };
  DB_VALUE va, vb, vc;
  db_make_datetimetz (&va, &a);
  db_make_datetimetz (&vb, &b);
  db_make_datetimetz (&vc, &c);
  REQUIRE (EQ (va, vb));	/* mr_cmpval_datetimetz: tz_id compared only in SA_MODE */
  CHECK (H (va) == H (vb));
  CHECK (H (va) != H (vc));
  /* date ^ time alike, values apart (mht_valhash xors them) */
  DB_DATETIMETZ x = { {5, 9}, 1 }, y = { {9, 5}, 1 };
  DB_VALUE vx, vy;
  db_make_datetimetz (&vx, &x);
  db_make_datetimetz (&vy, &y);
  CHECK (inconsistencies ({{"DTZ utc", va}, {"DTZ same utc, tz 7", vb}, {"DTZ +1ms", vc}, {"DTZ {5,9}", vx},
			   {"DTZ {9,5}", vy}}) == std::string ());

  DB_TIMESTAMPTZ t1 = { 1800000000U, 1 }, t2 = { 1800000000U, 9 }, t3 = { 1800000001U, 1 };
  DB_VALUE w1, w2, w3;
  db_make_timestamptz (&w1, &t1);
  db_make_timestamptz (&w2, &t2);
  db_make_timestamptz (&w3, &t3);
  REQUIRE (EQ (w1, w2));
  CHECK (H (w1) == H (w2));
  CHECK (inconsistencies ({{"TSTZ", w1}, {"TSTZ other tz", w2}, {"TSTZ +1s", w3}}) == std::string ());
}

TEST_CASE ("PR8237 C8: VARCHAR, CHAR - equal under the collation hash alike, distinct apart (no trailing-space-only "
	   "pairs)", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  INFO ("ignore_trailing_space=" << prm_get_bool_value (PRM_ID_IGNORE_TRAILING_SPACE));
  DB_VALUE ci_upper = varchar ("ABC", LANG_COLL_UTF8_EN_CI), ci_lower = varchar ("abc", LANG_COLL_UTF8_EN_CI);
  REQUIRE (EQ (ci_upper, ci_lower));
  CHECK (H (ci_upper) == H (ci_lower));

  const int colls[] = { LANG_COLL_ISO_BINARY, LANG_COLL_UTF8_BINARY, LANG_COLL_ISO_EN_CS, LANG_COLL_ISO_EN_CI,
    LANG_COLL_UTF8_EN_CS, LANG_COLL_UTF8_EN_CI, LANG_COLL_UTF8_TR_CS, LANG_COLL_UTF8_KO_CS, LANG_COLL_EUCKR_BINARY,
    LANG_COLL_BINARY
  };
  /* ASCII only - the same bytes are valid in every codeset of the list. No two texts differ only in leading or
   * trailing spaces: trailing is R1, leading under euckr_bin is O1 */
  const char *texts[] = { "a", "A", "b", "ab", "aB", "Ab", "ba", "a b", "abc", "ABC", "abd", "i", "I",
    "0", "00", "1", "a\tb", "zz", "zZ"
  };
  for (int coll:colls)
    {
      std::vector < nv > vals;
      for (const char *t:texts)
	{
	  vals.push_back ({std::string ("'") + t + "'", varchar (t, coll)});
	}
      INFO ("collation " << lang_get_collation_name (coll));
      CHECK (inconsistencies (vals) == std::string ());
    }

  /* a leading space is part of the value and of the hash (euckr_bin aside: O1) */
  for (int coll:{LANG_COLL_ISO_BINARY, LANG_COLL_UTF8_BINARY, LANG_COLL_UTF8_EN_CI, LANG_COLL_UTF8_KO_CS})
    {
      INFO ("collation " << lang_get_collation_name (coll));
      DB_VALUE a = varchar ("a", coll), sp_a = varchar (" a", coll);
      REQUIRE_FALSE (EQ (a, sp_a));
      CHECK (H (a) != H (sp_a));
    }

  /* CHAR(n): a column pads every value to n */
  std::vector < nv > ch = {
    {"CHAR(5) 'ab'", fixed_char ("ab", 5, LANG_COLL_UTF8_BINARY)},
    {"CHAR(5) 'ab' (another)", fixed_char ("ab", 5, LANG_COLL_UTF8_BINARY)},
    {"CHAR(5) 'ac'", fixed_char ("ac", 5, LANG_COLL_UTF8_BINARY)},
    {"CHAR(5) 'AB'", fixed_char ("AB", 5, LANG_COLL_UTF8_BINARY)},
  };
  CHECK (inconsistencies (ch) == std::string ());
}

TEST_CASE ("PR8237 C9: SHORT, INT, DATE, TIME, TIMESTAMP, DATETIME, OID - equal iff same hash", "[contract]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  DB_VALUE v;
  std::vector < nv > vals;
  db_make_short (&v, 0);
  vals.push_back ({"SHORT 0", v});
  db_make_short (&v, -1);
  vals.push_back ({"SHORT -1", v});
  db_make_short (&v, 32767);
  vals.push_back ({"SHORT 32767", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  db_make_int (&v, 0);
  vals.push_back ({"INT 0", v});
  db_make_int (&v, 1);
  vals.push_back ({"INT 1", v});
  db_make_int (&v, -1);
  vals.push_back ({"INT -1", v});
  db_make_int (&v, 2147483647);
  vals.push_back ({"INT max", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  db_make_date (&v, 1, 1, 1970);
  vals.push_back ({"DATE 1970-01-01", v});
  db_make_date (&v, 1, 2, 1970);
  vals.push_back ({"DATE 1970-01-02", v});
  db_make_date (&v, 12, 31, 9999);
  vals.push_back ({"DATE 9999-12-31", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  db_make_time (&v, 0, 0, 0);
  vals.push_back ({"TIME 00:00:00", v});
  db_make_time (&v, 23, 59, 59);
  vals.push_back ({"TIME 23:59:59", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  db_make_timestamp (&v, 0);
  vals.push_back ({"TIMESTAMP 0", v});
  db_make_timestamp (&v, 1800000000U);
  vals.push_back ({"TIMESTAMP 1.8e9", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  DB_DATETIME d1 = { 5, 9 }, d2 = { 9, 5 }, d3 = { 5, 10 };
  db_make_datetime (&v, &d1);
  vals.push_back ({"DATETIME {5,9}", v});
  db_make_datetime (&v, &d2);
  vals.push_back ({"DATETIME {9,5}", v});
  db_make_datetime (&v, &d3);
  vals.push_back ({"DATETIME {5,10}", v});
  CHECK (inconsistencies (vals) == std::string ());

  vals.clear ();
  OID o1 = { 10, 3, 0 }, o2 = { 10, 4, 0 }, o3 = { 11, 3, 0 }, o4 = { 10, 3, 1 };
  db_make_oid (&v, &o1);
  vals.push_back ({"OID 10|3|0", v});
  db_make_oid (&v, &o2);
  vals.push_back ({"OID 10|4|0", v});
  db_make_oid (&v, &o3);
  vals.push_back ({"OID 11|3|0", v});
  db_make_oid (&v, &o4);
  vals.push_back ({"OID 10|3|1", v});
  CHECK (inconsistencies (vals) == std::string ());
}

/* ================================================================ defects (PASS = the defect is there) */

TEST_CASE ("PR8237 R1: VARCHAR 'a' and 'a ' compare unequal with ignore_trailing_space=no but hash alike - also '', "
	   "' ', '  ' (current behaviour; PASS = defect reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  /* system_parameter.c prm_Def PRM_ID_IGNORE_TRAILING_SPACE: default false; mr_cmpval_string reads it */
  REQUIRE (prm_get_bool_value (PRM_ID_IGNORE_TRAILING_SPACE) == false);

  for (int coll:{LANG_COLL_UTF8_BINARY, LANG_COLL_ISO_BINARY, LANG_COLL_UTF8_EN_CI})
    {
      INFO ("collation " << lang_get_collation_name (coll));
      DB_VALUE a = varchar ("a", coll), a_sp = varchar ("a ", coll), a_sp2 = varchar ("a  ", coll);
      DB_VALUE empty = varchar ("", coll), sp = varchar (" ", coll), sp2 = varchar ("  ", coll);
      WARN ("R1 " << lang_get_collation_name (coll) << ": hash 'a' " << H (a) << ", 'a ' " << H (a_sp) << ", 'a  ' "
	    << H (a_sp2) << ", '' " << H (empty) << ", ' ' " << H (sp) << ", '  ' " << H (sp2) << " [" << impl () << "]");

      /* the comparison the reservoir's caller makes keeps them apart ... */
      REQUIRE_FALSE (EQ (a, a_sp));
      REQUIRE_FALSE (EQ (a_sp, a_sp2));
      REQUIRE_FALSE (EQ (empty, sp));
      REQUIRE_FALSE (EQ (sp, sp2));
      /* ... (with ignore_trailing_space=yes the collation would call them equal - the hash follows that setting) */
      REQUIRE (QSTR_COMPARE (coll, (const unsigned char *) "a", 1, (const unsigned char *) "a ", 2, true) == 0);
      REQUIRE (QSTR_COMPARE (coll, (const unsigned char *) "a", 1, (const unsigned char *) "a ", 2, false) != 0);

      /* ... the hash folds them */
      CHECK (H (a) == H (a_sp));
      CHECK (H (a_sp) == H (a_sp2));
      CHECK (H (empty) == H (sp));
      CHECK (H (sp) == H (sp2));
    }
}

TEST_CASE ("PR8237 R1d: Duj1 of 2,048 VARCHAR values each with a trailing-space twin (4,096 distinct) is 2,048 "
	   "(current behaviour; PASS = defect reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  REQUIRE (prm_get_bool_value (PRM_ID_IGNORE_TRAILING_SPACE) == false);
  /* reservoir = population of 4,096 keys, so the exact answer is known: 'k0', 'k0 ', 'k1', 'k1 ', ... */
  const int n = unitseam_duj1_sample_capacity ();
  REQUIRE (n == 4096);
  std::vector < std::string > texts;
  for (int i = 0; i < n / 2; i++)
    {
      texts.push_back ("k" + std::to_string (i));
      texts.push_back ("k" + std::to_string (i) + " ");
    }
  std::vector < UINT64 > hashes;
  std::set < UINT64 > distinct;
  for (const std::string & t:texts)
    {
      DB_VALUE v = varchar (t.c_str (), LANG_COLL_UTF8_BINARY);
      hashes.push_back (H (v));
      distinct.insert (H (v));
    }
  DB_VALUE k0 = varchar ("k0", LANG_COLL_UTF8_BINARY), k0_sp = varchar ("k0 ", LANG_COLL_UTF8_BINARY);
  DB_VALUE k1 = varchar ("k1", LANG_COLL_UTF8_BINARY), k1_sp = varchar ("k1 ", LANG_COLL_UTF8_BINARY);
  REQUIRE_FALSE (EQ (k0, k0_sp));
  /* which values a hash folds together: the PR folds 'k0' with 'k0 '; a fix that xors the trailing-space count into
   * the string hash folds 'k0 ' with 'k1' instead (their collation hashes differ in the lowest bit only) */
  WARN ("R1d hash 'k0' " << H (k0) << ", 'k0 ' " << H (k0_sp) << ", 'k1' " << H (k1) << ", 'k1 ' " << H (k1_sp)
	<< " [" << impl () << "]");
  INT64 estimate = unitseam_duj1_estimate (hashes.data (), n, n);
  WARN ("R1d Duj1: 4,096 keys, true distinct level-0 prefixes " << n << ", distinct hashes " << distinct.size ()
	<< ", Duj1 estimate " << estimate << " [" << impl () << "]");
  CHECK (estimate < n);
}

TEST_CASE ("PR8237 R2: float NUMERIC 100 as (coefficient 1, scale -2, precision 1) and (100, 0, 3) compare equal but "
	   "hash apart - also 10^39 as (p39, s-1)/(p40, s0) and 0 with scale -2 (current behaviour; PASS = defect "
	   "reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  DB_VALUE c1_s_2 = float_numeric_raw ("1", 1, -2), c100_s0 = float_numeric_raw ("100", 3, 0);
  DB_VALUE lit100 = float_numeric ("100");
  WARN ("R2 A " << pscale (c1_s_2) << " hash " << H (c1_s_2) << ", B " << pscale (c100_s0) << " hash "
	<< H (c100_s0) << ", literal 100 " << pscale (lit100) << " [" << impl () << "]");
  REQUIRE (EQ (c100_s0, lit100));
  REQUIRE (EQ (c1_s_2, c100_s0));
  CHECK (H (c1_s_2) != H (c100_s0));

  /* precision 39, scale -1 (10^39) against precision 40, scale 0 */
  DB_VALUE p39 = float_numeric_raw ("100000000000000000000000000000000000000", 39, -1);
  DB_VALUE p40 = float_numeric_raw ("1000000000000000000000000000000000000000", 40, 0);
  WARN ("R2 C " << pscale (p39) << " hash " << H (p39) << ", D " << pscale (p40) << " hash " << H (p40));
  REQUIRE (EQ (p39, p40));
  CHECK (H (p39) != H (p40));

  /* zero with a negative scale */
  DB_VALUE z_s_2 = float_numeric_raw ("0", 1, -2), z = float_numeric ("0");
  WARN ("R2 E " << pscale (z_s_2) << " hash " << H (z_s_2) << ", F " << pscale (z) << " hash " << H (z));
  REQUIRE (EQ (z_s_2, z));
  CHECK (H (z_s_2) != H (z));
}

TEST_CASE ("PR8237 R2d: Duj1 of 2,048 float NUMERIC values each in both forms (coefficient k, scale -2) and (k*100, "
	   "scale 0) is 4,096 (current behaviour; PASS = defect reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  const int n = unitseam_duj1_sample_capacity ();
  std::vector < UINT64 > hashes;
  int unequal = 0;
  for (int k = 1; k <= n / 2; k++)
    {
      std::string a = std::to_string (k), b = std::to_string (k * 100);
      DB_VALUE va = float_numeric_raw (a.c_str (), (int) a.size (), -2);
      DB_VALUE vb = float_numeric_raw (b.c_str (), (int) b.size (), 0);
      unequal += EQ (va, vb) ? 0 : 1;
      hashes.push_back (H (va));
      hashes.push_back (H (vb));
    }
  REQUIRE (unequal == 0);
  INT64 estimate = unitseam_duj1_estimate (hashes.data (), n, n);
  WARN ("R2d Duj1: 4,096 keys, true distinct " << n / 2 << ", Duj1 estimate " << estimate << " [" << impl () << "]");
  CHECK (estimate > n / 2);
}

TEST_CASE ("PR8237 R2r: float NUMERIC subtraction gives 100 as precision 1, scale -2 and the float NUMERIC column "
	   "domain keeps it (reach; PASS = the engine's own arithmetic makes a form that hashes apart from its equal)",
	   "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  /* CAST ('1000...0100' AS NUMERIC): 42 digits -> precision 40, scale -2 (determine_prec_scale Case 1-B) */
  DB_VALUE big_a = float_numeric ("100000000000000000000000000000000000000100");
  DB_VALUE big_b = float_numeric ("100000000000000000000000000000000000000000");
  WARN ("R2r a " << pscale (big_a) << ", b " << pscale (big_b));
  bool fl = false;
  int p = 0, s = 0;
  db_get_numeric_precision_and_scale (&big_a, &p, &s, &fl);
  REQUIRE ((p == 40 && s == -2));

  /* the SQL '-' on two float NUMERICs (query_opfunc.c:4617 / type_checking.c:14704 constant folding) */
  DB_VALUE diff, zero_diff;
  REQUIRE (float_numeric_db_value_sub (&big_a, &big_b, &diff) == NO_ERROR);
  REQUIRE (float_numeric_db_value_sub (&big_a, &big_a, &zero_diff) == NO_ERROR);
  WARN ("R2r a - b " << pscale (diff) << ", a - a " << pscale (zero_diff));
  db_get_numeric_precision_and_scale (&diff, &p, &s, &fl);
  REQUIRE (fl);
  REQUIRE ((p == 1 && s == -2));

  /* INSERT into a NUMERIC column without (p,s): the float NUMERIC domain (csql_grammar.y NUMERIC opt_prec_2 ->
   * precision DB_DEFAULT_NUMERIC_PRECISION) - the cast keeps the precision and scale */
  DB_VALUE stored, stored_zero;
  db_make_null (&stored);
  db_make_null (&stored_zero);
  TP_DOMAIN *col = tp_domain_resolve_default (DB_TYPE_NUMERIC);
  REQUIRE (col->precision == DB_DEFAULT_NUMERIC_PRECISION);
  REQUIRE (tp_value_cast (&diff, &stored, col, false) == DOMAIN_COMPATIBLE);
  REQUIRE (tp_value_cast (&zero_diff, &stored_zero, col, false) == DOMAIN_COMPATIBLE);
  WARN ("R2r cast into NUMERIC (float domain): " << pscale (stored) << " hash " << H (stored) << ", zero "
	<< pscale (stored_zero) << " hash " << H (stored_zero));
  db_get_numeric_precision_and_scale (&stored, &p, &s, &fl);
  REQUIRE ((p == 1 && s == -2));

  DB_VALUE lit100 = float_numeric ("100"), lit0 = float_numeric ("0");
  WARN ("R2r literal 100 " << pscale (lit100) << " hash " << H (lit100) << ", literal 0 " << pscale (lit0) << " hash "
	<< H (lit0) << " [" << impl () << "]");
  REQUIRE (EQ (stored, lit100));
  REQUIRE (EQ (stored_zero, lit0));
  CHECK (H (stored) != H (lit100));
  CHECK (H (stored_zero) != H (lit0));
}

TEST_CASE ("PR8237 O1: euckr_bin - ' a' and 'a' (leading space) compare unequal but hash alike, and so do 'a' and 'a' "
	   "with a trailing EUC-KR space (current behaviour; PASS = defect reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  REQUIRE (prm_get_bool_value (PRM_ID_IGNORE_TRAILING_SPACE) == false);
  /* lang_mht2str_ko (language_support.c) maps an ASCII space or an EUC-KR space pair (0xA1 0xA1) to 0 and adds it to a
   * key that is still 0 at the start, then strips trailing ASCII and EUC-KR spaces; lang_fastcmp_ko compares a space
   * as 0 against the other string's byte */
  DB_VALUE a = varchar ("a", LANG_COLL_EUCKR_BINARY), sp_a = varchar (" a", LANG_COLL_EUCKR_BINARY);
  DB_VALUE sp2_a = varchar ("  a", LANG_COLL_EUCKR_BINARY), euc_a = varchar ("\xA1\xA1" "a", LANG_COLL_EUCKR_BINARY);
  DB_VALUE a_euc = varchar ("a\xA1\xA1", LANG_COLL_EUCKR_BINARY);
  WARN ("O1 euckr_bin: hash 'a' " << H (a) << ", ' a' " << H (sp_a) << ", '  a' " << H (sp2_a) << ", EUC space + 'a' "
	<< H (euc_a) << ", 'a' + EUC space " << H (a_euc) << " [" << impl () << "]");
  REQUIRE_FALSE (EQ (a, sp_a));
  REQUIRE_FALSE (EQ (a, sp2_a));
  REQUIRE_FALSE (EQ (a, euc_a));
  REQUIRE_FALSE (EQ (a, a_euc));
  CHECK (H (a) == H (sp_a));
  CHECK (H (a) == H (sp2_a));
  CHECK (H (a) == H (euc_a));
  CHECK (H (a) == H (a_euc));
}

/* day k after 1 January of start_y (proleptic Gregorian) */
static void
civil_day (int k, int start_y, int *y, int *m, int *d)
{
  static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  *y = start_y;
  *m = 1;
  *d = 1;
  while (k-- > 0)
    {
      int leap = ((*y % 4 == 0 && *y % 100 != 0) || *y % 400 == 0) && *m == 2;
      if (++*d > mdays[*m - 1] + leap)
	{
	  *d = 1;
	  if (++*m > 12)
	    {
	      *m = 1;
	      ++*y;
	    }
	}
    }
}

TEST_CASE ("PR8237 O2: VARCHAR 'YYYY-MM-DD' - distinct dates share a hash ('2015-01-20' = '2025-01-10', 32-bit "
	   "collation hash of mht_get_hash_number); 4,096 days from 2015-01-01 give fewer distinct hashes (current "
	   "behaviour; PASS = defect reproduced)", "[defect]")
{
  boot_types ();
  INFO ("element hash: " << impl ());
  for (int coll:{LANG_COLL_UTF8_BINARY, LANG_COLL_ISO_BINARY, LANG_COLL_UTF8_EN_CI})
    {
      INFO ("collation " << lang_get_collation_name (coll));
      DB_VALUE x = varchar ("2015-01-20", coll), y = varchar ("2025-01-10", coll);
      REQUIRE_FALSE (EQ (x, y));
      CHECK (H (x) == H (y));
    }

  const int n = unitseam_duj1_sample_capacity ();
  std::vector < std::string > dates;
  for (int k = 0; k < n; k++)
    {
      int y, m, d;
      char buf[16];
      civil_day (k, 2015, &y, &m, &d);
      snprintf (buf, sizeof (buf), "%04d-%02d-%02d", y, m, d);
      dates.push_back (buf);
    }
  REQUIRE (dates[0] == "2015-01-01");
  REQUIRE (std::set < std::string > (dates.begin (), dates.end ()).size () == (size_t) n);
  std::vector < UINT64 > by_day;
  std::set < UINT64 > distinct;
  for (const std::string & t:dates)
    {
      DB_VALUE v = varchar (t.c_str (), LANG_COLL_UTF8_BINARY);
      by_day.push_back (H (v));
      distinct.insert (H (v));
    }
  /* (a) the 4,096 dates once each - population = reservoir */
  INT64 est_once = unitseam_duj1_estimate (by_day.data (), n, n);
  /* (b) 1,000,000 keys of an index (date, id): each date on ~244 rows, a uniform 4,096-key reservoir (fixed seed);
   *     against the same reservoir hashed without collisions (the day number) */
  const INT64 keys = 1000000;
  std::vector < UINT64 > sample, ideal;
  unsigned long long state = 88172645463325252ULL;
  for (int i = 0; i < n; i++)
    {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      int day = (int) ((state % (unsigned long long) keys) % (unsigned long long) n);
      sample.push_back (by_day[day]);
      ideal.push_back ((UINT64) day * 0x9E3779B97F4A7C15ULL + 1);
    }
  INT64 est_rows = unitseam_duj1_estimate (sample.data (), n, keys);
  INT64 est_ideal = unitseam_duj1_estimate (ideal.data (), n, keys);
  WARN ("O2 utf8_bin 'YYYY-MM-DD' x 4,096 days from 2015-01-01: distinct hashes " << distinct.size ()
	<< "; Duj1 (a) once each: " << est_once << " (true 4096); (b) 1,000,000 keys, 4,096-key reservoir: "
	<< est_rows << " vs " << est_ideal << " with a collision-free hash (true 4096) [" << impl () << "]");
  CHECK (distinct.size () < (size_t) n);
  CHECK (est_once < n);

  /* a high-cardinality column (no value twice among 1,000,000 keys): 4,096 distinct '010-NNNN-NNNN' in the reservoir.
   * Duj1 reads a doubleton as a sign of few distinct values, so a handful of collisions moves the estimate far */
  std::set < std::string > phones;
  std::vector < UINT64 > ph, ph_ideal;
  while (phones.size () < (size_t) n)
    {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      char buf[24];
      snprintf (buf, sizeof (buf), "010-%04u-%04u", (unsigned) (state % 10000), (unsigned) ((state >> 20) % 10000));
      if (phones.insert (buf).second)
	{
	  DB_VALUE v = varchar (buf, LANG_COLL_UTF8_BINARY);
	  ph.push_back (H (v));
	  ph_ideal.push_back ((UINT64) phones.size () * 0x9E3779B97F4A7C15ULL + 1);
	}
    }
  std::set < UINT64 > ph_distinct (ph.begin (), ph.end ());
  WARN ("O2 utf8_bin '010-NNNN-NNNN' x 4,096 distinct, 1,000,000 distinct keys: distinct hashes " << ph_distinct.size ()
	<< "; Duj1 " << unitseam_duj1_estimate (ph.data (), n, keys) << " vs " << unitseam_duj1_estimate (ph_ideal.data (),
											       n, keys)
	<< " with a collision-free hash (true 1000000) [" << impl () << "]");
}
