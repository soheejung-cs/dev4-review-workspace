/*
 * PR#8625 (CBRD-28066) -- tp_domain_coll_flags_match (dom1, dom2)
 *
 * Unit under test: src/object/object_domain.c, the static helper the set domain cache lookups use
 * (tp_domain_find_set () for or_unpack_domain (), tp_is_domain_cached () for tp_domain_cache ()): two domains a
 * tp_domain_match () found alike match only when they and their element domains, nested included, carry the
 * same collation flags. A cached collection domain must not hand a plan's ENFORCE/LEAVE elements to another
 * plan's NORMAL ones (the plan load check refuses them, CBRD-27817) or the other way round.
 *
 * The domains are built by hand (calloc) over the real PR_TYPE objects of object_primitive.c; nothing is cached,
 * so no tp_init () is needed. hooks.cpp exposes the static helper.
 */
#include "catch.hpp"
#include "config.h"

#include <cstdlib>
#include <vector>

#include "object_domain.h"
#include "object_primitive.h"

bool unit_tp_domain_coll_flags_match (const TP_DOMAIN * dom1, const TP_DOMAIN * dom2);

namespace
{
  std::vector < TP_DOMAIN * >made;

  TP_DOMAIN *mk (const PR_TYPE * type, TP_DOMAIN_COLL_ACTION flag = TP_DOMAIN_COLL_NORMAL, TP_DOMAIN * elems = NULL)
  {
    TP_DOMAIN *d = (TP_DOMAIN *) calloc (1, sizeof (TP_DOMAIN));
    d->type = type;
    d->collation_flag = flag;
    d->setdomain = elems;
    made.push_back (d);
    return d;
  }

  /* a list of element domains, in order */
  TP_DOMAIN *lst (std::initializer_list < TP_DOMAIN * >ds)
  {
    TP_DOMAIN *first = NULL, *last = NULL;
    for (TP_DOMAIN * d:ds)
      {
	if (last == NULL)
	  {
	    first = d;
	  }
	else
	  {
	    last->next = d;
	  }
	last = d;
      }
    return first;
  }

  TP_DOMAIN *vc (TP_DOMAIN_COLL_ACTION f = TP_DOMAIN_COLL_NORMAL)
  {
    return mk (tp_Type_string, f);
  }
  TP_DOMAIN *in ()
  {
    return mk (tp_Type_integer);
  }
  TP_DOMAIN *set (const PR_TYPE * t, TP_DOMAIN * elems)
  {
    return mk (t, TP_DOMAIN_COLL_NORMAL, elems);
  }

  struct cleanup
  {
    ~cleanup ()
    {
      for (TP_DOMAIN * d:made)
	{
	  free (d);
	}
      made.clear ();
    }
  };
}

TEST_CASE ("C1 the same domain and alike domains of the same flags match, nested included", "[contract]")
{
  cleanup c;
  TP_DOMAIN *a = set (tp_Type_sequence, lst ({set (tp_Type_sequence, lst ({vc (), in ()})), vc ()}));
  TP_DOMAIN *b = set (tp_Type_sequence, lst ({set (tp_Type_sequence, lst ({vc (), in ()})), vc ()}));
  CHECK (unit_tp_domain_coll_flags_match (a, a));
  CHECK (unit_tp_domain_coll_flags_match (a, b));
  CHECK (unit_tp_domain_coll_flags_match (vc (TP_DOMAIN_COLL_ENFORCE), vc (TP_DOMAIN_COLL_ENFORCE)));
}

TEST_CASE ("C2 a string element of another flag does not match: ENFORCE, LEAVE against NORMAL, both ways", "[contract]")
{
  cleanup c;
  for (TP_DOMAIN_COLL_ACTION f:{TP_DOMAIN_COLL_ENFORCE, TP_DOMAIN_COLL_LEAVE})
    {
      TP_DOMAIN *undecided = set (tp_Type_multiset, lst ({in (), vc (f)}));
      TP_DOMAIN *decided = set (tp_Type_multiset, lst ({in (), vc ()}));
      CHECK_FALSE (unit_tp_domain_coll_flags_match (undecided, decided));
      CHECK_FALSE (unit_tp_domain_coll_flags_match (decided, undecided));
    }
}

TEST_CASE ("C3 a nested element of another flag does not match (a row value IN list)", "[contract]")
{
  cleanup c;
  TP_DOMAIN *a = set (tp_Type_sequence, lst ({set (tp_Type_sequence, lst ({vc (TP_DOMAIN_COLL_ENFORCE), in ()}))}));
  TP_DOMAIN *b = set (tp_Type_sequence, lst ({set (tp_Type_sequence, lst ({vc (), in ()}))}));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (a, b));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (b, a));
}

TEST_CASE ("C4 element lists of another length do not match; NULL matches only NULL", "[contract]")
{
  cleanup c;
  CHECK_FALSE (unit_tp_domain_coll_flags_match (set (tp_Type_set, lst ({vc (), in ()})), set (tp_Type_set, lst ({vc ()}))));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (set (tp_Type_set, lst ({vc ()})), set (tp_Type_set, lst ({vc (), in ()}))));
  CHECK (unit_tp_domain_coll_flags_match (NULL, NULL));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (vc (), NULL));
}

TEST_CASE ("C5 a type without collation does not compare its flag", "[contract]")
{
  cleanup c;
  CHECK (unit_tp_domain_coll_flags_match (mk (tp_Type_integer, TP_DOMAIN_COLL_ENFORCE), in ()));
}

TEST_CASE ("C6 elements of an unordered collection alike in another order never match a domain whose flags differ",
	   "[contract]")
{
  cleanup c;
  /* inner MULTISET {VARCHAR ENFORCE, INTEGER} against a cached {INTEGER, VARCHAR NORMAL}: tp_domain_match ()
   * matches unordered elements, this helper pairs them in list order */
  TP_DOMAIN *undecided = set (tp_Type_sequence, lst ({set (tp_Type_multiset, lst ({vc (TP_DOMAIN_COLL_ENFORCE), in ()}))}));
  TP_DOMAIN *decided = set (tp_Type_sequence, lst ({set (tp_Type_multiset, lst ({in (), vc ()}))}));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (decided, undecided));
  CHECK_FALSE (unit_tp_domain_coll_flags_match (undecided, decided));
}

TEST_CASE ("PR8625 U1: a pair of elements of another type compared only the first's flag - INTEGER (NORMAL) against "
	   "VARCHAR ENFORCE skipped, VARCHAR NORMAL against INTEGER passed: an undecided domain matched a decided one "
	   "in another order (PASS = defect present)", "[defect]")
{
  cleanup c;
  TP_DOMAIN *undecided = set (tp_Type_sequence, lst ({set (tp_Type_multiset, lst ({vc (TP_DOMAIN_COLL_ENFORCE), in ()}))}));
  TP_DOMAIN *decided = set (tp_Type_sequence, lst ({set (tp_Type_multiset, lst ({in (), vc ()}))}));
  CHECK (unit_tp_domain_coll_flags_match (decided, undecided));
}
