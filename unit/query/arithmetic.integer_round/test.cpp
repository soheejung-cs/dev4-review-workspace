/*
 * PR#8474 (CBRD-27843) -- ROUND of a SMALLINT / INTEGER / BIGINT to an integer place in integers, an error past the
 * value's type, and the NaN guards of WIDTH_BUCKET (src/query/arithmetic.c, server mode).
 *
 *   R  [contract] round_bigint () (static, new in the PR): num rounded half away from zero at place, true when the
 *      result is past BIGINT -- the PR's table (place >= 0 as it is; -1: 15 -> 20, -25 -> -30, 14 -> 10; INT64_MAX
 *      and INT64_MIN at -1 overflow; (INT64_MAX, -18) = 9e18; -19: |num| >= 5e18 overflows, below that 0; -20 and
 *      below 0) and a 128-bit decimal oracle over edge and random values at places -1 .. -40. Registered only on a
 *      revision that has round_bigint (the hooks.cpp probe), so a develop run lists what develop has.
 *   C  [contract] db_round_dbval () of SMALLINT / INTEGER / BIGINT: an integer place rounds in integers and keeps
 *      the type, past the type it is ER_IT_DATA_OVERFLOW naming the type (a grid against the same oracle); a DOUBLE
 *      place that is an integer takes the integer path; a NaN or +Inf place keeps round_double ()'s "double"
 *      overflow, -Inf is the type's overflow (the PR's table); RETURN_NULL_ON_FUNCTION_ERRORS gives NULL and
 *      clears the error. What does not change: ordinary values, positive places, a fractional DOUBLE place, zero.
 *      (current behaviour) an error already in the context is what the function returns, and the overflow branch
 *      does not replace it -- the `er_errid () == NO_ERROR` test stands for "round_double () raised it".
 *   W  [contract] db_width_bucket (): a NaN operand or bound (DOUBLE, FLOAT, MONETARY, and on the NUMERIC path of a
 *      BIGINT / NUMERIC operand) -> ER_QPROC_INVALID_PARAMETER, a NaN count -> ER_PROC_WIDTH_BUCKET_COUNT; an
 *      ordinary value 3, +-Inf operand 6 / 0 -- and no TP_DOMAIN left allocated on any exit.
 *   D  [defect] "PR8474 D*": develop's answers the PR fixes (PASS = the defect is there). defects_fixed_in maps
 *      "PR8474 D7" to 9b821cbe1 and the other D's to dd9d27e7f -- they FAIL at head (history), PASS at develop.
 *   O  [defect] "PR8474 O*": what the PR leaves as it was (develop same -- open, separate issue candidates; PASS =
 *      still there): O1 the FLOAT branch narrows a rounding past FLT_MAX to +-Inf, O2 a DOUBLE rounding past DBL_MAX
 *      is +-Inf (round_double () checks the scaled value, not the result).
 *   D7 / C9  the implicit coercion of a NaN DOUBLE / FLOAT / MONETARY into NUMERIC: 0 in develop and dd9d27e7f (why
 *      develop's WIDTH_BUCKET NUMERIC path answered 3, [defect] D7), an overflow like an infinity's from 9b821cbe1
 *      on (numeric_internal_real_to_num, numeric_opfunc.c; [contract] C9 FAILs before it).
 *
 * Real code compiled into the binary: arithmetic.c (unit under test, with hooks.cpp appended), numeric_opfunc.c,
 * db_macro.c, object_primitive.c (pr_type_name, tp_Double's name "double" that round_double () reports),
 * object_domain.c + object_domain_convert.cpp (tp_value_coerce of WIDTH_BUCKET's NUMERIC path and of a NUMERIC
 * place -- develop's NaN -> NUMERIC coercion is what turns the NaN bound into 0 there).
 * Fakes (suite.json "fakes"): er_set / er_errid / er_clear (the last id, and the type name argument of
 * ER_IT_DATA_OVERFLOW), prm_Def (RETURN_NULL_ON_FUNCTION_ERRORS switched per case, the rest 0), area_alloc /
 * area_free (tp_domain_new / tp_domain_free before tp_init: calloc / free with a live count).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "area_alloc.h"
#include "object_domain.h"
#include "numeric_opfunc.h"
#include "arithmetic.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- hooks.cpp (appended to arithmetic.c) */

extern const bool unit_round_bigint_present;
bool unit_round_bigint (DB_BIGINT num, DB_BIGINT place, DB_BIGINT * result);

/* ---------------------------------------------------------------- fakes */

/* error_manager.c: er_set replaces the current error; er_errid reads it; er_clear resets it. The type name of
 * ER_IT_DATA_OVERFLOW (its one argument) is kept to tell "smallint" / "integer" / "bigint" / "double" apart. */
static int g_errid = NO_ERROR;
static std::string g_err_arg;
static int g_er_set_calls = 0;

void
er_set (int, const char *, const int, int err_id, int num_args, ...)
{
  g_errid = err_id;
  g_err_arg.clear ();
  g_er_set_calls++;
  if (err_id == ER_IT_DATA_OVERFLOW && num_args >= 1)
    {
      va_list ap;
      va_start (ap, num_args);
      const char *s = va_arg (ap, const char *);
      g_err_arg = s != NULL ? s : "(null)";
      va_end (ap);
    }
}

int
er_errid (void)
{
  return g_errid;
}

void
er_clear (void)
{
  g_errid = NO_ERROR;
  g_err_arg.clear ();
}

/* system_parameter.c: zero entries read as false / 0 (no session flag, so the global value is read) */
SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };
static bool
boot_params ()
{
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].value.v.b = false;
  prm_Def[PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS].value.v.b = false;
  return true;
}
static const bool params_booted = boot_params ();

struct return_null_on_errors
{
  return_null_on_errors ()
  {
    prm_Def[PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS].value.v.b = true;
  }
  ~return_null_on_errors ()
  {
    prm_Def[PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS].value.v.b = false;
  }
};

/* area_alloc.c: tp_domain_new / tp_domain_free take TP_DOMAINs from tp_Domain_area, which tp_init () creates; here
 * each is a calloc'ed block and the live count says whether every exit gave its domain back */
static int g_area_live = 0;
static int g_area_allocs = 0;

void *
area_alloc (AREA *)
{
  g_area_live++;
  g_area_allocs++;
  return calloc (1, sizeof (TP_DOMAIN));
}

int
area_free (AREA *, void *ptr)
{
  if (ptr != NULL)
    {
      g_area_live--;
      free (ptr);
    }
  return NO_ERROR;
}

/* ---------------------------------------------------------------- values */

static DB_VALUE
v_short (short s)
{
  DB_VALUE v;
  db_make_short (&v, s);
  return v;
}

static DB_VALUE
v_int (int i)
{
  DB_VALUE v;
  db_make_int (&v, i);
  return v;
}

static DB_VALUE
v_bigint (DB_BIGINT b)
{
  DB_VALUE v;
  db_make_bigint (&v, b);
  return v;
}

static DB_VALUE
v_double (double d)
{
  DB_VALUE v;
  db_make_double (&v, d);
  return v;
}

static DB_VALUE
v_float (float f)
{
  DB_VALUE v;
  db_make_float (&v, f);
  return v;
}

static DB_VALUE
v_monetary (double amount)
{
  DB_VALUE v;
  /* a dollar amount: DB_CURRENCY_DEFAULT asks lang_currency (), which needs a booted language */
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, amount);
  return v;
}

/* a floating NUMERIC from its text */
static DB_VALUE
v_numeric (const char *text)
{
  DB_VALUE v;
  db_make_null (&v);
  REQUIRE (numeric_coerce_string_to_num (text, (int) strlen (text), INTL_CODESET_ISO88591, &v) == NO_ERROR);
  return v;
}

static const double NaN = std::numeric_limits<double>::quiet_NaN ();
static const double Inf = std::numeric_limits<double>::infinity ();
static const DB_BIGINT I64_MAX = std::numeric_limits<DB_BIGINT>::max ();
static const DB_BIGINT I64_MIN = std::numeric_limits<DB_BIGINT>::min ();

static std::string
err_name (int code)
{
  switch (code)
    {
    case ER_IT_DATA_OVERFLOW:
      return "ER_IT_DATA_OVERFLOW";
    case ER_QPROC_INVALID_PARAMETER:
      return "ER_QPROC_INVALID_PARAMETER";
    case ER_PROC_WIDTH_BUCKET_COUNT:
      return "ER_PROC_WIDTH_BUCKET_COUNT";
    case ER_QPROC_OVERFLOW_ADDITION:
      return "ER_QPROC_OVERFLOW_ADDITION";
    case ER_QPROC_OVERFLOW_SUBTRACTION:
      return "ER_QPROC_OVERFLOW_SUBTRACTION";
    case ER_QPROC_OVERFLOW_DIVISION:
      return "ER_QPROC_OVERFLOW_DIVISION";
    case ER_QPROC_INVALID_DATATYPE:
      return "ER_QPROC_INVALID_DATATYPE";
    default:
      return std::to_string (code);
    }
}

static std::string
fmt_double (double d)
{
  if (std::isnan (d))
    {
      return "nan";
    }
  if (std::isinf (d))
    {
      return d > 0 ? "inf" : "-inf";
    }
  char buf[64];
  snprintf (buf, sizeof (buf), "%.17g", d);
  return buf;
}

/* what a call answered: "error <code>(<type name>)", "NULL", or "<type> <value>"; a value next to a raised error
 * (rc 0 with er_errid () set) is marked */
static std::string
show (int rc, const DB_VALUE & r)
{
  if (rc != NO_ERROR)
    {
      return "error " + err_name (rc) + (g_err_arg.empty () ? "" : "(" + g_err_arg + ")");
    }
  std::string s;
  if (DB_IS_NULL (&r))
    {
      s = "NULL";
    }
  else
    {
      switch (DB_VALUE_DOMAIN_TYPE (&r))
	{
	case DB_TYPE_SHORT:
	  s = "smallint " + std::to_string (db_get_short (&r));
	  break;
	case DB_TYPE_INTEGER:
	  s = "integer " + std::to_string (db_get_int (&r));
	  break;
	case DB_TYPE_BIGINT:
	  s = "bigint " + std::to_string ((long long) db_get_bigint (&r));
	  break;
	case DB_TYPE_FLOAT:
	  s = "float " + fmt_double (db_get_float (&r));
	  break;
	case DB_TYPE_DOUBLE:
	  s = "double " + fmt_double (db_get_double (&r));
	  break;
	default:
	  s = "type " + std::to_string ((int) DB_VALUE_DOMAIN_TYPE (&r));
	  break;
	}
    }
  if (g_errid != NO_ERROR)
    {
      s += " +er_errid " + err_name (g_errid);
    }
  return s;
}

static void
clear_errors ()
{
  g_errid = NO_ERROR;
  g_err_arg.clear ();
}

/* ROUND (a, b) as fetch calls it: the result starts NULL */
static std::string
rnd (DB_VALUE a, DB_VALUE b)
{
  DB_VALUE r;
  db_make_null (&r);
  clear_errors ();
  int rc = db_round_dbval (&r, &a, &b);
  return show (rc, r);
}

/* WIDTH_BUCKET (a, lo, hi, n): n is the DOUBLE the function reads with db_get_double */
static std::string
wb (DB_VALUE a, DB_VALUE lo, DB_VALUE hi, double n)
{
  DB_VALUE r, cnt;
  db_make_null (&r);
  db_make_double (&cnt, n);
  clear_errors ();
  int live = g_area_live;
  int rc = db_width_bucket (&r, &a, &lo, &hi, &cnt);
  std::string s = show (rc, r);
  if (g_area_live != live)
    {
      s += " +domains left " + std::to_string (g_area_live - live);
    }
  return s;
}

/* ---------------------------------------------------------------- the decimal oracle */

/* num rounded half away from zero at place, in 128 bits: false and *out when it fits a BIGINT, true when not */
static bool
oracle_round (DB_BIGINT num, long long place, DB_BIGINT * out)
{
  if (place >= 0)
    {
      *out = num;
      return false;
    }
  if (place < -38)
    {
      *out = 0;
      return false;
    }
  __int128 unit = 1;
  for (long long i = 0; i < -place; i++)
    {
      unit *= 10;
    }
  __int128 n = num;
  __int128 rest = n % unit;
  __int128 q = n - rest;
  if (2 * rest >= unit)
    {
      q += unit;
    }
  else if (2 * rest <= -unit)
    {
      q -= unit;
    }
  if (q > (__int128) I64_MAX || q < (__int128) I64_MIN)
    {
      return true;
    }
  *out = (DB_BIGINT) q;
  return false;
}

static const char *
type_label (DB_TYPE t)
{
  return t == DB_TYPE_SHORT ? "smallint" : t == DB_TYPE_INTEGER ? "integer" : "bigint";
}

/* what the PR says ROUND (value of type t, integer place) answers */
static std::string
expect_integer_round (DB_TYPE t, DB_BIGINT v, long long place)
{
  DB_BIGINT q = 0;
  bool overflow = oracle_round (v, place, &q);
  if (!overflow && t == DB_TYPE_SHORT)
    {
      overflow = q > SHRT_MAX || q < SHRT_MIN;
    }
  if (!overflow && t == DB_TYPE_INTEGER)
    {
      overflow = q > INT_MAX || q < INT_MIN;
    }
  if (overflow)
    {
      return std::string ("error ER_IT_DATA_OVERFLOW(") + type_label (t) + ")";
    }
  return std::string (type_label (t)) + " " + std::to_string ((long long) q);
}

static DB_VALUE
v_of (DB_TYPE t, DB_BIGINT v)
{
  return t == DB_TYPE_SHORT ? v_short ((short) v) : t == DB_TYPE_INTEGER ? v_int ((int) v) : v_bigint (v);
}

/* edge values of a type: its ends, around 0, and k*10^j +- half a unit +- 1 for every power that fits */
static std::vector<DB_BIGINT>
edge_values (DB_TYPE t)
{
  const DB_BIGINT hi = t == DB_TYPE_SHORT ? SHRT_MAX : t == DB_TYPE_INTEGER ? INT_MAX : I64_MAX;
  const DB_BIGINT lo = t == DB_TYPE_SHORT ? SHRT_MIN : t == DB_TYPE_INTEGER ? INT_MIN : I64_MIN;
  std::vector<DB_BIGINT> out;
  for (int d = 0; d <= 12; d++)
    {
      out.push_back (hi - d);
      out.push_back (lo + d);
    }
  for (int d = -12; d <= 12; d++)
    {
      out.push_back (d);
    }
  __int128 p = 10;
  for (int j = 1; j <= 19; j++, p *= 10)
    {
      for (int k = -9; k <= 9; k++)
	{
	  for (int e = -1; e <= 1; e++)
	    {
	      __int128 x = (__int128) k * p + p / 2 + e;
	      __int128 y = (__int128) k * p - p / 2 + e;
	      if (x <= hi && x >= lo)
		{
		  out.push_back ((DB_BIGINT) x);
		}
	      if (y <= hi && y >= lo)
		{
		  out.push_back ((DB_BIGINT) y);
		}
	    }
	}
    }
  /* 2^53 + 1 and its neighbours: a BIGINT a double cannot hold */
  if (t == DB_TYPE_BIGINT)
    {
      for (DB_BIGINT x : { 9007199254740993LL, -9007199254740993LL, 4999999999999999999LL, 5000000000000000000LL,
	   -4999999999999999999LL, -5000000000000000000LL, 9000000000000000000LL, -9000000000000000000LL })
	{
	  out.push_back (x);
	}
    }
  return out;
}

static std::vector<DB_BIGINT>
random_values (DB_TYPE t, int n, std::uint64_t seed)
{
  std::mt19937_64 g (seed);
  std::vector<DB_BIGINT> out;
  for (int i = 0; i < n; i++)
    {
      std::uint64_t r = g ();
      DB_BIGINT x;
      if (t == DB_TYPE_SHORT)
	{
	  x = (short) (r & 0xffff);
	}
      else if (t == DB_TYPE_INTEGER)
	{
	  x = (int) (r & 0xffffffffu);
	}
      else
	{
	  /* every magnitude, not only the huge ones a uniform 64-bit draw gives */
	  int shift = (int) ((r >> 58) & 63);
	  x = (DB_BIGINT) (g () >> shift);
	  if (r & 1)
	    {
	      x = -x;
	    }
	}
      out.push_back (x);
    }
  return out;
}

static void
verbose (const std::string & what)
{
  if (getenv ("UNITSEAM_VERBOSE") != NULL)
    {
      fprintf (stderr, "  %s\n", what.c_str ());
    }
}

/* ================================================================ R: round_bigint () -- registered where it exists */

#define RB_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = unit_round_bigint_present \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

static std::string
rb (DB_BIGINT num, DB_BIGINT place)
{
  DB_BIGINT r = 12345;
  bool overflow = unit_round_bigint (num, place, &r);
  return overflow ? "overflow" : std::to_string ((long long) r);
}

/* registered on every revision: the probe must not lose round_bigint where the TU rounds in integers */
TEST_CASE ("R0 guard: the round_bigint probe finds the function exactly where ROUND (BIGINT max, 0) gives the value "
	   "itself (the integer path) - R1 and R2 are registered only there", "[contract]")
{
  std::string exact = rnd (v_bigint (I64_MAX), v_int (0));
  bool integer_path = exact == "bigint 9223372036854775807";
  WARN ("round_bigint " << (unit_round_bigint_present ? "present" : "absent") << ", ROUND (BIGINT max, 0) = " << exact);
  CHECK (unit_round_bigint_present == integer_path);
}

RB_TEST_CASE (rb_table, "R1 round_bigint: the PR's table - place >= 0 as it is, half away from zero at -1, "
	      "INT64_MAX / INT64_MIN at -1 overflow, (INT64_MAX, -18) = 9e18, -19 overflows from abs (num) >= 5e18 "
	      "and is 0 below, -20 and below 0", "[contract]")
{
  /* place 0 and positive */
  CHECK (rb (15, 0) == "15");
  CHECK (rb (-15, 0) == "-15");
  CHECK (rb (15, 3) == "15");
  CHECK (rb (I64_MAX, 0) == "9223372036854775807");
  CHECK (rb (I64_MIN, 0) == "-9223372036854775808");
  CHECK (rb (I64_MAX, I64_MAX) == "9223372036854775807");
  CHECK (rb (I64_MIN, 1) == "-9223372036854775808");

  /* -1: half away from zero */
  CHECK (rb (15, -1) == "20");
  CHECK (rb (-25, -1) == "-30");
  CHECK (rb (14, -1) == "10");
  CHECK (rb (-14, -1) == "-10");
  CHECK (rb (-15, -1) == "-20");
  CHECK (rb (25, -1) == "30");
  CHECK (rb (5, -1) == "10");
  CHECK (rb (-5, -1) == "-10");
  CHECK (rb (4, -1) == "0");
  CHECK (rb (-4, -1) == "0");
  CHECK (rb (0, -1) == "0");

  /* the ends at -1 */
  CHECK (rb (I64_MAX, -1) == "overflow");
  CHECK (rb (I64_MIN, -1) == "overflow");
  CHECK (rb (I64_MAX - 7, -1) == "9223372036854775800");
  CHECK (rb (I64_MIN + 8, -1) == "-9223372036854775800");
  CHECK (rb (I64_MAX, -2) == "9223372036854775800");
  CHECK (rb (I64_MAX, -3) == "overflow");

  /* -18: the largest power of ten a BIGINT holds */
  CHECK (rb (I64_MAX, -18) == "9000000000000000000");
  CHECK (rb (I64_MIN, -18) == "-9000000000000000000");
  CHECK (rb (8500000000000000000LL, -18) == "9000000000000000000");
  CHECK (rb (-8500000000000000000LL, -18) == "-9000000000000000000");
  CHECK (rb (499999999999999999LL, -18) == "0");
  CHECK (rb (500000000000000000LL, -18) == "1000000000000000000");

  /* -19: the unit is past BIGINT */
  CHECK (rb (4999999999999999999LL, -19) == "0");
  CHECK (rb (5000000000000000000LL, -19) == "overflow");
  CHECK (rb (-4999999999999999999LL, -19) == "0");
  CHECK (rb (-5000000000000000000LL, -19) == "overflow");
  CHECK (rb (I64_MAX, -19) == "overflow");
  CHECK (rb (I64_MIN, -19) == "overflow");

  /* -20 and below */
  CHECK (rb (I64_MAX, -20) == "0");
  CHECK (rb (I64_MIN, -20) == "0");
  CHECK (rb (5000000000000000000LL, -20) == "0");
  CHECK (rb (-5000000000000000000LL, -400) == "0");
  CHECK (rb (I64_MAX, -400) == "0");
  CHECK (rb (1, I64_MIN) == "0");
}

RB_TEST_CASE (rb_oracle, "R2 round_bigint: equals a 128-bit decimal oracle (round half away from zero, true past "
	      "BIGINT) for edge and random values at places 1 .. -40", "[contract]")
{
  std::vector<DB_BIGINT> values = edge_values (DB_TYPE_BIGINT);
  std::vector<DB_BIGINT> rnd_values = random_values (DB_TYPE_BIGINT, 20000, 0x8474);
  values.insert (values.end (), rnd_values.begin (), rnd_values.end ());
  long long n = 0, bad = 0;
  for (DB_BIGINT v : values)
    {
      for (long long place = 1; place >= -40; place--)
	{
	  DB_BIGINT want = 0, got = 0;
	  bool want_ovf = oracle_round (v, place, &want);
	  bool got_ovf = unit_round_bigint (v, place, &got);
	  n++;
	  if (want_ovf != got_ovf || (!want_ovf && want != got))
	    {
	      if (bad++ < 8)
		{
		  INFO ("round_bigint (" << (long long) v << ", " << place << ") = " << (got_ovf ? "overflow" :
			std::to_string ((long long) got)) << ", oracle " << (want_ovf ? "overflow" :
			std::to_string ((long long) want)));
		  CHECK (false);
		}
	    }
	}
    }
  verbose ("R2: " + std::to_string (n) + " (value, place) pairs, " + std::to_string (bad) + " differ");
  CHECK (bad == 0);
}

/* ================================================================ C: db_round_dbval () */

TEST_CASE ("C1 ROUND of SMALLINT / INTEGER / BIGINT past the type at an integer place is ER_IT_DATA_OVERFLOW naming "
	   "the type (32767 / -32768 at -1, INT max / min at -1, BIGINT max / min at -1, BIGINT max at -19, "
	   "SMALLINT 32767 at a NUMERIC place -1.5 that casts to -2)", "[contract]")
{
  CHECK (rnd (v_short (32767), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(smallint)");
  CHECK (rnd (v_short (-32768), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(smallint)");
  CHECK (rnd (v_short (32767), v_numeric ("-1.5")) == "error ER_IT_DATA_OVERFLOW(smallint)");
  CHECK (rnd (v_int (INT_MAX), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(integer)");
  CHECK (rnd (v_int (INT_MIN), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(integer)");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(bigint)");
  CHECK (rnd (v_bigint (I64_MIN), v_int (-1)) == "error ER_IT_DATA_OVERFLOW(bigint)");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-19)) == "error ER_IT_DATA_OVERFLOW(bigint)");
  CHECK (rnd (v_bigint (5000000000000000000LL), v_bigint (-19)) == "error ER_IT_DATA_OVERFLOW(bigint)");
  CHECK (rnd (v_bigint (-5000000000000000000LL), v_short (-19)) == "error ER_IT_DATA_OVERFLOW(bigint)");
}

TEST_CASE ("C2 ROUND of SMALLINT / INTEGER / BIGINT that fits is exact and keeps the type ((INT max, -9) = 2000000000, "
	   "(BIGINT max, 0 / 2 / 20) itself, (BIGINT max, -18) = 9e18, 9007199254740993 itself, "
	   "(4999999999999999999, -19) = 0, -400 = 0, 309 / 400 / BIGINT max places = the value)", "[contract]")
{
  CHECK (rnd (v_int (INT_MAX), v_int (-9)) == "integer 2000000000");
  CHECK (rnd (v_int (INT_MIN), v_int (-9)) == "integer -2000000000");
  CHECK (rnd (v_int (INT_MAX), v_int (-10)) == "integer 0");
  CHECK (rnd (v_short (32767), v_int (-5)) == "smallint 0");
  CHECK (rnd (v_short (32764), v_int (-1)) == "smallint 32760");
  CHECK (rnd (v_short (-32764), v_int (-1)) == "smallint -32760");
  CHECK (rnd (v_bigint (I64_MAX), v_int (0)) == "bigint 9223372036854775807");
  CHECK (rnd (v_bigint (I64_MAX), v_int (2)) == "bigint 9223372036854775807");
  CHECK (rnd (v_bigint (I64_MAX), v_int (20)) == "bigint 9223372036854775807");
  CHECK (rnd (v_bigint (I64_MIN), v_int (0)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-18)) == "bigint 9000000000000000000");
  CHECK (rnd (v_bigint (9007199254740993LL), v_int (0)) == "bigint 9007199254740993");
  CHECK (rnd (v_bigint (4999999999999999999LL), v_int (0)) == "bigint 4999999999999999999");
  CHECK (rnd (v_bigint (4999999999999999999LL), v_int (-19)) == "bigint 0");
  CHECK (rnd (v_int (5), v_int (-400)) == "integer 0");
  CHECK (rnd (v_int (INT_MAX), v_int (-400)) == "integer 0");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-400)) == "bigint 0");
  CHECK (rnd (v_short (-32768), v_int (-400)) == "smallint 0");
  CHECK (rnd (v_int (5), v_int (400)) == "integer 5");
  CHECK (rnd (v_int (5), v_int (309)) == "integer 5");
  CHECK (rnd (v_int (5), v_bigint (I64_MAX)) == "integer 5");
  CHECK (rnd (v_int (5), v_bigint (I64_MIN)) == "integer 0");
}

TEST_CASE ("C3 ROUND of SMALLINT / INTEGER / BIGINT at an integer place equals the decimal oracle with the type's "
	   "range (edge and random values, INTEGER / SHORT / BIGINT places -45 .. 3 and -400 / 400)", "[contract]")
{
  long long n = 0, bad = 0;
  for (DB_TYPE t : { DB_TYPE_SHORT, DB_TYPE_INTEGER, DB_TYPE_BIGINT })
    {
      std::vector<DB_BIGINT> values = edge_values (t);
      std::vector<DB_BIGINT> rv = random_values (t, 600, 0x27843 + (int) t);
      values.insert (values.end (), rv.begin (), rv.end ());
      std::vector<long long> places = { 3, 2, 1, 400, -400 };
      for (long long p = 0; p >= -45; p--)
	{
	  places.push_back (p);
	}
      for (DB_BIGINT v : values)
	{
	  for (long long place : places)
	    {
	      /* the place as each integer type a query can give it */
	      for (int pk = 0; pk < 3; pk++)
		{
		  if (pk == 0 && (place > SHRT_MAX || place < SHRT_MIN))
		    {
		      continue;
		    }
		  DB_VALUE pv = pk == 0 ? v_short ((short) place) : pk == 1 ? v_int ((int) place) : v_bigint (place);
		  std::string want = expect_integer_round (t, v, place);
		  std::string got = rnd (v_of (t, v), pv);
		  n++;
		  if (got != want && bad++ < 8)
		    {
		      INFO ("ROUND (" << type_label (t) << " " << (long long) v << ", " << place << ") = " << got
			    << ", oracle " << want);
		      CHECK (false);
		    }
		}
	    }
	}
    }
  verbose ("C3: " + std::to_string (n) + " calls, " + std::to_string (bad) + " differ");
  CHECK (bad == 0);
}

TEST_CASE ("C4 a DOUBLE place that is a finite integer takes the integer path (INT max at -1.0 = integer overflow, "
	   "15 at -1.0 = 20, BIGINT max at -18.0 = 9e18, -1e300 = 0, 1e300 = the value, -0.0 = the value)",
	   "[contract]")
{
  CHECK (rnd (v_int (INT_MAX), v_double (-1.0)) == "error ER_IT_DATA_OVERFLOW(integer)");
  CHECK (rnd (v_int (15), v_double (-1.0)) == "integer 20");
  CHECK (rnd (v_short (-25), v_double (-1.0)) == "smallint -30");
  CHECK (rnd (v_bigint (I64_MAX), v_double (-18.0)) == "bigint 9000000000000000000");
  CHECK (rnd (v_bigint (I64_MAX), v_double (-1e300)) == "bigint 0");
  CHECK (rnd (v_int (7), v_double (1e300)) == "integer 7");
  CHECK (rnd (v_int (7), v_double (-0.0)) == "integer 7");
}

TEST_CASE ("C5 a place that is not a finite integer keeps the double rounding: NaN and +Inf keep round_double ()'s "
	   "\"double\" overflow, -Inf is the type's overflow (the PR's table), a fractional place rounds as before "
	   "((1234, -1.5) = 1233), and zero stays 0 at any place; (current behaviour) a fractional place below "
	   "about -323.6, where pow () underflows to 0, is the type's overflow as -Inf is (0 / 0)", "[contract]")
{
  CHECK (rnd (v_int (5), v_double (NaN)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_short (5), v_double (NaN)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_bigint (5), v_double (NaN)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_int (5), v_double (Inf)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_int (5), v_double (-Inf)) == "error ER_IT_DATA_OVERFLOW(integer)");
  CHECK (rnd (v_short (5), v_double (-Inf)) == "error ER_IT_DATA_OVERFLOW(smallint)");
  CHECK (rnd (v_bigint (5), v_double (-Inf)) == "error ER_IT_DATA_OVERFLOW(bigint)");
  CHECK (rnd (v_int (1234), v_double (-1.5)) == "integer 1233");
  CHECK (rnd (v_int (5), v_double (-300.5)) == "integer 0");
  CHECK (rnd (v_int (5), v_double (-400.5)) == "error ER_IT_DATA_OVERFLOW(integer)");
  CHECK (rnd (v_int (0), v_double (NaN)) == "integer 0");
  CHECK (rnd (v_int (0), v_double (-Inf)) == "integer 0");
  CHECK (rnd (v_int (0), v_double (Inf)) == "integer 0");
}

TEST_CASE ("C6 with return_null_on_function_errors the overflow is NULL and the error is cleared (SMALLINT 32767 at "
	   "-1, INT 5 at -Inf, INT 5 at NaN)", "[contract]")
{
  return_null_on_errors on;
  CHECK (rnd (v_short (32767), v_int (-1)) == "NULL");
  CHECK (er_errid () == NO_ERROR);
  CHECK (rnd (v_int (5), v_double (-Inf)) == "NULL");
  CHECK (er_errid () == NO_ERROR);
  CHECK (rnd (v_int (5), v_double (NaN)) == "NULL");
  CHECK (er_errid () == NO_ERROR);
  CHECK (rnd (v_int (15), v_int (-1)) == "integer 20");
}

TEST_CASE ("C7 (current behaviour) an error already in the context is what db_round_dbval returns (its return "
	   "er_errid ()): the overflow branch does not raise its own over it, and a value that fits returns it too",
	   "[contract]")
{
  DB_VALUE r, a, b;

  /* an overflow: the stale error stays, no ER_IT_DATA_OVERFLOW */
  db_make_null (&r);
  a = v_short (32767);
  b = v_int (-1);
  clear_errors ();
  er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_QPROC_INVALID_DATATYPE, 0);
  int calls = g_er_set_calls;
  int rc = db_round_dbval (&r, &a, &b);
  CHECK (err_name (rc) == "ER_QPROC_INVALID_DATATYPE");
  CHECK (g_er_set_calls - calls == 0);

  /* a value that fits: the same stale error is the return */
  db_make_null (&r);
  a = v_int (15);
  clear_errors ();
  er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_QPROC_INVALID_DATATYPE, 0);
  rc = db_round_dbval (&r, &a, &b);
  CHECK (err_name (rc) == "ER_QPROC_INVALID_DATATYPE");
  clear_errors ();
}

TEST_CASE ("C8 what the PR keeps: ordinary values and directions (15 -> 20, -25 -> -30, 14 -> 10 in each type), "
	   "positive places, the result type, DOUBLE / MONETARY ROUND", "[contract]")
{
  CHECK (rnd (v_short (15), v_int (-1)) == "smallint 20");
  CHECK (rnd (v_short (-25), v_int (-1)) == "smallint -30");
  CHECK (rnd (v_short (14), v_int (-1)) == "smallint 10");
  CHECK (rnd (v_int (15), v_int (-1)) == "integer 20");
  CHECK (rnd (v_int (-25), v_int (-1)) == "integer -30");
  CHECK (rnd (v_int (14), v_int (-1)) == "integer 10");
  CHECK (rnd (v_bigint (15), v_int (-1)) == "bigint 20");
  CHECK (rnd (v_bigint (-25), v_int (-1)) == "bigint -30");
  CHECK (rnd (v_bigint (14), v_int (-1)) == "bigint 10");
  CHECK (rnd (v_int (1234), v_int (2)) == "integer 1234");
  CHECK (rnd (v_int (1250), v_int (-2)) == "integer 1300");
  CHECK (rnd (v_double (2.5), v_int (0)) == "double 3");
  CHECK (rnd (v_double (-2.5), v_int (0)) == "double -3");
  CHECK (rnd (v_double (1234.5678), v_int (2)) == "double 1234.5699999999999");
}

/* ================================================================ W: db_width_bucket () */

TEST_CASE ("W1 WIDTH_BUCKET of a NaN operand, lower or upper bound is ER_QPROC_INVALID_PARAMETER (DOUBLE, FLOAT, "
	   "MONETARY; the NUMERIC path of a BIGINT and of a NUMERIC operand) and leaves no domain", "[contract]")
{
  CHECK (wb (v_double (NaN), v_double (0), v_double (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_double (5), v_double (NaN), v_double (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_double (5), v_double (0), v_double (NaN), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_float (std::numeric_limits<float>::quiet_NaN ()), v_float (0), v_float (10), 5)
	 == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_monetary (NaN), v_monetary (0), v_monetary (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_monetary (5), v_monetary (NaN), v_monetary (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_int (5), v_double (NaN), v_double (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_bigint (5), v_double (NaN), v_double (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_bigint (5), v_double (0), v_double (NaN), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_bigint (5), v_monetary (NaN), v_monetary (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_bigint (5), v_float (std::numeric_limits<float>::quiet_NaN ()), v_float (10), 5)
	 == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (wb (v_numeric ("5.5"), v_double (NaN), v_double (10), 5) == "error ER_QPROC_INVALID_PARAMETER");
  CHECK (g_area_live == 0);
}

TEST_CASE ("W2 WIDTH_BUCKET with a NaN bucket count is ER_PROC_WIDTH_BUCKET_COUNT, like a count below 1 or from "
	   "INT max on (DOUBLE and BIGINT operands)", "[contract]")
{
  CHECK (wb (v_double (5), v_double (0), v_double (10), NaN) == "error ER_PROC_WIDTH_BUCKET_COUNT");
  CHECK (wb (v_bigint (5), v_bigint (0), v_bigint (10), NaN) == "error ER_PROC_WIDTH_BUCKET_COUNT");
  CHECK (wb (v_double (5), v_double (0), v_double (10), 0.5) == "error ER_PROC_WIDTH_BUCKET_COUNT");
  CHECK (wb (v_double (5), v_double (0), v_double (10), (double) INT_MAX) == "error ER_PROC_WIDTH_BUCKET_COUNT");
  CHECK (wb (v_double (5), v_double (0), v_double (10), Inf) == "error ER_PROC_WIDTH_BUCKET_COUNT");
  CHECK (wb (v_double (5), v_double (0), v_double (10), -Inf) == "error ER_PROC_WIDTH_BUCKET_COUNT");
}

TEST_CASE ("W3 WIDTH_BUCKET of ordinary and infinite operands is unchanged: (5, 0, 10, 5) = 3, +Inf = 6, -Inf = 0, "
	   "a reversed range, the BIGINT and NUMERIC paths, and no domain left", "[contract]")
{
  CHECK (wb (v_double (5), v_double (0), v_double (10), 5) == "integer 3");
  CHECK (wb (v_double (Inf), v_double (0), v_double (10), 5) == "integer 6");
  CHECK (wb (v_double (-Inf), v_double (0), v_double (10), 5) == "integer 0");
  CHECK (wb (v_double (5), v_double (10), v_double (0), 5) == "integer 3");
  CHECK (wb (v_int (5), v_int (0), v_int (10), 5) == "integer 3");
  CHECK (wb (v_bigint (5), v_bigint (0), v_bigint (10), 5) == "integer 3");
  CHECK (wb (v_bigint (5), v_double (0), v_double (10), 5) == "integer 3");
  CHECK (wb (v_numeric ("5.5"), v_numeric ("0"), v_numeric ("10"), 5) == "integer 3");
  CHECK (wb (v_double (10), v_double (0), v_double (10), 5) == "integer 6");
  CHECK (wb (v_double (-0.5), v_double (0), v_double (10), 5) == "integer 0");
  CHECK (g_area_live == 0);
}

TEST_CASE ("W4 with return_null_on_function_errors a NaN operand or count is NULL and the error is cleared",
	   "[contract]")
{
  return_null_on_errors on;
  CHECK (wb (v_double (NaN), v_double (0), v_double (10), 5) == "NULL");
  CHECK (er_errid () == NO_ERROR);
  CHECK (wb (v_double (5), v_double (0), v_double (10), NaN) == "NULL");
  CHECK (er_errid () == NO_ERROR);
  CHECK (wb (v_bigint (5), v_double (NaN), v_double (10), 5) == "NULL");
  CHECK (er_errid () == NO_ERROR);
}

/* ================================================================ D: develop's answers the PR fixes */

TEST_CASE ("PR8474 D1: ROUND of SMALLINT 32767 / -32768 to place -1 wraps to -32766 / 32766 without an error, and "
	   "32767 at a NUMERIC place -1.5 (cast to -2) to -32736 (develop) (PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_short (32767), v_int (-1)) == "smallint -32766");
  CHECK (rnd (v_short (-32768), v_int (-1)) == "smallint 32766");
  CHECK (rnd (v_short (32767), v_numeric ("-1.5")) == "smallint -32736");
}

TEST_CASE ("PR8474 D2: ROUND of INTEGER max / min to place -1 (INTEGER or DOUBLE -1.0) gives -2147483648 without an "
	   "error (develop) (PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_int (INT_MAX), v_int (-1)) == "integer -2147483648");
  CHECK (rnd (v_int (INT_MIN), v_int (-1)) == "integer -2147483648");
  CHECK (rnd (v_int (INT_MAX), v_double (-1.0)) == "integer -2147483648");
}

TEST_CASE ("PR8474 D3: ROUND of BIGINT max at places 0 / 2 / 20 / -1 / -19 and of BIGINT min at -1 gives "
	   "-9223372036854775808 without an error (develop) (PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_bigint (I64_MAX), v_int (0)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MAX), v_int (2)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MAX), v_int (20)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-1)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-19)) == "bigint -9223372036854775808");
  CHECK (rnd (v_bigint (I64_MIN), v_int (-1)) == "bigint -9223372036854775808");
}

TEST_CASE ("PR8474 D4: ROUND through a double is off where it fits - (INT max, -9) = 1999999999, (INT min, -9) = "
	   "-1999999999, (BIGINT max, -18) = 8999999999999998976, 9007199254740993 -> 9007199254740992, "
	   "4999999999999999999 -> 5000000000000000000 and at -19 -> BIGINT min (develop) (PASS = defect present)",
	   "[defect]")
{
  CHECK (rnd (v_int (INT_MAX), v_int (-9)) == "integer 1999999999");
  CHECK (rnd (v_int (INT_MIN), v_int (-9)) == "integer -1999999999");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-18)) == "bigint 8999999999999998976");
  CHECK (rnd (v_bigint (9007199254740993LL), v_int (0)) == "bigint 9007199254740992");
  CHECK (rnd (v_bigint (4999999999999999999LL), v_int (0)) == "bigint 5000000000000000000");
  CHECK (rnd (v_bigint (4999999999999999999LL), v_int (-19)) == "bigint -9223372036854775808");
}

TEST_CASE ("PR8474 D5: ROUND at place -400 or -Inf gives the type's minimum through 0 / 0 = NaN (INT 5 -> "
	   "-2147483648, BIGINT max -> -9223372036854775808), and at places 309 / 400 / BIGINT max raises a "
	   "\"double\" overflow for a value that needs no rounding (develop) (PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_int (5), v_int (-400)) == "integer -2147483648");
  CHECK (rnd (v_bigint (I64_MAX), v_int (-400)) == "bigint -9223372036854775808");
  CHECK (rnd (v_int (5), v_double (-Inf)) == "integer -2147483648");
  CHECK (rnd (v_int (5), v_int (400)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_int (5), v_int (309)) == "error ER_IT_DATA_OVERFLOW(double)");
  CHECK (rnd (v_int (5), v_bigint (I64_MAX)) == "error ER_IT_DATA_OVERFLOW(double)");
}

TEST_CASE ("PR8474 D6: WIDTH_BUCKET with a NaN gives a bucket - operand -2147483648 (DOUBLE, FLOAT, MONETARY), "
	   "lower bound 6, upper bound 0, count -1073741823, and a NaN bound (DOUBLE, MONETARY, FLOAT) of a BIGINT or "
	   "NUMERIC operand, coerced to NUMERIC 0, 3 (develop) (PASS = defect present)", "[defect]")
{
  CHECK (wb (v_double (NaN), v_double (0), v_double (10), 5) == "integer -2147483648");
  CHECK (wb (v_float (std::numeric_limits<float>::quiet_NaN ()), v_float (0), v_float (10), 5)
	 == "integer -2147483648");
  CHECK (wb (v_monetary (NaN), v_monetary (0), v_monetary (10), 5) == "integer -2147483648");
  CHECK (wb (v_double (5), v_double (NaN), v_double (10), 5) == "integer 6");
  CHECK (wb (v_double (5), v_double (0), v_double (NaN), 5) == "integer 0");
  CHECK (wb (v_double (5), v_double (0), v_double (10), NaN) == "integer -1073741823");
  CHECK (wb (v_bigint (5), v_double (NaN), v_double (10), 5) == "integer 3");
  CHECK (wb (v_bigint (5), v_monetary (NaN), v_monetary (10), 5) == "integer 3");
  CHECK (wb (v_bigint (5), v_float (std::numeric_limits<float>::quiet_NaN ()), v_float (10), 5) == "integer 3");
  CHECK (wb (v_numeric ("5.5"), v_double (NaN), v_double (10), 5) == "integer 3");
}

/* ================================================================ O: left as it was (open) */

TEST_CASE ("PR8474 O1: ROUND of a FLOAT that rounds past FLT_MAX narrows the double to +Inf without an error "
	   "((FLT_MAX, -35) = 3.403e38 -> inf) - the FLOAT branch keeps the unchecked narrowing the PR removes for the "
	   "integers (develop same) (PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_float (FLT_MAX), v_int (-35)) == "float inf");
  CHECK (rnd (v_float (-FLT_MAX), v_int (-35)) == "float -inf");
}

TEST_CASE ("PR8474 O2: ROUND of a DOUBLE whose rounding carries past DBL_MAX gives +-Inf without an error ((DBL_MAX, "
	   "-307) = 18 / 1e-307) - round_double () checks the scaled value only, not the result (develop same) "
	   "(PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_double (DBL_MAX), v_int (-307)) == "double inf");
  CHECK (rnd (v_double (-DBL_MAX), v_int (-307)) == "double -inf");
}

/* tp_value_coerce of a value into WIDTH_BUCKET's NUMERIC (2 * DB_BIGINT_PRECISION, DB_FLOAT_DECIMAL_PRECISION) */
static std::string
coerce_to_width_bucket_numeric (DB_VALUE src)
{
  TP_DOMAIN *d = tp_domain_new (DB_TYPE_NUMERIC);
  REQUIRE (d != NULL);
  d->precision = 2 * DB_BIGINT_PRECISION;
  d->scale = DB_FLOAT_DECIMAL_PRECISION;
  DB_VALUE dst;
  db_make_null (&dst);
  clear_errors ();
  TP_DOMAIN_STATUS st = tp_value_coerce (&src, &dst, d);
  tp_domain_free (d);
  std::string s = st == DOMAIN_COMPATIBLE ? "compatible" : st == DOMAIN_OVERFLOW ? "overflow"
    : st == DOMAIN_INCOMPATIBLE ? "incompatible" : "status " + std::to_string ((int) st);
  if (st == DOMAIN_COMPATIBLE && !DB_IS_NULL (&dst) && DB_VALUE_DOMAIN_TYPE (&dst) == DB_TYPE_NUMERIC)
    {
      char buf[128];
      s += std::string (" ") + numeric_db_value_print (&dst, buf);
    }
  return s;
}

TEST_CASE ("PR8474 D7: the implicit coercion of a NaN DOUBLE / FLOAT / MONETARY into a NUMERIC is DOMAIN_COMPATIBLE "
	   "with the value 0 while an infinity is an overflow - why develop's WIDTH_BUCKET NUMERIC path answered 3 for "
	   "a NaN bound (develop and dd9d27e7f; 9b821cbe1 makes it an overflow) (PASS = defect present)", "[defect]")
{
  CHECK (coerce_to_width_bucket_numeric (v_double (NaN)) == "compatible 0.0000000");
  CHECK (coerce_to_width_bucket_numeric (v_float (std::numeric_limits<float>::quiet_NaN ())) == "compatible 0.0000000");
  CHECK (coerce_to_width_bucket_numeric (v_monetary (NaN)) == "compatible 0.0000000");
  CHECK (coerce_to_width_bucket_numeric (v_double (Inf)) == "overflow");
  CHECK (g_area_live == 0);
}

TEST_CASE ("C9 the implicit coercion of a NaN DOUBLE / FLOAT / MONETARY into a NUMERIC is DOMAIN_OVERFLOW as an "
	   "infinity's is (numeric_internal_real_to_num, 9b821cbe1 - FAIL before it), and a finite value still "
	   "converts", "[contract]")
{
  CHECK (coerce_to_width_bucket_numeric (v_double (NaN)) == "overflow");
  CHECK (coerce_to_width_bucket_numeric (v_float (std::numeric_limits<float>::quiet_NaN ())) == "overflow");
  CHECK (coerce_to_width_bucket_numeric (v_monetary (NaN)) == "overflow");
  CHECK (coerce_to_width_bucket_numeric (v_double (Inf)) == "overflow");
  CHECK (coerce_to_width_bucket_numeric (v_double (-Inf)) == "overflow");
  CHECK (coerce_to_width_bucket_numeric (v_double (2.5)) == "compatible 2.5000000");
  CHECK (g_area_live == 0);
}
