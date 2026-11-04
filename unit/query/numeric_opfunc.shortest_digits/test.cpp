/*
 * PR#8349 (CBRD-27819) -- numeric_shortest_digits () (src/query/numeric_opfunc.c, server), the digits DOUBLE and FLOAT
 * are written with on a cast to a string (tp_dtoa_buffer () / tp_ftoa_buffer (), object_domain_convert.cpp).
 *
 * The PR replaces develop's _dtoa (x, 0, ndigits, &decpt, &sign, &rve, buf, float_type) followed by
 * buf[ndigits] = '\0' (TP_DOUBLE_MANTISA_DECIMAL_PRECISION 16 for DOUBLE with float_type 0,
 * TP_FLOAT_MANTISA_DECIMAL_PRECISION 7 for FLOAT with float_type 1) by numeric_shortest_digits (x, is_float, ndigits,
 * buf, &decpt, &sign); format_floating_point () is unchanged. The contract: the same digits, decpt and sign for every
 * value - the zeros, the infinities and NaNs ("Infinity" cut to 7 for FLOAT), subnormals, the powers of two (the
 * narrower interval below them) and of ten with their neighbours, integers near 2^53 and FLOAT integers below and past
 * 10^15 (develop writes those as they are), random bit patterns and random decimals.
 *
 * Both run side by side in one binary: _dtoa () is still in the tree (src/base/dtoa.c, used by util_common.c).
 * Real code: numeric_opfunc.c (unit under test), dtoa.c, mprec.c. Faked: er_set / er_errid / er_clear, prm_Def (zero).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "mprec.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

static int g_errid = NO_ERROR;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  g_errid = err_id;
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
}

SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };

struct cmp_stats
{
  long n = 0, bad = 0;
  std::string first;
};

/* develop tp_dtoa_buffer () / tp_ftoa_buffer (): _dtoa (x, 0, ndigits, ..., float_type) then buf[ndigits] = '\0' */
static void
develop_digits (double x, bool is_float, int ndigits, char *buf, int *decpt, int *sign)
{
  char *rve = buf;
  _dtoa (x, 0, ndigits, decpt, sign, &rve, buf, is_float ? 1 : 0);
  buf[ndigits] = '\0';
}

static void
check (double x, bool is_float, cmp_stats & st)
{
  const int nd = is_float ? 7 : 16;
  char a[64], b[64];
  int da = 0, sa = 0, db = 0, sb = 0;
  std::memset (a, 0, sizeof (a));
  std::memset (b, 0, sizeof (b));
  develop_digits (x, is_float, nd, a, &da, &sa);
  numeric_shortest_digits (x, is_float, nd, b, &db, &sb);
  st.n++;
  if (std::strcmp (a, b) != 0 || da != db || sa != sb)
    {
      if (st.bad++ == 0)
	{
	  char t[256];
	  uint64_t bits;
	  std::memcpy (&bits, &x, 8);
	  std::snprintf (t, sizeof (t), "x=%.17g bits=%016llx float=%d develop='%s' %d %d head='%s' %d %d", x,
			 (unsigned long long) bits, (int) is_float, a, da, sa, b, db, sb);
	  st.first = t;
	}
    }
}

static double
from_bits (uint64_t u)
{
  double x;
  std::memcpy (&x, &u, 8);
  return x;
}

static float
f_from_bits (uint32_t u)
{
  float x;
  std::memcpy (&x, &u, 4);
  return x;
}

TEST_CASE ("S1 DOUBLE: special values, powers of two and ten with neighbours, integers near 2^53 give develop's _dtoa () digits, decpt and sign", "[contract]")
{
  cmp_stats st;
  double specials[] = { 0.0, -0.0, INFINITY, -INFINITY, NAN, -NAN, DBL_MAX, -DBL_MAX, DBL_MIN, -DBL_MIN,
    4.9406564584124654e-324, -4.9406564584124654e-324, 0.1, 0.2, 0.1 + 0.2, 1.0 / 3, 2.0 / 3, 1e15, 1e16, 1e17,
    9007199254740993.0, 123456789012345678.0, 1e-5, 1e-4, 1e-3, 5e-324, 2.2250738585072009e-308,
    1.7976931348623157e308, 0.30000000000000004, 9.999999999999999e22, 1e23, 8.41e21, 5e-310
  };
  for (double x:specials)
    {
      check (x, false, st);
    }
  check (from_bits (0x7ff0000000000001ULL), false, st);	/* signalling NaN */
  check (from_bits (0xfff8000000000001ULL), false, st);
  for (int e = 0; e < 2047; e++)
    {
      for (int k = -3; k <= 3; k++)
	{
	  uint64_t u = ((uint64_t) e << 52) + (uint64_t) (int64_t) k;
	  if (e == 0 && k < 0)
	    {
	      continue;
	    }
	  check (from_bits (u), false, st);
	  check (-from_bits (u), false, st);
	}
    }
  for (int p = -323; p <= 308; p++)
    {
      double x = std::pow (10.0, p);
      if (!std::isfinite (x) || x == 0)
	{
	  continue;
	}
      uint64_t u;
      std::memcpy (&u, &x, 8);
      for (int k = -3; k <= 3; k++)
	{
	  check (from_bits (u + (uint64_t) (int64_t) k), false, st);
	}
    }
  for (int64_t k = -2000; k <= 2000; k++)
    {
      check ((double) ((1LL << 53) + k), false, st);
      check ((double) ((1LL << 54) + 2 * k), false, st);
      check ((double) (100000000000000000LL + 16 * k), false, st);
    }
  WARN ("double specials: " << st.n << " compared, " << st.bad << " differ " << st.first);
  CHECK (st.bad == 0);
}

TEST_CASE ("S2 DOUBLE: random bit patterns and random decimals give develop's _dtoa () digits, decpt and sign", "[contract]")
{
  cmp_stats st;
  std::mt19937_64 rng (8349);
  for (int i = 0; i < 500000; i++)
    {
      check (from_bits (rng ()), false, st);
    }
  std::uniform_int_distribution < int64_t > num (1, 999999999999999999LL);
  std::uniform_int_distribution < int >ex (-30, 30);
  for (int i = 0; i < 200000; i++)
    {
      double x = (double) num (rng) * std::pow (10.0, ex (rng));
      check (x, false, st);
      check ((double) (i + 1) / 7.0, false, st);
    }
  WARN ("double random: " << st.n << " compared, " << st.bad << " differ " << st.first);
  CHECK (st.bad == 0);
}

TEST_CASE ("S3 FLOAT (float_type 1): specials, powers of two with neighbours, subnormals, integers below and past 10^15, random give develop's _dtoa () digits, decpt and sign", "[contract]")
{
  cmp_stats st;
  float specials[] = { 0.0f, -0.0f, INFINITY, -INFINITY, NAN, FLT_MAX, -FLT_MAX, FLT_MIN, -FLT_MIN, 1.401298464e-45f,
    0.1f, 0.2f, 1.0f / 3, 16777216.0f, 16777218.0f, 1e15f, 9.99999986e14f, 1e14f, 123456789.0f, 3.4e38f, 1e-38f
  };
  for (float x:specials)
    {
      check ((double) x, true, st);
    }
  for (uint32_t e = 0; e < 255; e++)
    {
      for (int k = -3; k <= 3; k++)
	{
	  if (e == 0 && k < 0)
	    {
	      continue;
	    }
	  uint32_t u = (e << 23) + (uint32_t) k;
	  check ((double) f_from_bits (u), true, st);
	  check ((double) -f_from_bits (u), true, st);
	}
    }
  for (uint32_t u = 1; u < (1u << 23); u += 61)
    {
      check ((double) f_from_bits (u), true, st);	/* subnormal floats */
    }
  for (int64_t k = 1; k < 50000; k++)
    {
      check ((double) (float) (k * 4999999999LL), true, st);	/* float integers up to 10^15 and past */
    }
  std::mt19937 rng (27819);
  for (int i = 0; i < 500000; i++)
    {
      uint32_t u = rng ();
      if (((u >> 23) & 0xff) == 0xff)
	{
	  continue;
	}
      check ((double) f_from_bits (u), true, st);
    }
  WARN ("float: " << st.n << " compared, " << st.bad << " differ " << st.first);
  CHECK (st.bad == 0);
}
