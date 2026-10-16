/*
 * PR#7977 (CBRD-27251) -- CHAR histogram keys (src/optimizer/histogram/histogram_cl.cpp, cs mode).
 *
 * The PR keys string histograms by the COLUMN type (one rule, hist::string_key_size_for_column, shared
 * by the server sampler and the planner probe), re-pads stored CHAR values to the declared width
 * before LIKE/REGEXP matching (histogram_repad_char_value, TU static), and carries that width in the
 * former HV2_RESERVED header slot (offset 28) that HistogramReader::reset folds to [0, 2048].
 *
 *   key        string_key_size_for_column: CHAR drops trailing ' ' only; VARCHAR/BIT keep bytes
 *   repad      histogram_repad_char_value: guards (type, width range, codeset), width in characters
 *              under the codeset, no truncation, buffer reuse
 *   roundtrip  strip (key rule) then re-pad rebuilds the heap image pr_pad_char_to_precision makes
 *   reader     HST2 header slot 28 -> value_precision (), out of range -> 0, rest of header intact
 *   ctx        histogram_repad_ctx: type/width from the blob, charset from the collation
 *   probe      histogram_extract_key: the constant's key follows the column type (the PR's core fix)
 *
 * Every case is [contract]. Cases that need a function the PR introduced FAIL with "absent at this
 * revision" before CBRD-27251 (hooks.cpp and the probes below keep the suite compiling there); the
 * probe-key cases run on both sides and FAIL at the pre-PR base by behaviour, not by absence.
 *
 * Real code at THIS revision is compiled into the test binary (the test's -I resolves to the worktree):
 *   byte_order.c          ntohd/htond behind OR_GET_DOUBLE/OR_PUT_DOUBLE (blob header, MCV freq)
 *   histogram_reader.cpp  HistogramReader::reset, where the width slot is read
 *   intl_support.c        intl_char_count, which the re-pad counts characters with
 * Faked: lang_get_collation / lang_Collations / lang_Loc_charset / lang_charset (see below).
 */
#include "catch.hpp"

#include "byte_order.c"
#include "histogram_reader.cpp"
#include "intl_support.c"

#include "dbtype_def.h"
#include "language_support.h"
#include "histogram_cl.hpp"
#include <climits>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/* ---------------------------------------------------------------- hooks (hooks.cpp, in the TU) */

std::string_view unitseam_repad (std::string_view value, int type, int precision, int codeset, std::string &buf,
				 bool *present);
bool unitseam_repad_ctx (const hist::HistogramReader &reader, int collation_id, int *type, int *precision,
			 int *codeset);
std::string_view unitseam_repad_via_reader (const hist::HistogramReader &reader, int collation_id,
    std::string_view value, std::string &buf, bool *present);
bool unitseam_extract_str_key (const DB_VALUE *v, int column_type, std::string *out, bool *by_column);

/* ---------------------------------------------------------------- fakes */

/* language_support.c: lang_init_builtin () fills EVERY slot with coll_Iso_binary (language_support.c:
 * 873-877), then register_collation () puts each built-in at its id. Only codeset is read on the paths
 * under test, so the fakes carry the built-ins' real codesets (language_support.c coll_* initializers):
 * 0 iso88591_bin, 1 utf8_bin, 2 iso88591_en_cs, 3 iso88591_en_ci, 4 utf8_en_cs, 5 utf8_en_ci,
 * 6 utf8_tr_cs, 7 utf8_ko_cs, 8 euckr_bin, 9 binary (INTL_CODESET_BINARY == RAW_BYTES). */
LANG_COLLATION *lang_Collations[LANG_MAX_COLLATIONS];
/* language_support.c:68 initial value; boot sets it from the locale. LANG_SYS_CODESET reads this
 * variable in a release (NDEBUG) build and lang_charset () in a debug build -- both are provided. */
INTL_CODESET lang_Loc_charset = INTL_CODESET_ISO88591;

static LANG_COLLATION fake_coll[LANG_COLL_BUILTIN_MAX + 1];

static bool
boot_collations ()
{
  static const INTL_CODESET cs[LANG_COLL_BUILTIN_MAX + 1] =
  {
    INTL_CODESET_ISO88591, INTL_CODESET_UTF8, INTL_CODESET_ISO88591, INTL_CODESET_ISO88591, INTL_CODESET_UTF8,
    INTL_CODESET_UTF8, INTL_CODESET_UTF8, INTL_CODESET_UTF8, INTL_CODESET_KSC5601_EUC, INTL_CODESET_BINARY
  };
  for (int i = 0; i <= LANG_COLL_BUILTIN_MAX; i++)
    {
      fake_coll[i].codeset = cs[i];
      fake_coll[i].built_in = 1;
      fake_coll[i].coll.coll_id = i;
    }
  for (int i = 0; i < LANG_MAX_COLLATIONS; i++)
    {
      lang_Collations[i] = &fake_coll[LANG_COLL_ISO_BINARY];
    }
  for (int i = 0; i <= LANG_COLL_BUILTIN_MAX; i++)
    {
      lang_Collations[i] = &fake_coll[i];
    }
  return true;
}
static const bool collations_booted = boot_collations ();

/* language_support.c:1731 body */
LANG_COLLATION *
lang_get_collation (const int coll_id)
{
  assert (coll_id >= 0 && coll_id < LANG_MAX_COLLATIONS);
  return lang_Collations[coll_id];
}

/* language_support.c:2054, initialized-charset branch */
INTL_CODESET
lang_charset (void)
{
  return lang_Loc_charset;
}

/* ---------------------------------------------------------------- probes for code the PR added */

namespace unitseam_probe
{
  struct absent_t
  {
  };
  template <typename... A> absent_t string_key_size_for_column (A &&...);
}
namespace hist
{
  /* consulted by hist::string_key_size_for_column only when hist itself declares none */
  using namespace unitseam_probe;
}

#define ABSENT_BEFORE_27251(what) FAIL (what " absent at this revision (before CBRD-27251)")

template <typename T>
static int
key_size (DB_TYPE column_type, const char *s, T size)
{
  using R = decltype (hist::string_key_size_for_column (column_type, s, size));
  if constexpr (std::is_same<R, unitseam_probe::absent_t>::value)
    {
      ABSENT_BEFORE_27251 ("hist::string_key_size_for_column");
      return -1;
    }
  else
    {
      return hist::string_key_size_for_column (column_type, s, size);
    }
}

template <typename R>
static auto
precision_of (const R &r, int) -> decltype (r.value_precision ())
{
  return r.value_precision ();
}

template <typename R>
static int
precision_of (const R &, long)
{
  ABSENT_BEFORE_27251 ("HistogramReader::value_precision");
  return 0;
}

static std::string
repad (const std::string &value, int type, int precision, int codeset, std::string &buf)
{
  bool present = false;
  std::string_view r = unitseam_repad (value, type, precision, codeset, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_char_value");
    }
  return std::string (r);
}

/* ---------------------------------------------------------------- test data */

/* UTF-8: U+AC00 / U+B098 (3 bytes each), U+1F600 (4 bytes); EUC-KR: the same two syllables (2 bytes) */
static const std::string U_GA = "\xEA\xB0\x80";
static const std::string U_NA = "\xEB\x82\x98";
static const std::string U_EMOJI = "\xF0\x9F\x98\x80";
static const std::string E_GA = "\xB0\xA1";
static const std::string E_NA = "\xB3\xAA";

/* HST2 blob, written with the OR_* macros the builder uses (histogram_builder.cpp:59-73, 269-280).
 * Offsets are literal: they are the on-disk format (histogram_reader.hpp HV2_*), and slot 28 was
 * HV2_RESERVED (always written 0) before CBRD-27251 -- the base revision has no enum name for it. */
namespace blob
{
  const int MAGIC = 0, VERSION = 4, NMCV = 8, NBUCKETS = 12, STR_SIZE = 16, TYPE = 20, TOTAL_SIZE = 24,
	    PRECISION = 28, TOTAL_ROWS = 32, NULL_FREQ = 40, HEADER = 48;
  const int MCV_REC = 16, BUCKET_REC = 24;

  struct spec
  {
    DB_TYPE type = DB_TYPE_CHAR;
    int precision = 0;
    std::vector<std::pair<std::string, double>> mcvs;
    std::vector<std::string> buckets;
    std::int64_t total_rows = 1000;
    double null_freq = 0.0;
  };

  std::string
  make (const spec &s)
  {
    const std::size_t fixed = HEADER + MCV_REC * s.mcvs.size () + BUCKET_REC * s.buckets.size ();
    std::string b (fixed, '\0');
    std::string strs;
    auto slot = [&b, &strs] (std::size_t at, const std::string &v)
    {
      OR_PUT_INT (&b[at], (int) v.size ());
      if (v.size () <= 4)
	{
	  memcpy (&b[at + 4], v.data (), v.size ());
	}
      else
	{
	  OR_PUT_INT (&b[at + 4], (int) strs.size ());
	  strs += v;
	}
    };
    for (std::size_t i = 0; i < s.mcvs.size (); i++)
      {
	const std::size_t at = HEADER + MCV_REC * i;
	slot (at, s.mcvs[i].first);
	OR_PUT_DOUBLE (&b[at + 8], s.mcvs[i].second);
      }
    for (std::size_t j = 0; j < s.buckets.size (); j++)
      {
	const std::size_t at = HEADER + MCV_REC * s.mcvs.size () + BUCKET_REC * j;
	std::int64_t cum = 10 * (std::int64_t) (j + 1), ndv = 1;
	slot (at, s.buckets[j]);
	OR_PUT_INT64 (&b[at + 8], &cum);
	OR_PUT_INT64 (&b[at + 16], &ndv);
      }
    b += strs;
    memcpy (&b[MAGIC], "HST2", 4);
    OR_PUT_INT (&b[VERSION], 2);
    OR_PUT_INT (&b[NMCV], (int) s.mcvs.size ());
    OR_PUT_INT (&b[NBUCKETS], (int) s.buckets.size ());
    OR_PUT_INT (&b[STR_SIZE], (int) strs.size ());
    OR_PUT_INT (&b[TYPE], (int) s.type);
    OR_PUT_INT (&b[TOTAL_SIZE], (int) b.size ());
    OR_PUT_INT (&b[PRECISION], s.precision);
    std::int64_t rows = s.total_rows;
    OR_PUT_INT64 (&b[TOTAL_ROWS], &rows);
    OR_PUT_DOUBLE (&b[NULL_FREQ], s.null_freq);
    return b;
  }
}

/* a CHAR(n) blob with one inline MCV ('ab'), one blob-area MCV ('abcdef') and one bucket */
static std::string
char_blob (int precision, DB_TYPE type = DB_TYPE_CHAR)
{
  blob::spec s;
  s.type = type;
  s.precision = precision;
  s.mcvs = { { "ab", 0.25 }, { "abcdef", 0.5 } };
  s.buckets = { "zzzzz" };
  s.total_rows = 1600;
  s.null_freq = 0.125;
  return blob::make (s);
}

/* A heap CHAR(n) image: content padded with ' ' to n characters -- what pr_pad_char_to_precision
 * (object_primitive.c:12629-12660) stores: memset ' ' for (precision - char_count) characters,
 * char_count from the value's codeset. */
static std::string
heap_char (const std::string &content, int content_chars, int precision)
{
  return content + std::string ((std::size_t) std::max (0, precision - content_chars), ' ');
}

/* a string constant as the estimator receives it: db_value_domain_init (type, precision, 0) followed by
 * db_make_db_char (dbtype_function.i:1095) -- fields set directly, s must outlive v */
static void
make_str (DB_VALUE *v, DB_TYPE type, int precision, const std::string &s, int codeset = INTL_CODESET_ISO88591,
	  int collation = LANG_COLL_ISO_BINARY)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = type;
  v->domain.char_info.length = precision;
  v->domain.char_info.collation_id = collation;
  v->domain.general_info.is_null = 0;
  v->data.ch.info.style = MEDIUM_STRING;
  v->data.ch.medium.codeset = codeset;
  v->data.ch.medium.size = (int) s.size ();
  v->data.ch.medium.buf = s.data ();
  v->data.ch.medium.compressed_buf = NULL;
  v->data.ch.medium.compressed_size = DB_NOT_YET_COMPRESSED;
  v->data.ch.medium.length = -1;
}

/* ================================================================ 1. key rule */

TEST_CASE ("PR7977 key: CHAR column drops trailing spaces and nothing else", "[contract][key]")
{
  CHECK (key_size (DB_TYPE_CHAR, "abcd    ", 8) == 4);
  CHECK (key_size (DB_TYPE_CHAR, "abcd", 4) == 4);
  CHECK (key_size (DB_TYPE_CHAR, " a b  ", 6) == 4);		/* leading and embedded spaces stay */
  CHECK (key_size (DB_TYPE_CHAR, "ab\t", 3) == 3);		/* only ' ' (0x20) is padding */
  CHECK (key_size (DB_TYPE_CHAR, "a\0  ", 4) == 2);		/* an embedded NUL is data */
  CHECK (key_size (DB_TYPE_CHAR, (U_GA + "  ").c_str (), 5) == 3);	/* UTF-8 bytes are >= 0x80, never 0x20 */
}

TEST_CASE ("PR7977 key: VARCHAR, BIT and VARBIT columns keep every byte", "[contract][key]")
{
  CHECK (key_size (DB_TYPE_STRING, "abcd ", 5) == 5);
  CHECK (key_size (DB_TYPE_STRING, "abcd    ", 8) == 8);
  CHECK (key_size (DB_TYPE_STRING, "        ", 8) == 8);
  CHECK (key_size (DB_TYPE_BIT, "ab  ", 4) == 4);
  CHECK (key_size (DB_TYPE_VARBIT, "ab  ", 4) == 4);
  CHECK (key_size (DB_TYPE_INTEGER, "ab  ", 4) == 4);
}

TEST_CASE ("PR7977 key: NULL bytes and non-positive sizes are returned as is", "[contract][key]")
{
  CHECK (key_size (DB_TYPE_CHAR, (const char *) NULL, 0) == 0);
  CHECK (key_size (DB_TYPE_CHAR, (const char *) NULL, 5) == 5);	/* s NULL: never read */
  CHECK (key_size (DB_TYPE_STRING, (const char *) NULL, 3) == 3);
  CHECK (key_size (DB_TYPE_CHAR, "a", -1) == -1);			/* callers reject len < 0 themselves */
}

TEST_CASE ("PR7977 key: all-space and empty CHAR values key on zero bytes", "[contract][key]")
{
  CHECK (key_size (DB_TYPE_CHAR, "        ", 8) == 0);
  CHECK (key_size (DB_TYPE_CHAR, " ", 1) == 0);
  CHECK (key_size (DB_TYPE_CHAR, "", 0) == 0);
  CHECK (key_size (DB_TYPE_STRING, "", 0) == 0);
}

/* ================================================================ 2. re-pad */

TEST_CASE ("PR7977 repad: CHAR(8) value is padded to 8 characters in the caller's buffer", "[contract][repad]")
{
  std::string buf;
  bool present = false;
  const std::string v = "abcd";
  std::string_view r = unitseam_repad (v, DB_TYPE_CHAR, 8, INTL_CODESET_ISO88591, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_char_value");
    }
  CHECK (std::string (r) == "abcd    ");
  CHECK (r.data () == buf.data ());		/* a view into buf, not into the blob */
}

TEST_CASE ("PR7977 repad: width <= 0 or > DB_MAX_CHAR_PRECISION returns the stored value itself",
	   "[contract][repad]")
{
  const std::string v = "abcd";
  for (int p : { 0, -1, INT_MIN, DB_MAX_CHAR_PRECISION + 1, INT_MAX })
    {
      std::string buf = "untouched";
      bool present = false;
      std::string_view r = unitseam_repad (v, DB_TYPE_CHAR, p, INTL_CODESET_ISO88591, buf, &present);
      if (!present)
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_char_value");
	}
      INFO ("precision " << p);
      CHECK (r.data () == v.data ());
      CHECK (r.size () == v.size ());
      CHECK (buf == "untouched");
    }
}

TEST_CASE ("PR7977 repad: width == DB_MAX_CHAR_PRECISION pads up to 2048 characters", "[contract][repad]")
{
  std::string buf;
  const std::string r = repad ("abcd", DB_TYPE_CHAR, DB_MAX_CHAR_PRECISION, INTL_CODESET_ISO88591, buf);
  CHECK (r.size () == (std::size_t) DB_MAX_CHAR_PRECISION);
  CHECK (r.compare (0, 4, "abcd") == 0);
  CHECK (r.find_first_not_of (' ', 4) == std::string::npos);
}

TEST_CASE ("PR7977 repad: a value already at or over the width is returned unchanged (no truncation)",
	   "[contract][repad]")
{
  for (const std::string v : { std::string ("abcdefgh"), std::string ("abcdefghij") })
    {
      std::string buf = "untouched";
      bool present = false;
      std::string_view r = unitseam_repad (v, DB_TYPE_CHAR, 8, INTL_CODESET_ISO88591, buf, &present);
      if (!present)
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_char_value");
	}
      CHECK (r.data () == v.data ());
      CHECK (r.size () == v.size ());
      CHECK (buf == "untouched");
    }
}

TEST_CASE ("PR7977 repad: UTF-8 width counts characters, not bytes", "[contract][repad]")
{
  std::string buf;

  /* 2 chars / 6 bytes in CHAR(4): 2 pad chars */
  CHECK (repad (U_GA + U_NA, DB_TYPE_CHAR, 4, INTL_CODESET_UTF8, buf) == U_GA + U_NA + "  ");
  /* 4-byte character */
  CHECK (repad (U_EMOJI, DB_TYPE_CHAR, 3, INTL_CODESET_UTF8, buf) == U_EMOJI + "  ");
  /* 10 ASCII + 1 three-byte char = 13 bytes / 11 chars: crosses the 8-byte ASCII fast path into the
   * slow path of intl_count_utf8_chars */
  CHECK (repad ("abcdefghij" + U_GA, DB_TYPE_CHAR, 16, INTL_CODESET_UTF8, buf) == "abcdefghij" + U_GA + "     ");
  /* exactly at width in characters (6 bytes > 2): nothing to add */
  CHECK (repad (U_GA + U_NA, DB_TYPE_CHAR, 2, INTL_CODESET_UTF8, buf) == U_GA + U_NA);
  /* the same bytes counted under ISO-8859-1 are 6 characters: already over CHAR(4) */
  CHECK (repad (U_GA + U_NA, DB_TYPE_CHAR, 4, INTL_CODESET_ISO88591, buf) == U_GA + U_NA);
}

TEST_CASE ("PR7977 repad: EUC-KR width counts 2-byte characters", "[contract][repad]")
{
  std::string buf;
  CHECK (repad (E_GA + E_NA, DB_TYPE_CHAR, 4, INTL_CODESET_KSC5601_EUC, buf) == E_GA + E_NA + "  ");
  CHECK (repad ("a" + E_GA, DB_TYPE_CHAR, 3, INTL_CODESET_KSC5601_EUC, buf) == "a" + E_GA + " ");
}

TEST_CASE ("PR7977 repad: binary collation (RAW_BYTES) counts bytes", "[contract][repad]")
{
  std::string buf;
  CHECK (repad (U_GA, DB_TYPE_CHAR, 4, INTL_CODESET_RAW_BYTES, buf) == U_GA + " ");
  CHECK (repad (std::string ("a\0b", 3), DB_TYPE_CHAR, 5, INTL_CODESET_RAW_BYTES, buf)
	 == std::string ("a\0b  ", 5));
}

TEST_CASE ("PR7977 repad: codesets intl_char_count does not handle are returned as is", "[contract][repad]")
{
  const std::string v = "ab";
  for (int cs : { (int) INTL_CODESET_ASCII, (int) INTL_CODESET_RAW_BITS, (int) INTL_CODESET_NONE,
		  (int) INTL_CODESET_ERROR, 99 })
    {
      std::string buf = "untouched";
      bool present = false;
      std::string_view r = unitseam_repad (v, DB_TYPE_CHAR, 8, cs, buf, &present);
      if (!present)
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_char_value");
	}
      INFO ("codeset " << cs);
      CHECK (r.data () == v.data ());
      CHECK (buf == "untouched");
    }
}

TEST_CASE ("PR7977 repad: non-CHAR column types are never padded", "[contract][repad]")
{
  const std::string v = "ab";
  for (int t : { (int) DB_TYPE_STRING, (int) DB_TYPE_BIT, (int) DB_TYPE_VARBIT, (int) DB_TYPE_UNKNOWN })
    {
      std::string buf = "untouched";
      bool present = false;
      std::string_view r = unitseam_repad (v, t, 8, INTL_CODESET_UTF8, buf, &present);
      if (!present)
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_char_value");
	}
      INFO ("type " << t);
      CHECK (r.data () == v.data ());
      CHECK (buf == "untouched");
    }
}

TEST_CASE ("PR7977 repad: an empty stored value (all-space CHAR row) becomes width spaces", "[contract][repad]")
{
  std::string buf;
  CHECK (repad ("", DB_TYPE_CHAR, 5, INTL_CODESET_UTF8, buf) == "     ");
  /* a zero-length view with no storage behind it (what a default string_view carries) */
  bool present = false;
  std::string_view r = unitseam_repad (std::string_view (), DB_TYPE_CHAR, 3, INTL_CODESET_ISO88591, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_char_value");
    }
  CHECK (std::string (r) == "   ");
}

TEST_CASE ("PR7977 repad: the buffer is reused -- each call overwrites it, a pass-through leaves it alone",
	   "[contract][repad]")
{
  std::string buf;
  bool present = false;

  std::string_view r1 = unitseam_repad ("ab", DB_TYPE_CHAR, 64, INTL_CODESET_ISO88591, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_char_value");
    }
  CHECK (r1.size () == 64);
  const char *storage = buf.data ();

  /* second MCV of the same predicate: overwritten, not appended; no new allocation for a smaller pad */
  std::string_view r2 = unitseam_repad ("xyz", DB_TYPE_CHAR, 8, INTL_CODESET_ISO88591, buf, &present);
  CHECK (std::string (r2) == "xyz     ");
  CHECK (buf == "xyz     ");
  CHECK (r2.data () == storage);

  /* a value already at width: the result is the value, the buffer keeps the previous pad */
  const std::string full = "12345678";
  std::string_view r3 = unitseam_repad (full, DB_TYPE_CHAR, 8, INTL_CODESET_ISO88591, buf, &present);
  CHECK (r3.data () == full.data ());
  CHECK (buf == "xyz     ");
}

/* ================================================================ 1+2. round trip */

TEST_CASE ("PR7977 roundtrip: strip by the key rule then re-pad rebuilds the padded heap CHAR image",
	   "[contract][roundtrip]")
{
  struct c
  {
    std::string content;
    int chars;
    int precision;
    int codeset;
  } cases[] =
  {
    { "ab", 2, 6, INTL_CODESET_ISO88591 },
    { "ab  ", 4, 6, INTL_CODESET_ISO88591 },		/* the value's own trailing spaces */
    { "abcd", 4, 4, INTL_CODESET_ISO88591 },		/* full width: nothing stripped or added */
    { "", 0, 4, INTL_CODESET_ISO88591 },		/* all-space row */
    { U_GA + U_NA, 2, 5, INTL_CODESET_UTF8 },
    { U_GA + " " + U_EMOJI, 3, 3, INTL_CODESET_UTF8 },
    { E_GA + E_NA, 2, 3, INTL_CODESET_KSC5601_EUC },
    { std::string ("a\0", 2), 2, 4, INTL_CODESET_RAW_BYTES },
  };
  for (const c &x : cases)
    {
      const std::string heap = heap_char (x.content, x.chars, x.precision);
      const int n = key_size (DB_TYPE_CHAR, heap.data (), (int) heap.size ());
      const std::string stored = heap.substr (0, (std::size_t) n);
      std::string buf;
      INFO ("content bytes " << x.content.size () << " precision " << x.precision << " codeset " << x.codeset);
      CHECK (repad (stored, DB_TYPE_CHAR, x.precision, x.codeset, buf) == heap);
    }
}

/* ================================================================ 3. reader */

TEST_CASE ("PR7977 reader: the width in header slot 28 reads back as value_precision", "[contract][reader]")
{
  for (int p : { 1, 8, 255 })
    {
      const std::string b = char_blob (p);
      hist::HistogramReader r;
      REQUIRE (r.reset (b) == NO_ERROR);
      INFO ("precision " << p);
      CHECK (precision_of (r, 0) == p);
    }
}

TEST_CASE ("PR7977 reader: a blob from before CBRD-27251 (slot written 0) reads width 0", "[contract][reader]")
{
  const std::string b = char_blob (0);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);
  CHECK (precision_of (r, 0) == 0);
}

TEST_CASE ("PR7977 reader: a width outside [0, DB_MAX_CHAR_PRECISION] is folded to 0", "[contract][reader]")
{
  for (int p : { -1, INT_MIN, DB_MAX_CHAR_PRECISION + 1, 4096, INT_MAX })
    {
      const std::string b = char_blob (p);
      hist::HistogramReader r;
      REQUIRE (r.reset (b) == NO_ERROR);
      INFO ("precision " << p);
      CHECK (precision_of (r, 0) == 0);
    }
  const std::string edge = char_blob (DB_MAX_CHAR_PRECISION);
  hist::HistogramReader r;
  REQUIRE (r.reset (edge) == NO_ERROR);
  CHECK (precision_of (r, 0) == DB_MAX_CHAR_PRECISION);
}

TEST_CASE ("PR7977 reader: a width in slot 28 leaves the rest of the header and the values intact "
	   "(old and new binaries read each other's blobs)", "[contract][reader]")
{
  for (int p : { 0, 8, -5 })
    {
      const std::string b = char_blob (p);
      hist::HistogramReader r;
      INFO ("precision " << p);
      REQUIRE (r.reset (b) == NO_ERROR);
      CHECK (r.value_type () == DB_TYPE_CHAR);
      CHECK (r.mcv_count () == 2);
      CHECK (r.bucket_count () == 1);
      CHECK (r.total_rows () == 1600);
      CHECK (r.null_frequency () == 0.125);
      CHECK (r.mcv_freq (1) == 0.5);
      CHECK (r.mcv_hi<std::string_view> (0) == "ab");
      CHECK (r.mcv_hi<std::string_view> (1) == "abcdef");
      CHECK (r.bucket_hi<std::string_view> (0) == "zzzzz");
    }
}

/* ================================================================ ctx: reader + collation */

TEST_CASE ("PR7977 ctx: type and width come from the blob header, charset from the collation", "[contract][ctx]")
{
  const std::string b = char_blob (8);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);
  struct
  {
    int coll;
    int codeset;
  } m[] =
  {
    { LANG_COLL_UTF8_BINARY, INTL_CODESET_UTF8 }, { LANG_COLL_UTF8_KO_CS, INTL_CODESET_UTF8 },
    { LANG_COLL_ISO_EN_CS, INTL_CODESET_ISO88591 }, { LANG_COLL_EUCKR_BINARY, INTL_CODESET_KSC5601_EUC },
    { LANG_COLL_BINARY, INTL_CODESET_RAW_BYTES },
  };
  for (auto &x : m)
    {
      int type = -1, precision = -1, codeset = -1;
      if (!unitseam_repad_ctx (r, x.coll, &type, &precision, &codeset))
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_ctx");
	}
      INFO ("collation " << x.coll);
      CHECK (type == DB_TYPE_CHAR);
      CHECK (precision == 8);
      CHECK (codeset == x.codeset);
    }
}

TEST_CASE ("PR7977 ctx: no collation object for the id falls back to LANG_SYS_CODESET (lang not initialized)",
	   "[contract][ctx]")
{
  const std::string b = char_blob (8);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);

  LANG_COLLATION *saved = lang_Collations[LANG_COLL_UTF8_EN_CS];
  const INTL_CODESET saved_cs = lang_Loc_charset;
  lang_Collations[LANG_COLL_UTF8_EN_CS] = NULL;
  lang_Loc_charset = INTL_CODESET_KSC5601_EUC;
  int type = -1, precision = -1, codeset = -1;
  const bool present = unitseam_repad_ctx (r, LANG_COLL_UTF8_EN_CS, &type, &precision, &codeset);
  lang_Collations[LANG_COLL_UTF8_EN_CS] = saved;
  lang_Loc_charset = saved_cs;
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_ctx");
    }
  CHECK (codeset == INTL_CODESET_KSC5601_EUC);
  CHECK (precision == 8);
}

TEST_CASE ("PR7977 ctx+repad: a CHAR(8) UTF-8 blob re-pads its stored MCV to 8 characters", "[contract][ctx]")
{
  blob::spec s;
  s.type = DB_TYPE_CHAR;
  s.precision = 8;
  s.mcvs = { { U_GA, 0.5 }, { U_GA + U_NA, 0.25 } };
  const std::string b = blob::make (s);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);

  std::string buf;
  bool present = false;
  std::string_view v0 = unitseam_repad_via_reader (r, LANG_COLL_UTF8_BINARY, r.mcv_hi<std::string_view> (0), buf,
			&present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_ctx");
    }
  CHECK (std::string (v0) == U_GA + "       ");
  std::string_view v1 = unitseam_repad_via_reader (r, LANG_COLL_UTF8_BINARY, r.mcv_hi<std::string_view> (1), buf,
			&present);
  CHECK (std::string (v1) == U_GA + U_NA + "      ");
}

TEST_CASE ("PR7977 ctx+repad: width 0 (blob collected before CBRD-27251) disables re-padding", "[contract][ctx]")
{
  const std::string b = char_blob (0);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);
  std::string buf = "untouched";
  bool present = false;
  const std::string_view stored = r.mcv_hi<std::string_view> (0);
  std::string_view v = unitseam_repad_via_reader (r, LANG_COLL_ISO_BINARY, stored, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_ctx");
    }
  CHECK (v.data () == stored.data ());
  CHECK (v == "ab");
  CHECK (buf == "untouched");
}

TEST_CASE ("PR7977 ctx+repad: a corrupt width is folded by the reader and never becomes a pad length",
	   "[contract][ctx]")
{
  for (int p : { 4096, INT_MAX, -8 })
    {
      const std::string b = char_blob (p);
      hist::HistogramReader r;
      REQUIRE (r.reset (b) == NO_ERROR);
      std::string buf = "untouched";
      bool present = false;
      const std::string_view stored = r.mcv_hi<std::string_view> (0);
      std::string_view v = unitseam_repad_via_reader (r, LANG_COLL_ISO_BINARY, stored, buf, &present);
      if (!present)
	{
	  ABSENT_BEFORE_27251 ("histogram_repad_ctx");
	}
      INFO ("precision " << p);
      CHECK (v.data () == stored.data ());
      CHECK (buf == "untouched");
    }
}

TEST_CASE ("PR7977 ctx+repad: a VARCHAR blob is not padded even if its header carries a width", "[contract][ctx]")
{
  const std::string b = char_blob (8, DB_TYPE_STRING);
  hist::HistogramReader r;
  REQUIRE (r.reset (b) == NO_ERROR);
  std::string buf = "untouched";
  bool present = false;
  const std::string_view stored = r.mcv_hi<std::string_view> (0);
  std::string_view v = unitseam_repad_via_reader (r, LANG_COLL_ISO_BINARY, stored, buf, &present);
  if (!present)
    {
      ABSENT_BEFORE_27251 ("histogram_repad_ctx");
    }
  CHECK (v.data () == stored.data ());
  CHECK (buf == "untouched");
}

/* ================================================================ probe key (runs on both sides) */

TEST_CASE ("PR7977 probe: a CHAR literal against a VARCHAR column keeps its trailing spaces "
	   "(CBRD-27251 core; base strips by the literal's type)", "[contract][probe]")
{
  const std::string lit = "abcd ";
  DB_VALUE v;
  make_str (&v, DB_TYPE_CHAR, 5, lit);
  std::string key;
  bool by_column = false;
  REQUIRE (unitseam_extract_str_key (&v, DB_TYPE_STRING, &key, &by_column));
  CHECK (key == "abcd ");
}

TEST_CASE ("PR7977 probe: a VARCHAR constant against a CHAR column is stripped like the stored values "
	   "(base keeps it by the constant's type)", "[contract][probe]")
{
  const std::string hv = "abcd   ";
  DB_VALUE v;
  make_str (&v, DB_TYPE_STRING, DB_MAX_VARCHAR_PRECISION, hv);
  std::string key;
  bool by_column = false;
  REQUIRE (unitseam_extract_str_key (&v, DB_TYPE_CHAR, &key, &by_column));
  CHECK (key == "abcd");
}

TEST_CASE ("PR7977 probe: same-type constants -- CHAR vs CHAR stripped, VARCHAR vs VARCHAR kept", "[contract][probe]")
{
  const std::string a = "abcd    ", b = "abcd  ";
  DB_VALUE va, vb;
  make_str (&va, DB_TYPE_CHAR, 8, a);
  make_str (&vb, DB_TYPE_STRING, DB_MAX_VARCHAR_PRECISION, b);
  std::string ka, kb;
  bool by_column = false;
  REQUIRE (unitseam_extract_str_key (&va, DB_TYPE_CHAR, &ka, &by_column));
  REQUIRE (unitseam_extract_str_key (&vb, DB_TYPE_STRING, &kb, &by_column));
  CHECK (ka == "abcd");
  CHECK (kb == "abcd  ");
}
