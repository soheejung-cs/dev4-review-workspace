/*
 * PR#8213 (CBRD-27583) -- three decisions the PR moved from the row to the compiler (src/query/domain_rules.c, server
 * mode, cited at 76ba766e3). The functions are new in the PR, so there is no base to run against: the contract is the
 * row-time rule each one replaces (R7-06's proposal), at the PR head.
 *
 *   K10 domain_compare_mirror (2226; B-tree, an index key first): the mirror of a key comparison decided for (a, b) -
 *       as pt_decide_key_compare (xasl_generation.c:14357-14386) normalizes it before it is streamed - is the
 *       decision for (b, a): the optdebug assert at xasl_generation.c:14602-14604, run here over every pair of the key
 *       pair table's keys. On values, comparing (b, a) by the mirror (domain_compare_decided, as btree_compare_bound_key
 *       does with DOMAIN_KEY_BOUND.reversed, unpacked by stx_build_key_compares, stream_to_xasl.c:6042) reverses
 *       comparing (a, b) by the decision, with the same comparability and error
 *   K11 domain_key_values_equal (1885; memoize, subquery cache): two values of one key position whose type and
 *       collation are the position's domain's are equal exactly when mht_compare_dbvalues_are_identical says so -
 *       develop's cache key rule (memoize.cpp:916, subquery_cache.c:263 at 2d925cb86): strings byte by byte, the rest
 *       by value
 *   K12 domain_resolve_hash_key (2805) / domain_decide_hash_join_key (2757; hash join): the domain a key pair hashes in
 *       is the one develop's hjoin_init_domain_info chose at every execution (query_hash_join.c:1020-1138 at
 *       2d925cb86: the more general type, precisions merged; need_coerce carried to the keys after a coerced one),
 *       except a string against a number or a date, where it is the type the comparison converts both to (DOUBLE, the
 *       date type) - pairs type checking casts before a hash join sees them. A pair of NULL-typed columns
 *       (xts_hash_join_key_domain, xasl_to_stream.c:7556, 76ba766e3) is decided as the NULL domain against itself
 *   [defect] PR8213 R7-06 ENUM-JSON: tp_more_general_type ranks ENUM and JSON both 0 (neither is in db_type_rank,
 *       object_domain.c:107), so tp_value_compare_common_domain converts the first side in either order and the
 *       mirror of (ENUM, JSON) is not the decision for (JSON, ENUM) - the one key pair family K10 does not hold for,
 *       against the claim of domain_rules.h:614-616 (PASS = asymmetric). On values it changes no answer: every ENUM-JSON
 *       conversion fails, so the decision, the mirror and the exchanged decision all answer not comparable / -181
 *       (btree_compare_bound_key: DB_UNK). What it can reach is the optdebug assert, and only if such a pair is ever a
 *       key comparison (pt_common_type gives ENUM and JSON the common type VARCHAR, type_checking.c:11643-11645 and
 *       11039-11042, so a bound of one type against an index column of the other is unlikely - not checked here)
 *   [defect] PR8213 R7-06 (latent): keys whose codeset is not their collation's (no domain gives one) - the mirror is
 *       not the exchanged decision on 8 of 49 pairs: LANG_RT_COMMON_COLL merges two binary collations of one codeset
 *       only into a second ISO binary (language_support.h:65), and an off-codeset ENUM's codeset side is side 0 when
 *       both sides differ. On values the mirror still reverses the decision (-1150 both ways), as the row function
 *       is itself order dependent there (tp_value_compare_with_error: -1150 one way, EQ the other)
 *   [defect] PR8213 R7-02 (latent): a string key outside the compiler's claim (a value whose type or collation is not
 *       the position's domain's, a LEAVE or VARIABLE domain) falls back to tp_value_compare (domain_rules.c:1909, after
 *       the assert at 1907-1908), which finds 'A' = 'a' under a case-insensitive collation where develop compared the
 *       bytes (PASS = defect present). R6-02's reproduction of a broken claim (an INT key under a VARCHAR position) was
 *       closed by 4cfaa6e5a (the key domain is the regu's original_domain, xasl_generation.c:33470-33478)
 *
 * Real code at THIS revision compiled into the test binary: system_parameter.c, language_support.c, intl_support.c,
 * chartype.c, mprec.c, dtoa.c, object_primitive.c, object_domain.c, object_domain_convert.cpp, numeric_opfunc.c,
 * db_date.c, db_macro.c, string_opfunc.c (#included, as in domain_resolve.compare_collation); memory_hash.c and the
 * db_json*.cpp sources are linked as separately compiled TUs (suite.json extra). The target TU is domain_rules.c.
 * The release flags (NDEBUG) matter: K11's boundary and the R7-02 case pass values outside the claim, which a debug
 * build stops on (domain_rules.c:1907). UNITSEAM_CAPTURE=1 prints the captured expectations instead of checking them.
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
#include "memory_hash.h"
#include "db_json.hpp"
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this. DOWN makes prm_get_value (system_parameter.c:10829) read a
 * session parameter's global value - its default here - instead of a session's copy, which needs a thread entry */
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

/* memory_alloc.c:698 body (db_json's string conversions duplicate through it) */
char *
db_private_strdup (THREAD_ENTRY *, const char *s)
{
  if (s == NULL)
    {
      return NULL;
    }
  const size_t len = strlen (s);
  char *cp = (char *) malloc (len + 1);
  if (cp != NULL)
    {
      memcpy (cp, s, len);
      cp[len] = '\0';
    }
  return cp;
}

/* oid.c:252 body: an OID's cmpval (an OBJECT key position's values are OIDs on the server) */
int
oid_compare (const void *a, const void *b)
{
  const OID *oid1_p = (const OID *) a;
  const OID *oid2_p = (const OID *) b;
  int diff = oid1_p->volid - oid2_p->volid;
  if (diff)
    {
      return diff;
    }
  diff = oid1_p->pageid - oid2_p->pageid;
  if (diff)
    {
      return diff;
    }
  return oid1_p->slotid - oid2_p->slotid;
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
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset (tp_apply_sys_charset) */
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


/* ---------------------------------------------------------------- values */

struct named_value
{
  std::string name;
  DB_VALUE v;
  bool garbage;			/* a string or ENUM whose codeset is not its collation's, or a precision no column has */
};

static std::vector<named_value> g_values;

static void
add (const char *name, const DB_VALUE & v, bool garbage = false)
{
  g_values.push_back ({name, v, garbage});
}

static DB_VALUE
numeric (const char *text, int p, int s)
{
  DB_VALUE src, dst;
  db_make_string (&src, text);
  db_make_null (&dst);
  REQUIRE (tp_value_cast (&src, &dst, tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0), false)
	   == DOMAIN_COMPATIBLE);
  return dst;
}

static DB_VALUE
varchar (const char *s, int codeset, int collation, int precision = DB_MAX_VARCHAR_PRECISION)
{
  DB_VALUE v;
  db_make_varchar (&v, precision, s, (int) strlen (s), codeset, collation);
  return v;
}

static DB_VALUE
fixed_char (const char *s, int precision, int codeset, int collation)
{
  DB_VALUE v;
  db_make_char (&v, precision, s, (int) strlen (s), codeset, collation);
  return v;
}

static DB_VALUE
json_value (const char *text)
{
  JSON_DOC *doc = NULL;
  DB_VALUE v;
  REQUIRE (db_json_get_json_from_str (text, doc, strlen (text)) == NO_ERROR);
  db_make_json (&v, doc, true);
  return v;
}

static void
build_values ()
{
  if (!g_values.empty ())
    {
      return;
    }
  static unsigned char bits16[] = { 0x12, 0x34 }, bits8[] = { 0x12 };
  DB_VALUE v;
  db_make_null (&v);
  add ("NULL", v);
  db_make_short (&v, 12);
  add ("SHORT 12", v);
  db_make_int (&v, 12);
  add ("INT 12", v);
  db_make_int (&v, -3);
  add ("INT -3", v);
  db_make_bigint (&v, 12);
  add ("BIGINT 12", v);
  db_make_bigint (&v, 1099511627776LL);
  add ("BIGINT 2^40", v);
  db_make_float (&v, 1.5f);
  add ("FLOAT 1.5", v);
  db_make_double (&v, 12.0);
  add ("DOUBLE 12", v);
  db_make_double (&v, 12.5);
  add ("DOUBLE 12.5", v);
  db_make_double (&v, 1e300);
  add ("DOUBLE 1e300", v);
  add ("NUMERIC(4,2) 12.00", numeric ("12.00", 4, 2));
  add ("NUMERIC(2,1) 1.5", numeric ("1.5", 2, 1));
  add ("NUMERIC(38,0) 1e37", numeric ("10000000000000000000000000000000000000", 38, 0));
  db_make_monetary (&v, DB_CURRENCY_DOLLAR, 12.0);
  add ("MONETARY 12", v);
  add ("VARCHAR iso_bin '12'", varchar ("12", INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY));
  add ("VARCHAR utf8_bin '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_en_ci '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI));
  add ("VARCHAR utf8_bin 'abc'", varchar ("abc", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_en_ci 'ABC'", varchar ("ABC", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI));
  add ("VARCHAR utf8_en_cs 'abc'", varchar ("abc", INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CS));
  add ("VARCHAR iso_en_ci 'ABC'", varchar ("ABC", INTL_CODESET_ISO88591, LANG_COLL_ISO_EN_CI));
  add ("VARCHAR euckr_bin 'abc'", varchar ("abc", INTL_CODESET_KSC5601_EUC, LANG_COLL_EUCKR_BINARY));
  add ("VARCHAR utf8_bin '1a'", varchar ("1a", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin ''", varchar ("", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin ' 12 '", varchar (" 12 ", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '2020-01-02'", varchar ("2020-01-02", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '10:11:12'", varchar ("10:11:12", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("VARCHAR utf8_bin '2020-01-02 10:11:12'", varchar ("2020-01-02 10:11:12", INTL_CODESET_UTF8,
							   LANG_COLL_UTF8_BINARY));
  add ("CHAR(5) iso_bin 'ab'", fixed_char ("ab", 5, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY));
  add ("CHAR(5) utf8_bin 'ab'", fixed_char ("ab", 5, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  add ("CHAR(2) utf8_bin '12'", fixed_char ("12", 2, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY));
  db_make_date (&v, 1, 2, 2020);
  add ("DATE 2020-01-02", v);
  db_make_time (&v, 10, 11, 12);
  add ("TIME 10:11:12", v);
  {
    DB_DATETIME dt;
    db_datetime_encode (&dt, 1, 2, 2020, 10, 11, 12, 0);
    db_make_datetime (&v, &dt);
    add ("DATETIME 2020-01-02 10:11:12", v);
  }
  db_make_enumeration (&v, 2, "abc", 3, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  add ("ENUM(2,'abc') utf8_bin", v);
  db_make_enumeration (&v, 1, "ABC", 3, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  add ("ENUM(1,'ABC') iso_bin", v);
  db_make_bit (&v, 16, (DB_CONST_C_BIT) bits16, 16);
  add ("BIT(16) x1234", v);
  db_make_varbit (&v, 64, (DB_CONST_C_BIT) bits8, 8);
  add ("VARBIT x12", v);
  /* garbage: no column gives a value these (a bind's string takes the session collation's codeset,
   * lang_get_client_charset/_collation language_support.c:2767; a DBLINK value is cast into the local
   * column's domain, dblink_scan.c:1391), but nothing in a DB_VALUE prevents them */
  add ("VARCHAR cs=utf8 coll=iso_bin '12'", varchar ("12", INTL_CODESET_UTF8, LANG_COLL_ISO_BINARY), true);
  add ("VARCHAR cs=iso coll=utf8_bin 'ab'", varchar ("ab", INTL_CODESET_ISO88591, LANG_COLL_UTF8_BINARY), true);
  add ("VARCHAR coll=200 (unregistered) 'ab'", varchar ("ab", INTL_CODESET_UTF8, 200), true);
  add ("VARCHAR prec=-1 'ab'", varchar ("ab", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY,
					TP_FLOATING_PRECISION_VALUE), true);
  add ("VARCHAR prec=0 ''", varchar ("", INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY, 0), true);
  add ("CHAR prec=-1 'ab'", fixed_char ("ab", TP_FLOATING_PRECISION_VALUE, INTL_CODESET_UTF8,
					LANG_COLL_UTF8_BINARY), true);
  add ("CHAR(1) 'ab' (longer than its precision)", fixed_char ("ab", 1, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY),
       true);
  db_make_varbit (&v, TP_FLOATING_PRECISION_VALUE, (DB_CONST_C_BIT) bits8, 8);
  add ("VARBIT prec=-1", v, true);
  db_make_enumeration (&v, 3, "ab", 2, INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI);
  add ("ENUM(3,'ab') utf8_en_ci", v);
  /* JSON scalars and a document (decided_compare: R7-06 asked for the ENUM-JSON pairs) */
  add ("JSON '\"abc\"'", json_value ("\"abc\""));
  add ("JSON '12'", json_value ("12"));
  add ("JSON '1.5'", json_value ("1.5"));
  add ("JSON '{\"a\":1}'", json_value ("{\"a\":1}"));
}

/* codeset != its collation's codeset: the keys the type pair table has no exact row for */
static bool
codeset_off_collation (const DB_VALUE * v)
{
  DOMAIN_COMPARE_KEY key;
  domain_compare_key_of_value (v, &key);
  return TP_TYPE_HAS_COLLATION (key.type) && key.collation >= 0 && key.collation < LANG_MAX_COLLATIONS
    && key.codeset != lang_get_collation (key.collation)->codeset;
}

struct outcome
{
  int result;
  bool can_compare;
  int error;
  bool operator== (const outcome & o) const
  {
    return result == o.result && can_compare == o.can_compare && error == o.error;
  }
};

static std::string
show (const outcome & o)
{
  static const char *names[] = { "EQ", "LT", "GT", "UNK" };
  const int r = o.result;
  return std::string (r == DB_EQ ? names[0] : r == DB_LT ? names[1] : r == DB_GT ? names[2] : names[3]) + "/"
    + (o.can_compare ? "comparable" : "not comparable") + "/er " + std::to_string (o.error);
}


static DB_VALUE_COMPARE_RESULT
reversed_result (int r)
{
  return r == DB_LT ? DB_GT : r == DB_GT ? DB_LT : (DB_VALUE_COMPARE_RESULT) r;
}

static const TP_DOMAIN *
vc_domain (int precision, int collation)
{
  return tp_domain_resolve (DB_TYPE_VARCHAR, NULL, precision, 0, NULL, collation);
}

static const TP_DOMAIN *
char_domain (int precision, int collation)
{
  return tp_domain_resolve (DB_TYPE_CHAR, NULL, precision, 0, NULL, collation);
}

static const TP_DOMAIN *
numeric_domain (int precision, int scale)
{
  return tp_domain_resolve (DB_TYPE_NUMERIC, NULL, precision, scale, NULL, 0);
}

static const TP_DOMAIN *
type_domain (DB_TYPE type)
{
  return tp_domain_resolve_default (type);
}

/* "TYPE(precision,scale) coll=c" or the error code */
static std::string
domain_text (int error, const TP_DOMAIN * d)
{
  if (error != NO_ERROR)
    {
      return "error " + std::to_string (error);
    }
  if (d == NULL)
    {
      return "no domain";
    }
  char buf[128];
  snprintf (buf, sizeof (buf), "%s(%d,%d) coll=%d", pr_type_name (TP_DOMAIN_TYPE (d)), d->precision, d->scale,
	    TP_TYPE_HAS_COLLATION (TP_DOMAIN_TYPE (d)) ? d->collation_id : -1);
  return buf;
}

static const bool capture = getenv ("UNITSEAM_CAPTURE") != NULL;

/* ---------------------------------------------------------------- K10: keys and decisions */

struct named_key
{
  std::string name;
  DOMAIN_COMPARE_KEY key;
};

/* the key pair table's keys (domain_key_pairs_make, domain_rules.c:2551): every type domain_element_type lists, a
 * string or ENUM type once per registered collation (domain_collation_registered), in that collation's codeset */
static std::vector<named_key>
table_keys ()
{
  static const DB_TYPE element_types[] = {
    DB_TYPE_INTEGER, DB_TYPE_FLOAT, DB_TYPE_DOUBLE, DB_TYPE_VARCHAR, DB_TYPE_OBJECT, DB_TYPE_SET, DB_TYPE_MULTISET,
    DB_TYPE_SEQUENCE, DB_TYPE_ELO, DB_TYPE_TIME, DB_TYPE_TIMESTAMP, DB_TYPE_DATE, DB_TYPE_MONETARY, DB_TYPE_SHORT,
    DB_TYPE_VOBJ, DB_TYPE_OID, DB_TYPE_NUMERIC, DB_TYPE_BIT, DB_TYPE_VARBIT, DB_TYPE_CHAR, DB_TYPE_BIGINT,
    DB_TYPE_DATETIME, DB_TYPE_BLOB, DB_TYPE_CLOB, DB_TYPE_ENUMERATION, DB_TYPE_TIMESTAMPTZ, DB_TYPE_TIMESTAMPLTZ,
    DB_TYPE_DATETIMETZ, DB_TYPE_DATETIMELTZ, DB_TYPE_JSON
  };
  std::vector<named_key> keys;
  for (DB_TYPE t : element_types)
    {
      if (!TP_TYPE_HAS_COLLATION (t))
	{
	  keys.push_back ({pr_type_name (t), DOMAIN_COMPARE_KEY { t, -1, -1 }});
	  continue;
	}
      for (int c = 0; c < LANG_MAX_COLLATIONS; c++)
	{
	  const LANG_COLLATION *lc = lang_get_collation (c);
	  if (lc == NULL || lc->coll.coll_id != c)
	    {
	      continue;
	    }
	  keys.push_back ({std::string (pr_type_name (t)) + " " + lc->coll.coll_name,
			   DOMAIN_COMPARE_KEY { t, (int) lc->codeset, c }});
	}
    }
  return keys;
}

/* A key comparison as XASL generation streams it for an index scan's bound: domain_resolve_comparison, normalized by
 * pt_decide_key_compare (xasl_generation.c:14374-14384 - an OBJECT side compares OIDs by the first value's cmpval;
 * nothing converted, the first value's type). stx_build_key_compares mirrors exactly that (stream_to_xasl.c:6042). */
static DOMAIN_COMPARE
key_decision (const DOMAIN_COMPARE_KEY & a, const DOMAIN_COMPARE_KEY & b)
{
  DOMAIN_COMPARE d;
  REQUIRE (domain_resolve_comparison (&a, &b, &d) == NO_ERROR);
  if (d.method == DOMAIN_COMPARE_OBJECT)
    {
      d.method = DOMAIN_COMPARE_DIRECT;
      d.cmp = pr_type_from_id ((DB_TYPE) d.source[0]);
    }
  if (d.conv[0] == NULL && d.conv[1] == NULL)
    {
      d.first = 0;
      d.converted_first = d.source[0];
    }
  return d;
}

static bool
enum_json (DB_TYPE a, DB_TYPE b)
{
  return (a == DB_TYPE_ENUMERATION && b == DB_TYPE_JSON) || (a == DB_TYPE_JSON && b == DB_TYPE_ENUMERATION);
}

static std::string
decision_text (const DOMAIN_COMPARE & d)
{
  char buf[320];
  snprintf (buf, sizeof (buf), "method %d first %d source %s,%s converted_first %s rank %d codeset_side %d coll %d "
	    "cmp %s conv %s,%s target %s,%s", (int) d.method, (int) d.first, pr_type_name ((DB_TYPE) d.source[0]),
	    pr_type_name ((DB_TYPE) d.source[1]), pr_type_name ((DB_TYPE) d.converted_first), (int) d.rank,
	    (int) d.codeset_side, (int) d.collation, d.cmp != NULL ? d.cmp->name : "-", d.conv[0] != NULL ? "yes" : "-",
	    d.conv[1] != NULL ? "yes" : "-", d.target[0] != NULL ? pr_type_name (TP_DOMAIN_TYPE (d.target[0])) : "-",
	    d.target[1] != NULL ? pr_type_name (TP_DOMAIN_TYPE (d.target[1])) : "-");
  return buf;
}

/* what btree_compare_bound_key gives for two values by a key comparison: domain_compare_decided */
static outcome
decided_outcome (const DOMAIN_COMPARE & d, const DB_VALUE * a, const DB_VALUE * b)
{
  outcome o = { 0, true, NO_ERROR };
  er_clear ();
  o.result = domain_compare_decided (&d, a, b, &o.can_compare);
  o.error = er_errid ();
  return o;
}

/* a value a key comparison meets: not NULL (the B-tree answers a NULL before it compares) and a string or ENUM whose
 * codeset is its collation's (no domain gives another; K4 of compare_collation) */
static bool
key_value (const named_value & v)
{
  return !DB_IS_NULL (&v.v) && !codeset_off_collation (&v.v);
}

static const named_value *
find_value (const char *name)
{
  for (const named_value & v : g_values)
    {
      if (v.name == name)
	{
	  return &v;
	}
    }
  return NULL;
}

/* ---------------------------------------------------------------- K10 cases */

TEST_CASE ("K10 mirror: the mirror of a key comparison decided for (a, b) is the decision for (b, a), every pair of "
	   "the key pair table's keys (ENUM-JSON aside)", "[contract]")
{
  REQUIRE (boot_types ());
  const std::vector<named_key> keys = table_keys ();
  int pairs = 0, enum_json_pairs = 0, n_enum = 0;
  for (const named_key & k : keys)
    {
      n_enum += k.key.type == DB_TYPE_ENUMERATION;
    }
  for (const named_key & a : keys)
    {
      for (const named_key & b : keys)
	{
	  if (enum_json (a.key.type, b.key.type))
	    {
	      enum_json_pairs++;	/* the [defect] case below */
	      continue;
	    }
	  const DOMAIN_COMPARE d = key_decision (a.key, b.key), r = key_decision (b.key, a.key);
	  DOMAIN_COMPARE m;
	  domain_compare_mirror (&d, &m);
	  INFO (a.name << " vs " << b.name << "\n  decided   " << decision_text (d) << "\n  mirrored  "
		<< decision_text (m) << "\n  exchanged " << decision_text (r));
	  CHECK (domain_compare_equal (&m, &r));
	  pairs++;
	}
    }
  INFO ("keys " << keys.size () << ", ENUM keys " << n_enum);
  CHECK (enum_json_pairs == 2 * n_enum);
  CHECK (pairs == (int) (keys.size () * keys.size ()) - 2 * n_enum);
  CHECK (keys.size () > 50);
}

TEST_CASE ("K10 mirror on values: comparing (b, a) by the mirror reverses comparing (a, b) by the decision - result, "
	   "comparability, error (47 key values, four JSON among them: every pair but ENUM-JSON)", "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  int pairs = 0, converted = 0, failed = 0;
  for (const named_value & a : g_values)
    {
      for (const named_value & b : g_values)
	{
	  if (!key_value (a) || !key_value (b))
	    {
	      continue;
	    }
	  DOMAIN_COMPARE_KEY ka, kb;
	  domain_compare_key_of_value (&a.v, &ka);
	  domain_compare_key_of_value (&b.v, &kb);
	  if (enum_json (ka.type, kb.type))
	    {
	      continue;		/* the [defect] case below */
	    }
	  const DOMAIN_COMPARE d = key_decision (ka, kb);
	  DOMAIN_COMPARE m;
	  domain_compare_mirror (&d, &m);
	  const outcome ab = decided_outcome (d, &a.v, &b.v), ba = decided_outcome (m, &b.v, &a.v);
	  INFO (a.name << " vs " << b.name << ": decision " << show (ab) << ", mirror on (b, a) " << show (ba) << "\n  "
		<< decision_text (d));
	  CHECK (ab.result == reversed_result (ba.result));
	  CHECK (ab.can_compare == ba.can_compare);
	  CHECK (ab.error == ba.error);
	  pairs++;
	  converted += d.method == DOMAIN_COMPARE_CONVERT;
	  failed += !ab.can_compare;
	}
    }
  INFO ("pairs " << pairs << ", converting " << converted << ", not comparable " << failed);
  CHECK (pairs > 1000);
  CHECK (converted > 100);
  CHECK (failed > 50);
}

TEST_CASE ("K10 boundary: a converted side, a codeset side, collations that do not merge and a pair compared as it "
	   "is keep their sides in the mirror", "[contract]")
{
  REQUIRE (boot_types ());
  build_values ();
  struct
  {
    const char *a, *b, *ab, *ba;
  } cases[] = {
    /* the string side is converted first (TO_DOUBLE), the other after it */
    {"VARCHAR utf8_bin '12'", "INT 12", "EQ/comparable/er 0", "EQ/comparable/er 0"},
    /* the string's conversion fails first: the rank of (VARCHAR, INT), GT, and its mirror's of (INT, VARCHAR), LT */
    {"VARCHAR utf8_bin '1a'", "INT 12", "GT/not comparable/er -181", "LT/not comparable/er -181"},
    /* the string is converted into the date */
    {"VARCHAR utf8_bin '2020-01-02'", "DATE 2020-01-02", "EQ/comparable/er 0", "EQ/comparable/er 0"},
    /* an ENUM against a string of another codeset: the string is brought into the ENUM's (codeset_side 1 -> 0) */
    {"ENUM(1,'ABC') iso_bin", "VARCHAR utf8_en_ci 'ABC'", "EQ/comparable/er 0", "EQ/comparable/er 0"},
    /* collations that do not merge: -1150 both ways */
    {"VARCHAR utf8_en_ci 'ABC'", "VARCHAR utf8_en_cs 'abc'", "UNK/not comparable/er -1150",
     "UNK/not comparable/er -1150"},
    /* numbers of two types: the BIGINT converted into DOUBLE in either order */
    {"BIGINT 2^40", "DOUBLE 1e300", "LT/comparable/er 0", "GT/comparable/er 0"},
    /* nothing converted: the mirror compares by the cmpval of its own first value's type (CHAR, then VARCHAR) */
    {"CHAR(5) utf8_bin 'ab'", "VARCHAR utf8_bin 'abc'", "LT/comparable/er 0", "GT/comparable/er 0"},
  };
  for (auto & c : cases)
    {
      const named_value *a = find_value (c.a), *b = find_value (c.b);
      REQUIRE (a != NULL);
      REQUIRE (b != NULL);
      DOMAIN_COMPARE_KEY ka, kb;
      domain_compare_key_of_value (&a->v, &ka);
      domain_compare_key_of_value (&b->v, &kb);
      const DOMAIN_COMPARE d = key_decision (ka, kb);
      DOMAIN_COMPARE m;
      domain_compare_mirror (&d, &m);
      const std::string ab = show (decided_outcome (d, &a->v, &b->v)), ba = show (decided_outcome (m, &b->v, &a->v));
      if (capture)
	{
	  printf ("K10b {\"%s\", \"%s\", \"%s\", \"%s\"}  decided %s\n", c.a, c.b, ab.c_str (), ba.c_str (),
		  decision_text (d).c_str ());
	  continue;
	}
      INFO (c.a << " vs " << c.b << "\n  " << decision_text (d) << "\n  mirror " << decision_text (m));
      CHECK (ab == c.ab);
      CHECK (ba == c.ba);
      const DOMAIN_COMPARE r = key_decision (kb, ka);
      CHECK (domain_compare_equal (&m, &r));
      if (d.conv[0] == NULL && d.conv[1] == NULL)
	{
	  /* the mirror's first value is b: its type's cmpval and converted_first (domain_compare_mirror's last step) */
	  CHECK (m.cmp == pr_type_from_id (kb.type));
	  CHECK ((int) m.converted_first == (int) kb.type);
	}
    }
}

TEST_CASE ("PR8213 R7-06: ENUM against JSON - tp_more_general_type ranks both 0, so each order converts its first "
	   "side and the mirror of (ENUM, JSON) is not the decision for (JSON, ENUM) (PASS = asymmetric)", "[defect]")
{
  REQUIRE (boot_types ());
  build_values ();
  const std::vector<named_key> keys = table_keys ();
  const DOMAIN_COMPARE_KEY json = { DB_TYPE_JSON, -1, -1 };
  int enums = 0, differ = 0;
  /* both types out of db_type_rank: each order converts its first side */
  CHECK (tp_more_general_type (DB_TYPE_ENUMERATION, DB_TYPE_JSON) == 0);
  CHECK (tp_value_compare_common_domain (DB_TYPE_ENUMERATION, DB_TYPE_JSON) == TP_COMPARE_COERCE_FIRST_TO_SECOND);
  CHECK (tp_value_compare_common_domain (DB_TYPE_JSON, DB_TYPE_ENUMERATION) == TP_COMPARE_COERCE_FIRST_TO_SECOND);
  for (const named_key & e : keys)
    {
      if (e.key.type != DB_TYPE_ENUMERATION)
	{
	  continue;
	}
      enums++;
      const DOMAIN_COMPARE d = key_decision (e.key, json), r = key_decision (json, e.key);
      DOMAIN_COMPARE m;
      domain_compare_mirror (&d, &m);
      INFO (e.name << " vs JSON\n  decided   " << decision_text (d) << "\n  mirrored  " << decision_text (m)
	    << "\n  exchanged " << decision_text (r));
      /* (ENUM, JSON) converts the ENUM into JSON, (JSON, ENUM) the JSON into the ENUM */
      CHECK ((d.first == 0 && d.conv[0] != NULL && d.conv[1] == NULL && TP_DOMAIN_TYPE (d.target[0]) == DB_TYPE_JSON));
      CHECK ((r.first == 0 && r.conv[0] != NULL && r.conv[1] == NULL
	      && TP_DOMAIN_TYPE (r.target[0]) == DB_TYPE_ENUMERATION));
      /* the mirror keeps the decision's conversion: the ENUM, now side 1 */
      CHECK ((m.first == 1 && m.conv[1] == d.conv[0] && m.conv[0] == NULL));
      differ += !domain_compare_equal (&m, &r);
    }
  INFO ("ENUM keys " << enums << ", mirror != exchanged decision " << differ);
  CHECK (enums > 5);
  CHECK (differ == enums);

  /* On values the asymmetry changes no answer: each conversion fails - JSON <- ENUM has no converter ('.' in the policy
   * matrix), and ENUM <- JSON (tp_value_convert_json_scalar_to_enumeration) converts into the ENUM type's default
   * domain, whose empty element list matches no label - and a failed conversion answers with the rank of the two
   * types - 0, so LT in either order - not comparable, -181. The decision, the mirror and the exchanged decision
   * agree, and so does the row function tp_value_compare_with_error in both orders; btree_compare_bound_key answers
   * DB_UNK for any of them (btree.c:23112). */
  const named_value *e = find_value ("ENUM(2,'abc') utf8_bin"), *j = find_value ("JSON '\"abc\"'");
  REQUIRE (e != NULL);
  REQUIRE (j != NULL);
  DOMAIN_COMPARE_KEY ke, kj;
  domain_compare_key_of_value (&e->v, &ke);
  domain_compare_key_of_value (&j->v, &kj);
  const DOMAIN_COMPARE d = key_decision (ke, kj), r = key_decision (kj, ke);
  DOMAIN_COMPARE m;
  domain_compare_mirror (&d, &m);
  const std::string by_decision = show (decided_outcome (d, &e->v, &j->v));
  const std::string by_mirror = show (decided_outcome (m, &j->v, &e->v));
  const std::string by_exchanged = show (decided_outcome (r, &j->v, &e->v));
  outcome row_ej = { 0, true, NO_ERROR }, row_je = { 0, true, NO_ERROR };
  er_clear ();
  row_ej.result = tp_value_compare_with_error (&e->v, &j->v, 1, 1, &row_ej.can_compare);
  row_ej.error = er_errid ();
  er_clear ();
  row_je.result = tp_value_compare_with_error (&j->v, &e->v, 1, 1, &row_je.can_compare);
  row_je.error = er_errid ();
  if (capture)
    {
      printf ("R7-06 enum-json: decision %s / mirror on (j, e) %s / exchanged decision on (j, e) %s / row (e, j) %s / "
	      "row (j, e) %s\n", by_decision.c_str (), by_mirror.c_str (), by_exchanged.c_str (), show (row_ej).c_str (),
	      show (row_je).c_str ());
      return;
    }
  CHECK (by_decision == "LT/not comparable/er -181");
  CHECK (by_mirror == "LT/not comparable/er -181");
  CHECK (by_exchanged == "LT/not comparable/er -181");
  CHECK (show (row_ej) == "LT/not comparable/er -181");
  CHECK (show (row_je) == "LT/not comparable/er -181");
}

TEST_CASE ("PR8213 R7-06 (latent): keys whose codeset is not their collation's - the mirror is not the exchanged "
	   "decision where LANG_RT_COMMON_COLL depends on the order (two binary collations of one codeset) or an ENUM's "
	   "codeset side does (PASS = asymmetric; no domain gives such a key)", "[defect]")
{
  REQUIRE (boot_types ());
  build_values ();
  /* two codeset-off string keys, an off-codeset ENUM key, and table keys of their codesets and collations */
  const struct
  {
    const char *name;
    DOMAIN_COMPARE_KEY key;
    bool off;
  } keys[] = {
    {"VARCHAR cs=utf8 coll=iso_bin", {DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_ISO_BINARY}, true},
    {"VARCHAR cs=iso coll=utf8_bin", {DB_TYPE_VARCHAR, INTL_CODESET_ISO88591, LANG_COLL_UTF8_BINARY}, true},
    {"ENUM cs=utf8 coll=iso_bin", {DB_TYPE_ENUMERATION, INTL_CODESET_UTF8, LANG_COLL_ISO_BINARY}, true},
    {"VARCHAR utf8_bin", {DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY}, false},
    {"VARCHAR iso_bin", {DB_TYPE_VARCHAR, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY}, false},
    {"CHAR utf8_en_ci", {DB_TYPE_CHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_EN_CI}, false},
    {"INT", {DB_TYPE_INTEGER, -1, -1}, false},
  };
  /* the pairs whose mirror is not the exchanged decision, as found at 76ba766e3 */
  const char *const asymmetric[][2] = {
    /* LANG_RT_COMMON_COLL (language_support.h:65): two coercible collations merge only into a second ISO binary */
    {"VARCHAR cs=utf8 coll=iso_bin", "VARCHAR utf8_bin"}, {"VARCHAR utf8_bin", "VARCHAR cs=utf8 coll=iso_bin"},
    {"VARCHAR cs=iso coll=utf8_bin", "VARCHAR iso_bin"}, {"VARCHAR iso_bin", "VARCHAR cs=iso coll=utf8_bin"},
    /* an off-codeset ENUM against a string: codeset_side is side 0 when both sides differ from the ENUM's collation's
     * codeset (domain_compute_comparison), so each order brings its own first side */
    {"ENUM cs=utf8 coll=iso_bin", "VARCHAR utf8_bin"}, {"VARCHAR utf8_bin", "ENUM cs=utf8 coll=iso_bin"},
    {"ENUM cs=utf8 coll=iso_bin", "CHAR utf8_en_ci"}, {"CHAR utf8_en_ci", "ENUM cs=utf8 coll=iso_bin"},
  };
  const int n_keys = (int) (sizeof (keys) / sizeof (keys[0]));
  const int n_asymmetric = (int) (sizeof (asymmetric) / sizeof (asymmetric[0]));
  int differ = 0, expected_hits = 0;
  for (int i = 0; i < n_keys; i++)
    {
      for (int k = 0; k < n_keys; k++)
	{
	  const DOMAIN_COMPARE d = key_decision (keys[i].key, keys[k].key), r = key_decision (keys[k].key, keys[i].key);
	  DOMAIN_COMPARE m;
	  domain_compare_mirror (&d, &m);
	  const bool symmetric = domain_compare_equal (&m, &r);
	  bool listed = false;
	  for (int a = 0; a < n_asymmetric; a++)
	    {
	      listed = listed || (strcmp (asymmetric[a][0], keys[i].name) == 0
				  && strcmp (asymmetric[a][1], keys[k].name) == 0);
	    }
	  INFO (keys[i].name << " vs " << keys[k].name << "\n  decided   " << decision_text (d) << "\n  mirrored  "
		<< decision_text (m) << "\n  exchanged " << decision_text (r));
	  CHECK (symmetric == !listed);
	  /* every asymmetric pair has a codeset-off key: the table's keys are K10's */
	  CHECK ((symmetric || keys[i].off || keys[k].off));
	  differ += !symmetric;
	  expected_hits += listed && !symmetric;
	}
    }
  CHECK (differ == n_asymmetric);
  CHECK (expected_hits == n_asymmetric);

  /* on values the mirror still reverses the decision (the B-tree stays consistent): both answer -1150, where the
   * exchanged decision merges into ISO binary and finds '12' = '12' - the order dependence tp_value_compare_with_error
   * has itself (domain_resolve.compare_collation K4) */
  const named_value *off = find_value ("VARCHAR cs=utf8 coll=iso_bin '12'"), *bin = find_value ("VARCHAR utf8_bin '12'");
  REQUIRE (off != NULL);
  REQUIRE (bin != NULL);
  DOMAIN_COMPARE_KEY ko, kb;
  domain_compare_key_of_value (&off->v, &ko);
  domain_compare_key_of_value (&bin->v, &kb);
  const DOMAIN_COMPARE d = key_decision (ko, kb), r = key_decision (kb, ko);
  DOMAIN_COMPARE m;
  domain_compare_mirror (&d, &m);
  const std::string by_decision = show (decided_outcome (d, &off->v, &bin->v));
  const std::string by_mirror = show (decided_outcome (m, &bin->v, &off->v));
  const std::string by_exchanged = show (decided_outcome (r, &bin->v, &off->v));
  outcome row_ob = { 0, true, NO_ERROR }, row_bo = { 0, true, NO_ERROR };
  er_clear ();
  row_ob.result = tp_value_compare_with_error (&off->v, &bin->v, 1, 1, &row_ob.can_compare);
  row_ob.error = er_errid ();
  er_clear ();
  row_bo.result = tp_value_compare_with_error (&bin->v, &off->v, 1, 1, &row_bo.can_compare);
  row_bo.error = er_errid ();
  if (capture)
    {
      printf ("R7-06 latent codeset-off: decision %s / mirror on (b, a) %s / exchanged decision on (b, a) %s / "
	      "row (a, b) %s / row (b, a) %s\n", by_decision.c_str (), by_mirror.c_str (), by_exchanged.c_str (),
	      show (row_ob).c_str (), show (row_bo).c_str ());
      return;
    }
  CHECK (by_decision == "UNK/not comparable/er -1150");
  CHECK (by_mirror == "UNK/not comparable/er -1150");
  CHECK (by_exchanged == "EQ/comparable/er 0");
  CHECK (show (row_ob) == "UNK/not comparable/er -1150");
  CHECK (show (row_bo) == "EQ/comparable/er 0");
}

/* ---------------------------------------------------------------- K11 */

struct key_pair_case
{
  const char *what;
  const TP_DOMAIN *domain;
  DB_VALUE a, b;
  bool equal;			/* what develop's key rule answers: mht_compare_dbvalues_are_identical */
};

static void
check_key_pairs (std::vector<key_pair_case> &cases)
{
  for (key_pair_case & c : cases)
    {
      const bool head = domain_key_values_equal (c.domain, &c.a, &c.b);
      const bool develop = mht_compare_dbvalues_are_identical (&c.a, &c.b) != 0;
      INFO (c.what << ": domain_key_values_equal " << head << ", mht_compare_dbvalues_are_identical " << develop
	    << ", expected " << c.equal);
      CHECK (develop == c.equal);
      CHECK (head == develop);
    }
}

static DB_VALUE
v_int (int i)
{
  DB_VALUE v;
  db_make_int (&v, i);
  return v;
}

static DB_VALUE
v_double (double d)
{
  DB_VALUE v;
  db_make_double (&v, d);
  return v;
}

static DB_VALUE
v_oid (int volid, int pageid, int slotid)
{
  OID oid;
  oid.volid = (short) volid;
  oid.pageid = pageid;
  oid.slotid = (short) slotid;
  DB_VALUE v;
  db_make_oid (&v, &oid);
  return v;
}

static DB_VALUE
v_null (DB_TYPE type = DB_TYPE_NULL)
{
  DB_VALUE v;
  if (type == DB_TYPE_NULL)
    {
      db_make_null (&v);
    }
  else
    {
      db_value_domain_init (&v, type, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
    }
  return v;
}

static DB_VALUE
v_enum (int index, const char *label)
{
  DB_VALUE v;
  db_make_enumeration (&v, (unsigned short) index, label, (int) strlen (label), INTL_CODESET_UTF8,
		       LANG_COLL_UTF8_BINARY);
  return v;
}

TEST_CASE ("K11 cache key: two values of one key position whose type and collation are its domain's are equal as "
	   "mht_compare_dbvalues_are_identical says (develop's memoize and subquery cache key rule)", "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE date1, date2, dt1, dt2, bi1, bi2;
  db_make_date (&date1, 1, 2, 2020);
  db_make_date (&date2, 1, 2, 2020);
  {
    DB_DATETIME dt;
    db_datetime_encode (&dt, 1, 2, 2020, 10, 11, 12, 0);
    db_make_datetime (&dt1, &dt);
    db_datetime_encode (&dt, 1, 2, 2020, 10, 11, 12, 1);
    db_make_datetime (&dt2, &dt);
  }
  db_make_bigint (&bi1, 1099511627776LL);
  db_make_bigint (&bi2, 1099511627776LL);
  const int ci = LANG_COLL_UTF8_EN_CI, bin = LANG_COLL_UTF8_BINARY;
  std::vector<key_pair_case> cases = {
    {"INT 12, 12", type_domain (DB_TYPE_INTEGER), v_int (12), v_int (12), true},
    {"INT 12, 13", type_domain (DB_TYPE_INTEGER), v_int (12), v_int (13), false},
    {"BIGINT 2^40, 2^40", type_domain (DB_TYPE_BIGINT), bi1, bi2, true},
    {"DOUBLE 12.5, 12.5", type_domain (DB_TYPE_DOUBLE), v_double (12.5), v_double (12.5), true},
    {"DATE 2020-01-02, 2020-01-02", type_domain (DB_TYPE_DATE), date1, date2, true},
    {"DATETIME .000, .001", type_domain (DB_TYPE_DATETIME), dt1, dt2, false},
    {"VARCHAR utf8_bin 'abc', 'abc'", vc_domain (DB_MAX_VARCHAR_PRECISION, bin),
     varchar ("abc", INTL_CODESET_UTF8, bin), varchar ("abc", INTL_CODESET_UTF8, bin), true},
    {"VARCHAR utf8_bin 'abc', 'abd'", vc_domain (DB_MAX_VARCHAR_PRECISION, bin),
     varchar ("abc", INTL_CODESET_UTF8, bin), varchar ("abd", INTL_CODESET_UTF8, bin), false},
    /* equal under the collation, two keys: ASCII (key) tells them apart */
    {"VARCHAR utf8_en_ci 'A', 'a'", vc_domain (DB_MAX_VARCHAR_PRECISION, ci), varchar ("A", INTL_CODESET_UTF8, ci),
     varchar ("a", INTL_CODESET_UTF8, ci), false},
    {"CHAR(5) utf8_bin 'ab', 'ab'", char_domain (5, bin), fixed_char ("ab", 5, INTL_CODESET_UTF8, bin),
     fixed_char ("ab", 5, INTL_CODESET_UTF8, bin), true},
  };
  check_key_pairs (cases);
}

TEST_CASE ("K11 boundary: NULLs, trailing spaces, NUMERIC scales, -0.0, NaN, MONETARY currencies, OIDs of an OBJECT "
	   "position, ENUM labels, and two values outside the claim that answer alike", "[contract]")
{
  REQUIRE (boot_types ());
  const int bin = LANG_COLL_UTF8_BINARY;
  static unsigned char bits8[] = { 0x12 }, bits8b[] = { 0x13 };
  DB_VALUE vb1, vb2, vb3, money1, money2, bigint12;
  db_make_varbit (&vb1, 64, (DB_CONST_C_BIT) bits8, 8);
  db_make_varbit (&vb2, 64, (DB_CONST_C_BIT) bits8, 8);
  db_make_varbit (&vb3, 64, (DB_CONST_C_BIT) bits8b, 8);
  db_make_monetary (&money1, DB_CURRENCY_DOLLAR, 12.0);
  db_make_monetary (&money2, DB_CURRENCY_WON, 12.0);
  db_make_bigint (&bigint12, 12);
  std::vector<key_pair_case> cases = {
    /* NULL is one key: equal to a NULL, to nothing else (memoize hashes a NULL too) */
    {"NULL, NULL", type_domain (DB_TYPE_INTEGER), v_null (), v_null (), true},
    {"NULL, INT 1", type_domain (DB_TYPE_INTEGER), v_null (), v_int (1), false},
    {"INT 1, NULL", type_domain (DB_TYPE_INTEGER), v_int (1), v_null (), false},
    {"typed NULL VARCHAR, 'a'", vc_domain (DB_MAX_VARCHAR_PRECISION, bin), v_null (DB_TYPE_VARCHAR),
     varchar ("a", INTL_CODESET_UTF8, bin), false},
    {"typed NULL VARCHAR, typed NULL VARCHAR", vc_domain (DB_MAX_VARCHAR_PRECISION, bin), v_null (DB_TYPE_VARCHAR),
     v_null (DB_TYPE_VARCHAR), true},
    /* bytes: a trailing space is another key (LENGTH (key) differs) */
    {"VARCHAR utf8_bin 'a', 'a '", vc_domain (DB_MAX_VARCHAR_PRECISION, bin), varchar ("a", INTL_CODESET_UTF8, bin),
     varchar ("a ", INTL_CODESET_UTF8, bin), false},
    {"CHAR(5) utf8_bin 'ab' (2 bytes), 'ab   ' (5 bytes)", char_domain (5, bin),
     fixed_char ("ab", 5, INTL_CODESET_UTF8, bin), fixed_char ("ab   ", 5, INTL_CODESET_UTF8, bin), false},
    {"VARCHAR utf8_bin '', ''", vc_domain (DB_MAX_VARCHAR_PRECISION, bin), varchar ("", INTL_CODESET_UTF8, bin),
     varchar ("", INTL_CODESET_UTF8, bin), true},
    /* a value: NUMERIC 12.00 and 12 are one key, -0.0 and 0.0 are one key */
    {"NUMERIC(4,2) 12.00, NUMERIC(2,0) 12", numeric_domain (4, 2), numeric ("12.00", 4, 2), numeric ("12", 2, 0), true},
    {"NUMERIC(4,2) 12.00, 12.01", numeric_domain (4, 2), numeric ("12.00", 4, 2), numeric ("12.01", 4, 2), false},
    {"DOUBLE 0.0, -0.0", type_domain (DB_TYPE_DOUBLE), v_double (0.0), v_double (-0.0), true},
    {"DOUBLE NaN, NaN", type_domain (DB_TYPE_DOUBLE), v_double (std::nan ("")), v_double (std::nan ("")), true},
    {"MONETARY $12, W12", type_domain (DB_TYPE_MONETARY), money1, money2, true},
    /* an OBJECT position's values are OIDs on the server (domain_key_value_domain) */
    {"OBJECT position: OID 1|2|3, 1|2|3", type_domain (DB_TYPE_OBJECT), v_oid (1, 2, 3), v_oid (1, 2, 3), true},
    {"OBJECT position: OID 1|2|3, 1|2|4", type_domain (DB_TYPE_OBJECT), v_oid (1, 2, 3), v_oid (1, 2, 4), false},
    /* an ENUM compares by its index */
    {"ENUM 2 'abc', 2 'abc'", type_domain (DB_TYPE_ENUMERATION), v_enum (2, "abc"), v_enum (2, "abc"), true},
    {"ENUM 1 'abc', 2 'abc'", type_domain (DB_TYPE_ENUMERATION), v_enum (1, "abc"), v_enum (2, "abc"), false},
    {"ENUM 2 'abc', 2 'xyz' (one index, two labels)", type_domain (DB_TYPE_ENUMERATION), v_enum (2, "abc"),
     v_enum (2, "xyz"), true},
    {"VARBIT x12, x12", type_domain (DB_TYPE_VARBIT), vb1, vb2, true},
    {"VARBIT x12, x13", type_domain (DB_TYPE_VARBIT), vb1, vb3, false},
    /* outside the claim (release: tp_value_compare at domain_rules.c:1909), the same answer as develop's: a number of
     * another type by value, and a VARCHAR 'a' against a CHAR 'a ' that the VARCHAR's cmpval does not find equal */
    {"INT position: BIGINT 12, INT 12", type_domain (DB_TYPE_INTEGER), bigint12, v_int (12), true},
    {"VARCHAR utf8_bin position: VARCHAR 'a', CHAR(2) 'a '", vc_domain (DB_MAX_VARCHAR_PRECISION, bin),
     varchar ("a", INTL_CODESET_UTF8, bin), fixed_char ("a ", 2, INTL_CODESET_UTF8, bin), false},
  };
  check_key_pairs (cases);
}

TEST_CASE ("PR8213 R7-02: (latent) a string key outside the compiler's claim - a value whose type or collation is not "
	   "its position's domain's, a LEAVE or VARIABLE domain - is compared under the collation where develop compared "
	   "the bytes (PASS = defect present)", "[defect]")
{
  REQUIRE (boot_types ());
  const int ci = LANG_COLL_UTF8_EN_CI, bin = LANG_COLL_UTF8_BINARY;
  const TP_DOMAIN *leave = tp_domain_resolve_default_w_coll (DB_TYPE_VARCHAR, bin, TP_DOMAIN_COLL_LEAVE);
  REQUIRE (TP_DOMAIN_COLLATION_FLAG (leave) == TP_DOMAIN_COLL_LEAVE);
  struct
  {
    const char *what;
    const TP_DOMAIN *domain;
    DB_VALUE a, b;
  } cases[] = {
    {"VARCHAR utf8_en_ci position, CHAR(1) utf8_en_ci values 'A', 'a'", vc_domain (DB_MAX_VARCHAR_PRECISION, ci),
     fixed_char ("A", 1, INTL_CODESET_UTF8, ci), fixed_char ("a", 1, INTL_CODESET_UTF8, ci)},
    {"VARCHAR utf8_bin position, utf8_en_ci values 'A', 'a'", vc_domain (DB_MAX_VARCHAR_PRECISION, bin),
     varchar ("A", INTL_CODESET_UTF8, ci), varchar ("a", INTL_CODESET_UTF8, ci)},
    {"LEAVE VARCHAR position, utf8_en_ci values 'A', 'a'", leave, varchar ("A", INTL_CODESET_UTF8, ci),
     varchar ("a", INTL_CODESET_UTF8, ci)},
    {"VARIABLE position, utf8_en_ci values 'A', 'a'", type_domain (DB_TYPE_VARIABLE),
     varchar ("A", INTL_CODESET_UTF8, ci), varchar ("a", INTL_CODESET_UTF8, ci)},
  };
  int differ = 0;
  for (auto & c : cases)
    {
      const bool head = domain_key_values_equal (c.domain, &c.a, &c.b);
      const bool develop = mht_compare_dbvalues_are_identical (&c.a, &c.b) != 0;
      if (capture)
	{
	  printf ("R7-02 %s: head %d develop %d\n", c.what, head, develop);
	}
      INFO (c.what << ": domain_key_values_equal " << head << ", develop's key rule " << develop);
      /* one key here (equal under utf8_en_ci), two for develop (the bytes differ) */
      CHECK (head);
      CHECK (!develop);
      differ += head != develop;
    }
  CHECK (differ == (int) (sizeof (cases) / sizeof (cases[0])));
}

/* ---------------------------------------------------------------- K12 */

/* develop's hjoin_init_domain_info for one key column (query_hash_join.c:1020-1138 at 2d925cb86, copied rule for
 * rule; its NULL-against-a-type branches are assert (false) there); need_coerce carries over from the columns before
 * it. NULL: hashed as it is */
static const TP_DOMAIN *
develop_hash_key (const TP_DOMAIN * outer, const TP_DOMAIN * inner, bool *need_coerce)
{
  const DB_TYPE outer_type = TP_DOMAIN_TYPE (outer), inner_type = TP_DOMAIN_TYPE (inner);
  DB_TYPE common_type;
  if (outer_type == inner_type)
    {
      common_type = outer_type;
    }
  else
    {
      *need_coerce = true;
      if (outer_type == DB_TYPE_NULL)
	{
	  return inner;
	}
      if (inner_type == DB_TYPE_NULL)
	{
	  return outer;
	}
      common_type = tp_more_general_type (outer_type, inner_type) > 0 ? outer_type : inner_type;
    }
  const int op = outer->precision, os = outer->scale, ip = inner->precision, is = inner->scale;
  int cp, cs;
  if (op == ip && os == is)
    {
      cp = ip;
      cs = is;
    }
  else
    {
      *need_coerce = true;
      if (op == TP_FLOATING_PRECISION_VALUE || ip == TP_FLOATING_PRECISION_VALUE)
	{
	  cp = TP_FLOATING_PRECISION_VALUE;
	  cs = 0;
	}
      else if (common_type == DB_TYPE_NUMERIC)
	{
	  if (op == DB_DEFAULT_NUMERIC_PRECISION || ip == DB_DEFAULT_NUMERIC_PRECISION)
	    {
	      cp = DB_DEFAULT_NUMERIC_PRECISION;
	      cs = DB_DEFAULT_NUMERIC_SCALE;
	    }
	  else
	    {
	      cs = MAX (os, is);
	      cp = MAX (op - os, ip - is) + cs;
	      if (cp > DB_MAX_FIXED_NUMERIC_PRECISION)
		{
		  cp = DB_DEFAULT_NUMERIC_PRECISION;
		  cs = DB_DEFAULT_NUMERIC_SCALE;
		}
	    }
	}
      else
	{
	  cp = MAX (op, ip);
	  cs = 0;
	}
    }
  if (!*need_coerce)
    {
      return NULL;
    }
  if (common_type == outer_type && cp == op && cs == os)
    {
      return outer;
    }
  if (common_type == inner_type && cp == ip && cs == is)
    {
      return inner;
    }
  TP_DOMAIN *copy = tp_domain_copy (common_type == outer_type ? outer : inner, false);
  REQUIRE (copy != NULL);
  copy->precision = cp;
  copy->scale = cs;
  return tp_domain_cache (copy);
}

struct named_domain
{
  std::string name;
  const TP_DOMAIN *domain;
};

static std::vector<named_domain>
hash_key_domains ()
{
  const int bin = LANG_COLL_UTF8_BINARY;
  std::vector<named_domain> d = {
    {"SHORT", type_domain (DB_TYPE_SHORT)}, {"INT", type_domain (DB_TYPE_INTEGER)},
    {"BIGINT", type_domain (DB_TYPE_BIGINT)}, {"FLOAT", type_domain (DB_TYPE_FLOAT)},
    {"DOUBLE", type_domain (DB_TYPE_DOUBLE)}, {"MONETARY", type_domain (DB_TYPE_MONETARY)},
    {"NUMERIC(4,2)", numeric_domain (4, 2)}, {"NUMERIC(10,0)", numeric_domain (10, 0)},
    {"NUMERIC(10,4)", numeric_domain (10, 4)}, {"NUMERIC(38,0)", numeric_domain (38, 0)},
    {"NUMERIC(38,2)", numeric_domain (38, 2)}, {"NUMERIC default (floating)", type_domain (DB_TYPE_NUMERIC)},
    {"CHAR(5) utf8_bin", char_domain (5, bin)}, {"CHAR(10) utf8_bin", char_domain (10, bin)},
    {"VARCHAR(10) utf8_bin", vc_domain (10, bin)}, {"VARCHAR(20) utf8_bin", vc_domain (20, bin)},
    {"VARCHAR(10) utf8_en_ci", vc_domain (10, LANG_COLL_UTF8_EN_CI)},
    {"VARCHAR(10) iso_bin", vc_domain (10, LANG_COLL_ISO_BINARY)},
    {"VARCHAR max utf8_bin", vc_domain (DB_MAX_VARCHAR_PRECISION, bin)},
    {"VARCHAR(-1) utf8_bin", vc_domain (TP_FLOATING_PRECISION_VALUE, bin)},
    {"BIT(8)", tp_domain_resolve (DB_TYPE_BIT, NULL, 8, 0, NULL, 0)},
    {"VARBIT(16)", tp_domain_resolve (DB_TYPE_VARBIT, NULL, 16, 0, NULL, 0)},
    {"DATE", type_domain (DB_TYPE_DATE)}, {"TIME", type_domain (DB_TYPE_TIME)},
    {"TIMESTAMP", type_domain (DB_TYPE_TIMESTAMP)}, {"DATETIME", type_domain (DB_TYPE_DATETIME)},
    {"TIMESTAMPTZ", type_domain (DB_TYPE_TIMESTAMPTZ)}, {"TIMESTAMPLTZ", type_domain (DB_TYPE_TIMESTAMPLTZ)},
    {"DATETIMETZ", type_domain (DB_TYPE_DATETIMETZ)}, {"DATETIMELTZ", type_domain (DB_TYPE_DATETIMELTZ)},
    {"ENUM", type_domain (DB_TYPE_ENUMERATION)}, {"JSON", type_domain (DB_TYPE_JSON)},
    {"OID", type_domain (DB_TYPE_OID)}, {"SET", type_domain (DB_TYPE_SET)},
    {"SEQUENCE", type_domain (DB_TYPE_SEQUENCE)}, {"BLOB", type_domain (DB_TYPE_BLOB)},
    {"CLOB", type_domain (DB_TYPE_CLOB)},
  };
  for (const named_domain & x : d)
    {
      INFO (x.name);
      REQUIRE (x.domain != NULL);
    }
  return d;
}

/* the pairs the two rules part on by design: a string against a number or a date */
static bool
string_against_number_or_date (DB_TYPE a, DB_TYPE b)
{
  auto other = [] (DB_TYPE t)
  {
    return TP_IS_NUMERIC_TYPE (t) || TP_IS_DATE_OR_TIME_TYPE (t);
  };
  return (TP_IS_CHAR_TYPE (a) && other (b)) || (TP_IS_CHAR_TYPE (b) && other (a));
}

/* the type such a pair's comparison converts both sides to (tp_value_compare_common_domain): DOUBLE for a string
 * against a number (TO_DOUBLE), the date side's type for a string against a date (FIRST/SECOND_TO_DATE) */
static DB_TYPE
comparison_target_type (DB_TYPE a, DB_TYPE b)
{
  if ((TP_IS_CHAR_TYPE (a) && TP_IS_NUMERIC_TYPE (b)) || (TP_IS_NUMERIC_TYPE (a) && TP_IS_CHAR_TYPE (b)))
    {
      return DB_TYPE_DOUBLE;
    }
  return TP_IS_CHAR_TYPE (a) ? b : a;
}

TEST_CASE ("K12 hash key domain: domain_resolve_hash_key gives develop's hjoin_init_domain_info domain for every pair "
	   "but a string against a number or a date, which hashes in the built-in domain of the comparison's type",
	   "[contract]")
{
  REQUIRE (boot_types ());
  const std::vector<named_domain> d = hash_key_domains ();
  int same = 0, parted = 0;
  for (const named_domain & outer : d)
    {
      for (const named_domain & inner : d)
	{
	  const TP_DOMAIN *head = NULL;
	  const int error = domain_resolve_hash_key (outer.domain, inner.domain, &head);
	  bool coerce = false;
	  const TP_DOMAIN *develop = develop_hash_key (outer.domain, inner.domain, &coerce);
	  /* develop hashes a pair of one domain as it is; domain_resolve_hash_key answers first then
	   * (domain_decide_hash_join_key turns it into no common domain) */
	  const TP_DOMAIN *develop_common = develop != NULL ? develop : outer.domain;
	  INFO (outer.name << " / " << inner.name << ": head " << domain_text (error, head) << ", develop "
		<< domain_text (NO_ERROR, develop_common));
	  REQUIRE (error == NO_ERROR);
	  const DB_TYPE ot = TP_DOMAIN_TYPE (outer.domain), it = TP_DOMAIN_TYPE (inner.domain);
	  if (string_against_number_or_date (ot, it))
	    {
	      parted++;
	      /* develop: the string type (more general than any number or date), precisions merged */
	      CHECK (TP_IS_CHAR_TYPE (TP_DOMAIN_TYPE (develop_common)));
	      CHECK (head == tp_domain_resolve_default (comparison_target_type (ot, it)));
	      continue;
	    }
	  CHECK (head == develop_common);
	  same++;
	}
    }
  INFO ("same " << same << ", string against number or date " << parted);
  CHECK (same + parted == (int) (d.size () * d.size ()));
  CHECK (parted > 200);
}

TEST_CASE ("K12 boundary: string vs number in DOUBLE, string vs date in the date type, NUMERIC past 38 digits in the "
	   "floating NUMERIC, a floating precision, and the coercion carried to the keys after a coerced one",
	   "[contract]")
{
  REQUIRE (boot_types ());
  const int bin = LANG_COLL_UTF8_BINARY;
  struct
  {
    const char *what;
    const TP_DOMAIN *outer, *inner;
    const char *head, *develop;
  } cases[] = {
    /* parted: the comparison's type (TO_DOUBLE / the date), develop's the more general type */
    {"INT, VARCHAR(10)", type_domain (DB_TYPE_INTEGER), vc_domain (10, bin), "double(15,0) coll=-1",
     "character varying(10,0) coll=1"},
    {"VARCHAR(10), NUMERIC(10,4)", vc_domain (10, bin), numeric_domain (10, 4), "double(15,0) coll=-1",
     "character varying(10,0) coll=1"},
    {"VARCHAR(10), DATE", vc_domain (10, bin), type_domain (DB_TYPE_DATE), "date(10,0) coll=-1",
     "character varying(10,0) coll=1"},
    {"DATETIME, VARCHAR max", type_domain (DB_TYPE_DATETIME), vc_domain (DB_MAX_VARCHAR_PRECISION, bin),
     "datetime(23,3) coll=-1", "character varying(1073741823,0) coll=1"},
    /* NUMERIC: integral digits and scale merged, as develop */
    {"NUMERIC(4,2), NUMERIC(10,4)", numeric_domain (4, 2), numeric_domain (10, 4), "numeric(10,4) coll=-1",
     "numeric(10,4) coll=-1"},
    {"INT, NUMERIC(4,2)", type_domain (DB_TYPE_INTEGER), numeric_domain (4, 2), "numeric(12,2) coll=-1",
     "numeric(12,2) coll=-1"},
    {"BIGINT, NUMERIC(38,2)", type_domain (DB_TYPE_BIGINT), numeric_domain (38, 2), "numeric(38,2) coll=-1",
     "numeric(38,2) coll=-1"},
    /* past 38 digits: the floating NUMERIC (precision DB_DEFAULT_NUMERIC_PRECISION 40, CBRD-27575), as develop.
     * The hash join keeps it (the floating key recomputes a fixed value's precision, query_hash_join.c:2839-2850);
     * the hash list scan's pt_hash_key_domain (xasl_generation.c:7186-7194, ca80ac5f9) turns its hash off on exactly
     * this common domain of two fixed NUMERICs - a static of the client TU, not run here */
    {"NUMERIC(38,0), NUMERIC(38,2)", numeric_domain (38, 0), numeric_domain (38, 2), "numeric(40,0) coll=-1",
     "numeric(40,0) coll=-1"},
    /* a floating precision on one side is floating */
    {"VARCHAR(-1), VARCHAR(10)", vc_domain (TP_FLOATING_PRECISION_VALUE, bin), vc_domain (10, bin),
     "character varying(-1,0) coll=1", "character varying(-1,0) coll=1"},
    /* numbers and dates: the more general */
    {"INT, BIGINT", type_domain (DB_TYPE_INTEGER), type_domain (DB_TYPE_BIGINT), "bigint(19,0) coll=-1",
     "bigint(19,0) coll=-1"},
    {"FLOAT, DOUBLE", type_domain (DB_TYPE_FLOAT), type_domain (DB_TYPE_DOUBLE), "double(15,0) coll=-1",
     "double(15,0) coll=-1"},
    {"DATE, DATETIME", type_domain (DB_TYPE_DATE), type_domain (DB_TYPE_DATETIME), "datetime(23,3) coll=-1",
     "datetime(23,3) coll=-1"},
  };
  for (auto & c : cases)
    {
      const TP_DOMAIN *head = NULL;
      const int error = domain_resolve_hash_key (c.outer, c.inner, &head);
      bool coerce = false;
      const TP_DOMAIN *develop = develop_hash_key (c.outer, c.inner, &coerce);
      const std::string h = domain_text (error, head), dv = domain_text (NO_ERROR, develop != NULL ? develop : c.outer);
      if (capture)
	{
	  printf ("K12b {\"%s\", \"%s\", \"%s\"}\n", c.what, h.c_str (), dv.c_str ());
	  continue;
	}
      INFO (c.what);
      CHECK (h == c.head);
      CHECK (dv == c.develop);
    }
  {
    /* the floating NUMERIC is the common domain pt_hash_key_domain's hash-off condition reads */
    const TP_DOMAIN *head = NULL;
    REQUIRE (domain_resolve_hash_key (numeric_domain (38, 0), numeric_domain (38, 2), &head) == NO_ERROR);
    CHECK (TP_DOMAIN_TYPE (head) == DB_TYPE_NUMERIC);
    CHECK (head->precision == DB_DEFAULT_NUMERIC_PRECISION);
  }

  /* domain_decide_hash_join_key over two keys: a coerced first key coerces the second as well, into the domain
   * develop's loop-carried need_coerce_domains gave it; alone, the second is hashed as it is */
  const TP_DOMAIN *vc10 = vc_domain (10, bin), *integer = type_domain (DB_TYPE_INTEGER);
  const TP_DOMAIN *bigint = type_domain (DB_TYPE_BIGINT);
  bool coerce = false, develop_coerce = false;
  const TP_DOMAIN *common = NULL;
  int conv_kind[2];
  DOMAIN_COMPARE compare;
  REQUIRE (domain_decide_hash_join_key (integer, bigint, &coerce, &common, conv_kind, &compare) == NO_ERROR);
  CHECK (coerce);
  CHECK (common == develop_hash_key (integer, bigint, &develop_coerce));
  CHECK (conv_kind[0] == DOMAIN_HASH_KEY_CONVERTED);
  CHECK (conv_kind[1] == DOMAIN_HASH_KEY_AS_IS);
  REQUIRE (domain_decide_hash_join_key (vc10, vc10, &coerce, &common, conv_kind, &compare) == NO_ERROR);
  CHECK (coerce);
  CHECK (common == develop_hash_key (vc10, vc10, &develop_coerce));
  CHECK (common == vc10);
  CHECK (conv_kind[0] == DOMAIN_HASH_KEY_AS_IS);
  CHECK (conv_kind[1] == DOMAIN_HASH_KEY_AS_IS);
  coerce = develop_coerce = false;
  REQUIRE (domain_decide_hash_join_key (vc10, vc10, &coerce, &common, conv_kind, &compare) == NO_ERROR);
  CHECK (!coerce);
  CHECK (common == NULL);
  CHECK (develop_hash_key (vc10, vc10, &develop_coerce) == NULL);
  /* a JSON side is coerced at the row (tp_value_coerce converts it by its scalar), as develop coerced every value */
  coerce = false;
  REQUIRE (domain_decide_hash_join_key (type_domain (DB_TYPE_JSON), vc10, &coerce, &common, conv_kind, &compare)
	   == NO_ERROR);
  CHECK (coerce);
  CHECK (common == vc10);
  CHECK (conv_kind[0] == DOMAIN_HASH_KEY_COERCED);
  CHECK (conv_kind[1] == DOMAIN_HASH_KEY_AS_IS);
}

TEST_CASE ("K12 a pair of NULL-typed key columns (76ba766e3): decided as the NULL domain against itself - hashed as "
	   "they are, compared by their values - as develop hashed one domain as it was", "[contract]")
{
  REQUIRE (boot_types ());
  const TP_DOMAIN *null_domain = tp_domain_resolve_default (DB_TYPE_NULL);
  REQUIRE (null_domain != NULL);
  const TP_DOMAIN *common = NULL;
  REQUIRE (domain_resolve_hash_key (null_domain, null_domain, &common) == NO_ERROR);
  CHECK (common == null_domain);

  /* alone: no common domain, nothing converted, the values compared by themselves (their keys are NULL: the hash join
   * skips a tuple with a NULL key before it hashes, query_hash_join.c:2833-2836) */
  bool coerce = false, develop_coerce = false;
  int conv_kind[2] = { -1, -1 };
  DOMAIN_COMPARE compare;
  REQUIRE (domain_decide_hash_join_key (null_domain, null_domain, &coerce, &common, conv_kind, &compare) == NO_ERROR);
  CHECK (!coerce);
  CHECK (common == NULL);
  CHECK (conv_kind[0] == DOMAIN_HASH_KEY_AS_IS);
  CHECK (conv_kind[1] == DOMAIN_HASH_KEY_AS_IS);
  CHECK (compare.method == DOMAIN_COMPARE_VALUES);
  CHECK (develop_hash_key (null_domain, null_domain, &develop_coerce) == NULL);
  CHECK (!develop_coerce);

  /* after a coerced key: the NULL domain itself, which develop's carried need_coerce_domains gave it as well */
  coerce = develop_coerce = true;
  REQUIRE (domain_decide_hash_join_key (null_domain, null_domain, &coerce, &common, conv_kind, &compare) == NO_ERROR);
  CHECK (coerce);
  CHECK (common == null_domain);
  CHECK (conv_kind[0] == DOMAIN_HASH_KEY_AS_IS);
  CHECK (conv_kind[1] == DOMAIN_HASH_KEY_AS_IS);
  CHECK (compare.method == DOMAIN_COMPARE_VALUES);
  CHECK (develop_hash_key (null_domain, null_domain, &develop_coerce) == null_domain);
}
