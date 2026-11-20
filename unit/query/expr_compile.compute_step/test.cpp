/*
 * PR#8660 (CBRD-28249) -- expr_scan_pred_compute_step (): the page batch computes, before the rows, a generic step
 * the row loop computes once per execution (src/query/expr_compile.c, server mode).
 *
 *   [contract]  the call computes the step (one body call) and marks the execution, so the row loop then answers the
 *               slot without calling the body; a step the row loop already computed this execution is not computed
 *               again; the next execution computes again; a body that fails leaves no error behind (the error stack
 *               restores the state before the call), no mark, and ER_FAILED -- the row loop then computes it again and
 *               raises the error there.
 *
 * The fakes and builders are expr_compile.generic_once's (fetch_arith_ops.c is not linked: one recording body), plus
 * er_stack_push/er_stack_pop over the fake error id and a switch that makes the body fail.
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
#include "heap_file.h"
#include "fetch.h"
#include "fetch_arith_ops.h"
#include "expr_compile.h"
#include "thread_entry.hpp"
#include "area_alloc.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
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

/* error_manager.c: the error stack -- a push saves the current error and starts clean, a pop restores the saved one */
static std::vector<int> g_err_stack;
void
er_stack_push (void)
{
  g_err_stack.push_back (g_errid);
  g_errid = NO_ERROR;
}

void
er_stack_pop (void)
{
  assert (!g_err_stack.empty ());
  g_errid = g_err_stack.back ();
  g_err_stack.pop_back ();
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

/* the boot defaults of what these paths read; oracle_style_empty_string off gives T_CONCAT its '?' rule ("L?r") */
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

/* heap_file.c: the current record's decoded value of an attribute */
struct attr_slot
{
  DB_VALUE v;
};
static std::map<int, attr_slot> g_attrs;
static HEAP_CACHE_ATTRINFO g_attrinfo;

DB_VALUE *
heap_attrinfo_access (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  (void) attr_info;
  return &g_attrs[attrid].v;
}

/* area_alloc.c: the domain area's element (tp_domain_new for a VARCHAR(n) domain) */
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

/* language_support.c: every collation slot is iso88591_bin before the built-ins are registered */
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

/* query_executor.c: no node here links a subquery */
int
qexec_execute_mainblock (THREAD_ENTRY * thread_p, xasl_node * xasl, xasl_state * xstate,
			 UPDDEL_CLASS_INSTANCE_LOCK_INFO * p_class_instance_lock_info)
{
  (void) thread_p;
  (void) xasl;
  (void) xstate;
  (void) p_class_instance_lock_info;
  assert (false);
  return ER_FAILED;
}

/* ---- the operator body: one recording body for every opcode (fetch_arith_ops.c is not linked) */
static std::vector<std::string> g_log;

static char
opnd_mark (const DB_VALUE * v)
{
  return v == NULL ? '-' : DB_IS_NULL (v) ? 'n' : 'v';
}

static bool g_body_fails = false;	/* the body sets ER_QSTR_INVALID_ESCAPE_CHARACTER and fails */

static int
fake_body (THREAD_ENTRY * thread_p, FETCH_ARITH_ARGS * a)
{
  (void) thread_p;
  const OPERATOR_TYPE op = a->arithptr->opcode;
  std::string mask = { opnd_mark (a->peek_left), opnd_mark (a->peek_right), opnd_mark (a->peek_third),
    opnd_mark (a->peek_fourth)
  };
  g_log.push_back ("B" + std::to_string ((int) op) + ":" + mask);
  if (g_body_fails)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_QSTR_INVALID_ESCAPE_CHARACTER, 0);
      return ER_FAILED;
    }
  if (op == T_INCR || op == T_DECR)
    {
      return FETCH_ARITH_RAW_LEFT;
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

#define D(t) tp_domain_resolve_default (t)

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

static REGU_VARIABLE *
arith (OPERATOR_TYPE op, TP_DOMAIN * dom, REGU_VARIABLE * l, REGU_VARIABLE * r)
{
  ARITH_TYPE *a = new ARITH_TYPE ();
  std::memset ((void *) a, 0, sizeof (*a));
  a->domain = dom;
  a->value = new DB_VALUE;
  db_make_null (a->value);
  a->leftptr = l;
  a->rightptr = r;
  a->opcode = op;
  REGU_VARIABLE *rv = new_regu (TYPE_INARITH, dom);
  rv->value.arithptr = a;
  return rv;
}

static REGU_VARIABLE *
hv (int pos)
{
  REGU_VARIABLE *r = new_regu (TYPE_POS_VALUE, D (DB_TYPE_VARCHAR));
  r->value.val_pos = pos;
  return r;
}

static REGU_VARIABLE *
lit_str (const char *s)
{
  REGU_VARIABLE *c = new_regu (TYPE_DBVAL, D (DB_TYPE_VARCHAR));
  db_make_string (&c->value.dbval, s);
  return c;
}

/* ================================================================ running */

static VAL_DESCR g_vd;
static XASL_STATE g_xs;
static DB_VALUE g_hv[2];

/* the host variables and the execution (0: no execution identity) of the next compile or evaluation */
static void
exec_bind (QUERY_ID query, const char *v0, const char *v1)
{
  if (v0 != NULL)
    {
      db_make_string (&g_hv[0], v0);
    }
  else
    {
      db_make_null (&g_hv[0]);
    }
  if (v1 != NULL)
    {
      db_make_string (&g_hv[1], v1);
    }
  else
    {
      db_make_null (&g_hv[1]);
    }
  g_vd.dbval_ptr = g_hv;
  g_vd.dbval_cnt = 2;
  g_xs.query_id = query;
  g_vd.xasl_state = (query != 0) ? &g_xs : NULL;
}

static void
exec_unbind ()
{
  g_vd.dbval_ptr = NULL;
  g_vd.dbval_cnt = 0;
  g_vd.xasl_state = NULL;
}

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
  thr->xasl_recursion_depth = 0;
}

static std::string
show (const DB_VALUE * v)
{
  if (v == NULL)
    {
      return "(no value)";
    }
  if (DB_IS_NULL (v))
    {
      return "NULL";
    }
  if (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_INTEGER)
    {
      return "INT:" + std::to_string (db_get_int (v));
    }
  return "type" + std::to_string ((int) DB_VALUE_DOMAIN_TYPE (v));
}

struct outcome
{
  std::string value;		/* show () of the value, or ERR:<er_errid> */
  std::string log;		/* the body calls, in order */
};

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
  outcome o;
  o.value = (rc != NO_ERROR) ? "ERR:" + std::to_string (g_errid) : show (expr_prog_value (p, idx));
  for (const std::string & s : g_log)
    {
      o.log += s + " ";
    }
  return o;
}

/* the calls of an operator's body in one evaluation's log */
static int
body_calls (const outcome & o, OPERATOR_TYPE op)
{
  const std::string tag = "B" + std::to_string ((int) op) + ":";
  int n = 0;

  for (size_t at = o.log.find (tag); at != std::string::npos; at = o.log.find (tag, at + 1))
    {
      n++;
    }
  return n;
}

/* evaluate the program once per entry of `queries` (each the execution of that evaluation): the body calls of `op` */
static std::vector<int>
calls_per_eval (EXPR_PROG * p, int idx, OPERATOR_TYPE op, const std::vector<QUERY_ID> & queries,
		std::vector<std::string> *values = NULL)
{
  std::vector<int> calls;

  for (QUERY_ID q : queries)
    {
      g_xs.query_id = q;
      g_vd.xasl_state = (q != 0) ? &g_xs : NULL;
      outcome o = run_prog (p, idx);
      calls.push_back (body_calls (o, op));
      if (values != NULL)
	{
	  values->push_back (o.value);
	}
    }
  return calls;
}

/* hooks.cpp: the generic steps of a program that own a slot (the ones that compute once per execution) */
int unitseam_generic_slots (const EXPR_PROG * prog);

/* ================================================================ cases */

/* hooks.cpp: the program's generic step that owns a slot (-1: none), and expr_scan_pred_compute_step () over a filter
 * root holding the program */
int unitseam_generic_slot_step (const EXPR_PROG * prog);
int unitseam_compute_step (EXPR_PROG * prog, int step, THREAD_ENTRY * thread_p, VAL_DESCR * vd);

static int
compute (EXPR_PROG * p, int step, QUERY_ID q, int *calls)
{
  g_xs.query_id = q;
  g_vd.xasl_state = (q != 0) ? &g_xs : NULL;
  reset_row ();
  int rc = unitseam_compute_step (p, step, thr, &g_vd);
  *calls = (int) g_log.size ();
  return rc;
}

TEST_CASE ("the batch computes the step once for the execution and the row loop then answers its slot", "[contract]")
{
  exec_bind (7, "pay%", NULL);
  int idx = -1, calls = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  const int step = unitseam_generic_slot_step (p);
  REQUIRE (step >= 0);
  CHECK (compute (p, step, 7, &calls) == NO_ERROR);
  CHECK (calls == 1);
  CHECK (g_errid == NO_ERROR);
  std::vector<std::string> values;
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7 }, &values) == std::vector<int> ({ 0, 0 }));
  CHECK (values == std::vector<std::string> ({ "INT:1", "INT:1" }));
  /* the next execution: the batch computes again, the row loop again answers the slot */
  CHECK (compute (p, step, 8, &calls) == NO_ERROR);
  CHECK (calls == 1);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 8 }) == std::vector<int> ({ 0 }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a step the row loop computed this execution is not computed again", "[contract]")
{
  exec_bind (7, "pay%", NULL);
  int idx = -1, calls = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  const int step = unitseam_generic_slot_step (p);
  REQUIRE (step >= 0);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7 }) == std::vector<int> ({ 1 }));
  CHECK (compute (p, step, 7, &calls) == NO_ERROR);
  CHECK (calls == 0);
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a failing step leaves no error and no mark: the row loop computes it again and raises the error",
	   "[contract]")
{
  exec_bind (7, "pay%", NULL);
  int idx = -1, calls = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  const int step = unitseam_generic_slot_step (p);
  REQUIRE (step >= 0);
  g_body_fails = true;
  CHECK (compute (p, step, 7, &calls) == ER_FAILED);
  CHECK (calls == 1);
  CHECK (g_errid == NO_ERROR);	/* discarded: the state before the call */
  CHECK (g_err_stack.empty ());
  std::vector<std::string> values;
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7 }, &values) == std::vector<int> ({ 1 }));
  CHECK (values == std::vector<std::string> ({ "ERR:" + std::to_string (ER_QSTR_INVALID_ESCAPE_CHARACTER) }));
  g_body_fails = false;
  /* the next row of the same execution computes once more and keeps the value */
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7 }) == std::vector<int> ({ 1, 0 }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("an error set before the call is kept when the step succeeds or fails", "[contract]")
{
  exec_bind (7, "pay%", NULL);
  int idx = -1, calls = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  const int step = unitseam_generic_slot_step (p);
  REQUIRE (step >= 0);
  g_xs.query_id = 7;
  g_vd.xasl_state = &g_xs;
  g_log.clear ();
  g_errid = ER_INTERRUPTED;
  g_body_fails = true;
  CHECK (unitseam_compute_step (p, step, thr, &g_vd) == ER_FAILED);
  CHECK (g_errid == ER_INTERRUPTED);
  g_body_fails = false;
  CHECK (unitseam_compute_step (p, step, thr, &g_vd) == NO_ERROR);
  CHECK (g_errid == ER_INTERRUPTED);
  expr_prog_free (p);
  exec_unbind ();
}
