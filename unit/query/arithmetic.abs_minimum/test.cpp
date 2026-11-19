/*
 * CBRD-28015 -- ABS of the minimum of SMALLINT / INTEGER / BIGINT (src/query/arithmetic.c db_abs_dbval (), server
 * mode).
 *
 *   A  [contract] db_abs_dbval () of SMALLINT / INTEGER / BIGINT: every value but the minimum gives its absolute
 *      value in the same type with no error (edges and 20,000 random values per type against a wider oracle); the
 *      minimum returns ER_IT_DATA_OVERFLOW and sets it with the name of the type ("smallint" / "integer" /
 *      "bigint"), the error of a CAST past the range; with RETURN_NULL_ON_FUNCTION_ERRORS the minimum gives NULL,
 *      NO_ERROR and sets no error. A NULL value returns NO_ERROR and leaves the result as it was (the callers
 *      handle NULL before the call). DOUBLE / FLOAT / MONETARY keep fabs () (their edges and -0.0).
 *   D  [defect] "CBRD28015 D1": develop's answer -- the minimum comes back as the negative minimum with NO_ERROR
 *      (SMALLINT through the narrowing of abs () to short, INTEGER / BIGINT through abs () / llabs () of the
 *      minimum, which is undefined behavior and gives the minimum on x86-64). PASS = the defect is there; FAILs
 *      from dcf66492f on (defects_fixed_in).
 *
 * Real code compiled into the binary: arithmetic.c (unit under test), numeric_opfunc.c, db_macro.c,
 * object_primitive.c (pr_type_name), object_domain.c + object_domain_convert.cpp (what arithmetic.c links to).
 * Fakes (suite.json "fakes"): er_set / er_errid / er_clear (the last id, the type name argument of
 * ER_IT_DATA_OVERFLOW and a call count), prm_Def (RETURN_NULL_ON_FUNCTION_ERRORS switched per case, the rest 0),
 * area_alloc / area_free (calloc / free -- tp_init is not run).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "area_alloc.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "numeric_opfunc.h"
#include "arithmetic.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

/* ---------------------------------------------------------------- fakes */

/* error_manager.c: er_set replaces the current error; er_errid reads it; er_clear resets it. The type name of
 * ER_IT_DATA_OVERFLOW (its one argument) is kept to tell "smallint" / "integer" / "bigint" apart. */
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

/* area_alloc.c: tp_domain_new / tp_domain_free take TP_DOMAINs from tp_Domain_area, which tp_init () creates */
void *
area_alloc (AREA *)
{
  return calloc (1, sizeof (TP_DOMAIN));
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

/* ---------------------------------------------------------------- helpers */

struct abs_call
{
  int rc;
  DB_VALUE result;
  int errid;
  std::string arg;
  int er_set_calls;
};

/* the result starts as an INTEGER 777, so an untouched result is visible */
static abs_call
call_abs (DB_VALUE value)
{
  abs_call c;
  er_clear ();
  g_er_set_calls = 0;
  db_make_int (&c.result, 777);
  c.rc = db_abs_dbval (&c.result, &value);
  c.errid = g_errid;
  c.arg = g_err_arg;
  c.er_set_calls = g_er_set_calls;
  return c;
}

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

/* ---------------------------------------------------------------- [contract] */

TEST_CASE ("A1 every value but the minimum gives its absolute value in the same type", "[contract]")
{
  const short shorts[] = { -32767, -32766, -1000, -1, 0, 1, 1000, 32766, 32767 };
  for (short s : shorts)
    {
      abs_call c = call_abs (v_short (s));
      INFO ("smallint " << s);
      CHECK (c.rc == NO_ERROR);
      CHECK (c.er_set_calls == 0);
      CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_SHORT);
      CHECK (db_get_short (&c.result) == (short) std::abs ((int) s));
    }

  const int ints[] = { INT_MIN + 1, INT_MIN + 2, -100000, -1, 0, 1, 100000, INT_MAX - 1, INT_MAX };
  for (int i : ints)
    {
      abs_call c = call_abs (v_int (i));
      INFO ("integer " << i);
      CHECK (c.rc == NO_ERROR);
      CHECK (c.er_set_calls == 0);
      CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_INTEGER);
      CHECK ((long long) db_get_int (&c.result) == std::llabs ((long long) i));
    }

  const DB_BIGINT bigints[] = { LLONG_MIN + 1, LLONG_MIN + 2, -10000000000LL, -1, 0, 1, 10000000000LL,
    LLONG_MAX - 1, LLONG_MAX
  };
  for (DB_BIGINT b : bigints)
    {
      abs_call c = call_abs (v_bigint (b));
      INFO ("bigint " << b);
      CHECK (c.rc == NO_ERROR);
      CHECK (c.er_set_calls == 0);
      CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_BIGINT);
      CHECK (db_get_bigint (&c.result) == (b < 0 ? -b : b));
    }
}

TEST_CASE ("A2 random values other than the minimum against a wider oracle", "[contract]")
{
  std::mt19937_64 rng (28015);
  int bad = 0;
  for (int n = 0; n < 20000; n++)
    {
      short s = (short) (rng () % 65535 - 32767);	/* -32767 .. 32767 */
      abs_call c = call_abs (v_short (s));
      bad += !(c.rc == NO_ERROR && DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_SHORT
	       && (int) db_get_short (&c.result) == std::abs ((int) s));

      int i = (int) (rng () % 4294967295ULL - 2147483647LL);	/* -2147483647 .. 2147483647 */
      c = call_abs (v_int (i));
      bad += !(c.rc == NO_ERROR && DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_INTEGER
	       && (long long) db_get_int (&c.result) == std::llabs ((long long) i));

      DB_BIGINT b = (DB_BIGINT) rng ();
      if (b == LLONG_MIN)
	{
	  b++;
	}
      c = call_abs (v_bigint (b));
      __int128 want = b < 0 ? -(__int128) b : (__int128) b;
      bad += !(c.rc == NO_ERROR && DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_BIGINT
	       && (__int128) db_get_bigint (&c.result) == want);
    }
  CHECK (bad == 0);
}

TEST_CASE ("A3 the minimum is ER_IT_DATA_OVERFLOW naming the type", "[contract]")
{
  struct
  {
    DB_VALUE v;
    const char *name;
  } mins[] = {
    { v_short (SHRT_MIN), "smallint" },
    { v_int (INT_MIN), "integer" },
    { v_bigint (LLONG_MIN), "bigint" },
  };
  for (auto &m : mins)
    {
      abs_call c = call_abs (m.v);
      INFO (m.name);
      CHECK (c.rc == ER_IT_DATA_OVERFLOW);
      CHECK (c.errid == ER_IT_DATA_OVERFLOW);
      CHECK (c.er_set_calls == 1);
      CHECK (c.arg == m.name);
    }
}

TEST_CASE ("A4 with RETURN_NULL_ON_FUNCTION_ERRORS the minimum is NULL with no error", "[contract]")
{
  return_null_on_errors on;
  DB_VALUE mins[] = { v_short (SHRT_MIN), v_int (INT_MIN), v_bigint (LLONG_MIN) };
  for (DB_VALUE & m : mins)
    {
      abs_call c = call_abs (m);
      INFO (pr_type_name (DB_VALUE_DOMAIN_TYPE (&m)));
      CHECK (c.rc == NO_ERROR);
      CHECK (c.errid == NO_ERROR);
      CHECK (c.er_set_calls == 0);
      CHECK (DB_IS_NULL (&c.result));
    }

  /* the other values do not change with the parameter */
  abs_call c = call_abs (v_int (INT_MIN + 1));
  CHECK (c.rc == NO_ERROR);
  CHECK (db_get_int (&c.result) == INT_MAX);
}

TEST_CASE ("A5 a NULL value returns NO_ERROR and leaves the result as it was", "[contract]")
{
  DB_VALUE n;
  db_make_null (&n);
  abs_call c = call_abs (n);
  CHECK (c.rc == NO_ERROR);
  CHECK (c.er_set_calls == 0);
  CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_INTEGER);
  CHECK (db_get_int (&c.result) == 777);

  DB_VALUE typed_null;
  db_value_domain_init (&typed_null, DB_TYPE_INTEGER, 0, 0);
  c = call_abs (typed_null);
  CHECK (c.rc == NO_ERROR);
  CHECK (db_get_int (&c.result) == 777);
}

TEST_CASE ("A6 DOUBLE, FLOAT and MONETARY keep fabs ()", "[contract]")
{
  const double doubles[] = { -DBL_MAX, -1.5, -0.0, 0.0, DBL_MIN, DBL_MAX };
  for (double d : doubles)
    {
      DB_VALUE v;
      db_make_double (&v, d);
      abs_call c = call_abs (v);
      INFO ("double " << d);
      CHECK (c.rc == NO_ERROR);
      CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_DOUBLE);
      CHECK (db_get_double (&c.result) == std::fabs (d));
      CHECK (!std::signbit (db_get_double (&c.result)));
    }

  const float floats[] = { -FLT_MAX, -2.5f, -0.0f, FLT_MAX };
  for (float f : floats)
    {
      DB_VALUE v;
      db_make_float (&v, f);
      abs_call c = call_abs (v);
      INFO ("float " << f);
      CHECK (c.rc == NO_ERROR);
      CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_FLOAT);
      CHECK (db_get_float (&c.result) == std::fabs (f));
    }

  DB_VALUE m;
  db_make_monetary (&m, DB_CURRENCY_DOLLAR, -12.25);
  abs_call c = call_abs (m);
  CHECK (c.rc == NO_ERROR);
  CHECK (DB_VALUE_DOMAIN_TYPE (&c.result) == DB_TYPE_MONETARY);
  CHECK (db_get_monetary (&c.result)->amount == 12.25);
}

/* ---------------------------------------------------------------- [defect] */

TEST_CASE ("CBRD28015 D1 develop gives the minimum back negative with no error", "[defect]")
{
  abs_call c = call_abs (v_short (SHRT_MIN));
  CHECK (c.rc == NO_ERROR);
  CHECK (db_get_short (&c.result) == SHRT_MIN);

  c = call_abs (v_int (INT_MIN));
  CHECK (c.rc == NO_ERROR);
  CHECK (db_get_int (&c.result) == INT_MIN);

  c = call_abs (v_bigint (LLONG_MIN));
  CHECK (c.rc == NO_ERROR);
  CHECK (db_get_bigint (&c.result) == LLONG_MIN);
}
