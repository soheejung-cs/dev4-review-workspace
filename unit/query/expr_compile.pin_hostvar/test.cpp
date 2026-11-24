/*
 * CBRD-28165 (PR#8661) -- a scan-filter leaf pins a host variable (an auto-parameterized literal) of a narrower number
 * than its left side, converting the bound value once per execution (src/query/expr_compile.c, server mode).
 *
 *   P1 [contract] expr_pin_hostvar_safe (): the (left type, host variable type) pairs whose conversion cannot fail --
 *      DOUBLE <- SMALLINT INTEGER BIGINT FLOAT NUMERIC MONETARY, BIGINT <- SMALLINT INTEGER, INTEGER <- SMALLINT --
 *      and no other pair of the types listed.
 *   P2 [contract] the row path's first use of the leaf (expr_scan_side_value ()) converts the bound value with the
 *      term's decided converter into the target type, and the leaf compares that value; later uses in the same
 *      execution (query id) do not call the converter again, even when the bound array changed; another execution
 *      converts its own value.
 *   P3 [contract] a NULL bound value leaves the pinned value NULL without calling the converter (the leaf's prologue
 *      answers UNKNOWN), and the next execution's value converts.
 *   P4 [contract] without an execution identity (no xasl_state) every use converts again.
 *   P5 [contract] expr_scan_pred_prepare () -- what a page batch calls before it loads its constants -- converts every
 *      pinned host variable of the tree and leaves an unpinned leaf alone; a second call in the same execution does
 *      not convert again.
 *   P6 [contract] expr_scan_pred_reset () forgets the execution: the next use converts again under the same query id.
 *   P7 [contract] a conversion that fails (expr_pin_hostvar_safe () admits none) returns an error, sets it through
 *      tp_domain_status_er_set (), and leaves no value behind.
 *
 * Real code: expr_compile.c (unit under test, hooks.cpp appended -- the leaves and the value descriptor are built there),
 * db_macro.c, object_primitive.c, object_domain.c (the domains, DB_VALUE functions, pr_clear_value,
 * tp_domain_status_er_set). The converter is a fake that counts its calls (the decided comparison's converter is what
 * the leaf must use; which function the decision picks is domain_decide_comparison ()'s contract, not this one's).
 * Fakes: er_set / er_errid / er_clear (the last id), prm_Def (zeros), area_alloc / area_free (calloc / free).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "area_alloc.h"
#include "object_domain.h"

#include <cstdlib>
#include <cstring>

/* ---------------------------------------------------------------- hooks.cpp (appended to expr_compile.c) */

bool unitseam_pin_safe (DB_TYPE to, DB_TYPE from);
void *unitseam_hostvar_regu (int val_pos, DB_TYPE type);
void *unitseam_compare (TP_VALUE_CONVERTER conv, DB_TYPE target);
void *unitseam_leaf (void *rhs_regu, void *compare, DB_TYPE fast_type, bool pin);
void *unitseam_and (void *l, void *r);
void *unitseam_vd (const DB_VALUE * vals, int n, long long query_id);
void unitseam_vd_free (void *vd);
int unitseam_rhs_use (void *pred, void *vd);
const DB_VALUE *unitseam_rhs_value (void *pred);
int unitseam_prepare (void *root, void *vd);
void unitseam_reset (void *root);
void unitseam_free (void *root);

/* ---------------------------------------------------------------- fakes */

static int g_errid = NO_ERROR;
static int g_er_set_calls = 0;

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  g_errid = err_id;
  g_er_set_calls++;
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

SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };

void *
area_alloc (AREA *)
{
  return calloc (1, sizeof (TP_DOMAIN));
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

/* the decided comparison's converter: an INTEGER into DOUBLE, counting its calls */
static int g_conv_calls = 0;

static TP_DOMAIN_STATUS
conv_int_to_double (const DB_VALUE * src, DB_VALUE * dst, const TP_DOMAIN *, struct date_conversion_error *)
{
  g_conv_calls++;
  db_make_double (dst, (double) db_get_int (src));
  return DOMAIN_COMPATIBLE;
}

static TP_DOMAIN_STATUS
conv_fails (const DB_VALUE *, DB_VALUE *, const TP_DOMAIN *, struct date_conversion_error *)
{
  g_conv_calls++;
  return DOMAIN_OVERFLOW;
}

/* ---------------------------------------------------------------- helpers */

struct pinned_leaf
{
  void *regu, *compare, *leaf;

  explicit pinned_leaf (TP_VALUE_CONVERTER conv = conv_int_to_double)
  {
    regu = unitseam_hostvar_regu (0, DB_TYPE_INTEGER);
    compare = unitseam_compare (conv, DB_TYPE_DOUBLE);
    leaf = unitseam_leaf (regu, compare, DB_TYPE_DOUBLE, true);
  }
  ~pinned_leaf ()
  {
    unitseam_free (leaf);
    free (compare);
    free (regu);
  }
};

static double
rhs_double (void *leaf)
{
  const DB_VALUE *v = unitseam_rhs_value (leaf);
  REQUIRE (v != NULL);
  REQUIRE (!DB_IS_NULL (v));
  REQUIRE (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_DOUBLE);
  return db_get_double (v);
}

/* ---------------------------------------------------------------- [contract] */

TEST_CASE ("P1 the pairs a host variable is pinned for are exactly the conversions that cannot fail", "[contract]")
{
  const DB_TYPE types[] = { DB_TYPE_SHORT, DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_FLOAT, DB_TYPE_DOUBLE,
    DB_TYPE_NUMERIC, DB_TYPE_MONETARY, DB_TYPE_VARCHAR, DB_TYPE_CHAR, DB_TYPE_DATE, DB_TYPE_DATETIME
  };
  for (DB_TYPE to : types)
    {
      for (DB_TYPE from : types)
	{
	  bool want = (to == DB_TYPE_DOUBLE && (from == DB_TYPE_SHORT || from == DB_TYPE_INTEGER
						|| from == DB_TYPE_BIGINT || from == DB_TYPE_FLOAT
						|| from == DB_TYPE_NUMERIC || from == DB_TYPE_MONETARY))
	    || (to == DB_TYPE_BIGINT && (from == DB_TYPE_SHORT || from == DB_TYPE_INTEGER))
	    || (to == DB_TYPE_INTEGER && from == DB_TYPE_SHORT);
	  INFO ("to " << (int) to << " from " << (int) from);
	  CHECK (unitseam_pin_safe (to, from) == want);
	}
    }
}

TEST_CASE ("P2 the first use in an execution converts once, the next execution converts its own value", "[contract]")
{
  pinned_leaf p;
  DB_VALUE vals[1];

  db_make_int (&vals[0], 24);
  void *vd1 = unitseam_vd (vals, 1, 7);
  g_conv_calls = 0;
  REQUIRE (unitseam_rhs_use (p.leaf, vd1) == NO_ERROR);
  CHECK (g_conv_calls == 1);
  CHECK (rhs_double (p.leaf) == 24.0);

  /* the same execution: the value stays, even if the array changed under it */
  db_make_int (&vals[0], 10);
  REQUIRE (unitseam_rhs_use (p.leaf, vd1) == NO_ERROR);
  REQUIRE (unitseam_rhs_use (p.leaf, vd1) == NO_ERROR);
  CHECK (g_conv_calls == 1);
  CHECK (rhs_double (p.leaf) == 24.0);

  /* another execution binds 10 */
  void *vd2 = unitseam_vd (vals, 1, 8);
  REQUIRE (unitseam_rhs_use (p.leaf, vd2) == NO_ERROR);
  CHECK (g_conv_calls == 2);
  CHECK (rhs_double (p.leaf) == 10.0);
  unitseam_vd_free (vd1);
  unitseam_vd_free (vd2);
}

TEST_CASE ("P3 a NULL bound value stays NULL without a conversion, the next value converts", "[contract]")
{
  pinned_leaf p;
  DB_VALUE vals[1];

  db_make_null (&vals[0]);
  void *vd1 = unitseam_vd (vals, 1, 11);
  g_conv_calls = 0;
  REQUIRE (unitseam_rhs_use (p.leaf, vd1) == NO_ERROR);
  CHECK (g_conv_calls == 0);
  CHECK (DB_IS_NULL (unitseam_rhs_value (p.leaf)));

  db_make_int (&vals[0], 3);
  void *vd2 = unitseam_vd (vals, 1, 12);
  REQUIRE (unitseam_rhs_use (p.leaf, vd2) == NO_ERROR);
  CHECK (g_conv_calls == 1);
  CHECK (rhs_double (p.leaf) == 3.0);
  unitseam_vd_free (vd1);
  unitseam_vd_free (vd2);
}

TEST_CASE ("P4 without an execution identity every use converts", "[contract]")
{
  pinned_leaf p;
  DB_VALUE vals[1];

  db_make_int (&vals[0], 5);
  void *vd = unitseam_vd (vals, 1, 0);
  g_conv_calls = 0;
  REQUIRE (unitseam_rhs_use (p.leaf, vd) == NO_ERROR);
  db_make_int (&vals[0], 6);
  REQUIRE (unitseam_rhs_use (p.leaf, vd) == NO_ERROR);
  CHECK (g_conv_calls == 2);
  CHECK (rhs_double (p.leaf) == 6.0);
  unitseam_vd_free (vd);
}

TEST_CASE ("P5 prepare converts every pinned host variable of the tree once per execution", "[contract]")
{
  DB_VALUE vals[2];
  void *r0 = unitseam_hostvar_regu (0, DB_TYPE_INTEGER);
  void *r1 = unitseam_hostvar_regu (1, DB_TYPE_DOUBLE);
  void *cmp = unitseam_compare (conv_int_to_double, DB_TYPE_DOUBLE);
  void *pinned = unitseam_leaf (r0, cmp, DB_TYPE_DOUBLE, true);
  void *plain = unitseam_leaf (r1, NULL, DB_TYPE_DOUBLE, false);
  void *root = unitseam_and (plain, pinned);

  db_make_int (&vals[0], 40);
  db_make_double (&vals[1], 1.5);
  void *vd = unitseam_vd (vals, 2, 21);
  g_conv_calls = 0;
  REQUIRE (unitseam_prepare (root, vd) == NO_ERROR);
  CHECK (g_conv_calls == 1);
  CHECK (rhs_double (pinned) == 40.0);
  /* the plain leaf's holder is not touched: the row fetches it */
  CHECK (unitseam_rhs_value (plain) == NULL);
  REQUIRE (unitseam_prepare (root, vd) == NO_ERROR);
  CHECK (g_conv_calls == 1);
  unitseam_vd_free (vd);
  unitseam_free (root);
  free (cmp);
  free (r0);
  free (r1);
}

TEST_CASE ("P6 reset forgets the execution", "[contract]")
{
  pinned_leaf p;
  DB_VALUE vals[1];

  db_make_int (&vals[0], 9);
  void *vd = unitseam_vd (vals, 1, 31);
  g_conv_calls = 0;
  REQUIRE (unitseam_rhs_use (p.leaf, vd) == NO_ERROR);
  unitseam_reset (p.leaf);
  db_make_int (&vals[0], 4);
  REQUIRE (unitseam_rhs_use (p.leaf, vd) == NO_ERROR);
  CHECK (g_conv_calls == 2);
  CHECK (rhs_double (p.leaf) == 4.0);
  unitseam_vd_free (vd);
}

TEST_CASE ("P7 a failed conversion is an error and leaves no value", "[contract]")
{
  pinned_leaf p (conv_fails);
  DB_VALUE vals[1];

  db_make_int (&vals[0], 1);
  void *vd = unitseam_vd (vals, 1, 41);
  g_conv_calls = 0;
  g_er_set_calls = 0;
  er_clear ();
  CHECK (unitseam_rhs_use (p.leaf, vd) != NO_ERROR);
  CHECK (g_conv_calls == 1);
  CHECK (g_er_set_calls == 1);
  CHECK (er_errid () != NO_ERROR);
  CHECK (DB_IS_NULL (unitseam_rhs_value (p.leaf)));
  unitseam_vd_free (vd);
}

/* PR#8661 review (beyondykk9, expr_compile.c:3350): the build pins a host variable only when the term's decided
 * comparison converts the right side, but the leaf reads the comparison of the term the server unpacked; if the two
 * disagree, the pin used to read through a NULL compare (or converter) and crash in release.  It is an error now. */
TEST_CASE ("P8 a pinned leaf whose comparison has no converter is an error and leaves no value", "[contract]")
{
  void *regu = unitseam_hostvar_regu (0, DB_TYPE_INTEGER);
  void *no_conv = unitseam_compare (NULL, DB_TYPE_DOUBLE);
  void *const compares[2] = { NULL, no_conv };
  DB_VALUE vals[1];

  db_make_int (&vals[0], 1);
  for (int i = 0; i < 2; i++)
    {
      void *leaf = unitseam_leaf (regu, compares[i], DB_TYPE_DOUBLE, true);
      void *vd = unitseam_vd (vals, 1, 51 + i);

      g_conv_calls = 0;
      g_er_set_calls = 0;
      er_clear ();
      CHECK (unitseam_rhs_use (leaf, vd) != NO_ERROR);
      CHECK (g_conv_calls == 0);
      CHECK (g_er_set_calls == 1);
      CHECK (er_errid () != NO_ERROR);
      CHECK (DB_IS_NULL (unitseam_rhs_value (leaf)));
      unitseam_vd_free (vd);
      unitseam_free (leaf);
    }
  free (no_conv);
  free (regu);
}
