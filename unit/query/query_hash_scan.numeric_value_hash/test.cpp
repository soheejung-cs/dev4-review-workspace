/*
 * PR#8414 (CBRD-27798) -- qdata_hash_scan_value () of NUMERIC keys (src/query/query_hash_scan.c, server).
 *
 * qdata_hash_scan_value () hashes one key value for the hash list scan (scan_manager.c, build and probe through
 * qdata_hash_scan_key ()), the hash join and its precomputed hashes (query_hash_join.c, px_hash_join), and the hash
 * GROUP BY and its key sketch (query_aggregate.cpp qdata_hash_agg_hkey ()). The hash list scan looks a probe key up by
 * its hash alone and applies the predicate to that bucket's tuples only, so a pair the predicate finds equal must hash
 * alike or the row is lost without an error. The PR hashes a NUMERIC by its value: its coefficient times 10^-scale
 * modulo the prime 2^61 - 1 and its sign (qdata_hash_scan_numeric ()), then mht_get_hash_number ()'s mixing and
 * reduction; pt_hash_key_domain () then hashes two NUMERIC keys as they are, whatever their scales.
 *
 *   N1 [contract] NUMERIC values equal under the comparison (numeric_db_value_compare ()) hash alike, for every table
 *      size: fixed (p, s) at every scale that holds the value - negative scales and scales past p as the grammar allows
 *      (-84 .. 127) - floating with trailing zeros, floating negative scales down to -214 and scales up to 252 (the
 *      loop of qdata_hash_pow10_mod_prime () past 40), coefficients of 2^64 and more (the high word) and of 2^128 and
 *      more (byte 0: a floating value of 39 or 40 digits), both signs, zero at every scale.
 *      develop FAILs: a fixed NUMERIC hashed its digits, so 1.0 and 1.00 hashed apart (the PR's fix)
 *   N2 [contract] different values hash apart: 1.23, 12.3, 123, 1230, 0.123, -123 and -1.23 (one coefficient, other
 *      scales or signs); x and -x; and the spread of structured columns (rule #018): 4,096 values of a NUMERIC(15,2)
 *      money column, 4,096 integers, 4,096 YYYYMMDD dates, 4,096 values one step of 0.0001 apart - at least 4,000
 *      distinct full-range hashes each, and no bucket of a 1,024-bucket table above 24 for the money column
 *   N3 [contract] the bucket follows mht_get_hash_number ()'s reduction: 0 for a table of 0 or 1 bucket, never
 *      UINT_MAX for the full range (UINT_MAX), below the size otherwise and the full-range hash modulo the size
 *   N4 [contract] a hash list scan key: a build key and a probe key whose NUMERIC values are equal at other scales or
 *      representations, next to INTEGER and BIGINT columns, hash alike by qdata_hash_scan_key () for the three hash
 *      methods (IN_MEM, HYBRID, HASH_FILE), and the one-value-at-a-time step (qdata_hash_scan_key_step () and _finish
 *      ()) gives the same hash. develop FAILs (the issue's pairs)
 *   N5 [contract] a value other than a NUMERIC hashes as mht_get_hash_number () does (the PR changed the NUMERIC branch
 *      only), but for a FLOAT, DOUBLE or MONETARY zero, which hashes as +0
 *
 * Real code: query_hash_scan.c (unit under test), memory_hash.c (mht_get_hash_number ()), numeric_opfunc.c
 * (comparison, construction from a digit string), db_macro.c. Faked: er_set / er_errid / er_clear, prm_Def (zero).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "memory_hash.h"
#include "query_hash_scan.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

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
static bool
boot_params ()
{
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].value.v.b = false;
  return true;
}
static const bool params_booted = boot_params ();

/* the full range a hash table of the hash list scan asks for, and other table sizes */
static const unsigned int SIZES[] = { UINT_MAX, 4294967291u, 1000003u, 1024u, 7u, 2u };

/* a NUMERIC made directly: coefficient (decimal digits), precision, scale, sign; fixed when is_fixed */
static DB_VALUE
make (const std::string & digits, int p, int s, bool neg, bool is_fixed)
{
  DB_VALUE f, v;
  db_make_null (&f);
  REQUIRE (numeric_coerce_string_to_num (digits.c_str (), (int) digits.size (), INTL_CODESET_ISO88591, &f) ==
	   NO_ERROR);
  unsigned char buf[DB_NUMERIC_BUF_SIZE];
  std::memcpy (buf, db_locate_numeric (&f), DB_NUMERIC_BUF_SIZE);
  db_make_null (&v);
  REQUIRE (db_make_numeric (&v, buf, p, s, DB_NUMERIC_BUF_SIZE, neg, !is_fixed) == NO_ERROR);
  return v;
}

static bool
num_equal (const DB_VALUE & a, const DB_VALUE & b)
{
  DB_VALUE ans;
  db_make_null (&ans);
  REQUIRE (numeric_db_value_compare (&a, &b, &ans) == NO_ERROR);
  return db_get_int (&ans) == 0;
}

static std::string
desc (const DB_VALUE & v)
{
  char buf[600];
  int p = 0, s = 0;
  bool fl = false;
  db_get_numeric_precision_and_scale (&v, &p, &s, &fl);
  numeric_db_value_print (&v, buf);
  return std::string (buf) + " p" + std::to_string (p) + " s" + std::to_string (s)
    + (DB_VALUE_NUMERIC_IS_VALUE_NEGATIVE (&v) ? " neg" : "") + (fl ? " float" : " fixed");
}

static unsigned int
h (const DB_VALUE & v, unsigned int size = UINT_MAX)
{
  return qdata_hash_scan_value (&v, size);
}

/* every form of k * 10^e this test builds: coefficient k followed by z zeros at scale z - e - floating (precision
 * the digits), fixed (p, s) where the grammar allows the domain (p <= 38, -84 <= s <= 127) and a wider fixed (38, s) */
static void
forms (const std::string & k, int e, bool neg, std::vector < DB_VALUE > &out)
{
  const int kd = (int) k.size ();
  for (int z = 0; kd + z <= DB_MAX_NUMERIC_PRECISION; z++)
    {
      const int s = z - e;
      const int p = kd + z;
      if (s < DB_MIN_NUMERIC_SCALE || s > DB_MAX_NUMERIC_SCALE)
	{
	  continue;
	}
      const std::string c = k + std::string (z, '0');
      out.push_back (make (c, p, s, neg, false));
      if (p <= DB_MAX_FIXED_NUMERIC_PRECISION && s >= DB_MIN_FIXED_NUMERIC_SCALE && s <= DB_MAX_FIXED_NUMERIC_SCALE)
	{
	  out.push_back (make (c, p, s, neg, true));
	  if (p < DB_MAX_FIXED_NUMERIC_PRECISION)
	    {
	      out.push_back (make (c, DB_MAX_FIXED_NUMERIC_PRECISION, s, neg, true));
	    }
	}
    }
}

TEST_CASE ("N1 NUMERIC values equal under the comparison hash alike: fixed at every scale the grammar allows, floating with trailing zeros, negative scales, scales past 40, coefficients past 2^64 and 2^128, both signs, zero", "[contract]")
{
  struct
  {
    const char *k;
    int e;
  } values[] = {
    {"1", 0}, {"1", -1}, {"1", -2}, {"1", 2}, {"15", -1}, {"15", 0}, {"12345", -3}, {"12345", 3}, {"5", 0},
    {"7", -38}, {"9", 37}, {"1", 39}, {"1", 100}, {"123", -200}, {"7", -127}, {"3", 84}, {"11", -214}, {"9", 250},
    {"7", -45}, {"7", 45}, {"4381872152352264", -2}, {"753980177740969", -1}, {"9007199254740993", -1},
    {"18446744073709551617", 0}, {"18446744073709551617", -7},	/* 2^64 + 1: the high word */
    {"340282366920938463463374607431768211457", 0}, {"340282366920938463463374607431768211457", -3},	/* 2^128 + 1 */
    {"9999999999999999999999999999999999999999", -20},	/* 40 digits: byte 0 */
    {"2305843009213693951", 0},	/* the prime itself: residue 0, as zero's */
  };
  long pairs = 0, bad = 0;
  for (auto &x:values)
    {
      for (int sign = 0; sign <= 1; sign++)
	{
	  std::vector < DB_VALUE > f;
	  forms (x.k, x.e, sign != 0, f);
	  for (size_t i = 0; i < f.size (); i++)
	    {
	      for (size_t j = i + 1; j < f.size (); j++)
		{
		  if (!num_equal (f[i], f[j]))
		    {
		      continue;
		    }
		  pairs++;
		  for (unsigned int size:SIZES)
		    {
		      if (h (f[i], size) != h (f[j], size) && bad++ < 10)
			{
			  FAIL_CHECK (desc (f[i]) + " and " + desc (f[j]) + " compare equal but hash apart (table " +
				      std::to_string (size) + ")");
			}
		    }
		}
	    }
	}
    }
  /* zero: every scale, fixed and floating (a negative zero compares apart from zero - only equal pairs count) */
  std::vector < DB_VALUE > z;
  for (int s = -90; s <= 130; s++)
    {
      z.push_back (make ("0", 1, s, false, false));
      z.push_back (make ("0", 1, s, true, false));
      if (s >= DB_MIN_FIXED_NUMERIC_SCALE && s <= DB_MAX_FIXED_NUMERIC_SCALE)
	{
	  z.push_back (make ("0", DB_MAX_FIXED_NUMERIC_PRECISION, s, false, true));
	}
    }
  for (size_t i = 0; i < z.size (); i++)
    {
      for (size_t j = i + 1; j < z.size (); j++)
	{
	  if (num_equal (z[i], z[j]))
	    {
	      pairs++;
	      if (h (z[i]) != h (z[j]) && bad++ < 10)
		{
		  FAIL_CHECK (desc (z[i]) + " and " + desc (z[j]) + " compare equal but hash apart");
		}
	    }
	}
    }
  WARN ("N1: " << pairs << " equal pairs, " << bad << " hash apart");
  CHECK (pairs > 2000);
  CHECK (bad == 0);
}

TEST_CASE ("N2 different values hash apart: one coefficient at other scales or signs, x and -x, and structured columns spread", "[contract]")
{
  /* 1.23, 12.3, 123, 1230 (fixed (4, -1)), 0.123, -123, -1.23 */
  std::vector < DB_VALUE > v = {
    make ("123", 3, 2, false, true), make ("123", 3, 1, false, true), make ("123", 3, 0, false, true),
    make ("123", 3, -1, false, true), make ("123", 3, 3, false, true), make ("123", 3, 0, true, true),
    make ("123", 3, 2, true, true),
  };
  std::set < unsigned int >distinct;
  for (auto &x:v)
    {
      distinct.insert (h (x));
    }
  CHECK (distinct.size () == v.size ());

  /* x and -x, and the spread of a NUMERIC(15,2) money column: i * 79.19 */
  std::set < unsigned int >money, ints, dates, steps;
  std::map < unsigned int, int >bucket;
  int same_sign = 0;
  for (int i = 1; i <= 4096; i++)
    {
      char t[64];
      std::snprintf (t, sizeof (t), "%d", i * 7919);
      DB_VALUE pos = make (t, 15, 2, false, true);
      DB_VALUE neg = make (t, 15, 2, true, true);
      same_sign += (h (pos) == h (neg));
      money.insert (h (pos));
      bucket[h (pos, 1024)]++;

      std::snprintf (t, sizeof (t), "%d", i);
      ints.insert (h (make (t, 10, 0, false, true)));

      /* YYYYMMDD of 4,096 days from 2015-01-01, as NUMERIC(8,0) */
      static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
      int y = 2015, m = 1, d = 1;
      for (int k = 1; k < i; k++)
	{
	  int dim = mdays[m - 1] + ((m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0);
	  if (++d > dim)
	    {
	      d = 1;
	      if (++m > 12)
		{
		  m = 1;
		  y++;
		}
	    }
	}
      std::snprintf (t, sizeof (t), "%04d%02d%02d", y, m, d);
      dates.insert (h (make (t, 8, 0, false, true)));

      /* 1.0000 + i * 0.0001 as NUMERIC(10,4) */
      std::snprintf (t, sizeof (t), "%d", 10000 + i);
      steps.insert (h (make (t, 10, 4, false, true)));
    }
  int max_bucket = 0;
  for (auto &b:bucket)
    {
      max_bucket = std::max (max_bucket, b.second);
    }
  WARN ("N2: money " << money.size () << ", integers " << ints.size () << ", dates " << dates.size () << ", steps " <<
	steps.size () << " distinct of 4096; x and -x alike " << same_sign << "; fullest of 1,024 buckets " <<
	max_bucket);
  CHECK (same_sign == 0);
  CHECK (money.size () >= 4000);
  CHECK (ints.size () >= 4000);
  CHECK (dates.size () >= 4000);
  CHECK (steps.size () >= 4000);
  CHECK (max_bucket <= 24);
}

TEST_CASE ("N3 a NUMERIC's bucket follows mht_get_hash_number ()'s reduction: 0 for 0 or 1 bucket, never UINT_MAX for the full range, the full-range hash modulo the size", "[contract]")
{
  std::vector < DB_VALUE > v;
  for (int i = 0; i < 2000; i++)
    {
      char t[64];
      std::snprintf (t, sizeof (t), "%d", i * 104729 + 7);
      v.push_back (make (t, 20, i % 7, (i % 3) == 0, (i % 2) == 0));
    }
  int bad = 0;
  for (auto &x:v)
    {
      const unsigned int full = h (x, UINT_MAX);
      if (h (x, 0) != 0 || h (x, 1) != 0 || full == UINT_MAX)
	{
	  bad++;
	}
      for (unsigned int size:{2u, 7u, 1024u, 1000003u, 4294967291u})
	{
	  const unsigned int b = h (x, size);
	  if (b >= size || (full != 0 && b != full % size))
	    {
	      bad++;
	    }
	}
    }
  CHECK (bad == 0);
}

/* a key of three values, as qdata_build_hscan_key () points them */
static unsigned int
key_hash (DB_VALUE * a, DB_VALUE * b, DB_VALUE * c, HASH_METHOD method)
{
  DB_VALUE *values[3] = { a, b, c };
  HASH_SCAN_KEY key;
  key.val_count = 3;
  key.free_values = false;
  key.values = values;
  return qdata_hash_scan_key (&key, UINT_MAX, method);
}

TEST_CASE ("N4 a hash list scan key: equal NUMERIC values at other scales or representations next to INTEGER and BIGINT hash alike by qdata_hash_scan_key () for every method, and the step and finish give the same hash", "[contract]")
{
  struct
  {
    DB_VALUE build, probe;
  } pairs[] = {
    /* NUMERIC(18,2) against a floating arithmetic key: 753980177740970.00 and 753980177740970.0 */
    {make ("75398017774097000", 18, 2, false, true), make ("7539801777409700", 16, 1, false, false)},
    /* NUMERIC(38,0) against NUMERIC(38,2) + 0 */
    {make ("4381872152352264", 38, 0, false, true), make ("438187215235226400", 39, 2, false, false)},
    /* NUMERIC(38,2) against NUMERIC(38,6) */
    {make ("900719925474099350", 38, 2, true, true), make ("9007199254740993500000", 38, 6, true, true)},
    /* NUMERIC(10,0) against a floating value of a negative scale: 100 as (1, -2) */
    {make ("100", 10, 0, false, true), make ("1", 1, -2, false, false)},
  };
  DB_VALUE i7, b9;
  db_make_int (&i7, 7);
  db_make_bigint (&b9, 9000000000000000009LL);
  int bad = 0;
  for (auto &p:pairs)
    {
      REQUIRE (num_equal (p.build, p.probe));
      for (HASH_METHOD m:{HASH_METH_IN_MEM, HASH_METH_HYBRID, HASH_METH_HASH_FILE})
	{
	  const unsigned int hb = key_hash (&p.build, &i7, &b9, m);
	  const unsigned int hp = key_hash (&p.probe, &i7, &b9, m);
	  if (hb != hp && bad++ < 10)
	    {
	      FAIL_CHECK (desc (p.build) + " and " + desc (p.probe) + " keys hash apart (method " + std::to_string (m) +
			  ")");
	    }
	  unsigned int step = 0;
	  step = qdata_hash_scan_key_step (step, &p.probe, UINT_MAX);
	  step = qdata_hash_scan_key_step (step, &i7, UINT_MAX);
	  step = qdata_hash_scan_key_step (step, &b9, UINT_MAX);
	  if (qdata_hash_scan_key_finish (step, m) != hp)
	    {
	      bad++;
	    }
	}
    }
  CHECK (bad == 0);
}

TEST_CASE ("N5 a value other than a NUMERIC hashes as mht_get_hash_number () does, but a FLOAT, DOUBLE or MONETARY zero hashes as +0", "[contract]")
{
  std::vector < DB_VALUE > v (12);
  db_make_int (&v[0], -123);
  db_make_short (&v[1], 77);
  db_make_bigint (&v[2], 9007199254740993LL);
  db_make_double (&v[3], 1.5);
  db_make_float (&v[4], 2.5f);
  /* a DATE and a TIME by their encoded fields (db_make_date () encodes through db_date.c, not linked here) */
  db_value_domain_init (&v[5], DB_TYPE_DATE, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
  v[5].data.date = 2461355;
  v[5].domain.general_info.is_null = 0;
  db_value_domain_init (&v[6], DB_TYPE_TIME, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
  v[6].data.time = 66848;
  v[6].domain.general_info.is_null = 0;
  db_make_double (&v[7], 1e300);
  db_make_monetary (&v[8], DB_CURRENCY_WON, 1234.5);
  db_make_int (&v[9], 0);
  db_make_bigint (&v[10], -1);
  db_make_null (&v[11]);
  int bad = 0;
  for (auto &x:v)
    {
      for (unsigned int size:SIZES)
	{
	  if (qdata_hash_scan_value (&x, size) != mht_get_hash_number (size, &x))
	    {
	      bad++;
	    }
	}
    }
  DB_VALUE pz, nz;
  db_make_double (&pz, 0.0);
  db_make_double (&nz, -0.0);
  CHECK (qdata_hash_scan_value (&nz, UINT_MAX) == qdata_hash_scan_value (&pz, UINT_MAX));
  db_make_float (&pz, 0.0f);
  db_make_float (&nz, -0.0f);
  CHECK (qdata_hash_scan_value (&nz, UINT_MAX) == qdata_hash_scan_value (&pz, UINT_MAX));
  CHECK (bad == 0);
}
