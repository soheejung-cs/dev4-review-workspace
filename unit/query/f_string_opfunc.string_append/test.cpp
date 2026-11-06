/*
 * PR#8398 (CBRD-27926) -- db_string_append (), db_string_append_zero_bytes () and qstr_reserve (), src/query/string_opfunc.c.
 *
 * Before the PR, GROUP_CONCAT rebuilt its accumulator per append: qdata_concatenate_dbval () made
 * db_string_concatenate (acc, v) in a new value (qdata_add_chars_to_dbval) and qdata_group_concat_(first_)value ()
 * cloned it into the accumulator when it was not NULL. The PR appends in the accumulator's own buffer and claims the
 * accumulator ends as that flow left it (pr-body 29, 37). The develop flow is emulated here with the develop functions
 * the head TU still has unchanged (db_string_concatenate, pr_clone_value); the head flow is the PR's db_string_append.
 *
 *   E1 VARCHAR accumulator, every built-in collation: random strings (multibyte, empty), VARCHAR / fully padded CHAR /
 *      CHAR literal values -- type, precision, size, bytes, codeset, collation, NULL flag and character length agree
 *   E2 VARBIT accumulator: BIT / VARBIT values of 0..17 bits (not multiples of 8, random trailing bits)
 *   E3 oracle_style_empty_string on: empty values are NULL and add nothing; the accumulator stays non-NULL
 *   E4 two collations (ASCII text): the common collation and the codeset coercion, or the same error
 *   E5 string_max_size_bytes: the same append fails with ER_QPROC_STRING_SIZE_TOO_BIG and leaves the accumulator alike
 *   R1 qstr_reserve: the recorded capacity never exceeds the block the accumulator holds and covers size + 1;
 *      16,384 appends of 32 bytes take O(log N) allocations (develop's flow: two per append)
 *   R2 limit: with group_concat_max_len 1000 the capacity stops at 1001 unless one append needs more
 *   Z1 truncation at group_concat_max_len inside a multibyte character (spare bytes): the value after
 *      db_string_fix_string_size () (qdata_finalize_aggregate_list) is develop's
 *   The E cases treat TP_FLOATING_PRECISION_VALUE and the type's maximum precision alike (E4 after a codeset coercion
 *   of the accumulator ignores the precision); where they differ is recorded as [defect] (PASS = difference there):
 *   P1 head writes TP_FLOATING_PRECISION_VALUE where develop's db_make_varchar / db_make_varbit (db_value_domain_init)
 *      wrote DB_MAX_VARCHAR_PRECISION / DB_MAX_VARBIT_PRECISION; db_string_fix_string_size () normalizes a VARCHAR's,
 *      a VARBIT's stays
 *   P2 after the accumulator itself is coerced to another codeset its precision becomes its length + the value's
 *      (QSTR_VALUE_PRECISION of a floating precision), develop's stays the maximum
 *   C1 a BIT start value and a BIT value make a BIT (qstr_bit_concatenate), so the accumulator stays BIT
 *   D1 (develop as well) a bit string truncated at group_concat_max_len keeps new_size bits, not bytes
 *
 * Real code at THIS revision is compiled into the test binary (as unit/query/domain_resolve.convert_numeric):
 * system_parameter.c, language_support.c, intl_support.c, chartype.c, mprec.c, dtoa.c, object_primitive.c,
 * object_domain_convert.cpp (when present), numeric_opfunc.c, db_date.c, db_macro.c, string_opfunc.c.
 * Faked: error manager (last id), private allocator (malloc with a live-block size map), area allocator, boot globals.
 * The caller qdata_concatenate_dbval ()'s truncation at group_concat_max_len is copied into Z1 (query_opfunc.c is
 * not linked).
 */
#include "catch.hpp"
#if __has_include("domain_rules.h")
#include "domain_rules.h"
#define UNITSEAM_HAS_CONVERTERS 1
#endif
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
#if defined (UNITSEAM_HAS_CONVERTERS)
#include "object_domain_convert.cpp"
#undef new
#endif
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
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

/* ---------------------------------------------------------------- fakes: what a booted server process has */

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

/* DOWN: prm_get_value reads a session parameter's global value (no thread entry) */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

const char *
envvar_get (const char *)
{
  return NULL;
}

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

void
dk_deduplicate_key_attribute_initialized ()
{
}

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

/* memory_alloc.c: the private heap is malloc here; live blocks keep their size so a test can compare a recorded
 * capacity with the block it describes */
static std::unordered_map<void *, size_t> f_blocks;
static long f_allocs = 0;
static long f_allocs_4k = 0;

void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  void *p = malloc (size);
  if (p != NULL)
    {
      f_blocks[p] = size;
      f_allocs++;
      if (size >= 4096)
	{
	  f_allocs_4k++;
	}
    }
  return p;
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  if (ptr != NULL)
    {
      f_blocks.erase (ptr);
      free (ptr);
    }
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  void *p = realloc (ptr, size);
  if (p != NULL)
    {
      if (ptr != NULL)
	{
	  f_blocks.erase (ptr);
	}
      f_blocks[p] = size;
      f_allocs++;
    }
  return p;
}

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

/* ---------------------------------------------------------------- the two flows */

/* develop: qdata_concatenate_dbval () skips a NULL value; qdata_add_chars_to_dbval () = db_string_concatenate () into
 * a new value, ER_FAILED on truncation; the caller replaces the accumulator by a clone when the result is not NULL */
static int
dev_append (DB_VALUE * acc, const DB_VALUE * v)
{
  if (DB_IS_NULL (acc) || DB_IS_NULL (v))
    {
      return NO_ERROR;
    }
  DB_VALUE tmp;
  DB_DATA_STATUS st;
  db_make_null (&tmp);
  const int err = db_string_concatenate (acc, v, &tmp, &st);
  if (err != NO_ERROR || st != DATA_STATUS_OK)
    {
      pr_clear_value (&tmp);
      return err != NO_ERROR ? err : ER_FAILED;
    }
  if (!DB_IS_NULL (&tmp))
    {
      pr_clear_value (acc);
      pr_clone_value (&tmp, acc);
    }
  pr_clear_value (&tmp);
  return NO_ERROR;
}

/* head: qdata_concatenate_dbval () skips a NULL value, then db_string_append () */
static int
head_append (DB_VALUE * acc, const DB_VALUE * v, int *capacity, int max_size)
{
  if (DB_IS_NULL (acc) || DB_IS_NULL (v))
    {
      return NO_ERROR;
    }
  return db_string_append (acc, v, capacity, max_size);
}

/* qdata_group_concat_first_value (): the accumulator starts as the empty string of its type, not NULL */
static void
start_acc (DB_VALUE * acc, DB_TYPE type, int codeset, int coll)
{
  db_make_null (acc);
  db_value_domain_init (acc, type, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
  REQUIRE (db_string_make_empty_typed_string (acc, type, DB_DEFAULT_PRECISION, codeset, coll) == NO_ERROR);
  if (prm_get_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING))
    {
      acc->domain.general_info.is_null = 0;
    }
}

/* the precision db_value_domain_init () gives TP_FLOATING_PRECISION_VALUE (DB_DEFAULT_PRECISION) of a VARCHAR or VARBIT */
static int
norm_prec (int type, int prec)
{
  if (prec != TP_FLOATING_PRECISION_VALUE)
    {
      return prec;
    }
  return type == DB_TYPE_VARCHAR ? DB_MAX_VARCHAR_PRECISION : type == DB_TYPE_VARBIT ? DB_MAX_VARBIT_PRECISION : prec;
}

/* appends whose two accumulators agree but for TP_FLOATING_PRECISION_VALUE against the maximum precision */
static long g_prec_raw_diffs = 0;
static long g_prec_raw_example_dev = 0, g_prec_raw_example_head = 0;

struct snap
{
  int type, is_null, prec, size_field, codeset, coll, length;
  std::string bytes;
  /* equal but for the floating precision written as such (head) or as the maximum (develop's db_value_domain_init) */
  bool operator== (const snap & o) const
  {
    return type == o.type && is_null == o.is_null && norm_prec (type, prec) == norm_prec (o.type, o.prec)
	   && size_field == o.size_field && codeset == o.codeset && coll == o.coll && length == o.length
	   && bytes == o.bytes;
  }
  bool raw_equal (const snap & o) const
  {
    return *this == o && prec == o.prec;
  }
  bool eq_but_prec (const snap & o) const
  {
    return type == o.type && is_null == o.is_null && size_field == o.size_field && codeset == o.codeset
	   && coll == o.coll && length == o.length && bytes == o.bytes;
  }
};

/* appends whose accumulators agree but for a precision that is not the floating one in either form (E4 after a
 * codeset coercion of the accumulator) */
static long g_prec_coerce_diffs = 0;

static void
note_prec (const snap & dev, const snap & head)
{
  if (dev == head && !dev.raw_equal (head))
    {
      g_prec_raw_diffs++;
      g_prec_raw_example_dev = dev.prec;
      g_prec_raw_example_head = head.prec;
    }
}

static std::string
hex (const std::string & s)
{
  static const char *d = "0123456789abcdef";
  std::string r;
  for (unsigned char c : s)
    {
      r += d[c >> 4];
      r += d[c & 15];
    }
  return r;
}

static std::string
show (const snap & s)
{
  char head[256];
  snprintf (head, sizeof (head), "type %d null %d prec %d size %d cs %d coll %d len %d bytes ", s.type, s.is_null,
	    s.prec, s.size_field, s.codeset, s.coll, s.length);
  return std::string (head) + hex (s.bytes.substr (0, 96)) + (s.bytes.size () > 96 ? "..." : "");
}

static snap
take (const DB_VALUE * v)
{
  snap s;
  s.type = DB_VALUE_DOMAIN_TYPE (v);
  s.is_null = DB_IS_NULL (v) ? 1 : 0;
  s.prec = DB_VALUE_PRECISION (v);
  s.size_field = v->data.ch.medium.size;
  s.codeset = s.is_null ? -1 : db_get_string_codeset (v);
  s.coll = s.is_null ? -1 : db_get_string_collation (v);
  s.length = s.is_null ? -2 : db_get_string_length (v);
  const char *p = db_get_string (v);
  const int n = s.is_null ? 0 : db_get_string_size (v);
  if (p != NULL && n > 0)
    {
      s.bytes.assign (p, n);
    }
  return s;
}

/* ---------------------------------------------------------------- random text per codeset */

static std::mt19937 rng (8398);

static int
pick (int lo, int hi)
{
  return std::uniform_int_distribution < int >(lo, hi) (rng);
}

/* n characters of the codeset; ascii_only keeps every byte below 0x80 */
static std::string
text (INTL_CODESET cs, int n, bool ascii_only)
{
  std::string s;
  for (int i = 0; i < n; i++)
    {
      const int k = ascii_only ? 0 : pick (0, 3);
      switch (cs)
	{
	case INTL_CODESET_UTF8:
	  if (k == 0)
	    {
	      s += (char) pick (0x20, 0x7e);
	    }
	  else if (k == 1)
	    {
	      s += "\xc3\xa9";	/* e acute */
	    }
	  else if (k == 2)
	    {
	      s += "\xea\xb0\x80";	/* hangul ga */
	    }
	  else
	    {
	      s += "\xf0\x9f\x98\x80";	/* emoji */
	    }
	  break;
	case INTL_CODESET_KSC5601_EUC:
	  if (k <= 1)
	    {
	      s += (char) pick (0x20, 0x7e);
	    }
	  else
	    {
	      s += (char) pick (0xb0, 0xc8);
	      s += (char) pick (0xa1, 0xfe);
	    }
	  break;
	case INTL_CODESET_ISO88591:
	  s += (char) (k == 0 ? pick (0x20, 0x7e) : pick (0xa0, 0xff));
	  break;
	default:		/* binary */
	  s += (char) (k == 0 ? pick (0x20, 0x7e) : pick (1, 0xff));
	  break;
	}
    }
  return s;
}

/* a value of the collation: kind 0 VARCHAR floating, 1 VARCHAR(len + slack), 2 CHAR(len) fully padded, 3 CHAR literal
 * (floating precision). The text outlives the value (kept in a vector by the caller) */
static void
make_char_value (DB_VALUE * v, int kind, const std::string & s, int nchars, int coll)
{
  const int cs = lang_get_collation (coll)->codeset;
  switch (kind)
    {
    case 0:
      db_make_varchar (v, DB_DEFAULT_PRECISION, s.c_str (), (int) s.size (), cs, coll);
      break;
    case 1:
      db_make_varchar (v, nchars + pick (0, 5), s.c_str (), (int) s.size (), cs, coll);
      break;
    case 2:
      db_make_char (v, nchars, s.c_str (), (int) s.size (), cs, coll);
      break;
    default:
      db_make_char (v, TP_FLOATING_PRECISION_VALUE, s.c_str (), (int) s.size (), cs, coll);
      break;
    }
}

static const int COLLS[] = {
  LANG_COLL_ISO_BINARY, LANG_COLL_UTF8_BINARY, LANG_COLL_ISO_EN_CS, LANG_COLL_ISO_EN_CI, LANG_COLL_UTF8_EN_CS,
  LANG_COLL_UTF8_EN_CI, LANG_COLL_UTF8_TR_CS, LANG_COLL_UTF8_KO_CS, LANG_COLL_EUCKR_BINARY, LANG_COLL_BINARY
};

/* run one sequence of appends through both flows; return the first difference or "" */
static std::string
run_char_sequence (int acc_coll, int val_coll, int steps, bool ascii_only, bool allow_empty, int *n_appended,
		   int warmup = 0, bool ignore_prec = false)
{
  DB_VALUE dev, head;
  const int acc_cs = lang_get_collation (acc_coll)->codeset;
  start_acc (&dev, DB_TYPE_VARCHAR, acc_cs, acc_coll);
  start_acc (&head, DB_TYPE_VARCHAR, acc_cs, acc_coll);
  int capacity = 0;
  std::vector < std::string > keep;
  keep.reserve (steps);
  std::string diff;
  for (int i = 0; i < steps && diff.empty (); i++)
    {
      /* the first warmup values are of the accumulator's own collation (the buffer grows), the rest of val_coll */
      const int coll_i = i < warmup ? acc_coll : val_coll;
      const INTL_CODESET val_cs = (INTL_CODESET) lang_get_collation (coll_i)->codeset;
      const int nchars = allow_empty ? pick (0, 12) : pick (1, 12);
      keep.push_back (text (val_cs, nchars, ascii_only));
      DB_VALUE v;
      make_char_value (&v, pick (0, 3), keep.back (), nchars, coll_i);

      fake_er_id = NO_ERROR;
      const int e_dev = dev_append (&dev, &v);
      const int er_dev = fake_er_id;
      fake_er_id = NO_ERROR;
      const int e_head = head_append (&head, &v, &capacity, 1 << 30);
      const int er_head = fake_er_id;
      if ((e_dev == NO_ERROR) != (e_head == NO_ERROR) || er_dev != er_head)
	{
	  char b[160];
	  snprintf (b, sizeof (b), "step %d: error dev %d (er %d) head %d (er %d)", i, e_dev, er_dev, e_head, er_head);
	  diff = b;
	  break;
	}
      const snap a = take (&dev), b = take (&head);
      note_prec (a, b);
      if (ignore_prec && a.eq_but_prec (b) && !(a == b))
	{
	  g_prec_coerce_diffs++;
	}
      else if (!(a == b))
	{
	  diff = "step " + std::to_string (i) + " value " + show (take (&v)) + "\n  dev  " + show (a) + "\n  head " + show (b);
	}
      /* the memory contract qstr_reserve () relies on: a known capacity never exceeds the block the accumulator holds */
      if (diff.empty () && capacity > 0)
	{
	  const auto it = f_blocks.find ((void *) db_get_string (&head));
	  if (it == f_blocks.end () || (size_t) capacity > it->second || capacity < db_get_string_size (&head) + 1)
	    {
	      diff = "step " + std::to_string (i) + ": capacity " + std::to_string (capacity) + " block "
		+ (it == f_blocks.end ()? std::string ("unknown") : std::to_string (it->second)) + " size "
		+ std::to_string (db_get_string_size (&head));
	    }
	}
      if (e_head == NO_ERROR)
	{
	  (*n_appended)++;
	}
    }
  if (diff.empty () && !DB_IS_NULL (&dev) && !DB_IS_NULL (&head))
    {
      /* qdata_finalize_aggregate_list (): db_string_fix_string_size () re-makes the value; then even the precision agrees */
      db_string_fix_string_size (&dev);
      db_string_fix_string_size (&head);
      const snap a = take (&dev), b = take (&head);
      if (ignore_prec ? !a.eq_but_prec (b) : !a.raw_equal (b))
	{
	  diff = "after db_string_fix_string_size\n  dev  " + show (a) + "\n  head " + show (b);
	}
    }
  pr_clear_value (&dev);
  pr_clear_value (&head);
  return diff;
}

/* ---------------------------------------------------------------- cases */

TEST_CASE ("E1 VARCHAR accumulator, every built-in collation: db_string_append leaves what develop's concatenate and "
	   "clone left (type, precision, size, bytes, codeset, collation, NULL flag, length)", "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  int appended = 0;
  for (int coll : COLLS)
    {
      for (int rep = 0; rep < 20; rep++)
	{
	  const std::string d = run_char_sequence (coll, coll, 120, false, true, &appended);
	  INFO ("collation " << coll << " rep " << rep << "\n" << d);
	  CHECK (d.empty ());
	  if (!d.empty ())
	    {
	      break;
	    }
	}
    }
  WARN ("E1 appends compared: " << appended);
}

TEST_CASE ("E3 oracle_style_empty_string on: empty values are NULL and add nothing, the accumulator agrees", "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, true);
  int appended = 0;
  for (int coll : COLLS)
    {
      for (int rep = 0; rep < 10; rep++)
	{
	  const std::string d = run_char_sequence (coll, coll, 80, false, true, &appended);
	  INFO ("collation " << coll << " rep " << rep << "\n" << d);
	  CHECK (d.empty ());
	  if (!d.empty ())
	    {
	      break;
	    }
	}
    }
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  WARN ("E3 appends compared: " << appended);
}

TEST_CASE ("E4 two collations (ASCII text): the common collation and the codeset coercion as db_string_concatenate, or "
	   "the same error", "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  int appended = 0;
  int pairs = 0;
  for (int c1 : COLLS)
    {
      for (int c2 : COLLS)
	{
	  pairs++;
	  const std::string d = run_char_sequence (c1, c2, 8, true, false, &appended);
	  INFO ("collations " << c1 << " / " << c2 << "\n" << d);
	  CHECK (d.empty ());
	  /* the accumulator has grown a buffer (capacity > 0) before the other collation comes */
	  const std::string w = run_char_sequence (c1, c2, 16, true, false, &appended, 8, true);
	  INFO ("collations " << c1 << " / " << c2 << " after 8 of " << c1 << "\n" << w);
	  CHECK (w.empty ());
	}
    }
  WARN ("E4 pairs " << pairs << ", appends that succeeded: " << appended
	<< "; appends after a codeset coercion of the accumulator whose precision alone differs: " << g_prec_coerce_diffs);
}

TEST_CASE ("E2 VARBIT accumulator: BIT / VARBIT values of 0..17 bits append bit by bit as develop", "[contract]")
{
  REQUIRE (boot_types ());
  int compared = 0;
  for (int rep = 0; rep < 200; rep++)
    {
      DB_VALUE dev, head;
      start_acc (&dev, DB_TYPE_VARBIT, INTL_CODESET_RAW_BITS, 0);
      start_acc (&head, DB_TYPE_VARBIT, INTL_CODESET_RAW_BITS, 0);
      int capacity = 0;
      std::vector < std::string > keep;
      keep.reserve (64);
      std::string diff;
      for (int i = 0; i < 60 && diff.empty (); i++)
	{
	  const int bits = pick (0, 17);
	  std::string buf;
	  for (int k = 0; k < (bits + 7) / 8 + 2; k++)	/* +2: bit_ncat reads one byte past a value */
	    {
	      buf += (char) pick (0, 255);
	    }
	  keep.push_back (buf);
	  DB_VALUE v;
	  if (pick (0, 1) == 0)
	    {
	      db_make_varbit (&v, pick (0, 1) ? DB_DEFAULT_PRECISION : bits + pick (0, 9), keep.back ().c_str (), bits);
	    }
	  else
	    {
	      db_make_bit (&v, bits, keep.back ().c_str (), bits);
	    }
	  const int e_dev = dev_append (&dev, &v);
	  const int e_head = head_append (&head, &v, &capacity, 1 << 30);
	  if ((e_dev == NO_ERROR) != (e_head == NO_ERROR))
	    {
	      diff = "step " + std::to_string (i) + ": error dev " + std::to_string (e_dev) + " head " + std::to_string (e_head);
	      break;
	    }
	  const snap a = take (&dev), b = take (&head);
	  note_prec (a, b);
	  if (!(a == b))
	    {
	      diff = "step " + std::to_string (i) + " bits " + std::to_string (bits) + "\n  dev  " + show (a) + "\n  head "
		     + show (b);
	    }
	  compared++;
	}
      INFO ("rep " << rep << "\n" << diff);
      CHECK (diff.empty ());
      pr_clear_value (&dev);
      pr_clear_value (&head);
      if (!diff.empty ())
	{
	  break;
	}
    }
  WARN ("E2 appends compared: " << compared);
}

TEST_CASE ("E5 string_max_size_bytes: the same append fails with ER_QPROC_STRING_SIZE_TOO_BIG, the accumulator agrees",
	   "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bigint_value (PRM_ID_STRING_MAX_SIZE_BYTES, 200);
  for (int coll : { LANG_COLL_UTF8_BINARY, LANG_COLL_ISO_BINARY })
    {
      DB_VALUE dev, head;
      const int cs = lang_get_collation (coll)->codeset;
      start_acc (&dev, DB_TYPE_VARCHAR, cs, coll);
      start_acc (&head, DB_TYPE_VARCHAR, cs, coll);
      int capacity = 0;
      const std::string ten = "0123456789";
      int failed_dev = -1, failed_head = -1, er_dev = 0, er_head = 0;
      for (int i = 0; i < 40; i++)
	{
	  DB_VALUE v;
	  db_make_varchar (&v, DB_DEFAULT_PRECISION, ten.c_str (), 10, cs, coll);
	  fake_er_id = NO_ERROR;
	  if (dev_append (&dev, &v) != NO_ERROR && failed_dev < 0)
	    {
	      failed_dev = i;
	      er_dev = fake_er_id;
	    }
	  fake_er_id = NO_ERROR;
	  if (head_append (&head, &v, &capacity, 1 << 30) != NO_ERROR && failed_head < 0)
	    {
	      failed_head = i;
	      er_head = fake_er_id;
	    }
	  CHECK (take (&dev) == take (&head));
	}
      CHECK (failed_dev == 20);
      CHECK (failed_head == failed_dev);
      CHECK (er_dev == ER_QPROC_STRING_SIZE_TOO_BIG);
      CHECK (er_head == er_dev);
      pr_clear_value (&dev);
      pr_clear_value (&head);
    }
  prm_set_bigint_value (PRM_ID_STRING_MAX_SIZE_BYTES, 1024 * 1024);
}

TEST_CASE ("R1 qstr_reserve: the capacity never exceeds the accumulator's block and covers size + 1; 16,384 appends of "
	   "32 bytes take O(log N) allocations", "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bigint_value (PRM_ID_STRING_MAX_SIZE_BYTES, 32 * 1024 * 1024);
  const std::string row = text (INTL_CODESET_UTF8, 32, true);

  DB_VALUE head;
  start_acc (&head, DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  int capacity = 0;
  const long a0 = f_allocs, k0 = f_allocs_4k;
  bool cap_ok = true;
  std::string first_bad;
  for (int i = 0; i < 16384; i++)
    {
      DB_VALUE v;
      db_make_varchar (&v, DB_DEFAULT_PRECISION, row.c_str (), 32, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
      REQUIRE (head_append (&head, &v, &capacity, 32 * 1024 * 1024) == NO_ERROR);
      void *blk = (void *) db_get_string (&head);
      const auto it = f_blocks.find (blk);
      const bool ok = it != f_blocks.end () && (size_t) capacity <= it->second
	&& capacity >= db_get_string_size (&head) + 1 && (capacity & (capacity - 1)) == 0 && capacity >= 64;
      if (!ok && cap_ok)
	{
	  cap_ok = false;
	  first_bad = "append " + std::to_string (i) + " capacity " + std::to_string (capacity) + " block "
		      + (it == f_blocks.end ()? std::string ("unknown") : std::to_string (it->second)) + " size "
		      + std::to_string (db_get_string_size (&head));
	}
    }
  const long head_allocs = f_allocs - a0, head_4k = f_allocs_4k - k0;
  INFO (first_bad);
  CHECK (cap_ok);
  CHECK (db_get_string_size (&head) == 16384 * 32);
  CHECK (head_allocs <= 20);
  pr_clear_value (&head);

  DB_VALUE dev;
  start_acc (&dev, DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  const long a1 = f_allocs, k1 = f_allocs_4k;
  for (int i = 0; i < 16384; i++)
    {
      DB_VALUE v;
      db_make_varchar (&v, DB_DEFAULT_PRECISION, row.c_str (), 32, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
      REQUIRE (dev_append (&dev, &v) == NO_ERROR);
    }
  const long dev_allocs = f_allocs - a1, dev_4k = f_allocs_4k - k1;
  pr_clear_value (&dev);
  WARN ("R1 allocations for 16,384 appends of 32 bytes: develop flow " << dev_allocs << " (>= 4 KiB: " << dev_4k
	<< "), head " << head_allocs << " (>= 4 KiB: " << head_4k << ")");
  prm_set_bigint_value (PRM_ID_STRING_MAX_SIZE_BYTES, 1024 * 1024);
}

TEST_CASE ("R2 group_concat_max_len 1000: the capacity stops at 1001 unless one append needs more", "[contract]")
{
  REQUIRE (boot_types ());
  DB_VALUE head;
  start_acc (&head, DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  int capacity = 0;
  const std::string row = text (INTL_CODESET_UTF8, 30, true);
  int max_cap = 0;
  for (int i = 0; i < 33; i++)	/* 990 bytes: below the limit */
    {
      DB_VALUE v;
      db_make_varchar (&v, DB_DEFAULT_PRECISION, row.c_str (), 30, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
      REQUIRE (head_append (&head, &v, &capacity, 1000) == NO_ERROR);
      max_cap = std::max (max_cap, capacity);
    }
  CHECK (max_cap == 1001);
  /* one more append needs 1021 bytes: an exact block */
  DB_VALUE v;
  db_make_varchar (&v, DB_DEFAULT_PRECISION, row.c_str (), 30, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  REQUIRE (head_append (&head, &v, &capacity, 1000) == NO_ERROR);
  CHECK (capacity == 1021);
  const auto it = f_blocks.find ((void *) db_get_string (&head));
  REQUIRE (it != f_blocks.end ());
  CHECK ((size_t) capacity <= it->second);
  pr_clear_value (&head);
}

/* qdata_concatenate_dbval ()'s string branch (query_opfunc.c), for the truncation at max: develop's spare extension
 * of the result (it re-makes the result with size + spare_bytes) or the head's zero bytes */
static int
gc_concat (DB_VALUE * acc, DB_VALUE * v, int max, bool head, int *capacity)
{
  if (DB_IS_NULL (acc) || DB_IS_NULL (v))
    {
      return NO_ERROR;
    }
  const int res_size = db_get_string_size (acc);
  const int val_size = db_get_string_size (v);
  if (res_size >= max)
    {
      return NO_ERROR;
    }
  DB_VALUE t;
  db_make_null (&t);
  int spare = 0;
  int err = NO_ERROR;
  DB_VALUE *add = v;
  if (res_size + val_size > max)
    {
      err = db_string_limit_size_string (v, &t, max - res_size, &spare);
      if (err != NO_ERROR)
	{
	  return err;
	}
      add = &t;
    }
  if (head)
    {
      const int cs = db_get_string_codeset (acc), coll = db_get_string_collation (acc);
      err = db_string_append (acc, add, capacity, max);
      if (err == NO_ERROR && spare > 0)
	{
	  err = db_string_append_zero_bytes (acc, spare, capacity, max);
	  db_string_put_cs_and_collation (acc, cs, coll);
	}
    }
  else
    {
      DB_VALUE r;
      DB_DATA_STATUS st;
      db_make_null (&r);
      err = db_string_concatenate (acc, add, &r, &st);
      if (err == NO_ERROR && st != DATA_STATUS_OK)
	{
	  err = ER_FAILED;
	}
      if (err == NO_ERROR && spare > 0)
	{
	  /* develop re-made the result with size + spare_bytes over its exact block; copy it into a block that holds the
	   * spare bytes as zeros first, so that the clone below reads no byte past a block */
	  const int sz = db_get_string_size (&r);
	  char *wide = (char *) db_private_alloc (NULL, sz + spare + 1);
	  memcpy (wide, db_get_string (&r), sz);
	  memset (wide + sz, 0, spare + 1);
	  const int prec = DB_VALUE_PRECISION (&r);
	  const DB_TYPE ty = DB_VALUE_DOMAIN_TYPE (&r);
	  pr_clear_value (&r);
	  qstr_make_typed_string (ty, &r, prec, wide, sz + spare, db_get_string_codeset (acc), db_get_string_collation (acc));
	  r.need_clear = true;
	}
      if (err == NO_ERROR && !DB_IS_NULL (&r))
	{
	  pr_clear_value (acc);
	  pr_clone_value (&r, acc);
	}
      pr_clear_value (&r);
    }
  pr_clear_value (&t);
  return err;
}

TEST_CASE ("Z1 truncation inside a multibyte character at group_concat_max_len: after db_string_fix_string_size the "
	   "value is develop's", "[contract]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  int truncations = 0;
  for (int coll : { LANG_COLL_UTF8_BINARY, LANG_COLL_UTF8_KO_CS, LANG_COLL_EUCKR_BINARY, LANG_COLL_ISO_BINARY })
    {
      const INTL_CODESET cs = (INTL_CODESET) lang_get_collation (coll)->codeset;
      for (int max = 4; max <= 64; max++)
	{
	  DB_VALUE dev, head, sep;
	  start_acc (&dev, DB_TYPE_VARCHAR, cs, coll);
	  start_acc (&head, DB_TYPE_VARCHAR, cs, coll);
	  db_make_char (&sep, TP_FLOATING_PRECISION_VALUE, ",", 1, cs, coll);
	  int capacity = 0;
	  std::vector < std::string > keep;
	  keep.reserve (40);
	  for (int i = 0; i < 30; i++)
	    {
	      const int n = pick (1, 6);
	      keep.push_back (text (cs, n, false));
	      DB_VALUE v;
	      make_char_value (&v, pick (0, 3), keep.back (), n, coll);
	      if (i > 0)
		{
		  REQUIRE (gc_concat (&dev, &sep, max, false, NULL) == NO_ERROR);
		  REQUIRE (gc_concat (&head, &sep, max, true, &capacity) == NO_ERROR);
		}
	      const int before = db_get_string_size (&head);
	      REQUIRE (gc_concat (&dev, &v, max, false, NULL) == NO_ERROR);
	      REQUIRE (gc_concat (&head, &v, max, true, &capacity) == NO_ERROR);
	      if (before < max && db_get_string_size (&head) == max && before + db_get_string_size (&v) > max)
		{
		  truncations++;
		}
	    }
	  CHECK (db_get_string_size (&dev) == db_get_string_size (&head));
	  db_string_fix_string_size (&dev);
	  db_string_fix_string_size (&head);
	  const snap a = take (&dev), b = take (&head);
	  INFO ("collation " << coll << " max " << max << "\n  dev  " << show (a) << "\n  head " << show (b));
	  CHECK (a == b);
	  pr_clear_value (&dev);
	  pr_clear_value (&head);
	}
    }
  WARN ("Z1 truncations at max: " << truncations);
}

TEST_CASE ("P1 the accumulator's precision: db_string_append leaves TP_FLOATING_PRECISION_VALUE where develop's "
	   "concatenate and clone left DB_MAX_VARCHAR_PRECISION / DB_MAX_VARBIT_PRECISION (PASS = the difference is there; "
	   "a VARCHAR's goes at db_string_fix_string_size, a VARBIT's stays)", "[defect]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  WARN ("raw precision differences seen in E1-E4: " << g_prec_raw_diffs << " (example: develop " << g_prec_raw_example_dev
	<< ", head " << g_prec_raw_example_head << ")");

  /* VARCHAR: two appends of the literal 'ab' -- a CHAR of floating precision, as a SEPARATOR literal is */
  DB_VALUE dev, head, v;
  start_acc (&dev, DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  start_acc (&head, DB_TYPE_VARCHAR, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  int capacity = 0;
  db_make_char (&v, TP_FLOATING_PRECISION_VALUE, "ab", 2, INTL_CODESET_UTF8, LANG_COLL_UTF8_BINARY);
  for (int i = 0; i < 2; i++)
    {
      REQUIRE (dev_append (&dev, &v) == NO_ERROR);
      REQUIRE (head_append (&head, &v, &capacity, 1024) == NO_ERROR);
    }
  INFO ("VARCHAR precision: develop " << DB_VALUE_PRECISION (&dev) << ", head " << DB_VALUE_PRECISION (&head));
  const bool varchar_differs = DB_VALUE_PRECISION (&dev) == DB_MAX_VARCHAR_PRECISION
    && DB_VALUE_PRECISION (&head) == TP_FLOATING_PRECISION_VALUE;
  db_string_fix_string_size (&dev);
  db_string_fix_string_size (&head);
  const bool varchar_fixed = DB_VALUE_PRECISION (&dev) == DB_VALUE_PRECISION (&head);
  pr_clear_value (&dev);
  pr_clear_value (&head);

  /* VARBIT: two appends of the literal B'101' -- a BIT of floating precision */
  static const char bits[2] = { (char) 0xa0, 0 };
  start_acc (&dev, DB_TYPE_VARBIT, INTL_CODESET_RAW_BITS, 0);
  start_acc (&head, DB_TYPE_VARBIT, INTL_CODESET_RAW_BITS, 0);
  capacity = 0;
  db_make_bit (&v, TP_FLOATING_PRECISION_VALUE, bits, 3);
  for (int i = 0; i < 2; i++)
    {
      REQUIRE (dev_append (&dev, &v) == NO_ERROR);
      REQUIRE (head_append (&head, &v, &capacity, 1024) == NO_ERROR);
    }
  INFO ("VARBIT precision: develop " << DB_VALUE_PRECISION (&dev) << ", head " << DB_VALUE_PRECISION (&head));
  const bool varbit_differs = DB_VALUE_PRECISION (&dev) == DB_MAX_VARBIT_PRECISION
    && DB_VALUE_PRECISION (&head) == TP_FLOATING_PRECISION_VALUE;
  /* db_string_fix_string_size () takes no bit string (qdata_finalize_aggregate_list calls it on a VARBIT too) */
  CHECK (db_string_fix_string_size (&head) == ER_QSTR_INVALID_DATA_TYPE);
  const bool varbit_stays = DB_VALUE_PRECISION (&head) == TP_FLOATING_PRECISION_VALUE;
  pr_clear_value (&dev);
  pr_clear_value (&head);

  CHECK (g_prec_raw_diffs > 0);
  CHECK (varchar_differs);
  CHECK (varchar_fixed);
  CHECK (varbit_differs);
  CHECK (varbit_stays);
}

TEST_CASE ("P2 a codeset coercion of the accumulator after a floating-precision value: head's precision becomes the "
	   "accumulator's length plus the value's, develop's stays DB_MAX_VARCHAR_PRECISION -- and db_string_fix_string_size "
	   "keeps it (PASS = the difference is there; GROUP_CONCAT does not coerce, CBRD-27583)", "[defect]")
{
  REQUIRE (boot_types ());
  prm_set_bool_value (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  WARN ("E4 appends whose precision alone differs after a coercion: " << g_prec_coerce_diffs);
  DB_VALUE dev, head, lit, other;
  start_acc (&dev, DB_TYPE_VARCHAR, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  start_acc (&head, DB_TYPE_VARCHAR, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  int capacity = 0;
  db_make_char (&lit, TP_FLOATING_PRECISION_VALUE, "ab", 2, INTL_CODESET_ISO88591, LANG_COLL_ISO_BINARY);
  REQUIRE (dev_append (&dev, &lit) == NO_ERROR);
  REQUIRE (head_append (&head, &lit, &capacity, 1024) == NO_ERROR);
  /* binary collation: LANG_RT_COMMON_COLL (iso88591_bin, binary) = binary, whose codeset is not the accumulator's */
  const int cs9 = lang_get_collation (LANG_COLL_BINARY)->codeset;
  db_make_varchar (&other, 2, "cd", 2, cs9, LANG_COLL_BINARY);
  REQUIRE (dev_append (&dev, &other) == NO_ERROR);
  REQUIRE (head_append (&head, &other, &capacity, 1024) == NO_ERROR);
  INFO ("develop " << show (take (&dev)) << "\nhead    " << show (take (&head)));
  CHECK (take (&dev).eq_but_prec (take (&head)));
  CHECK (DB_VALUE_PRECISION (&dev) == DB_MAX_VARCHAR_PRECISION);
  CHECK (DB_VALUE_PRECISION (&head) == 4);
  db_string_fix_string_size (&dev);
  db_string_fix_string_size (&head);
  CHECK (DB_VALUE_PRECISION (&head) == 4);
  CHECK (DB_VALUE_PRECISION (&dev) == DB_MAX_VARCHAR_PRECISION);
  pr_clear_value (&dev);
  pr_clear_value (&head);
}

TEST_CASE ("C1 a BIT start value: db_string_concatenate makes a BIT of two BITs, so the accumulator stays BIT and every "
	   "append takes the copying branch with capacity 0 (PASS = the comment's 'becomes the VARCHAR or VARBIT ... once' "
	   "does not hold; GROUP_CONCAT's accumulator is a VARCHAR or VARBIT, so no query reaches it)", "[defect]")
{
  REQUIRE (boot_types ());
  static const char bits[2] = { (char) 0xa0, 0 };
  DB_VALUE acc, v;
  db_make_null (&acc);
  db_value_domain_init (&acc, DB_TYPE_BIT, 3, 0);
  REQUIRE (db_string_make_empty_typed_string (&acc, DB_TYPE_BIT, 3, INTL_CODESET_RAW_BITS, 0) == NO_ERROR);
  db_make_bit (&v, 3, bits, 3);
  int capacity = 0;
  bool stays_bit = true;
  for (int i = 0; i < 4; i++)
    {
      REQUIRE (db_string_append (&acc, &v, &capacity, 1024) == NO_ERROR);
      stays_bit = stays_bit && DB_VALUE_DOMAIN_TYPE (&acc) == DB_TYPE_BIT && capacity == 0;
    }
  INFO ("type " << DB_VALUE_DOMAIN_TYPE (&acc) << " bits " << acc.data.ch.medium.size << " capacity " << capacity);
  CHECK (stays_bit);
  pr_clear_value (&acc);
}

TEST_CASE ("D1 (develop as well) GROUP_CONCAT's truncation of a bit string: db_string_limit_size_string () takes new_size "
	   "in bytes but makes a bit string of new_size bits (PASS = the defect is there: 24 bits, not 24 bytes)", "[defect]")
{
  REQUIRE (boot_types ());
  std::string buf (100, (char) 0xff);
  DB_VALUE v, t;
  db_make_varbit (&v, DB_DEFAULT_PRECISION, buf.c_str (), 800);	/* 100 bytes */
  db_make_null (&t);
  int spare = -1;
  REQUIRE (db_string_limit_size_string (&v, &t, 24, &spare) == NO_ERROR);
  INFO ("bits " << t.data.ch.medium.size << " bytes " << db_get_string_size (&t) << " spare " << spare);
  CHECK (t.data.ch.medium.size == 24);	/* 24 bits */
  CHECK (db_get_string_size (&t) == 3);
  pr_clear_value (&t);

  /* through the head's GROUP_CONCAT append: 10 values of 100 bytes then one more at group_concat_max_len 1024 */
  DB_VALUE acc;
  start_acc (&acc, DB_TYPE_VARBIT, INTL_CODESET_RAW_BITS, 0);
  int capacity = 0;
  for (int i = 0; i < 11; i++)
    {
      REQUIRE (gc_concat (&acc, &v, 1024, true, &capacity) == NO_ERROR);
    }
  INFO ("accumulator bytes " << db_get_string_size (&acc) << " (group_concat_max_len 1024)");
  CHECK (db_get_string_size (&acc) == 1003);
  pr_clear_value (&acc);
}
