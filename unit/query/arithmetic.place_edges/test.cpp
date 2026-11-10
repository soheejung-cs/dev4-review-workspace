/*
 * PR#8474 (CBRD-27843) self-review (.51, 2026-11-10) -- the edges `query/arithmetic.integer_round` (the author's
 * suite, dev4 14dff39) does not hold, and the paths around the PR it does not change (src/query/arithmetic.c, server).
 *
 *   E  [contract] the PR's functions at edges integer_round leaves out:
 *      E1 db_round_dbval () of SMALLINT / INTEGER / BIGINT at a place given as a BIGINT or a SMALLINT at its type's
 *         ends: INT64_MIN and -32768 are 0, INT64_MAX and 32767 keep the value (develop: 0 / 0 = NaN narrowed to the
 *         type's minimum, and a "double" overflow for INT64_MAX).
 *      E2 db_width_bucket ()'s last range check, now OR_CHECK_INT_OVERFLOW_FROM_FP: with NaN inputs refused, the one
 *         input that still brings a NaN there is a bucket width that underflows to 0 with the operand on the lower
 *         bound (0 / 0) -- ER_QPROC_OVERFLOW_ADDITION (develop narrows the NaN to INT_MIN). A nonzero numerator over
 *         that width stays ER_QPROC_OVERFLOW_DIVISION. (current behaviour: the bucket the operand is in is 1 -- a
 *         subnormal width is the precision class, reported apart)
 *      U1 tp_value_cast () in place of a NaN DOUBLE / FLOAT / MONETARY into NUMERIC (10, 2) -- the call
 *         heap_object_upgrade_domain () makes for each row of ALTER TABLE ... CHANGE / MODIFY into NUMERIC: from
 *         9b821cbe1 DOMAIN_OVERFLOW with the value left a NaN (develop DOMAIN_COMPATIBLE 0.00). The strict=no branch
 *         of that function (heap_file.c, static) then reads the sign with `v >= 0`, false for a NaN, and writes
 *         db_value_domain_min () of the domain -- -99999999.99, checked here.
 *      U2 (develop same since CBRD-27660) the same in-place cast of a NaN into INTEGER / SMALLINT / BIGINT already
 *         overflows -- the integer targets of that ALTER already take the minimum for a NaN; U1 adds NUMERIC.
 *   T  [defect] TRUNC, the sibling of ROUND the PR leaves (develop and release/11.4 same; PASS = defect present):
 *      T1 db_trunc_dbval () of a BIGINT at a BIGINT place INT64_MIN divides by zero (truncate_bigint (): -INT64_MIN
 *         wraps to INT64_MIN, pow (10, -9.2e18) = 0, num % 0) -- SIGFPE, run in a child process
 *      T2 SMALLINT / INTEGER / DOUBLE at place -400 keep their value (truncate_double (): pow () underflows to 0, the
 *         scaled 0 equals its floor and the function returns the value "as it is") -- 0 is right
 *      T3 BIGINT INT64_MIN at place -19 / -400 stays INT64_MIN (truncate_bigint (): (DB_BIGINT) pow (10, 19) is past
 *         the range, x86 gives INT64_MIN, INT64_MIN % INT64_MIN = 0) -- 0 is right
 *   O3 [defect] ROUND of a DOUBLE / FLOAT / MONETARY at place -400 is NaN (round_double (): pow () underflows to 0,
 *      round (0) / 0) where the PR makes the integers 0 (develop same; PASS = defect present).
 *
 * Real code compiled into the binary: arithmetic.c (unit under test), numeric_opfunc.c, db_macro.c,
 * object_primitive.c, object_domain.c + object_domain_convert.cpp (tp_value_cast, the NUMERIC converter).
 * Fakes (suite.json "fakes"): er_set / er_errid / er_clear (the last id), prm_Def (RETURN_NULL_ON_FUNCTION_ERRORS
 * switched per case, the rest 0), area_alloc / area_free (tp_domain_new / tp_domain_free before tp_init).
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
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

/* ---------------------------------------------------------------- fakes */

/* error_manager.c: er_set replaces the current error; er_errid reads it; er_clear resets it */
static int g_errid = NO_ERROR;
static std::string g_err_arg;

void
er_set (int, const char *, const int, int err_id, int num_args, ...)
{
  g_errid = err_id;
  g_err_arg.clear ();
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

/* system_parameter.c: zero entries read as false / 0 */
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

/* area_alloc.c: tp_domain_new / tp_domain_free before tp_init () -- calloc'ed blocks with a live count */
static int g_area_live = 0;

void *
area_alloc (AREA *)
{
  g_area_live++;
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
  /* DB_CURRENCY_DEFAULT asks lang_currency (), which needs a booted language */
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, amount);
  return v;
}

static const double NaN = std::numeric_limits<double>::quiet_NaN ();
static const float NaNf = std::numeric_limits<float>::quiet_NaN ();
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
    case ER_QPROC_OVERFLOW_DIVISION:
      return "ER_QPROC_OVERFLOW_DIVISION";
    case ER_QPROC_OVERFLOW_SUBTRACTION:
      return "ER_QPROC_OVERFLOW_SUBTRACTION";
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

/* what a call answered: "error <code>(<type name>)", "NULL", or "<type> <value>" */
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
	case DB_TYPE_MONETARY:
	  s = "monetary " + fmt_double (db_get_monetary (&r)->amount);
	  break;
	case DB_TYPE_NUMERIC:
	  {
	    char buf[128];
	    s = std::string ("numeric ") + numeric_db_value_print (&r, buf);
	  }
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

/* TRUNC (a, b) as fetch calls it */
static std::string
trc (DB_VALUE a, DB_VALUE b)
{
  DB_VALUE r;
  db_make_null (&r);
  clear_errors ();
  int rc = db_trunc_dbval (&r, &a, &b);
  return show (rc, r);
}

/* TRUNC (a, b) in a child: "signal <n>" when it dies, else what it answered */
static std::string
trc_in_child (DB_VALUE a, DB_VALUE b)
{
  int fd[2];
  REQUIRE (pipe (fd) == 0);
  fflush (stdout);
  fflush (stderr);
  pid_t pid = fork ();
  REQUIRE (pid >= 0);
  if (pid == 0)
    {
      /* the test framework's handlers would report the signal as a failure of this process */
      signal (SIGFPE, SIG_DFL);
      signal (SIGSEGV, SIG_DFL);
      signal (SIGABRT, SIG_DFL);
      close (fd[0]);
      std::string s = trc (a, b);
      ssize_t w = write (fd[1], s.c_str (), s.size ());
      (void) w;
      close (fd[1]);
      _exit (0);
    }
  close (fd[1]);
  std::string out;
  char buf[256];
  ssize_t n;
  while ((n = read (fd[0], buf, sizeof (buf))) > 0)
    {
      out.append (buf, (size_t) n);
    }
  close (fd[0]);
  int status = 0;
  REQUIRE (waitpid (pid, &status, 0) == pid);
  if (WIFSIGNALED (status))
    {
      return "signal " + std::to_string (WTERMSIG (status));
    }
  return out;
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

/* tp_value_cast (v, v, <type> (p, s), false) -- ALTER TABLE ... CHANGE converts each row's value in place so:
 * "<status> <the value after>" */
static std::string
cast_in_place (DB_VALUE v, DB_TYPE type, int p, int s)
{
  TP_DOMAIN *d = tp_domain_new (type);
  REQUIRE (d != NULL);
  d->precision = p;
  d->scale = s;
  clear_errors ();
  TP_DOMAIN_STATUS st = tp_value_cast (&v, &v, d, false);
  std::string out = st == DOMAIN_COMPATIBLE ? "compatible" : st == DOMAIN_OVERFLOW ? "overflow"
    : st == DOMAIN_INCOMPATIBLE ? "incompatible" : "status " + std::to_string ((int) st);
  clear_errors ();
  out += " -> " + show (NO_ERROR, v);
  tp_domain_free (d);
  return out;
}

static std::string
cast_in_place_to_numeric (DB_VALUE v, int p, int s)
{
  return cast_in_place (v, DB_TYPE_NUMERIC, p, s);
}

/* ================================================================ E / U: the PR's functions, edges left out */

TEST_CASE ("E1 ROUND of SMALLINT / INTEGER / BIGINT at a BIGINT or SMALLINT place at its type's ends: INT64_MIN and "
	   "-32768 give 0, INT64_MAX and 32767 keep the value (develop: the type's minimum through 0 / 0, and a "
	   "\"double\" overflow)", "[contract]")
{
  CHECK (rnd (v_int (5), v_bigint (I64_MIN)) == "integer 0");
  CHECK (rnd (v_short (5), v_bigint (I64_MIN)) == "smallint 0");
  CHECK (rnd (v_bigint (I64_MIN), v_bigint (I64_MIN)) == "bigint 0");
  CHECK (rnd (v_bigint (I64_MAX), v_bigint (I64_MIN)) == "bigint 0");
  CHECK (rnd (v_bigint (I64_MAX), v_bigint (I64_MAX)) == "bigint 9223372036854775807");
  CHECK (rnd (v_int (INT_MIN), v_bigint (I64_MAX)) == "integer -2147483648");
  CHECK (rnd (v_int (5), v_short (SHRT_MIN)) == "integer 0");
  CHECK (rnd (v_int (INT_MAX), v_short (SHRT_MAX)) == "integer 2147483647");
}

TEST_CASE ("E2 WIDTH_BUCKET whose bucket width underflows to 0 with the operand on the lower bound (0 / 0 = NaN) is "
	   "ER_QPROC_OVERFLOW_ADDITION, the last range check refusing a NaN (develop: INT_MIN); a nonzero numerator "
	   "over it is ER_QPROC_OVERFLOW_DIVISION as before; with return_null_on_function_errors NULL",
	   "[contract]")
{
  /* (1e-320 - 0) / 100000 = 1e-325 rounds to 0: (0 - 0) / 0 = NaN */
  CHECK (wb (v_double (0), v_double (0), v_double (1e-320), 100000) == "error ER_QPROC_OVERFLOW_ADDITION");
  /* the descending range: (1e-320 - 1e-320) / ((1e-320 - 0) / 100000) */
  CHECK (wb (v_double (1e-320), v_double (1e-320), v_double (0), 100000) == "error ER_QPROC_OVERFLOW_ADDITION");
  /* 5e-324 / 0 = +inf: the division check */
  CHECK (wb (v_double (5e-324), v_double (0), v_double (1e-320), 100000) == "error ER_QPROC_OVERFLOW_DIVISION");
  /* the same width, an operand outside: no division */
  CHECK (wb (v_double (-1), v_double (0), v_double (1e-320), 100000) == "integer 0");
  CHECK (wb (v_double (1), v_double (0), v_double (1e-320), 100000) == "integer 100001");
  {
    return_null_on_errors on;
    CHECK (wb (v_double (0), v_double (0), v_double (1e-320), 100000) == "NULL");
    CHECK (er_errid () == NO_ERROR);
  }
}

TEST_CASE ("U1 tp_value_cast in place (ALTER TABLE ... CHANGE's per-row call) of a NaN DOUBLE / FLOAT / MONETARY "
	   "into NUMERIC (10, 2) is DOMAIN_OVERFLOW and leaves the value a NaN (develop: compatible 0.00); an "
	   "infinity overflows as before, 1.5 converts; the domain minimum the strict=no ALTER writes for a value "
	   "that is not >= 0 is -99999999.99", "[contract]")
{
  CHECK (cast_in_place_to_numeric (v_double (NaN), 10, 2) == "overflow -> double nan");
  CHECK (cast_in_place_to_numeric (v_float (NaNf), 10, 2) == "overflow -> float nan");
  CHECK (cast_in_place_to_numeric (v_monetary (NaN), 10, 2) == "overflow -> monetary nan");
  CHECK (cast_in_place_to_numeric (v_double (-std::numeric_limits<double>::infinity ()), 10, 2)
	 == "overflow -> double -inf");
  CHECK (cast_in_place_to_numeric (v_double (1.5), 10, 2) == "compatible -> numeric 1.50");
  CHECK (g_area_live == 0);

  /* heap_object_upgrade_domain ()'s strict=no branch: is_positive = (db_get_double (v) >= 0) -- false for a NaN */
  CHECK_FALSE (NaN >= 0);
  DB_VALUE m;
  db_make_null (&m);
  REQUIRE (db_value_domain_min (&m, DB_TYPE_NUMERIC, 10, 2, 0, 0, NULL) == NO_ERROR);
  CHECK (show (NO_ERROR, m) == "numeric -99999999.99");
}

TEST_CASE ("U2 (develop same since CBRD-27660) tp_value_cast in place of a NaN DOUBLE into INTEGER / SMALLINT / "
	   "BIGINT is DOMAIN_OVERFLOW with the value left a NaN - the integer targets of ALTER TABLE ... CHANGE "
	   "already take the strict=no minimum for a NaN; U1 adds NUMERIC to them", "[contract]")
{
  CHECK (cast_in_place (v_double (NaN), DB_TYPE_INTEGER, 0, 0) == "overflow -> double nan");
  CHECK (cast_in_place (v_double (NaN), DB_TYPE_SHORT, 0, 0) == "overflow -> double nan");
  CHECK (cast_in_place (v_double (NaN), DB_TYPE_BIGINT, 0, 0) == "overflow -> double nan");
  CHECK (cast_in_place (v_double (1.5), DB_TYPE_INTEGER, 0, 0) == "compatible -> integer 2");
  CHECK (g_area_live == 0);
}

/* ================================================================ T / O: the siblings the PR leaves (develop same) */

TEST_CASE ("PR8474 T1: TRUNC of a BIGINT at a BIGINT place INT64_MIN dies of SIGFPE - truncate_bigint () negates "
	   "INT64_MIN (wraps), pow (10, -9.2e18) = 0, num % 0 (develop and release/11.4 same) (PASS = defect present)",
	   "[defect]")
{
  CHECK (trc_in_child (v_bigint (5), v_bigint (I64_MIN)) == "signal " + std::to_string (SIGFPE));
  CHECK (trc_in_child (v_bigint (I64_MIN), v_bigint (I64_MIN)) == "signal " + std::to_string (SIGFPE));
}

TEST_CASE ("PR8474 T2: TRUNC of a SMALLINT / INTEGER / DOUBLE at place -400 keeps the value (truncate_double (): "
	   "pow () underflows to 0, the scaled 0 equals its floor, 'return as it is') where 0 is right - ROUND of the "
	   "integers gives 0 from the PR on (develop same) (PASS = defect present)", "[defect]")
{
  CHECK (trc (v_int (1234), v_int (-400)) == "integer 1234");
  CHECK (trc (v_short (1234), v_int (-400)) == "smallint 1234");
  CHECK (trc (v_double (1234.5), v_int (-400)) == "double 1234.5");
  /* where pow () still gives a subnormal the answer is right */
  CHECK (trc (v_int (1234), v_int (-320)) == "integer 0");
}

TEST_CASE ("PR8474 T3: TRUNC of BIGINT INT64_MIN at place -19 or -400 stays INT64_MIN - truncate_bigint ()'s "
	   "(DB_BIGINT) pow (10, 19) is past the range (x86: INT64_MIN, and INT64_MIN % INT64_MIN = 0) where 0 is "
	   "right (develop same) (PASS = defect present)", "[defect]")
{
  CHECK (trc (v_bigint (I64_MIN), v_int (-19)) == "bigint -9223372036854775808");
  CHECK (trc (v_bigint (I64_MIN), v_int (-400)) == "bigint -9223372036854775808");
  /* the same wrong divisor happens to give 0 for every other value */
  CHECK (trc (v_bigint (I64_MAX), v_int (-19)) == "bigint 0");
}

TEST_CASE ("PR8474 O3: ROUND of a DOUBLE / FLOAT / MONETARY at place -400 is NaN without an error (round_double (): "
	   "pow () underflows to 0, round (0) / 0) where the PR makes the integers 0 (develop same) "
	   "(PASS = defect present)", "[defect]")
{
  CHECK (rnd (v_double (1234.5), v_int (-400)) == "double nan");
  CHECK (rnd (v_float (1234.5f), v_int (-400)) == "float nan");
  CHECK (rnd (v_monetary (1234.5), v_int (-400)) == "monetary nan");
}
