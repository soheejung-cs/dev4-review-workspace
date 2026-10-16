/*
 * PR#8013 -- the static inline helpers of src/query/bind_variant.h (header only). The target TU
 * src/query/xasl_cache.c (server) is linked so hooks.cpp can expose ITS compiled copies -- the ones
 * xcache_bind_variant runs; every case runs against both those and the test TU's own copy.
 *
 *   bind_fingerprint_distance (a, b, floor)   largest fold, either way; DBL_MAX when the term sets differ
 *   bind_fingerprint_known (fp)               at least one slot priced (>= 0)
 *   bind_variant_nearest (records, n, fp, floor, &dist)
 *
 * Callers (head 0dd6fa57a): db_vdb.c:550 known; db_vdb.c:457/572/622 and xasl_cache.c:2926 nearest,
 * with floor BIND_WATCH_ROW_FLOOR 1000 and band BIND_WATCH_BAND 1.5 (histogram_cl.hpp:161/169);
 * db_vdb.c:573 and xasl_cache.c:2927 MATCH on dist < band.
 * Slot values: bind_term_add (histogram_cl.cpp:3004) records MAX (1.0, sel * total_rows), or
 * BIND_TERM_UNKNOWN (-1) for a value the histogram cannot price; 0 can only arrive over the wire.
 */
#include "catch.hpp"
#include "bind_variant.h"
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <initializer_list>

/* hooks.cpp, compiled inside xasl_cache.c */
double unitseam_bv_tu_distance (const BIND_FINGERPRINT *a, const BIND_FINGERPRINT *b, double floor);
bool unitseam_bv_tu_known (const BIND_FINGERPRINT *fp);
int unitseam_bv_tu_nearest (const BIND_VARIANT_RECORD *records, int n_records, const BIND_FINGERPRINT *fp,
			    double floor, double *out_dist);

namespace
{
  const double FLOOR = 1000.0;	/* BIND_WATCH_ROW_FLOOR, histogram_cl.hpp:169 */
  const double BAND = 1.5;	/* BIND_WATCH_BAND, histogram_cl.hpp:161 */
  const double UNK = BIND_TERM_UNKNOWN;
  const double POISON = 7.0e9;	/* fills the slots past `terms` */

  double hdr_distance (const BIND_FINGERPRINT *a, const BIND_FINGERPRINT *b, double floor)
  {
    return bind_fingerprint_distance (a, b, floor);
  }
  bool hdr_known (const BIND_FINGERPRINT *fp)
  {
    return bind_fingerprint_known (fp);
  }
  int hdr_nearest (const BIND_VARIANT_RECORD *r, int n, const BIND_FINGERPRINT *fp, double floor, double *d)
  {
    return bind_variant_nearest (r, n, fp, floor, d);
  }

  struct impl
  {
    const char *name;
    double (*distance) (const BIND_FINGERPRINT *, const BIND_FINGERPRINT *, double);
    bool (*known) (const BIND_FINGERPRINT *);
    int (*nearest) (const BIND_VARIANT_RECORD *, int, const BIND_FINGERPRINT *, double, double *);
  };
  const impl IMPLS[] =
  {
    {"bind_variant.h as the test TU compiles it", hdr_distance, hdr_known, hdr_nearest},
    {"bind_variant.h as xasl_cache.c compiles it (hooks.cpp)", unitseam_bv_tu_distance, unitseam_bv_tu_known,
     unitseam_bv_tu_nearest},
  };

  BIND_FINGERPRINT fp (std::initializer_list<double> rows)
  {
    BIND_FINGERPRINT f;
    int i = 0;

    for (int k = 0; k < BIND_WATCH_MAX_TERMS; k++)
      {
	f.rows[k] = POISON;
      }
    for (double r : rows)
      {
	f.rows[i++] = r;
      }
    f.terms = i;
    return f;
  }

  BIND_VARIANT_RECORD rec (int variant, const BIND_FINGERPRINT &f)
  {
    BIND_VARIANT_RECORD r;
    r.variant = variant;
    r.fp = f;
    return r;
  }

  /* deterministic generator for the property cases */
  struct lcg
  {
    std::uint64_t s;
    unsigned next (unsigned n)
    {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return (unsigned) ((s >> 33) % n);
    }
  };
}

/* ------------------------------------------------------------- distance */

TEST_CASE ("distance: different term counts, or no terms at all, are DBL_MAX", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT one = fp ({5000}), two = fp ({5000, 5000}), none = fp ({});

      CHECK (m.distance (&one, &two, FLOOR) == DBL_MAX);
      CHECK (m.distance (&two, &one, FLOOR) == DBL_MAX);
      CHECK (m.distance (&none, &none, FLOOR) == DBL_MAX);	/* terms <= 0, even against itself */
    }
}

TEST_CASE ("distance: the largest fold among the slots, whichever way it moved", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT a = fp ({2000, 3000}), b = fp ({3000, 3000});
      BIND_FINGERPRINT up2_down3_a = fp ({2000, 9000}), up2_down3_b = fp ({4000, 3000});

      CHECK (m.distance (&a, &b, FLOOR) == 1.5);
      CHECK (m.distance (&b, &a, FLOOR) == 1.5);
      CHECK (m.distance (&up2_down3_a, &up2_down3_b, FLOOR) == 3.0);	/* 2x up, 3x down -> 3 */
      CHECK (m.distance (&a, &a, FLOOR) == 1.0);
    }
}

TEST_CASE ("distance: a slot below the floor on BOTH sides is skipped; at or above it on one side it is compared",
	   "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT lo = fp ({10}), lo2 = fp ({999.999}), at = fp ({1000}), lo_hi = fp ({10, 4000}),
	hi_hi = fp ({900, 2000});

      CHECK (m.distance (&lo, &lo2, FLOOR) == 1.0);	/* 10 -> 999.999: both below 1000 */
      CHECK (m.distance (&lo, &at, FLOOR) == 100.0);	/* 1000 is not below the floor */
      CHECK (m.distance (&lo_hi, &hi_hi, FLOOR) == 2.0);	/* slot 0 skipped, slot 1 4000 vs 2000 */
      CHECK (m.distance (&lo, &lo2, 1.0) == Approx (99.9999));	/* the floor is the caller's */
    }
}

TEST_CASE ("distance: zero rows on one side is DBL_MAX unless both sides are below the floor", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT zero = fp ({0.0}), big = fp ({5000}), small = fp ({500});

      CHECK (m.distance (&zero, &big, FLOOR) == DBL_MAX);
      CHECK (m.distance (&big, &zero, FLOOR) == DBL_MAX);
      CHECK (m.distance (&zero, &small, FLOOR) == 1.0);	/* floor skip comes first */
      CHECK (m.distance (&zero, &zero, 0.0) == DBL_MAX);	/* floor 0: 0 < 0 is false, so the zero check runs */
    }
}

TEST_CASE ("distance: a slot unknown (-1) on either side is skipped, the rest are compared", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT a = fp ({UNK, 5000}), b = fp ({300000, 5000}), c = fp ({300000, 10000}), d = fp ({UNK, 2000});

      CHECK (m.distance (&a, &b, FLOOR) == 1.0);
      CHECK (m.distance (&a, &c, FLOOR) == 2.0);
      CHECK (m.distance (&c, &a, FLOOR) == 2.0);
      CHECK (m.distance (&a, &d, FLOOR) == 2.5);	/* unknown on both sides too */
    }
}

TEST_CASE ("distance: every slot below the floor on both sides gives 1.0 (by design: nothing that moves a plan moved)",
	   "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT a = fp ({10, 200, 999}), b = fp ({900, 1, 30});

      CHECK (m.distance (&a, &b, FLOOR) == 1.0);
    }
}

TEST_CASE ("distance: slots past `terms` are never read", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT a = fp ({3000}), b = fp ({3000});

      a.rows[1] = 0.0;
      b.rows[1] = 1.0e12;
      b.rows[BIND_WATCH_MAX_TERMS - 1] = UNK;
      CHECK (m.distance (&a, &b, FLOOR) == 1.0);
    }
}

TEST_CASE ("distance: symmetric, d(a,b) == d(b,a), over generated fingerprints; d(a,a) == 1.0 for a known one",
	   "[contract]")
{
  const double palette[] = { UNK, 0.0, 1.0, 10.0, 500.0, 999.0, 1000.0, 1001.0, 1500.0, 2250.0, 3000.0, 50000.0,
    1.0e6, 2.5e7
  };
  const int np = (int) (sizeof (palette) / sizeof (palette[0]));
  const double floors[] = { FLOOR, 1.0, 0.0 };

  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      lcg g = { 20261016 };
      int asym = 0, nonid = 0, pairs = 0;

      for (int it = 0; it < 4000; it++)
	{
	  BIND_FINGERPRINT a = fp ({}), b = fp ({});
	  int n = 1 + (int) g.next (6);
	  bool a_has_zero = false;

	  a.terms = n;
	  b.terms = (g.next (10) == 0) ? 1 + (int) g.next (6) : n;	/* mostly the same term count */
	  for (int i = 0; i < n; i++)
	    {
	      a.rows[i] = palette[g.next (np)];
	      a_has_zero |= (a.rows[i] == 0.0);
	    }
	  for (int i = 0; i < b.terms; i++)
	    {
	      b.rows[i] = palette[g.next (np)];
	    }
	  for (double f : floors)
	    {
	      pairs++;
	      if (m.distance (&a, &b, f) != m.distance (&b, &a, f))
		{
		  asym++;
		}
	      if (m.known (&a) && !a_has_zero && f > 0.0 && m.distance (&a, &a, f) != 1.0)
		{
		  nonid++;
		}
	    }
	}
      INFO ("pairs " << pairs);
      CHECK (asym == 0);
      CHECK (nonid == 0);
    }
}

/* ---------------------------------------------------------------- known */

TEST_CASE ("known: false with no terms or every slot unknown; true once any slot is >= 0, zero included",
	   "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT none = fp ({}), unk = fp ({UNK, UNK}), one = fp ({UNK, 1.0}), zero = fp ({0.0});

      CHECK_FALSE (m.known (&none));
      CHECK_FALSE (m.known (&unk));
      CHECK (m.known (&one));
      CHECK (m.known (&zero));
    }
}

/* -------------------------------------------------------------- nearest */

TEST_CASE ("nearest: no records gives -1 and DBL_MAX; out_dist may be NULL", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT q = fp ({5000});
      BIND_VARIANT_RECORD r[1] = { rec (3, fp ({5000})) };
      double d = 0.0;

      CHECK (m.nearest (r, 0, &q, FLOOR, &d) == -1);
      CHECK (d == DBL_MAX);
      CHECK (m.nearest (r, 0, &q, FLOOR, NULL) == -1);
      CHECK (m.nearest (r, 1, &q, FLOOR, NULL) == 0);
    }
}

TEST_CASE ("nearest: the smallest distance wins; a tie keeps the earlier record", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT q = fp ({2000});
      BIND_VARIANT_RECORD later_wins[2] = { rec (0, fp ({1000})), rec (1, fp ({3000})) };	/* 2.0 vs 1.5 */
      BIND_VARIANT_RECORD tie[3] = { rec (5, fp ({1000})), rec (6, fp ({4000})), rec (7, fp ({1000})) };	/* 2.0 x3 */
      double d = 0.0;

      CHECK (m.nearest (later_wins, 2, &q, FLOOR, &d) == 1);
      CHECK (d == 1.5);
      CHECK (m.nearest (tie, 3, &q, FLOOR, &d) == 0);
      CHECK (d == 2.0);
    }
}

TEST_CASE ("PR8013 F25: nearest returns the first record even at DBL_MAX -- a caller that needs the band must "
	   "test out_dist itself (db_vdb.c:573, xasl_cache.c:2927 do)", "[contract]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      BIND_FINGERPRINT q = fp ({5000, 5000});
      BIND_VARIANT_RECORD r[3] = { rec (4, fp ({5000})), rec (5, fp ({5000, 5000, 5000})), rec (6, fp ({0.0, 5000})) };
      double d = 0.0;

      CHECK (m.nearest (r, 3, &q, FLOOR, &d) == 0);	/* every record is DBL_MAX away */
      CHECK (d == DBL_MAX);
      CHECK_FALSE (d < BAND);
    }
}

/* --------------------------------------------------------------- defect */

TEST_CASE ("PR8013 F9: with no slot priced on both sides the distance is still 1.0 -- inside the band, so the record "
	   "MATCHes and outranks one that really compares (PASS = defect)", "[defect]")
{
  for (const impl &m : IMPLS)
    {
      INFO (m.name);
      /* this execution could not price slot 0; the record was planned when slot 1 could not be priced */
      BIND_FINGERPRINT now = fp ({UNK, 50000});
      BIND_FINGERPRINT unrelated = fp ({300, UNK});
      BIND_FINGERPRINT comparable = fp ({UNK, 60000});	/* slot 1: 60000 vs 50000 = 1.2 */
      BIND_VARIANT_RECORD r[2] = { rec (1, comparable), rec (2, unrelated) };
      double d = 0.0;

      /* both pass the only screen in front of the comparison (db_vdb.c:550) */
      REQUIRE (m.known (&now));
      REQUIRE (m.known (&unrelated));

      CHECK (m.distance (&now, &unrelated, FLOOR) == 1.0);
      CHECK (m.distance (&now, &unrelated, FLOOR) < BAND);
      CHECK (m.nearest (r, 2, &now, FLOOR, &d) == 1);	/* the record with nothing in common wins */
      CHECK (d == 1.0);
    }
}
