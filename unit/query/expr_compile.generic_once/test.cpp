/*
 * PR#8623 (CBRD-28120) -- a compiled generic step computes once per execution where the interpreter keeps its node
 * (src/query/expr_compile.c, server mode).
 *
 * A generic step (an operator the kernel table does not cover) whose operand cells all hold one value for the
 * execution -- a literal, a host variable, a value of those alone, or another such step -- owns a slot
 * (expr_compile_generic ()): it computes on the first evaluation of an execution and answers the slot on the later
 * ones (expr_k_generic ()), as the interpreter keeps the node for the rest of the execution (FETCH_ALL_CONST).  An
 * execution is the value descriptor's query (xasl_state->query_id); with none the program enters a new execution on
 * every evaluation (expr_prog_prepare ()).
 *
 * fetch_arith_ops.c is NOT linked: fetch_arith_op () hands the generic step one recording body, which logs every
 * call with the operands it was handed ('-' not fetched, 'n' NULL, 'v' a value) and answers the number of values --
 * so the calls of one evaluation say whether the step computed in it, and the operands what it was handed.
 *
 * Real code linked in (suite.json extra): numeric_opfunc.c, query_opfunc.c, object_domain.c,
 * object_domain_convert.cpp, fetch.c, object_primitive.c, db_macro.c, chartype.c, intl_support.c.
 * Faked: fetch_arith_op, er_set/er_errid/er_clear, prm_Def, the thread entry, heap_attrinfo_access,
 * qexec_execute_mainblock (no subquery leaf here), area_alloc/area_free, lang_Collations.
 * (The fakes are those of expr_compile.generic_step, which develop has outgrown: domain_plan.h, plan_item.)
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

/* memory_alloc.c: the private heap.  An armed failure is the private heap running out (hl_lea_alloc ()): the
 * allocation sets ER_OUT_OF_VIRTUAL_MEMORY and answers NULL -- a string's setval then answers that error with its
 * destination left NULL (mr_setval_string ()). */
static int g_alloc_fail_in = 0;	/* > 0: the n-th next private allocation fails */

void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  if (g_alloc_fail_in > 0 && --g_alloc_fail_in == 0)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
      return NULL;
    }
  return malloc (size > 0 ? size : 1);
}

void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  free (ptr);
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
static bool g_body_string = false;	/* every body answers the VARCHAR 'pay' (a copy of it allocates), not an INT */

static char
opnd_mark (const DB_VALUE * v)
{
  return v == NULL ? '-' : DB_IS_NULL (v) ? 'n' : 'v';
}

static int
fake_body (THREAD_ENTRY * thread_p, FETCH_ARITH_ARGS * a)
{
  (void) thread_p;
  const OPERATOR_TYPE op = a->arithptr->opcode;
  std::string mask = { opnd_mark (a->peek_left), opnd_mark (a->peek_right), opnd_mark (a->peek_third),
    opnd_mark (a->peek_fourth)
  };
  g_log.push_back ("B" + std::to_string ((int) op) + ":" + mask);
  if (op == T_INCR || op == T_DECR)
    {
      return FETCH_ARITH_RAW_LEFT;
    }
  if (g_body_string)
    {
      db_make_string (a->arithptr->value, "pay");
      return NO_ERROR;
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
  if (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_VARCHAR)
    {
      return "STR:" + std::string (db_get_string (v), db_get_string_size (v));
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

TEST_CASE ("a generic step over a host variable computes once per execution -- the first evaluation of an execution "
	   "calls the body, the later ones answer the slot's value, the next execution calls it again", "[contract]")
{
  exec_bind (7, "pay%", NULL);
  int idx = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 1);
  std::vector<std::string> values;
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7, 7, 8, 8 }, &values) == std::vector<int> ({ 1, 0, 0, 1, 0 }));
  for (const std::string & v : values)
    {
      CHECK (v == "INT:1");
    }
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("without an execution identity the program enters an execution on every evaluation, so the step computes "
	   "every time", "[contract]")
{
  exec_bind (0, "pay%", NULL);
  int idx = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), hv (0), NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 1);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 0, 0, 0 }) == std::vector<int> ({ 1, 1, 1 }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a generic step over a literal computes once per execution, not once for the program's life -- every "
	   "execution computes it again", "[contract]")
{
  exec_bind (7, NULL, NULL);
  int idx = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), lit_str ("pay%"), NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 1);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7, 8, 9 }) == std::vector<int> ({ 1, 0, 1, 1 }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a generic step over a column, an operator the interpreter does not keep (SYS_GUID) and a body that "
	   "fetches its own operands (COALESCE) compute per row", "[contract]")
{
  db_make_string (&g_attrs[1].v, "payx");
  exec_bind (7, "a", "b");
  REGU_VARIABLE *roots[3] = {
    arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), attr (1, D (DB_TYPE_VARCHAR)), NULL),
    arith (T_SYS_GUID, D (DB_TYPE_INTEGER), NULL, NULL),
    arith (T_COALESCE, D (DB_TYPE_INTEGER), hv (0), hv (1))
  };
  const OPERATOR_TYPE ops[3] = { T_LIKE_LOWER_BOUND, T_SYS_GUID, T_COALESCE };
  for (int k = 0; k < 3; k++)
    {
      INFO ("root " << k);
      int idx = -1;
      EXPR_PROG *p = compile1 (roots[k], &idx);
      REQUIRE (p != NULL);
      CHECK (unitseam_generic_slots (p) == 0);
      CHECK (calls_per_eval (p, idx, ops[k], { 7, 7, 7 }) == std::vector<int> ({ 1, 1, 1 }));
      expr_prog_free (p);
    }
  exec_unbind ();
}

TEST_CASE ("CONCAT's '?' rule -- a NULL left operand skips the right one on the first evaluation of the execution, "
	   "the later evaluations answer at once and leave no skip mark behind, and the next execution with a left "
	   "value fetches the right operand", "[contract]")
{
  exec_bind (7, NULL, NULL);
  int idx = -1;
  EXPR_PROG *p = compile1 (arith (T_CONCAT, D (DB_TYPE_INTEGER), hv (0), lit_str ("%")), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 1);
  const std::string tag = "B" + std::to_string ((int) T_CONCAT) + ":";
  outcome o = run_prog (p, idx);
  CHECK (o.log == tag + "n--- ");	/* left NULL, right not fetched */
  CHECK (body_calls (run_prog (p, idx), T_CONCAT) == 0);
  CHECK (body_calls (run_prog (p, idx), T_CONCAT) == 0);
  exec_bind (8, "pay", NULL);
  o = run_prog (p, idx);
  CHECK (o.log == tag + "vv-- ");	/* left and right: no skip mark left over */
  CHECK (body_calls (run_prog (p, idx), T_CONCAT) == 0);
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a generic step over another once-per-execution step computes once per execution too (the bounds over "
	   "CONCAT (?, '%'))", "[contract]")
{
  exec_bind (7, "pay", NULL);
  int idx = -1;
  REGU_VARIABLE *cat = arith (T_CONCAT, D (DB_TYPE_INTEGER), hv (0), lit_str ("%"));
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), cat, NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 2);
  CHECK (calls_per_eval (p, idx, T_CONCAT, { 7, 7, 8 }) == std::vector<int> ({ 1, 0, 1 }));
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 9, 9 }) == std::vector<int> ({ 1, 0 }));
  expr_prog_free (p);
  exec_unbind ();
}

/* ---- PR#8623 review T0: a copy into a step's own slot that runs out of memory (pr_clone_value () drops the error of
 * the type's setval).  The [defect] cases pass while the copy's failure is dropped; the [contract] cases are the fix. */

/* the bodies answer a VARCHAR for the case, and no armed failure outlives it */
struct string_bodies
{
  string_bodies ()
  {
    g_body_string = true;
  }
  ~string_bodies ()
  {
    g_body_string = false;
    g_alloc_fail_in = 0;
  }
};

/* LIKE_LOWER_BOUND (?): a once-per-execution generic step, which copies the body's value into its slot */
static EXPR_PROG *
compile_once_bound (int *idx)
{
  exec_bind (7, "pay%", NULL);
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_VARCHAR), hv (0), NULL), idx);
  if (p != NULL && unitseam_generic_slots (p) != 1)
    {
      expr_prog_free (p);
      return NULL;
    }
  return p;
}

/* LIKE_LOWER_BOUND (col) COLLATE: a per-row generic step and its relabel step, which copies the value into its slot */
static EXPR_PROG *
compile_relabel (int *idx)
{
  db_make_string (&g_attrs[1].v, "payx");
  exec_bind (7, NULL, NULL);
  REGU_VARIABLE *r = arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_VARCHAR), attr (1, D (DB_TYPE_VARCHAR)), NULL);
  REGU_VARIABLE_SET_FLAG (r, REGU_VARIABLE_APPLY_COLLATION);
  return compile1 (r, idx);
}

TEST_CASE ("PR#8623 T0 once: a failed copy into a once-per-execution step's slot is kept as NULL for the execution -- "
	   "the evaluation answers NULL with the allocation's error set, the later ones answer it without the body",
	   "[defect]")
{
  string_bodies scope;
  int idx = -1;
  EXPR_PROG *p = compile_once_bound (&idx);
  REQUIRE (p != NULL);
  g_alloc_fail_in = 1;
  outcome o = run_prog (p, idx);
  CHECK (o.value == "NULL");
  CHECK (g_errid == ER_OUT_OF_VIRTUAL_MEMORY);
  o = run_prog (p, idx);
  CHECK (o.value == "NULL");
  CHECK (body_calls (o, T_LIKE_LOWER_BOUND) == 0);
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a copy into a once-per-execution step's slot that runs out of memory fails the evaluation with the "
	   "allocation's error and keeps nothing -- the next evaluation of the execution computes and keeps the value",
	   "[contract]")
{
  string_bodies scope;
  int idx = -1;
  EXPR_PROG *p = compile_once_bound (&idx);
  REQUIRE (p != NULL);
  g_alloc_fail_in = 1;
  outcome o = run_prog (p, idx);
  CHECK (o.value == "ERR:" + std::to_string (ER_OUT_OF_VIRTUAL_MEMORY));
  CHECK (body_calls (o, T_LIKE_LOWER_BOUND) == 1);
  o = run_prog (p, idx);
  CHECK (o.value == "STR:pay");
  CHECK (body_calls (o, T_LIKE_LOWER_BOUND) == 1);
  o = run_prog (p, idx);
  CHECK (o.value == "STR:pay");
  CHECK (body_calls (o, T_LIKE_LOWER_BOUND) == 0);
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("PR#8623 T0 relabel: a failed copy into a COLLATE relabel step's slot answers the row a NULL with the "
	   "allocation's error set", "[defect]")
{
  string_bodies scope;
  int idx = -1;
  EXPR_PROG *p = compile_relabel (&idx);
  REQUIRE (p != NULL);
  g_alloc_fail_in = 1;
  outcome o = run_prog (p, idx);
  CHECK (o.value == "NULL");
  CHECK (g_errid == ER_OUT_OF_VIRTUAL_MEMORY);
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("a copy into a COLLATE relabel step's slot that runs out of memory fails the row with the allocation's "
	   "error -- the next row relabels its value", "[contract]")
{
  string_bodies scope;
  int idx = -1;
  EXPR_PROG *p = compile_relabel (&idx);
  REQUIRE (p != NULL);
  g_alloc_fail_in = 1;
  outcome o = run_prog (p, idx);
  CHECK (o.value == "ERR:" + std::to_string (ER_OUT_OF_VIRTUAL_MEMORY));
  o = run_prog (p, idx);
  CHECK (o.value == "STR:pay");
  CHECK (body_calls (o, T_LIKE_LOWER_BOUND) == 1);
  expr_prog_free (p);
  exec_unbind ();
}

/* ---- CBRD-28250 (PR#8775): a kernel step between two generic steps.  A kernel that converts or combines cells of one
 * value for the execution (the CAST the parser wraps a function argument in, integer arithmetic) gives one value too,
 * so the generic step over it computes once per execution; the kernel itself keeps running.  On the base the generic
 * step over a kernel computes on every evaluation. */

static REGU_VARIABLE *
cast_to (TP_DOMAIN * dom, REGU_VARIABLE * operand)
{
  return arith (T_CAST, dom, NULL, operand);
}

TEST_CASE ("CBRD-28250: a generic step over a CAST of a once-per-execution step computes once per execution too "
	   "(the shape of the bound over CONCAT (CAST (NULLIF (?, '') AS CHAR), '%'), with a cast the unit links: INT to BIGINT)", "[contract]")
{
  exec_bind (7, "pay", NULL);
  int idx = -1;
  REGU_VARIABLE *cat = arith (T_CONCAT, D (DB_TYPE_INTEGER), hv (0), lit_str ("%"));
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), cast_to (D (DB_TYPE_BIGINT), cat), NULL),
			   &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 2);
  std::vector<std::string> values;
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7, 8, 8 }, &values) == std::vector<int> ({ 1, 0, 1, 0 }));
  CHECK (values == std::vector<std::string> ({ "INT:1", "INT:1", "INT:1", "INT:1" }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("CBRD-28250: a generic step over a CAST of a column computes on every evaluation -- the kernel's cell is "
	   "a value per row", "[contract]")
{
  db_make_int (&g_attrs[3].v, 4);
  exec_bind (7, NULL, NULL);
  int idx = -1;
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER),
				  cast_to (D (DB_TYPE_BIGINT), attr (3, D (DB_TYPE_INTEGER))), NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 0);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7, 7 }) == std::vector<int> ({ 1, 1, 1 }));
  expr_prog_free (p);
  exec_unbind ();
}

TEST_CASE ("CBRD-28250: a generic step over integer arithmetic of a once-per-execution step and a column computes on "
	   "every evaluation", "[contract]")
{
  db_make_int (&g_attrs[2].v, 5);
  exec_bind (7, "pay", NULL);
  int idx = -1;
  REGU_VARIABLE *cat = arith (T_CONCAT, D (DB_TYPE_INTEGER), hv (0), lit_str ("%"));
  REGU_VARIABLE *sum = arith (T_ADD, D (DB_TYPE_INTEGER), cat, attr (2, D (DB_TYPE_INTEGER)));
  EXPR_PROG *p = compile1 (arith (T_LIKE_LOWER_BOUND, D (DB_TYPE_INTEGER), sum, NULL), &idx);
  REQUIRE (p != NULL);
  CHECK (unitseam_generic_slots (p) == 1);
  CHECK (calls_per_eval (p, idx, T_LIKE_LOWER_BOUND, { 7, 7, 7 }) == std::vector<int> ({ 1, 1, 1 }));
  expr_prog_free (p);
  exec_unbind ();
}
