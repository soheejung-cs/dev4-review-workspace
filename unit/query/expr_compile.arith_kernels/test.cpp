/*
 * PR#7658 (CBRD-27215) -- compiled expression programs (src/query/expr_compile.c, server mode) against the
 * interpreter they replace, in one binary.
 *
 * Every case builds a regu tree by hand, evaluates it with the interpreter (fetch_peek_dbval -> fetch_peek_arith ->
 * the opcode body in fetch_arith_ops.c -> qdata_arith_dbval) and with the program compiled from the same tree
 * (expr_prog_compile_roots + expr_prog_eval), and compares the value (type, payload, a NUMERIC's precision/scale and
 * coefficient bytes) or the error (er_errid), and the subqueries executed, in order.  The interpreter is develop's:
 * query_opfunc.c's number operators differ from the base 4adcd4090 only in comments, fetch.c/fetch_arith_ops.c are
 * the PR's mechanical move of the operator bodies (the base has no expr_compile.c: every case is new code).
 *
 *   kernels     the arithmetic kernel table (+,-,*,/ x INTEGER/BIGINT/DOUBLE/NUMERIC, the NUMERIC float/plain
 *               families) over boundary grids: overflow per operator, zero divide, INT_MIN / -1, NULL operands,
 *               the 38/40-digit edge, the epilogue coercion into the node's NUMERIC(p,s), a wider/narrower number
 *               domain, a string domain (declined to the generic step), a type outside the table, a drifted value
 *               type (row to the interpreter), float_numeric_db_value_mul_fixed64 bit-identity
 *   lazy        a right operand that can fail (a subquery) is skipped for a NULL left (arithmetic, NUMERIC mix with its
 *               coercion inside the region, NVL's jump_notnull)
 *   lists       several roots: hoisted literal steps move ahead of earlier columns; jumps, regions, first error stay
 *   executions  the gate's constant (plan_item->ref, review 2026-10-15 #1) and the open step's refill across
 *               executions, previous value blocks poisoned
 *   fuzz        random lists over shared columns (TYPE_CONSTANT: one cell per column -> CSE, commuted CSE), subqueries,
 *               literals, NVL, CASE / predicate-as-value; then over NUMERIC and INTEGER together
 *   F2          [defect, latent] a failed prologue is marked done and skipped on the next evaluation
 *
 * Real code linked in (suite.json extra): numeric_opfunc.c, query_opfunc.c, object_domain.c,
 * object_domain_convert.cpp, fetch.c, fetch_arith_ops.c, object_primitive.c, db_macro.c, query_evaluator.c.
 * Faked (see suite.json fakes): er_set/er_errid/er_clear, prm_Def, the thread entry, heap_attrinfo_access,
 * qexec_execute_mainblock (scalar subquery leaves), area_alloc/area_free (domain area), lang_Collations.
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "numeric_opfunc.h"
#include "query_opfunc.h"
#include "regu_var.hpp"
#include "xasl.h"
#include "domain_plan.h"
#include "heap_file.h"
#include "fetch.h"
#include "fetch_arith_ops.h"
#include "expr_compile.h"
#include "thread_entry.hpp"
#include "area_alloc.h"

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

/* ================================================================ fakes */

static int g_errid = NO_ERROR;
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  g_errid = err_id;
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

static void
prm_int (PARAM_ID id, int v)
{
  prm_Def[id].datatype = PRM_INTEGER;
  prm_Def[id].value.v.i = v;
}

static void
prm_bool (PARAM_ID id, bool v)
{
  prm_Def[id].datatype = PRM_BOOLEAN;
  prm_Def[id].value.v.b = v;
}

static bool
boot_params ()
{
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 400);
  prm_Def[PRM_ID_COMPAT_MODE].datatype = PRM_KEYWORD;
  prm_Def[PRM_ID_COMPAT_MODE].value.v.i = COMPAT_CUBRID;
  prm_bool (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  prm_bool (PRM_ID_RETURN_NULL_ON_FUNCTION_ERRORS, false);
  prm_bool (PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR, false);
  prm_bool (PRM_ID_ALLOW_TRUNCATED_STRING, false);
  prm_bool (PRM_ID_IGNORE_TRAILING_SPACE, false);
  return true;
}
static const bool params_booted = boot_params ();

alignas (cubthread::entry) static unsigned char thr_storage[sizeof (cubthread::entry)];
static cubthread::entry *const thr = reinterpret_cast<cubthread::entry *> (thr_storage);

struct attr_slot
{
  DB_VALUE v;
};
static std::map<int, attr_slot> g_attrs;
static HEAP_CACHE_ATTRINFO g_attrinfo;

/* heap_file.c: the current record's decoded value of an attribute (the scan read the record before the row is
 * evaluated). A column fetch is not observable -- it cannot fail and changes nothing -- so it is not logged: the
 * program fetches a right column even for a NULL left one (a leaf that cannot fail stays out of the lazy region,
 * expr_steps_fallible), the interpreter does not, and neither difference shows in a value or an error. */
static int g_attr_reads = 0;
DB_VALUE *
heap_attrinfo_access (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  (void) attr_info;
  g_attr_reads++;
  return &g_attrs[attrid].v;
}

/* area_alloc.c: one element of the area. The only area these paths reach is the domain area (tp_domain_new for a
 * NUMERIC(p,s) / VARCHAR(n) domain), which tp_init () would have created; the element is a TP_DOMAIN that
 * tp_domain_init () fills, and area_free () gives it back */
void *
area_alloc (AREA * area)
{
  (void) area;
  return calloc (1, sizeof (TP_DOMAIN));
}

int
area_free (AREA * area, void *ptr)
{
  (void) area;
  free (ptr);
  return NO_ERROR;
}

/* language_support.c: lang_init_builtin () fills every collation slot with iso88591_bin before the built-ins are
 * registered; the coercion of a number into a VARCHAR(n) iso88591_bin domain reads only the codeset and the id.
 * lang_get_collation is the language_support.c:1731 body over that array. */
#include "language_support.h"
LANG_COLLATION *lang_Collations[LANG_MAX_COLLATIONS];
static LANG_COLLATION fake_iso_bin;
static bool
boot_collations ()
{
  fake_iso_bin.codeset = INTL_CODESET_ISO88591;
  fake_iso_bin.built_in = 1;
  fake_iso_bin.coll.coll_id = LANG_COLL_ISO_BINARY;
  for (int i = 0; i < LANG_MAX_COLLATIONS; i++)
    {
      lang_Collations[i] = &fake_iso_bin;
    }
  return true;
}
static const bool collations_booted = boot_collations ();

LANG_COLLATION *
lang_get_collation (const int coll_id)
{
  assert (coll_id >= 0 && coll_id < LANG_MAX_COLLATIONS);
  return lang_Collations[coll_id];
}

/* ---- scalar subquery leaves: the one observable, fallible leaf.  fetch_peek_dbval_slow () executes a linked XASL
 * (TYPE_CONSTANT with regu->xasl, EXECUTE_REGU_VARIABLE_XASL) through qexec_execute_mainblock: on success the real
 * one leaves the single value in the regu's dbvalptr and sets the node XASL_SUCCESS; on failure it has set the error
 * (the macro marks the node XASL_FAILURE).  The fake does exactly that, and logs the execution. */
struct sq_leaf
{
  std::string name;
  DB_VALUE value;
  int fail;			/* NO_ERROR, or the error the execution raises */
  DB_VALUE holder;		/* the regu's dbvalptr target */
  XASL_NODE *xasl;
};
static std::deque<sq_leaf> g_sqs;
static std::vector<std::string> g_log;

int
qexec_execute_mainblock (THREAD_ENTRY * thread_p, xasl_node * xasl, xasl_state * xstate,
			 UPDDEL_CLASS_INSTANCE_LOCK_INFO * p_class_instance_lock_info)
{
  (void) thread_p;
  (void) xstate;
  (void) p_class_instance_lock_info;
  for (sq_leaf & q:g_sqs)
    {
      if (q.xasl == xasl)
	{
	  g_log.push_back (q.name);
	  if (q.fail != NO_ERROR)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, q.fail, 0);
	      return q.fail;
	    }
	  q.holder = q.value;
	  xasl->status = XASL_SUCCESS;
	  return NO_ERROR;
	}
    }
  assert (false);
  return ER_FAILED;
}

/* ================================================================ builders */

static REGU_VARIABLE *
new_regu (REGU_DATATYPE type, TP_DOMAIN * dom)
{
  REGU_VARIABLE *r = new REGU_VARIABLE ();
  std::memset ((void *) r, 0, sizeof (*r));
  r->type = type;
  r->domain = dom;
  return r;
}

static std::vector<REGU_VARIABLE *> g_attr_leaves;

static REGU_VARIABLE *
attr (int id, TP_DOMAIN * dom)
{
  REGU_VARIABLE *r = new_regu (TYPE_ATTR_ID, dom);
  r->value.attr_descr.id = id;
  r->value.attr_descr.type = TP_DOMAIN_TYPE (dom);
  r->value.attr_descr.cache_attrinfo = &g_attrinfo;
  g_attr_leaves.push_back (r);
  return r;
}

static sq_leaf *
sq_find (const std::string & name)
{
  for (sq_leaf & q:g_sqs)
    {
      if (q.name == name)
	{
	  return &q;
	}
    }
  return NULL;
}

/* a scalar subquery operand named name: its value per row is set with sq_val / sq_fail */
static REGU_VARIABLE *
sq (const char *name, TP_DOMAIN * dom)
{
  REGU_VARIABLE *r = new_regu (TYPE_CONSTANT, dom);
  g_sqs.emplace_back ();
  sq_leaf & q = g_sqs.back ();
  q.name = name;
  db_make_null (&q.value);
  db_make_null (&q.holder);
  q.fail = NO_ERROR;
  q.xasl = (XASL_NODE *) calloc (1, sizeof (XASL_NODE));
  q.xasl->flag = XASL_LINK_TO_REGU_VARIABLE;
  q.xasl->status = XASL_INITIALIZED;
  r->xasl = q.xasl;
  r->value.dbvalptr = &q.holder;
  return r;
}

static void
sq_val (const char *name, DB_VALUE v)
{
  sq_find (name)->value = v;
  sq_find (name)->fail = NO_ERROR;
}

static void
sq_fail (const char *name, int err)
{
  sq_find (name)->fail = err;
}

static DOMAIN_PLAN_ITEM *
number_plan (DB_TYPE t, TP_DOMAIN * dom)
{
  DOMAIN_PLAN_ITEM *it = new DOMAIN_PLAN_ITEM ();
  std::memset ((void *) it, 0, sizeof (*it));
  it->resolved_index = -1;
  it->ref = -1;
  it->node_domain_index = -1;
  it->fixed.domain = dom;
  it->fixed.arith.kind = DOMAIN_ARITH_NUMBER;
  it->fixed.arith.type = t;
  it->temporaries[0] = it->temporaries[1] = -1;
  return it;
}

static REGU_VARIABLE *
arith (OPERATOR_TYPE op, TP_DOMAIN * dom, REGU_VARIABLE * l, REGU_VARIABLE * r, DOMAIN_PLAN_ITEM * item)
{
  ARITH_TYPE *a = new ARITH_TYPE ();
  std::memset ((void *) a, 0, sizeof (*a));
  a->domain = dom;
  a->plan_item = item;
  a->value = new DB_VALUE;
  db_make_null (a->value);
  a->leftptr = l;
  a->rightptr = r;
  a->opcode = op;
  REGU_VARIABLE *rv = new_regu (TYPE_INARITH, dom);
  rv->value.arithptr = a;
  return rv;
}

static VAL_DESCR g_vd;
static int g_outer_depth = 0;	/* the thread's frames above the evaluation (a correlated subquery's, a caller's) */

static void
reset_row ()
{
  g_errid = NO_ERROR;
  g_log.clear ();
  for (REGU_VARIABLE *r : g_attr_leaves)
    {
      r->value.attr_descr.cache_dbvalp = NULL;
      r->value.attr_descr.cache_slot = NULL;
    }
  for (sq_leaf & q:g_sqs)
    {
      q.xasl->status = XASL_INITIALIZED;	/* a correlated subquery runs again for every row */
      db_make_null (&q.holder);
    }
  thr->xasl_recursion_depth = g_outer_depth;
}

/* a value's full identity: type and payload; a NUMERIC with its domain and header precision/scale, sign and
 * coefficient bytes -- two NUMERICs that print alike but differ in representation (fixed vs float) differ here */
static std::string
show (const DB_VALUE * v)
{
  char buf[512];
  if (v == NULL)
    {
      return "(no value)";
    }
  if (DB_IS_NULL (v))
    {
      return "NULL";
    }
  switch (DB_VALUE_DOMAIN_TYPE (v))
    {
    case DB_TYPE_INTEGER:
      return "INT:" + std::to_string (db_get_int (v));
    case DB_TYPE_BIGINT:
      return "BIGINT:" + std::to_string ((long long) db_get_bigint (v));
    case DB_TYPE_SHORT:
      return "SHORT:" + std::to_string (db_get_short (v));
    case DB_TYPE_DOUBLE:
      std::snprintf (buf, sizeof (buf), "DOUBLE:%a", db_get_double (v));
      return buf;
    case DB_TYPE_NUMERIC:
      {
	char num[256], hex[64];
	int p = -1, sc = -1;
	bool is_float = false;
	const unsigned char *b = (const unsigned char *) db_locate_numeric (v);
	db_get_numeric_precision_and_scale (v, &p, &sc, &is_float);
	numeric_db_value_print (v, num);
	for (int k = 0; k < DB_NUMERIC_BUF_SIZE; k++)
	  {
	    std::snprintf (hex + 2 * k, 3, "%02x", b[k]);
	  }
	std::snprintf (buf, sizeof (buf), "NUMERIC%s(%d,%d)dom(%d,%d)neg%d:%s[%s]", is_float ? "f" : "x", p, sc,
		       v->domain.numeric_info.precision, v->domain.numeric_info.scale,
		       (int) v->domain.numeric_info.is_value_negative, num, hex);
	return buf;
      }
    case DB_TYPE_VARCHAR:
    case DB_TYPE_CHAR:
      return std::string ("STR:") + std::string (db_get_string (v), db_get_string_size (v));
    default:
      return "type" + std::to_string ((int) DB_VALUE_DOMAIN_TYPE (v));
    }
}

struct outcome
{
  std::string value;		/* show () of the value, or ERR:<er_errid> */
  std::string log;		/* the leaves fetched, in order */
};

static outcome
make_outcome (int rc, const DB_VALUE * v)
{
  outcome o;
  o.value = (rc != NO_ERROR) ? "ERR:" + std::to_string (g_errid) : show (v);
  for (const std::string & s : g_log)
    {
      o.log += s + " ";
    }
  return o;
}

static outcome
run_interp (REGU_VARIABLE * root)
{
  DB_VALUE *peek = NULL;
  reset_row ();
  int rc = fetch_peek_dbval (thr, root, &g_vd, NULL, NULL, NULL, &peek);
  return make_outcome (rc, peek);
}

/* a compiled single-root program; NULL when the compiler declines the root */
static EXPR_PROG *
compile1 (REGU_VARIABLE * root, int *idx)
{
  REGU_VARIABLE *roots[1] = { root };
  reset_row ();
  EXPR_PROG *p = expr_prog_compile_roots (thr, roots, 1, &g_vd, false, false, false, idx, NULL);
  if (p != NULL && *idx < 0)
    {
      expr_prog_free (p);
      return NULL;
    }
  return p;
}

static outcome
run_prog (EXPR_PROG * p, int idx)
{
  reset_row ();
  int rc = expr_prog_eval (p, thr, &g_vd, NULL, NULL);
  return make_outcome (rc, rc == NO_ERROR ? expr_prog_value (p, idx) : NULL);
}


/* ================================================================ hooks (hooks.cpp, appended to expr_compile.c) */

EXPR_KERNEL_FN unitseam_arith_kernel (int opcode, int type, bool numeric_pure);
const char *unitseam_kernel_name (EXPR_KERNEL_FN fn);

static std::string
kernels_of (const EXPR_PROG * p)
{
  std::string k;
  for (int i = 0; i < p->n_steps; i++)
    {
      k += std::string (unitseam_kernel_name (p->steps[i].kernel)) + " ";
    }
  return k;
}

/* ================================================================ values */

static DB_VALUE
v_null ()
{
  DB_VALUE v;
  db_make_null (&v);
  return v;
}

static DB_VALUE
v_int (int i)
{
  DB_VALUE v;
  db_make_int (&v, i);
  return v;
}

static DB_VALUE
v_short (short i)
{
  DB_VALUE v;
  db_make_short (&v, i);
  return v;
}

static DB_VALUE
v_big (DB_BIGINT i)
{
  DB_VALUE v;
  db_make_bigint (&v, i);
  return v;
}

static DB_VALUE
v_dbl (double d)
{
  DB_VALUE v;
  db_make_double (&v, d);
  return v;
}

/* a NUMERIC as an intermediate result holds it: the float form numeric_coerce_string_to_num () makes (precision and
 * scale in the header); with a column domain, the fixed form a NUMERIC(p,s) column holds (tp_value_coerce) */
static DB_VALUE
v_num (const char *s, TP_DOMAIN * column = NULL)
{
  DB_VALUE v, w;
  db_make_null (&v);
  REQUIRE (numeric_coerce_string_to_num (s, (int) std::strlen (s), INTL_CODESET_ISO88591, &v) == NO_ERROR);
  if (column == NULL)
    {
      return v;
    }
  db_make_null (&w);
  REQUIRE (tp_value_coerce (&v, &w, column) == DOMAIN_COMPATIBLE);
  return w;
}

static TP_DOMAIN *
d_num (int p, int s)
{
  return tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0);
}

#define D(t) tp_domain_resolve_default (t)

static const char *
op_name (OPERATOR_TYPE op)
{
  return op == T_ADD ? "+" : op == T_SUB ? "-" : op == T_MUL ? "*" : op == T_DIV ? "/" : "?";
}

/* ================================================================ the differential driver */

struct grid_result
{
  int rows = 0, errs = 0, nulls = 0;
  std::string kernels;		/* the program's steps, "(declined)" when the compiler left the root interpreted */
  std::vector<std::string> mismatches;
  std::map<std::string, int> err_codes;
};

/* One node "attr(1) op attr(2)" compiled once, then every (left, right) row evaluated by the interpreter
 * (fetch_peek_dbval on the node) and by the program: value (or er_errid) and the leaves fetched must agree. */
static grid_result
diff_grid (OPERATOR_TYPE op, DB_TYPE plan_type, TP_DOMAIN * node_dom, TP_DOMAIN * ldom, TP_DOMAIN * rdom,
	   const std::vector<DB_VALUE> &L, const std::vector<DB_VALUE> &R)
{
  grid_result g;
  REGU_VARIABLE *root = arith (op, node_dom, attr (1, ldom), attr (2, rdom), number_plan (plan_type, node_dom));
  int idx = -1;
  EXPR_PROG *p = compile1 (root, &idx);
  if (p == NULL)
    {
      g.kernels = "(declined)";
      return g;
    }
  g.kernels = kernels_of (p);
  for (const DB_VALUE & l:L)
    {
      for (const DB_VALUE & r:R)
	{
	  g_attrs[1].v = l;
	  g_attrs[2].v = r;
	  outcome a = run_interp (root);
	  outcome b = run_prog (p, idx);
	  g.rows++;
	  if (a.value.compare (0, 4, "ERR:") == 0)
	    {
	      g.errs++;
	      g.err_codes[a.value]++;
	    }
	  else if (a.value == "NULL")
	    {
	      g.nulls++;
	    }
	  if (a.value != b.value || a.log != b.log)
	    {
	      g.mismatches.push_back (show (&l) + " " + op_name (op) + " " + show (&r) + "\n    interp: " + a.value + " {" +
				      a.log + "}\n    program: " + b.value + " {" + b.log + "}");
	    }
	}
    }
  expr_prog_free (p);
  return g;
}

static std::string
first_mismatches (const grid_result & g, size_t n = 6)
{
  std::string s;
  for (size_t i = 0; i < g.mismatches.size () && i < n; i++)
    {
      s += g.mismatches[i] + "\n";
    }
  return s;
}

#define REQUIRE_SAME(g) \
  do \
    { \
      INFO ("steps: " << (g).kernels << "\nrows " << (g).rows << ", mismatches " << (g).mismatches.size () << "\n" \
	    << first_mismatches (g)); \
      CHECK ((g).rows > 0); \
      CHECK ((g).mismatches.empty ()); \
    } \
  while (0)

/* outcome of one row of one node, interpreter and program, for the anchored expectations */
static std::pair<std::string, std::string>
one_row (OPERATOR_TYPE op, DB_TYPE plan_type, TP_DOMAIN * node_dom, TP_DOMAIN * ldom, TP_DOMAIN * rdom, DB_VALUE l,
	 DB_VALUE r, std::string * steps = NULL)
{
  REGU_VARIABLE *root = arith (op, node_dom, attr (1, ldom), attr (2, rdom), number_plan (plan_type, node_dom));
  int idx = -1;
  g_attrs[1].v = l;
  g_attrs[2].v = r;
  outcome a = run_interp (root);
  EXPR_PROG *p = compile1 (root, &idx);
  if (p == NULL)
    {
      return { a.value, "(declined)" };
    }
  if (steps != NULL)
    {
      *steps = kernels_of (p);
    }
  outcome b = run_prog (p, idx);
  expr_prog_free (p);
  return { a.value, b.value };
}

static const OPERATOR_TYPE ARITH_OPS[] = { T_ADD, T_SUB, T_MUL, T_DIV };

static std::vector<DB_VALUE>
ints ()
{
  return { v_int (0), v_int (1), v_int (-1), v_int (2), v_int (-2), v_int (7), v_int (-7), v_int (46341),
    v_int (-46341), v_int (65536), v_int (INT_MAX), v_int (INT_MIN), v_int (INT_MAX - 1), v_int (INT_MIN + 1),
    v_null ()
  };
}

static std::vector<DB_VALUE>
bigints ()
{
  return { v_big (0), v_big (1), v_big (-1), v_big (7), v_big (-7), v_big (3037000500LL), v_big (-3037000500LL),
    v_big (4294967296LL), v_big (LLONG_MAX), v_big (LLONG_MIN), v_big (LLONG_MAX - 1), v_big (LLONG_MIN + 1),
    v_null ()
  };
}

static std::vector<DB_VALUE>
doubles ()
{
  return { v_dbl (0.0), v_dbl (-0.0), v_dbl (1.5), v_dbl (-2.25), v_dbl (3.0), v_dbl (1e308), v_dbl (-1e308),
    v_dbl (DBL_MAX), v_dbl (1e-308), v_dbl (4.9e-324), v_dbl (DBL_EPSILON), v_dbl (DBL_EPSILON / 2),
    v_dbl (-DBL_EPSILON / 4), v_null ()
  };
}

/* float-form NUMERICs (intermediate results) and fixed-form ones (columns), across the 64-bit single-word
 * boundary (mul_fixed64), the 38/40-digit edge and wide scales */
static std::vector<DB_VALUE>
numerics ()
{
  static const char *f[] = {
    "0", "1", "-1", "1.5", "-2.25", "0.1", "3.33", "123456789012345678", "9999999999999999999",
    "18446744073709551615", "18446744073709551616", "-18446744073709551615", "99999999999999999999999999999999999999",
    "9999999999999999999999999999999999999999", "-9999999999999999999999999999999999999999",
    "0.0000000000000000000000000000000000000001", "12345.678901234567890123456789", "-0.000000000000000000001"
  };
  std::vector<DB_VALUE> v;
  for (const char *s : f)
    {
      v.push_back (v_num (s));
    }
  v.push_back (v_num ("1.5", d_num (38, 10)));
  v.push_back (v_num ("-12345678901234567890.0123456789", d_num (38, 10)));
  v.push_back (v_num ("99999.99999", d_num (10, 5)));
  v.push_back (v_num ("7", d_num (5, 0)));
  v.push_back (v_null ());
  return v;
}

/* ================================================================ cases */

TEST_CASE ("kernel table: a kernel per (+,-,*,/) x (INTEGER, BIGINT, DOUBLE, NUMERIC) but DOUBLE division; "
	   "NUMERIC has a float (pure) and a plain (integer side) family; nothing else", "[contract]")
{
  const DB_TYPE types[] = { DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_DOUBLE, DB_TYPE_NUMERIC };
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      for (DB_TYPE t : types)
	{
	  INFO ("op " << op_name (op) << " type " << (int) t);
	  EXPR_KERNEL_FN pure = unitseam_arith_kernel (op, t, true);
	  EXPR_KERNEL_FN mix = unitseam_arith_kernel (op, t, false);
	  if (op == T_DIV && t == DB_TYPE_DOUBLE)
	    {
	      CHECK (pure == NULL);
	      continue;
	    }
	  REQUIRE (pure != NULL);
	  if (t == DB_TYPE_NUMERIC)
	    {
	      CHECK (mix != NULL);
	      CHECK (mix != pure);
	      CHECK (std::string (unitseam_kernel_name (mix)).back () == '*');
	    }
	  else
	    {
	      CHECK (mix == pure);	/* the mix only splits NUMERIC */
	    }
	}
    }
  const DB_TYPE others[] = { DB_TYPE_SHORT, DB_TYPE_FLOAT, DB_TYPE_MONETARY, DB_TYPE_VARCHAR, DB_TYPE_DATE };
  for (DB_TYPE t : others)
    {
      for (OPERATOR_TYPE op : ARITH_OPS)
	{
	  CHECK (unitseam_arith_kernel (op, t, true) == NULL);
	}
    }
  const OPERATOR_TYPE nonarith[] = { T_MOD, T_INTDIV, T_STRCAT, T_UNMINUS };
  for (OPERATOR_TYPE op : nonarith)
    {
      CHECK (unitseam_arith_kernel (op, DB_TYPE_INTEGER, true) == NULL);
    }
}

TEST_CASE ("INTEGER kernels == interpreter over the boundary grid (overflow per operator, zero divide, "
	   "INT_MIN / -1, NULL operand)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  const char *expect[] = { "add_int", "sub_int", "mul_int", "div_int" };
  int k = 0;
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      grid_result g = diff_grid (op, DB_TYPE_INTEGER, i, i, i, ints (), ints ());
      INFO ("op " << op_name (op));
      CHECK (g.kernels.find (expect[k++]) != std::string::npos);
      CHECK (g.errs > 0);
      CHECK (g.nulls > 0);
      REQUIRE_SAME (g);
    }
}

TEST_CASE ("INTEGER anchors: the error each overflow raises, NULL before the zero divide", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  auto chk =[&](OPERATOR_TYPE op, DB_VALUE l, DB_VALUE r, const std::string & want)
  {
    auto o = one_row (op, DB_TYPE_INTEGER, i, i, i, l, r);
    INFO (show (&l) << " " << op_name (op) << " " << show (&r));
    CHECK (o.first == want);
    CHECK (o.second == want);
  };
  chk (T_ADD, v_int (INT_MAX), v_int (1), "ERR:" + std::to_string (ER_QPROC_OVERFLOW_ADDITION));
  chk (T_SUB, v_int (INT_MIN), v_int (1), "ERR:" + std::to_string (ER_QPROC_OVERFLOW_SUBTRACTION));
  chk (T_MUL, v_int (65536), v_int (65536), "ERR:" + std::to_string (ER_QPROC_OVERFLOW_MULTIPLICATION));
  chk (T_DIV, v_int (INT_MIN), v_int (-1), "ERR:" + std::to_string (ER_QPROC_OVERFLOW_DIVISION));
  chk (T_DIV, v_int (7), v_int (0), "ERR:" + std::to_string (ER_QPROC_ZERO_DIVIDE));
  chk (T_DIV, v_null (), v_int (0), "NULL");
  chk (T_DIV, v_int (-7), v_int (2), "INT:-3");
  chk (T_MUL, v_int (46341), v_int (-46341), "ERR:" + std::to_string (ER_QPROC_OVERFLOW_MULTIPLICATION));
}

TEST_CASE ("BIGINT kernels == interpreter over the boundary grid", "[contract]")
{
  TP_DOMAIN *b = D (DB_TYPE_BIGINT);
  const char *expect[] = { "add_bigint", "sub_bigint", "mul_bigint", "div_bigint" };
  int k = 0;
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      grid_result g = diff_grid (op, DB_TYPE_BIGINT, b, b, b, bigints (), bigints ());
      INFO ("op " << op_name (op));
      CHECK (g.kernels.find (expect[k++]) != std::string::npos);
      CHECK (g.errs > 0);
      REQUIRE_SAME (g);
    }
}

TEST_CASE ("DOUBLE: +,-,* kernels and the interpreted division (generic step: DBL_EPSILON zero test) == "
	   "interpreter", "[contract]")
{
  TP_DOMAIN *d = D (DB_TYPE_DOUBLE);
  const char *expect[] = { "add_double", "sub_double", "mul_double", "generic" };
  int k = 0;
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      grid_result g = diff_grid (op, DB_TYPE_DOUBLE, d, d, d, doubles (), doubles ());
      INFO ("op " << op_name (op));
      CHECK (g.kernels.find (expect[k++]) != std::string::npos);
      REQUIRE_SAME (g);
    }
  auto o = one_row (T_DIV, DB_TYPE_DOUBLE, d, d, d, v_dbl (1.0), v_dbl (DBL_EPSILON / 2));
  CHECK (o.first == "ERR:" + std::to_string (ER_QPROC_ZERO_DIVIDE));
  CHECK (o.second == o.first);
}

TEST_CASE ("NUMERIC x NUMERIC (float family, mul via fixed64 first) == interpreter, default and narrow node "
	   "domains (the epilogue coercion)", "[contract]")
{
  TP_DOMAIN *n = D (DB_TYPE_NUMERIC);
  TP_DOMAIN *node_doms[] = { n, d_num (38, 10), d_num (20, 5), d_num (10, 2), d_num (5, 0) };
  const char *expect[] = { "add_numeric ", "sub_numeric ", "mul_numeric ", "div_numeric " };
  for (TP_DOMAIN * nd : node_doms)
    {
      int k = 0;
      for (OPERATOR_TYPE op : ARITH_OPS)
	{
	  grid_result g = diff_grid (op, DB_TYPE_NUMERIC, nd, n, n, numerics (), numerics ());
	  INFO ("op " << op_name (op) << " node NUMERIC(" << nd->precision << "," << nd->scale << ")");
	  CHECK (g.kernels.find (expect[k++]) != std::string::npos);
	  REQUIRE_SAME (g);
	}
    }
}

TEST_CASE ("NUMERIC with an integer side (plain family over the coerced integer), either side, SHORT/INTEGER/"
	   "BIGINT == interpreter", "[contract]")
{
  TP_DOMAIN *n = D (DB_TYPE_NUMERIC);
  const char *expect[] = { "add_numeric*", "sub_numeric*", "mul_numeric*", "div_numeric*" };
  struct side
  {
    TP_DOMAIN *dom;
    std::vector<DB_VALUE> vals;
  };
  side sides[] = {
    {D (DB_TYPE_INTEGER), ints ()},
    {D (DB_TYPE_BIGINT), bigints ()},
    {D (DB_TYPE_SHORT), {v_short (0), v_short (1), v_short (-7), v_short (32767), v_short (-32768), v_null ()}},
  };
  for (TP_DOMAIN * nd : { n, d_num (38, 10), d_num (10, 2) })
    {
      for (side & sd : sides)
	{
	  int k = 0;
	  for (OPERATOR_TYPE op : ARITH_OPS)
	    {
	      INFO ("op " << op_name (op) << " integer type " << (int) TP_DOMAIN_TYPE (sd.dom) << " node NUMERIC(" <<
		    nd->precision << "," << nd->scale << ")");
	      grid_result g1 = diff_grid (op, DB_TYPE_NUMERIC, nd, n, sd.dom, numerics (), sd.vals);
	      CHECK (g1.kernels.find ("coerce_numeric") != std::string::npos);
	      CHECK (g1.kernels.find (expect[k]) != std::string::npos);
	      REQUIRE_SAME (g1);
	      grid_result g2 = diff_grid (op, DB_TYPE_NUMERIC, nd, sd.dom, n, sd.vals, numerics ());
	      CHECK (g2.kernels.find (expect[k]) != std::string::npos);
	      REQUIRE_SAME (g2);
	      k++;
	    }
	}
    }
}

TEST_CASE ("result domain other than the operation's type: a wider number domain keeps the kernel and coerces; "
	   "a narrower one raises what the interpreter raises", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *b = D (DB_TYPE_BIGINT);
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      INFO ("op " << op_name (op));
      grid_result w1 = diff_grid (op, DB_TYPE_INTEGER, b, i, i, ints (), ints ());
      CHECK (w1.kernels.find ("_int") != std::string::npos);
      REQUIRE_SAME (w1);
      grid_result w2 = diff_grid (op, DB_TYPE_INTEGER, D (DB_TYPE_DOUBLE), i, i, ints (), ints ());
      REQUIRE_SAME (w2);
      grid_result w3 = diff_grid (op, DB_TYPE_INTEGER, d_num (15, 2), i, i, ints (), ints ());
      REQUIRE_SAME (w3);
      grid_result n1 = diff_grid (op, DB_TYPE_BIGINT, i, b, b, bigints (), bigints ());
      CHECK (n1.kernels.find ("_bigint") != std::string::npos);
      REQUIRE_SAME (n1);
    }
}

TEST_CASE ("result domain that is not a number (a coercion into a string): the kernel is declined, the generic "
	   "step gives the interpreter's value", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  TP_DOMAIN *vc = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 64, 0, NULL, LANG_COLL_ISO_BINARY);
  REQUIRE (vc != NULL);
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      INFO ("op " << op_name (op));
      grid_result g = diff_grid (op, DB_TYPE_INTEGER, vc, i, i, ints (), ints ());
      CHECK (g.kernels.find ("_int") == std::string::npos);
      CHECK (g.kernels.find ("generic") != std::string::npos);
      REQUIRE_SAME (g);
    }
}

TEST_CASE ("a plan type outside the table (SHORT) goes to the generic step == interpreter", "[contract]")
{
  TP_DOMAIN *s = D (DB_TYPE_SHORT);
  std::vector<DB_VALUE> sv = { v_short (0), v_short (1), v_short (-1), v_short (181), v_short (-182), v_short (32767),
    v_short (-32768), v_null ()
  };
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      INFO ("op " << op_name (op));
      grid_result g = diff_grid (op, DB_TYPE_SHORT, s, s, s, sv, sv);
      CHECK (g.kernels.find ("generic") != std::string::npos);
      REQUIRE_SAME (g);
    }
}

TEST_CASE ("a value whose type drifted from the kernel's (a BIGINT or SHORT under an INTEGER kernel): the row goes "
	   "to the interpreter, never a raw db_get_int", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<DB_VALUE> drift = { v_big (5), v_big (4294967296LL), v_short (3), v_int (2), v_null () };
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      INFO ("op " << op_name (op));
      grid_result g = diff_grid (op, DB_TYPE_INTEGER, i, i, i, drift, drift);
      CHECK (g.kernels.find ("_int") != std::string::npos);
      REQUIRE_SAME (g);
    }
}

TEST_CASE ("float_numeric_db_value_mul_fixed64 == float_numeric_db_value_mul bit for bit where it answers, and it "
	   "declines past one word", "[contract]")
{
  std::vector<DB_VALUE> v = numerics ();
  v.pop_back ();		/* the NULL */
  int answered = 0, declined = 0;
  std::vector<std::string> bad;
  for (const DB_VALUE & a:v)
    {
      for (const DB_VALUE & b:v)
	{
	  DB_VALUE r1, r2;
	  db_make_null (&r1);
	  db_make_null (&r2);
	  int rc_ref = float_numeric_db_value_mul (&a, &b, &r1);
	  if (float_numeric_db_value_mul_fixed64 (&a, &b, &r2))
	    {
	      answered++;
	      if (rc_ref != NO_ERROR || show (&r1) != show (&r2))
		{
		  bad.push_back (show (&a) + " * " + show (&b) + "\n  ref " + show (&r1) + "\n  f64 " + show (&r2));
		}
	    }
	  else
	    {
	      declined++;
	    }
	}
    }
  INFO ("answered " << answered << ", declined " << declined << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (answered > 50);
  CHECK (declined > 50);
  CHECK (bad.empty ());
  DB_VALUE two64 = v_num ("18446744073709551616"), one = v_num ("1"), r;
  db_make_null (&r);
  CHECK (float_numeric_db_value_mul_fixed64 (&two64, &one, &r) == 0);
}

static REGU_VARIABLE *
lit_int_ak (int k)
{
  REGU_VARIABLE *c = new_regu (TYPE_DBVAL, D (DB_TYPE_INTEGER));
  c->value.dbval = v_int (k);
  return c;
}

/* ================================================================ lazy right operands (an operand that can fail) */

static const int E_LEFT = -9001, E_RIGHT = -9002;

/* every (left, right) state of two subquery operands: a value, NULL, or a failure */
struct sq_state
{
  const char *what;
  bool fail;
  DB_VALUE v;
};

static std::vector<std::string>
diff_sq_states (REGU_VARIABLE * root, const std::vector<sq_state> &ls, const std::vector<sq_state> &rs,
		std::string * steps)
{
  std::vector<std::string> bad;
  int idx = -1;
  EXPR_PROG *p = compile1 (root, &idx);
  if (p == NULL)
    {
      bad.push_back ("(declined)");
      return bad;
    }
  *steps = kernels_of (p);
  for (const sq_state & l:ls)
    {
      for (const sq_state & r:rs)
	{
	  sq_val ("L", l.v);
	  sq_val ("R", r.v);
	  if (l.fail)
	    {
	      sq_fail ("L", E_LEFT);
	    }
	  if (r.fail)
	    {
	      sq_fail ("R", E_RIGHT);
	    }
	  outcome a = run_interp (root);
	  outcome b = run_prog (p, idx);
	  if (a.value != b.value || a.log != b.log)
	    {
	      bad.push_back (std::string ("L=") + l.what + " R=" + r.what + "\n    interp: " + a.value + " {" + a.log +
			     "}\n    program: " + b.value + " {" + b.log + "}");
	    }
	}
    }
  expr_prog_free (p);
  return bad;
}

#define REQUIRE_NO_DIFF(bad, steps) \
  do \
    { \
      INFO ("steps: " << (steps) << "\n" << ((bad).empty ()? std::string () : (bad)[0])); \
      CHECK ((bad).empty ()); \
    } \
  while (0)

TEST_CASE ("lazy right operand of a kernel: a NULL left skips a right side that can fail -- the subquery is not "
	   "run and its error not raised, as in the interpreter", "[contract]")
{
  for (DB_TYPE t : { DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_DOUBLE })
    {
      TP_DOMAIN *d = D (t);
      DB_VALUE one, zero;
      db_value_domain_init (&one, t, 0, 0);
      db_value_domain_init (&zero, t, 0, 0);
      if (t == DB_TYPE_INTEGER)
	{
	  db_make_int (&one, 1), db_make_int (&zero, 0);
	}
      else if (t == DB_TYPE_BIGINT)
	{
	  db_make_bigint (&one, 1), db_make_bigint (&zero, 0);
	}
      else
	{
	  db_make_double (&one, 1.0), db_make_double (&zero, 0.0);
	}
      std::vector<sq_state> states = { {"1", false, one}, {"0", false, zero}, {"NULL", false, v_null ()},
      {"fail", true, v_null ()}
      };
      for (OPERATOR_TYPE op : ARITH_OPS)
	{
	  REGU_VARIABLE *root = arith (op, d, sq ("L", d), sq ("R", d), number_plan (t, d));
	  std::string steps;
	  std::vector<std::string> bad = diff_sq_states (root, states, states, &steps);
	  INFO ("type " << (int) t << " op " << op_name (op));
	  if (!(t == DB_TYPE_DOUBLE && op == T_DIV))
	    {
	      CHECK (steps.find ("jump_null") != std::string::npos);
	    }
	  REQUIRE_NO_DIFF (bad, steps);
	  g_sqs.clear ();
	}
    }
}

TEST_CASE ("lazy right operand under the NUMERIC mix: the integer side's coercion sits inside the skipped region",
	   "[contract]")
{
  TP_DOMAIN *n = D (DB_TYPE_NUMERIC), *i = D (DB_TYPE_INTEGER);
  std::vector<sq_state> ns = { {"1.5", false, v_num ("1.5")}, {"0", false, v_num ("0")}, {"NULL", false, v_null ()},
  {"fail", true, v_null ()}
  };
  std::vector<sq_state> is = { {"2", false, v_int (2)}, {"0", false, v_int (0)}, {"NULL", false, v_null ()},
  {"fail", true, v_null ()}
  };
  for (OPERATOR_TYPE op : ARITH_OPS)
    {
      std::string steps;
      REGU_VARIABLE *r1 = arith (op, n, sq ("L", n), sq ("R", i), number_plan (DB_TYPE_NUMERIC, n));
      std::vector<std::string> bad = diff_sq_states (r1, ns, is, &steps);
      INFO ("op " << op_name (op) << " numeric op integer");
      REQUIRE_NO_DIFF (bad, steps);
      g_sqs.clear ();
      REGU_VARIABLE *r2 = arith (op, n, sq ("L", i), sq ("R", n), number_plan (DB_TYPE_NUMERIC, n));
      bad = diff_sq_states (r2, is, ns, &steps);
      INFO ("op " << op_name (op) << " integer op numeric");
      REQUIRE_NO_DIFF (bad, steps);
      g_sqs.clear ();
    }
}

TEST_CASE ("NVL: a non-NULL left skips a right side that can fail (jump_notnull); the pointer select returns the "
	   "interpreter's value", "[contract]")
{
  for (DB_TYPE t : { DB_TYPE_INTEGER, DB_TYPE_BIGINT, DB_TYPE_DOUBLE })
    {
      TP_DOMAIN *d = D (t);
      DB_VALUE seven;
      if (t == DB_TYPE_INTEGER)
	{
	  db_make_int (&seven, 7);
	}
      else if (t == DB_TYPE_BIGINT)
	{
	  db_make_bigint (&seven, 7);
	}
      else
	{
	  db_make_double (&seven, 7.0);
	}
      std::vector<sq_state> states = { {"7", false, seven}, {"NULL", false, v_null ()}, {"fail", true, v_null ()} };
      for (OPERATOR_TYPE op : { T_NVL, T_IFNULL, T_COALESCE })
	{
	  REGU_VARIABLE *root = arith (op, d, sq ("L", d), sq ("R", d), NULL);
	  std::string steps;
	  std::vector<std::string> bad = diff_sq_states (root, states, states, &steps);
	  INFO ("type " << (int) t << " opcode " << (int) op);
	  CHECK (steps.find ("jump_notnull") != std::string::npos);
	  CHECK (steps.find ("nvl_select") != std::string::npos);
	  REQUIRE_NO_DIFF (bad, steps);
	  g_sqs.clear ();
	}
    }
}

/* ================================================================ a list of roots (the projection's program) */

/* The interpreter evaluates a list column by column and stops at the first error; the program runs every covered
 * root's steps in one pass (prologues first).  With fallback roots allowed (expr_prog_compile) every column is in
 * the program in list order, so the values and the first error must be the interpreter's. */
static std::string
interp_list (const std::vector<REGU_VARIABLE *> &roots)
{
  std::string s;
  for (REGU_VARIABLE * r:roots)
    {
      outcome o;
      DB_VALUE *peek = NULL;
      g_errid = NO_ERROR;
      int rc = fetch_peek_dbval (thr, r, &g_vd, NULL, NULL, NULL, &peek);
      if (rc != NO_ERROR)
	{
	  return "ERR:" + std::to_string (g_errid);	/* the row fails: no column of it is produced */
	}
      s += show (peek) + " | ";
    }
  return s;
}

static std::string
prog_list (EXPR_PROG * p, const std::vector<int> &idx)
{
  g_errid = NO_ERROR;
  int rc = expr_prog_eval (p, thr, &g_vd, NULL, NULL);
  if (rc != NO_ERROR)
    {
      return "ERR:" + std::to_string (g_errid);
    }
  std::string s;
  for (int i : idx)
    {
      s += show (expr_prog_value (p, i)) + " | ";
    }
  return s;
}

static EXPR_PROG *
compile_list (std::vector<REGU_VARIABLE *> &roots, std::vector<int> &idx, bool fallback)
{
  idx.assign (roots.size (), -1);
  reset_row ();
  return expr_prog_compile_roots (thr, roots.data (), (int) roots.size (), &g_vd, fallback, false, false, idx.data (),
				  NULL);
}

static std::string
log_str ()
{
  std::string s;
  for (const std::string & x:g_log)
    {
      s += x + " ";
    }
  return s;
}

TEST_CASE ("a list: hoisted literal steps move ahead of earlier columns, yet the jumps, the lazy regions and the "
	   "first error stay the interpreter's", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *n = D (DB_TYPE_NUMERIC), *b = D (DB_TYPE_BIGINT);
  DB_VALUE lit7 = v_int (7), lit3 = v_int (3);
  REGU_VARIABLE *c7 = new_regu (TYPE_DBVAL, i);
  c7->value.dbval = lit7;
  REGU_VARIABLE *c3 = new_regu (TYPE_DBVAL, i);
  c3->value.dbval = lit3;
  /* col1: L + R (lazy: R can fail) ; col2: n + 7 (the literal's coercion to NUMERIC is hoisted to the literal
   * prologue, ahead of col1's steps) ; col3: NVL (L, R2) ; col4: CAST (3 AS BIGINT) (a literal cast, fallible: stays
   * a row step behind col1's fallible ones) */
  std::vector<REGU_VARIABLE *> roots = {
    arith (T_ADD, i, sq ("L", i), sq ("R", i), number_plan (DB_TYPE_INTEGER, i)),
    arith (T_ADD, n, attr (1, n), c7, number_plan (DB_TYPE_NUMERIC, n)),
    arith (T_NVL, i, sq ("L2", i), sq ("R2", i), NULL),
    arith (T_CAST, b, NULL, c3, NULL),
  };
  std::vector<int> idx;
  EXPR_PROG *p = compile_list (roots, idx, true);
  REQUIRE (p != NULL);
  INFO ("steps: " << kernels_of (p) << " prologue " << p->n_prologue << " exec prologue " << p->n_exec_prologue);
  CHECK (p->n_prologue > 0);
  struct st
  {
    const char *what;
    bool fail;
    DB_VALUE v;
  };
  std::vector<st> states = { {"5", false, v_int (5)}, {"NULL", false, v_null ()}, {"fail", true, v_null ()} };
  std::vector<std::string> bad;
  int rows = 0;
  for (int row = 0; row < 2; row++)	/* the literal prologue runs on the first row only */
    {
      for (const st & l:states)
	for (const st & r:states)
	  for (const st & l2:states)
	    for (const st & r2:states)
	      {
		sq_val ("L", l.v), sq_val ("R", r.v), sq_val ("L2", l2.v), sq_val ("R2", r2.v);
		if (l.fail)
		  sq_fail ("L", E_LEFT);
		if (r.fail)
		  sq_fail ("R", E_RIGHT);
		if (l2.fail)
		  sq_fail ("L2", -9003);
		if (r2.fail)
		  sq_fail ("R2", -9004);
		g_attrs[1].v = v_num ("1.25");
		reset_row ();
		std::string a = interp_list (roots), al = log_str ();
		reset_row ();
		std::string bb = prog_list (p, idx), bl = log_str ();
		rows++;
		if (a != bb || al != bl)
		  {
		    bad.push_back (std::string ("L=") + l.what + " R=" + r.what + " L2=" + l2.what + " R2=" + r2.what +
				   "\n    interp: " + a + " {" + al + "}\n    program: " + bb + " {" + bl + "}");
		  }
	      }
    }
  INFO ("rows " << rows << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (bad.empty ());
  expr_prog_free (p);
  g_sqs.clear ();
}

/* ================================================================ executions: the gate's state (resolve_domains) */

/* What resolve_domains leaves for one execution (XASL_STATE.resolved_domain): the value block the rows read
 * (vd.dbval_ptr = vals: the binds, then the constant expressions' values at their ref), whether each constant was
 * evaluated, and the resolution of each late-binding item of the plan.  Built by hand with the fields the readers
 * touch: qexec_owns_resolved_index (plan, item inside plan->items, readable, n_resolved), qexec_late_bind_domain
 * (domains[resolved_index]), fetch_constant_evaluated (value_states[ref]), expr_item_resolution
 * (plan->resolved_non_cacheable). */
struct exec_env
{
  XASL_STATE xs;
  struct domain_plan plan;
  DOMAIN_PLAN_ITEM items[8];
  bool non_cacheable[8];
  RESOLVED_DOMAIN resolved[8];
  unsigned char states[16];
  DB_VALUE *vals = NULL;
  int n_binds = 0, n_vals = 0;
  QUERY_ID next_qid = 100;

  exec_env ()
  {
    std::memset ((void *) &xs, 0, sizeof (xs));
    std::memset ((void *) &plan, 0, sizeof (plan));
    std::memset ((void *) items, 0, sizeof (items));
    std::memset (non_cacheable, 0, sizeof (non_cacheable));
    std::memset ((void *) resolved, 0, sizeof (resolved));
    std::memset (states, 0, sizeof (states));
    plan.n_items = 8;
    plan.items = items;
    plan.n_resolved = 8;
    plan.resolved_non_cacheable = non_cacheable;
    for (DOMAIN_PLAN_ITEM & it:items)
      {
	it.resolved_index = -1;
	it.ref = -1;
	it.node_domain_index = -1;
	it.temporaries[0] = it.temporaries[1] = -1;
      }
  }

  /* a new execution: a fresh value block (the previous one is poisoned, as the gate frees it), its binds and the
   * constants' values and states; vd points at it like qexec_resolve_domains_internal leaves it */
  void begin (const std::vector<DB_VALUE> &binds, const std::vector<std::pair<DB_VALUE, int>> &consts)
  {
    if (vals != NULL)
      {
	for (int k = 0; k < n_vals; k++)
	  {
	    db_make_int (&vals[k], 0x5eed);	/* poison: a stale read shows as INT:24301 */
	  }
      }
    n_binds = (int) binds.size ();
    n_vals = n_binds + (int) consts.size ();
    vals = new DB_VALUE[n_vals];
    for (int k = 0; k < n_binds; k++)
      {
	vals[k] = binds[k];
      }
    for (size_t k = 0; k < consts.size (); k++)
      {
	vals[n_binds + k] = consts[k].first;
	states[n_binds + k] = (unsigned char) consts[k].second;
      }
    xs.query_id = next_qid++;
    xs.resolved_domain.vals = vals;
    xs.resolved_domain.n_vals = n_vals;
    xs.resolved_domain.value_states = states;
    xs.resolved_domain.domains = resolved;
    xs.resolved_domain.n_resolved = 8;
    xs.resolved_domain.plan = &plan;
    xs.resolved_domain.readable = true;
    g_vd.dbval_ptr = vals;
    g_vd.dbval_cnt = n_binds;
    g_vd.xasl_state = &xs;
  }
};

static void
leave_exec_env ()
{
  g_vd.dbval_ptr = NULL;
  g_vd.dbval_cnt = 0;
  g_vd.xasl_state = NULL;
}

static REGU_VARIABLE *
hv (int pos, TP_DOMAIN * dom)
{
  REGU_VARIABLE *r = new_regu (TYPE_POS_VALUE, dom);
  r->value.val_pos = pos;
  return r;
}

/* the consumer protocol around a program kept with the clone (qdata_valptr_prog_ensure): verify the bind signature
 * and refill the open steps once per execution, recompile on a mismatch; then evaluate the row */
static std::string
consumer_row (EXPR_PROG ** p, int *idx, REGU_VARIABLE * root, int *recompiles)
{
  if (*p != NULL && !expr_prog_signature_ok (*p, &g_vd, EXPR_PROG_EXEC_STAMP (&g_vd)))
    {
      expr_prog_free (*p);
      *p = NULL;
      (*recompiles)++;
    }
  if (*p == NULL)
    {
      *p = compile1 (root, idx);
      if (*p == NULL)
	{
	  return "(declined)";
	}
    }
  reset_row ();
  return run_prog (*p, *idx).value;
}

TEST_CASE ("a constant expression the gate evaluated (plan_item->ref): the program serves the gate's value or "
	   "computes it, execution after execution, never a freed value block (review 2026-10-15 #1)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *n = D (DB_TYPE_NUMERIC);
  struct shape
  {
    const char *what;
    TP_DOMAIN *bind_dom;
    DB_TYPE plan_type;
    TP_DOMAIN *node_dom;
  };
  shape shapes[] = {
    {"?(INT) + 1", i, DB_TYPE_INTEGER, i},
    {"?(NUMERIC) + 1", n, DB_TYPE_NUMERIC, n},
  };
  for (shape & sh:shapes)
    {
      exec_env env;
      DOMAIN_PLAN_ITEM *item = &env.items[0];
      item->ref = 1;		/* vals[0] the bind, vals[1] the constant's value */
      item->fixed.domain = sh.node_dom;
      item->fixed.arith.kind = DOMAIN_ARITH_NUMBER;
      item->fixed.arith.type = sh.plan_type;
      REGU_VARIABLE *c1 = new_regu (TYPE_DBVAL, i);
      c1->value.dbval = v_int (1);
      REGU_VARIABLE *root = arith (T_ADD, sh.node_dom, hv (0, sh.bind_dom), c1, item);
      EXPR_PROG *p = NULL;
      int idx = -1, recompiles = 0;
      std::vector<std::string> bad;
      /* bind b, the gate's state of b + 1: evaluated, evaluated, pending (the rows compute it), evaluated */
      const int binds[] = { 1, 10, 100, 1000 };
      const int states[] = { DOMAIN_VALUE_EVALUATED, DOMAIN_VALUE_EVALUATED, DOMAIN_VALUE_PENDING,
	DOMAIN_VALUE_EVALUATED
      };
      for (int e = 0; e < 4; e++)
	{
	  DB_VALUE b = (sh.plan_type == DB_TYPE_INTEGER) ? v_int (binds[e]) : v_num (std::to_string (binds[e]).c_str ());
	  DB_VALUE c;
	  /* the gate's value is the node's own computation (resolve_domains evaluates it with the same operator) */
	  if (sh.plan_type == DB_TYPE_INTEGER)
	    {
	      c = v_int (binds[e] + 1);
	    }
	  else
	    {
	      DB_VALUE one = v_int (1), onen;
	      DB_DATA_STATUS ds;
	      db_value_domain_init (&onen, DB_TYPE_NUMERIC, DB_DEFAULT_PRECISION, DB_DEFAULT_SCALE);
	      numeric_db_value_coerce_to_num (&one, &onen, &ds);
	      db_make_null (&c);
	      numeric_db_value_add (&b, &onen, &c);
	    }
	  env.begin ({b}, {{c, states[e]}});
	  for (int row = 0; row < 2; row++)
	    {
	      reset_row ();
	      outcome a = run_interp (root);
	      std::string pv = consumer_row (&p, &idx, root, &recompiles);
	      if (a.value != pv)
		{
		  bad.push_back (std::string (sh.what) + " execution " + std::to_string (e + 1) + " row " +
				 std::to_string (row + 1) + "\n    interp: " + a.value + "\n    program: " + pv);
		}
	    }
	  if (p != NULL)
	    {
	      expr_prog_reset (p);	/* the end of the execution (qexec_clear_xasl, is_final) */
	    }
	}
      INFO ("steps: " << (p ? kernels_of (p) : std::string ("-")) << (bad.empty ()? "" : "\n" + bad[0]));
      CHECK (bad.empty ());
      CHECK (recompiles == 0);
      if (p != NULL)
	{
	  expr_prog_free (p);
	}
      leave_exec_env ();
    }
}

TEST_CASE ("a node whose domain the gate resolves (open step): a new resolution of the same type refills the "
	   "kernel's domain, another type makes the consumer recompile -- each execution == interpreter", "[contract]")
{
  TP_DOMAIN *n = D (DB_TYPE_NUMERIC), *var = D (DB_TYPE_VARIABLE);
  REQUIRE (var != NULL);
  exec_env env;
  DOMAIN_PLAN_ITEM *item = &env.items[0];
  item->flags = DOMAIN_PLAN_LATE_BIND;
  item->resolved_index = 0;
  item->fixed.domain = var;
  item->fixed.arith.kind = DOMAIN_ARITH_UNRESOLVED;
  REGU_VARIABLE *root = arith (T_MUL, var, hv (0, n), hv (1, n), item);
  struct exe
  {
    const char *a, *b;
    TP_DOMAIN *res;
  };
  /* the same bind types every time (NUMERIC), the gate's resolution moving: precision/scale, then a narrow one the
   * product overflows, then back */
  exe exes[] = {
    {"1.5", "2.25", d_num (10, 3)},
    {"123.456", "0.5", d_num (20, 6)},
    {"99999.5", "99999.5", d_num (8, 2)},
    {"1.5", "2.25", d_num (10, 3)},
  };
  EXPR_PROG *p = NULL;
  int idx = -1, recompiles = 0;
  std::vector<std::string> bad, seen;
  std::string steps;
  for (exe & x:exes)
    {
      env.resolved[0].domain = x.res;
      env.resolved[0].arith.kind = DOMAIN_ARITH_NUMBER;
      env.resolved[0].arith.type = DB_TYPE_NUMERIC;
      env.begin ({v_num (x.a), v_num (x.b)}, {});
      reset_row ();
      outcome a = run_interp (root);
      std::string pv = consumer_row (&p, &idx, root, &recompiles);
      if (p != NULL && steps.empty ())
	{
	  steps = kernels_of (p);
	}
      seen.push_back (a.value);
      if (a.value != pv)
	{
	  bad.push_back (std::string (x.a) + " * " + x.b + " resolved NUMERIC(" + std::to_string (x.res->precision) +
			 "," + std::to_string (x.res->scale) + ")\n    interp: " + a.value + "\n    program: " + pv);
	}
      if (p != NULL)
	{
	  expr_prog_reset (p);
	}
    }
  INFO ("steps: " << steps << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (steps.find ("mul_numeric") != std::string::npos);
  /* the resolutions did reach the rows: each execution's value has its own scale, the narrow one overflows */
  REQUIRE (seen.size () == 4);
  CHECK (seen[0].find ("(10,3)") != std::string::npos);
  CHECK (seen[1].find ("(20,6)") != std::string::npos);
  CHECK (seen[2].compare (0, 4, "ERR:") == 0);
  CHECK (seen[3] == seen[0]);
  CHECK (bad.empty ());
  CHECK (recompiles == 0);

  /* the same binds, the resolution's type changes (NUMERIC -> DOUBLE): refill reports it, the consumer recompiles */
  env.resolved[0].domain = D (DB_TYPE_DOUBLE);
  env.resolved[0].arith.type = DB_TYPE_DOUBLE;
  env.begin ({v_num ("1.5"), v_num ("2.25")}, {});
  reset_row ();
  outcome a = run_interp (root);
  std::string pv = consumer_row (&p, &idx, root, &recompiles);
  CHECK (recompiles == 1);
  INFO ("interp " << a.value << " program " << pv);
  CHECK (a.value == pv);
  if (p != NULL)
    {
      expr_prog_free (p);
    }
  leave_exec_env ();
}

TEST_CASE ("a gate constant whose node stays a row step (a fallible column precedes it): the resolved_value check "
	   "serves the gate's value or lets the node compute, over executions", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  exec_env env;
  DOMAIN_PLAN_ITEM *item = &env.items[0];
  item->ref = 1;
  item->fixed.domain = i;
  item->fixed.arith.kind = DOMAIN_ARITH_NUMBER;
  item->fixed.arith.type = DB_TYPE_INTEGER;
  REGU_VARIABLE *c1 = new_regu (TYPE_DBVAL, i);
  c1->value.dbval = v_int (1);
  std::vector<REGU_VARIABLE *> roots = { sq ("S", i), arith (T_ADD, i, hv (0, i), c1, item) };
  std::vector<int> idx;
  EXPR_PROG *p = NULL;
  std::vector<std::string> bad;
  const int binds[] = { 1, 10, 100, 1000, INT_MAX };
  const int states[] = { DOMAIN_VALUE_EVALUATED, DOMAIN_VALUE_EVALUATED, DOMAIN_VALUE_PENDING,
    DOMAIN_VALUE_EVALUATED, DOMAIN_VALUE_PENDING
  };
  std::string steps;
  for (int e = 0; e < 5; e++)
    {
      DB_VALUE c = (binds[e] == INT_MAX) ? v_null () : v_int (binds[e] + 1);
      env.begin ({v_int (binds[e])}, {{c, states[e]}});
      sq_val ("S", v_int (e));
      if (p == NULL)
	{
	  p = compile_list (roots, idx, true);
	  REQUIRE (p != NULL);
	  steps = kernels_of (p);
	}
      else
	{
	  REQUIRE (expr_prog_signature_ok (p, &g_vd, EXPR_PROG_EXEC_STAMP (&g_vd)));
	}
      for (int row = 0; row < 2; row++)
	{
	  reset_row ();
	  std::string a = interp_list (roots);
	  reset_row ();
	  std::string b = prog_list (p, idx);
	  if (a != b)
	    {
	      bad.push_back ("execution " + std::to_string (e + 1) + " row " + std::to_string (row + 1) + "\n    interp: "
			     + a + "\n    program: " + b);
	    }
	}
      expr_prog_reset (p);
    }
  INFO ("steps: " << steps << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (steps.find ("resolved_value") != std::string::npos);
  CHECK (bad.empty ());
  expr_prog_free (p);
  g_sqs.clear ();
  leave_exec_env ();
}

/* ================================================================ random lists (CSE, commuted CSE, guards, hoisting) */

#include <random>
#include <functional>

/* A list of 1..4 INTEGER expressions over shared columns (TYPE_CONSTANT leaves: a list scan's columns point at one
 * value per column, so two occurrences are one cell and their subtrees one chain -- the CSE the program does),
 * scalar subqueries (fallible, observable), literals, host variables; the operators +, -, *, / (kernels, lazy
 * regions where the right side can fail) and NVL.  Structurally equal subtrees recur on purpose (a + b next to
 * b + a, the same subtree under another left operand). */
struct fuzz_env
{
  std::mt19937 rng;
  bool with_case = false;
  DB_VALUE cols[4];
  std::vector<REGU_VARIABLE *> col_leaves;
  explicit fuzz_env (unsigned seed) : rng (seed)
  {
  }
  int pick (int n)
  {
    return (int) (rng () % (unsigned) n);
  }
};

static REGU_VARIABLE *
fuzz_tree (fuzz_env & f, int depth, std::vector<REGU_VARIABLE *> &pool)
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  if (!pool.empty () && f.pick (5) == 0)
    {
      return pool[f.pick ((int) pool.size ())];	/* the same subtree again (shared regu) */
    }
  if (depth == 0 || f.pick (3) == 0)
    {
      switch (f.pick (4))
	{
	case 0:
	case 1:
	  {
	    REGU_VARIABLE *c = new_regu (TYPE_CONSTANT, i);
	    c->value.dbvalptr = &f.cols[f.pick (4)];
	    return c;
	  }
	case 2:
	  {
	    static const char *names[] = { "S0", "S1" };
	    const char *n = names[f.pick (2)];
	    return sq (n, i);
	  }
	default:
	  {
	    static const int lits[] = { 0, 1, 2, -1, 7, INT_MAX };
	    REGU_VARIABLE *c = new_regu (TYPE_DBVAL, i);
	    c->value.dbval = v_int (lits[f.pick (6)]);
	    return c;
	  }
	}
    }
  REGU_VARIABLE *l = fuzz_tree (f, depth - 1, pool);
  REGU_VARIABLE *r = fuzz_tree (f, depth - 1, pool);
  REGU_VARIABLE *n;
  const int k = f.pick (f.with_case ? 9 : 6);
  if (k == 5)
    {
      n = arith (T_NVL, i, l, r, NULL);
    }
  else if (k >= 6)
    {
      /* CASE WHEN <comparison(s) over two more subtrees> THEN l ELSE r END, or the predicate as a value */
      using namespace cubxasl;
      auto comp =[&](void)->pred_expr *
      {
	pred_expr *c = (pred_expr *) calloc (1, sizeof (pred_expr));
	static const REL_OP rels[] = { R_EQ, R_NE, R_GT, R_LT, R_GE, R_LE };
	c->type = T_EVAL_TERM;
	c->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
	c->pe.m_eval_term.et.et_comp.lhs = fuzz_tree (f, depth - 1, pool);
	c->pe.m_eval_term.et.et_comp.rhs = fuzz_tree (f, depth - 1, pool);
	c->pe.m_eval_term.et.et_comp.rel_op = rels[f.pick (6)];
	c->pe.m_eval_term.et.et_comp.type = DB_TYPE_INTEGER;
	return c;
      };
      pred_expr *pr = comp ();
      if (f.pick (3) == 0)
	{
	  pred_expr *both = (pred_expr *) calloc (1, sizeof (pred_expr));
	  both->type = T_PRED;
	  both->pe.m_pred.lhs = pr;
	  both->pe.m_pred.rhs = comp ();
	  both->pe.m_pred.bool_op = f.pick (2) ? B_AND : B_OR;
	  pr = both;
	}
      n = arith (k == 8 ? T_PREDICATE : T_CASE, i, k == 8 ? NULL : l, k == 8 ? NULL : r, NULL);
      n->value.arithptr->pred = pr;
    }
  else
    {
      const OPERATOR_TYPE ops[] = { T_ADD, T_SUB, T_MUL, T_DIV, T_ADD };
      n = arith (ops[k], i, l, r, number_plan (DB_TYPE_INTEGER, i));
    }
  pool.push_back (n);
  if (f.pick (4) == 0 && (n->value.arithptr->opcode == T_ADD || n->value.arithptr->opcode == T_MUL))
    {
      /* the operand-swapped twin of this node, a separate node */
      REGU_VARIABLE *twin = arith (n->value.arithptr->opcode, i, r, l, number_plan (DB_TYPE_INTEGER, i));
      pool.push_back (twin);
    }
  return n;
}

static std::string
tree_str (const REGU_VARIABLE * r, const fuzz_env & f)
{
  char buf[64];
  switch (r->type)
    {
    case TYPE_CONSTANT:
      if (r->xasl != NULL)
	{
	  for (const sq_leaf & q:g_sqs)
	    if (q.xasl == r->xasl)
	      return q.name;
	  return "S?";
	}
      std::snprintf (buf, sizeof (buf), "c%d", (int) (r->value.dbvalptr - f.cols));
      return buf;
    case TYPE_DBVAL:
      return show (&r->value.dbval).substr (4);
    case TYPE_INARITH:
      {
	const ARITH_TYPE *a = r->value.arithptr;
	if (a->opcode == T_NVL)
	  return "NVL(" + tree_str (a->leftptr, f) + ", " + tree_str (a->rightptr, f) + ")";
	if (a->opcode == T_CASE || a->opcode == T_PREDICATE)
	  {
	    std::function < std::string (const cubxasl::pred_expr *) > ps =[&](const cubxasl::pred_expr * p)
	    {
	      if (p->type == T_PRED)
		return "(" + ps (p->pe.m_pred.lhs) + (p->pe.m_pred.bool_op == B_AND ? " AND " : " OR ") +
		  ps (p->pe.m_pred.rhs) + ")";
	      const auto & c = p->pe.m_eval_term.et.et_comp;
	      static const char *rn[] = { "?", "=", "<>", ">", ">=", "<", "<=" };
	      return tree_str (c.lhs, f) + " " + ((int) c.rel_op < 7 ? rn[(int) c.rel_op] : "rel") + " " +
		tree_str (c.rhs, f);
	    };
	    if (a->opcode == T_PREDICATE)
	      return "PRED(" + ps (a->pred) + ")";
	    return "CASE WHEN " + ps (a->pred) + " THEN " + tree_str (a->leftptr, f) + " ELSE " + tree_str (a->rightptr, f)
	      + " END";
	  }
	return "(" + tree_str (a->leftptr, f) + " " + op_name (a->opcode) + " " + tree_str (a->rightptr, f) + ")";
      }
    default:
      return "?";
    }
}

static std::string
row_str (const fuzz_env & f)
{
  std::string s;
  for (int c = 0; c < 4; c++)
    s += "c" + std::to_string (c) + "=" + show (&f.cols[c]) + " ";
  for (const sq_leaf & q:g_sqs)
    s += q.name + "=" + (q.fail ? std::string ("fail") : show (&q.value)) + " ";
  return s;
}

TEST_CASE ("random lists over shared columns, subqueries and literals: the program's values and first error are "
	   "the interpreter's on every row (CSE, commuted CSE, lazy guards, hoisting, CASE branches)", "[contract]")
{
  std::vector<std::string> bad;
  int lists = 0, rows = 0, compiled = 0;
  int with_case = 0, with_pred = 0, with_fallback = 0, with_lazy = 0, with_generic = 0;
  static const int colvals[] = { 0, 1, -1, 2, 3, INT_MAX, INT_MIN };
  for (unsigned seed = 1; seed <= 2000; seed++)
    {
      fuzz_env f (seed);
      f.with_case = (seed > 400);	/* after the first 400 lists, CASE / predicate-as-value nodes too */
      g_sqs.clear ();
      std::vector<REGU_VARIABLE *> pool;
      std::vector<REGU_VARIABLE *> roots;
      const int nroots = 1 + f.pick (4);
      for (int k = 0; k < nroots; k++)
	{
	  roots.push_back (fuzz_tree (f, 1 + f.pick (3), pool));
	}
      /* subquery leaves may not exist in this list; sq_val needs them */
      for (const char *n : { "S0", "S1" })
	{
	  if (sq_find (n) == NULL)
	    {
	      (void) sq (n, D (DB_TYPE_INTEGER));
	    }
	}
      std::vector<int> idx;
      EXPR_PROG *p = compile_list (roots, idx, true);
      lists++;
      if (p == NULL)
	{
	  continue;
	}
      compiled++;
      {
	const std::string ks = kernels_of (p);
	with_case += (ks.find ("case_branch") != std::string::npos);
	with_pred += (ks.find ("predicate") != std::string::npos);
	with_fallback += (ks.find ("fallback") != std::string::npos);
	with_lazy += (ks.find ("jump_null") != std::string::npos || ks.find ("jump_notnull") != std::string::npos);
	with_generic += (ks.find ("generic") != std::string::npos);
      }
      for (int r = 0; r < 24; r++)
	{
	  for (int c = 0; c < 4; c++)
	    {
	      f.cols[c] = (f.pick (6) == 0) ? v_null () : v_int (colvals[f.pick (7)]);
	    }
	  for (const char *n : { "S0", "S1" })
	    {
	      const int st = f.pick (6);
	      sq_val (n, st == 0 ? v_null () : v_int (colvals[f.pick (7)]));
	      if (st == 1)
		{
		  sq_fail (n, n[1] == '0' ? -9500 : -9501);
		}
	    }
	  reset_row ();
	  std::string a = interp_list (roots), al = log_str ();
	  reset_row ();
	  std::string b = prog_list (p, idx), bl = log_str ();
	  if (b.compare (0, 4, "ERR:") == 0)
	    {
	      /* an error ends the execution: qexec_execute_query's error path clears the XASL (is_final), which resets
	       * the program (expr_prog_reset) -- what a consumer does; re-evaluating a failed program inside the same
	       * execution is PR7658 F2 below */
	      expr_prog_reset (p);
	    }
	  rows++;
	  if (a != b || al != bl)
	    {
	      std::string list;
	      for (REGU_VARIABLE * rt:roots)
		list += tree_str (rt, f) + " | ";
	      bad.push_back ("seed " + std::to_string (seed) + " row " + std::to_string (r) + "\n    list: " + list +
			     "\n    row: " + row_str (f) + "\n    interp: " + a + " {" + al + "}\n    program: " + b +
			     " {" + bl + "}\n    steps: " + kernels_of (p) + " prologue " +
			     std::to_string (p->n_prologue));
	    }
	}
      expr_prog_free (p);
    }
  g_sqs.clear ();
  std::string first;
  std::string last_seed;
  for (size_t k = 0; k < bad.size (); k++)
    {
      const std::string seed_of = bad[k].substr (0, bad[k].find (" row"));
      if (seed_of != last_seed)
	{
	  first += "\n" + bad[k];	/* the first mismatching row of each list */
	  last_seed = seed_of;
	}
    }
  INFO ("lists " << lists << " (compiled " << compiled << "), rows " << rows << ", mismatches " << bad.size () <<
	"; programs with case_branch " << with_case << ", predicate " << with_pred << ", lazy jumps " << with_lazy <<
	", generic " << with_generic << ", fallback " << with_fallback << first);
  WARN ("coverage: case_branch " << with_case << ", predicate " << with_pred << ", lazy " << with_lazy << ", generic "
	<< with_generic << ", fallback " << with_fallback << " of " << compiled << " programs");
  CHECK (with_case > 200);
  CHECK (with_lazy > 200);
  CHECK (compiled > 1500);
  CHECK (bad.empty ());
}

/* The same over NUMERIC and INTEGER together: columns, subqueries and literals of both types, a node NUMERIC when
 * an operand is (the integer side coerced: coerce_numeric, the plain kernel family), the parser's CAST_WRAP widening
 * an INTEGER subtree to NUMERIC, NVL and CASE over mixed branches (the generic step / the casting publisher). */
struct typed
{
  REGU_VARIABLE *r;
  bool num;
};

static typed
fuzz_num_tree (fuzz_env & f, int depth, DB_VALUE * cols)
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *n = D (DB_TYPE_NUMERIC);
  if (depth == 0 || f.pick (3) == 0)
    {
      switch (f.pick (5))
	{
	case 0:
	case 1:
	  {
	    const int c = f.pick (4);
	    REGU_VARIABLE *leaf = new_regu (TYPE_CONSTANT, c < 2 ? n : i);
	    leaf->value.dbvalptr = &cols[c];
	    return { leaf, c < 2 };
	  }
	case 2:
	  {
	    const bool num = f.pick (2) == 0;
	    return { sq (num ? "S0" : "S1", num ? n : i), num };
	  }
	case 3:
	  {
	    static const char *nl[] = { "1.5", "0", "-2.25", "0.1", "9999999999999999999999999999999999999999", "7" };
	    REGU_VARIABLE *c = new_regu (TYPE_DBVAL, n);
	    c->value.dbval = v_num (nl[f.pick (6)]);
	    return { c, true };
	  }
	default:
	  {
	    static const int il[] = { 0, 1, 2, -1, 7, INT_MAX };
	    REGU_VARIABLE *c = new_regu (TYPE_DBVAL, i);
	    c->value.dbval = v_int (il[f.pick (6)]);
	    return { c, false };
	  }
	}
    }
  typed l = fuzz_num_tree (f, depth - 1, cols);
  typed r = fuzz_num_tree (f, depth - 1, cols);
  const int k = f.pick (8);
  if (k == 5 && !l.num)
    {
      /* the parser's widening of an INTEGER operand: CAST_WRAP (x AS NUMERIC) */
      return { arith (T_CAST_WRAP, n, NULL, l.r, NULL), true };
    }
  const bool num = l.num || r.num;
  if (k == 6)
    {
      return { arith (T_NVL, num ? n : i, l.r, r.r, NULL), num };
    }
  if (k == 7)
    {
      using namespace cubxasl;
      typed a = fuzz_num_tree (f, depth - 1, cols);
      REGU_VARIABLE *lit = new_regu (TYPE_DBVAL, a.num ? n : i);
      lit->value.dbval = a.num ? v_num ("1.5") : v_int (1);
      pred_expr *c = (pred_expr *) calloc (1, sizeof (pred_expr));
      static const REL_OP rels[] = { R_EQ, R_NE, R_GT, R_LT, R_GE, R_LE };
      c->type = T_EVAL_TERM;
      c->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
      c->pe.m_eval_term.et.et_comp.lhs = a.r;
      c->pe.m_eval_term.et.et_comp.rhs = lit;
      c->pe.m_eval_term.et.et_comp.rel_op = rels[f.pick (6)];
      c->pe.m_eval_term.et.et_comp.type = a.num ? DB_TYPE_NUMERIC : DB_TYPE_INTEGER;
      REGU_VARIABLE *cs = arith (T_CASE, num ? n : i, l.r, r.r, NULL);
      cs->value.arithptr->pred = c;
      return { cs, num };
    }
  const OPERATOR_TYPE ops[] = { T_ADD, T_SUB, T_MUL, T_DIV, T_MUL };
  return { arith (ops[k % 5], num ? n : i, l.r, r.r, number_plan (num ? DB_TYPE_NUMERIC : DB_TYPE_INTEGER,
								   num ? n : i)), num };
}

TEST_CASE ("random lists over NUMERIC and INTEGER together (coercions, plain/float families, CAST_WRAP widening, "
	   "NVL and CASE over mixed branches): program == interpreter on every row", "[contract]")
{
  std::vector<std::string> bad;
  int compiled = 0, rows = 0, with_coerce = 0, with_cast = 0;
  static const char *numvals[] = { "1.5", "-2.25", "0", "0.000001", "9999999999999999999999999999999999999999",
    "-9999999999999999999999999999999999999999", "123456789012345678901234567890.0123456789", "7"
  };
  static const int intvals[] = { 0, 1, -1, 2, 3, INT_MAX, INT_MIN };
  for (unsigned seed = 1; seed <= 1500; seed++)
    {
      fuzz_env f (seed * 7919u);
      DB_VALUE cols[4];
      g_sqs.clear ();
      std::vector<REGU_VARIABLE *> roots;
      const int nroots = 1 + f.pick (3);
      for (int k = 0; k < nroots; k++)
	{
	  roots.push_back (fuzz_num_tree (f, 1 + f.pick (3), cols).r);
	}
      if (sq_find ("S0") == NULL)
	(void) sq ("S0", D (DB_TYPE_NUMERIC));
      if (sq_find ("S1") == NULL)
	(void) sq ("S1", D (DB_TYPE_INTEGER));
      std::vector<int> idx;
      EXPR_PROG *p = compile_list (roots, idx, true);
      if (p == NULL)
	{
	  continue;
	}
      compiled++;
      const std::string ks = kernels_of (p);
      with_coerce += (ks.find ("coerce_numeric") != std::string::npos);
      with_cast += (ks.find ("cast") != std::string::npos);
      for (int r = 0; r < 16; r++)
	{
	  for (int c = 0; c < 4; c++)
	    {
	      const bool null = f.pick (6) == 0;
	      cols[c] = null ? v_null () : (c < 2 ? v_num (numvals[f.pick (8)]) : v_int (intvals[f.pick (7)]));
	    }
	  const int s0 = f.pick (6), s1 = f.pick (6);
	  sq_val ("S0", s0 == 0 ? v_null () : v_num (numvals[f.pick (8)]));
	  if (s0 == 1)
	    sq_fail ("S0", -9600);
	  sq_val ("S1", s1 == 0 ? v_null () : v_int (intvals[f.pick (7)]));
	  if (s1 == 1)
	    sq_fail ("S1", -9601);
	  reset_row ();
	  std::string a = interp_list (roots), al = log_str ();
	  reset_row ();
	  std::string b = prog_list (p, idx), bl = log_str ();
	  if (b.compare (0, 4, "ERR:") == 0)
	    {
	      expr_prog_reset (p);	/* the execution ends with the error (see the INTEGER fuzz) */
	    }
	  rows++;
	  if (a != b || al != bl)
	    {
	      bad.push_back ("seed " + std::to_string (seed) + " row " + std::to_string (r) + "\n    interp: " + a + " {" +
			     al + "}\n    program: " + b + " {" + bl + "}\n    steps: " + ks);
	    }
	}
      expr_prog_free (p);
    }
  g_sqs.clear ();
  INFO ("compiled " << compiled << ", rows " << rows << ", with coerce_numeric " << with_coerce << ", with cast " <<
	with_cast << ", mismatches " << bad.size () << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (compiled > 1000);
  CHECK (with_coerce > 100);
  CHECK (bad.empty ());
}

/* ================================================================ PR7658 F2 (latent): a prologue that failed */

TEST_CASE ("PR7658 F2 (latent): a literal-prologue step that fails is marked done before it runs -- evaluated again "
	   "in the same execution (or with no execution identity) the program skips it and returns NULL where the "
	   "interpreter raises the error again (current behaviour, PASS = defect present)", "[defect]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  REGU_VARIABLE *c = lit_int_ak (INT_MAX), *d = lit_int_ak (7);
  std::vector<REGU_VARIABLE *> roots = { arith (T_ADD, i, c, d, number_plan (DB_TYPE_INTEGER, i)) };
  std::vector<int> idx;
  EXPR_PROG *p = compile_list (roots, idx, false);
  REQUIRE (p != NULL);
  CHECK (p->n_prologue == 1);
  const std::string overflow = "ERR:" + std::to_string (ER_QPROC_OVERFLOW_ADDITION);
  reset_row ();
  CHECK (interp_list (roots) == overflow);
  reset_row ();
  const std::string first = prog_list (p, idx);
  reset_row ();
  const std::string second = prog_list (p, idx);
  reset_row ();
  const std::string again = interp_list (roots);
  INFO ("program: first " << first << ", second " << second << "; interpreter again " << again);
  CHECK (first == overflow);
  CHECK (again == overflow);
  CHECK (second == "NULL | ");	/* the defect: the failed prologue is skipped */
  expr_prog_reset (p);		/* the end of the execution re-arms it */
  reset_row ();
  CHECK (prog_list (p, idx) == overflow);
  expr_prog_free (p);
}

TEST_CASE ("PR7658 F2 (latent): the same for the host-variable prologue -- ? + 1 with ? = INT_MAX fails once, then "
	   "the rest of the execution reads NULL (current behaviour, PASS = defect present)", "[defect]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  exec_env env;
  REGU_VARIABLE *one = lit_int_ak (1);
  std::vector<REGU_VARIABLE *> roots = { arith (T_ADD, i, hv (0, i), one, number_plan (DB_TYPE_INTEGER, i)) };
  env.begin ({v_int (INT_MAX)}, {});
  std::vector<int> idx;
  EXPR_PROG *p = compile_list (roots, idx, false);
  REQUIRE (p != NULL);
  CHECK (p->n_exec_prologue >= 1);
  const std::string overflow = "ERR:" + std::to_string (ER_QPROC_OVERFLOW_ADDITION);
  reset_row ();
  const std::string first = prog_list (p, idx);
  reset_row ();
  const std::string second = prog_list (p, idx);
  reset_row ();
  const std::string interp = interp_list (roots);
  INFO ("program: first " << first << ", second " << second << "; interpreter " << interp);
  CHECK (first == overflow);
  CHECK (interp == overflow);
  CHECK (second == "NULL | ");
  expr_prog_free (p);
  leave_exec_env ();
}

TEST_CASE ("a failed prologue and the end of the execution: after expr_prog_reset the next execution raises the "
	   "same error (the consumer's protocol, qexec_clear_xasl on the error path)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<REGU_VARIABLE *> roots = { arith (T_ADD, i, lit_int_ak (INT_MAX), lit_int_ak (7),
						number_plan (DB_TYPE_INTEGER, i)) };
  std::vector<int> idx;
  EXPR_PROG *p = compile_list (roots, idx, false);
  REQUIRE (p != NULL);
  const std::string overflow = "ERR:" + std::to_string (ER_QPROC_OVERFLOW_ADDITION);
  for (int e = 0; e < 3; e++)
    {
      reset_row ();
      CHECK (prog_list (p, idx) == overflow);
      expr_prog_reset (p);
    }
  expr_prog_free (p);
}
