/*
 * PR#8196 (CBRD-27361) group E -- a raw NUMERIC filter leaf ("numeric_col op constant") of the page batch
 * (src/query/expr_batch.c, server mode), TU-static, reached through the appendix:
 *
 *   eb_num_leaf_constant ()  brings the constant to the column's scale once a chunk (eb_load_leaf_constants ()):
 *                            k.n, the operator it becomes (k_op), or the answer for every non-NULL row (k_all)
 *   eb_raw_kid ()            the leaf over the chunk's selection (its EB_NUM branch reads the decoded vector)
 *
 * The reference is the row path's comparison of two NUMERIC values, numeric_db_value_compare () (numeric_opfunc.c,
 * linked as the real TU): the column value as a NUMERIC DB_VALUE at the column's scale against the constant's own.
 *
 *   [contract]  every constant whose coefficient fits 127 bits -- integers, decimals of more and fewer fraction
 *               digits than the column, 10^38 and 2^127 - 1 -- over column scales 0, 2, 4, eighteen column values
 *               and the six operators the compiled leaf takes (expr_compile.c: R_EQ .. R_GE): the rows the leaf
 *               keeps are the rows the comparison keeps
 *   [defect]    E-01: a constant whose coefficient is in [2^127, 2^128) -- 39 digits, which numeric_agg_expr_from_dbv ()
 *               takes (the word domain is uint128) and expr_numeric_cmp_const_ok () admits (any NUMERIC) -- is
 *               converted to a signed __int128 that wraps negative (expr_batch.c:3787), so "c < 2E38" keeps no row
 *               (PASS = the defect is there)
 *
 * Faked: expr_numeric_cmp_const_ok () (expr_compile.c:4351) -- defined below as that function reads: SHORT, INTEGER
 * and NUMERIC true, BIGINT inside (-10^15, 10^15), anything else false.  Nothing else on these paths.
 */
#include "catch.hpp"

#include "dbtype.h"
#include "numeric_opfunc.h"
#include "intl_support.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern int unitseam_eb_num_leaf (const __int128 * col_vals, const unsigned char *col_null, int n, int col_scale,
				 const DB_VALUE * k, int rel_op, int *kept, int *n_kept, int *k_all, int *k_op,
				 __int128 * kn);

/* expr_compile.c:4351 as it reads at 2381ce437 */
bool
expr_numeric_cmp_const_ok (const DB_VALUE * k)
{
  switch (DB_VALUE_DOMAIN_TYPE (k))
    {
    case DB_TYPE_SHORT:
    case DB_TYPE_INTEGER:
    case DB_TYPE_NUMERIC:
      return true;
    case DB_TYPE_BIGINT:
      return db_get_bigint (k) < 1000000000000000LL && db_get_bigint (k) > -1000000000000000LL;
    default:
      return false;
    }
}

/* REL_OP (xasl_predicate.hpp) */
enum { T_R_EQ = 1, T_R_NE, T_R_GT, T_R_GE, T_R_LT, T_R_LE };

static std::string
i128_str (__int128 v)
{
  bool neg = v < 0;
  unsigned __int128 u = neg ? (unsigned __int128) (-(v + 1)) + 1 : (unsigned __int128) v;
  std::string s;

  do
    {
      s.insert (s.begin (), (char) ('0' + (int) (u % 10)));
      u /= 10;
    }
  while (u != 0);
  return neg ? "-" + s : s;
}

/* coefficient c at scale s as a decimal string ("-0.05" for -5 at scale 2) */
static std::string
dec_str (__int128 c, int s)
{
  std::string m = i128_str (c < 0 ? -c : c);

  if (s > 0)
    {
      while ((int) m.size () <= s)
	{
	  m.insert (m.begin (), '0');
	}
      m.insert (m.end () - s, '.');
    }
  return (c < 0) ? "-" + m : m;
}

static void
make_num (const std::string & s, DB_VALUE * v)
{
  int rc = numeric_coerce_string_to_num (s.c_str (), (int) s.size (), INTL_CODESET_ISO88591, v);

  REQUIRE (rc == NO_ERROR);
  REQUIRE (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_NUMERIC);
}

static bool
ref_keep (int cmp, int op)
{
  switch (op)
    {
    case T_R_EQ:
      return cmp == 0;
    case T_R_NE:
      return cmp != 0;
    case T_R_LT:
      return cmp < 0;
    case T_R_LE:
      return cmp <= 0;
    case T_R_GT:
      return cmp > 0;
    default:
      return cmp >= 0;
    }
}

static const __int128 E15 = (__int128) 1000000000000000LL;
static const __int128 E37 = E15 * E15 * (__int128) 10000000LL;

static std::vector<__int128>
column_values ()
{
  return { -E37, -(E15 - 1), -2345, -235, -234, -1, 0, 1, 5, 7, 234, 235, 2345, 2399, 2400, 2401, E15 - 1, E37 };
}

/* the rows the leaf keeps vs the rows the comparison keeps; returns the number of rows that differ (-1: the leaf
 * declined the constant -- EXPR_BATCH_FALLBACK, the row path's chunk) */
static int
mismatches (const std::string & k_str, int col_scale, int op, std::string * first)
{
  std::vector<__int128> col = column_values ();
  int n = (int) col.size (), kept[256], n_kept = 0, k_all = 0, k_op = 0, i, diff = 0;
  __int128 kn = 0;
  DB_VALUE k;
  std::vector<bool> batch_keep (n, false);

  make_num (k_str, &k);
  if (unitseam_eb_num_leaf (col.data (), NULL, n, col_scale, &k, op, kept, &n_kept, &k_all, &k_op, &kn) != NO_ERROR)
    {
      return -1;
    }
  for (i = 0; i < n_kept; i++)
    {
      batch_keep[kept[i]] = true;
    }
  for (i = 0; i < n; i++)
    {
      DB_VALUE cv, ans;

      make_num (dec_str (col[i], col_scale), &cv);
      REQUIRE (numeric_db_value_compare (&cv, &k, &ans) == NO_ERROR);
      if (ref_keep (db_get_int (&ans), op) != batch_keep[i])
	{
	  if (diff == 0 && first != NULL)
	    {
	      *first = "col " + dec_str (col[i], col_scale) + " op " + std::to_string (op) + " K " + k_str
		+ ": row path " + (ref_keep (db_get_int (&ans), op) ? "keeps" : "drops") + ", batch "
		+ (batch_keep[i] ? "keeps" : "drops") + " (k_all " + std::to_string (k_all) + ", k_op "
		+ std::to_string (k_op) + ", k.n " + i128_str (kn) + ")";
	    }
	  diff++;
	}
    }
  return diff;
}

TEST_CASE ("contract: a NUMERIC leaf keeps what the comparison keeps for a constant of at most 127 bits", "[contract]")
{
  const char *ks[] = {
    "0", "1", "-1", "24", "0.05", "0.07", "-0.055", "2.345", "23.99", "24.005", "123456789012.345678",
    "0.000000000000000000000000000000000001",
    "99999999999999999999999999999999999999", "-99999999999999999999999999999999999999",
    "100000000000000000000000000000000000000", "-100000000000000000000000000000000000000",
    "170141183460469231731687303715884105727", "-170141183460469231731687303715884105727",
    "1.70141183460469231731687303715884105727"
  };
  int scales[] = { 0, 2, 4 };
  int ops[] = { T_R_EQ, T_R_NE, T_R_LT, T_R_LE, T_R_GT, T_R_GE };
  int checked = 0, declined = 0;

  for (const char *k : ks)
    {
      for (int s : scales)
	{
	  for (int op : ops)
	    {
	      std::string first;
	      int d = mismatches (k, s, op, &first);

	      if (d < 0)
		{
		  declined++;
		  continue;
		}
	      INFO (first);
	      CHECK (d == 0);
	      checked++;
	    }
	}
    }
  INFO ("checked " << checked << " (constant, scale, operator) triples, declined " << declined);
  CHECK (checked >= 300);
}

TEST_CASE ("E-01 defect: a 39-digit constant at or past 2^127 wraps negative and the leaf answers the opposite",
	   "[defect]")
{
  /* 2E38, -2E38, 2^128 - 1 and a 39-digit fraction: numeric_agg_expr_from_dbv () takes each (words[0] == 0) */
  const char *ks[] = {
    "200000000000000000000000000000000000000", "-200000000000000000000000000000000000000",
    "340282366920938463463374607431768211455", "0.200000000000000000000000000000000000001",
    "170141183460469231731687303715884105728"
  };
  int ops[] = { T_R_EQ, T_R_NE, T_R_LT, T_R_LE, T_R_GT, T_R_GE };
  int total = 0, lt_all_dropped = 0;
  std::string first;

  for (const char *k : ks)
    {
      for (int op : ops)
	{
	  std::string f;
	  int d = mismatches (k, 2, op, &f);

	  REQUIRE (d >= 0);	/* taken, not declined: the leaf runs */
	  if (d > 0 && first.empty ())
	    {
	      first = f;
	    }
	  total += d;
	}
    }
  /* "c < 2E38" over a NUMERIC column: the row path keeps every row, the leaf none */
  {
    std::vector<__int128> col = column_values ();
    int kept[256], n_kept = -1, k_all = 0, k_op = 0;
    __int128 kn = 0;
    DB_VALUE k;

    make_num ("200000000000000000000000000000000000000", &k);
    REQUIRE (unitseam_eb_num_leaf (col.data (), NULL, (int) col.size (), 2, &k, T_R_LT, kept, &n_kept, &k_all, &k_op,
				   &kn) == NO_ERROR);
    lt_all_dropped = (n_kept == 0);
  }
  INFO ("first difference: " << first << "; rows that differ in all: " << total);
  CHECK (total > 0);
  CHECK (lt_all_dropped == 1);
}
