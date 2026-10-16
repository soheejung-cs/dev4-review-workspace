/*
 * PR#7658 (CBRD-27215, with the CBRD-27408 port) -- the NUMERIC SUM accumulator (src/query/numeric_opfunc.c, server).
 *
 * SUM / AVG over NUMERIC keep a resident 3-word accumulator (SUM_ACC, CBRD-27178) that is exact up to 57 digits;
 * the binary "+" rounds once a result passes 40 digits (float_numeric_db_value_add).  The PR ports CBRD-27408: a sum
 * that reaches 10^40 (41 digits) is taken by one rounded binary add and reloaded, so SUM (n) equals n1 + n2 + ... as
 * the user writes it (design D24).  The contract here: an accumulator fed value by value -- through a DB_VALUE
 * (numeric_sum_acc_add_dbv) or through the word register path of a fused operand (numeric_sum_acc_add_expr_val) --
 * snapshots to the value of the "+" chain over the same values in the same order; a merge of two partial
 * accumulators (the parallel workers' combine) equals the "+" of their two snapshots.
 *
 * The base (develop before the port) keeps the exact sum and rounds once at the end: the crossing cases FAIL there
 * by behaviour -- that is the PR's documented change, not a regression.  Below 41 digits both revisions agree.
 *
 * Real code: numeric_opfunc.c is the unit under test; db_macro.c (db_value_domain_init) and object_primitive.c are
 * linked in.  Faked: er_set / er_errid (the error manager: records the code) and prm_Def (the defaults).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "object_domain.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes */

static int g_errid = NO_ERROR;
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
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

/* system_parameter.c: the parameters these paths read are inline over prm_Def (numeric_db_value_print reads
 * oracle_compat_number_behavior); booted values: the defaults */
SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };
static bool
boot_params ()
{
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].value.v.b = false;
  return true;
}
static const bool params_booted = boot_params ();

/* ---------------------------------------------------------------- values */

static DB_VALUE
num (const std::string & s)
{
  DB_VALUE v;
  db_make_null (&v);
  REQUIRE (numeric_coerce_string_to_num (s.c_str (), (int) s.size (), INTL_CODESET_ISO88591, &v) == NO_ERROR);
  return v;
}

static std::string
show (const DB_VALUE * v)
{
  char buf[256];
  if (DB_IS_NULL (v))
    {
      return "NULL";
    }
  int p = -1, s = -1;
  bool f = false;
  db_get_numeric_precision_and_scale (v, &p, &s, &f);
  numeric_db_value_print (v, buf);
  return std::string (buf) + " (" + std::to_string (p) + "," + std::to_string (s) + ")";
}

/* the "+" chain: ((v1 + v2) + v3) ... with the binary operator of two NUMERICs */
static std::string
plus_chain (const std::vector<DB_VALUE> &vals)
{
  DB_VALUE acc = vals[0], next;
  for (size_t i = 1; i < vals.size (); i++)
    {
      db_make_null (&next);
      if (float_numeric_db_value_add (&acc, &vals[i], &next) != NO_ERROR)
	{
	  return "ERR";
	}
      acc = next;
    }
  return show (&acc);
}

static std::string
snapshot (const SUM_ACC & acc)
{
  DB_VALUE r;
  db_make_null (&r);
  if (numeric_sum_acc_snapshot (&acc, &r) != NO_ERROR)
    {
      return "ERR";
    }
  return show (&r);
}

/* SUM through the accumulator, one DB_VALUE at a time (the plain SUM path) */
static std::string
acc_sum (const std::vector<DB_VALUE> &vals, SUM_ACC * out = NULL)
{
  SUM_ACC acc;
  std::memset (&acc, 0, sizeof (acc));
  for (const DB_VALUE & v:vals)
    {
      if (numeric_sum_acc_add_dbv (&acc, &v) != NO_ERROR)
	{
	  return "ERR";
	}
    }
  if (out != NULL)
    {
      *out = acc;
    }
  return snapshot (acc);
}

/* SUM through the accumulator from the fused operand's word registers (numeric_sum_acc_add_expr_val): the first
 * value loads the accumulator as a DB_VALUE (the caller activates it so), the rest go in as words */
static std::string
acc_sum_words (const std::vector<DB_VALUE> &vals, bool * all_words)
{
  SUM_ACC acc;
  std::memset (&acc, 0, sizeof (acc));
  *all_words = true;
  for (const DB_VALUE & v:vals)
    {
      NUMERIC_AGG_EXPR_VAL w;
      if (!acc.is_active || !numeric_agg_expr_from_dbv (&v, &w))
	{
	  *all_words = *all_words && !acc.is_active;
	  if (numeric_sum_acc_add_dbv (&acc, &v) != NO_ERROR)
	    {
	      return "ERR";
	    }
	  continue;
	}
      if (numeric_sum_acc_add_expr_val (&acc, &w) != NO_ERROR)
	{
	  return "ERR";
	}
    }
  return snapshot (acc);
}

static std::string
nines (int n)
{
  return std::string (n, '9');
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("a coefficient below 10^40 throughout (integer and fraction digits within 40): the accumulator is exact "
	   "and equals the + chain -- the same on both revisions", "[contract]")
{
  std::vector<DB_VALUE> v = {
    num ("1.5"), num ("-2.25"), num ("123456789012345678.123"), num ("-99999.99999"), num (nines (30)),
    num ("-" + nines (29)), num ("7"), num ("0.5"), num ("-0.5"), num ("0.00001")
  };
  const std::string a = acc_sum (v), c = plus_chain (v);
  bool words = false;
  const std::string w = acc_sum_words (v, &words);
  INFO ("acc " << a << "\nchain " << c << "\nwords " << w);
  CHECK (a == c);
  CHECK (w == c);
}

TEST_CASE ("CBRD-27408 through the scale: 30 integer + 30 fraction digits put the aligned coefficient past 10^40 "
	   "-- SUM equals the + chain (...789; the base's exact sum rounds once to ...790)", "[contract]")
{
  std::vector<DB_VALUE> v = {
    num ("1.5"), num ("-2.25"), num ("123456789012345678.123"), num ("0.0000000001"), num ("-99999.99999"),
    num (nines (30)), num ("-" + nines (29)), num ("7"), num ("0.5"), num ("-0.5"),
    num ("12345678901234567890123456789.0123456789"), num ("-0.000000000000000000000000000001")
  };
  const std::string a = acc_sum (v), c = plus_chain (v);
  bool words = false;
  const std::string w = acc_sum_words (v, &words);
  INFO ("acc " << a << "\nchain " << c << "\nwords " << w);
  CHECK (a == c);
  CHECK (w == c);
}

TEST_CASE ("CBRD-27408: 9999...998 (40 digits) + 4 + 4 -- SUM equals the + chain once the sum reaches 10^40 "
	   "(the base keeps the exact sum and differs)", "[contract]")
{
  std::vector<DB_VALUE> v = { num (nines (39) + "8"), num ("4"), num ("4") };
  const std::string a = acc_sum (v), c = plus_chain (v);
  bool words = false;
  const std::string w = acc_sum_words (v, &words);
  INFO ("acc " << a << "\nchain " << c << "\nwords " << w);
  CHECK (a == c);
  CHECK (w == c);
}

TEST_CASE ("CBRD-27408: 3036 x (10^38 - 1) -- SUM equals the + chain (...9900), the rounding taken row by row "
	   "from where the sum first reaches 10^40", "[contract]")
{
  std::vector<DB_VALUE> v (3036, num (nines (38)));
  const std::string a = acc_sum (v), c = plus_chain (v);
  bool words = false;
  const std::string w = acc_sum_words (v, &words);
  INFO ("acc " << a << "\nchain " << c << "\nwords " << w);
  CHECK (a == c);
  CHECK (w == c);
}

TEST_CASE ("crossing 10^40 up and back down with mixed signs and scales: the accumulator stays the + chain (a "
	   "capacity fallback leaves the accumulator and its sign untouched before the rounded add)", "[contract]")
{
  std::vector<std::vector<DB_VALUE>> seqs = {
    {num (nines (40)), num ("1"), num ("-" + nines (40)), num ("0.5")},
    {num ("-" + nines (40)), num ("-1"), num (nines (40)), num ("2"), num ("-0.25")},
    {num ("1.5"), num (nines (39)), num (nines (39)), num ("-" + nines (39)), num ("0.000000000000000000001")},
    {num ("0.0000000000000000000000000000000000000001"), num (nines (40)), num ("-1")},
    {num (nines (20) + "." + nines (20)), num (nines (20) + "." + nines (20)), num ("-1"),
     num (nines (39) + "0")},
  };
  std::vector<std::string> bad;
  for (size_t k = 0; k < seqs.size (); k++)
    {
      const std::string a = acc_sum (seqs[k]), c = plus_chain (seqs[k]);
      bool words = false;
      const std::string w = acc_sum_words (seqs[k], &words);
      if (a != c || w != c)
	{
	  bad.push_back ("sequence " + std::to_string (k) + "\n  acc " + a + "\n  words " + w + "\n  chain " + c);
	}
    }
  INFO ((bad.empty ()? std::string () : bad[0]));
  CHECK (bad.empty ());
}

TEST_CASE ("merge of two partial accumulators (the parallel workers' combine) equals the + of their two snapshots, "
	   "below and across 10^40", "[contract]")
{
  struct split
  {
    std::vector<DB_VALUE> a, b;
  };
  std::vector<split> splits = {
    {{num ("1.5"), num ("2.25")}, {num ("-0.75"), num ("100")}},
    {{num (nines (39) + "8")}, {num ("4"), num ("4")}},
    {std::vector<DB_VALUE> (1500, num (nines (38))), std::vector<DB_VALUE> (1536, num (nines (38)))},
    {{num ("-" + nines (40))}, {num ("-" + nines (40)), num ("0.5")}},
  };
  std::vector<std::string> bad;
  for (size_t k = 0; k < splits.size (); k++)
    {
      SUM_ACC a, b;
      acc_sum (splits[k].a, &a);
      acc_sum (splits[k].b, &b);
      DB_VALUE sa, sb, sum;
      db_make_null (&sa);
      db_make_null (&sb);
      db_make_null (&sum);
      REQUIRE (numeric_sum_acc_snapshot (&a, &sa) == NO_ERROR);
      REQUIRE (numeric_sum_acc_snapshot (&b, &sb) == NO_ERROR);
      REQUIRE (float_numeric_db_value_add (&sa, &sb, &sum) == NO_ERROR);
      REQUIRE (numeric_sum_acc_merge (&a, &b) == NO_ERROR);
      const std::string m = snapshot (a), c = show (&sum);
      if (m != c)
	{
	  bad.push_back ("split " + std::to_string (k) + "\n  merge " + m + "\n  snapshot+snapshot " + c);
	}
    }
  INFO ((bad.empty ()? std::string () : bad[0]));
  CHECK (bad.empty ());
}
