/*
 * PR#7658 (CBRD-27215) -- the generic step's operand protocol (src/query/expr_compile.c, server mode) against the
 * interpreter's fetch switch (fetch_peek_arith in fetch.c), in one binary.
 *
 * An operator the kernel table does not cover is a generic step: its operands are compiled in the order
 * expr_generic_operand_spec gives ("L?Rt", "LR!d", "^" ...) and the step runs the opcode's body inside the node
 * protocol.  fetch_arith_ops.c is NOT linked: fetch_arith_op () returns one recording body to both paths, so what
 * is compared is the protocol -- which operands are fetched (scalar subquery leaves, logged when executed), in which
 * order, under which condition (the '?' lazy skip), which error a fetch failure becomes (the try step for a
 * CAST-wrapped operand of a date/time function), which operands the body is handed and at what recursion depth.
 *
 *   sweep       every opcode 0..T_COLLECTION_TO_STRING the step takes, every operand shape (L/R/T present or not),
 *               every {value, NULL, failure} assignment; again under compat_mode=mysql and
 *               oracle_style_empty_string=yes; T_ADD/T_STRCAT/T_CONCAT over a string node
 *   cast-wrap   the same with each operand CAST_WRAP (subquery), and a cast that fails on its own value
 *   WIDTH_BUCKET left, the two bounds (a BETWEEN predicate's right sides), then the count (review 2026-10-15 #5)
 *   recursion   max_recursion_sql_depth for kernel chains, lazy right spines, generic chains, outer frames, a
 *               skipped deep region, the exact boundary; trees past the 128-step tables (nested programs)
 *   lists       a later column's hoisted literal step ahead of a generic '?' node
 *   F1          [defect] right-nested chains k + (k+1 + (... + c)): declined at 127+ levels, compile time exponential
 *               in depth (1.7 s at 300, 312 s at 399 measured)
 *
 * Real code linked in (suite.json extra): numeric_opfunc.c, query_opfunc.c, object_domain.c,
 * object_domain_convert.cpp, fetch.c, object_primitive.c, db_macro.c, chartype.c, intl_support.c.
 * Faked: fetch_arith_op (the recording body, with three bodies mirrored -- see below), er_set/er_errid/er_clear,
 * prm_Def, the thread entry, heap_attrinfo_access, qexec_execute_mainblock, area_alloc/area_free, lang_Collations.
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

/* ---- the operator bodies (fetch_arith_ops.c is NOT linked): fetch_arith_op () hands both paths -- the interpreter's
 * fetch_peek_arith () and the program's generic step -- the same recording body, so what is compared is the operand
 * protocol around it (which operands are fetched, in which order, under which condition, which error a fetch failure
 * becomes, how many frames the body sees), not what an operator computes.  It logs the operands it was handed
 * ('-' not fetched, 'n' NULL, 'v' a value) and the thread's recursion depth, and answers the number of values.
 * Three bodies mirror the real ones because the program serves those nodes with a kernel instead of a body:
 *   T_CAST / T_CAST_WRAP: fetch_op_cast's non-collation branch -- tp_value_cast_force into the node's domain
 *     (tp_value_cast for a STRICT_TYPE_CAST-flagged T_CAST_WRAP), the domain error set on failure;
 *   T_ADD..T_DIV with a plan item: fetch_arith_binary's direct path -- qdata_arith_dbval over the plan's arith;
 *   T_INCR / T_DECR: FETCH_ARITH_RAW_LEFT (the node's value is its left operand). */
static int g_body_fail = NO_ERROR;

static char
opnd_mark (const DB_VALUE * v)
{
  return v == NULL ? '-' : DB_IS_NULL (v) ? 'n' : 'v';
}

static int
fake_body (THREAD_ENTRY * thread_p, FETCH_ARITH_ARGS * a)
{
  const OPERATOR_TYPE op = a->arithptr->opcode;

  if (op == T_CAST || op == T_CAST_WRAP)
    {
      const bool force = !REGU_VARIABLE_IS_FLAGED (a->regu_var, REGU_VARIABLE_STRICT_TYPE_CAST) || op != T_CAST_WRAP;
      TP_DOMAIN_STATUS st = force ? tp_value_cast_force (a->peek_right, a->arithptr->value, a->arith_domain, false)
	: tp_value_cast (a->peek_right, a->arithptr->value, a->arith_domain, false);
      if (st != DOMAIN_COMPATIBLE)
	{
	  (void) tp_domain_status_er_set (st, ARG_FILE_LINE, a->peek_right, a->arith_domain);
	  return ER_FAILED;
	}
      return NO_ERROR;
    }
  if ((op == T_ADD || op == T_SUB || op == T_MUL || op == T_DIV) && a->arithptr->plan_item != NULL)
    {
      return qdata_arith_dbval (op, &a->arithptr->plan_item->fixed.arith, a->peek_left, a->peek_right,
				a->arithptr->value, a->domain);
    }

  std::string mask = { opnd_mark (a->peek_left), opnd_mark (a->peek_right), opnd_mark (a->peek_third),
    opnd_mark (a->peek_fourth)
  };
  g_log.push_back ("B" + std::to_string ((int) op) + ":" + mask + "@" +
		   std::to_string (thread_get_recursion_depth (thread_p)));
  if (op == T_INCR || op == T_DECR)
    {
      return FETCH_ARITH_RAW_LEFT;
    }
  if (g_body_fail != NO_ERROR)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, g_body_fail, 0);
      return ER_FAILED;
    }
  int n = 0;
  for (char c : mask)
    {
      n += (c == 'v');
    }
  db_make_int (a->arithptr->value, n);
  return NO_ERROR;
}

FETCH_ARITH_OP
fetch_arith_op (OPERATOR_TYPE opcode)
{
  (void) opcode;
  return fake_body;
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


/* ================================================================ the operand protocol sweep */

static const int E_L = -9101, E_R = -9102, E_T = -9103;

static REGU_VARIABLE *
node3 (OPERATOR_TYPE op, TP_DOMAIN * dom, REGU_VARIABLE * l, REGU_VARIABLE * r, REGU_VARIABLE * t,
       DOMAIN_PLAN_ITEM * item = NULL)
{
  REGU_VARIABLE *n = arith (op, dom, l, r, item);
  n->value.arithptr->thirdptr = t;
  return n;
}

/* the opcodes the sweep leaves out: the CASE family needs its predicate (declined without one), the casts are always
 * a cast kernel (their protocol is the cast-wrap case below), WIDTH_BUCKET's right operand is a BETWEEN predicate (its
 * own case below) */
static bool
sweep_excluded (OPERATOR_TYPE op)
{
  return op == T_CASE || op == T_DECODE || op == T_IF || op == T_PREDICATE || op == T_CAST || op == T_CAST_WRAP
    || op == T_WIDTH_BUCKET;
}

struct sweep_result
{
  int compiled = 0, rows = 0;
  std::vector<std::string> declined;
  std::vector<std::string> bad;
};

/* operand slot k of a row: 0 a value, 1 NULL, 2 the fetch fails */
static void
set_slot (const char *name, int k, int err)
{
  if (k == 0)
    {
      sq_val (name, v_int (1));
    }
  else if (k == 1)
    {
      sq_val (name, v_null ());
    }
  else
    {
      sq_val (name, v_null ());
      sq_fail (name, err);
    }
}

/* One node op (L, R, T) over subquery operands -- present as the shape says -- compiled once and evaluated for every
 * assignment of {value, NULL, failure} to the present operands by both paths.  wrap: each operand is
 * CAST_WRAP (subquery AS INTEGER), the parser's cast an operand gets */
static void
sweep_node (OPERATOR_TYPE op, int shape, bool wrap, TP_DOMAIN * node_dom, sweep_result & res)
{
  TP_DOMAIN *vc = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 64, 0, NULL, LANG_COLL_ISO_BINARY);
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *b = D (DB_TYPE_BIGINT);
  /* the operands' compile-time types differ from each other and from the node, so no dedicated kernel (NVL, NULLIF,
   * EXTRACT, arithmetic) takes the node: it is the generic step */
  auto opnd =[&](const char *name, TP_DOMAIN * dom, TP_DOMAIN * wrap_dom)->REGU_VARIABLE *
  {
    REGU_VARIABLE *q = sq (name, wrap ? i : dom);
    return wrap ? arith (T_CAST_WRAP, wrap_dom, NULL, q, NULL) : q;
  };
  g_sqs.clear ();
  REGU_VARIABLE *l = (shape & 1) ? opnd ("L", vc, b) : NULL;
  REGU_VARIABLE *r = (shape & 2) ? opnd ("R", i, i) : NULL;
  REGU_VARIABLE *t = (shape & 4) ? opnd ("T", b, D (DB_TYPE_DOUBLE)) : NULL;
  REGU_VARIABLE *root = node3 (op, node_dom, l, r, t);
  int idx = -1;
  EXPR_PROG *p = compile1 (root, &idx);
  if (p == NULL)
    {
      if (shape == 7)
	{
	  res.declined.push_back (std::to_string ((int) op));
	}
      return;
    }
  res.compiled++;
  const int nl = l ? 3 : 1, nr = r ? 3 : 1, nt = t ? 3 : 1;
  for (int a = 0; a < nl; a++)
    for (int bb = 0; bb < nr; bb++)
      for (int c = 0; c < nt; c++)
	{
	  if (l)
	    set_slot ("L", a, E_L);
	  if (r)
	    set_slot ("R", bb, E_R);
	  if (t)
	    set_slot ("T", c, E_T);
	  outcome x = run_interp (root);
	  outcome y = run_prog (p, idx);
	  res.rows++;
	  if (x.value != y.value || x.log != y.log)
	    {
	      static const char *k[] = { "v", "n", "f" };
	      res.bad.push_back ("opcode " + std::to_string ((int) op) + " shape " + std::to_string (shape) +
				 (wrap ? " wrapped" : "") + " L=" + (l ? k[a] : "-") + " R=" + (r ? k[bb] : "-") +
				 " T=" + (t ? k[c] : "-") + "\n    interp: " + x.value + " {" + x.log +
				 "}\n    program: " + y.value + " {" + y.log + "}\n    steps: " + kernels_of (p));
	    }
	}
  expr_prog_free (p);
}

static sweep_result
sweep_all (bool wrap, TP_DOMAIN * node_dom)
{
  sweep_result res;
  for (int o = 0; o <= (int) T_COLLECTION_TO_STRING; o++)
    {
      const OPERATOR_TYPE op = (OPERATOR_TYPE) o;
      if (sweep_excluded (op))
	{
	  continue;
	}
      for (int shape : { 7, 3, 5, 1, 6, 2 })
	{
	  /* a binary node the compiler types both operands of before it picks a path (NVL, NULLIF) always has both:
	   * only the full shape is a node the parser makes */
	  if (shape != 7 && shape != 3 && (op == T_NVL || op == T_IFNULL || op == T_COALESCE || op == T_NULLIF))
	    {
	      continue;
	    }
	  sweep_node (op, shape, wrap, node_dom, res);
	}
    }
  return res;
}

static std::string
sweep_report (const sweep_result & r)
{
  std::string s = "compiled nodes " + std::to_string (r.compiled) + ", rows " + std::to_string (r.rows) +
    ", mismatches " + std::to_string (r.bad.size ()) + ", declined opcodes (full shape):";
  for (const std::string & d:r.declined)
    {
      s += " " + d;
    }
  for (size_t k = 0; k < r.bad.size () && k < 4; k++)
    {
      s += "\n" + r.bad[k];
    }
  return s;
}

TEST_CASE ("generic step operand rules == the interpreter's fetch switch for every opcode the step takes: which "
	   "operands, in which order, the lazy '?' skip, the operands the body sees and its frame depth", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  for (int outer : { 0, 5 })
    {
      g_outer_depth = outer;
      sweep_result r = sweep_all (false, i);
      INFO ("outer depth " << outer << ": " << sweep_report (r));
      CHECK (r.compiled > 150);
      CHECK (r.bad.empty ());
    }
  g_outer_depth = 0;
}

TEST_CASE ("the same sweep under compat_mode=mysql and oracle_style_empty_string=yes (the rules that read them: "
	   "the L?Rt group's third operand, T_CONCAT's right)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  prm_Def[PRM_ID_COMPAT_MODE].value.v.i = COMPAT_MYSQL;
  sweep_result r1 = sweep_all (false, i);
  prm_Def[PRM_ID_COMPAT_MODE].value.v.i = COMPAT_CUBRID;
  prm_bool (PRM_ID_ORACLE_STYLE_EMPTY_STRING, true);
  sweep_result r2 = sweep_all (false, i);
  prm_bool (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  INFO ("mysql: " << sweep_report (r1));
  CHECK (r1.bad.empty ());
  INFO ("oracle_style_empty_string: " << sweep_report (r2));
  CHECK (r2.bad.empty ());
}

TEST_CASE ("oracle_style_empty_string: T_ADD / T_STRCAT over a string node fetch the right operand for a NULL left "
	   "(plus as concatenation), over a number node they skip it -- in both paths", "[contract]")
{
  TP_DOMAIN *vc = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 64, 0, NULL, LANG_COLL_ISO_BINARY);
  prm_bool (PRM_ID_ORACLE_STYLE_EMPTY_STRING, true);
  sweep_result r;
  for (OPERATOR_TYPE op : { T_ADD, T_STRCAT, T_CONCAT })
    {
      for (int shape : { 3, 7 })
	{
	  sweep_node (op, shape, false, vc, r);
	  sweep_node (op, shape, false, D (DB_TYPE_INTEGER), r);
	}
    }
  prm_bool (PRM_ID_ORACLE_STYLE_EMPTY_STRING, false);
  INFO (sweep_report (r));
  CHECK (r.compiled >= 6);
  CHECK (r.bad.empty ());
}

TEST_CASE ("cast-wrapped operands: a fetch failure under a date / time function becomes ER_DATE_CONVERSION / "
	   "ER_TIME_CONVERSION (the try step), elsewhere it stays the operand's error -- every opcode, both paths",
	   "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  sweep_result r = sweep_all (true, i);
  INFO (sweep_report (r));
  CHECK (r.compiled > 150);
  CHECK (r.bad.empty ());
}

TEST_CASE ("cast-wrapped operand whose CAST itself fails (a string that is no number): the substitution covers the "
	   "cast's own error too", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<std::string> bad;
  int ndate = 0, ntime = 0;
  for (OPERATOR_TYPE op : { T_MAKEDATE, T_ADDTIME, T_WEEK, T_FROM_TZ, T_MAKETIME, T_NEW_TIME, T_YEAR, T_MONTH,
       T_HOUR, T_SECTOTIME, T_TODAYS, T_EVALUATE_VARIABLE, T_DEFINE_VARIABLE, T_MOD, T_SUBSTRING })
    {
      for (int which = 0; which < 3; which++)
	{
	  g_sqs.clear ();
	  REGU_VARIABLE *q[3] = { sq ("L", i), sq ("R", i), sq ("T", i) };
	  REGU_VARIABLE *o[3] = { q[0], q[1], q[2] };
	  o[which] = arith (T_CAST_WRAP, i, NULL, q[which], NULL);
	  REGU_VARIABLE *root = node3 (op, i, o[0], o[1], o[2]);
	  int idx = -1;
	  EXPR_PROG *p = compile1 (root, &idx);
	  if (p == NULL)
	    {
	      continue;
	    }
	  sq_val ("L", v_int (1)), sq_val ("R", v_int (1)), sq_val ("T", v_int (1));
	  DB_VALUE s;
	  db_make_string (&s, "not a number");
	  sq_val (which == 0 ? "L" : which == 1 ? "R" : "T", s);
	  outcome x = run_interp (root);
	  outcome y = run_prog (p, idx);
	  ndate += (x.value == "ERR:" + std::to_string (ER_DATE_CONVERSION));
	  ntime += (x.value == "ERR:" + std::to_string (ER_TIME_CONVERSION));
	  if (x.value != y.value || x.log != y.log)
	    {
	      bad.push_back ("opcode " + std::to_string ((int) op) + " operand " + std::to_string (which) +
			     "\n    interp: " + x.value + " {" + x.log + "}\n    program: " + y.value + " {" + y.log +
			     "}\n    steps: " + kernels_of (p));
	    }
	  expr_prog_free (p);
	}
    }
  INFO ((bad.empty ()? std::string () : bad[0]));
  CHECK (ndate > 0);
  CHECK (ntime > 0);
  CHECK (bad.empty ());
}

/* ================================================================ WIDTH_BUCKET */

/* WIDTH_BUCKET (L, lo, hi, count): the parser carries the bounds as a PT_BETWEEN right operand -- an arithmetic node
 * whose predicate is (x >= lo AND x < hi), read by fetch_peek_min_max_value_of_width_bucket_func -- and the count as
 * the third operand */
static REGU_VARIABLE *
width_bucket (TP_DOMAIN * i)
{
  using namespace cubxasl;
  pred_expr *lo = (pred_expr *) calloc (1, sizeof (pred_expr));
  pred_expr *hi = (pred_expr *) calloc (1, sizeof (pred_expr));
  pred_expr *both = (pred_expr *) calloc (1, sizeof (pred_expr));
  lo->type = T_EVAL_TERM;
  lo->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
  lo->pe.m_eval_term.et.et_comp.rhs = sq ("LO", i);
  lo->pe.m_eval_term.et.et_comp.rel_op = R_GE;
  hi->type = T_EVAL_TERM;
  hi->pe.m_eval_term.et_type = T_COMP_EVAL_TERM;
  hi->pe.m_eval_term.et.et_comp.rhs = sq ("HI", i);
  hi->pe.m_eval_term.et.et_comp.rel_op = R_LT;
  both->type = T_PRED;
  both->pe.m_pred.lhs = lo;
  both->pe.m_pred.rhs = hi;
  both->pe.m_pred.bool_op = B_AND;
  REGU_VARIABLE *between = arith (T_PREDICATE, i, NULL, NULL, NULL);	/* an arithmetic node carrying the BETWEEN predicate */
  between->value.arithptr->pred = both;
  return node3 (T_WIDTH_BUCKET, i, sq ("X", i), between, sq ("N", i));
}

TEST_CASE ("WIDTH_BUCKET: left, the two bounds, then the bucket count -- the fetch order, the first failing operand "
	   "and a NULL left (no lazy skip) are the interpreter's (review 2026-10-15 #5)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  g_sqs.clear ();
  REGU_VARIABLE *root = width_bucket (i);
  int idx = -1;
  EXPR_PROG *p = compile1 (root, &idx);
  REQUIRE (p != NULL);
  std::string steps = kernels_of (p);
  std::vector<std::string> bad;
  const char *names[] = { "X", "LO", "HI", "N" };
  int rows = 0;
  for (int m = 0; m < 81; m++)
    {
      int k = m;
      for (int s = 0; s < 4; s++, k /= 3)
	{
	  set_slot (names[s], k % 3, -9200 - s);
	}
      outcome x = run_interp (root);
      outcome y = run_prog (p, idx);
      rows++;
      if (x.value != y.value || x.log != y.log)
	{
	  bad.push_back ("row " + std::to_string (m) + "\n    interp: " + x.value + " {" + x.log + "}\n    program: " +
			 y.value + " {" + y.log + "}");
	}
    }
  /* the anchor: every operand a value -> the order is X LO HI N, then the body with all four */
  for (const char *n : names)
    {
      sq_val (n, v_int (1));
    }
  outcome x = run_interp (root);
  INFO ("steps: " << steps << "\nall values: " << x.log << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (x.log.rfind ("X LO HI N B", 0) == 0);
  CHECK (bad.empty ());
  expr_prog_free (p);
}

/* ================================================================ the recursion limit and nested programs */

static REGU_VARIABLE *
lit_int (int k)
{
  REGU_VARIABLE *c = new_regu (TYPE_DBVAL, D (DB_TYPE_INTEGER));
  c->value.dbval = v_int (k);
  return c;
}

/* shape 0: left-deep ((A + 1) + 2) ... (the deepest node on the left spine, always reached)
 * shape 1: right-deep 1 + (2 + (... + A)) (each right operand a region that can fail: lazy)
 * shape 2: a generic chain ABS (ABS (... A)) (one generic step per level, the body reached at every level) */
static REGU_VARIABLE *
chain (int shape, int depth, REGU_VARIABLE * leaf)
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  REGU_VARIABLE *n = leaf;
  for (int k = 1; k <= depth; k++)
    {
      switch (shape)
	{
	case 0:
	  n = arith (T_ADD, i, n, lit_int (k), number_plan (DB_TYPE_INTEGER, i));
	  break;
	case 1:
	  n = arith (T_ADD, i, lit_int (k), n, number_plan (DB_TYPE_INTEGER, i));
	  break;
	default:
	  n = arith (T_ABS, i, NULL, n, NULL);
	  break;
	}
    }
  return n;
}

struct depth_case
{
  int shape, depth, limit, outer;
};

TEST_CASE ("recursion limit: max_recursion_sql_depth is raised at the node the interpreter raises it at -- the "
	   "thread's frames above plus the node's level, for kernel chains, lazy right spines and generic chains, "
	   "including the frames a generic body sees", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<std::string> bad;
  int errs = 0, oks = 0;
  for (int shape : { 0, 1, 2 })
    for (int depth : { 1, 5, 6, 7, 30 })
      for (int limit : { 3, 6, 29, 400 })
	for (int outer : { 0, 1, 2, 5 })
	  {
	    g_sqs.clear ();
	    prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, limit);
	    g_outer_depth = outer;
	    REGU_VARIABLE *root = chain (shape, depth, sq ("A", i));
	    int idx = -1;
	    EXPR_PROG *p = compile1 (root, &idx);
	    if (p == NULL)
	      {
		bad.push_back ("declined shape " + std::to_string (shape) + " depth " + std::to_string (depth));
		continue;
	      }
	    for (int k = 0; k < 2; k++)
	      {
		sq_val ("A", k == 0 ? v_int (3) : v_null ());
		outcome x = run_interp (root);
		outcome y = run_prog (p, idx);
		(x.value.compare (0, 4, "ERR:") == 0 ? errs : oks)++;
		if (x.value != y.value || x.log != y.log)
		  {
		    bad.push_back ("shape " + std::to_string (shape) + " depth " + std::to_string (depth) + " limit " +
				   std::to_string (limit) + " outer " + std::to_string (outer) + (k ? " A=NULL" : "") +
				   "\n    interp: " + x.value + " {" + x.log + "}\n    program: " + y.value + " {" + y.log +
				   "}");
		  }
	      }
	    expr_prog_free (p);
	  }
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 400);
  g_outer_depth = 0;
  INFO ("errors " << errs << ", values " << oks << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (errs > 0);
  CHECK (oks > 0);
  CHECK (bad.empty ());
}

TEST_CASE ("recursion limit under a skipped region: a NULL left operand skips a right subtree deeper than the limit "
	   "-- no error in either path; a value left reaches it -- the same error in both", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<std::string> bad;
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 6);
  for (int depth : { 3, 8, 20 })
    {
      g_sqs.clear ();
      /* L + (deep right chain over subquery R): the right side can fail, so it is the lazy region */
      REGU_VARIABLE *root = arith (T_ADD, i, sq ("L", i), chain (0, depth, sq ("R", i)),
				   number_plan (DB_TYPE_INTEGER, i));
      int idx = -1;
      EXPR_PROG *p = compile1 (root, &idx);
      REQUIRE (p != NULL);
      for (int k = 0; k < 2; k++)
	{
	  sq_val ("L", k == 0 ? v_int (1) : v_null ());
	  sq_val ("R", v_int (2));
	  outcome x = run_interp (root);
	  outcome y = run_prog (p, idx);
	  if (x.value != y.value || x.log != y.log)
	    {
	      bad.push_back ("depth " + std::to_string (depth) + (k ? " L=NULL" : " L=1") + "\n    interp: " + x.value +
			     " {" + x.log + "}\n    program: " + y.value + " {" + y.log + "}\n    steps: " +
			     kernels_of (p));
	    }
	}
      expr_prog_free (p);
    }
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 400);
  INFO ((bad.empty ()? std::string () : bad[0]));
  CHECK (bad.empty ());
}

TEST_CASE ("recursion limit at the boundary: a row the deep part of which is skipped runs the checked loop, and a "
	   "node exactly at max_recursion_sql_depth there is still allowed -- one frame more is an error -- as in the "
	   "interpreter", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<std::string> bad;
  int errs = 0, oks = 0;
  for (int limit : { 3, 6, 10 })
    for (int outer : { 0, 1, 2 })
      for (int extra : { 0, 1 })	/* 0: the left spine's deepest node exactly at the limit; 1: one past it */
	{
	  const int left_depth = limit - outer + extra - 1;	/* below the root: the deepest left node at level left_depth + 1 */
	  if (left_depth < 0)
	    {
	      continue;
	    }
	  g_sqs.clear ();
	  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, limit);
	  g_outer_depth = outer;
	  /* (left chain over L) + (a right chain deeper than the limit over R): the right side is a lazy region */
	  REGU_VARIABLE *root = arith (T_ADD, i, chain (0, left_depth, sq ("L", i)), chain (0, limit + 5, sq ("R", i)),
				       number_plan (DB_TYPE_INTEGER, i));
	  int idx = -1;
	  EXPR_PROG *p = compile1 (root, &idx);
	  REQUIRE (p != NULL);
	  for (int k = 0; k < 2; k++)
	    {
	      sq_val ("L", k == 0 ? v_null () : v_int (1));
	      sq_val ("R", v_int (2));
	      outcome x = run_interp (root);
	      outcome y = run_prog (p, idx);
	      (x.value.compare (0, 4, "ERR:") == 0 ? errs : oks)++;
	      if (x.value != y.value || x.log != y.log)
		{
		  bad.push_back ("limit " + std::to_string (limit) + " outer " + std::to_string (outer) + " extra " +
				 std::to_string (extra) + (k ? " L=1" : " L=NULL") + "\n    interp: " + x.value +
				 " {" + x.log + "}\n    program: " + y.value + " {" + y.log + "}");
		}
	    }
	  expr_prog_free (p);
	}
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 400);
  g_outer_depth = 0;
  INFO ("errors " << errs << ", values " << oks << (bad.empty ()? "" : "\n" + bad[0]));
  CHECK (oks > 0);
  CHECK (errs > 0);
  CHECK (bad.empty ());
}

TEST_CASE ("a tree past the program's tables (128 steps): the overflowing subtree becomes a nested program -- values, "
	   "errors and the recursion limit stay the interpreter's at 70..300 terms, left- and right-deep "
	   "(the 70-term core, 2026-10-13)", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  std::vector<std::string> bad;
  int nested = 0, declined = 0;
  for (int shape : { 0, 1, 2 })
    for (int depth : { 70, 127, 130, 200, 300 })
      for (int limit : { 400, 150 })
	if (!(shape == 1 && depth > 200))	/* at the PR head a right-deep chain this deep compiles for seconds (F1) */
	{
	  g_sqs.clear ();
	  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, limit);
	  REGU_VARIABLE *root = chain (shape, depth, sq ("A", i));
	  int idx = -1;
	  EXPR_PROG *p = compile1 (root, &idx);
	  if (p == NULL)
	    {
	      declined++;	/* the root stays interpreted: legal (PR7658 F1 below is why it happens here) */
	      continue;
	    }
	  nested += (kernels_of (p).find ("nested_prog") != std::string::npos);
	  for (int k = 0; k < 3; k++)
	    {
	      sq_val ("A", k == 0 ? v_int (3) : k == 1 ? v_null () : v_int (INT_MAX));
	      outcome x = run_interp (root);
	      outcome y = run_prog (p, idx);
	      if (x.value != y.value || x.log != y.log)
		{
		  bad.push_back ("shape " + std::to_string (shape) + " depth " + std::to_string (depth) + " limit " +
				 std::to_string (limit) + " row " + std::to_string (k) + "\n    interp: " + x.value +
				 "\n    program: " + y.value);
		}
	    }
	  expr_prog_free (p);
	}
  prm_int (PRM_ID_MAX_RECURSION_SQL_DEPTH, 400);
  INFO ("programs with a nested program: " << nested << ", declined roots " << declined <<
	(bad.empty ()? "" : "\n" + bad[0]));
  CHECK (nested > 0);
  CHECK (bad.empty ());
}

/* ================================================================ a list: prologue reordering under skip masks */

TEST_CASE ("a list whose later column hoists a literal step ahead of a generic '?' node: the lazy check still marks "
	   "the generic step it guards and lands on it", "[contract]")
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER), *n = D (DB_TYPE_NUMERIC);
  g_sqs.clear ();
  REGU_VARIABLE *c7 = lit_int (7);
  std::vector<REGU_VARIABLE *> roots = {
    node3 (T_MOD, i, sq ("L", i), sq ("R", i), NULL),
    node3 (T_SUBSTRING, i, sq ("L2", i), sq ("R2", i), sq ("T2", i)),
    arith (T_ADD, n, sq ("N", n), c7, number_plan (DB_TYPE_NUMERIC, n)),
  };
  REGU_VARIABLE *rs[3] = { roots[0], roots[1], roots[2] };
  int idx[3] = { -1, -1, -1 };
  reset_row ();
  EXPR_PROG *p = expr_prog_compile_roots (thr, rs, 3, &g_vd, true, false, false, idx, NULL);
  REQUIRE (p != NULL);
  INFO ("steps: " << kernels_of (p) << " prologue " << p->n_prologue);
  CHECK (p->n_prologue > 0);
  std::vector<std::string> bad;
  const char *names[] = { "L", "R", "L2", "R2", "T2" };
  for (int m = 0; m < 243; m++)
    {
      int k = m;
      for (int s = 0; s < 5; s++, k /= 3)
	{
	  set_slot (names[s], k % 3, -9300 - s);
	}
      sq_val ("N", v_num ("1.5"));
      std::string a, al, b, bl;
      reset_row ();
      for (REGU_VARIABLE * r:roots)
	{
	  DB_VALUE *peek = NULL;
	  if (fetch_peek_dbval (thr, r, &g_vd, NULL, NULL, NULL, &peek) != NO_ERROR)
	    {
	      a = "ERR:" + std::to_string (g_errid);
	      break;
	    }
	  a += show (peek) + " | ";
	}
      for (const std::string & x:g_log)
	al += x + " ";
      reset_row ();
      if (expr_prog_eval (p, thr, &g_vd, NULL, NULL) != NO_ERROR)
	{
	  b = "ERR:" + std::to_string (g_errid);
	}
      else
	{
	  for (int j = 0; j < 3; j++)
	    b += show (expr_prog_value (p, idx[j])) + " | ";
	}
      for (const std::string & x:g_log)
	bl += x + " ";
      if (a != b || al != bl)
	{
	  bad.push_back ("row " + std::to_string (m) + "\n    interp: " + a + " {" + al + "}\n    program: " + b + " {" +
			 bl + "}");
	}
    }
  INFO ((bad.empty ()? std::string () : bad[0]));
  CHECK (bad.empty ());
  expr_prog_free (p);
}

/* ================================================================ PR7658 F1: deep right-nested chains */

#include <chrono>

/* k + (k+1 + (... + c)) -- literal left operands, a column at the bottom: every right side holds a fallible add, so
 * every level is a lazy region with a guard of its own */
static REGU_VARIABLE *
right_chain (int depth)
{
  TP_DOMAIN *i = D (DB_TYPE_INTEGER);
  REGU_VARIABLE *n = attr (1, i);
  for (int k = 1; k <= depth; k++)
    {
      n = arith (T_ADD, i, lit_int (k), n, number_plan (DB_TYPE_INTEGER, i));
    }
  return n;
}

/* (... ((c + 1) + 2) ... + k): no lazy region */
static REGU_VARIABLE *
left_chain (int depth)
{
  return chain (0, depth, attr (1, D (DB_TYPE_INTEGER)));
}

static double
compile_seconds (REGU_VARIABLE * root, bool * compiled)
{
  double best = 1e9;
  for (int rep = 0; rep < 3; rep++)
    {
      int idx = -1;
      auto t0 = std::chrono::steady_clock::now ();
      EXPR_PROG *p = compile1 (root, &idx);
      auto t1 = std::chrono::steady_clock::now ();
      *compiled = (p != NULL);
      if (p != NULL)
	{
	  expr_prog_free (p);
	}
      best = std::min (best, std::chrono::duration < double >(t1 - t0).count ());
    }
  return best;
}

TEST_CASE ("PR7658 F1: a right-nested arithmetic chain k + (k+1 + (... + c)) of 127 levels or more is declined -- "
	   "the overflow recovery (rewind, recompile the subtree as a nested program) cannot place it; 63/64/65 "
	   "show the same at the root of a left-deep chain (current behaviour, PASS = defect present)", "[defect]")
{
  bool c130 = false, c200 = false, l63 = false, l64 = false, l65 = false;
  (void) compile_seconds (right_chain (130), &c130);
  (void) compile_seconds (right_chain (200), &c200);
  (void) compile_seconds (left_chain (63), &l63);
  (void) compile_seconds (left_chain (64), &l64);
  (void) compile_seconds (left_chain (65), &l65);
  INFO ("right 130 compiled " << c130 << ", right 200 " << c200 << ", left 63/64/65 " << l63 << l64 << l65);
  CHECK_FALSE (c130);
  CHECK_FALSE (c200);
  CHECK (l63);
  CHECK_FALSE (l64);		/* the 129th cell is the root's own: a top-level root has no nesting retry */
  CHECK (l65);
}

TEST_CASE ("PR7658 F1: compiling that chain takes time exponential in its depth -- 260 levels cost over 4x 200 "
	   "levels and over 50 ms (1.7 s at 300, 14 s at 340, 312 s at 399 measured), and the root is declined in "
	   "the end (current behaviour, PASS = defect present)", "[defect]")
{
  bool c200 = false, c260 = false;
  const double t200 = compile_seconds (right_chain (200), &c200);
  const double t260 = compile_seconds (right_chain (260), &c260);
  INFO ("200 levels " << t200 << " s (compiled " << c200 << "), 260 levels " << t260 << " s (compiled " << c260 <<
	")");
  CHECK (t260 > 0.05);
  CHECK (t260 > 4 * t200);
  CHECK_FALSE (c260);
}

TEST_CASE ("a left-deep chain of 300 levels (no lazy region) compiles into nested programs within 50 ms", "[contract]")
{
  bool c = false;
  const double t = compile_seconds (left_chain (300), &c);
  INFO ("300 levels " << t << " s");
  CHECK (c);
  CHECK (t < 0.05);
}
