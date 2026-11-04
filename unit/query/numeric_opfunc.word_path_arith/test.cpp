/*
 * PR#8349 (CBRD-27819) -- float_numeric_db_value_add/sub/mul (src/query/numeric_opfunc.c, server): the 128-bit word
 * path the PR puts first (numeric_word_from_dbv/_add/_mul/_to_dbv, the bodies of numeric_agg_expr_*) must give what
 * develop gives -- the same return code, NULL-ness, error id, domain precision/scale, header precision/scale, sign
 * and 17 bytes -- for every operand pair.
 *
 * The contract is a digest: a fixed, deterministic pair list (an edge cross product of magnitudes x scales x signs,
 * then seeded random float and fixed NUMERICs, the second scale near the first half the time) goes through each
 * function and every result record is folded into an FNV-1a digest per function. EXPECTED_* are the digests of develop
 * 80b84317d (the PR's base). A FAIL means some record differs: set DIFF_OUT=<file> on both revisions to get the block
 * digests (4096 pairs a block) and diff them, then DUMP_BLOCK=<add|sub|mul>:<block> to write that block's records.
 *
 * The [defect] case is develop's, not this PR's: a product of two coefficients below 2^64 whose result scale is below
 * -214 takes the 64-bit fast path (kept by the PR for the scales its word path declines), db_make_numeric () refuses
 * the scale and the answer is NULL with NO_ERROR, where the general path reports ER_IT_DATA_OVERFLOW.
 *
 * Real code: numeric_opfunc.c (unit under test), db_macro.c, object_primitive.c. Faked: er_set/er_errid/er_clear
 * (last id), tp_domain_resolve_default (a tp_Numeric domain, for the overflow message), intl_is_space (ASCII blanks),
 * prm_Def (oracle_compat_number_behavior = false).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "numeric_opfunc.h"
#include "object_domain.h"
#include "object_primitive.h"

#include <cstdio>
#include <cstdlib>
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

/* the overflow paths name the NUMERIC type: tp_domain_resolve_default (DB_TYPE_NUMERIC) -> a domain of tp_Numeric */
TP_DOMAIN *
tp_domain_resolve_default (DB_TYPE type)
{
  static TP_DOMAIN dom;
  (void) type;
  dom.type = &tp_Numeric;
  return &dom;
}

/* numeric_coerce_string_to_num () skips blanks: ISO-8859-1 only here */
bool
intl_is_space (const char *str, const char *str_end, const INTL_CODESET codeset, int *space_size)
{
  (void) str_end;
  (void) codeset;
  if (space_size != NULL)
    {
      *space_size = 1;
    }
  return *str == ' ' || *str == '\t' || *str == '\n' || *str == '\r' || *str == '\f' || *str == '\v';
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

/* ---------------------------------------------------------------- 17-byte big-endian magnitudes */

typedef struct
{
  unsigned char b[DB_NUMERIC_BUF_SIZE];
} MAG;

static MAG P10[41];

static void
mag_mul_small (MAG & m, unsigned int f, unsigned int add)
{
  unsigned int carry = add;
  for (int i = DB_NUMERIC_BUF_SIZE - 1; i >= 0; i--)
    {
      unsigned int v = m.b[i] * f + carry;
      m.b[i] = (unsigned char) (v & 0xff);
      carry = v >> 8;
    }
}

static int
mag_cmp (const MAG & a, const MAG & b)
{
  return std::memcmp (a.b, b.b, DB_NUMERIC_BUF_SIZE);
}

static bool
init_p10 ()
{
  std::memset (&P10[0], 0, sizeof (MAG));
  P10[0].b[DB_NUMERIC_BUF_SIZE - 1] = 1;
  for (int i = 1; i <= 40; i++)
    {
      P10[i] = P10[i - 1];
      mag_mul_small (P10[i], 10, 0);
    }
  return true;
}
static const bool p10_ready = init_p10 ();

static int
mag_digits (const MAG & m)
{
  int d = 1;
  while (d < 40 && mag_cmp (m, P10[d]) >= 0)
    {
      d++;
    }
  return d;
}

static MAG
mag_from_u128 (unsigned __int128 v, unsigned char top)
{
  MAG m;
  m.b[0] = top;
  for (int i = 16; i >= 1; i--)
    {
      m.b[i] = (unsigned char) (v & 0xff);
      v >>= 8;
    }
  return m;
}

static MAG
mag_minus_one (const MAG & a)
{
  MAG m = a;
  for (int i = DB_NUMERIC_BUF_SIZE - 1; i >= 0; i--)
    {
      if (m.b[i]-- != 0)
	{
	  break;
	}
    }
  return m;
}

static MAG
mag_plus_one (const MAG & a)
{
  MAG m = a;
  for (int i = DB_NUMERIC_BUF_SIZE - 1; i >= 0; i--)
    {
      if (++m.b[i] != 0)
	{
	  break;
	}
    }
  return m;
}

/* ---------------------------------------------------------------- generator */

static uint64_t rng_state = 0x8349c0de8349c0deULL;
static uint64_t
rnd ()
{
  uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static int
rnd_range (int lo, int hi)
{
  return lo + (int) (rnd () % (uint64_t) (hi - lo + 1));
}

static std::vector<MAG> edge_mags;

static void
init_edges ()
{
  if (!edge_mags.empty ())
    {
      return;
    }
  const unsigned __int128 one = 1;
  unsigned __int128 vs[] = {
    0, 1, 2, 5, 9, 10, 99, 12345, (one << 32), (one << 63) - 1, (one << 63), (one << 64) - 1, (one << 64),
    (one << 64) + 1, (one << 127) - 1, (one << 127), ~(unsigned __int128) 0
  };
  for (unsigned __int128 v:vs)
    {
      edge_mags.push_back (mag_from_u128 (v, 0));
    }
  edge_mags.push_back (mag_from_u128 (0, 1));	/* 2^128 */
  edge_mags.push_back (mag_plus_one (mag_from_u128 (0, 1)));	/* 2^128 + 1 */
  for (int k : { 18, 19, 20, 37, 38, 39 })
    {
      edge_mags.push_back (P10[k]);
      edge_mags.push_back (mag_minus_one (P10[k]));
    }
  edge_mags.push_back (mag_minus_one (P10[40]));	/* 10^40 - 1 */
}

static MAG
rand_mag ()
{
  if (rnd () % 8 == 0)
    {
      init_edges ();
      return edge_mags[rnd () % edge_mags.size ()];
    }
  int bits = rnd_range (0, 133);
  MAG m;
  std::memset (&m, 0, sizeof (m));
  for (int i = 0; i < bits; i++)
    {
      if (rnd () & 1)
	{
	  m.b[DB_NUMERIC_BUF_SIZE - 1 - i / 8] |= (unsigned char) (1u << (i % 8));
	}
    }
  if (bits > 0)
    {
      m.b[DB_NUMERIC_BUF_SIZE - 1 - (bits - 1) / 8] |= (unsigned char) (1u << ((bits - 1) % 8));
    }
  while (mag_cmp (m, P10[40]) >= 0)
    {
      m.b[0] >>= 1;
    }
  return m;
}

static const int edge_scales[] = {
  -214, -213, -200, -120, -108, -107, -100, -84, -40, -39, -38, -37, -20, -19, -2, -1, 0, 1, 2, 5, 18, 19, 20, 37,
  38, 39, 40, 76, 100, 126, 127, 200, 214, 251, 252
};

static int
rand_scale (int near_scale, bool use_near)
{
  if (use_near)
    {
      int s = near_scale + rnd_range (-45, 45);
      return s < -214 ? -214 : s > 252 ? 252 : s;
    }
  if (rnd () % 3 == 0)
    {
      return edge_scales[rnd () % (sizeof (edge_scales) / sizeof (edge_scales[0]))];
    }
  return rnd_range (-214, 252);
}

typedef struct
{
  MAG mag;
  int prec;
  int scale;
  bool neg;
  bool is_float;
} SPEC;

static SPEC
rand_spec (int near_scale, bool use_near)
{
  SPEC s;
  s.mag = rand_mag ();
  const int digits = mag_digits (s.mag);
  const bool is_zero = mag_cmp (s.mag, P10[0]) < 0;
  s.neg = (rnd () & 1) != 0;
  if (is_zero && rnd () % 50 != 0)
    {
      s.neg = false;		/* a forged -0 now and then */
    }
  s.is_float = digits > 38 || (rnd () % 3 != 0);
  if (s.is_float)
    {
      s.prec = (rnd () % 5 == 0) ? rnd_range (digits, 40) : digits;
      s.scale = rand_scale (near_scale, use_near);
    }
  else
    {
      s.prec = rnd_range (digits, 38);
      s.scale = (rnd () % 10 == 0) ? rnd_range (-84, -1) : rnd_range (0, s.prec);
      if (use_near && rnd () % 2 == 0)
	{
	  int t = near_scale + rnd_range (-3, 3);
	  s.scale = t < -84 ? -84 : t > 127 ? 127 : t;
	}
    }
  return s;
}

static void
make (DB_VALUE * v, const SPEC & s)
{
  db_make_null (v);
  int rc = db_make_numeric (v, (DB_C_NUMERIC) s.mag.b, s.prec, s.scale, DB_NUMERIC_BUF_SIZE, s.neg, s.is_float);
  REQUIRE (rc == NO_ERROR);
}

static std::string
spec_text (const SPEC & s)
{
  char t[128];
  int n = std::snprintf (t, sizeof (t), "%s%c(", s.is_float ? "F" : "X", s.neg ? '-' : '+');
  for (int i = 0; i < DB_NUMERIC_BUF_SIZE; i++)
    {
      n += std::snprintf (t + n, sizeof (t) - n, "%02x", s.mag.b[i]);
    }
  std::snprintf (t + n, sizeof (t) - n, ") p%d s%d", s.prec, s.scale);
  return t;
}

static std::string
result_text (int ret, const DB_VALUE * r)
{
  char t[200];
  int n = std::snprintf (t, sizeof (t), "ret=%d err=%d ", ret, g_errid);
  if (DB_IS_NULL (r))
    {
      std::snprintf (t + n, sizeof (t) - n, "NULL type=%d", (int) DB_VALUE_DOMAIN_TYPE (r));
      return t;
    }
  if (DB_VALUE_DOMAIN_TYPE (r) == DB_TYPE_INTEGER)
    {
      std::snprintf (t + n, sizeof (t) - n, "int=%d", db_get_int (r));
      return t;
    }
  int p = 0, s = 0;
  bool fl = false;
  db_get_numeric_precision_and_scale (r, &p, &s, &fl);
  n += std::snprintf (t + n, sizeof (t) - n, "dom=%d/%d p%d s%d %s %c ", (int) r->domain.numeric_info.precision,
		      r->domain.numeric_info.scale, p, s, fl ? "F" : "X", r->domain.numeric_info.is_value_negative ? '-' : '+');
  const unsigned char *b = (const unsigned char *) db_locate_numeric (r);
  for (int i = 0; i < DB_NUMERIC_BUF_SIZE; i++)
    {
      n += std::snprintf (t + n, sizeof (t) - n, "%02x", b[i]);
    }
  return t;
}

static uint64_t
fnv (uint64_t h, const std::string & s)
{
  for (unsigned char c:s)
    {
      h ^= c;
      h *= 0x100000001b3ULL;
    }
  return h;
}

/* ---------------------------------------------------------------- the digest contract */

enum
{ OP_ADD, OP_SUB, OP_MUL, OP_N };
static const char *op_name[OP_N] = { "add", "sub", "mul" };

/* develop 80b84317d */
static const uint64_t EXPECTED[OP_N] = { 0x68903e8b953abcc0ULL, 0x0fa87fe685a3deefULL, 0xf297223ccf00c025ULL };

static std::string
run_op (int op, const DB_VALUE * a, const DB_VALUE * b)
{
  DB_VALUE r;
  int ret;
  db_make_null (&r);
  er_clear ();
  switch (op)
    {
    case OP_ADD:
      ret = float_numeric_db_value_add (a, b, &r);
      break;
    case OP_SUB:
      ret = float_numeric_db_value_sub (a, b, &r);
      break;
    default:
      ret = float_numeric_db_value_mul (a, b, &r);
      break;
    }
  return result_text (ret, &r);
}

static const int lib_edge_scales[] = { -214, -200, -108, -107, -40, -39, -38, -19, -1, 0, 1, 19, 38, 39, 127, 252 };

static bool
below_2_128 (const MAG & m)
{
  return m.b[0] == 0;
}

TEST_CASE ("W1 float_numeric_db_value_add, _sub and _mul give develop's record (return code, NULL, error id, domain "
	   "and header precision/scale, sign, 17 bytes) for every pair of an edge cross product (32 magnitudes 0..10^40-1 "
	   "x 16 scales -214..252 x both signs) and 500,000 seeded random float/fixed pairs -- digests of develop 80b84317d",
	   "[contract]")
{
  const char *path = std::getenv ("DIFF_OUT");
  const char *dump = std::getenv ("DUMP_BLOCK");
  const long n_rand = 500000L;
  int dump_op = -1;
  long dump_block = -1;
  if (dump != NULL)
    {
      char opn[8] = { 0 };
      std::sscanf (dump, "%7[^:]:%ld", opn, &dump_block);
      for (int i = 0; i < OP_N; i++)
	{
	  if (std::strcmp (opn, op_name[i]) == 0)
	    {
	      dump_op = i;
	    }
	}
    }
  FILE *out = path ? std::fopen (path, "w") : NULL;

  rng_state = 0x8349c0de8349c0deULL;
  std::vector<SPEC> edges;
  init_edges ();
  for (const MAG & m:edge_mags)
    {
      for (int sc:lib_edge_scales)
	{
	  for (int neg = 0; neg < 2; neg++)
	    {
	      SPEC s;
	      s.mag = m;
	      s.prec = mag_digits (m);
	      s.scale = sc;
	      s.neg = neg != 0 && mag_cmp (m, P10[0]) >= 0;
	      s.is_float = true;
	      edges.push_back (s);
	    }
	}
    }

  const long n_edge = (long) edges.size () * (long) edges.size ();
  const long total = n_edge + n_rand;
  const long block_size = 4096;
  uint64_t digest[OP_N], block[OP_N];
  long word_domain = 0, wide = 0, null_no_error[OP_N] = { 0 }, overflow[OP_N] = { 0 };
  for (int op = 0; op < OP_N; op++)
    {
      digest[op] = block[op] = 0xcbf29ce484222325ULL;
    }

  for (long pair = 0; pair < total; pair++)
    {
      SPEC sa, sb;
      if (pair < n_edge)
	{
	  sa = edges[pair / (long) edges.size ()];
	  sb = edges[pair % (long) edges.size ()];
	}
      else
	{
	  sa = rand_spec (0, false);
	  sb = rand_spec (sa.scale, (rnd () & 1) != 0);
	}
      DB_VALUE a, b;
      make (&a, sa);
      make (&b, sb);
      if (below_2_128 (sa.mag) && below_2_128 (sb.mag))
	{
	  word_domain++;
	}
      else
	{
	  wide++;
	}
      for (int op = 0; op < OP_N; op++)
	{
	  const std::string res = run_op (op, &a, &b);
	  digest[op] = fnv (digest[op], res);
	  block[op] = fnv (block[op], res);
	  if (res.find ("ret=0 err=-1373 NULL") != std::string::npos)
	    {
	      null_no_error[op]++;
	    }
	  else if (res.find ("ret=-427") != std::string::npos)
	    {
	      overflow[op]++;
	    }
	  if (out != NULL && dump_op == op && pair / block_size == dump_block)
	    {
	      std::fprintf (out, "%s %ld %s %s -> %s\n", op_name[op], pair, spec_text (sa).c_str (),
			    spec_text (sb).c_str (), res.c_str ());
	    }
	}
      if ((pair + 1) % block_size == 0 || pair + 1 == total)
	{
	  for (int op = 0; op < OP_N; op++)
	    {
	      if (out != NULL && dump_op < 0)
		{
		  std::fprintf (out, "%s %ld %016llx\n", op_name[op], pair / block_size, (unsigned long long) block[op]);
		}
	      block[op] = 0xcbf29ce484222325ULL;
	    }
	}
    }
  if (out != NULL)
    {
      std::fclose (out);
    }
  for (int op = 0; op < OP_N; op++)
    {
      char hex[32];
      std::snprintf (hex, sizeof (hex), "0x%016llxULL", (unsigned long long) digest[op]);
      if (std::getenv ("UNITSEAM_VERBOSE") != NULL || digest[op] != EXPECTED[op])
	{
	  WARN (op_name[op] << " digest " << hex << " (pairs " << total << ": both coefficients below 2^128 "
		<< word_domain << ", wider " << wide << "; NULL with NO_ERROR " << null_no_error[op] << ", overflow "
		<< overflow[op] << ")");
	}
      CHECK (digest[op] == EXPECTED[op]);
    }
}

/* ---------------------------------------------------------------- develop's NULL below scale -214 */

static SPEC
spec_of (unsigned __int128 coef, unsigned char top, int scale, bool neg)
{
  SPEC s;
  s.mag = mag_from_u128 (coef, top);
  s.prec = mag_digits (s.mag);
  s.scale = scale;
  s.neg = neg;
  s.is_float = true;
  return s;
}

TEST_CASE ("W2 develop (not this PR): a product of two coefficients below 2^64 whose result scale is below -214 -- "
	   "1e200 x 1e100 as (1, -200) x (1, -100), (2, -150) x (2^63, -131) -- is NULL with NO_ERROR and ER_INVALID_SCALE "
	   "set (the 64-bit fast path, db_make_numeric () refusing the scale), where a coefficient past 2^64 -- "
	   "(2^64+1, -200) x (1, -100) -- gets ER_IT_DATA_OVERFLOW (PASS = defect present)", "[defect]")
{
  const unsigned __int128 one = 1;
  DB_VALUE a, b, r;
  int ret;

  make (&a, spec_of (1, 0, -200, false));
  make (&b, spec_of (1, 0, -100, false));
  db_make_null (&r);
  er_clear ();
  ret = float_numeric_db_value_mul (&a, &b, &r);
  INFO (result_text (ret, &r));
  REQUIRE (ret == NO_ERROR);
  REQUIRE (DB_IS_NULL (&r));
  REQUIRE (er_errid () == ER_INVALID_SCALE);

  make (&a, spec_of (2, 0, -150, true));
  make (&b, spec_of (one << 63, 0, -131, false));
  db_make_null (&r);
  er_clear ();
  ret = float_numeric_db_value_mul (&a, &b, &r);
  INFO (result_text (ret, &r));
  REQUIRE (ret == NO_ERROR);
  REQUIRE (DB_IS_NULL (&r));

  make (&a, spec_of ((one << 64) + 1, 0, -200, false));
  make (&b, spec_of (1, 0, -100, false));
  db_make_null (&r);
  er_clear ();
  ret = float_numeric_db_value_mul (&a, &b, &r);
  INFO (result_text (ret, &r));
  REQUIRE (ret == ER_IT_DATA_OVERFLOW);
  REQUIRE (DB_IS_NULL (&r));

  /* in range: -214 itself is a value */
  make (&a, spec_of (1, 0, -107, false));
  make (&b, spec_of (1, 0, -107, false));
  db_make_null (&r);
  er_clear ();
  ret = float_numeric_db_value_mul (&a, &b, &r);
  INFO (result_text (ret, &r));
  REQUIRE (ret == NO_ERROR);
  REQUIRE (!DB_IS_NULL (&r));
  REQUIRE (db_get_numeric_scale (&r, NULL) == -214);
}
