/*
 * PR#8349 (CBRD-27819) -- mht_valhash () of NUMERIC and BIT values (src/base/memory_hash.c, server).
 *
 * mht_valhash () buckets the keys of the query result cache (list_file.c qfile_hash_db_value_array: the host
 * variables of a cached XASL, compared by the position's decided comparison, qfile_list_cache_values_equal) and of
 * the workspace's virtual MOP table (work_space.c ws_vmop: tp_value_equal without coercion). Before the PR it hashed a
 * NUMERIC's buffer up to its first zero byte (key_size -1), so every NUMERIC below 2^128 hashed 0; the PR hashes all
 * DB_NUMERIC_BUF_SIZE bytes, and since 3cece75de normalizes a fixed NUMERIC's trailing zeros as it does a floating
 * one's (float_numeric_normalize_for_hash), so 1.0 and 1.00 still meet. BIT/VARBIT now hash db_get_string_size ()
 * bytes instead of reading up to a zero byte.
 *
 *   V1 [contract] NUMERIC values the comparison says are equal hash alike: fixed NUMERIC (p, s) at every scale that
 *      holds the value, floating NUMERICs with trailing zeros, negative scales (100 as (1, -2)), precision 40 with a
 *      negative scale, zero and a negative zero, both signs
 *   V2 [contract] (head) distinct NUMERIC values below 2^128 spread: 4,096 distinct values give at least 4,000
 *      distinct hashes (develop: one, every hash 0 - the PR's stated fix, pr-body:16)
 *   V3 [contract] BIT / VARBIT: equal under the comparison (same bytes, same byte length) hash alike
 *   V4 [contract] (head) a BIT / VARBIT value's hash does not depend on the bytes past its length, also when they are
 *      not zero (develop: key_size -1 read up to a zero byte, past the value - an over-read and a hash that changes
 *      with the memory after the value)
 *
 *   V5 [contract] mht_get_hash_number () of a NUMERIC gives develop's values (hash partitioning places rows by it;
 *      the PR only rewrote the fold as numeric_repr_fold32 ()) - 210 value forms x 4 table sizes taken at 80b84317d
 *
 * Real code: memory_hash.c (unit under test), numeric_opfunc.c (normalization, comparison, construction), db_macro.c.
 * Faked: er_set / er_errid / er_clear, prm_Def (zero).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "memory_hash.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

static const unsigned int HT = 4294967291u;	/* a prime: the hash itself, nearly */

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

/* every form of k * 10^e this test builds: fixed (p, s) with trailing zeros, floating with trailing zeros, negative
 * scales; k has no trailing zero */
static void
forms (const std::string & k, int e, bool neg, std::vector < DB_VALUE > &out)
{
  const int kd = (int) k.size ();
  for (int z = 0; kd + z <= DB_MAX_NUMERIC_PRECISION; z++)
    {
      /* coefficient k * 10^z at scale z - e */
      const int s = z - e;
      const int p = kd + z;
      if (s < DB_MIN_NUMERIC_SCALE || s > DB_MAX_NUMERIC_SCALE)
	{
	  continue;
	}
      const std::string c = k + std::string (z, '0');
      out.push_back (make (c, p, s, neg, false));
      if (s >= 0 && s <= p && p <= DB_MAX_FIXED_NUMERIC_PRECISION)
	{
	  out.push_back (make (c, p, s, neg, true));
	  if (p < DB_MAX_FIXED_NUMERIC_PRECISION)
	    {
	      out.push_back (make (c, DB_MAX_FIXED_NUMERIC_PRECISION, s, neg, true));	/* a wider fixed domain */
	    }
	}
    }
}

TEST_CASE ("V1 NUMERIC values equal under the comparison hash alike (fixed and floating, trailing zeros, negative scales, precision 40, zero)", "[contract]")
{
  struct
  {
    const char *k;
    int e;
  } values[] = {
    {"1", 0}, {"1", -1}, {"1", -2}, {"1", 2}, {"15", -1}, {"15", 0}, {"12345", -3}, {"12345", 3}, {"7", -38},
    {"34028236692093846346337460743176821145", -1}, {"9", 37}, {"1", 39}, {"1", 100}, {"123", -200}, {"5", 0},
  };
  int pairs = 0, bad = 0;
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
		  if (mht_valhash (&f[i], HT) != mht_valhash (&f[j], HT) && bad++ < 10)
		    {
		      FAIL_CHECK (desc (f[i]) + " and " + desc (f[j]) + " compare equal but hash apart");
		    }
		}
	    }
	}
    }
  /* zero: every scale, both signs, fixed and floating */
  std::vector < DB_VALUE > z;
  for (int s = -3; s <= 40; s++)
    {
      z.push_back (make ("0", 1, s, false, false));
      z.push_back (make ("0", 1, s, true, false));
      if (s >= 0 && s <= 38)
	{
	  z.push_back (make ("0", 38, s, false, true));
	}
    }
  for (size_t i = 0; i < z.size (); i++)
    {
      for (size_t j = i + 1; j < z.size (); j++)
	{
	  if (num_equal (z[i], z[j]))
	    {
	      pairs++;
	      if (mht_valhash (&z[i], HT) != mht_valhash (&z[j], HT) && bad++ < 10)
		{
		  FAIL_CHECK (desc (z[i]) + " and " + desc (z[j]) + " compare equal but hash apart");
		}
	    }
	}
    }
  WARN ("V1: " << pairs << " equal pairs, " << bad << " hash apart");
  CHECK (pairs > 1000);
  CHECK (bad == 0);
}

TEST_CASE ("V2 (head) distinct NUMERIC values below 2^128 spread over the hash (develop hashed every one of them 0)", "[contract]")
{
  /* coefficients without a trailing zero: the normalization keeps them as they are. (Values a power of ten apart whose
   * trailing zeros the normalization strips - 79.19 and 791.90 - share a hash by design: the scale is not hashed.) */
  std::set < unsigned int >hashes, folded;
  int n = 0;
  for (int i = 1; n < 4096; i++)
    {
      if (i % 10 == 0)
	{
	  continue;
	}
      char t[64];
      std::snprintf (t, sizeof (t), "%d", i * 7919);
      DB_VALUE v = make (t, (int) std::strlen (t) + 2, 2, false, true);	/* fixed (p, 2): i * 79.19 */
      hashes.insert (mht_valhash (&v, HT));
      n++;
    }
  for (int i = 1; i <= 4096; i++)
    {
      char t[64];
      std::snprintf (t, sizeof (t), "%d", i * 7919);
      DB_VALUE v = make (t, (int) std::strlen (t) + 2, 2, false, true);
      folded.insert (mht_valhash (&v, HT));
    }
  WARN ("V2: 4096 coefficients without a trailing zero give " << hashes.size () << " distinct hashes; i * 79.19 for i = 1..4096 (409 of them a power of ten from another) give " << folded.size ());
  CHECK (hashes.size () >= 4000);
}

TEST_CASE ("V3 BIT and VARBIT: equal values hash alike, the bytes past a value's length do not count", "[contract]")
{
  unsigned char a[8] = { 0x0F, 0xAB, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44 };
  unsigned char b[8] = { 0x0F, 0xAB, 0x00, 0x00, 0x55, 0x66, 0x77, 0x88 };
  DB_VALUE va, vb;
  /* 16 bits (2 bytes): the bytes after them differ */
  db_make_varbit (&va, 64, (DB_CONST_C_BIT) a, 16);
  db_make_varbit (&vb, 64, (DB_CONST_C_BIT) b, 16);
  CHECK (mht_valhash (&va, HT) == mht_valhash (&vb, HT));
  db_make_bit (&va, 16, (DB_CONST_C_BIT) a, 16);
  db_make_bit (&vb, 16, (DB_CONST_C_BIT) b, 16);
  CHECK (mht_valhash (&va, HT) == mht_valhash (&vb, HT));
  /* 32 bits: X'0FAB0000' - the zero bytes are the value's own, so a value of other zero bytes is another value */
  unsigned char c[8] = { 0x0F, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  DB_VALUE vc, vd;
  db_make_varbit (&vc, 64, (DB_CONST_C_BIT) c, 32);
  db_make_varbit (&vd, 64, (DB_CONST_C_BIT) a, 32);
  CHECK (mht_valhash (&vc, HT) == mht_valhash (&vd, HT));	/* same 4 bytes */
}

TEST_CASE ("V4 (head) a BIT / VARBIT value's hash reads its own bytes only, also when the bytes after it are not zero (develop read up to a zero byte past the value)", "[contract]")
{
  unsigned char a[8] = { 0x0F, 0xAB, 0x11, 0x22, 0x33, 0x00, 0x00, 0x00 };
  unsigned char b[8] = { 0x0F, 0xAB, 0x55, 0x66, 0x77, 0x00, 0x00, 0x00 };
  DB_VALUE va, vb;
  db_make_varbit (&va, 64, (DB_CONST_C_BIT) a, 16);
  db_make_varbit (&vb, 64, (DB_CONST_C_BIT) b, 16);
  CHECK (mht_valhash (&va, HT) == mht_valhash (&vb, HT));
  db_make_bit (&va, 16, (DB_CONST_C_BIT) a, 16);
  db_make_bit (&vb, 16, (DB_CONST_C_BIT) b, 16);
  CHECK (mht_valhash (&va, HT) == mht_valhash (&vb, HT));
}

/* mht_get_hash_number () at develop 80b84317d (a hash partition's row placement reads it; the PR only folds the same
 * five words with numeric_repr_fold32 ()) - the digits, negative, scale, fixed (precision 38) or floating (its digits)
 * and the hash for table sizes 2, 7, 1024, 1000003 */
struct hash_row
{
  const char *digits;
  int neg, s, fixed;
  unsigned int h[4];
};
static const hash_row hash_rows[] = {
  {"0", 0, -2, 0, {0u, 4u, 362u, 310154u}},
  {"0", 0, -1, 0, {0u, 4u, 362u, 310154u}},
  {"0", 0, 0, 0, {0u, 4u, 362u, 310154u}},
  {"0", 0, 0, 1, {0u, 4u, 362u, 310154u}},
  {"0", 0, 1, 0, {0u, 4u, 362u, 310154u}},
  {"0", 0, 1, 1, {0u, 4u, 362u, 310154u}},
  {"0", 0, 2, 0, {0u, 4u, 362u, 310154u}},
  {"0", 0, 3, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, -2, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, -1, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, 0, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, 0, 1, {0u, 4u, 362u, 310154u}},
  {"0", 1, 1, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, 1, 1, {0u, 4u, 362u, 310154u}},
  {"0", 1, 2, 0, {0u, 4u, 362u, 310154u}},
  {"0", 1, 3, 0, {0u, 4u, 362u, 310154u}},
  {"1", 0, -2, 0, {0u, 4u, 74u, 687380u}},
  {"1", 0, -1, 0, {0u, 0u, 740u, 326287u}},
  {"1", 0, 0, 0, {1u, 6u, 157u, 889112u}},
  {"1", 0, 0, 1, {1u, 6u, 157u, 889112u}},
  {"1", 0, 1, 0, {1u, 6u, 157u, 889112u}},
  {"1", 0, 1, 1, {1u, 6u, 157u, 889112u}},
  {"1", 0, 2, 0, {1u, 6u, 157u, 889112u}},
  {"1", 0, 3, 0, {1u, 6u, 157u, 889112u}},
  {"1", 1, -2, 0, {0u, 4u, 74u, 687380u}},
  {"1", 1, -1, 0, {0u, 0u, 740u, 326287u}},
  {"1", 1, 0, 0, {1u, 6u, 157u, 889112u}},
  {"1", 1, 0, 1, {1u, 6u, 157u, 889112u}},
  {"1", 1, 1, 0, {1u, 6u, 157u, 889112u}},
  {"1", 1, 1, 1, {1u, 6u, 157u, 889112u}},
  {"1", 1, 2, 0, {1u, 6u, 157u, 889112u}},
  {"1", 1, 3, 0, {1u, 6u, 157u, 889112u}},
  {"5", 0, -2, 0, {1u, 5u, 425u, 653095u}},
  {"5", 0, -1, 0, {1u, 2u, 509u, 633197u}},
  {"5", 0, 0, 0, {1u, 2u, 653u, 595231u}},
  {"5", 0, 0, 1, {1u, 2u, 653u, 595231u}},
  {"5", 0, 1, 0, {1u, 2u, 653u, 595231u}},
  {"5", 0, 1, 1, {1u, 2u, 653u, 595231u}},
  {"5", 0, 2, 0, {1u, 2u, 653u, 595231u}},
  {"5", 0, 3, 0, {1u, 2u, 653u, 595231u}},
  {"5", 1, -2, 0, {1u, 5u, 425u, 653095u}},
  {"5", 1, -1, 0, {1u, 2u, 509u, 633197u}},
  {"5", 1, 0, 0, {1u, 2u, 653u, 595231u}},
  {"5", 1, 0, 1, {1u, 2u, 653u, 595231u}},
  {"5", 1, 1, 0, {1u, 2u, 653u, 595231u}},
  {"5", 1, 1, 1, {1u, 2u, 653u, 595231u}},
  {"5", 1, 2, 0, {1u, 2u, 653u, 595231u}},
  {"5", 1, 3, 0, {1u, 2u, 653u, 595231u}},
  {"15", 0, -2, 0, {1u, 4u, 675u, 740828u}},
  {"15", 0, -1, 0, {1u, 4u, 467u, 55039u}},
  {"15", 0, 0, 0, {0u, 6u, 280u, 243599u}},
  {"15", 0, 0, 1, {0u, 6u, 280u, 243599u}},
  {"15", 0, 1, 0, {0u, 6u, 280u, 243599u}},
  {"15", 0, 1, 1, {0u, 6u, 280u, 243599u}},
  {"15", 0, 2, 0, {0u, 6u, 280u, 243599u}},
  {"15", 0, 2, 1, {0u, 6u, 280u, 243599u}},
  {"15", 0, 3, 0, {0u, 6u, 280u, 243599u}},
  {"15", 1, -2, 0, {1u, 4u, 675u, 740828u}},
  {"15", 1, -1, 0, {1u, 4u, 467u, 55039u}},
  {"15", 1, 0, 0, {0u, 6u, 280u, 243599u}},
  {"15", 1, 0, 1, {0u, 6u, 280u, 243599u}},
  {"15", 1, 1, 0, {0u, 6u, 280u, 243599u}},
  {"15", 1, 1, 1, {0u, 6u, 280u, 243599u}},
  {"15", 1, 2, 0, {0u, 6u, 280u, 243599u}},
  {"15", 1, 2, 1, {0u, 6u, 280u, 243599u}},
  {"15", 1, 3, 0, {0u, 6u, 280u, 243599u}},
  {"150", 0, -2, 0, {1u, 2u, 77u, 156474u}},
  {"150", 0, -1, 0, {1u, 4u, 675u, 740828u}},
  {"150", 0, 0, 0, {1u, 4u, 467u, 55039u}},
  {"150", 0, 0, 1, {1u, 4u, 467u, 55039u}},
  {"150", 0, 1, 0, {0u, 6u, 280u, 243599u}},
  {"150", 0, 1, 1, {1u, 4u, 467u, 55039u}},
  {"150", 0, 2, 0, {0u, 6u, 280u, 243599u}},
  {"150", 0, 2, 1, {1u, 4u, 467u, 55039u}},
  {"150", 0, 3, 0, {0u, 6u, 280u, 243599u}},
  {"150", 0, 3, 1, {1u, 4u, 467u, 55039u}},
  {"150", 1, -2, 0, {1u, 2u, 77u, 156474u}},
  {"150", 1, -1, 0, {1u, 4u, 675u, 740828u}},
  {"150", 1, 0, 0, {1u, 4u, 467u, 55039u}},
  {"150", 1, 0, 1, {1u, 4u, 467u, 55039u}},
  {"150", 1, 1, 0, {0u, 6u, 280u, 243599u}},
  {"150", 1, 1, 1, {1u, 4u, 467u, 55039u}},
  {"150", 1, 2, 0, {0u, 6u, 280u, 243599u}},
  {"150", 1, 2, 1, {1u, 4u, 467u, 55039u}},
  {"150", 1, 3, 0, {0u, 6u, 280u, 243599u}},
  {"150", 1, 3, 1, {1u, 4u, 467u, 55039u}},
  {"12345", 0, -2, 0, {1u, 6u, 867u, 332269u}},
  {"12345", 0, -1, 0, {0u, 2u, 334u, 334749u}},
  {"12345", 0, 0, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 0, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 1, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 1, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 2, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 2, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 3, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 0, 3, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 1, -2, 0, {1u, 6u, 867u, 332269u}},
  {"12345", 1, -1, 0, {0u, 2u, 334u, 334749u}},
  {"12345", 1, 0, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 0, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 1, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 1, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 2, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 2, 1, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 3, 0, {1u, 3u, 531u, 37377u}},
  {"12345", 1, 3, 1, {1u, 3u, 531u, 37377u}},
  {"1234567890123456789", 0, -2, 0, {1u, 4u, 825u, 31564u}},
  {"1234567890123456789", 0, -1, 0, {0u, 0u, 728u, 763310u}},
  {"1234567890123456789", 0, 0, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 0, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 1, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 1, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 2, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 2, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 3, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 0, 3, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, -2, 0, {1u, 4u, 825u, 31564u}},
  {"1234567890123456789", 1, -1, 0, {0u, 0u, 728u, 763310u}},
  {"1234567890123456789", 1, 0, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 0, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 1, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 1, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 2, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 2, 1, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 3, 0, {1u, 4u, 1013u, 778221u}},
  {"1234567890123456789", 1, 3, 1, {1u, 4u, 1013u, 778221u}},
  {"12345678901234567890123", 0, -2, 0, {0u, 1u, 860u, 479086u}},
  {"12345678901234567890123", 0, -1, 0, {0u, 0u, 178u, 670706u}},
  {"12345678901234567890123", 0, 0, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 0, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 1, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 1, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 2, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 2, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 3, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 0, 3, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, -2, 0, {0u, 1u, 860u, 479086u}},
  {"12345678901234567890123", 1, -1, 0, {0u, 0u, 178u, 670706u}},
  {"12345678901234567890123", 1, 0, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 0, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 1, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 1, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 2, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 2, 1, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 3, 0, {1u, 5u, 657u, 295847u}},
  {"12345678901234567890123", 1, 3, 1, {1u, 5u, 657u, 295847u}},
  {"34028236692093846346337460743176821145", 0, -2, 0, {1u, 2u, 509u, 633197u}},
  {"34028236692093846346337460743176821145", 0, -1, 0, {1u, 2u, 653u, 595231u}},
  {"34028236692093846346337460743176821145", 0, 0, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 0, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 1, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 1, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 2, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 2, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 3, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 0, 3, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, -2, 0, {1u, 2u, 509u, 633197u}},
  {"34028236692093846346337460743176821145", 1, -1, 0, {1u, 2u, 653u, 595231u}},
  {"34028236692093846346337460743176821145", 1, 0, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 0, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 1, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 1, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 2, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 2, 1, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 3, 0, {0u, 0u, 532u, 574678u}},
  {"34028236692093846346337460743176821145", 1, 3, 1, {0u, 0u, 532u, 574678u}},
  {"99999999999999999999999999999999999999", 0, -2, 0, {1u, 3u, 901u, 552102u}},
  {"99999999999999999999999999999999999999", 0, -1, 0, {0u, 3u, 244u, 138851u}},
  {"99999999999999999999999999999999999999", 0, 0, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 0, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 1, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 1, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 2, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 2, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 3, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 0, 3, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, -2, 0, {1u, 3u, 901u, 552102u}},
  {"99999999999999999999999999999999999999", 1, -1, 0, {0u, 3u, 244u, 138851u}},
  {"99999999999999999999999999999999999999", 1, 0, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 0, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 1, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 1, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 2, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 2, 1, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 3, 0, {0u, 5u, 588u, 403492u}},
  {"99999999999999999999999999999999999999", 1, 3, 1, {0u, 5u, 588u, 403492u}},
  {"340282366920938463463374607431768211457", 0, -2, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 0, -1, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 0, 0, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 0, 1, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 0, 2, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 0, 3, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, -2, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, -1, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, 0, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, 1, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, 2, 0, {0u, 4u, 362u, 310154u}},
  {"340282366920938463463374607431768211457", 1, 3, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, -2, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, -1, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, 0, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, 1, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, 2, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 0, 3, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, -2, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, -1, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, 0, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, 1, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, 2, 0, {0u, 4u, 362u, 310154u}},
  {"1701411834604692317316873037158841057275", 1, 3, 0, {0u, 4u, 362u, 310154u}},
};

TEST_CASE ("V5 mht_get_hash_number () of a NUMERIC is develop's, for fixed and floating values, both signs, scales -2..3 and coefficients past 2^128 (a hash partition places rows by it)", "[contract]")
{
  const int sizes[] = { 2, 7, 1024, 1000003 };
  int bad = 0;
  for (const hash_row & r:hash_rows)
    {
      const int nd = (int) std::strlen (r.digits);
      DB_VALUE v = make (r.digits, r.fixed ? 38 : nd, r.s, r.neg != 0 && std::strcmp (r.digits, "0") != 0, r.fixed != 0);
      for (int k = 0; k < 4; k++)
	{
	  const unsigned int got = mht_get_hash_number (sizes[k], &v);
	  if (got != r.h[k] && bad++ < 10)
	    {
	      FAIL_CHECK (std::string (r.digits) + " neg " + std::to_string (r.neg) + " s " + std::to_string (r.s)
			  + (r.fixed ? " fixed" : " float") + " size " + std::to_string (sizes[k]) + ": "
			  + std::to_string (got) + " != " + std::to_string (r.h[k]));
	    }
	}
    }
  CHECK (bad == 0);
}
