/*
 * CBRD-28024 (.51, 2026-11-10) -- WIDTH_BUCKET of a BIGINT or NUMERIC operand, db_width_bucket_calculate_numeric ()
 * (src/query/arithmetic.c, server): the cases harness-implement S2 picked the fix with (self-evolution rule #007 --
 * the issue's sketches, multiply first and refuse a zero width, measured as mutants beside the adopted one).
 *
 *   C  [contract] what develop answers right stays:
 *      C1 buckets at and between the bounds, below and above the range, ascending and descending, BIGINT and
 *         NUMERIC (p, s) operands with INTEGER bounds; the operand on the lower bound of a range of 2e-252
 *      C2 20,000 random BIGINT operands and 20,000 random NUMERIC (12, 2) operands against exact integer arithmetic
 *         (bounds within +-10^6, counts 1..1000; a third of the operands outside the range, both directions)
 *      C3 a range near the largest NUMERIC, (1e250, 0, 2e250, 1000000) = 500001
 *      C4 an operand so close to the start of a huge range that its fraction of the range underflows to 0: bucket 1
 *   F  [contract] what the fix changes (develop FAILs them -- see requires_note in suite.json):
 *      F1 a range so small that the width of a bucket, (hi - lo) / count, falls below the smallest NUMERIC (scale 252)
 *         and rounds to 0: develop divides by it -- SIGFPE in float_numeric_div_fast (), so these run in a child
 *      F2 a range near the smallest NUMERIC where the width keeps a digit or two: develop answers from the rounded
 *         width, 3 for 4 and 201 for 334
 *      F3 a descending range with the operand below it gets count + 1 = 10, as the DOUBLE path and the definition
 *         give. develop (and release/11.4, since 2014) compared the two bounds where it meant the operand and the
 *         lower bound, so it went on to compute -- 14 and 55 (the second commit of the CBRD-28024 PR)
 *
 * Real code compiled into the binary: arithmetic.c (unit under test), numeric_opfunc.c, db_macro.c,
 * object_primitive.c, object_domain.c + object_domain_convert.cpp. Fakes as arithmetic.place_edges (suite.json
 * "fakes"). NUMERIC operands are made the way CAST (<double> AS NUMERIC) makes them: numeric_db_value_coerce_to_num
 * () into the default precision, which keeps the double's own digits and scale (1e-252 = 1 at scale 252).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "area_alloc.h"
#include "object_domain.h"
#include "numeric_opfunc.h"
#include "arithmetic.h"

#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/* CAST (d AS NUMERIC (p, s)); p = DB_DEFAULT_NUMERIC_PRECISION is the floating NUMERIC of CAST (d AS NUMERIC) */
static DB_VALUE
v_num (double d, int p = DB_DEFAULT_NUMERIC_PRECISION, int s = DB_DEFAULT_NUMERIC_SCALE)
{
  DB_VALUE src, dst;
  DB_DATA_STATUS st = DATA_STATUS_OK;
  db_make_double (&src, d);
  db_value_domain_init (&dst, DB_TYPE_NUMERIC, p, s);
  REQUIRE (numeric_db_value_coerce_to_num (&src, &dst, &st) == NO_ERROR);
  REQUIRE (st == DATA_STATUS_OK);
  return dst;
}

static std::string
err_name (int code)
{
  switch (code)
    {
    case ER_IT_DATA_OVERFLOW:
      return "ER_IT_DATA_OVERFLOW";
    case ER_QPROC_ZERO_DIVIDE:
      return "ER_QPROC_ZERO_DIVIDE";
    case ER_QPROC_OVERFLOW_ADDITION:
      return "ER_QPROC_OVERFLOW_ADDITION";
    case ER_QPROC_OVERFLOW_DIVISION:
      return "ER_QPROC_OVERFLOW_DIVISION";
    case ER_QPROC_OVERFLOW_SUBTRACTION:
      return "ER_QPROC_OVERFLOW_SUBTRACTION";
    default:
      return std::to_string (code);
    }
}

/* WIDTH_BUCKET (a, lo, hi, n) as fetch calls it: "integer <bucket>", "NULL" or "error <code>" */
static std::string
wb (DB_VALUE a, DB_VALUE lo, DB_VALUE hi, double n)
{
  DB_VALUE r, cnt;
  db_make_null (&r);
  db_make_double (&cnt, n);
  g_errid = NO_ERROR;
  g_err_arg.clear ();
  int live = g_area_live;
  int rc = db_width_bucket (&r, &a, &lo, &hi, &cnt);
  std::string s;
  if (rc != NO_ERROR)
    {
      s = "error " + err_name (rc);
    }
  else if (DB_IS_NULL (&r))
    {
      s = "NULL";
    }
  else if (DB_VALUE_DOMAIN_TYPE (&r) == DB_TYPE_INTEGER)
    {
      s = "integer " + std::to_string (db_get_int (&r));
    }
  else
    {
      s = "type " + std::to_string ((int) DB_VALUE_DOMAIN_TYPE (&r));
    }
  if (g_area_live != live)
    {
      s += " +domains left " + std::to_string (g_area_live - live);
    }
  return s;
}

/* the same in a child: "signal <n>" when it dies */
static std::string
wb_in_child (DB_VALUE a, DB_VALUE lo, DB_VALUE hi, double n)
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
      std::string s = wb (a, lo, hi, n);
      ssize_t w = write (fd[1], s.c_str (), s.size ());
      (void) w;
      close (fd[1]);
      _exit (0);
    }
  close (fd[1]);
  std::string out;
  char buf[256];
  ssize_t k;
  while ((k = read (fd[0], buf, sizeof (buf))) > 0)
    {
      out.append (buf, (size_t) k);
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

/* the bucket by definition, exact: lo < hi counts up from lo, lo > hi counts down from lo (all values scaled to
 * integers by the caller) */
static long long
bucket_exact (__int128 a, __int128 lo, __int128 hi, long long n)
{
  if (lo < hi)
    {
      if (a < lo)
	{
	  return 0;
	}
      if (a >= hi)
	{
	  return n + 1;
	}
      return (long long) ((a - lo) * n / (hi - lo)) + 1;
    }
  if (a > lo)
    {
      return 0;
    }
  if (a <= hi)
    {
      return n + 1;
    }
  return (long long) ((lo - a) * n / (lo - hi)) + 1;
}

/* a fixed-seed generator, so every revision sees the same grid */
static unsigned long long g_seed = 0x2802400000000001ULL;

static long long
rnd (long long lo, long long hi)
{
  g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
  return lo + (long long) ((g_seed >> 17) % (unsigned long long) (hi - lo + 1));
}

static std::string
ints (std::initializer_list<long long> v)
{
  std::string s;
  for (long long x : v)
    {
      s += (s.empty ()? "" : ", ") + std::to_string (x);
    }
  return s;
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("CBRD-28024 C1: buckets develop answers right - bounds, between, outside, ascending and descending",
	   "[contract]")
{
  /* ascending: on a bound the operand goes up into the bucket that bound opens */
  CHECK (wb (v_bigint (2), v_int (0), v_int (6), 9) == "integer 4");
  CHECK (wb (v_bigint (4), v_int (0), v_int (6), 9) == "integer 7");
  CHECK (wb (v_bigint (5), v_int (0), v_int (15), 9) == "integer 4");
  CHECK (wb (v_bigint (1), v_int (0), v_int (10), 30) == "integer 4");
  CHECK (wb (v_bigint (2), v_int (0), v_int (3), 9) == "integer 7");
  CHECK (wb (v_bigint (3), v_int (0), v_int (10), 5) == "integer 2");
  CHECK (wb (v_bigint (0), v_int (0), v_int (10), 5) == "integer 1");
  CHECK (wb (v_bigint (9), v_int (0), v_int (10), 5) == "integer 5");
  CHECK (wb (v_bigint (-1), v_int (0), v_int (10), 5) == "integer 0");
  CHECK (wb (v_bigint (10), v_int (0), v_int (10), 5) == "integer 6");
  CHECK (wb (v_bigint (13), v_int (0), v_int (6), 9) == "integer 10");
  /* descending: counts down from the first bound */
  CHECK (wb (v_bigint (6), v_int (6), v_int (0), 9) == "integer 1");
  CHECK (wb (v_bigint (4), v_int (6), v_int (0), 9) == "integer 4");
  CHECK (wb (v_bigint (0), v_int (6), v_int (0), 9) == "integer 10");
  CHECK (wb (v_bigint (7), v_int (6), v_int (0), 9) == "integer 0");
  /* NUMERIC (p, s) operand */
  CHECK (wb (v_num (2, 10, 2), v_int (0), v_int (6), 9) == "integer 4");
  CHECK (wb (v_num (4.5, 10, 2), v_int (0), v_int (6), 9) == "integer 7");
  /* the operand on the lower bound of a range of 2e-252: 0 over the width, develop never divides by it */
  CHECK (wb (v_num (-1e-252), v_num (-1e-252), v_num (1e-252), 5) == "integer 1");
  CHECK (wb (v_num (1e-252), v_num (1e-252), v_num (-1e-252), 5) == "integer 1");
}

/* 40,000 random draws, 20,000 BIGINT and 20,000 NUMERIC (12, 2) operands, against exact integer arithmetic: with
 * desc_below the draws of a descending range with the operand at or below its lower bound (F3), else all the others */
static std::string
random_grid (bool desc_below, int *n_run_out, int *n_bad_out)
{
  std::string bad;
  int n_bad = 0, n_run = 0;
  for (int i = 0; i < 40000; i++)
    {
      bool num = i >= 20000;
      long long lo = rnd (-1000000, 1000000), hi = rnd (-1000000, 1000000), n = rnd (1, 1000);
      if (lo == hi)
	{
	  continue;
	}
      /* the operand in hundredths for NUMERIC (12, 2), whole for BIGINT; a third of them outside the range */
      long long span = (hi > lo ? hi - lo : lo - hi);
      long long base = (lo < hi ? lo : hi);
      long long a = rnd (base - span / 2, base + span + span / 2);
      long long a100 = num ? a * 100 + rnd (-99, 99) : a * 100;
      if ((lo > hi && a100 <= hi * 100) != desc_below)
	{
	  continue;
	}
      long long expect = bucket_exact (a100, (__int128) lo * 100, (__int128) hi * 100, n);
      std::string got = num ? wb (v_num ((double) a100 / 100.0, 12, 2), v_int ((int) lo), v_int ((int) hi), (double) n)
	: wb (v_bigint (a), v_int ((int) lo), v_int ((int) hi), (double) n);
      n_run++;
      if (got != "integer " + std::to_string (expect))
	{
	  if (n_bad++ < 8)
	    {
	      bad += (num ? "numeric " : "bigint ") + ints ({a100, lo, hi, n}) + ": " + got + " (exact "
		+ std::to_string (expect) + ")\n";
	    }
	}
    }
  *n_run_out = n_run;
  *n_bad_out = n_bad;
  return bad;
}

TEST_CASE ("CBRD-28024 C2: a random grid of BIGINT and NUMERIC (12, 2) operands matches exact arithmetic", "[contract]")
{
  int n_run, n_bad;
  std::string bad = random_grid (false, &n_run, &n_bad);
  INFO ("ran " << n_run << ", wrong " << n_bad);
  CHECK (bad == "");
  CHECK (n_run > 30000);
}

TEST_CASE ("CBRD-28024 C3: a range near the largest NUMERIC keeps its bucket", "[contract]")
{
  CHECK (wb (v_num (1e250), v_int (0), v_num (2e250), 1000000) == "integer 500001");
  CHECK (wb (v_num (1e250), v_num (2e250), v_int (0), 1000000) == "integer 500001");
}

TEST_CASE ("CBRD-28024 C4: a fraction of the range that underflows to 0 is bucket 1", "[contract]")
{
  /* 1e-250 into a range of 1e250: the fraction, 1e-500, is below the smallest NUMERIC; floor (1e-500 * 5) + 1 = 1 */
  CHECK (wb (v_num (1e-250), v_int (0), v_num (1e250), 5) == "integer 1");
  CHECK (wb (v_bigint (0), v_num (1e-250), v_num (-1e250), 5) == "integer 1");
}

TEST_CASE ("CBRD-28024 F1: a bucket narrower than the smallest NUMERIC - develop divides by 0 (SIGFPE)", "[contract]")
{
  /* the CAST makes 1 at scale 252, not 0 */
  DB_VALUE tiny = v_num (1e-252);
  REQUIRE (!numeric_db_value_is_zero (&tiny));
  CHECK (wb_in_child (v_bigint (0), v_num (-1e-252), v_num (1e-252), 5) == "integer 3");
  CHECK (wb_in_child (v_bigint (0), v_num (1e-252), v_num (-1e-252), 5) == "integer 3");
  CHECK (wb_in_child (v_bigint (0), v_num (-1e-244), v_num (1e-244), 2147483646) == "integer 1073741824");
  /* an INTEGER lower bound becomes NUMERIC (38, 7) in db_width_bucket (): develop answered 3 in one run and died of
   * SIGFPE in the next -- the division by the zero width reads what is left on the stack */
  CHECK (wb_in_child (v_num (1e-252), v_int (0), v_num (2e-252), 5) == "integer 3");
  CHECK (wb_in_child (v_num (1e-252), v_num (0), v_num (2e-252), 5) == "integer 3");
  CHECK (wb_in_child (v_num (1e-252), v_num (2e-252), v_num (0), 5) == "integer 3");
}

TEST_CASE ("CBRD-28024 F2: a bucket width that loses digits below the smallest NUMERIC - develop 3 and 201",
	   "[contract]")
{
  CHECK (wb_in_child (v_num (2e-252), v_int (0), v_num (3e-252), 5) == "integer 4");
  CHECK (wb_in_child (v_num (2e-250), v_int (0), v_num (3e-250), 500) == "integer 334");
}

TEST_CASE ("CBRD-28024 F3: descending with the operand below the range is count + 1 - develop 14 and 55", "[contract]")
{
  CHECK (wb (v_bigint (-3), v_int (6), v_int (0), 9) == "integer 10");
  CHECK (wb (v_bigint (-30), v_int (6), v_int (0), 9) == "integer 10");
  CHECK (wb (v_num (-3, 5, 1), v_int (6), v_int (0), 9) == "integer 10");
  /* far below: develop's formula overflowed -- ER_QPROC_OVERFLOW_ADDITION (self-review of PR#8478) */
  CHECK (wb (v_num (-1e20), v_int (5), v_int (0), 10) == "integer 11");
  CHECK (wb (v_bigint (-9000000000000000000LL), v_int (5), v_int (0), 2147483646) == "integer 2147483647");
  /* on the lower bound, and just above it: develop answers these right */
  CHECK (wb (v_num (0, 5, 1), v_int (6), v_int (0), 9) == "integer 10");
  CHECK (wb (v_num (0.1, 5, 1), v_int (6), v_int (0), 9) == "integer 9");
  /* the C2 grid's draws below a descending range */
  int n_run, n_bad;
  std::string bad = random_grid (true, &n_run, &n_bad);
  INFO ("ran " << n_run << ", wrong " << n_bad);
  CHECK (bad == "");
  CHECK (n_run > 3000);
}
