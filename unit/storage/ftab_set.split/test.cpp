/* PR#8497 (CBRD-28043) 3328953d3 -- ftab_set::split (), src/storage/ftab_set.hpp: how the page bitmap of a heap is cut
 * among the workers of a parallel heap scan (px_scan_input_handler_heap.cpp init_on_main ()), the statistics sampler
 * and the parallel index build.
 *
 * Real code: ftab_set (header only: convert (), split (), get_next (), size ()) and bit64_count_ones () (bit.c, --extra).
 * No fake.  The TU is ftab_set.cpp (its includes only).
 *
 * develop (99279799f) cut equal numbers of sectors (the first size % n sets one more); 3328953d3 cuts contiguous runs
 * of about the same number of pages: set i ends at the first sector where the pages so far reach
 * floor (total * (i + 1) / n), the last set takes the rest.  The cases marked "(PR#8497 TO-BE)" assert the page
 * balance and fail on develop by design; the others hold on both.
 */
#include "catch.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "bit.h"
#include "ftab_set.hpp"

namespace fs
{
  /* a sector per entry; pages[i] pages set in sector i (the low bits, or spread when spread is true) */
  std::vector<FILE_PARTIAL_SECTOR>
  sectors (const std::vector<int> &pages, bool spread = false)
  {
    std::vector<FILE_PARTIAL_SECTOR> v;
    for (size_t i = 0; i < pages.size (); i++)
      {
	FILE_PARTIAL_SECTOR s;
	s.vsid.volid = 0;
	s.vsid.sectid = 10 + (int) i;
	s.page_bitmap = 0;
	for (int b = 0; b < pages[i]; b++)
	  {
	    int bit = spread ? (int) ((b * 37) % 64) : b;
	    while (s.page_bitmap & (((UINT64) 1) << bit))
	      {
		bit = (bit + 1) % 64;
	      }
	    s.page_bitmap |= ((UINT64) 1) << bit;
	  }
	v.push_back (s);
      }
    return v;
  }

  ftab_set
  of (std::vector<FILE_PARTIAL_SECTOR> &v)
  {
    FILE_FTAB_COLLECTOR c;
    c.npages = 0;
    c.nsects = (int) v.size ();
    c.partsect_ftab = v.data ();
    ftab_set s;
    s.convert (&c);
    return s;
  }

  /* the sector ids of one set, by get_next () until the null VSID */
  std::vector<int>
  ids (ftab_set &s)
  {
    std::vector<int> r;
    for (;;)
      {
	FILE_PARTIAL_SECTOR p = s.get_next ();
	if (VSID_IS_NULL (&p.vsid))
	  {
	    break;
	  }
	r.push_back (p.vsid.sectid);
	if (r.size () > 100000)
	  {
	    break;
	  }
      }
    return r;
  }

  struct cut
  {
    std::vector<std::vector<int>> sets;	/* sector ids */
    std::vector<int> pages;		/* pages per set */
  };

  cut
  split (std::vector<int> pages, int n)
  {
    std::vector<FILE_PARTIAL_SECTOR> v = sectors (pages);
    ftab_set s = of (v);
    std::vector<ftab_set> parts = s.split (n);
    cut c;
    for (ftab_set &p : parts)
      {
	std::vector<int> i = ids (p);
	int np = 0;
	for (int id : i)
	  {
	    np += pages[id - 10];
	  }
	c.sets.push_back (i);
	c.pages.push_back (np);
      }
    return c;
  }

  std::string
  str (const cut &c)
  {
    std::string s;
    char b[32];
    for (size_t i = 0; i < c.sets.size (); i++)
      {
	s += "[";
	for (int id : c.sets[i])
	  {
	    snprintf (b, sizeof (b), " %d", id - 10);
	    s += b;
	  }
	snprintf (b, sizeof (b), " ]=%d ", c.pages[i]);
	s += b;
      }
    return s;
  }

  /* every sector once, in order, in contiguous runs */
  bool
  partition_ok (const cut &c, size_t nsect)
  {
    int next = 10;
    for (const std::vector<int> &set : c.sets)
      {
	for (int id : set)
	  {
	    if (id != next++)
	      {
		return false;
	      }
	  }
      }
    return (size_t) (next - 10) == nsect;
  }

  /* 3328953d3's rule: for k in 1..n-1 the first k sets hold g_k = floor (total * k / n) pages or more, and fewer than
   * g_k + 64 (the last sector a set takes was needed to reach g_k, and a sector has 64 pages at most) */
  bool
  balanced (const cut &c, int total)
  {
    int n = (int) c.sets.size ();
    long long done = 0;
    for (int k = 1; k < n; k++)
      {
	done += c.pages[k - 1];
	long long g = (long long) total * k / n;
	if (done < g || done >= g + 64)
	  {
	    return false;
	  }
      }
    return true;
  }
}

using namespace fs;

TEST_CASE ("S1 n_sets 0 or below: no set at all", "[contract]")
{
  std::vector<FILE_PARTIAL_SECTOR> v = sectors ({ 3, 5, 7 });
  ftab_set s = of (v);
  CHECK (s.split (0).empty ());
  CHECK (s.split (-1).empty ());
  CHECK (s.size () == 3);
}

TEST_CASE ("S2 n sets, every sector exactly once, in file order, as contiguous runs (fewer sectors than sets: some sets "
	   "empty)", "[contract]")
{
  for (int n = 1; n <= 9; n++)
    {
      for (std::vector<int> pages : std::vector<std::vector<int>> ({ { 64 }, { 1, 2, 3, 4, 5 }, { 0, 0, 64, 64 },
		{ 64, 64, 64, 0, 0, 0 }, { 0, 0, 0 }, { 5, 0, 5, 0, 5, 0, 5, 0, 5, 0, 5, 0 } }))
	{
	  cut c = split (pages, n);
	  CAPTURE (n, str (c));
	  CHECK ((int) c.sets.size () == n);
	  CHECK (partition_ok (c, pages.size ()));
	}
    }
}

TEST_CASE ("S3 get_next () gives a set's sectors in order, then the null VSID, and stays there", "[contract]")
{
  std::vector<FILE_PARTIAL_SECTOR> v = sectors ({ 4, 4 });
  ftab_set s = of (v);
  std::vector<ftab_set> parts = s.split (1);
  REQUIRE (parts.size () == 1);
  CHECK (parts[0].get_next ().vsid.sectid == 10);
  CHECK (parts[0].get_next ().vsid.sectid == 11);
  FILE_PARTIAL_SECTOR end1 = parts[0].get_next ();
  FILE_PARTIAL_SECTOR end2 = parts[0].get_next ();
  CHECK (VSID_IS_NULL (&end1.vsid));
  CHECK (VSID_IS_NULL (&end2.vsid));
}

TEST_CASE ("S4 (PR#8497 TO-BE) a heap whose last sectors lost their pages: three full sectors then three empty ones in "
	   "two sets are cut by pages -- [0 1] 128 pages and [2 3 4 5] 64, not 192 and 0 (develop: three sectors each)",
	   "[contract]")
{
  cut c = split ({ 64, 64, 64, 0, 0, 0 }, 2);
  CAPTURE (str (c));
  CHECK (c.sets == std::vector<std::vector<int>> ({ { 10, 11 }, { 12, 13, 14, 15 } }));
  CHECK (c.pages == std::vector<int> ({ 128, 64 }));
}

TEST_CASE ("S5 (PR#8497 TO-BE) empty sectors go with the set the next page falls in: two empty sectors before two full "
	   "ones in two sets -- [0 1 2] and [3]", "[contract]")
{
  cut c = split ({ 0, 0, 64, 64 }, 2);
  CAPTURE (str (c));
  CHECK (c.sets == std::vector<std::vector<int>> ({ { 10, 11, 12 }, { 13 } }));
}

TEST_CASE ("S6 (PR#8497 TO-BE, current behavior at the edges) fewer pages than sets: 1 + 1 pages in four sets -- "
	   "[] [0] [] [1]; no page at all -- every sector in the last set", "[contract]")
{
  cut c = split ({ 1, 1 }, 4);
  CAPTURE (str (c));
  CHECK (c.pages == std::vector<int> ({ 0, 1, 0, 1 }));
  CHECK (c.sets[1] == std::vector<int> ({ 10 }));
  CHECK (c.sets[3] == std::vector<int> ({ 11 }));

  cut z = split ({ 0, 0, 0 }, 3);
  CAPTURE (str (z));
  CHECK (z.sets == std::vector<std::vector<int>> ({ {}, {}, { 10, 11, 12 } }));
}

TEST_CASE ("S7 (PR#8497 TO-BE) 3,000 random bitmaps (0-64 pages a sector, runs of empty sectors, 1-40 sectors) in 1-9 "
	   "sets: a partition in file order, and the first k sets hold between floor (total * k / n) and that + 63 pages",
	   "[contract]")
{
  std::mt19937 rng (8497);
  int bad_partition = 0, unbalanced = 0, cases = 0;
  std::string first_bad;

  for (int t = 0; t < 3000; t++)
    {
      int nsect = 1 + (int) (rng () % 40);
      std::vector<int> pages;
      int total = 0;
      for (int i = 0; i < nsect; i++)
	{
	  int kind = (int) (rng () % 4);
	  int p = kind == 0 ? 0 : kind == 1 ? 64 : (int) (rng () % 65);
	  pages.push_back (p);
	  total += p;
	}
      int n = 1 + (int) (rng () % 9);
      cut c = split (pages, n);
      cases++;
      if ((int) c.sets.size () != n || !partition_ok (c, pages.size ()))
	{
	  bad_partition++;
	}
      if (!balanced (c, total))
	{
	  if (unbalanced++ == 0)
	    {
	      first_bad = "n=" + std::to_string (n) + " " + str (c);
	    }
	}
    }
  CAPTURE (cases, first_bad);
  CHECK (bad_partition == 0);
  CHECK (unbalanced == 0);
}
