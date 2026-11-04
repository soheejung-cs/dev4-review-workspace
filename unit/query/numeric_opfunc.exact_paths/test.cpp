/*
 * PR#8349 (CBRD-27819) -- the 128-bit and exact paths the PR puts in front of develop's NUMERIC code, against
 * develop's answer for the same inputs (server mode; src/query/numeric_opfunc.c, arithmetic.c, domain_rules.c).
 * The PR's claim: the value, type, precision, scale and error of every result stay what they were.
 *
 *   X  numeric_db_value_compare: head compares equal scales at any two precisions by the coefficients, and scales
 *      at most 38 apart with both coefficients below 2^128 by a u128 product (an overflowing product is the larger);
 *      develop sent every pair of other precisions or scales to float_numeric_compare. Expected: the exact order
 *      (a decimal oracle, no engine code) and float_numeric_compare itself (static in numeric_opfunc.c, unchanged
 *      by the PR: develop's general path, still in the head binary)
 *   I  SHORT / INT / BIGINT against NUMERIC: head compares them exactly (numeric_compare_bigint_num_exact, chosen
 *      per type pair by domain_compare_cmp, no conversion); develop coerced the integer into NUMERIC(40,0)
 *      (tp_value_compare_common_domain: NUMERIC ranks above the integers). Expected: the exact order from
 *      tp_value_compare_with_error (develop's row comparison), the resolved comparison (domain_resolve_comparison
 *      + domain_compare_values), the key decision (domain_compare_decided) and its B-tree mirror (domain_compare_mirror)
 *   R  ROUND / TRUNC of a NUMERIC (db_round_dbval / db_trunc_dbval): head tries numeric_db_value_round_to_place
 *      first (coefficient below 2^128, scale >= 0); develop rounds the zero-padded decimal string. Expected:
 *      develop_round below -- develop's string code as a model, which the base run checks against develop itself
 *   F  FLOOR / CEIL (db_floor_dbval / db_ceil_dbval): head tries numeric_db_value_floor_ceil (scale 1..38).
 *      Expected: develop_floor_ceil (develop's string code as a model, the precision growth of the carry included)
 *   H  registered only where the PR's functions link: numeric_compare_bigint_num_exact, numeric_db_value_round_to_place
 *      and numeric_db_value_floor_ceil are taken for the inputs their comments promise and give the develop answer
 *      there -- so a head PASS of I / R / F ran the new code, not only the decimal fallback
 *   (not here) a place the decimal string cannot reach -- a DOUBLE place past BIGINT or NaN, a BIGINT place below
 *      -2^47 -- where head gives 0 and develop's wrapped pointer the value: query/arithmetic.numeric_round_place
 *      R1a / R1b (PR#8349 A-01). The R contracts keep their places within [-10^12, BIGINT max]
 *
 * Real code at THIS revision compiled into the test binary (#included as in domain_rules.decided_compare):
 * system_parameter.c, language_support.c, intl_support.c, chartype.c, mprec.c, dtoa.c, object_primitive.c,
 * object_domain.c, object_domain_convert.cpp, numeric_opfunc.c, db_date.c, db_macro.c, string_opfunc.c; arithmetic.c
 * is linked as a separately compiled TU (suite.json extra). The target TU is domain_rules.c. The fakes are the boot
 * fakes of decided_compare. UNITSEAM_VERBOSE=1 prints the coverage counters.
 */
#include "catch.hpp"
/* first: mprec.h (via the sources below) defines cmp, a member name of DOMAIN_COMPARE */
#include "domain_rules.h"
#include "system_parameter.c"
#undef new
#include "language_support.c"
#undef new
#include "intl_support.c"
#undef new
#include "chartype.c"
#undef new
#include "mprec.c"
#undef new
#include "dtoa.c"
#undef new
#include "object_primitive.c"
#undef new
#include "object_domain.c"
#undef new
#include "object_domain_convert.cpp"
#undef new
#include "numeric_opfunc.c"
#undef new
#include "db_date.c"
#undef new
#include "db_macro.c"
#undef new
#include "string_opfunc.c"
#undef new
#undef cmp
#include "boot_sr.h"
#include "arithmetic.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------- fakes: what a booted server process has */

/* system_parameter.c's static initializers (prm_Def's parallelism defaults, the regex engine keyword table) */
namespace cubthread
{
  std::size_t system_core_count (void)
  {
    return 8;
  }
}
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this. DOWN makes prm_get_value read a session parameter's global
 * value - its default here - instead of a session's copy, which needs a thread entry */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c:494 body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* tp_init () before 5de66400a (#8104) names the deduplicate-key attributes; nothing under test reads them */
void
dk_deduplicate_key_attribute_initialized ()
{
}

/* error_manager.c: the last error id er_set raised, as er_errid () reads it; er_clear () resets it */
static int fake_er_id = NO_ERROR;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  fake_er_id = err_id;
}

int
er_errid (void)
{
  return fake_er_id;
}

void
er_clear (void)
{
  fake_er_id = NO_ERROR;
}

/* memory_alloc.c: the private heap of a thread is malloc here */
void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  free (ptr);
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  return realloc (ptr, size);
}

/* area_alloc.c: tp_init's domain area hands out zeroed element-sized blocks */
struct fake_area
{
  size_t element_size;
};
AREA *
area_create (const char *, size_t element_size, size_t)
{
  return (AREA *) new fake_area { element_size };
}

void *
area_alloc (AREA * area)
{
  return calloc (1, ((fake_area *) area)->element_size);
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

/* The boot order of a server (boot_restart_server): built-in collations, the type module, then the database's
 * charset and language - en_US.utf8 here */
static bool
boot_types ()
{
  static bool done = false;
  static bool ok = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      ok = tp_init () == NO_ERROR;
      lang_Initialized = true;
      ok = ok && lang_set_charset_lang ("en_US.utf8") == NO_ERROR;
    }
  return ok;
}

static bool
verbose ()
{
  const char *v = getenv ("UNITSEAM_VERBOSE");
  return v != NULL && *v == '1';
}

/* ---------------------------------------------------------------- the PR's API, where this revision has it */

/* ::name inside a template: qualified lookup consults the using-directive only when the global namespace declares no
 * such name -- the real function where it exists (head), the fallback returning absent_t where it does not (develop).
 * The device of compat/db_vdb.kept_tree_registry and optimizer/histogram_cl.char_keys. */
namespace unitseam_absent
{
  struct absent_t
  {
  };
  template <typename... A> absent_t numeric_compare_bigint_num_exact (A &&...);
  template <typename... A> absent_t numeric_db_value_round_to_place (A &&...);
  template <typename... A> absent_t numeric_db_value_floor_ceil (A &&...);
  template <typename... A> absent_t domain_compare_cmp (A &&...);
}
using namespace unitseam_absent;

template <typename B, typename V>
static constexpr bool
has_exact_t ()
{
  return !std::is_same<decltype (::numeric_compare_bigint_num_exact (std::declval<B> (), std::declval<V> ())),
		       unitseam_absent::absent_t>::value;
}

template <typename V, typename B, typename R>
static constexpr bool
has_round_t ()
{
  return !std::is_same<decltype (::numeric_db_value_round_to_place (std::declval<V> (), std::declval<B> (), true,
								     std::declval<R> ())), unitseam_absent::absent_t>::value;
}

template <typename V, typename R>
static constexpr bool
has_floor_ceil_t ()
{
  return !std::is_same<decltype (::numeric_db_value_floor_ceil (std::declval<V> (), true, std::declval<R> ())),
		       unitseam_absent::absent_t>::value;
}

template <typename T>
static constexpr bool
has_cmp_t ()
{
  return !std::is_same<decltype (::domain_compare_cmp (std::declval<T> (), std::declval<T> (), std::declval<T> ())),
		       unitseam_absent::absent_t>::value;
}

static const bool HAS_EXACT = has_exact_t<DB_BIGINT, const DB_VALUE *> ();
static const bool HAS_ROUND = has_round_t<const DB_VALUE *, DB_BIGINT, DB_VALUE *> ();
static const bool HAS_FLOOR_CEIL = has_floor_ceil_t<const DB_VALUE *, DB_VALUE *> ();
static const bool HAS_CMP = has_cmp_t<DB_TYPE> ();

template <typename B, typename V>
static int
call_exact_t (B value, V num)
{
  if constexpr (has_exact_t<B, V> ())
    {
      return (int) ::numeric_compare_bigint_num_exact (value, num);
    }
  else
    {
      return DB_UNK;
    }
}

template <typename V, typename B, typename R>
static bool
call_round_t (V value, B place, bool half_up, R result)
{
  if constexpr (has_round_t<V, B, R> ())
    {
      return ::numeric_db_value_round_to_place (value, place, half_up, result);
    }
  else
    {
      return false;
    }
}

template <typename V, typename R>
static bool
call_floor_ceil_t (V value, bool is_floor, R result)
{
  if constexpr (has_floor_ceil_t<V, R> ())
    {
      return ::numeric_db_value_floor_ceil (value, is_floor, result);
    }
  else
    {
      return false;
    }
}

template <typename T>
static const PR_TYPE *
call_cmp_t (T source0, T source1, T compared)
{
  if constexpr (has_cmp_t<T> ())
    {
      return ::domain_compare_cmp (source0, source1, compared);
    }
  else
    {
      return NULL;
    }
}

/* a Catch2 case registered only when the PR's functions link (what TEST_CASE expands to, behind a condition) */
#define HEAD_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = HAS_EXACT && HAS_ROUND && HAS_FLOOR_CEIL && HAS_CMP \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* ---------------------------------------------------------------- decimal arithmetic of the oracle (no engine code) */

static std::string
dec_strip (const std::string & d)
{
  size_t i = 0;
  while (i + 1 < d.size () && d[i] == '0')
    {
      i++;
    }
  return d.substr (i);
}

static std::string
dec_strip_trailing (const std::string & d)
{
  size_t n = d.size ();
  while (n > 1 && d[n - 1] == '0')
    {
      n--;
    }
  return d.substr (0, n);
}

static int
dec_cmp (const std::string & a, const std::string & b)
{
  if (a.size () != b.size ())
    {
      return a.size () < b.size () ? -1 : 1;
    }
  const int c = a.compare (b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

static std::string
dec_add (const std::string & a, const std::string & b)
{
  std::string r;
  int carry = 0;
  for (size_t i = 0; i < std::max (a.size (), b.size ()) || carry != 0; i++)
    {
      int s = carry;
      if (i < a.size ())
	{
	  s += a[a.size () - 1 - i] - '0';
	}
      if (i < b.size ())
	{
	  s += b[b.size () - 1 - i] - '0';
	}
      r.push_back ((char) ('0' + s % 10));
      carry = s / 10;
    }
  std::reverse (r.begin (), r.end ());
  return dec_strip (r);
}

/* a - b, a >= b */
static std::string
dec_sub (const std::string & a, const std::string & b)
{
  std::string r;
  int borrow = 0;
  for (size_t i = 0; i < a.size (); i++)
    {
      int s = (a[a.size () - 1 - i] - '0') - borrow - (i < b.size () ? b[b.size () - 1 - i] - '0' : 0);
      borrow = s < 0;
      r.push_back ((char) ('0' + (s + 10) % 10));
    }
  std::reverse (r.begin (), r.end ());
  return dec_strip (r);
}

static std::string
dec_pow10 (int k)
{
  return "1" + std::string (k, '0');
}

static std::string
dec_shift (const std::string & d, int k)
{
  return d == "0" ? d : d + std::string (k, '0');
}

static std::string
dec_of_u64 (unsigned long long u)
{
  return std::to_string (u);
}

/* the 17-byte big-endian magnitude (DB_NUMERIC::d.buf) of a decimal; false past 2^136 */
static bool
dec_to_buf (const std::string & d, unsigned char *buf)
{
  memset (buf, 0, DB_NUMERIC_BUF_SIZE);
  for (char ch : d)
    {
      int carry = ch - '0';
      for (int i = DB_NUMERIC_BUF_SIZE - 1; i >= 0; i--)
	{
	  const int x = buf[i] * 10 + carry;
	  buf[i] = (unsigned char) (x & 0xff);
	  carry = x >> 8;
	}
      if (carry != 0)
	{
	  return false;
	}
    }
  return true;
}

static std::string
buf_to_dec (const unsigned char *src)
{
  unsigned char b[DB_NUMERIC_BUF_SIZE];
  memcpy (b, src, DB_NUMERIC_BUF_SIZE);
  std::string r;
  bool zero;
  do
    {
      int rem = 0;
      zero = true;
      for (int i = 0; i < DB_NUMERIC_BUF_SIZE; i++)
	{
	  const int x = rem * 256 + b[i];
	  b[i] = (unsigned char) (x / 10);
	  rem = x % 10;
	  zero = zero && b[i] == 0;
	}
      r.push_back ((char) ('0' + rem));
    }
  while (!zero);
  std::reverse (r.begin (), r.end ());
  return dec_strip (r);
}

static const std::string TWO64 = "18446744073709551616";
static const std::string TWO127 = "170141183460469231731687303715884105728";
static const std::string TWO128 = "340282366920938463463374607431768211456";

static bool
below_2_128 (const std::string & mag)
{
  return dec_cmp (mag, TWO128) < 0;
}

/* ---------------------------------------------------------------- values */

/* a NUMERIC value made from its decimal parts, with the oracle's keys */
struct numv
{
  std::string name;
  std::string mag;		/* coefficient, decimal */
  int p, s;
  bool neg, is_float;
  DB_VALUE v;
  int sign;			/* -1, 0, 1 */
  long exp10;			/* digits - scale: the value is 0.d1d2.. x 10^exp10 */
  std::string digits;		/* the coefficient without trailing zeros */
};

static numv
make_num (const std::string & mag_in, int p, int s, bool neg, bool is_float)
{
  numv n;
  n.mag = dec_strip (mag_in);
  n.p = p;
  n.s = s;
  n.neg = neg && n.mag != "0";
  n.is_float = is_float;
  unsigned char buf[DB_NUMERIC_BUF_SIZE];
  REQUIRE (dec_to_buf (n.mag, buf));
  db_make_null (&n.v);
  er_clear ();
  REQUIRE (db_make_numeric (&n.v, buf, p, s, DB_NUMERIC_BUF_SIZE, n.neg, is_float) == NO_ERROR);
  REQUIRE (er_errid () == NO_ERROR);
  int rp = 0, rs = 0;
  bool rf = false;
  db_get_numeric_precision_and_scale (&n.v, &rp, &rs, &rf);
  REQUIRE ((rp == p && rs == s && rf == is_float));
  n.sign = n.mag == "0" ? 0 : n.neg ? -1 : 1;
  n.exp10 = (long) n.mag.size () - s;
  n.digits = dec_strip_trailing (n.mag);
  n.name = std::string (n.neg ? "-" : "") + n.mag + "e" + std::to_string (-s) + " (p" + std::to_string (p)
    + (is_float ? " float)" : " fixed)");
  return n;
}

/* the forms the engine gives a coefficient at a scale: a float NUMERIC (its digit count as precision, what the
 * arithmetic makes), the fixed NUMERIC(p, s) of the fewest digits and NUMERIC(38, s) (a column's) */
static void
add_forms (std::vector<numv> &out, const std::string & mag, int s, bool both_signs)
{
  const int digits = (int) dec_strip (mag).size ();
  for (int sign = 0; sign < (both_signs ? 2 : 1); sign++)
    {
      const bool neg = sign == 1;
      if (neg && dec_strip (mag) == "0")
	{
	  continue;
	}
      if (digits <= DB_MAX_NUMERIC_PRECISION)
	{
	  out.push_back (make_num (mag, std::max (1, digits), s, neg, true));
	}
      const int p_min = std::max (std::max (digits, s), 1);
      if (s >= 0 && p_min <= 38)	/* a column's NUMERIC precision is at most 38 */
	{
	  out.push_back (make_num (mag, p_min, s, neg, false));
	}
      if (s >= 0 && s <= 38 && digits <= 38 && p_min != 38)
	{
	  out.push_back (make_num (mag, 38, s, neg, false));
	}
    }
}

static int
exact_order (const numv & a, const numv & b)
{
  if (a.sign != b.sign)
    {
      return a.sign < b.sign ? -1 : 1;
    }
  if (a.sign == 0)
    {
      return 0;
    }
  int c;
  if (a.exp10 != b.exp10)
    {
      c = a.exp10 < b.exp10 ? -1 : 1;
    }
  else
    {
      const int x = a.digits.compare (b.digits);
      c = x < 0 ? -1 : x > 0 ? 1 : 0;
    }
  return a.sign < 0 ? -c : c;
}

/* ---------------------------------------------------------------- results */

struct num_out
{
  int rc, er;
  int type;
  bool is_null, is_float, neg;
  int p, s;
  std::string mag;
};

static num_out
read_out (int rc, int er, const DB_VALUE & r)
{
  num_out o {};
  o.rc = rc;
  o.er = er;
  if (rc != NO_ERROR)
    {
      return o;
    }
  o.type = (int) DB_VALUE_DOMAIN_TYPE (&r);
  o.is_null = DB_IS_NULL (&r);
  if (o.type != DB_TYPE_NUMERIC || o.is_null)
    {
      return o;
    }
  db_get_numeric_precision_and_scale (&r, &o.p, &o.s, &o.is_float);
  o.neg = DB_VALUE_NUMERIC_IS_VALUE_NEGATIVE (&r);
  o.mag = buf_to_dec ((const unsigned char *) db_locate_numeric (&r));
  return o;
}

static bool
same_out (const num_out & a, const num_out & b)
{
  if (a.rc != b.rc || a.er != b.er)
    {
      return false;
    }
  if (a.rc != NO_ERROR)
    {
      return true;
    }
  return a.type == b.type && a.is_null == b.is_null && a.is_float == b.is_float && a.neg == b.neg && a.p == b.p
    && a.s == b.s && a.mag == b.mag;
}

static std::string
show_out (const num_out & o)
{
  if (o.rc != NO_ERROR)
    {
      return "rc " + std::to_string (o.rc) + " er " + std::to_string (o.er);
    }
  if (o.type != DB_TYPE_NUMERIC || o.is_null)
    {
      return "type " + std::to_string (o.type) + (o.is_null ? " NULL" : "");
    }
  return std::string (o.neg ? "-" : "") + o.mag + "e" + std::to_string (-o.s) + " (p" + std::to_string (o.p)
    + (o.is_float ? " float)" : " fixed)") + (o.er != NO_ERROR ? " er " + std::to_string (o.er) : "");
}

/* a bounded list of mismatches for the report */
struct mismatches
{
  int count = 0;
  std::string first;
  void add (const std::string & what)
  {
    if (count++ < 12)
      {
	first += what + "\n";
      }
  }
};

/* ---------------------------------------------------------------- develop's ROUND / TRUNC / FLOOR / CEIL (model) */

/* db_round_dbval / db_trunc_dbval at 80b84317d (arithmetic.c:2518-2623, 3547-3576), on the value's decimal string
 * numeric_coerce_num_to_dec_str writes: a '-' for a negative value, then TWICE_NUM_MAX_PREC digits, zero padded.
 *   place > scale            the value itself (*result = *value1)
 *   scale - place > length   zero (the string is cleared)
 *   otherwise                the scale - place lowest digits become 0; ROUND adds 10^(scale - place) when the first
 *                            dropped digit is 5 or more; a round up that grows the value past 40 digits gives the
 *                            last digit (a 0) to the scale: scale - 1, precision 40, ER_IT_DATA_OVERFLOW below -214
 *   result                   a float NUMERIC of the value's precision and scale, negative when not zero */
static num_out
develop_round (const numv & n, long long place, bool half_up)
{
  num_out o {};
  o.rc = o.er = NO_ERROR;
  o.type = DB_TYPE_NUMERIC;
  if (place > (long long) n.s)
    {
      o.is_float = n.is_float;
      o.p = n.p;
      o.s = n.s;
      o.neg = n.neg;
      o.mag = n.mag;
      return o;
    }
  const long long cleared = (long long) n.s - place;
  const long long length = TWICE_NUM_MAX_PREC + (n.neg ? 1 : 0);
  const long long len = (long long) n.mag.size ();
  std::string m;
  int p = n.p, s = n.s;
  if (cleared > length)
    {
      m = "0";
    }
  else if (cleared == 0)
    {
      m = n.mag;
    }
  else
    {
      const int first = cleared <= len ? n.mag[len - cleared] - '0' : 0;
      m = cleared < len ? dec_strip (n.mag.substr (0, len - cleared) + std::string ((size_t) cleared, '0')) : "0";
      if (half_up && first >= 5)
	{
	  m = dec_add (m, dec_pow10 ((int) cleared));
	  if ((int) m.size () > DB_MAX_NUMERIC_PRECISION)
	    {
	      s--;
	      p = DB_MAX_NUMERIC_PRECISION;
	      m = m.substr (0, m.size () - 1);
	      if (s < DB_MIN_NUMERIC_SCALE)
		{
		  o.rc = o.er = ER_IT_DATA_OVERFLOW;
		  return o;
		}
	    }
	}
    }
  o.is_float = true;
  o.p = p;
  o.s = s;
  o.neg = n.neg && m != "0";
  o.mag = m;
  return o;
}

/* db_floor_dbval / db_ceil_dbval at 80b84317d (arithmetic.c:144-240, 313-419):
 *   scale 0        the value as a float NUMERIC (precision kept)
 *   scale < 0      the value as a float NUMERIC (the fraction loop starts past the string's end)
 *   scale > 0      the fraction digits become 0; FLOOR of a negative and CEIL of a positive value with a nonzero
 *                  fraction add one at the units digit, and when the carry ends at digit d >= precision (d counted
 *                  from the right, the units digit at the scale) the precision becomes d + 1, at most 40; a scale
 *                  above 40 becomes 39 after the fraction is cleared (DB_MAX_NUMERIC_PRECISION - 1)
 *   result         a float NUMERIC, negative when not zero */
static num_out
develop_floor_ceil (const numv & n, bool is_floor)
{
  num_out o {};
  o.rc = o.er = NO_ERROR;
  o.type = DB_TYPE_NUMERIC;
  o.is_float = true;
  o.p = n.p;
  if (n.s <= 0)
    {
      o.s = n.s;
      o.neg = n.neg;
      o.mag = n.mag;
      return o;
    }
  const int s0 = n.s;
  const int s_out = s0 > DB_MAX_NUMERIC_PRECISION ? DB_MAX_NUMERIC_PRECISION - 1 : s0;
  const int len = (int) n.mag.size ();
  const std::string integral = len > s0 ? n.mag.substr (0, len - s0) : "0";
  bool fraction = false;
  for (int i = std::max (0, len - s0); i < len; i++)
    {
      fraction = fraction || n.mag[i] != '0';
    }
  const bool away = fraction && (is_floor ? n.neg : !n.neg);
  int p = n.p;
  std::string m;
  if (away)
    {
      int d = s_out;
      for (int i = (int) integral.size () - 1; i >= 0 && integral[i] == '9'; i--)
	{
	  d++;
	}
      if (d >= p)
	{
	  p = std::min (d + 1, DB_MAX_NUMERIC_PRECISION);
	}
      m = dec_shift (dec_add (integral, "1"), s_out);
    }
  else
    {
      m = integral == "0" ? "0" : dec_shift (integral, s0);
    }
  o.p = p;
  o.s = s_out;
  o.neg = n.neg && m != "0";
  o.mag = m;
  return o;
}

/* the inputs numeric_db_value_round_to_place's comment promises to take: a coefficient below 2^128, a scale of 0 or
 * more, unless the round up passes 2^128 */
static bool
round_taken (const numv & n, long long place, bool half_up)
{
  if (!below_2_128 (n.mag) || n.s < 0)
    {
      return false;
    }
  if (place > (long long) n.s || place < (long long) n.s - 38)
    {
      return true;
    }
  const int cleared = (int) (n.s - place);
  const int len = (int) n.mag.size ();
  if (!half_up || cleared == 0 || cleared > len)
    {
      return true;
    }
  const int first = n.mag[len - cleared] - '0';
  if (first < 5)
    {
      return true;
    }
  const std::string trunc = cleared < len ? dec_strip (n.mag.substr (0, len - cleared) + std::string (cleared, '0'))
    : "0";
  return below_2_128 (dec_add (trunc, dec_pow10 (cleared)));
}

/* numeric_db_value_floor_ceil's: a coefficient below 2^128 and a scale from 1 to 38, unless the carry passes 2^128 */
static bool
floor_ceil_taken (const numv & n, bool is_floor)
{
  if (!below_2_128 (n.mag) || n.s < 1 || n.s > 38)
    {
      return false;
    }
  const num_out d = develop_floor_ceil (n, is_floor);
  return below_2_128 (d.mag);
}

/* ---------------------------------------------------------------- grids */

static std::vector<std::string>
compare_coefficients ()
{
  return {
    "0", "1", "4", "5", "9", "15", "99", "12345",
    "9223372036854775807", "9223372036854775808", "18446744073709551615", "10000000000000000000",
    "34028236692093846346", "34028236692093846347",
    TWO127, "340282366920938463463374607431768211455", TWO128,
    "99999999999999999999999999999999999999", "100000000000000000000000000000000000000",
    "9999999999999999999999999999999999999999"
  };
}

static std::vector<int>
compare_scales ()
{
  return { 0, 1, 2, 19, 20, 38, 39, 40, 252, -1, -19, -214 };
}

static const std::vector<numv> &
compare_grid ()
{
  static std::vector<numv> g;
  if (g.empty ())
    {
      for (const std::string & c : compare_coefficients ())
	{
	  for (int s : compare_scales ())
	    {
	      add_forms (g, c, s, true);
	    }
	}
    }
  return g;
}

/* ROUND / TRUNC / FLOOR / CEIL inputs: digits that round at .5 either way, nines that carry, the 2^64, 2^128 and
 * 10^38..10^40 edges, every scale class the paths tell apart */
static const std::vector<numv> &
round_grid ()
{
  static std::vector<numv> g;
  if (g.empty ())
    {
      const std::vector<std::string> cs = {
	"0", "1", "4", "5", "9", "14", "15", "25", "45", "49", "50", "55", "94", "95", "99", "149", "150", "994",
	"995", "999", "12345", "99999", "9223372036854775807", "999999999999999999999",
	"99999999999999999999999999999999999999", TWO127, "340282366920938463463374607431768211455",
	"340282366920938463463374607431768211449", "339999999999999999999999999999999999999", TWO128,
	"999999999999999999999999999999999999999", "9999999999999999999999999999999999999999"
      };
      const std::vector<int> ss = { 0, 1, 2, 3, 5, 19, 20, 37, 38, 39, 40, 41, 100, 252, -1, -3, -213, -214 };
      for (const std::string & c : cs)
	{
	  for (int s : ss)
	    {
	      add_forms (g, c, s, true);
	    }
	}
    }
  return g;
}

static std::vector<long long>
places_for (int s)
{
  std::vector<long long> p = { -1000000000000LL, -300, -258, -257, -256, -255, -100, -60, -41, -40, -39, -38, -21,
    -20, -19, -3, -2, -1, 0, 1, 2, 3, 300, DB_BIGINT_MAX
  };
  for (int d = -41; d <= 2; d++)
    {
      p.push_back ((long long) s + d);
    }
  std::sort (p.begin (), p.end ());
  p.erase (std::unique (p.begin (), p.end ()), p.end ());
  return p;
}

/* ---------------------------------------------------------------- comparisons of two values */

struct outcome
{
  int result;
  bool can_compare;
  int error;
};

static std::string
show (const outcome & o)
{
  const int r = o.result;
  return std::string (r == DB_EQ ? "EQ" : r == DB_LT ? "LT" : r == DB_GT ? "GT" : "UNK") + "/"
    + (o.can_compare ? "comparable" : "not comparable") + "/er " + std::to_string (o.error);
}

static int
reversed_result (int r)
{
  return r == DB_LT ? DB_GT : r == DB_GT ? DB_LT : r;
}

static outcome
by_row (const DB_VALUE * a, const DB_VALUE * b)
{
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = tp_value_compare_with_error (a, b, 1, 0, &o.can_compare);
  o.error = er_errid ();
  return o;
}

static outcome
by_resolved (const DB_VALUE * a, const DB_VALUE * b)
{
  DOMAIN_COMPARE_KEY ka, kb;
  DOMAIN_COMPARE c;
  domain_compare_key_of_value (a, &ka);
  domain_compare_key_of_value (b, &kb);
  REQUIRE (domain_resolve_comparison (&ka, &kb, &c) == NO_ERROR);
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = domain_compare_values (&c, a, b, 0, &o.can_compare);
  o.error = er_errid ();
  return o;
}

/* a key comparison as XASL generation streams it (pt_decide_key_compare's normalization: nothing converted, the first
 * value's type first) -- as domain_rules.decided_compare's key_decision */
static DOMAIN_COMPARE
key_decision (const DB_VALUE * a, const DB_VALUE * b)
{
  DOMAIN_COMPARE_KEY ka, kb;
  DOMAIN_COMPARE d;
  domain_compare_key_of_value (a, &ka);
  domain_compare_key_of_value (b, &kb);
  REQUIRE (domain_resolve_comparison (&ka, &kb, &d) == NO_ERROR);
  if (d.conv[0] == NULL && d.conv[1] == NULL)
    {
      d.first = 0;
      d.converted_first = d.source[0];
    }
  return d;
}

static outcome
by_decision (const DOMAIN_COMPARE & d, const DB_VALUE * a, const DB_VALUE * b)
{
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = domain_compare_decided (&d, a, b, &o.can_compare);
  o.error = er_errid ();
  return o;
}

/* ---------------------------------------------------------------- integers */

struct intv
{
  std::string name;
  DB_VALUE v;
  DB_BIGINT i;
  numv as_num;			/* the oracle's key of the integer: coefficient |i|, scale 0 */
};

static intv
make_int (DB_TYPE t, DB_BIGINT i)
{
  intv r;
  r.i = i;
  db_make_null (&r.v);
  if (t == DB_TYPE_SHORT)
    {
      db_make_short (&r.v, (short) i);
    }
  else if (t == DB_TYPE_INTEGER)
    {
      db_make_int (&r.v, (int) i);
    }
  else
    {
      db_make_bigint (&r.v, i);
    }
  const unsigned long long mag = i < 0 ? 0ULL - (unsigned long long) i : (unsigned long long) i;
  r.as_num = make_num (dec_of_u64 (mag), std::max (1, (int) dec_of_u64 (mag).size ()), 0, i < 0, true);
  r.name = std::string (pr_type_name (t)) + " " + std::to_string (i);
  return r;
}

static const std::vector<intv> &
int_grid ()
{
  static std::vector<intv> g;
  if (g.empty ())
    {
      for (DB_BIGINT i : std::vector<DB_BIGINT> { 0, 1, -1, 7, 32767, -32768 })
	{
	  g.push_back (make_int (DB_TYPE_SHORT, i));
	}
      for (DB_BIGINT i : std::vector<DB_BIGINT> { 0, 1, -1, 7, 2147483647, -2147483648LL })
	{
	  g.push_back (make_int (DB_TYPE_INTEGER, i));
	}
      for (DB_BIGINT i : std::vector<DB_BIGINT> { 0, 1, -1, 7, DB_BIGINT_MAX, -DB_BIGINT_MAX, DB_BIGINT_MIN,
	   1000000000000000000LL, -1000000000000000000LL, 9223372036854775806LL, 1844674407370955161LL,
	   3402823669209384634LL, 3402823669209384635LL, -3402823669209384635LL })
	{
	  g.push_back (make_int (DB_TYPE_BIGINT, i));
	}
    }
  return g;
}

/* NUMERIC values for the integers: each integer's magnitude at scales 0, 1, 19, 20 and 38 (equal), one unit of the
 * last digit above and below, both signs, float and fixed; and the edges the exact comparison's paths tell apart --
 * the u128 fast path (scale 0..19, coefficient below 2^128) and the wide one (a scale past 19, a coefficient at or
 * past 2^128, a negative scale) */
static const std::vector<numv> &
int_numeric_grid ()
{
  static std::vector<numv> g;
  if (g.empty ())
    {
      std::vector<std::string> mags = { "0", "1", "7", "32767", "32768", "2147483647", "2147483648",
	"9223372036854775807", "9223372036854775808", "9223372036854775806", "1000000000000000000",
	"1844674407370955161", "3402823669209384634", "3402823669209384635"
      };
      for (const std::string & m : mags)
	{
	  for (int s : { 0, 1, 19, 20, 38 })
	    {
	      const std::string eq = dec_shift (m, s);
	      if (eq.size () > DB_MAX_NUMERIC_PRECISION)
		{
		  continue;
		}
	      add_forms (g, eq, s, true);
	      add_forms (g, dec_add (eq, "1"), s, true);
	      if (eq != "0")
		{
		  add_forms (g, dec_sub (eq, "1"), s, true);
		}
	    }
	}
      for (int s : { 0, 1, 18, 19, 20, 21, 38, 39 })
	{
	  add_forms (g, "340282366920938463463374607431768211455", s, true);
	  add_forms (g, TWO128, s, true);
	}
      add_forms (g, "99999999999999999999999999999999999999", 0, true);
      add_forms (g, "99999999999999999999999999999999999999", 38, true);
      add_forms (g, "5", 1, true);
      add_forms (g, "1", -19, true);
      add_forms (g, "9", -18, true);
      add_forms (g, "92233720368547758", -2, true);
      add_forms (g, "922337203685477581", -1, true);
      add_forms (g, "922337203685477580", -1, true);
      add_forms (g, "1", 252, true);
      add_forms (g, "9999999999999999999999999999999999999999", -214, true);
      add_forms (g, "9999999999999999999999999999999999999999", 21, true);
    }
  return g;
}

/* ================================================================ guard */

TEST_CASE ("PR8349 API guard: numeric_compare_bigint_num_exact, numeric_db_value_round_to_place, "
	   "numeric_db_value_floor_ceil and domain_compare_cmp link all or none - the H cases are registered only when "
	   "all of them do", "[contract]")
{
  const int n = HAS_EXACT + HAS_ROUND + HAS_FLOOR_CEIL + HAS_CMP;
  WARN ("PR#8349 API at this revision: " << n << " of 4" << (n == 4 ? " - H cases registered"
							      : n == 0 ? " - H cases not applicable (develop)" : ""));
  CHECK ((n == 0 || n == 4));
}

/* ================================================================ X numeric_db_value_compare */

TEST_CASE ("X1 numeric_db_value_compare answers the exact order of every pair of a NUMERIC grid (float and fixed, "
	   "scales -214..252, coefficients 0..10^40-1 across 2^63, 2^64, 2^127, 2^128) as an INTEGER -1, 0 or 1",
	   "[contract]")
{
  REQUIRE (boot_types ());
  const std::vector<numv> &g = compare_grid ();
  mismatches bad;
  long pairs = 0, same_scale_other_precision = 0, u128_scaled = 0, overflow_side = 0;
  for (const numv & a : g)
    {
      for (const numv & b : g)
	{
	  DB_VALUE ans;
	  db_make_null (&ans);
	  const int rc = numeric_db_value_compare (&a.v, &b.v, &ans);
	  const int want = exact_order (a, b);
	  const bool ok = rc == NO_ERROR && DB_VALUE_TYPE (&ans) == DB_TYPE_INTEGER && db_get_int (&ans) == want;
	  if (!ok)
	    {
	      bad.add (a.name + " vs " + b.name + ": rc " + std::to_string (rc) + " answer "
		       + (DB_VALUE_TYPE (&ans) == DB_TYPE_INTEGER ? std::to_string (db_get_int (&ans)) : "non-int")
		       + ", exact " + std::to_string (want));
	    }
	  pairs++;
	  /* which of head's new branches the pair reaches (same sign only: a sign difference answers first) */
	  if (a.neg == b.neg)
	    {
	      if (a.s == b.s && a.p != b.p)
		{
		  same_scale_other_precision++;
		}
	      else if (a.s != b.s && below_2_128 (a.mag) && below_2_128 (b.mag) && std::abs (a.s - b.s) <= 38)
		{
		  u128_scaled++;
		  const numv & lo = a.s < b.s ? a : b;
		  overflow_side += !below_2_128 (dec_shift (lo.mag, std::abs (a.s - b.s)));
		}
	    }
	}
    }
  if (verbose ())
    {
      printf ("X1 pairs %ld, same scale other precision %ld, u128 scaled %ld (product past 2^128: %ld)\n", pairs,
	      same_scale_other_precision, u128_scaled, overflow_side);
    }
  INFO ("values " << g.size () << ", pairs " << pairs << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (pairs > 500000);
  CHECK (same_scale_other_precision > 1000);
  CHECK (u128_scaled > 10000);
  CHECK (overflow_side > 1000);
}

TEST_CASE ("X2 numeric_db_value_compare answers what float_numeric_compare - develop's general path, unchanged "
	   "by the PR - answers for every same-sign pair of the grid", "[contract]")
{
  REQUIRE (boot_types ());
  const std::vector<numv> &g = compare_grid ();
  mismatches bad;
  long pairs = 0;
  for (const numv & a : g)
    {
      for (const numv & b : g)
	{
	  if (a.neg != b.neg)
	    {
	      continue;
	    }
	  DB_VALUE ans;
	  db_make_null (&ans);
	  REQUIRE (numeric_db_value_compare (&a.v, &b.v, &ans) == NO_ERROR);
	  const int general = float_numeric_compare ((uint8_t *) db_locate_numeric (&a.v),
						     (uint8_t *) db_locate_numeric (&b.v), a.p, a.s, b.p, b.s, a.neg,
						     b.neg);
	  if (db_get_int (&ans) != general)
	    {
	      bad.add (a.name + " vs " + b.name + ": numeric_db_value_compare " + std::to_string (db_get_int (&ans))
		       + ", float_numeric_compare " + std::to_string (general));
	    }
	  pairs++;
	}
    }
  INFO ("pairs " << pairs << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (pairs > 250000);
}

TEST_CASE ("X3 boundary: scale differences 0 (other precisions), 1, 19, 38 (head's u128 product) and 39, 40, 466 "
	   "(general path); c x 10^k at 2^128 (34028236692093846346 / ...347 against 2^128-1 at scale 19, both orders); "
	   "2^128-1 against 2^128; opposite signs; zeros at every scale; one value at scales 0..39", "[contract]")
{
  REQUIRE (boot_types ());
  struct pair_case
  {
    numv a, b;
    int want;
  };
  std::vector<pair_case> cases;
  const std::string m128 = "340282366920938463463374607431768211455";
  /* c x 10^19 just below and just past 2^128 - 1: ...346 x 10^19 < 2^128 - 1 at scale 19; ...347 x 10^19 overflows */
  for (bool neg : { false, true })
    {
      const int sg = neg ? -1 : 1;
      const numv lo = make_num ("34028236692093846346", 20, 0, neg, true);
      const numv hi = make_num ("34028236692093846347", 20, 0, neg, true);
      const numv big = make_num (m128, 39, 19, neg, true);
      cases.push_back ({lo, big, -sg});
      cases.push_back ({hi, big, sg});
      cases.push_back ({big, lo, sg});
      cases.push_back ({big, hi, -sg});
      /* scale difference 38: 3 x 10^38 < 2^128 - 1 < 4 x 10^38 (the product of 4 overflows) */
      cases.push_back ({make_num ("3", 1, 0, neg, true), make_num (m128, 39, 38, neg, true), -sg});
      cases.push_back ({make_num ("4", 1, 0, neg, true), make_num (m128, 39, 38, neg, true), sg});
      cases.push_back ({make_num (m128, 39, 38, neg, true), make_num ("4", 1, 0, neg, true), -sg});
      /* scale difference 39 (past the u128 table): the general path */
      cases.push_back ({make_num ("4", 1, 0, neg, true), make_num ("4" + std::string (39, '0'), 40, 39, neg, true), 0});
      cases.push_back ({make_num ("1", 1, 39, neg, true), make_num ("1", 1, 0, neg, true), -sg});
      /* 2^128 - 1 against 2^128 (a 17th byte): one side leaves the u128 path */
      cases.push_back ({make_num (m128, 39, 0, neg, true), make_num (TWO128, 39, 0, neg, true), -sg});
      cases.push_back ({make_num (m128, 39, 1, neg, true), make_num (TWO128, 39, 0, neg, true), -sg});
      cases.push_back ({make_num (TWO128, 39, 1, neg, true), make_num (m128, 39, 0, neg, true), -sg});
      /* equal scales, other precisions (head: the coefficient compare at any precisions) */
      cases.push_back ({make_num ("12345", 5, 2, neg, false), make_num ("12345", 38, 2, neg, false), 0});
      cases.push_back ({make_num ("12345", 5, 2, neg, true), make_num ("12346", 38, 2, neg, false), -sg});
      cases.push_back ({make_num (TWO128, 39, 2, neg, true), make_num (m128, 39, 2, neg, true), sg});
      /* the widest scale difference: -214 against 252 */
      cases.push_back ({make_num ("1", 1, -214, neg, true), make_num ("1", 1, 252, neg, true), sg});
    }
  /* opposite signs answer by the sign, whatever the magnitudes */
  cases.push_back ({make_num ("1", 1, 252, false, true), make_num ("9999999999999999999999999999999999999999", 40,
								    -214, true, true), 1});
  cases.push_back ({make_num ("1", 1, 38, true, true), make_num ("0", 1, 0, false, true), -1});
  /* one value (7) at scales 0..39 is equal to itself everywhere; its neighbours are not */
  std::vector<numv> sevens;
  for (int s = 0; s <= 39; s++)
    {
      sevens.push_back (make_num (dec_shift ("7", s), s + 1, s, false, true));
      if (s + 1 <= 38)
	{
	  sevens.push_back (make_num (dec_shift ("7", s), s + 1, s, false, false));
	}
    }
  for (const numv & a : sevens)
    {
      for (const numv & b : sevens)
	{
	  cases.push_back ({a, b, 0});
	}
      cases.push_back ({a, make_num (dec_add (dec_shift ("7", 20), "1"), 21, 20, false, true), -1});
    }
  /* zeros at every scale class are equal, and below any positive value */
  std::vector<numv> zeros;
  for (int s : { 0, 1, 19, 38, 39, 252, -214 })
    {
      zeros.push_back (make_num ("0", 1, s, false, true));
      if (s >= 0 && s <= 38)
	{
	  zeros.push_back (make_num ("0", std::max (1, s), s, false, false));
	}
    }
  for (const numv & a : zeros)
    {
      for (const numv & b : zeros)
	{
	  cases.push_back ({a, b, 0});
	}
      cases.push_back ({a, make_num ("1", 1, 252, false, true), -1});
      cases.push_back ({make_num ("1", 1, 252, true, true), a, -1});
    }
  mismatches bad;
  for (const pair_case & c : cases)
    {
      DB_VALUE ans;
      db_make_null (&ans);
      const int rc = numeric_db_value_compare (&c.a.v, &c.b.v, &ans);
      if (exact_order (c.a, c.b) != c.want)
	{
	  bad.add (c.a.name + " vs " + c.b.name + ": the case table says " + std::to_string (c.want) + ", the oracle "
		   + std::to_string (exact_order (c.a, c.b)));
	}
      if (rc != NO_ERROR || db_get_int (&ans) != c.want)
	{
	  bad.add (c.a.name + " vs " + c.b.name + ": " + std::to_string (db_get_int (&ans)) + ", want "
		   + std::to_string (c.want));
	}
    }
  INFO ("cases " << cases.size () << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
}

/* ================================================================ I integer against NUMERIC */

static void
check_int_numeric (const std::vector<intv> &ints, const std::vector<numv> &nums, mismatches & bad, long &pairs)
{
  for (const intv & i : ints)
    {
      for (const numv & n : nums)
	{
	  const int want = exact_order (i.as_num, n);
	  const DB_VALUE *a = &i.v, *b = &n.v;
	  const outcome row_ab = by_row (a, b), row_ba = by_row (b, a);
	  const outcome res_ab = by_resolved (a, b), res_ba = by_resolved (b, a);
	  const DOMAIN_COMPARE d = key_decision (a, b);
	  DOMAIN_COMPARE m;
	  domain_compare_mirror (&d, &m);
	  const outcome dec_ab = by_decision (d, a, b), mir_ba = by_decision (m, b, a);
	  const DOMAIN_COMPARE r = key_decision (b, a);
	  const outcome dec_ba = by_decision (r, b, a);
	  struct
	  {
	    const char *what;
	    outcome o;
	    int want;
	  } checks[] = {
	    {"tp_value_compare_with_error (i, n)", row_ab, want},
	    {"tp_value_compare_with_error (n, i)", row_ba, reversed_result (want)},
	    {"resolved (i, n)", res_ab, want},
	    {"resolved (n, i)", res_ba, reversed_result (want)},
	    {"decided (i, n)", dec_ab, want},
	    {"decided (n, i)", dec_ba, reversed_result (want)},
	    {"mirror of (i, n) on (n, i)", mir_ba, reversed_result (want)},
	  };
	  for (const auto & c : checks)
	    {
	      if (c.o.result != c.want || !c.o.can_compare || c.o.error != NO_ERROR)
		{
		  bad.add (i.name + " vs " + n.name + ": " + c.what + " " + show (c.o) + ", exact "
			   + std::to_string (c.want));
		}
	    }
	  if (!domain_compare_equal (&m, &r))
	    {
	      bad.add (i.name + " vs " + n.name + ": the mirror of (i, n) is not the decision for (n, i)");
	    }
	  pairs++;
	}
    }
}

TEST_CASE ("I1 SHORT, INT and BIGINT against NUMERIC: tp_value_compare_with_error (develop's row comparison: the "
	   "integer coerced into NUMERIC(40,0)), the resolved comparison, the key decision and its B-tree mirror answer "
	   "the exact order in both argument orders - grid", "[contract]")
{
  REQUIRE (boot_types ());
  std::vector<numv> nums;
  for (const numv & n : compare_grid ())
    {
      if (n.s >= -19 && n.s <= 40)
	{
	  nums.push_back (n);
	}
    }
  mismatches bad;
  long pairs = 0;
  check_int_numeric (int_grid (), nums, bad, pairs);
  INFO ("pairs " << pairs << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (pairs > 10000);
}

TEST_CASE ("I2 boundary: +-(2^63-1), -2^63, 0, SHORT/INT limits against each magnitude at scales 0, 1, 19, 20, 38 "
	   "and one unit either side; NUMERIC(38,0) 10^38-1, NUMERIC(38,38); 2^128-1 and 2^128 at scales 18..39 (the "
	   "u128 fast path ends at scale 19); 1E+19, 9E+18, 9223372036854775810 (negative scales); scale 252 and -214",
	   "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long pairs = 0;
  check_int_numeric (int_grid (), int_numeric_grid (), bad, pairs);
  INFO ("pairs " << pairs << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (pairs > 20000);
}

/* ================================================================ R ROUND / TRUNC */

static void
check_round (bool half_up, const std::vector<numv> &g, mismatches & bad, long &evaluated)
{
  for (const numv & n : g)
    {
      for (long long place : places_for (n.s))
	{
	  DB_VALUE in = n.v, pl, res;
	  db_make_bigint (&pl, place);
	  db_make_null (&res);
	  er_clear ();
	  const int rc = half_up ? db_round_dbval (&res, &in, &pl) : db_trunc_dbval (&res, &in, &pl);
	  const num_out got = read_out (rc, er_errid (), res);
	  const num_out want = develop_round (n, place, half_up);
	  if (!same_out (got, want))
	    {
	      bad.add (std::string (half_up ? "ROUND (" : "TRUNC (") + n.name + ", " + std::to_string (place) + ") = "
		       + show_out (got) + ", develop " + show_out (want));
	    }
	  evaluated++;
	}
    }
}

TEST_CASE ("R1 ROUND (db_round_dbval) of a NUMERIC gives develop's value, precision, scale, sign, float form and "
	   "error - every value of the grid at places from -10^12 to BIGINT max", "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long evaluated = 0;
  check_round (true, round_grid (), bad, evaluated);
  INFO ("evaluated " << evaluated << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (evaluated > 50000);
}

TEST_CASE ("R2 TRUNC (db_trunc_dbval) of a NUMERIC gives develop's value, precision, scale, sign, float form and "
	   "error - the same grid", "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long evaluated = 0;
  check_round (false, round_grid (), bad, evaluated);
  INFO ("evaluated " << evaluated << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (evaluated > 50000);
}

TEST_CASE ("R3 boundary: half up at .5 (12.5 -> 13, -12.5 -> -13, 12.49 -> 12), a negative value rounding to 0 is not "
	   "negative, 9.95 -> 10.0 keeps precision 3 (a 4-digit coefficient, as develop), place = scale and scale+1 "
	   "(the value), scale-38 / scale-39 (the last place the u128 path rounds, then 0), 2^128-1 rounding past 2^128, "
	   "INT, SHORT and DOUBLE places, a 40-digit value at scale -214 rounding up (ER_IT_DATA_OVERFLOW)",
	   "[contract]")
{
  REQUIRE (boot_types ());
  struct rcase
  {
    numv n;
    DB_VALUE place;
    long long place_i;
    bool half_up;
    std::string expect;		/* show_out of the expected result */
  };
  std::vector<rcase> cases;
  auto bigint = [](long long x)
  {
    DB_VALUE v;
    db_make_bigint (&v, x);
    return v;
  };
  auto integer = [](int x)
  {
    DB_VALUE v;
    db_make_int (&v, x);
    return v;
  };
  auto shortv = [](short x)
  {
    DB_VALUE v;
    db_make_short (&v, x);
    return v;
  };
  auto dbl = [](double x)
  {
    DB_VALUE v;
    db_make_double (&v, x);
    return v;
  };
  const numv v125 = make_num ("125", 3, 1, false, false);	/* NUMERIC(3,1) 12.5 */
  const numv n125 = make_num ("125", 3, 1, true, false);	/* -12.5 */
  const numv v1249 = make_num ("1249", 4, 2, false, false);
  const numv n4 = make_num ("4", 1, 1, true, true);	/* -0.4 */
  const numv v995 = make_num ("995", 3, 2, false, false);	/* 9.95 */
  const numv m128 = make_num ("340282366920938463463374607431768211455", 39, 0, false, true);
  const numv m128s = make_num ("340282366920938463463374607431768211455", 39, 38, false, true);
  const numv big = make_num ("9999999999999999999999999999999999999999", 40, -214, false, true);
  const numv big213 = make_num ("9999999999999999999999999999999999999999", 40, -213, false, true);
  cases.push_back ({v125, bigint (0), 0, true, "130e-1 (p3 float)"});
  cases.push_back ({n125, bigint (0), 0, true, "-130e-1 (p3 float)"});
  cases.push_back ({v1249, bigint (0), 0, true, "1200e-2 (p4 float)"});
  cases.push_back ({v1249, bigint (1), 1, true, "1250e-2 (p4 float)"});
  cases.push_back ({n4, bigint (0), 0, true, "0e-1 (p1 float)"});
  cases.push_back ({n4, bigint (0), 0, false, "0e-1 (p1 float)"});
  cases.push_back ({v995, bigint (1), 1, true, "1000e-2 (p3 float)"});
  cases.push_back ({v995, bigint (0), 0, true, "1000e-2 (p3 float)"});
  cases.push_back ({v125, bigint (1), 1, true, "125e-1 (p3 float)"});
  cases.push_back ({v125, bigint (2), 2, true, "125e-1 (p3 fixed)"});
  cases.push_back ({v125, bigint (DB_BIGINT_MAX), DB_BIGINT_MAX, true, "125e-1 (p3 fixed)"});
  cases.push_back ({v125, bigint (-1), -1, true, "100e-1 (p3 float)"});
  cases.push_back ({v125, bigint (-2), -2, true, "0e-1 (p3 float)"});
  cases.push_back ({m128s, bigint (0), 0, true, "3" + std::string (38, '0') + "e-38 (p39 float)"});
  cases.push_back ({m128s, bigint (-1), -1, true, "0e-38 (p39 float)"});
  cases.push_back ({m128s, bigint (-2), -2, true, "0e-38 (p39 float)"});
  cases.push_back ({m128, bigint (-1), -1, true, "340282366920938463463374607431768211460e0 (p39 float)"});
  cases.push_back ({m128, bigint (-1), -1, false, "340282366920938463463374607431768211450e0 (p39 float)"});
  cases.push_back ({v125, integer (0), 0, true, "130e-1 (p3 float)"});
  cases.push_back ({v125, shortv (0), 0, true, "130e-1 (p3 float)"});
  cases.push_back ({v125, dbl (0.9), 0, true, "130e-1 (p3 float)"});	/* (DB_BIGINT) 0.9 is 0 */
  cases.push_back ({v125, dbl (-1.9), -1, true, "100e-1 (p3 float)"});
  /* NUMERIC(38,0) 10^38 - 1 rounds to 10^38: 39 digits under precision 38 (develop keeps the precision) */
  cases.push_back ({make_num (std::string (38, '9'), 38, 0, false, false), bigint (-1), -1, true,
		    dec_pow10 (38) + "e0 (p38 float)"});
  cases.push_back ({big, bigint (-215), -215, true,
		    "rc " + std::to_string (ER_IT_DATA_OVERFLOW) + " er " + std::to_string (ER_IT_DATA_OVERFLOW)});
  cases.push_back ({big213, bigint (-214), -214, true, dec_pow10 (39) + "e214 (p40 float)"});
  mismatches bad;
  for (rcase & c : cases)
    {
      DB_VALUE in = c.n.v, res;
      db_make_null (&res);
      er_clear ();
      const int rc = c.half_up ? db_round_dbval (&res, &in, &c.place) : db_trunc_dbval (&res, &in, &c.place);
      const num_out got = read_out (rc, er_errid (), res);
      const num_out want = develop_round (c.n, c.place_i, c.half_up);
      const std::string label = std::string (c.half_up ? "ROUND (" : "TRUNC (") + c.n.name + ", "
	+ std::to_string (c.place_i) + ")";
      if (show_out (want) != c.expect)
	{
	  bad.add (label + ": the case table says " + c.expect + ", the develop model " + show_out (want));
	}
      if (!same_out (got, want))
	{
	  bad.add (label + " = " + show_out (got) + ", develop " + show_out (want));
	}
    }
  INFO ("first mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
}

/* ================================================================ F FLOOR / CEIL */

static void
check_floor_ceil (bool is_floor, const std::vector<numv> &g, mismatches & bad, long &evaluated)
{
  for (const numv & n : g)
    {
      DB_VALUE in = n.v, res;
      db_make_null (&res);
      er_clear ();
      const int rc = is_floor ? db_floor_dbval (&res, &in) : db_ceil_dbval (&res, &in);
      const num_out got = read_out (rc, er_errid (), res);
      const num_out want = develop_floor_ceil (n, is_floor);
      if (!same_out (got, want))
	{
	  bad.add (std::string (is_floor ? "FLOOR (" : "CEIL (") + n.name + ") = " + show_out (got) + ", develop "
		   + show_out (want));
	}
      evaluated++;
    }
}

TEST_CASE ("F1 FLOOR and CEIL (db_floor_dbval, db_ceil_dbval) of a NUMERIC give develop's value, precision, scale, "
	   "sign and float form - every value of the grid", "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long evaluated = 0;
  check_floor_ceil (true, round_grid (), bad, evaluated);
  check_floor_ceil (false, round_grid (), bad, evaluated);
  INFO ("evaluated " << evaluated << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (evaluated > 2000);
}

TEST_CASE ("F2 boundary: FLOOR (-9.5) = -10.0 with precision 3 (the carry past the precision), FLOOR (-0.05) = -1.00 "
	   "(precision 1 below the scale), CEIL (-0.5) = 0 not negative, CEIL (0.99..9 NUMERIC(38,38)) = 1 with precision "
	   "39, FLOOR (-99.95 NUMERIC(38,2)) keeps 38, scale 39 and 41 (decimal path; 41 becomes 39), a coefficient of "
	   "2^128, a negative scale", "[contract]")
{
  REQUIRE (boot_types ());
  struct fcase
  {
    numv n;
    bool is_floor;
    std::string expect;
  };
  const std::string nines38 = std::string (38, '9');
  const std::vector<fcase> cases = {
    {make_num ("95", 2, 1, true, false), true, "-100e-1 (p3 float)"},
    {make_num ("95", 2, 1, false, false), false, "100e-1 (p3 float)"},
    {make_num ("95", 2, 1, false, false), true, "90e-1 (p2 float)"},
    {make_num ("5", 1, 2, true, true), true, "-100e-2 (p3 float)"},
    {make_num ("5", 1, 1, true, true), false, "0e-1 (p1 float)"},
    {make_num (nines38, 38, 38, false, false), false, dec_pow10 (38) + "e-38 (p39 float)"},
    {make_num (nines38, 38, 38, true, false), true, "-" + dec_pow10 (38) + "e-38 (p39 float)"},
    {make_num ("9995", 38, 2, true, false), true, "-10000e-2 (p38 float)"},
    {make_num ("15", 2, 39, true, true), true, "-" + dec_pow10 (39) + "e-39 (p40 float)"},
    {make_num ("15", 2, 41, true, true), true, "-" + dec_pow10 (39) + "e-39 (p40 float)"},
    {make_num ("15", 2, 41, false, true), true, "0e-39 (p2 float)"},
    {make_num (TWO128, 39, 1, true, true), true, "-340282366920938463463374607431768211460e-1 (p39 float)"},
    {make_num ("15", 2, -3, true, true), true, "-15e3 (p2 float)"},
    {make_num ("15", 2, 0, true, false), false, "-15e0 (p2 float)"},
  };
  mismatches bad;
  for (const fcase & c : cases)
    {
      DB_VALUE in = c.n.v, res;
      db_make_null (&res);
      er_clear ();
      const int rc = c.is_floor ? db_floor_dbval (&res, &in) : db_ceil_dbval (&res, &in);
      const num_out got = read_out (rc, er_errid (), res);
      const num_out want = develop_floor_ceil (c.n, c.is_floor);
      const std::string label = std::string (c.is_floor ? "FLOOR (" : "CEIL (") + c.n.name + ")";
      if (show_out (want) != c.expect)
	{
	  bad.add (label + ": the case table says " + c.expect + ", the develop model " + show_out (want));
	}
      if (!same_out (got, want))
	{
	  bad.add (label + " = " + show_out (got) + ", develop " + show_out (want));
	}
    }
  INFO ("first mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
}

/* ================================================================ H the PR's functions (registered at head only) */

HEAD_TEST_CASE (h1_exact_compare, "H1 numeric_compare_bigint_num_exact (i, n) is the exact order for every integer "
		"and NUMERIC of the I grids, and domain_compare_cmp gives the six integer-NUMERIC type pairs their exact "
		"comparator (the key decision converts nothing) and every other pair the compared type's own",
		"[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long pairs = 0, fast = 0;
  std::vector<numv> nums = int_numeric_grid ();
  for (const numv & n : compare_grid ())
    {
      nums.push_back (n);
    }
  for (const intv & i : int_grid ())
    {
      for (const numv & n : nums)
	{
	  const int got = call_exact_t<DB_BIGINT, const DB_VALUE *> (i.i, &n.v);
	  const int want = exact_order (i.as_num, n);
	  if (got != want)
	    {
	      bad.add ("numeric_compare_bigint_num_exact (" + std::to_string (i.i) + ", " + n.name + ") = "
		       + std::to_string (got) + ", exact " + std::to_string (want));
	    }
	  fast += n.s >= 0 && n.s <= 19 && below_2_128 (n.mag);
	  pairs++;
	}
    }
  /* the comparator by type pair */
  const DB_TYPE ints[] = { DB_TYPE_SHORT, DB_TYPE_INTEGER, DB_TYPE_BIGINT };
  for (DB_TYPE t : ints)
    {
      const PR_TYPE *a = call_cmp_t<DB_TYPE> (t, DB_TYPE_NUMERIC, t);
      const PR_TYPE *b = call_cmp_t<DB_TYPE> (DB_TYPE_NUMERIC, t, DB_TYPE_NUMERIC);
      if (a == pr_type_from_id (t) || a == pr_type_from_id (DB_TYPE_NUMERIC) || b == pr_type_from_id (t)
	  || b == pr_type_from_id (DB_TYPE_NUMERIC) || a == b)
	{
	  bad.add (std::string ("domain_compare_cmp: no exact comparator for ") + pr_type_name (t) + " / NUMERIC");
	}
      const intv iv = make_int (t, 7);
      const numv nv = make_num ("70000000000000000000000000000000000001", 38, 37, false, false);	/* 7.0..01 */
      /* the comparator of each argument order, LT / EQ / GT: 7 against 7.0..01, 7.000 and 6.99 */
      const numv eq = make_num ("7000", 4, 3, false, false), lt = make_num ("699", 3, 2, false, false);
      const struct
      {
	const numv *n;
	int int_first;
      } three[] = { {&nv, DB_LT}, {&eq, DB_EQ}, {&lt, DB_GT} };
      for (const auto & c : three)
	{
	  const int ab = a->cmpval ((DB_VALUE *) & iv.v, (DB_VALUE *) & c.n->v, 1, 0, NULL, 0);
	  const int ba = b->cmpval ((DB_VALUE *) & c.n->v, (DB_VALUE *) & iv.v, 1, 0, NULL, 0);
	  if (ab != c.int_first || ba != reversed_result (c.int_first))
	    {
	      bad.add (std::string ("domain_compare_cmp: the exact comparators of ") + pr_type_name (t) + " / NUMERIC give "
		       + std::to_string (ab) + " / " + std::to_string (ba) + " for 7 against " + c.n->name);
	    }
	}
      const DOMAIN_COMPARE d = key_decision (&iv.v, &nv.v);
      if (d.method != DOMAIN_COMPARE_DIRECT || d.conv[0] != NULL || d.conv[1] != NULL)
	{
	  bad.add (std::string ("key decision ") + pr_type_name (t) + " / NUMERIC: method "
		   + std::to_string ((int) d.method) + (d.conv[0] || d.conv[1] ? " converts" : ""));
	}
    }
  const DB_TYPE others[][3] = {
    {DB_TYPE_NUMERIC, DB_TYPE_NUMERIC, DB_TYPE_NUMERIC}, {DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_BIGINT},
    {DB_TYPE_INTEGER, DB_TYPE_DOUBLE, DB_TYPE_DOUBLE}, {DB_TYPE_NUMERIC, DB_TYPE_DOUBLE, DB_TYPE_DOUBLE},
    {DB_TYPE_VARCHAR, DB_TYPE_NUMERIC, DB_TYPE_DOUBLE}, {DB_TYPE_NUMERIC, DB_TYPE_FLOAT, DB_TYPE_FLOAT}
  };
  for (const auto & o : others)
    {
      if (call_cmp_t<DB_TYPE> (o[0], o[1], o[2]) != pr_type_from_id (o[2]))
	{
	  bad.add (std::string ("domain_compare_cmp (") + pr_type_name (o[0]) + ", " + pr_type_name (o[1]) + ", "
		   + pr_type_name (o[2]) + ") is not the compared type's");
	}
    }
  if (verbose ())
    {
      printf ("H1 pairs %ld, u128 fast path %ld\n", pairs, fast);
    }
  INFO ("pairs " << pairs << " (u128 fast path " << fast << ")\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (pairs > 20000);
  CHECK (fast > 5000);
}

static void
check_round_taken (bool half_up, mismatches & bad, long &taken, long &declined)
{
  for (const numv & n : round_grid ())
    {
      for (long long place : places_for (n.s))
	{
	  DB_VALUE res;
	  db_make_null (&res);
	  er_clear ();
	  const bool t = call_round_t<const DB_VALUE *, DB_BIGINT, DB_VALUE *> (&n.v, place, half_up, &res);
	  const bool want_t = round_taken (n, place, half_up);
	  const std::string label = std::string ("round_to_place (") + n.name + ", " + std::to_string (place)
	    + (half_up ? ", ROUND)" : ", TRUNC)");
	  if (t != want_t)
	    {
	      bad.add (label + (t ? " taken" : " declined") + ", its comment says " + (want_t ? "taken" : "declined"));
	      continue;
	    }
	  if (!t)
	    {
	      declined++;
	      continue;
	    }
	  taken++;
	  const num_out got = read_out (NO_ERROR, er_errid (), res);
	  const num_out want = develop_round (n, place, half_up);
	  if (!same_out (got, want))
	    {
	      bad.add (label + " = " + show_out (got) + ", develop " + show_out (want));
	    }
	}
    }
}

HEAD_TEST_CASE (h2_round_to_place, "H2 numeric_db_value_round_to_place takes every NUMERIC below 2^128 with a scale "
		"of 0 or more (declines a round up past 2^128, a negative scale and a 17-byte coefficient) and answers "
		"what develop's ROUND / TRUNC answers there", "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long taken = 0, declined = 0;
  check_round_taken (true, bad, taken, declined);
  check_round_taken (false, bad, taken, declined);
  if (verbose ())
    {
      printf ("H2 round_to_place taken %ld, declined %ld\n", taken, declined);
    }
  INFO ("taken " << taken << ", declined " << declined << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (taken > 50000);
  CHECK (declined > 10000);
}

HEAD_TEST_CASE (h3_floor_ceil, "H3 numeric_db_value_floor_ceil takes every NUMERIC below 2^128 with a scale from 1 "
		"to 38 (declines a carry past 2^128, scale 0, a negative scale, 39 and up) and answers what develop's "
		"FLOOR / CEIL answers there", "[contract]")
{
  REQUIRE (boot_types ());
  mismatches bad;
  long taken = 0, declined = 0;
  for (const numv & n : round_grid ())
    {
      for (bool is_floor : { true, false })
	{
	  DB_VALUE res;
	  db_make_null (&res);
	  const bool t = call_floor_ceil_t<const DB_VALUE *, DB_VALUE *> (&n.v, is_floor, &res);
	  const bool want_t = floor_ceil_taken (n, is_floor);
	  const std::string label = std::string (is_floor ? "floor_ceil (FLOOR, " : "floor_ceil (CEIL, ") + n.name + ")";
	  if (t != want_t)
	    {
	      bad.add (label + (t ? " taken" : " declined") + ", its comment says " + (want_t ? "taken" : "declined"));
	      continue;
	    }
	  if (!t)
	    {
	      declined++;
	      continue;
	    }
	  taken++;
	  const num_out got = read_out (NO_ERROR, NO_ERROR, res);
	  const num_out want = develop_floor_ceil (n, is_floor);
	  if (!same_out (got, want))
	    {
	      bad.add (label + " = " + show_out (got) + ", develop " + show_out (want));
	    }
	}
    }
  if (verbose ())
    {
      printf ("H3 floor_ceil taken %ld, declined %ld\n", taken, declined);
    }
  INFO ("taken " << taken << ", declined " << declined << "\nfirst mismatches:\n" << bad.first);
  CHECK (bad.count == 0);
  CHECK (taken > 1000);
  CHECK (declined > 1000);
}
