/*
 * PR#8196 (CBRD-27361) group E -- the per-element scale of a NUMERIC step result (src/query/expr_batch.c, server
 * mode), TU-static, reached through the appendix:
 *
 *   eb_run_steps ()  a NUMERIC add/sub/mul runs numeric_agg_expr_add/_mul () (numeric_opfunc.c) per row and stores the
 *                    result with eb_num_put (): the coefficient in an int128 element, the scale in the vector's scale
 *                    byte for that row -- an unsigned char (EB_VEC.scales, expr_batch.c:140; eb_num_put () :545)
 *   eb_num_get ()    reads the element back for the SUM loop and the next step: scale = scales[r]
 *   eb_pack_dbv ()   packs it as the DB_VALUE the aggregate's first value, MIN/MAX and the projection take
 *
 * The reference is the word-domain operation itself (numeric_agg_expr_mul/_add (), the real TU) -- what the row path's
 * float_numeric_db_value_mul/_add () give (numeric_opfunc.h:216) -- and numeric_agg_expr_to_dbv () of its result.
 * Since CBRD-26006 a fixed NUMERIC column takes a scale in [-84, 127] (dbtype_def.h:585-586) and the word domain
 * keeps a negative scale (numeric_agg_expr_mul () checks it against DB_MIN_NUMERIC_SCALE, -214).
 *
 *   [contract]  column scales 0, 2, 5 and constant scales 0, 1, 3, add and mul: every row's element reads back as the
 *               operation's result (coefficient, scale, sign) and packs to a value equal to it
 *   [defect]    E-02: a column of a negative scale (NUMERIC(10, -2)): the step's result scale -2 is stored in the
 *               unsigned scale byte as 254, so the element reads back as coefficient * 10^-254 and packs so
 *               (PASS = the defect is there)
 *
 * Faked: nothing on these paths.
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "numeric_opfunc.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern int unitseam_eb_num_step (int op, const int64_t * col, int n, int col_scale, __int128 k, int k_scale,
				 NUMERIC_AGG_EXPR_VAL * got, DB_VALUE * packed);
extern int unitseam_eb_op_add (void);
extern int unitseam_eb_op_mul (void);

/* er_set (): counted, not raised -- db_make_numeric () sets an error for a scale out of range (db_macro.c) */
static int er_set_calls = 0;
static int er_set_last = 0;

void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  er_set_calls++;
  er_set_last = err_id;
}

static NUMERIC_AGG_EXPR_VAL
nv_of (__int128 c, int scale)
{
  NUMERIC_AGG_EXPR_VAL v;

  v.coefficient = (uint128_t) (c < 0 ? -c : c);
  v.scale = scale;
  v.is_negative = (c < 0);
  return v;
}

/* rows whose read-back or packed value differs from the operation's result */
static int
differing (int op, int col_scale, int k_scale, std::string * first)
{
  std::vector<int64_t> col = { 0, 1, -1, 7, 12345, -12345, 999999999999LL, -999999999999LL };
  __int128 k = 25;
  std::vector<NUMERIC_AGG_EXPR_VAL> got (col.size ());
  std::vector<DB_VALUE> packed (col.size ());
  int i, diff = 0;

  REQUIRE (unitseam_eb_num_step (op, col.data (), (int) col.size (), col_scale, k, k_scale, got.data (),
				 packed.data ()) == NO_ERROR);
  for (i = 0; i < (int) col.size (); i++)
    {
      NUMERIC_AGG_EXPR_VAL x = nv_of (col[i], col_scale), y = nv_of (k, k_scale), z;
      DB_VALUE want, ans;
      bool ok = (op == unitseam_eb_op_mul ())? numeric_agg_expr_mul (&x, &y, &z) : numeric_agg_expr_add (&x, &y, false,
													 &z);
      bool same;

      REQUIRE (ok);
      numeric_agg_expr_to_dbv (&z, &want);
      /* a packed value the comparison refuses (its domain did not take the scale) differs too */
      same = (got[i].coefficient == z.coefficient && got[i].scale == z.scale && got[i].is_negative == z.is_negative
	      && numeric_db_value_compare (&packed[i], &want, &ans) == NO_ERROR && db_get_int (&ans) == 0);
      if (!same)
	{
	  if (diff == 0 && first != NULL)
	    {
	      *first = std::string ((op == unitseam_eb_op_mul ())? "mul" : "add") + " row " + std::to_string (i)
		+ ": result scale " + std::to_string (z.scale) + ", read back scale " + std::to_string (got[i].scale);
	    }
	  diff++;
	}
    }
  return diff;
}

TEST_CASE ("contract: a NUMERIC step's element reads back and packs as the operation's result", "[contract]")
{
  int col_scales[] = { 0, 2, 5 };
  int k_scales[] = { 0, 1, 3 };
  int checked = 0;

  for (int cs : col_scales)
    {
      for (int ks : k_scales)
	{
	  for (int op : { unitseam_eb_op_add (), unitseam_eb_op_mul ()})
	    {
	      std::string first;

	      INFO (first);
	      CHECK (differing (op, cs, ks, &first) == 0);
	      checked++;
	    }
	}
    }
  CHECK (checked == 18);
  CHECK (er_set_calls == 0);
}

TEST_CASE ("E-02 defect: a negative result scale is stored in the unsigned scale byte and reads back as 256 + scale",
	   "[defect]")
{
  std::string f1, f2;
  int calls0 = er_set_calls;
  int d_mul = differing (unitseam_eb_op_mul (), -2, 0, &f1);	/* NUMERIC(10, -2) * 25 : scale -2 */
  int d_add = differing (unitseam_eb_op_add (), -2, -3, &f2);	/* both negative: scale -2 */

  INFO ("mul: " << f1 << "; add: " << f2 << "; er_set while packing: " << (er_set_calls - calls0) << " (last "
	<< er_set_last << ")");
  CHECK (d_mul > 0);
  CHECK (d_add > 0);
}
