/*
 * PR#8196 (CBRD-27361) -- numeric_agg_expr_from_disk () (src/query/numeric_opfunc.c, server).
 *
 * The page batch (expr_batch.c, eb_extract) loads a fixed-precision NUMERIC column straight from the record bytes
 * into the word domain of the aggregate accumulator: numeric_agg_expr_from_disk (disk, avail, out).  Its contract,
 * as the function's own comment states it: "the same reading mr_data_readval_numeric () makes" -- so for every
 * NUMERIC value the engine can write (mr_data_writeval_numeric, through tp_Numeric.data_writeval), reading the bytes
 * back with from_disk must give exactly what reading them back through mr_data_readval_numeric and then
 * numeric_agg_expr_from_dbv gives: the same accept/decline (a coefficient past uint128 is declined by both), the
 * same coefficient, scale and sign.  The avail guard must decline anything shorter than the declared size, and a
 * zero must come out non-negative whatever the sign bit says (numeric_agg_expr_make normalizes).
 *
 * Real code: numeric_opfunc.c (unit under test), db_macro.c, object_primitive.c (the writer and the reader the
 * contract is measured against).  Faked: er_set / er_errid / er_clear and prm_Def (as in expr_compile.sum_acc).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "object_representation.h"

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

/* a floating NUMERIC from its text (the value's own digits as precision) */
static DB_VALUE
num (const std::string & s)
{
  DB_VALUE v;
  db_make_null (&v);
  REQUIRE (numeric_coerce_string_to_num (s.c_str (), (int) s.size (), INTL_CODESET_ISO88591, &v) == NO_ERROR);
  return v;
}

/* the same value in a fixed NUMERIC (p, s) domain -- what a column NUMERIC(p, s) stores.  p stays below 40:
 * a domain precision of 40 (DB_DEFAULT_NUMERIC_PRECISION) is what the engine calls a floating NUMERIC
 * (db_get_numeric_precision_and_scale, mr_data_readval_numeric), and the batch declines such a column
 * (eb_new_col_vec) -- so NUMERIC(40, s) columns are outside from_disk's contract */
static DB_VALUE
fixed (const std::string & s, int p, int sc)
{
  DB_VALUE f = num (s), v;
  REQUIRE (p < DB_DEFAULT_NUMERIC_PRECISION);
  unsigned char buf[DB_NUMERIC_BUF_SIZE];
  bool neg = DB_VALUE_NUMERIC_IS_VALUE_NEGATIVE (&f);
  int fp = 0, fs = 0;
  db_get_numeric_precision_and_scale (&f, &fp, &fs, NULL);
  REQUIRE (numeric_coerce_num_to_num (&f, fp, fs, p, sc, buf, &neg) == NO_ERROR);
  db_make_null (&v);
  REQUIRE (db_make_numeric (&v, buf, p, sc, DB_NUMERIC_BUF_SIZE, neg, false) == NO_ERROR);
  return v;
}

/* the record bytes mr_data_writeval_numeric () writes for the value */
static std::vector<unsigned char>
to_disk (const DB_VALUE & v)
{
  char raw[64];
  OR_BUF buf;
  std::memset (raw, 0xA5, sizeof (raw));
  or_init (&buf, raw, sizeof (raw));
  REQUIRE (tp_Numeric.data_writeval (&buf, &v) == NO_ERROR);
  return std::vector<unsigned char> (raw, raw + (buf.ptr - raw));
}

/* the bytes read back the engine's way: mr_data_readval_numeric () under a NUMERIC(p, s) domain */
static DB_VALUE
from_disk_engine (const std::vector<unsigned char> &d, int p, int sc)
{
  TP_DOMAIN dom;
  OR_BUF buf;
  DB_VALUE v;
  std::memset (&dom, 0, sizeof (dom));
  dom.type = &tp_Numeric;
  dom.precision = p;
  dom.scale = sc;
  or_init (&buf, (char *) d.data (), (int) d.size ());
  db_make_null (&v);
  REQUIRE (tp_Numeric.data_readval (&buf, &v, &dom, -1, true, NULL, 0) == NO_ERROR);
  return v;
}

static std::string
show (const NUMERIC_AGG_EXPR_VAL & w)
{
  char hi[32], lo[32];
  std::snprintf (hi, sizeof (hi), "%016llx", (unsigned long long) (w.coefficient >> 64));
  std::snprintf (lo, sizeof (lo), "%016llx", (unsigned long long) (w.coefficient & ~(uint64_t) 0));
  return std::string (w.is_negative ? "-" : "+") + hi + lo + " e-" + std::to_string (w.scale);
}

static bool
same (const NUMERIC_AGG_EXPR_VAL & a, const NUMERIC_AGG_EXPR_VAL & b)
{
  return a.coefficient == b.coefficient && a.scale == b.scale && a.is_negative == b.is_negative;
}

static std::string
nines (int n)
{
  return std::string (n, '9');
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("every fixed NUMERIC(p, s) the writer lays out (p 1..39, s 0/2/p-1, both signs, 0/1/10^k-1): from_disk "
	   "accepts iff from_dbv accepts, same coefficient/scale/sign, and readval of the bytes agrees", "[contract]")
{
  int checked = 0, declined = 0;
  for (int p = 1; p < DB_DEFAULT_NUMERIC_PRECISION; p++)
    {
      for (int sc : { 0, 2, p - 1 })
	{
	  if (sc < 0 || sc > p)
	    {
	      continue;
	    }
	  const int ip = p - sc;
	  std::vector<std::string> mags = { "0" };
	  for (int k = 1; k <= ip; k++)
	    {
	      mags.push_back (k == 1 ? "1" : nines (k - 1));
	      if (k == ip)
		{
		  mags.push_back (nines (k));
		}
	    }
	  if (sc > 0)
	    {
	      mags.push_back ((ip > 0 ? nines (ip) : "0") + "." + nines (sc));
	      mags.push_back ("0." + std::string (sc - 1, '0') + "1");
	    }
	  for (const std::string & m : mags)
	    {
	      for (const char *sign : { "", "-" })
		{
		  DB_VALUE v = fixed (sign + m, p, sc);
		  std::vector<unsigned char> d = to_disk (v);
		  NUMERIC_AGG_EXPR_VAL a, b, c;
		  std::memset (&a, 0, sizeof (a));
		  std::memset (&b, 0, sizeof (b));
		  std::memset (&c, 0, sizeof (c));
		  const bool ok_dbv = numeric_agg_expr_from_dbv (&v, &a);
		  const bool ok_disk = numeric_agg_expr_from_disk (d.data (), (int) d.size (), &b);
		  DB_VALUE rv = from_disk_engine (d, p, sc);
		  const bool ok_rv = numeric_agg_expr_from_dbv (&rv, &c);
		  INFO ("NUMERIC(" << p << "," << sc << ") " << sign << m << " disk " << d.size () << " bytes, header "
			<< (int) d[0] << "/" << (int) d[1] << "/" << (int) d[2]);
		  REQUIRE ((int) (d[0] & 0x7F) == (int) d.size ());
		  REQUIRE (ok_disk == ok_dbv);
		  REQUIRE (ok_rv == ok_dbv);
		  if (ok_dbv)
		    {
		      INFO ("dbv " << show (a) << "\ndisk " << show (b) << "\nreadval " << show (c));
		      REQUIRE (same (a, b));
		      REQUIRE (same (a, c));
		      checked++;
		    }
		  else
		    {
		      declined++;
		    }
		}
	    }
	}
    }
  INFO ("accepted " << checked << ", declined (past uint128) " << declined);
  CHECK (checked > 500);
  CHECK (declined > 0);		/* 39-digit magnitudes at 2^128 and above are declined on both sides */
}

TEST_CASE ("word-domain boundary: 2^128-1 (16-byte magnitude) is taken by from_disk and from_dbv alike; 2^128 "
	   "in memory is declined by from_dbv (a 17th byte)", "[contract]")
{
  const std::string max128 = "340282366920938463463374607431768211455";	/* 2^128 - 1, 39 digits */
  const std::string over128 = "340282366920938463463374607431768211456";	/* 2^128 */
  for (int sc : { 0, 1 })
    {
      const std::string txt = sc == 0 ? max128 : max128.substr (0, 38) + "." + max128.substr (38);
      DB_VALUE v = fixed (txt, 39, sc);
      std::vector<unsigned char> d = to_disk (v);
      NUMERIC_AGG_EXPR_VAL a, b;
      std::memset (&a, 0, sizeof (a));
      std::memset (&b, 0, sizeof (b));
      INFO (txt << " -> " << d.size () << " bytes");
      REQUIRE ((int) d.size () == NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE);
      REQUIRE (numeric_agg_expr_from_dbv (&v, &a));
      REQUIRE (numeric_agg_expr_from_disk (d.data (), (int) d.size (), &b));
      CHECK (same (a, b));
      CHECK (b.coefficient == ~(uint128_t) 0);
    }
  DB_VALUE w = fixed (over128, 39, 0);
  NUMERIC_AGG_EXPR_VAL a;
  CHECK_FALSE (numeric_agg_expr_from_dbv (&w, &a));
  /* a hand-laid 20-byte record whose 17th payload byte is set: declined; cleared: the 16-byte value */
  std::vector<unsigned char> rec (NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE, 0);
  rec[0] = NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE;
  rec[1] = 39;
  rec[2] = 0;
  rec[NUMERIC_HEADER_SIZE] = 1;
  rec[NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE - 1] = 7;
  CHECK_FALSE (numeric_agg_expr_from_disk (rec.data (), (int) rec.size (), &a));
  rec[NUMERIC_HEADER_SIZE] = 0;
  REQUIRE (numeric_agg_expr_from_disk (rec.data (), (int) rec.size (), &a));
  CHECK (a.coefficient == 7);
  CHECK (a.scale == 0);
  CHECK_FALSE (a.is_negative);
}

TEST_CASE ("develop (object_primitive.c, not this PR): a fixed NUMERIC(39, s) value at or above 2^128 whose low 16 "
	   "bytes start with a zero byte loses its 17th byte on disk -- mr_get_fixed_numeric_size counts bytes 1..16 "
	   "only, so 2^128 is written as a 4-byte record and reads back as 0 (PASS = defect present)", "[defect]")
{
  struct probe
  {
    const char *txt;
    const char *reads_back;	/* what readval gives, as from_dbv's coefficient (decimal of the low bytes kept) */
  };
  const probe probes[] = {
    {"340282366920938463463374607431768211456", "0"},	/* 2^128: data[0] = 1, data[1..16] = 0 -> 1 byte kept */
    {"340282366920938463463374607431768211461", "5"},	/* 2^128 + 5 -> the low byte only */
    {"680564733841876926926749214863536422912", "0"},	/* 2 * 2^128 */
  };
  for (const probe & pr : probes)
    {
      DB_VALUE v = fixed (pr.txt, 39, 0);
      NUMERIC_AGG_EXPR_VAL a, b;
      std::vector<unsigned char> d = to_disk (v);
      INFO (pr.txt << " -> " << d.size () << " bytes on disk (20 expected for a 17-byte magnitude)");
      CHECK_FALSE (numeric_agg_expr_from_dbv (&v, &a));	/* in memory it is the full 17-byte value */
      CHECK ((int) d.size () < NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE);	/* defect: the record is short */
      DB_VALUE rv = from_disk_engine (d, 39, 0);
      REQUIRE (numeric_agg_expr_from_dbv (&rv, &b));	/* and what comes back fits 128 bits: the top byte is gone */
      REQUIRE (numeric_agg_expr_from_disk (d.data (), (int) d.size (), &a));	/* from_disk reads the same short record */
      CHECK (same (a, b));
      CHECK (std::to_string ((unsigned long long) (b.coefficient & 0xFFFFFFFFFFFFFFFFull)) == pr.reads_back);
      CHECK ((b.coefficient >> 64) == 0);
    }
  /* the same magnitude with a non-zero data[1] (2^128 + 2^120) keeps all 17 bytes: 20-byte record, declined by both */
  DB_VALUE ok = fixed ("341611605303370564264219210289101193216", 39, 0);
  std::vector<unsigned char> d = to_disk (ok);
  NUMERIC_AGG_EXPR_VAL a;
  CHECK ((int) d.size () == NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE);
  CHECK_FALSE (numeric_agg_expr_from_disk (d.data (), (int) d.size (), &a));
}

TEST_CASE ("avail: fewer bytes than the declared size or the header is declined; exact size and -1 are taken; "
	   "NULL and a size byte below 3 or past 20 are declined", "[contract]")
{
  DB_VALUE v = fixed ("-12345.67", 10, 2);
  std::vector<unsigned char> d = to_disk (v);
  NUMERIC_AGG_EXPR_VAL w, full;
  const int size = (int) d.size ();
  REQUIRE (numeric_agg_expr_from_disk (d.data (), size, &full));
  CHECK (numeric_agg_expr_from_disk (d.data (), -1, &w));
  CHECK (same (w, full));
  CHECK (numeric_agg_expr_from_disk (d.data (), size + 5, &w));
  CHECK (same (w, full));
  CHECK_FALSE (numeric_agg_expr_from_disk (d.data (), size - 1, &w));
  CHECK_FALSE (numeric_agg_expr_from_disk (d.data (), NUMERIC_HEADER_SIZE - 1, &w));
  CHECK_FALSE (numeric_agg_expr_from_disk (d.data (), 0, &w));
  CHECK_FALSE (numeric_agg_expr_from_disk (NULL, size, &w));
  /* a header whose size byte is below the header itself, or past 20 (3 + 17), is declined */
  std::vector<unsigned char> bad = d;
  bad[0] = (unsigned char) ((bad[0] & 0x80) | 2);
  CHECK_FALSE (numeric_agg_expr_from_disk (bad.data (), -1, &w));
  bad[0] = (unsigned char) ((d[0] & 0x80) | (NUMERIC_HEADER_SIZE + DB_NUMERIC_BUF_SIZE + 1));
  CHECK_FALSE (numeric_agg_expr_from_disk (bad.data (), -1, &w));
}

TEST_CASE ("a zero is never negative (-0.00 stored, a zero payload under a forged sign bit); a negative scale "
	   "in header[1]/[2] is read as readval reads it", "[contract]")
{
  DB_VALUE z = fixed ("-0.00", 5, 2);
  std::vector<unsigned char> d = to_disk (z);
  NUMERIC_AGG_EXPR_VAL w;
  REQUIRE (numeric_agg_expr_from_disk (d.data (), (int) d.size (), &w));
  CHECK (w.coefficient == 0);
  CHECK_FALSE (w.is_negative);
  CHECK (w.scale == 2);

  std::vector<unsigned char> forged = d;
  forged[0] |= NUMERIC_VALUE_SIGN_BIT_MASK;
  REQUIRE (numeric_agg_expr_from_disk (forged.data (), (int) forged.size (), &w));
  CHECK (w.coefficient == 0);
  CHECK_FALSE (w.is_negative);

  /* scale sign: header[1] bit 7 set, header[2] = 3 -> scale -3 (what mr_data_readval_numeric reads) */
  DB_VALUE v = fixed ("-123", 10, 0);
  std::vector<unsigned char> e = to_disk (v);
  e[1] |= NUMERIC_HEADER_SCALE_SIGN_BIT_MASK;
  e[2] = 3;
  REQUIRE (numeric_agg_expr_from_disk (e.data (), (int) e.size (), &w));
  CHECK (w.coefficient == 123);
  CHECK (w.is_negative);
  CHECK (w.scale == -3);
  DB_VALUE rv = from_disk_engine (e, 10, 0);
  CHECK (db_get_numeric_scale (&rv, NULL) == -3);
}
