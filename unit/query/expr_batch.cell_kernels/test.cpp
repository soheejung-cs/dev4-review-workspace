/*
 * PR#8196 (CBRD-27361) -- the page batch's cell readers and string equality kernels (src/query/expr_batch.c,
 * server mode), TU-static, reached through the appendix:
 *
 *   eb_str_eq_class ()   "=" under a byte-lockstep collation (eb_str_eq_kind () == EB_STR_EQ_CLASS): the bytes are
 *                        equal, or both of the space class {0x20, 0x00} at a position; of different lengths, equal only
 *                        when ti and the longer one's tail is all space class -- the equality lang_fastcmp_byte ()
 *                        (language_support.c) gives with identity weights: SPACE read as ZERO inside, size1 - size2
 *                        without ti, weights_ti of the tail with it
 *   eb_str_eq_binary ()  "=" under the binary collation: a byte prefix, then a tail of 0x00 only
 *   eb_num_from_disk ()  a NUMERIC cell of at most 8 coefficient bytes decoded in place, else numeric_agg_expr_from_disk ()
 *                        (numeric_opfunc.c, linked as the real TU) -- the two must read every encoding the same way
 *   eb_cell_str_long ()  a long-format string (0xFF, compressed length, length, bytes)
 *
 *   [contract]  the kernels against the reference model over every string of length 0..4 from {a, b, 0x20, 0x00}
 *               (and the length-first order of review A-07 against the same model); the in-place NUMERIC decode
 *               against numeric_agg_expr_from_disk () over 20 000 random encodings of 3..20 bytes; a long uncompressed
 *               value read in place when the column has a scratch
 *   [defect]    A-11: a long uncompressed value of a column without a scratch (max_bytes 0) is refused (-1), which sends
 *               the chunk to the row path although no scratch is needed (PASS = the defect is there)
 *
 * Faked: nothing on these paths (compressed values -- which would reach the LZ4 stub -- are not used).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "numeric_opfunc.h"
#include "object_representation.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern bool unitseam_eb_str_eq_class (const unsigned char *a, int na, const unsigned char *b, int nb, bool ti);
extern bool unitseam_eb_str_eq_binary (const unsigned char *a, int na, const unsigned char *b, int nb);
extern int unitseam_eb_cell_str_long (int max_bytes, char *scratch, unsigned char *scratch_set, int r,
				      const unsigned char *d, int avail, const char **str, int *len);
extern bool unitseam_eb_num_from_disk (const unsigned char *disk, int avail, NUMERIC_AGG_EXPR_VAL * num);

/* ------------------------------------------------------------------ reference models */

static inline bool
space_class (unsigned char c)
{
  return c == 0x20 || c == 0x00;
}

/* lang_fastcmp_byte () == 0 with identity weights (weights_ti maps the space class to 0) */
static bool
ref_eq_class (const std::string & a, const std::string & b, bool ti)
{
  size_t n = std::min (a.size (), b.size ()), i;

  for (i = 0; i < n; i++)
    {
      unsigned char ca = (unsigned char) a[i], cb = (unsigned char) b[i];

      if ((ca == 0x20 ? 0 : ca) != (cb == 0x20 ? 0 : cb))
	{
	  return false;
	}
    }
  if (a.size () == b.size ())
    {
      return true;
    }
  if (!ti)
    {
      return false;
    }
  const std::string & l = (a.size () > b.size ()) ? a : b;
  for (i = n; i < l.size (); i++)
    {
      if (!space_class ((unsigned char) l[i]))
	{
	  return false;
	}
    }
  return true;
}

static bool
ref_eq_binary (const std::string & a, const std::string & b)
{
  size_t n = std::min (a.size (), b.size ()), i;

  if (memcmp (a.data (), b.data (), n) != 0)
    {
      return false;
    }
  const std::string & l = (a.size () > b.size ()) ? a : b;
  for (i = n; i < l.size (); i++)
    {
      if (l[i] != 0)
	{
	  return false;
	}
    }
  return true;
}

/* review A-07: the same equality with the lengths (and the tail) decided before memcmp */
static bool
a07_eq_class (const unsigned char *a, int na, const unsigned char *b, int nb, bool ti)
{
  int n = (na < nb) ? na : nb, i;
  const unsigned char *tail;

  if (na != nb)
    {
      if (!ti)
	{
	  return false;
	}
      tail = (na > nb) ? a + n : b + n;
      for (i = 0; i < ((na > nb) ? na - nb : nb - na); i++)
	{
	  if (!space_class (tail[i]))
	    {
	      return false;
	    }
	}
    }
  if (memcmp (a, b, (size_t) n) == 0)
    {
      return true;
    }
  for (i = 0; i < n; i++)
    {
      if (a[i] != b[i] && !(space_class (a[i]) && space_class (b[i])))
	{
	  return false;
	}
    }
  return true;
}

static std::vector < std::string > all_strings (int maxlen)
{
  static const char alpha[] = { 'a', 'b', 0x20, 0x00 };
  std::vector < std::string > out (1, std::string ());
  size_t from = 0;

  for (int len = 1; len <= maxlen; len++)
    {
      size_t to = out.size ();
      for (size_t k = from; k < to; k++)
	{
	  for (char c:alpha)
	    {
	      out.push_back (out[k] + c);
	    }
	}
      from = to;
    }
  return out;
}

#define U(s) ((const unsigned char *) (s).data ())

TEST_CASE ("contract: the lockstep equality kernels agree with the collation's compare", "[contract]")
{
  std::vector < std::string > s = all_strings (4);
  long mism_class = 0, mism_a07 = 0, mism_bin = 0, pairs = 0;

  for (const std::string & a:s)
    {
      for (const std::string & b:s)
	{
	  for (int ti = 0; ti < 2; ti++)
	    {
	      bool want = ref_eq_class (a, b, ti != 0);

	      mism_class += (unitseam_eb_str_eq_class (U (a), (int) a.size (), U (b), (int) b.size (), ti != 0) != want);
	      mism_a07 += (a07_eq_class (U (a), (int) a.size (), U (b), (int) b.size (), ti != 0) != want);
	    }
	  mism_bin += (unitseam_eb_str_eq_binary (U (a), (int) a.size (), U (b), (int) b.size ()) != ref_eq_binary (a, b));
	  pairs++;
	}
    }
  INFO ("pairs " << pairs);
  CHECK (mism_class == 0);
  CHECK (mism_a07 == 0);
  CHECK (mism_bin == 0);
}

/* a NUMERIC cell as mr_data_writeval_numeric () lays it out: header[0] = size | sign, header[1] = precision | scale sign,
 * header[2] = |scale|, then size - 3 big-endian magnitude bytes */
static int
mk_numeric (unsigned char *buf, int nbytes, bool neg, int scale, unsigned int seed)
{
  int i;

  buf[0] = (unsigned char) ((nbytes + 3) | (neg ? 0x80 : 0));
  buf[1] = (unsigned char) (38 | (scale < 0 ? 0x80 : 0));
  buf[2] = (unsigned char) (scale < 0 ? -scale : scale);
  for (i = 0; i < nbytes; i++)
    {
      seed = seed * 1103515245u + 12345u;
      buf[3 + i] = (unsigned char) (seed >> 16);
    }
  return nbytes + 3;
}

TEST_CASE ("contract: the in-place NUMERIC decode reads every encoding as numeric_agg_expr_from_disk", "[contract]")
{
  unsigned char buf[32];
  long cases = 0, differ = 0;

  for (unsigned int seed = 1; seed <= 20000; seed++)
    {
      int nbytes = (int) (seed % 18);	/* 0..17 coefficient bytes: the fast path takes 0..8 */
      bool neg = (seed / 18) % 2 == 1;
      int scale = (int) ((seed / 36) % 40) - 2;
      int size = mk_numeric (buf, nbytes, neg, scale, seed);
      NUMERIC_AGG_EXPR_VAL fast, slow;
      bool okf, oks;

      memset (&fast, 0, sizeof (fast));
      memset (&slow, 0, sizeof (slow));
      okf = unitseam_eb_num_from_disk (buf, size, &fast);
      oks = numeric_agg_expr_from_disk (buf, size, &slow);
      cases++;
      if (okf != oks)
	{
	  differ++;
	  continue;
	}
      if (!okf)
	{
	  continue;
	}
      if (fast.coefficient != slow.coefficient || fast.scale != slow.scale
	  || (fast.is_negative && fast.coefficient != 0) != (slow.is_negative && slow.coefficient != 0))
	{
	  differ++;
	}
    }
  INFO ("cases " << cases);
  CHECK (differ == 0);
}

/* a long-format VARCHAR cell stored as it is (compressed length 0) */
static std::vector < unsigned char > long_plain (int n)
{
  std::vector < unsigned char > d (1 + 2 * OR_INT_SIZE + n);
  int net;

  d[0] = 0xFF;
  OR_PUT_INT (&net, 0);
  memcpy (&d[1], &net, OR_INT_SIZE);
  OR_PUT_INT (&net, n);
  memcpy (&d[1 + OR_INT_SIZE], &net, OR_INT_SIZE);
  for (int i = 0; i < n; i++)
    {
      d[1 + 2 * OR_INT_SIZE + i] = (unsigned char) ('a' + i % 26);
    }
  return d;
}

TEST_CASE ("contract: a long uncompressed value is read in place when the column has a scratch", "[contract]")
{
  std::vector < unsigned char > d = long_plain (300);
  std::vector < char > scratch (600);
  unsigned char set[1] = { 0 };
  const char *str = NULL;
  int len = 0;

  REQUIRE (unitseam_eb_cell_str_long (600, scratch.data (), set, 0, d.data (), (int) d.size (), &str, &len) == 1);
  CHECK (len == 300);
  CHECK (str == (const char *) d.data () + 1 + 2 * OR_INT_SIZE);
}

TEST_CASE ("A-11 defect: a long uncompressed value of a column without a scratch is refused", "[defect]")
{
  std::vector < unsigned char > d = long_plain (300);
  const char *str = NULL;
  int len = 0;

  /* max_bytes 0: an unbounded VARCHAR (eb_new_col_vec () sets no scratch past EB_STR_SCRATCH_MAX); nothing would be
   * decompressed, the value could be pointed at -- yet the cell is refused and the chunk goes to the row path */
  CHECK (unitseam_eb_cell_str_long (0, NULL, NULL, 0, d.data (), (int) d.size (), &str, &len) == -1);
}
