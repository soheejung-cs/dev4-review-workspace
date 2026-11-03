/*
 * PR#8378 (CBRD-27847) -- src/storage/heap_file.c, server mode:
 *   heap_attrinfo_set_converted () stores a CHAR/VARCHAR/BIT/VARBIT value that is of the attribute's own domain by
 *   sharing its buffer (pr_type->setval (..., false)) instead of casting it, when the static
 *   heap_attrinfo_can_share_string () says so. The contract the suite holds is the PR's at 91465a14a:
 *     - the plan's type and the value's type are the column's type;
 *     - the value's precision as tp_domain_resolve_value makes it (an unset VARCHAR/VARBIT one, -1 or 0, is the
 *       maximum) is the column's; a precision above the maximum is not taken for the maximum - such a value is cast;
 *     - its collation (TP_EXACT_MATCH, what heap_attrinfo_set () tries first) and its codeset are the column's;
 *     - a fixed-length value fills its precision (CHAR, BIT: length == precision - neither the cast nor writing pads
 *       it, and they pad a shorter one differently); a variable one fits it (VARCHAR: size <= precision, else length
 *       in characters <= precision; VARBIT: bits <= precision);
 *     - it owns no buffer (DB_NEED_CLEAR).
 *   So the stored bytes are the cast's in every codeset, and a shared value is the cast's value in memory as well.
 *
 * What the cases pin down (input -> output):
 *   D*  which (DB_VALUE, TP_DOMAIN) pairs take the shortcut, observed through heap_attrinfo_set_converted () itself:
 *       the attribute's value points at the source's bytes and owns nothing. A revision before the PR casts every
 *       string: the "shared" cases FAIL there (new behavior), the "cast" ones PASS.
 *   E*  the stored value is the cast's: the error code, the record image of the attribute value (get_disk_size_of_value
 *       - which pads a short CHAR in place - then data_writeval, as heap_attrinfo_get_record_payload_size () and
 *       heap_attrinfo_transform_{fixed,variable}_to_disk () do) and the value read back from it equal those of the
 *       value the cast makes (tp_value_auto_cast_with_precision_check_converter () with the plan's converter - the
 *       PR's else branch, and what the base revision runs for every string).
 *   M*  ownership: a shared source is neither freed nor written into; a source that owns its buffer is copied, so its
 *       holder clearing it right after the assignment (locator_mvcc_reev_cond_assigns) does not reach the stored value.
 *   P*  properties over a generated matrix (the five built-in collations' codesets, unset and oversized precisions,
 *       owned or held): the attribute shares exactly when heap_attrinfo_can_share_string () says so, stores what the
 *       cast stores and, when it shares, holds the cast's value in memory (P1); that function is the contract above
 *       (P2).
 *   O1  a BIT shorter than its precision is cast: the attribute holds the cast's bits, in memory too.
 *   U*  [defect] found by this suite at e97790141 - history since (suite.json defects_fixed_in), PASS = the defect is
 *       there: U1/U2 a CHAR shorter than its precision in the binary / EUC-KR charset was shared and padded by the
 *       writer with ASCII spaces where the cast pads with NUL / 0xA1A1; U3 a VARCHAR/VARBIT precision above the
 *       maximum passed the check but the shared setval refused it with ER_INVALID_PRECISION. 91465a14a closed all
 *       three (a CHAR or BIT is shared only at its full length; an oversized precision is cast).
 *
 * Real code at THIS revision linked into the test binary (suite.json extra): heap_file.c (target TU) with
 * object_primitive.c (setval, writers, pr_clear_value), object_domain.c (casts, tp_domain_check, domain cache),
 * object_domain_convert.cpp (converters), string_opfunc.c (db_char_string_coerce, qstr_coerce, db_get_string_length),
 * db_macro.c, object_representation.c (or_* writers), language_support.c, intl_support.c, chartype.c,
 * system_parameter.c (prm_Def defaults), numeric_opfunc.c, db_date.c, mprec.c, dtoa.c, memory_hash.c, oid.c,
 * mem_block.cpp (heap_file.c's static block allocator).
 * Fakes: only what a booted server process has (below) and the attribute cache of one attribute (attr_slot: an
 * OR_ATTRIBUTE with the column's type and domain, one HEAP_ATTRVALUE, a HEAP_CACHE_ATTRINFO with a NULL instance OID -
 * all heap_attrinfo_start_set () reads).
 */
#include "catch.hpp"
#include "dbtype.h"
#include "object_domain.h"
#include "object_domain_convert.h"
#include "object_primitive.h"
#include "object_representation.h"
#include "object_representation_sr.h"
#include "language_support.h"
#include "intl_support.h"
#include "system_parameter.h"
#include "boot_sr.h"
#include "error_manager.h"
#include "memory_alloc.h"
#include "area_alloc.h"
#include "environment_variable.h"
#include "string_regex.hpp"
#include "string_opfunc.h"
#include "heap_file.h"
#include "heap_attrinfo.h"
#include "oid.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
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

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this; DOWN keeps prm_get_value (system_parameter.c) on the global
 * values - the prm_Def defaults - instead of a session's copy, which needs a thread entry */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c envvar_trim_char body */
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

/* memory_alloc.c: a thread's private heap is malloc here. A buffer the test owns (a source value's bytes that the
 * value does not own) is "protected": freeing it is recorded, not done, so a wrong free shows as a failed check
 * instead of a crash */
static std::set < void *>g_protected;
static std::vector < void *>g_freed_protected;

void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  return malloc (size);
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  if (ptr != NULL && g_protected.count (ptr) != 0)
    {
      g_freed_protected.push_back (ptr);
      return;
    }
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

/* lz4 is not linked: a decompression fails as LZ4 does for malformed input. Only a mutation that shares a value
 * carrying a compressed buffer reaches it (the suite's compressed buffers are 8 junk bytes); the read-back of that
 * image then fails instead of the binary aborting on the stub */
extern "C" int
LZ4_decompress_safe (const char *, char *, int, int)
{
  return -1;
}

/* The boot order of a server (boot_restart_server): built-in collations, the type module, then the database's
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset */
static void
boot_types ()
{
  static bool done = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      REQUIRE (tp_init () == NO_ERROR);
      REQUIRE (lang_set_charset_lang ("en_US.utf8") == NO_ERROR);
    }
  er_clear ();
}

/* ---------------------------------------------------------------- hooks.cpp (appended to heap_file.c) */
bool unitseam_can_share_string (const DB_VALUE * value, const TP_DOMAIN * domain, bool *absent);

/* ---------------------------------------------------------------- the harness */
namespace
{
  const int MAXV = DB_MAX_VARCHAR_PRECISION;
  const int MAXB = DB_MAX_VARBIT_PRECISION;
  const int NO_RAW = INT_MIN;

  /* the source value's holder: HELD - a list tuple, a record or a plan constant holds the bytes for the row, the
   * value owns nothing; OWNS_BUF - need_clear (a computed value); OWNS_COMPRESSED - only its compressed buffer */
  enum owner
  { HELD, OWNS_BUF, OWNS_COMPRESSED };

  struct scenario
  {
    std::string name;
    DB_TYPE col_type;
    int col_prec;
    int col_coll;
    DB_TYPE val_type;
    int val_prec;
    int val_coll;
    int val_cs;			/* -1: the collation's codeset */
    std::string bytes;
    int bits;			/* BIT/VARBIT: the bit length; others ignore it */
    DB_TYPE plan_type;		/* the plan's value_type; DB_TYPE_NULL: the value's type */
    owner own;
    int raw_prec;		/* != NO_RAW: written straight into the value's precision (db_value_domain_init would
				 * turn a VARCHAR/VARBIT -1, 0 or oversized one into the maximum) */
  };

  scenario sc (const std::string & name, DB_TYPE col_type, int col_prec, int col_coll, DB_TYPE val_type, int val_prec,
	       int val_coll, const std::string & bytes, int bits = -1)
  {
    scenario s;
    s.name = name;
    s.col_type = col_type;
    s.col_prec = col_prec;
    s.col_coll = col_coll;
    s.val_type = val_type;
    s.val_prec = val_prec;
    s.val_coll = val_coll;
    s.val_cs = -1;
    s.bytes = bytes;
    s.bits = bits;
    s.plan_type = DB_TYPE_NULL;
    s.own = HELD;
    s.raw_prec = NO_RAW;
    return s;
  }

  struct source
  {
    DB_VALUE v;
    char *buf = NULL;
    size_t nbytes = 0;
    char *cbuf = NULL;
    size_t ncbytes = 0;
    owner own = HELD;
  };

  int coll_codeset (int coll)
  {
    return lang_get_collation (coll)->codeset;
  }

  void make_source (source & s, const scenario & c)
  {
    s.own = c.own;
    s.nbytes = c.bytes.size ();
    s.buf = (char *) malloc (s.nbytes + 1);
    memcpy (s.buf, c.bytes.data (), s.nbytes);
    s.buf[s.nbytes] = '\0';
    if (c.own != OWNS_BUF)
      {
	g_protected.insert (s.buf);
      }

    const int cs = c.val_cs >= 0 ? c.val_cs : (TP_IS_CHAR_TYPE (c.val_type) ? coll_codeset (c.val_coll) : 0);
    switch (c.val_type)
      {
      case DB_TYPE_CHAR:
	db_make_char (&s.v, c.val_prec, s.buf, (int) s.nbytes, cs, c.val_coll);
	break;
      case DB_TYPE_VARCHAR:
	db_make_varchar (&s.v, c.val_prec, s.buf, (int) s.nbytes, cs, c.val_coll);
	break;
      case DB_TYPE_BIT:
	db_make_bit (&s.v, c.val_prec, s.buf, c.bits);
	break;
      case DB_TYPE_VARBIT:
	db_make_varbit (&s.v, c.val_prec, s.buf, c.bits);
	break;
      default:
	FAIL ("make_source: not a string type");
      }
    if (c.raw_prec != NO_RAW)
      {
	s.v.domain.char_info.length = c.raw_prec;
      }
    if (c.own == OWNS_BUF)
      {
	s.v.need_clear = true;
      }
    else if (c.own == OWNS_COMPRESSED)
      {
	s.ncbytes = 8;
	s.cbuf = (char *) malloc (s.ncbytes);
	memset (s.cbuf, 'c', s.ncbytes);
	s.v.data.ch.medium.compressed_buf = s.cbuf;
	s.v.data.ch.medium.compressed_size = (int) s.ncbytes;
	s.v.data.ch.info.compressed_need_clear = true;
      }
  }

  void release_source (source & s)
  {
    if (s.own == OWNS_BUF || s.own == OWNS_COMPRESSED)
      {
	pr_clear_value (&s.v);	/* frees what the value owns */
      }
    if (s.own != OWNS_BUF)
      {
	g_protected.erase (s.buf);
	free (s.buf);
      }
    s.buf = NULL;
    s.cbuf = NULL;
  }

  /* the fields a holder of the value would see change */
  std::string fields (const DB_VALUE * v)
  {
    std::ostringstream o;
    o << "type " << (int) DB_VALUE_DOMAIN_TYPE (v) << " null " << (int) v->domain.general_info.is_null
      << " prec " << v->domain.char_info.length << " coll " << v->domain.char_info.collation_id
      << " cs " << (int) v->data.ch.info.codeset << " buf " << (const void *) v->data.ch.medium.buf
      << " size " << v->data.ch.medium.size << " length " << v->data.ch.medium.length
      << " cbuf " << (const void *) v->data.ch.medium.compressed_buf
      << " csize " << v->data.ch.medium.compressed_size << " need_clear " << (int) v->need_clear
      << " cneed_clear " << (int) v->data.ch.info.compressed_need_clear;
    return o.str ();
  }

  std::string hex (const std::string & b)
  {
    static const char *d = "0123456789abcdef";
    std::string h;
    for (unsigned char c:b)
      {
	h += d[c >> 4];
	h += d[c & 15];
      }
    return h;
  }

  /* one attribute's cache: what heap_attrinfo_start_set () reads (num_values, values, inst_oid, last_attrepr) */
  struct attr_slot
  {
    OR_ATTRIBUTE attr;
    HEAP_ATTRVALUE value;
    HEAP_CACHE_ATTRINFO info;

    explicit attr_slot (TP_DOMAIN * catalog)
    {
      attr.id = 7;
      attr.type = TP_DOMAIN_TYPE (catalog);
      attr.domain = catalog;
      attr.is_fixed = catalog->type->is_always_variable ()? 0 : 1;
      memset (&value, 0, sizeof (value));
      value.attrid = 7;
      value.state = HEAP_UNINIT_ATTRVALUE;
      value.attr_type = HEAP_INSTANCE_ATTR;
      value.last_attrepr = &attr;
      value.read_attrepr = NULL;
      db_make_null (&value.dbvalue);
      memset (&info, 0, sizeof (info));
      OID_SET_NULL (&info.class_oid);
      OID_SET_NULL (&info.inst_oid);
      info.last_cacheindex = -1;
      info.read_cacheindex = -1;
      info.num_values = 1;
      info.values = &value;
    }
    ~attr_slot ()
    {
      pr_clear_value (&value.dbvalue);
    }
  };

  /* the record image of an attribute value: the payload size first - get_disk_size_of_value, which pads a short
   * CHAR in place - then data_writeval of the attribute's type (heap_attrinfo_get_record_payload_size (),
   * heap_attrinfo_transform_{fixed,variable}_to_disk ()) */
  std::string image_of (const TP_DOMAIN * catalog, DB_VALUE * v, int *len)
  {
    *len = 0;
    if (DB_IS_NULL (v))
      {
	return "<NULL>";
      }
    const PR_TYPE *t = catalog->type;
    *len = t->get_disk_size_of_value (v);
    std::vector < char >buf ((size_t) * len + 64, '\x7e');
    OR_BUF b;
    or_init (&b, buf.data (), (int) buf.size ());
    if (t->data_writeval (&b, v) != NO_ERROR)
      {
	return "<write error>";
      }
    return std::string (buf.data (), (size_t) (b.ptr - buf.data ()));
  }

  /* the value a reader of that image gets (heap_attrvalue_read: data_readval of the attribute's domain, the image's
   * length) */
  bool read_image (const TP_DOMAIN * catalog, const std::string & image, DB_VALUE * out)
  {
    std::vector < char >buf (image.begin (), image.end ());
    buf.resize (buf.size () + 8, 0);
    OR_BUF b;
    or_init (&b, buf.data (), (int) image.size ());
    db_make_null (out);
    return catalog->type->data_readval (&b, out, (TP_DOMAIN *) catalog, (int) image.size (), true, NULL, 0) == NO_ERROR;
  }

  struct outcome
  {
    /* heap_attrinfo_set_converted () */
    int rc = NO_ERROR;
    int er = NO_ERROR;
    bool shared = false;	/* the attribute value points at the source's bytes and owns nothing */
    bool attr_null = false;
    std::string attr_fields;
    std::string image;
    int image_len = 0;
    /* the cast (tp_value_auto_cast_with_precision_check_converter with the plan's converter) */
    int ref_status = DOMAIN_COMPATIBLE;
    int ref_rc = NO_ERROR;
    int ref_er = NO_ERROR;
    std::string ref_fields;
    std::string ref_image;
    int ref_len = 0;
    int cmp = DB_UNK;		/* tp_value_compare (attribute value, cast value), before the images */
    int read_cmp = DB_UNK;	/* the same on what a reader of the two images gets */
    /* heap_attrinfo_can_share_string () through the hook */
    bool can_share = false;
    bool fn_absent = false;
    /* ownership */
    bool src_intact = true;	/* HELD: the source's fields and bytes are unchanged after set, image and clear */
    int freed_protected = 0;	/* the test's (unowned) source bytes someone freed */
  };

  TP_DOMAIN *column (DB_TYPE type, int precision, int coll)
  {
    return tp_domain_resolve (type, NULL, precision, 0, NULL, coll);
  }

  outcome run (const scenario & c)
  {
    outcome r;
    boot_types ();
    TP_DOMAIN *catalog = column (c.col_type, c.col_prec, c.col_coll);
    REQUIRE (catalog != NULL);
    const DB_TYPE plan_type = c.plan_type != DB_TYPE_NULL ? c.plan_type : c.val_type;
    TP_VALUE_CONVERTER conv = tp_value_find_converter (plan_type, catalog, DOMAIN_CONVERT_ASSIGN);

    source s;
    make_source (s, c);
    const std::string before = fields (&s.v);
    const std::string bytes_before (s.buf, s.nbytes);

    r.can_share = unitseam_can_share_string (&s.v, catalog, &r.fn_absent);

    /* the cast: the PR's else branch, and what the base revision runs for every string */
    DB_VALUE ref;
    db_value_domain_init (&ref, TP_DOMAIN_TYPE (catalog), catalog->precision, catalog->scale);
    er_clear ();
    r.ref_status = tp_value_auto_cast_with_precision_check_converter (&s.v, &ref, catalog, plan_type, conv);
    if (r.ref_status != DOMAIN_COMPATIBLE)
      {
	r.ref_rc = tp_domain_status_er_set ((TP_DOMAIN_STATUS) r.ref_status, ARG_FILE_LINE, &s.v, catalog);
	r.ref_er = er_errid ();
	db_make_null (&ref);
      }
    r.ref_fields = fields (&ref);

    /* heap_attrinfo_set_converted () */
    attr_slot a (catalog);
    er_clear ();
    r.rc = heap_attrinfo_set_converted (NULL, a.value.attrid, &s.v, &a.info, catalog, plan_type, conv);
    r.er = er_errid ();
    DB_VALUE *av = &a.value.dbvalue;
    r.attr_null = DB_IS_NULL (av);
    r.attr_fields = fields (av);
    if (!r.attr_null && TP_IS_CHAR_BIT_TYPE (DB_VALUE_DOMAIN_TYPE (av)))
      {
	r.shared = db_get_string (av) == db_get_string (&s.v) && !DB_NEED_CLEAR (av);
      }
    if (!r.attr_null && !DB_IS_NULL (&ref))
      {
	r.cmp = tp_value_compare (av, &ref, 1, 1);
      }

    /* a holder that owned the value clears it right after the assignment (locator_mvcc_reev_cond_assigns) */
    if (c.own == OWNS_BUF)
      {
	memset (s.buf, 'X', s.nbytes);
      }
    else if (c.own == OWNS_COMPRESSED)
      {
	memset (s.cbuf, 'X', s.ncbytes);
      }

    r.image = image_of (catalog, av, &r.image_len);
    r.ref_image = image_of (catalog, &ref, &r.ref_len);

    if (!r.attr_null && !DB_IS_NULL (&ref))
      {
	DB_VALUE x, y;
	if (read_image (catalog, r.image, &x) && read_image (catalog, r.ref_image, &y))
	  {
	    r.read_cmp = tp_value_compare (&x, &y, 1, 1);
	  }
	pr_clear_value (&x);
	pr_clear_value (&y);
      }

    /* the attribute value is cleared (heap_attrinfo_clear_dbvalues) */
    const size_t freed0 = g_freed_protected.size ();
    pr_clear_value (av);
    pr_clear_value (&ref);
    r.freed_protected = (int) (g_freed_protected.size () - freed0);

    if (c.own == HELD)
      {
	r.src_intact = fields (&s.v) == before && std::string (s.buf, s.nbytes) == bytes_before;
      }
    release_source (s);
    er_clear ();
    return r;
  }

  std::string said (const scenario & c, const outcome & r)
  {
    std::ostringstream o;
    o << c.name << ": rc " << r.rc << " er " << r.er << " shared " << r.shared << " can_share " << r.can_share
      << (r.fn_absent ? " (heap_attrinfo_can_share_string absent: a revision before the PR)" : "")
      << "\n  attr " << r.attr_fields << "\n  cast " << r.ref_fields << " status " << r.ref_status << " rc " << r.ref_rc
      << "\n  image " << hex (r.image) << " (" << r.image_len << ")\n  cast  " << hex (r.ref_image) << " (" << r.ref_len
      << ")\n  in-memory compare " << r.cmp << ", read-back compare " << r.read_cmp;
    return o.str ();
  }

  /* the stored value is the cast's: same error, same record image, equal once read back */
  void check_stores_the_cast (const scenario & c, const outcome & r)
  {
    INFO (said (c, r));
    CHECK (r.rc == r.ref_rc);
    CHECK (r.er == r.ref_er);
    CHECK (r.image == r.ref_image);
    CHECK (r.image_len == r.ref_len);
    if (r.rc == NO_ERROR && !r.attr_null)
      {
	CHECK (r.read_cmp == DB_EQ);
      }
    CHECK (r.freed_protected == 0);
    CHECK (r.src_intact);
  }

  std::string bits_of (std::initializer_list < unsigned char >b)
  {
    return std::string (b.begin (), b.end ());
  }

  const int UTF8_BIN = LANG_COLL_UTF8_BINARY;
  const int UTF8_CI = LANG_COLL_UTF8_EN_CI;
  const int ISO_BIN = LANG_COLL_ISO_BINARY;
  const int EUCKR_BIN = LANG_COLL_EUCKR_BINARY;
  const int BINARY = LANG_COLL_BINARY;

  /* three Hangul syllables in UTF-8: 9 bytes, 3 characters */
  const std::string GANADA = "\xea\xb0\x80\xeb\x82\x98\xeb\x8b\xa4";
  const std::string GANADARA = GANADA + "\xeb\x9d\xbc";
}

/* ======================================================================== D: which pairs take the shortcut */

namespace
{
  /* the stored value is the cast's: same error, same record image, equal once read back */
  void check_stored_as_cast (const outcome & r)
  {
    CHECK (r.rc == r.ref_rc);
    CHECK (r.image == r.ref_image);
    if (r.rc == NO_ERROR && !r.attr_null)
      {
	CHECK (r.read_cmp == DB_EQ);
      }
  }
}

TEST_CASE ("D1 normal: VARCHAR(10) utf8_bin 'abc' of the column's own domain, owning nothing, is stored by sharing its "
	   "buffer", "[contract]")
{
  scenario c = sc ("D1", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc");
  outcome r = run (c);
  INFO (said (c, r));
  CHECK (r.rc == NO_ERROR);
  CHECK (r.can_share);
  CHECK (r.shared);
  check_stored_as_cast (r);
}

TEST_CASE ("D2 normal: a CHAR at its full length (CHAR(10) 'abcdefghij' utf8_bin, iso88591_bin, binary, euckr_bin; "
	   "CHAR(3) of 3 multibyte characters), VARCHAR(10) utf8_en_ci, empty VARCHAR(10): shared", "[contract]")
{
  std::vector < scenario > cs = {
    sc ("CHAR(10) utf8_bin 'abcdefghij'", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abcdefghij"),
    sc ("CHAR(10) iso88591_bin 'abcdefghij'", DB_TYPE_CHAR, 10, ISO_BIN, DB_TYPE_CHAR, 10, ISO_BIN, "abcdefghij"),
    sc ("CHAR(10) binary 'abcdefghij'", DB_TYPE_CHAR, 10, BINARY, DB_TYPE_CHAR, 10, BINARY, "abcdefghij"),
    sc ("CHAR(10) euckr_bin 'abcdefghij'", DB_TYPE_CHAR, 10, EUCKR_BIN, DB_TYPE_CHAR, 10, EUCKR_BIN, "abcdefghij"),
    sc ("CHAR(3) utf8_bin 3 Hangul (9 bytes)", DB_TYPE_CHAR, 3, UTF8_BIN, DB_TYPE_CHAR, 3, UTF8_BIN, GANADA),
    sc ("VARCHAR(10) utf8_en_ci 'AbC'", DB_TYPE_VARCHAR, 10, UTF8_CI, DB_TYPE_VARCHAR, 10, UTF8_CI, "AbC"),
    sc ("VARCHAR(10) ''", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, ""),
  };
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK (r.shared);
      check_stored_as_cast (r);
    }
}

TEST_CASE ("D2b boundary: a CHAR shorter than its precision is cast in every codeset (CHAR(10) 'abc' utf8_bin, "
	   "iso88591_bin, binary, euckr_bin; CHAR(10) '') and stored as the cast pads it", "[contract]")
{
  std::vector < scenario > cs;
  for (int coll:{UTF8_BIN, ISO_BIN, BINARY, EUCKR_BIN})
    {
      cs.push_back (sc ("CHAR(10) 'abc' coll " + std::to_string (coll), DB_TYPE_CHAR, 10, coll, DB_TYPE_CHAR, 10, coll,
			"abc"));
    }
  cs.push_back (sc ("CHAR(10) ''", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, ""));
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK_FALSE (r.can_share);
      CHECK_FALSE (r.shared);
      check_stored_as_cast (r);
    }
}

TEST_CASE ("D3 boundary: VARCHAR(3) of 3 multibyte characters (9 bytes) is shared - its length in characters fits, "
	   "though its size does not", "[contract]")
{
  scenario c = sc ("D3", DB_TYPE_VARCHAR, 3, UTF8_BIN, DB_TYPE_VARCHAR, 3, UTF8_BIN, GANADA);
  outcome r = run (c);
  INFO (said (c, r));
  CHECK (r.rc == NO_ERROR);
  CHECK (r.shared);
  check_stored_as_cast (r);
}

TEST_CASE ("D4 boundary: a VARCHAR/VARBIT value whose precision is -1 or 0 (unset) counts as the maximum, as "
	   "tp_domain_resolve_value makes it: shared into the maximum column, cast into VARCHAR(10)", "[contract]")
{
  /* db_value_domain_init () turns them into the maximum itself (db_macro.c VARCHAR/VARBIT branches): such a value is
   * made by writing domain.char_info.length straight, as tp_value_slam_domain () and the parser do */
  for (int raw:{-1, 0})
    {
      CAPTURE (raw);
      {
	scenario c = sc ("VARCHAR unset precision into VARCHAR(max)", DB_TYPE_VARCHAR, MAXV, UTF8_BIN, DB_TYPE_VARCHAR,
			 MAXV, UTF8_BIN, "abc");
	c.raw_prec = raw;
	outcome r = run (c);
	INFO (said (c, r));
	CHECK (r.rc == NO_ERROR);
	CHECK (r.shared);
      }
      {
	scenario c = sc ("VARCHAR unset precision into VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR,
			 MAXV, UTF8_BIN, "abc");
	c.raw_prec = raw;
	outcome r = run (c);
	INFO (said (c, r));
	CHECK (r.rc == NO_ERROR);
	CHECK_FALSE (r.shared);
      }
      {
	scenario c = sc ("VARBIT unset precision into VARBIT(max)", DB_TYPE_VARBIT, MAXB, 0, DB_TYPE_VARBIT, MAXB, 0,
			 bits_of ({0xa5, 0x80}), 9);
	c.raw_prec = raw;
	outcome r = run (c);
	INFO (said (c, r));
	CHECK (r.rc == NO_ERROR);
	CHECK (r.shared);
      }
    }
}

TEST_CASE ("D5 boundary: BIT(16) of 16 bits and VARBIT(16) of 9 or 16 bits are shared", "[contract]")
{
  std::vector < scenario > cs = {
    sc ("BIT(16) 16 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5, 0x5a}), 16),
    sc ("VARBIT(16) 9 bits", DB_TYPE_VARBIT, 16, 0, DB_TYPE_VARBIT, 16, 0, bits_of ({0xa5, 0x80}), 9),
    sc ("VARBIT(16) 16 bits", DB_TYPE_VARBIT, 16, 0, DB_TYPE_VARBIT, 16, 0, bits_of ({0xa5, 0x5a}), 16),
  };
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK (r.shared);
      check_stored_as_cast (r);
    }
}

TEST_CASE ("D5b boundary: a BIT shorter than its precision (BIT(16) of 8 bits, BIT(12) of 8 bits) or longer (BIT(8) of "
	   "16 bits) is cast", "[contract]")
{
  std::vector < scenario > cs = {
    sc ("BIT(16) 8 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5}), 8),
    sc ("BIT(12) 8 bits", DB_TYPE_BIT, 12, 0, DB_TYPE_BIT, 12, 0, bits_of ({0xa5}), 8),
    sc ("BIT(8) 16 bits", DB_TYPE_BIT, 8, 0, DB_TYPE_BIT, 8, 0, bits_of ({0xa5, 0x5a}), 16),
  };
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK_FALSE (r.can_share);
      CHECK_FALSE (r.shared);
      check_stored_as_cast (r);
    }
}

TEST_CASE ("D6 boundary: a value that owns its buffer or only its compressed buffer is cast into a copy, which its "
	   "holder clearing it right after the assignment does not reach", "[contract]")
{
  for (owner own:{OWNS_BUF, OWNS_COMPRESSED})
    {
      scenario c = sc ("VARCHAR(10) owning", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc");
      c.own = own;
      CAPTURE ((int) own);
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK_FALSE (r.can_share);
      CHECK_FALSE (r.shared);
      /* the image was taken after the source was overwritten: it is still 'abc' */
      CHECK (r.image == r.ref_image);
      CHECK (r.image.find ('X') == std::string::npos);
    }
  scenario c = sc ("CHAR(10) owning", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abcdefghij");
  c.own = OWNS_BUF;
  outcome r = run (c);
  INFO (said (c, r));
  CHECK_FALSE (r.shared);
  CHECK (r.image.find ('X') == std::string::npos);
}

TEST_CASE ("D7 boundary: another collation, another codeset under the column's collation id, another precision (5, "
	   "20), a CHAR of floating precision (-1): cast", "[contract]")
{
  std::vector < scenario > cs = {
    sc ("utf8_en_ci value into utf8_bin VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_CI,
	"abc"),
    sc ("VARCHAR(20) value into VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 20, UTF8_BIN, "abc"),
    sc ("VARCHAR(5) value into VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 5, UTF8_BIN, "abc"),
    sc ("CHAR(5) value into CHAR(10)", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 5, UTF8_BIN, "abcde"),
    sc ("CHAR(-1) literal into CHAR(10)", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, -1, UTF8_BIN, "abcdefghij"),
  };
  /* ISO-8859-1 bytes labelled with the utf8_bin collation id: the cast converts them (0xe9 -> 0xc3 0xa9) */
  scenario cs_iso = sc ("ISO-8859-1 codeset value, utf8_bin collation id, into utf8_bin VARCHAR(10)", DB_TYPE_VARCHAR,
			10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "caf\xe9");
  cs_iso.val_cs = INTL_CODESET_ISO88591;
  cs.push_back (cs_iso);
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK_FALSE (r.can_share);
      CHECK_FALSE (r.shared);
    }
  outcome r = run (cs_iso);
  CHECK (hex (r.image).find ("c3a9") != std::string::npos);
}

TEST_CASE ("D8 boundary: a value longer than its own precision (VARCHAR(3) 'abcd', VARCHAR(3) of 4 multibyte "
	   "characters, CHAR(3) 'abc  ') is cast: the overflow error, or the trailing pad dropped", "[contract]")
{
  std::vector < scenario > over = {
    sc ("VARCHAR(3) 'abcd'", DB_TYPE_VARCHAR, 3, UTF8_BIN, DB_TYPE_VARCHAR, 3, UTF8_BIN, "abcd"),
    sc ("VARCHAR(3) 4 Hangul", DB_TYPE_VARCHAR, 3, UTF8_BIN, DB_TYPE_VARCHAR, 3, UTF8_BIN, GANADARA),
  };
  for (const scenario & c:over)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK_FALSE (r.can_share);
      CHECK (r.rc == ER_IT_DATA_OVERFLOW);
      CHECK (r.attr_null);
    }
  scenario c = sc ("CHAR(3) 'abc  '", DB_TYPE_CHAR, 3, UTF8_BIN, DB_TYPE_CHAR, 3, UTF8_BIN, "abc  ");
  outcome r = run (c);
  INFO (said (c, r));
  CHECK_FALSE (r.shared);
  CHECK (r.rc == NO_ERROR);
  CHECK (r.image == r.ref_image);
}

TEST_CASE ("D9 guard: the plan's type is not the value's (VARCHAR plan, CHAR value) or the value's type is not the "
	   "column's (VARCHAR value into CHAR(10)): cast", "[contract]")
{
  scenario a = sc ("VARCHAR plan, CHAR(10) value, VARCHAR(10) column", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10,
		   UTF8_BIN, "abcdefghij");
  a.plan_type = DB_TYPE_VARCHAR;
  scenario b = sc ("VARCHAR(10) value into CHAR(10)", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN,
		   "abcdefghij");
  for (const scenario & c:{a, b})
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.rc == NO_ERROR);
      CHECK_FALSE (r.shared);
    }
}

TEST_CASE ("D10 the branches the PR leaves: NULL stays NULL, INTEGER is cloned, NUMERIC(10,2) is cast", "[contract]")
{
  boot_types ();
  struct one
  {
    TP_DOMAIN *catalog;
    DB_VALUE v;
  } cases[3];
  cases[0].catalog = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 10, 0, NULL, UTF8_BIN);
  db_make_null (&cases[0].v);
  cases[1].catalog = tp_domain_resolve_default (DB_TYPE_INTEGER);
  db_make_int (&cases[1].v, 42);
  cases[2].catalog = tp_domain_resolve (DB_TYPE_NUMERIC, NULL, 10, 2, NULL, 0);
  REQUIRE (numeric_coerce_string_to_num ("12.5", 4, INTL_CODESET_ISO88591, &cases[2].v) == NO_ERROR);
  for (int i = 0; i < 3; i++)
    {
      TP_DOMAIN *catalog = cases[i].catalog;
      DB_TYPE vt = DB_IS_NULL (&cases[i].v) ? TP_DOMAIN_TYPE (catalog) : DB_VALUE_DOMAIN_TYPE (&cases[i].v);
      TP_VALUE_CONVERTER conv = tp_value_find_converter (vt, catalog, DOMAIN_CONVERT_ASSIGN);
      attr_slot a (catalog);
      CAPTURE (i);
      CHECK (heap_attrinfo_set_converted (NULL, a.value.attrid, &cases[i].v, &a.info, catalog, vt, conv) == NO_ERROR);
      CHECK (a.value.state == HEAP_WRITTEN_ATTRVALUE);
      if (i == 0)
	{
	  CHECK (DB_IS_NULL (&a.value.dbvalue));
	}
      else if (i == 1)
	{
	  CHECK (db_get_int (&a.value.dbvalue) == 42);
	}
      else
	{
	  CHECK (DB_VALUE_DOMAIN_TYPE (&a.value.dbvalue) == DB_TYPE_NUMERIC);
	  CHECK (db_value_precision (&a.value.dbvalue) == 10);
	  CHECK (db_value_scale (&a.value.dbvalue) == 2);
	}
      pr_clear_value (&cases[i].v);
    }
}

TEST_CASE ("D11 boundary: a VARCHAR/VARBIT precision above the maximum is not taken for the maximum: cast, stored as "
	   "the cast stores it (no ER_INVALID_PRECISION)", "[contract]")
{
  scenario v = sc ("VARCHAR precision above the maximum into VARCHAR(max)", DB_TYPE_VARCHAR, MAXV, UTF8_BIN,
		   DB_TYPE_VARCHAR, MAXV, UTF8_BIN, "abc");
  v.raw_prec = MAXV + 5;
  scenario b = sc ("VARBIT precision above the maximum into VARBIT(max)", DB_TYPE_VARBIT, MAXB, 0, DB_TYPE_VARBIT,
		   MAXB, 0, bits_of ({0xa5, 0x80}), 9);
  b.raw_prec = MAXB + 5;
  for (const scenario & c:{v, b})
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK_FALSE (r.can_share);
      CHECK_FALSE (r.shared);
      CHECK (r.rc == NO_ERROR);
      check_stored_as_cast (r);
    }
}

/* ======================================================================== E: the stored value is the cast's */

namespace
{
  /* the D scenarios and more: every built-in codeset, short and full CHAR/BIT, mismatches, owning, oversized */
  std::vector < scenario > all_scenarios ()
  {
    std::vector < scenario > v = {
      sc ("VARCHAR(10) 'abc'", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc"),
      sc ("CHAR(10) 'abc'", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abc"),
      sc ("CHAR(10) 'ab  '", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "ab  "),
      sc ("CHAR(10) ''", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, ""),
      sc ("CHAR(10) 'abcdefghij'", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abcdefghij"),
      sc ("CHAR(4) Hangul x3", DB_TYPE_CHAR, 4, UTF8_BIN, DB_TYPE_CHAR, 4, UTF8_BIN, GANADA),
      sc ("CHAR(3) Hangul x3", DB_TYPE_CHAR, 3, UTF8_BIN, DB_TYPE_CHAR, 3, UTF8_BIN, GANADA),
      sc ("VARCHAR(10) utf8_en_ci", DB_TYPE_VARCHAR, 10, UTF8_CI, DB_TYPE_VARCHAR, 10, UTF8_CI, "AbC"),
      sc ("CHAR(10) utf8_en_ci", DB_TYPE_CHAR, 10, UTF8_CI, DB_TYPE_CHAR, 10, UTF8_CI, "AbC"),
      sc ("CHAR(10) iso88591_bin 'ab\\xe9'", DB_TYPE_CHAR, 10, ISO_BIN, DB_TYPE_CHAR, 10, ISO_BIN, "ab\xe9"),
      sc ("CHAR(3) iso88591_bin 'ab\\xe9'", DB_TYPE_CHAR, 3, ISO_BIN, DB_TYPE_CHAR, 3, ISO_BIN, "ab\xe9"),
      sc ("CHAR(10) binary 'abc'", DB_TYPE_CHAR, 10, BINARY, DB_TYPE_CHAR, 10, BINARY, "abc"),
      sc ("CHAR(10) binary 'abcdefghij'", DB_TYPE_CHAR, 10, BINARY, DB_TYPE_CHAR, 10, BINARY, "abcdefghij"),
      sc ("VARCHAR(10) binary 'abc'", DB_TYPE_VARCHAR, 10, BINARY, DB_TYPE_VARCHAR, 10, BINARY, "abc"),
      sc ("CHAR(10) euckr_bin 'abc'", DB_TYPE_CHAR, 10, EUCKR_BIN, DB_TYPE_CHAR, 10, EUCKR_BIN, "abc"),
      sc ("CHAR(10) euckr_bin 'abcdefghij'", DB_TYPE_CHAR, 10, EUCKR_BIN, DB_TYPE_CHAR, 10, EUCKR_BIN, "abcdefghij"),
      sc ("VARCHAR(10) euckr_bin 'abc'", DB_TYPE_VARCHAR, 10, EUCKR_BIN, DB_TYPE_VARCHAR, 10, EUCKR_BIN, "abc"),
      sc ("VARCHAR(3) Hangul x3", DB_TYPE_VARCHAR, 3, UTF8_BIN, DB_TYPE_VARCHAR, 3, UTF8_BIN, GANADA),
      sc ("VARCHAR(10) ''", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, ""),
      sc ("BIT(16) 16 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5, 0x5a}), 16),
      sc ("BIT(16) 8 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5}), 8),
      sc ("BIT(12) 12 bits", DB_TYPE_BIT, 12, 0, DB_TYPE_BIT, 12, 0, bits_of ({0xa5, 0x50}), 12),
      sc ("VARBIT(16) 9 bits", DB_TYPE_VARBIT, 16, 0, DB_TYPE_VARBIT, 16, 0, bits_of ({0xa5, 0x80}), 9),
      sc ("VARCHAR(3) 'abcd' into VARCHAR(3)", DB_TYPE_VARCHAR, 3, UTF8_BIN, DB_TYPE_VARCHAR, 3, UTF8_BIN, "abcd"),
      sc ("CHAR(3) 'abc  '", DB_TYPE_CHAR, 3, UTF8_BIN, DB_TYPE_CHAR, 3, UTF8_BIN, "abc  "),
      sc ("BIT(8) 16 bits", DB_TYPE_BIT, 8, 0, DB_TYPE_BIT, 8, 0, bits_of ({0xa5, 0x5a}), 16),
      sc ("BIT(8) 16 bits, zero tail", DB_TYPE_BIT, 8, 0, DB_TYPE_BIT, 8, 0, bits_of ({0xa5, 0x00}), 16),
      sc ("VARCHAR(20) into VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 20, UTF8_BIN, "abc"),
      sc ("CHAR(5) into CHAR(10)", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 5, UTF8_BIN, "abc"),
      sc ("utf8_en_ci into utf8_bin", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_CI, "abc"),
    };
    scenario raw = sc ("VARCHAR -1 into VARCHAR(max)", DB_TYPE_VARCHAR, MAXV, UTF8_BIN, DB_TYPE_VARCHAR, MAXV, UTF8_BIN,
		       "abc");
    raw.raw_prec = -1;
    v.push_back (raw);
    scenario big = sc ("VARCHAR above the maximum into VARCHAR(max)", DB_TYPE_VARCHAR, MAXV, UTF8_BIN, DB_TYPE_VARCHAR,
		       MAXV, UTF8_BIN, "abc");
    big.raw_prec = MAXV + 5;
    v.push_back (big);
    scenario bigb = sc ("VARBIT above the maximum into VARBIT(max)", DB_TYPE_VARBIT, MAXB, 0, DB_TYPE_VARBIT, MAXB, 0,
			bits_of ({0xa5, 0x80}), 9);
    bigb.raw_prec = MAXB + 5;
    v.push_back (bigb);
    scenario iso = sc ("ISO-8859-1 bytes, utf8_bin id", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN,
		       "caf\xe9");
    iso.val_cs = INTL_CODESET_ISO88591;
    v.push_back (iso);
    for (owner own:{OWNS_BUF, OWNS_COMPRESSED})
      {
	scenario o = sc ("owning VARCHAR(10)", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc");
	o.own = own;
	v.push_back (o);
      }
    scenario g1 = sc ("VARCHAR plan, CHAR value", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abc");
    g1.plan_type = DB_TYPE_VARCHAR;
    v.push_back (g1);
    v.push_back (sc ("VARCHAR into CHAR(10)", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc"));
    return v;
  }
}

TEST_CASE ("E1 the stored value is the cast's in every codeset: same error, same record image and an equal value read "
	   "back (CHAR/VARCHAR utf8, iso88591, binary, euckr - short and full; BIT/VARBIT; mismatched, owning, "
	   "oversized)", "[contract]")
{
  for (const scenario & c:all_scenarios ())
    {
      outcome r = run (c);
      check_stores_the_cast (c, r);
    }
}

TEST_CASE ("E2 CHAR(10) 'abc' (shorter than its precision) is stored as the cast pads it: 'abc' + 7 spaces in utf8_bin, "
	   "'abc' + 7 NUL bytes in binary", "[contract]")
{
  {
    scenario c = sc ("E2 utf8_bin", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abc");
    outcome r = run (c);
    INFO (said (c, r));
    CHECK (r.image.find ("abc       ") != std::string::npos);
    CHECK (r.image == r.ref_image);
  }
  {
    scenario c = sc ("E2 binary", DB_TYPE_CHAR, 10, BINARY, DB_TYPE_CHAR, 10, BINARY, "abc");
    outcome r = run (c);
    INFO (said (c, r));
    CHECK (r.image.find (std::string ("abc\0\0\0\0\0\0\0", 10)) != std::string::npos);
    CHECK (r.image == r.ref_image);
  }
}

/* ======================================================================== M: ownership */

TEST_CASE ("M1 a shared source is neither freed nor written into: after the record image and the attribute's clear, "
	   "the source's fields and bytes are unchanged", "[contract]")
{
  std::vector < scenario > cs = {
    sc ("CHAR(10) 'abcdefghij'", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abcdefghij"),
    sc ("CHAR(10) 'abc'", DB_TYPE_CHAR, 10, UTF8_BIN, DB_TYPE_CHAR, 10, UTF8_BIN, "abc"),
    sc ("VARCHAR(10) 'abc'", DB_TYPE_VARCHAR, 10, UTF8_BIN, DB_TYPE_VARCHAR, 10, UTF8_BIN, "abc"),
    sc ("BIT(16) 16 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5, 0x5a}), 16),
    sc ("BIT(16) 8 bits", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5}), 8),
    sc ("VARBIT(16) 9 bits", DB_TYPE_VARBIT, 16, 0, DB_TYPE_VARBIT, 16, 0, bits_of ({0xa5, 0x80}), 9),
  };
  for (const scenario & c:cs)
    {
      outcome r = run (c);
      INFO (said (c, r));
      CHECK (r.freed_protected == 0);
      CHECK (r.src_intact);
    }
}

/* ======================================================================== P: properties */

namespace
{
  std::vector < scenario > matrix ()
  {
    std::vector < scenario > out;
    struct col
    {
      DB_TYPE t;
      int prec;
      int coll;
    };
    const std::vector < col > cols = {
      {DB_TYPE_VARCHAR, 3, UTF8_BIN}, {DB_TYPE_VARCHAR, 10, UTF8_BIN}, {DB_TYPE_VARCHAR, MAXV, UTF8_BIN},
      {DB_TYPE_VARCHAR, 10, UTF8_CI}, {DB_TYPE_VARCHAR, 10, ISO_BIN}, {DB_TYPE_VARCHAR, 10, BINARY},
      {DB_TYPE_VARCHAR, 10, EUCKR_BIN},
      {DB_TYPE_CHAR, 3, UTF8_BIN}, {DB_TYPE_CHAR, 10, UTF8_BIN}, {DB_TYPE_CHAR, 10, UTF8_CI},
      {DB_TYPE_CHAR, 10, ISO_BIN}, {DB_TYPE_CHAR, 10, BINARY}, {DB_TYPE_CHAR, 10, EUCKR_BIN},
      {DB_TYPE_BIT, 8, 0}, {DB_TYPE_BIT, 16, 0}, {DB_TYPE_VARBIT, 8, 0}, {DB_TYPE_VARBIT, 16, 0},
      {DB_TYPE_VARBIT, MAXB, 0},
    };
    const std::vector < int >char_colls = { UTF8_BIN, UTF8_CI, ISO_BIN, BINARY, EUCKR_BIN };
    const std::vector < std::string > texts = { "", "abc", "ab  ", "abcdefghij", "abcd", "abc  ", GANADA };
    const std::vector < std::pair < std::string, int >>bitvals = {
      {bits_of ({}), 0}, {bits_of ({0xa5}), 8}, {bits_of ({0xa5, 0x80}), 9}, {bits_of ({0xa5, 0x5a}), 16},
      {bits_of ({0xa5, 0x00}), 16},
    };
    const std::vector < int >precs = { 3, 5, 8, 9, 10, 16, 20 };
    for (const col & k:cols)
      {
	const bool is_char = TP_IS_CHAR_TYPE (k.t);
	std::vector < int >ps = precs;
	ps.push_back (k.t == DB_TYPE_VARCHAR ? MAXV : k.t == DB_TYPE_VARBIT ? MAXB : 2048);
	std::vector < int >raws = { NO_RAW };
	if (k.t == DB_TYPE_VARCHAR || k.t == DB_TYPE_VARBIT)
	  {
	    raws = { NO_RAW, -1, 0, (k.t == DB_TYPE_VARCHAR ? MAXV : MAXB) + 5 };
	  }
	else
	  {
	    raws = { NO_RAW, -1 };
	  }
	for (int p:ps)
	  for (int raw:raws)
	    {
	      if (raw != NO_RAW && p != ps.front ())
		{
		  continue;	/* a raw precision replaces the made one: once is enough */
		}
	      for (owner own:{HELD, OWNS_BUF})
		{
		  if (is_char)
		    {
		      for (int vc:char_colls)
			for (const std::string & t:texts)
			  {
			    if (t == GANADA && coll_codeset (vc) != INTL_CODESET_UTF8)
			      {
				continue;
			      }
			    scenario s = sc ("matrix", k.t, k.prec, k.coll, k.t, p, vc, t);
			    s.raw_prec = raw;
			    s.own = own;
			    out.push_back (s);
			  }
		    }
		  else
		    {
		      for (const auto & b:bitvals)
			{
			  scenario s = sc ("matrix", k.t, k.prec, 0, k.t, p, 0, b.first, b.second);
			  s.raw_prec = raw;
			  s.own = own;
			  out.push_back (s);
			}
		    }
		}
	    }
      }
    return out;
  }

  std::string describe (const scenario & s)
  {
    std::ostringstream o;
    o << "col " << (int) s.col_type << "(" << s.col_prec << ") coll " << s.col_coll << " <- val " << (int) s.val_type
      << "(" << s.val_prec << (s.raw_prec != NO_RAW ? " raw " : "") << (s.raw_prec !=
									NO_RAW ? std::to_string (s.raw_prec) : "")
      << ") coll " << s.val_coll << " own " << (int) s.own << " bytes " << hex (s.bytes) << " bits " << s.bits;
    return o.str ();
  }
}

TEST_CASE ("P1 property over a generated matrix (VARCHAR/CHAR in utf8_bin, utf8_en_ci, iso88591_bin, binary, "
	   "euckr_bin; BIT/VARBIT; precisions, unset -1/0 and above the maximum, owned or held): the attribute shares "
	   "exactly when heap_attrinfo_can_share_string says so, stores what the cast stores (error, record image, value "
	   "read back) and, when it shares, holds the cast's value in memory", "[contract]")
{
  int n = 0, n_shared = 0, n_err = 0;
  for (const scenario & c:matrix ())
    {
      outcome r = run (c);
      n++;
      n_shared += r.shared ? 1 : 0;
      n_err += r.rc != NO_ERROR ? 1 : 0;
      if (r.shared != r.can_share || r.rc != r.ref_rc || r.er != r.ref_er || r.image != r.ref_image
	  || r.image_len != r.ref_len || (r.rc == NO_ERROR && !r.attr_null && r.read_cmp != DB_EQ)
	  || (r.shared && r.cmp != DB_EQ) || r.freed_protected != 0 || !r.src_intact)
	{
	  scenario d = c;
	  d.name = describe (c);
	  INFO (said (d, r));
	  CHECK (r.shared == r.can_share);
	  CHECK (r.rc == r.ref_rc);
	  CHECK (r.image == r.ref_image);
	  CHECK (r.read_cmp == DB_EQ);
	  if (r.shared)
	    {
	      CHECK (r.cmp == DB_EQ);
	    }
	  CHECK (r.freed_protected == 0);
	  CHECK (r.src_intact);
	}
    }
  WARN ("P1 matrix: " << n << " pairs, " << n_shared << " shared, " << n_err << " errors (as the cast)");
  CHECK (n > 1000);
}

TEST_CASE ("P2 property: heap_attrinfo_can_share_string (v, d) iff heap_attrinfo_set's first try matches "
	   "(tp_domain_check TP_EXACT_MATCH), the codesets agree, a fixed-length value fills the precision (CHAR, BIT: "
	   "length = precision) and a variable one fits it, the precision is not above the maximum, and the value owns "
	   "nothing", "[contract]")
{
  boot_types ();
  int n = 0, n_true = 0;
  bool absent = false;
  for (const scenario & c:matrix ())
    {
      TP_DOMAIN *catalog = column (c.col_type, c.col_prec, c.col_coll);
      source s;
      make_source (s, c);
      bool fn_absent = false;
      const bool can = unitseam_can_share_string (&s.v, catalog, &fn_absent);
      absent = absent || fn_absent;
      const bool exact = tp_domain_check (catalog, &s.v, TP_EXACT_MATCH) == DOMAIN_COMPATIBLE;
      const bool same_cs = !TP_IS_CHAR_TYPE (c.col_type) || db_get_string_codeset (&s.v) == TP_DOMAIN_CODESET (catalog);
      const bool fixed = c.col_type == DB_TYPE_CHAR || c.col_type == DB_TYPE_BIT;
      const int len = db_get_string_length (&s.v);
      const bool fits = fixed ? len == catalog->precision : len <= catalog->precision;
      const bool oversized = (c.col_type == DB_TYPE_VARCHAR && db_value_precision (&s.v) > MAXV)
	|| (c.col_type == DB_TYPE_VARBIT && db_value_precision (&s.v) > MAXB);
      const bool owns = DB_NEED_CLEAR (&s.v);
      const bool want = exact && same_cs && fits && !oversized && !owns;
      n++;
      n_true += want ? 1 : 0;
      if (can != want)
	{
	  FAIL_CHECK (describe (c) << ": can_share " << can << " exact " << exact << " same_cs " << same_cs << " fits "
		      << fits << " oversized " << oversized << " owns " << owns << (fn_absent ? " (function absent)" :
										   ""));
	}
      release_source (s);
    }
  WARN ("P2 matrix: " << n << " pairs, " << n_true << " to share" << (absent ? " (function absent)" : ""));
  CHECK_FALSE (absent);
}

/* ======================================================================== O: observed */

TEST_CASE ("O1 a BIT(16) value of 8 bits is cast: the attribute holds the cast's 16 bits, equal to it in memory as "
	   "well as on disk (e97790141 shared it - 8 bits, less than the cast's until the writer padded it)",
	   "[contract]")
{
  scenario c = sc ("O1", DB_TYPE_BIT, 16, 0, DB_TYPE_BIT, 16, 0, bits_of ({0xa5}), 8);
  outcome r = run (c);
  INFO (said (c, r));
  REQUIRE (r.rc == NO_ERROR);
  CHECK_FALSE (r.shared);
  CHECK (r.cmp == DB_EQ);
  CHECK (r.image == r.ref_image);
  CHECK (r.read_cmp == DB_EQ);
}

/* ======================================================================== U: found by this suite */

TEST_CASE ("PR8378 U1 (latent): CHAR(10) CHARSET binary 'abc' of the column's domain is shared and the writer pads it "
	   "with spaces where the cast pads it with NUL bytes - the stored record differs from the cast's and compares "
	   "unequal to it once read back (PASS = defect present)", "[defect]")
{
  scenario c = sc ("U1", DB_TYPE_CHAR, 10, BINARY, DB_TYPE_CHAR, 10, BINARY, "abc");
  outcome r = run (c);
  INFO (said (c, r));
  REQUIRE (r.rc == NO_ERROR);
  CHECK (r.shared);
  CHECK (r.image != r.ref_image);
  CHECK (hex (r.image).find (hex ("abc       ")) != std::string::npos);
  CHECK (hex (r.ref_image).find (hex (std::string ("abc\0\0\0\0\0\0\0", 10))) != std::string::npos);
  CHECK (r.read_cmp != DB_EQ);
}

TEST_CASE ("PR8378 U2 (latent): CHAR(10) euckr_bin 'abc' of the column's domain is shared and the writer pads it with "
	   "7 ASCII spaces (10 bytes) where the cast pads it with 7 EUC-KR spaces 0xA1A1 (17 bytes); the two compare "
	   "equal once read back (PASS = defect present)", "[defect]")
{
  scenario c = sc ("U2", DB_TYPE_CHAR, 10, EUCKR_BIN, DB_TYPE_CHAR, 10, EUCKR_BIN, "abc");
  outcome r = run (c);
  INFO (said (c, r));
  REQUIRE (r.rc == NO_ERROR);
  CHECK (r.shared);
  CHECK (r.image != r.ref_image);
  CHECK (hex (r.image).find (hex ("abc       ")) != std::string::npos);
  CHECK (hex (r.ref_image).find ("616263a1a1a1a1a1a1a1a1a1a1a1a1a1a1") != std::string::npos);
  CHECK (r.read_cmp == DB_EQ);
}

TEST_CASE ("PR8378 U3 (latent): a VARCHAR/VARBIT value whose precision is above the maximum passes "
	   "heap_attrinfo_can_share_string (counted as the maximum) but the shared setval refuses it with "
	   "ER_INVALID_PRECISION where the cast stores it (PASS = defect present)", "[defect]")
{
  {
    scenario c = sc ("U3 VARCHAR", DB_TYPE_VARCHAR, MAXV, UTF8_BIN, DB_TYPE_VARCHAR, MAXV, UTF8_BIN, "abc");
    c.raw_prec = MAXV + 5;
    outcome r = run (c);
    INFO (said (c, r));
    CHECK (r.can_share);
    CHECK (r.rc == ER_INVALID_PRECISION);
    CHECK (r.ref_rc == NO_ERROR);
  }
  {
    scenario c = sc ("U3 VARBIT", DB_TYPE_VARBIT, MAXB, 0, DB_TYPE_VARBIT, MAXB, 0, bits_of ({0xa5, 0x80}), 9);
    c.raw_prec = MAXB + 5;
    outcome r = run (c);
    INFO (said (c, r));
    CHECK (r.can_share);
    CHECK (r.rc == ER_INVALID_PRECISION);
    CHECK (r.ref_rc == NO_ERROR);
  }
}
