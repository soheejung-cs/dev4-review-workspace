/*
 * PR#8029 (CBRD-27478 stage 1, prototype) -- the row-snapshot pass of src/optimizer/query_graph.c (CS_MODE):
 * qo_snapshot_apply () with its helpers qo_snapshot_threshold () / qo_snapshot_const_value () /
 * qo_snapshot_sarg_matches (), reached through appendix hooks (hooks.cpp).
 *
 * For a node with 0 < ncard <= QO_SNAPSHOT_ROWS whose every sarg is a constant `col = c` / `col IN (...)` /
 * `col RANGE (c1 =, ...)`, the pass fetches the rows, keeps the k that pass every sarg and, when k > 0,
 *   node selectivity := k / ncard                               (snapshot_card := k)
 *   each equi-join term col = other.col of that node :=
 *       sel = (1 / k) * SUM_i P (other.col = key_i) * (1 - nf_other)       (a NULL key adds 0)
 *   where P is histogram_get_equal_selectivity () on the other column (non-null-conditional),
 *   sel <= 0 keeps the old value, sel > 1 is clamped to 1, and the term is flagged
 *   QO_TERM_SEL_FROM_SNAPSHOT. Any failure leaves the node / the term as the histogram pass left it.
 * Every expected value below is computed by hand in the comment next to it (dyadic numbers where the
 * arithmetic is exact).
 *
 * Real code: the four TU statics, the bitset (query_bitset.c is #included below -- pure malloc code), the
 * inline dbtype accessors. Fakes for other TUs, each the branch of the real function this path takes:
 *   db_get_all_objects   db_info.c:274 -> sm_fetch_all_objects_internal (schema_manager.c:2792): the
 *                        class's instances as a DB_OBJLIST built by prepending (so reverse heap order),
 *                        NULL for an empty class (no error), NULL + er_set on failure (au_fetch_class)
 *   sm_fetch_all_objects_of_dirty_version  schema_manager.c:2768: the same list (not called yet -- see there)
 *   db_objlist_free      db_admin.c:2811 -> ml_ext_free of every link
 *   db_get               db_obj.c:234 -> obj_get (object_accessor.c): copies the attribute; an unknown
 *                        attribute or a failed read -> er_set + the error code (find_attribute)
 *   histogram_get_equal_selectivity  histogram_cl.cpp:1032: no histogram on the column, or a key of another
 *                        kind -> success false and selectivity untouched; otherwise the
 *                        non-null-conditional fraction in (0, 1] and success true (1188-1209)
 *   tp_value_compare     object_domain.c tp_value_compare_with_error: under total_order (NULL,NULL)=EQ,
 *                        (NULL,x)=LT, (x,NULL)=GT; integers by value (the only type the scenes use)
 *   db_set_size / db_set_get  db_set.c:559 / 510 on the test's set handle (element copy, range error)
 *   pr_clear_value       object_primitive.c: releases an owned value -- integers own nothing
 *   er_set / er_errid / er_stack_push / er_stack_pop  error_manager.c: push saves the current entry and
 *                        starts a clean one, pop restores the saved one, er_set overwrites the current one
 *
 * Scene: node 0 "it" (small, the snapshot candidate) and node 1 "mi" (large, ncard 1,000,000). Integer columns.
 * The QO_NODE / QO_TERM / PT_NODE structures are filled the way qo_add_node () / qo_add_term () /
 * qo_node_add_sarg () leave them for these fields; nothing else of the env is read by the pass.
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "error_code.h"
#include "error_manager.h"
#include "object_primitive.h"
#include "optimizer.h"
#include "query_graph.h"
#include "query_planner.h"
#include "histogram_cl.hpp"
#include "schema_manager.h"
#include "db.h"
#include "object_domain.h"
#include "dbtype.h"
#include "db_set_function.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

/* the real bitset: query_graph.c calls bitset_iterate / bitset_next_member / bitset_is_empty, and the test
 * builds sargs / term node sets with bitset_init / bitset_add */
#include "query_bitset.c"

/* ---- appendix hooks (compiled inside query_graph.c) ---- */
int unitseam_snap_threshold (void);
DB_VALUE *unitseam_snap_const_value (QO_ENV * env, PT_NODE * node);
int unitseam_snap_sarg_matches (QO_ENV * env, QO_NODE * node, QO_TERM * sarg, DB_OBJECT * obj);
void unitseam_snap_apply (QO_ENV * env);

typedef std::optional<int> ival;	/* nullopt = SQL NULL */

/* ------------------------------------------------------------------ fake state */

static std::vector<void *> g_alloc;	/* everything the scene allocated; freed by reset () */
static std::map<DB_OBJECT *, std::vector<DB_OBJECT *>> g_extent;	/* class mop -> instances, heap order */
static std::map<DB_OBJECT *, std::map<std::string, ival>> g_row;	/* instance mop -> column values */
static std::set<std::pair<DB_OBJECT *, std::string>> g_get_fail;	/* (instance, column) whose read fails */
static std::set<DB_OBJECT *> g_fetch_fail;	/* classes whose fetch fails */
static int g_fetch_calls, g_links_live, g_db_get_calls, g_clear_calls, g_cmp_unsupported;

struct fake_hist
{
  double other;			/* P (col = v) for a value that is not listed */
  std::map<int, double> mcv;	/* P (col = v) for listed values */
};
static std::map<std::string, fake_hist> g_hist;	/* column name -> its histogram; absent = none */
static std::vector<int> g_hist_keys;	/* keys priced, in call order */

static std::map<DB_COLLECTION *, std::vector<ival>> g_sets;

static int g_er_cur;		/* the current error entry's id */
static std::vector<int> g_er_saved;	/* entries saved by er_stack_push */
static int g_er_bad_pops, g_er_sets;

static void *
zalloc (size_t n)
{
  void *p = calloc (1, n);
  g_alloc.push_back (p);
  return p;
}

static DB_OBJECT *
new_mop (void)
{
  /* never dereferenced by the pass or the fakes: only compared and used as a map key */
  return (DB_OBJECT *) zalloc (16);
}

/* ------------------------------------------------------------------ fakes */

/* db_info.c:274 db_get_all_objects -> sm_fetch_all_objects (op, DB_FETCH_QUERY_READ):
 * sm_fetch_all_objects_internal prepends every non-deleted instance (new_->next = objects), so the list
 * comes out in reverse heap order; an empty class gives NULL with no error; a class that cannot be read
 * gives NULL after au_fetch_class er_set ()s */
DB_OBJLIST *
db_get_all_objects (DB_OBJECT * op)
{
  DB_OBJLIST *list = NULL, *link;

  g_fetch_calls++;
  if (g_fetch_fail.count (op))
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_AU_SELECT_FAILURE, 0);
      return NULL;
    }
  auto it = g_extent.find (op);
  if (it == g_extent.end ())
    {
      return NULL;
    }
  for (DB_OBJECT *inst:it->second)
    {
      link = (DB_OBJLIST *) malloc (sizeof (DB_OBJLIST));
      link->op = inst;
      link->next = list;
      list = link;
      g_links_live++;
    }
  return list;
}

/* schema_manager.c:2768 -> sm_fetch_all_objects_internal (op, purpose, LC_FETCH_DIRTY_VERSION): the same list.
 * Not called at cc3c4e821; it is the lock-free fetch the design review (Q2, option B) chose for the next
 * revision, faked here so the suite keeps running when the pass switches to it. */
DB_OBJLIST *
sm_fetch_all_objects_of_dirty_version (DB_OBJECT * op, DB_FETCH_MODE purpose)
{
  return db_get_all_objects (op);
}

/* db_admin.c:2811 -> ml_ext_free (list): every link */
void
db_objlist_free (DB_OBJLIST * list)
{
  while (list != NULL)
    {
      DB_OBJLIST *next = list->next;
      free (list);
      g_links_live--;
      list = next;
    }
}

/* db_obj.c:234 db_get -> obj_get_path -> obj_get: find_attribute () fails with er_set for an unknown
 * attribute; otherwise the attribute value is copied out (NULL stays NULL) */
int
db_get (DB_OBJECT * object, const char *attpath, DB_VALUE * value)
{
  g_db_get_calls++;
  auto r = g_row.find (object);
  if (r == g_row.end () || g_get_fail.count ({ object, attpath }) || r->second.find (attpath) == r->second.end ())
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OBJ_INVALID_ATTRIBUTE, 1, attpath);
      return ER_OBJ_INVALID_ATTRIBUTE;
    }
  const ival & v = r->second.find (attpath)->second;
  if (v)
    {
      db_make_int (value, *v);
    }
  else
    {
      db_make_null (value);
    }
  return NO_ERROR;
}

/* object_primitive.c: frees what the value owns -- integers and NULL own nothing */
int
pr_clear_value (DB_VALUE * var)
{
  g_clear_calls++;
  return NO_ERROR;
}

/* object_domain.c tp_value_compare -> tp_value_compare_with_error: the NULL branches as written there, then
 * integers by value. Any other type is outside what these scenes build (counted, so a case can assert 0). */
DB_VALUE_COMPARE_RESULT
tp_value_compare (const DB_VALUE * value1, const DB_VALUE * value2, int allow_coercion, int total_order)
{
  if (DB_IS_NULL (value1))
    {
      return DB_IS_NULL (value2) ? (total_order ? DB_EQ : DB_UNK) : (total_order ? DB_LT : DB_UNK);
    }
  if (DB_IS_NULL (value2))
    {
      return total_order ? DB_GT : DB_UNK;
    }
  if (value1->domain.general_info.type == DB_TYPE_INTEGER && value2->domain.general_info.type == DB_TYPE_INTEGER)
    {
      return DB_INT_TO_COMPARE_RESULT ((value1->data.i > value2->data.i) - (value1->data.i < value2->data.i));
    }
  g_cmp_unsupported++;
  return DB_UNK;
}

/* db_set.c:559 -> set_size */
int
db_set_size (DB_SET * set)
{
  auto s = g_sets.find ((DB_COLLECTION *) set);
  return (s == g_sets.end ())? -1 : (int) s->second.size ();
}

/* db_set.c:510 -> set_get_element: copy of element `index`, ER_SET_INVALID_INDEX outside */
int
db_set_get (DB_SET * set, int element_index, DB_VALUE * value)
{
  auto s = g_sets.find ((DB_COLLECTION *) set);
  if (s == g_sets.end () || element_index < 0 || element_index >= (int) s->second.size ())
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_SET_INVALID_INDEX, 1, element_index);
      return ER_SET_INVALID_INDEX;
    }
  if (s->second[element_index])
    {
      db_make_int (value, *s->second[element_index]);
    }
  else
    {
      db_make_null (value);
    }
  return NO_ERROR;
}

/* histogram_cl.cpp:1032: rhs NULL / no histogram (histogram_init_reader_from_lhs fails) / a key of another
 * kind -> success false, selectivity untouched. Otherwise an MCV's frequency or the residual share, divided
 * by (1 - null_frequency) and kept in (0, 1] -- the test states the final number directly. */
void
histogram_get_equal_selectivity (PT_NODE * lhs, DB_VALUE * rhs_db_value, double *selectivity, bool *success)
{
  if (rhs_db_value == NULL || DB_IS_NULL (rhs_db_value))
    {
      *success = false;
      return;
    }
  auto h = g_hist.find (lhs->info.name.original);
  if (h == g_hist.end () || rhs_db_value->domain.general_info.type != DB_TYPE_INTEGER)
    {
      *success = false;
      return;
    }
  g_hist_keys.push_back (rhs_db_value->data.i);
  auto m = h->second.mcv.find (rhs_db_value->data.i);
  *selectivity = (m != h->second.mcv.end ())? m->second : h->second.other;
  *success = true;
}

/* error_manager.c: er_set overwrites the current entry; er_stack_push saves it and starts a clean one
 * (context::push_error_stack); er_stack_pop drops the current one and restores the saved one */
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  g_er_sets++;
  g_er_cur = err_id;
}

int
er_errid (void)
{
  return g_er_cur;
}

void
er_stack_push (void)
{
  g_er_saved.push_back (g_er_cur);
  g_er_cur = NO_ERROR;
}

void
er_stack_pop (void)
{
  if (g_er_saved.empty ())
    {
      g_er_bad_pops++;
      return;
    }
  g_er_cur = g_er_saved.back ();
  g_er_saved.pop_back ();
}

/* ------------------------------------------------------------------ scene */

static PARSER_CONTEXT *g_parser;
static QO_ENV *g_env;

static void
reset (void)
{
  for (void *p:g_alloc)
    {
      free (p);
    }
  g_alloc.clear ();
  g_extent.clear ();
  g_row.clear ();
  g_get_fail.clear ();
  g_fetch_fail.clear ();
  g_hist.clear ();
  g_hist_keys.clear ();
  g_sets.clear ();
  g_fetch_calls = g_links_live = g_db_get_calls = g_clear_calls = g_cmp_unsupported = 0;
  g_er_cur = NO_ERROR;
  g_er_saved.clear ();
  g_er_bad_pops = g_er_sets = 0;
  unsetenv ("QO_SNAPSHOT_ROWS");
  g_parser = NULL;
  g_env = NULL;
}

static void
snapshot_rows (const char *v)
{
  if (v == NULL)
    {
      unsetenv ("QO_SNAPSHOT_ROWS");
    }
  else
    {
      setenv ("QO_SNAPSHOT_ROWS", v, 1);
    }
}

/* host_var_count user '?' first, then auto_param_count auto-parameterized literals (?:n) */
static void
scene_begin (int max_nodes, int max_terms, int host_var_count, int auto_param_count)
{
  int i;

  reset ();
  g_parser = (PARSER_CONTEXT *) zalloc (sizeof (PARSER_CONTEXT));
  g_parser->host_var_count = host_var_count;
  g_parser->auto_param_count = auto_param_count;
  if (host_var_count + auto_param_count > 0)
    {
      g_parser->host_variables = (DB_VALUE *) zalloc (sizeof (DB_VALUE) * (host_var_count + auto_param_count));
      for (i = 0; i < host_var_count + auto_param_count; i++)
	{
	  db_make_null (&g_parser->host_variables[i]);
	}
    }
  g_env = (QO_ENV *) zalloc (sizeof (QO_ENV));
  g_env->parser = g_parser;
  g_env->nodes = (QO_NODE *) zalloc (sizeof (QO_NODE) * max_nodes);
  g_env->terms = (QO_TERM *) zalloc (sizeof (QO_TERM) * max_terms);
  g_env->Nnodes = max_nodes;
  g_env->Nterms = max_terms;
}

static PT_NODE *
mk (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) zalloc (sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

/* qo_add_node (): one class, its statistics row count, snapshot_card -1, selectivity 1.0 */
static QO_NODE *
add_node (const char *name, unsigned long ncard)
{
  QO_NODE *node = QO_ENV_NODE (g_env, g_env->nnodes);
  PT_NODE *spec = mk (PT_SPEC);
  QO_CLASS_INFO *info = (QO_CLASS_INFO *) zalloc (sizeof (QO_CLASS_INFO));

  spec->info.spec.id = (UINTPTR) spec;
  info->n = 1;
  info->info[0].name = name;
  info->info[0].mop = new_mop ();
  node->env = g_env;
  node->entity_spec = spec;
  node->info = info;
  node->ncard = ncard;
  node->tcard = 1;
  node->class_name = name;
  node->idx = g_env->nnodes;
  node->selectivity = 1.0;
  node->snapshot_card = -1;
  bitset_init (&node->sargs, g_env);
  g_extent[info->info[0].mop];	/* the class exists, no rows yet */
  g_env->nnodes++;
  return node;
}

static DB_OBJECT *
add_row (QO_NODE * node, std::initializer_list<std::pair<const char *, ival>> cols)
{
  DB_OBJECT *obj = new_mop ();
  for (auto & c:cols)
    {
      g_row[obj][c.first] = c.second;
    }
  g_extent[node->info->info[0].mop].push_back (obj);
  return obj;
}

static PT_NODE *
col (QO_NODE * node, const char *name, double null_frequency = 0.0)
{
  PT_NODE *n = mk (PT_NAME);
  n->info.name.original = name;
  n->info.name.spec_id = node->entity_spec->info.spec.id;
  n->info.name.null_frequency = null_frequency;
  n->type_enum = PT_TYPE_INTEGER;
  return n;
}

static PT_NODE *
lit (int v)
{
  PT_NODE *n = mk (PT_VALUE);
  n->type_enum = PT_TYPE_INTEGER;
  db_make_int (&n->info.value.db_value, v);
  n->info.value.db_value_is_initialized = 1;
  return n;
}

/* a host variable (?:idx) and its bound value */
static PT_NODE *
hv (int idx, ival v, PT_MISC_TYPE var_type = PT_HOST_IN)
{
  PT_NODE *n = mk (PT_HOST_VAR);
  n->info.host_var.var_type = var_type;
  n->info.host_var.index = idx;
  n->type_enum = PT_TYPE_INTEGER;
  if (g_parser->host_variables != NULL && idx < g_parser->host_var_count + g_parser->auto_param_count)
    {
      if (v)
	{
	  db_make_int (&g_parser->host_variables[idx], *v);
	}
      else
	{
	  db_make_null (&g_parser->host_variables[idx]);
	}
    }
  return n;
}

static PT_NODE *
ex (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2)
{
  PT_NODE *n = mk (PT_EXPR);
  n->info.expr.op = op;
  n->info.expr.arg1 = a1;
  n->info.expr.arg2 = a2;
  n->type_enum = PT_TYPE_LOGICAL;
  return n;
}

/* `(c1, c2, ...)` kept as an element list: PT_FUNCTION of a set type, elements chained by next */
static PT_NODE *
fn_set (std::initializer_list<PT_NODE *> elems)
{
  PT_NODE *f = mk (PT_FUNCTION), *prev = NULL;
  f->type_enum = PT_TYPE_SEQUENCE;
  for (PT_NODE * e:elems)
    {
      if (prev == NULL)
	{
	  f->info.function.arg_list = e;
	}
      else
	{
	  prev->next = e;
	}
      prev = e;
    }
  return f;
}

/* `{c1, c2, ...}` folded into one set VALUE */
static PT_NODE *
val_set (std::vector<ival> elems)
{
  PT_NODE *n = mk (PT_VALUE);
  DB_COLLECTION *h = (DB_COLLECTION *) zalloc (16);
  g_sets[h] = elems;
  n->type_enum = PT_TYPE_SEQUENCE;
  n->info.value.db_value.domain.general_info.type = DB_TYPE_SEQUENCE;
  n->info.value.db_value.domain.general_info.is_null = 0;
  n->info.value.db_value.need_clear = false;
  n->info.value.db_value.data.set = (DB_SET *) h;
  n->info.value.db_value_is_initialized = 1;
  return n;
}

/* `RANGE (c1 =, c2 =, ...)`: PT_BETWEEN_EQ_NA elements chained by or_next, constant in arg1 */
static PT_NODE *
eq_ranges (std::initializer_list<PT_NODE *> consts)
{
  PT_NODE *head = NULL, *prev = NULL;
  for (PT_NODE * c:consts)
    {
      PT_NODE *r = ex (PT_BETWEEN_EQ_NA, c, NULL);
      if (prev == NULL)
	{
	  head = r;
	}
      else
	{
	  prev->or_next = r;
	}
      prev = r;
    }
  return head;
}

static const int SINGLE_EQ = QO_TERM_SINGLE_PRED | QO_TERM_EQUAL_OP;

static QO_TERM *
add_term (QO_TERMCLASS tc, PT_NODE * pt_expr, double sel, int flags, std::initializer_list<int> nodes)
{
  QO_TERM *term = QO_ENV_TERM (g_env, g_env->nterms);
  term->env = g_env;
  term->idx = g_env->nterms;
  term->term_class = tc;
  term->pt_expr = pt_expr;
  term->selectivity = sel;
  term->flag = flags;
  bitset_init (&term->nodes, g_env);
  for (int n:nodes)
    {
      bitset_add (&term->nodes, n);
    }
  g_env->nterms++;
  return term;
}

/* qo_node_add_sarg () (query_graph.c:9521): add to the node's sargs, multiply the node selectivity, floor
 * at 1 / ncard */
static QO_TERM *
add_sarg (QO_NODE * node, PT_NODE * pt_expr, double sel = 0.25, int flags = SINGLE_EQ, QO_TERMCLASS tc = QO_TC_SARG)
{
  QO_TERM *t = add_term (tc, pt_expr, sel, flags, { node->idx });
  double floor_ = (node->ncard == 0) ? 0 : 1.0 / (double) node->ncard;
  bitset_add (&node->sargs, t->idx);
  node->selectivity *= sel;
  if (node->selectivity < floor_)
    {
      node->selectivity = floor_;
    }
  return t;
}

static QO_TERM *
add_join (QO_NODE * a, QO_NODE * b, PT_NODE * pt_expr, double sel, int flags = SINGLE_EQ)
{
  return add_term (QO_TC_JOIN, pt_expr, sel, flags, { a->idx, b->idx });
}

static bool
from_snapshot (QO_TERM * t)
{
  return QO_TERM_IS_FLAGED (t, QO_TERM_SEL_FROM_SNAPSHOT) != 0;
}

/* nothing leaked: the error entry is what it was before the pass, every push popped, every link freed */
static void
require_clean (int er_before = NO_ERROR)
{
  REQUIRE (g_er_saved.empty ());
  REQUIRE (g_er_bad_pops == 0);
  REQUIRE (g_er_cur == er_before);
  REQUIRE (g_links_live == 0);
  REQUIRE (g_cmp_unsupported == 0);
}

/*
 * The standard scene.
 *   it (ncard 5)            mi (ncard 1,000,000; never a candidate)
 *   id kind grp
 *    1   7    1
 *    2   3    1
 *    3   7    2
 *    4   9    1
 *    5   3    2
 *   sarg  it.kind = ?:0 (auto-parameterized literal 7; host_var_count 0, auto_param_count 1), histogram sel 0.25
 *   join  it.id = mi.itid, histogram sel 0.01; mi.itid null_frequency 0.5
 *   histogram of mi.itid: P(=1) 0.25, P(=3) 0.125, P(=other) 1/1024
 */
struct std_scene
{
  QO_NODE *it, *mi;
  QO_TERM *sarg, *join;
};

static const double NF_MI = 0.5;

static void
std_hist (void)
{
  g_hist["itid"] = fake_hist { 1.0 / 1024, { { 1, 0.25 }, { 3, 0.125 } } };
}

static void
std_rows (QO_NODE * it)
{
  add_row (it, { { "id", 1 }, { "kind", 7 }, { "grp", 1 } });
  add_row (it, { { "id", 2 }, { "kind", 3 }, { "grp", 1 } });
  add_row (it, { { "id", 3 }, { "kind", 7 }, { "grp", 2 } });
  add_row (it, { { "id", 4 }, { "kind", 9 }, { "grp", 1 } });
  add_row (it, { { "id", 5 }, { "kind", 3 }, { "grp", 2 } });
}

static std_scene
std_build (bool with_rows = true, PT_NODE * (*filter) (QO_NODE *) = NULL)
{
  std_scene s;
  scene_begin (4, 16, 0, 1);
  s.it = add_node ("it", 5);
  s.mi = add_node ("mi", 1000000);
  if (with_rows)
    {
      std_rows (s.it);
    }
  s.sarg = add_sarg (s.it, filter ? filter (s.it) : ex (PT_EQ, col (s.it, "kind"), hv (0, 7)));
  s.join = add_join (s.it, s.mi, ex (PT_EQ, col (s.it, "id"), col (s.mi, "itid", NF_MI)), 0.01);
  std_hist ();
  return s;
}

/* ================================================================== qo_snapshot_threshold */

TEST_CASE ("threshold: QO_SNAPSHOT_ROWS unset/empty/non-positive/non-numeric is off, a count is taken as is, "
	   "anything above 1,000,000 is capped", "[contract]")
{
  reset ();
  snapshot_rows (NULL);
  CHECK (unitseam_snap_threshold () == 0);
  const std::pair<const char *, int> table[] = {
    { "", 0 }, { "0", 0 }, { "-3", 0 }, { "abc", 0 },
    { "1", 1 }, { "100", 100 }, { " 42", 42 }, { "12abc", 12 },
    { "1000000", 1000000 }, { "1000001", 1000000 }, { "99999999999999999999", 1000000 }
  };
  for (auto & e:table)
    {
      snapshot_rows (e.first);
      INFO ("QO_SNAPSHOT_ROWS='" << e.first << "'");
      CHECK (unitseam_snap_threshold () == e.second);
    }
  reset ();
}

/* ================================================================== qo_snapshot_const_value */

TEST_CASE ("const value: only auto-parameterized host variables [host_var_count, +auto_param_count) and "
	   "initialized scalar VALUEs are constants", "[contract]")
{
  scene_begin (1, 1, 2, 2);	/* ?0 ?1 are user host variables, ?:2 ?:3 auto-parameterized literals */
  PT_NODE *user0 = hv (0, 5), *user1 = hv (1, 5), *auto2 = hv (2, 7), *auto3 = hv (3, 8), *past = hv (4, std::nullopt);
  PT_NODE *out = hv (2, 7, PT_HOST_OUT);

  CHECK (unitseam_snap_const_value (g_env, user0) == NULL);
  CHECK (unitseam_snap_const_value (g_env, user1) == NULL);
  CHECK (unitseam_snap_const_value (g_env, auto2) == &g_parser->host_variables[2]);
  CHECK (unitseam_snap_const_value (g_env, auto3) == &g_parser->host_variables[3]);
  CHECK (unitseam_snap_const_value (g_env, past) == NULL);
  CHECK (unitseam_snap_const_value (g_env, out) == NULL);
  CHECK (unitseam_snap_const_value (g_env, NULL) == NULL);

  PT_NODE *v = lit (7);
  CHECK (unitseam_snap_const_value (g_env, v) == &v->info.value.db_value);
  PT_NODE *uninit = lit (7);
  uninit->info.value.db_value_is_initialized = 0;
  CHECK (unitseam_snap_const_value (g_env, uninit) == NULL);
  PT_NODE *null_lit = lit (0);
  null_lit->type_enum = PT_TYPE_NULL;
  CHECK (unitseam_snap_const_value (g_env, null_lit) == NULL);
  CHECK (unitseam_snap_const_value (g_env, val_set ({ 1, 2 })) == NULL);
  CHECK (unitseam_snap_const_value (g_env, mk (PT_NAME)) == NULL);

  g_parser->host_variables = NULL;	/* nothing bound at all */
  CHECK (unitseam_snap_const_value (g_env, auto2) == NULL);
  reset ();
}

/* ================================================================== qo_snapshot_sarg_matches */

TEST_CASE ("sarg match: NULL obj asks evaluability; a row matches by value; a NULL column value never matches",
	   "[contract]")
{
  std_scene s = std_build (false);
  DB_OBJECT *r7 = add_row (s.it, { { "id", 1 }, { "kind", 7 } });
  DB_OBJECT *r3 = add_row (s.it, { { "id", 2 }, { "kind", 3 } });
  DB_OBJECT *rn = add_row (s.it, { { "id", 3 }, { "kind", std::nullopt } });

  CHECK (unitseam_snap_sarg_matches (g_env, s.it, s.sarg, NULL) == 1);
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, s.sarg, r7) == 1);
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, s.sarg, r3) == 0);
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, s.sarg, rn) == 0);

  /* IN with a NULL element still does not match a NULL row (NULL = NULL is not true) */
  QO_TERM *in_null = add_sarg (s.it, ex (PT_IS_IN, col (s.it, "kind"), val_set ({ 7, std::nullopt })));
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, in_null, rn) == 0);
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, in_null, r7) == 1);

  /* a failed read of the column is "cannot evaluate" (-1), not "no match" */
  g_get_fail.insert ({ r3, "kind" });
  CHECK (unitseam_snap_sarg_matches (g_env, s.it, s.sarg, r3) == -1);
  REQUIRE (g_cmp_unsupported == 0);
  reset ();
}

TEST_CASE ("sarg match: shapes the pass cannot evaluate report -1 (LIKE, <, user ?, OR, not single, "
	   "column rhs, other spec, IN subquery, empty set, non-EQ range)", "[contract]")
{
  scene_begin (4, 32, 1, 1);	/* ?0 user, ?:1 auto */
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);

  struct shape
  {
    const char *what;
    QO_TERM *t;
  } shapes[] = {
    { "LIKE", add_sarg (it, ex (PT_LIKE, col (it, "kind"), lit (7)), 0.25, QO_TERM_SINGLE_PRED) },
    { "<", add_sarg (it, ex (PT_LT, col (it, "kind"), lit (7)), 0.25, QO_TERM_SINGLE_PRED) },
    { "user host variable", add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7))) },
    { "OR_PRED flag", add_sarg (it, ex (PT_EQ, col (it, "kind"), lit (7)), 0.25, SINGLE_EQ | QO_TERM_OR_PRED) },
    { "not SINGLE_PRED", add_sarg (it, ex (PT_EQ, col (it, "kind"), lit (7)), 0.25, QO_TERM_EQUAL_OP) },
    { "rhs is a column", add_sarg (it, ex (PT_EQ, col (it, "kind"), col (it, "grp"))) },
    { "lhs on another spec", add_sarg (it, ex (PT_EQ, col (mi, "kind"), lit (7))) },
    { "IN subquery", add_sarg (it, ex (PT_IS_IN, col (it, "kind"), mk (PT_SELECT)), 0.25, QO_TERM_SINGLE_PRED) },
    { "IN empty set VALUE", add_sarg (it, ex (PT_IS_IN, col (it, "kind"), val_set ({})), 0.25, QO_TERM_SINGLE_PRED) },
    { "IN list with a column", add_sarg (it, ex (PT_IS_IN, col (it, "kind"), fn_set ({ lit (7), col (it, "grp") })),
					 0.25, QO_TERM_SINGLE_PRED) },
    { "RANGE with a non-EQ element", nullptr },
    { "lhs is an expression", add_sarg (it, ex (PT_EQ, ex (PT_PLUS, col (it, "kind"), lit (1)), lit (8))) },
  };
  PT_NODE *r = eq_ranges ({ lit (7), lit (9) });
  r->or_next->info.expr.op = PT_BETWEEN_GE_LE;
  shapes[10].t = add_sarg (it, ex (PT_RANGE, col (it, "kind"), r), 0.25, QO_TERM_SINGLE_PRED | QO_TERM_RANGELIST);
  /* an OR chain on the expression itself */
  PT_NODE *orx = ex (PT_EQ, col (it, "kind"), lit (7));
  orx->or_next = ex (PT_EQ, col (it, "kind"), lit (9));
  QO_TERM *or_chain = add_sarg (it, orx, 0.25, SINGLE_EQ);

  for (auto & sh:shapes)
    {
      INFO (sh.what);
      CHECK (unitseam_snap_sarg_matches (g_env, it, sh.t, NULL) == -1);
    }
  CHECK (unitseam_snap_sarg_matches (g_env, it, or_chain, NULL) == -1);
  reset ();
}

/* ================================================================== qo_snapshot_apply */

TEST_CASE ("off: QO_SNAPSHOT_ROWS unset or 0 leaves every node and term as the histogram pass left them, "
	   "with no fetch", "[contract]")
{
  for (const char *v: { (const char *) NULL, "0", "-1"})
    {
      std_scene s = std_build ();
      snapshot_rows (v);
      double node_sel = s.it->selectivity;
      unitseam_snap_apply (g_env);
      INFO ("QO_SNAPSHOT_ROWS=" << (v ? v : "(unset)"));
      CHECK (g_fetch_calls == 0);
      CHECK (s.it->selectivity == node_sel);
      CHECK (s.it->snapshot_card == -1);
      CHECK (s.join->selectivity == 0.01);
      CHECK_FALSE (from_snapshot (s.join));
      require_clean ();
    }
  reset ();
}

TEST_CASE ("re-derivation: node sel = k/ncard and join sel = mean over survivors of P(other = key) * (1 - nf)",
	   "[contract]")
{
  std_scene s = std_build ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);

  /* kind = 7 keeps ids 1 and 3: k = 2 of ncard 5 */
  CHECK (g_fetch_calls == 1);
  CHECK (s.it->snapshot_card == 2);
  CHECK (s.it->selectivity == 2.0 / 5.0);
  /* (P(1) + P(3)) / 2 * (1 - 0.5) = (0.25 + 0.125) / 2 * 0.5 = 0.09375 (exact) */
  CHECK (s.join->selectivity == 0.09375);
  CHECK (from_snapshot (s.join));
  CHECK ((s.join->flag & SINGLE_EQ) == SINGLE_EQ);	/* the other flags stay */
  /* the large node is not a candidate (1,000,000 > 100) and its fields stay */
  CHECK (s.mi->snapshot_card == -1);
  CHECK (s.mi->selectivity == 1.0);
  /* the sarg term keeps its histogram value (stage-2 item, commented in the code) */
  CHECK (s.sarg->selectivity == 0.25);
  CHECK_FALSE (from_snapshot (s.sarg));
  /* one price per surviving key; 5 sarg reads + 2 key reads */
  std::vector<int> keys = g_hist_keys;
  std::sort (keys.begin (), keys.end ());
  CHECK (keys == std::vector<int> ({ 1, 3 }));
  CHECK (g_db_get_calls == 7);
  require_clean ();
  reset ();
}

TEST_CASE ("re-derivation: the small node may be either side of the term (mi.itid = it.id)", "[contract]")
{
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
  std_rows (it);
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *join = add_join (mi, it, ex (PT_EQ, col (mi, "itid", NF_MI), col (it, "id")), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (it->snapshot_card == 2);
  CHECK (join->selectivity == 0.09375);
  CHECK (from_snapshot (join));
  require_clean ();
  reset ();
}

static PT_NODE *
f_literal (QO_NODE * it)
{
  return ex (PT_EQ, col (it, "kind"), lit (7));
}

TEST_CASE ("re-derivation: a plain literal VALUE constant selects the same rows as the auto-parameter",
	   "[contract]")
{
  std_scene s = std_build (true, f_literal);
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (s.it->snapshot_card == 2);
  CHECK (s.join->selectivity == 0.09375);
  require_clean ();
  reset ();
}

static PT_NODE *
f_in_list (QO_NODE * it)
{
  return ex (PT_IS_IN, col (it, "kind"), fn_set ({ hv (0, 7), lit (9) }));
}

static PT_NODE *
f_in_value (QO_NODE * it)
{
  return ex (PT_IS_IN, col (it, "kind"), val_set ({ 7, 9 }));
}

static PT_NODE *
f_eq_some (QO_NODE * it)
{
  return ex (PT_EQ_SOME, col (it, "kind"), val_set ({ 9, 7 }));
}

static PT_NODE *
f_range (QO_NODE * it)
{
  return ex (PT_RANGE, col (it, "kind"), eq_ranges ({ hv (0, 7), lit (9) }));
}

TEST_CASE ("re-derivation: IN element list, IN set VALUE, =SOME set and RANGE (c =, ...) all keep kind in {7, 9}",
	   "[contract]")
{
  struct
  {
    const char *what;
    PT_NODE *(*f) (QO_NODE *);
  } forms[] = { { "IN (?:0, 9)", f_in_list }, { "IN {7, 9}", f_in_value }, { "=SOME {9, 7}", f_eq_some },
		{ "RANGE (?:0 =, 9 =)", f_range } };
  for (auto & form:forms)
    {
      INFO (form.what);
      std_scene s = std_build (true, form.f);
      s.sarg->flag = SINGLE_EQ | ((form.f == f_range) ? QO_TERM_RANGELIST : 0);
      snapshot_rows ("100");
      unitseam_snap_apply (g_env);
      /* ids 1, 3, 4: k = 3; (0.25 + 0.125 + 1/1024) / 3 * 0.5 */
      CHECK (s.it->snapshot_card == 3);
      CHECK (s.it->selectivity == 3.0 / 5.0);
      CHECK (s.join->selectivity == Approx ((0.25 + 0.125 + 1.0 / 1024) / 3 * 0.5).epsilon (1e-12));
      CHECK (from_snapshot (s.join));
      require_clean ();
    }
  reset ();
}

TEST_CASE ("re-derivation: several constant sargs are ANDed row by row", "[contract]")
{
  std_scene s = std_build ();
  add_sarg (s.it, ex (PT_EQ, col (s.it, "grp"), lit (1)));
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  /* kind = 7 AND grp = 1 keeps id 1 only: k = 1; P(1) * 0.5 = 0.125 */
  CHECK (s.it->snapshot_card == 1);
  CHECK (s.it->selectivity == 1.0 / 5.0);
  CHECK (s.join->selectivity == 0.125);
  require_clean ();
  reset ();
}

TEST_CASE ("re-derivation: duplicate keys among the survivors count once per row (no NDV collapse)", "[contract]")
{
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
  add_row (it, { { "id", 1 }, { "kind", 7 } });
  add_row (it, { { "id", 1 }, { "kind", 7 } });
  add_row (it, { { "id", 1 }, { "kind", 7 } });
  add_row (it, { { "id", 2 }, { "kind", 7 } });
  add_row (it, { { "id", 9 }, { "kind", 3 } });
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  g_hist["itid"] = fake_hist { 1.0 / 1024, { { 1, 0.25 }, { 2, 0.125 } } };
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  /* keys 1, 1, 1, 2: k = 4; (3 * 0.25 + 0.125) / 4 * 0.5 = 0.875 / 4 * 0.5 = 0.109375 (exact) --
   * the join output is SUM_i count(mi.itid = key_i), so a repeated key is counted per row */
  CHECK (it->snapshot_card == 4);
  CHECK (it->selectivity == 4.0 / 5.0);
  CHECK (join->selectivity == 0.109375);
  CHECK (g_hist_keys.size () == 4);
  require_clean ();
  reset ();
}

TEST_CASE ("re-derivation: a NULL join key stays in k and adds 0; the histogram is not asked for it",
	   "[contract]")
{
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
  add_row (it, { { "id", 1 }, { "kind", 7 } });
  add_row (it, { { "id", std::nullopt }, { "kind", 7 } });
  add_row (it, { { "id", 3 }, { "kind", 7 } });
  add_row (it, { { "id", std::nullopt }, { "kind", 7 } });
  add_row (it, { { "id", 5 }, { "kind", 3 } });
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  /* k = 4 (two NULL keys); (0.25 + 0 + 0.125 + 0) / 4 * 0.5 = 0.046875 (exact) */
  CHECK (it->snapshot_card == 4);
  CHECK (join->selectivity == 0.046875);
  CHECK (g_hist_keys.size () == 2);
  require_clean ();
  reset ();
}

TEST_CASE ("all surviving join keys NULL: node becomes k/ncard but the join term keeps its histogram estimate "
	   "(current behaviour -- one non-NULL key among them drives it to P/k instead)", "[contract]")
{
  /* every survivor's key is NULL: sum 0 -> sel 0 -> "nothing to say", the term keeps 0.01 */
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
  for (int i = 0; i < 4; i++)
    {
      add_row (it, { { "id", std::nullopt }, { "kind", 7 } });
    }
  add_row (it, { { "id", 5 }, { "kind", 3 } });
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (it->snapshot_card == 4);
  CHECK (it->selectivity == 4.0 / 5.0);
  CHECK (join->selectivity == 0.01);
  CHECK_FALSE (from_snapshot (join));
  CHECK (g_hist_keys.empty ());
  require_clean ();

  /* contrast: three NULL keys and one key priced at 1/1024 -> (1/1024) / 4 * 0.5 = 1/8192, far below 0.01 */
  scene_begin (4, 16, 0, 1);
  it = add_node ("it", 5);
  mi = add_node ("mi", 1000000);
  for (int i = 0; i < 3; i++)
    {
      add_row (it, { { "id", std::nullopt }, { "kind", 7 } });
    }
  add_row (it, { { "id", 42 }, { "kind", 7 } });
  add_row (it, { { "id", 5 }, { "kind", 3 } });
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (join->selectivity == 1.0 / 8192);
  CHECK (from_snapshot (join));
  require_clean ();
  reset ();
}

TEST_CASE ("zero survivors: nothing changes (node, term, snapshot_card), the fetched list is freed", "[contract]")
{
  std_scene s = std_build ();
  db_make_int (&g_parser->host_variables[0], 42);	/* kind = 42 matches no row */
  double node_sel = s.it->selectivity;
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 1);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.it->selectivity == node_sel);
  CHECK (s.join->selectivity == 0.01);
  CHECK_FALSE (from_snapshot (s.join));
  require_clean ();
  reset ();
}

TEST_CASE ("no snapshot: an empty class or a failed fetch falls back to the histogram estimate and leaves no "
	   "error behind (an error set before the pass survives it)", "[contract]")
{
  /* empty class: db_get_all_objects returns NULL with no error */
  std_scene s = std_build (false);
  double node_sel = s.it->selectivity;
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 1);
  CHECK (s.it->selectivity == node_sel);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.join->selectivity == 0.01);
  require_clean ();

  /* failed fetch (au_fetch_class er_set): the error stays inside the push/pop */
  s = std_build ();
  g_fetch_fail.insert (s.it->info->info[0].mop);
  g_er_cur = ER_GENERIC_ERROR;	/* an error the caller had before planning */
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 1);
  CHECK (g_er_sets == 1);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.join->selectivity == 0.01);
  require_clean (ER_GENERIC_ERROR);
  reset ();
}

TEST_CASE ("row-count bounds: ncard == threshold is a candidate, threshold + 1 and ncard 0 are not fetched",
	   "[contract]")
{
  std_scene s = std_build ();
  snapshot_rows ("5");		/* ncard 5 == 5 */
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 1);
  CHECK (s.it->snapshot_card == 2);

  s = std_build ();
  snapshot_rows ("4");		/* ncard 5 > 4 */
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 0);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.join->selectivity == 0.01);

  s = std_build ();
  s.it->ncard = 0;		/* no statistics rows */
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 0);
  CHECK (s.join->selectivity == 0.01);
  require_clean ();
  reset ();
}

TEST_CASE ("row-count bounds: a table that grew past its statistics is given up; one that shrank keeps ncard "
	   "as the denominator", "[contract]")
{
  /* 6 rows against ncard 5: the 6th row trips rows > ncard */
  std_scene s = std_build ();
  add_row (s.it, { { "id", 6 }, { "kind", 7 } });
  double node_sel = s.it->selectivity;
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (g_fetch_calls == 1);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.it->selectivity == node_sel);
  CHECK (s.join->selectivity == 0.01);
  require_clean ();

  /* 3 rows against ncard 5 (two deleted since the statistics): k = 2 -> 2/5, so ncard * sel = k */
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
  add_row (it, { { "id", 1 }, { "kind", 7 } });
  add_row (it, { { "id", 3 }, { "kind", 7 } });
  add_row (it, { { "id", 4 }, { "kind", 9 } });
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (it->snapshot_card == 2);
  CHECK (it->selectivity == 2.0 / 5.0);
  CHECK (join->selectivity == 0.09375);
  require_clean ();
  reset ();
}

TEST_CASE ("not a candidate: no sargs, no class info, a hierarchy (n != 1), no class mop, a derived table, a CTE, "
	   "a meta-class spec -- none is fetched", "[contract]")
{
  for (int variant = 0; variant < 7; variant++)
    {
      std_scene s = std_build ();
      QO_CLASS_INFO *two;
      switch (variant)
	{
	case 0:
	  bitset_init (&s.it->sargs, g_env);
	  break;
	case 1:
	  s.it->info = NULL;
	  break;
	case 2:
	  two = (QO_CLASS_INFO *) zalloc (sizeof (QO_CLASS_INFO) + sizeof (QO_CLASS_INFO_ENTRY));
	  two->n = 2;
	  two->info[0].mop = s.it->info->info[0].mop;
	  s.it->info = two;
	  break;
	case 3:
	  s.it->info->info[0].mop = NULL;
	  break;
	case 4:
	  s.it->entity_spec->info.spec.derived_table = mk (PT_SELECT);
	  break;
	case 5:
	  s.it->entity_spec->info.spec.cte_pointer = mk (PT_NODE_POINTER);
	  break;
	case 6:
	  s.it->entity_spec->info.spec.meta_class = PT_META_CLASS;
	  break;
	}
      snapshot_rows ("100");
      unitseam_snap_apply (g_env);
      INFO ("variant " << variant);
      CHECK (g_fetch_calls == 0);
      CHECK (s.it->snapshot_card == -1);
      CHECK (s.join->selectivity == 0.01);
      require_clean ();
    }
  reset ();
}

TEST_CASE ("all or nothing: one sarg the pass cannot evaluate (LIKE, user host variable, a QO_TC_OTHER term) "
	   "next to an evaluable one keeps the node and its terms untouched, with no fetch", "[contract]")
{
  for (int variant = 0; variant < 3; variant++)
    {
      scene_begin (4, 16, 1, 1);	/* ?0 user, ?:1 auto */
      QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000);
      std_rows (it);
      add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (1, 7)));
      if (variant == 0)
	{
	  add_sarg (it, ex (PT_LIKE, col (it, "grp"), lit (1)), 0.5, QO_TERM_SINGLE_PRED);
	}
      else if (variant == 1)
	{
	  add_sarg (it, ex (PT_EQ, col (it, "grp"), hv (0, 1)));
	}
      else
	{
	  add_sarg (it, ex (PT_EQ, col (it, "grp"), lit (1)), 0.5, SINGLE_EQ, QO_TC_OTHER);
	}
      QO_TERM *join = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
      std_hist ();
      double node_sel = it->selectivity;
      snapshot_rows ("100");
      unitseam_snap_apply (g_env);
      INFO ("variant " << variant);
      CHECK (g_fetch_calls == 0);
      CHECK (it->selectivity == node_sel);
      CHECK (it->snapshot_card == -1);
      CHECK (join->selectivity == 0.01);
      require_clean ();
    }
  reset ();
}

TEST_CASE ("a row whose filter column cannot be read gives the whole node up, error contained", "[contract]")
{
  std_scene s = std_build ();
  DB_OBJECT *bad = g_extent[s.it->info->info[0].mop][1];	/* id 2 */
  g_get_fail.insert ({ bad, "kind" });
  double node_sel = s.it->selectivity;
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (s.it->snapshot_card == -1);
  CHECK (s.it->selectivity == node_sel);
  CHECK (s.join->selectivity == 0.01);
  CHECK (g_er_sets == 1);
  require_clean ();
  reset ();
}

TEST_CASE ("per term: a term whose other column has no histogram, or whose key cannot be read for one survivor, "
	   "keeps its estimate; the node's other terms are still re-derived", "[contract]")
{
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000), *mc = add_node ("mc", 2000000);
  std_rows (it);
  add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *t_mi = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  QO_TERM *t_mc = add_join (it, mc, ex (PT_EQ, col (it, "id"), col (mc, "ctid", 0.0)), 0.02);	/* no histogram */
  QO_TERM *t_grp = add_join (it, mc, ex (PT_EQ, col (it, "grp"), col (mc, "g", 0.0)), 0.03);
  std_hist ();
  g_hist["g"] = fake_hist { 0.5, {} };
  /* survivors are ids 1 and 3; id 3's grp cannot be read */
  g_get_fail.insert ({ g_extent[it->info->info[0].mop][2], "grp" });
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (it->snapshot_card == 2);
  CHECK (t_mi->selectivity == 0.09375);
  CHECK (from_snapshot (t_mi));
  CHECK (t_mc->selectivity == 0.02);
  CHECK_FALSE (from_snapshot (t_mc));
  CHECK (t_grp->selectivity == 0.03);
  CHECK_FALSE (from_snapshot (t_grp));
  require_clean ();
  reset ();
}

TEST_CASE ("only plain equi-join terms of the node are rewritten (not <, not an expression side, not a RANGE "
	   "join, not a term between two other nodes, not a sarg)", "[contract]")
{
  scene_begin (4, 16, 0, 1);
  QO_NODE *it = add_node ("it", 5), *mi = add_node ("mi", 1000000), *mc = add_node ("mc", 2000000);
  std_rows (it);
  QO_TERM *sarg = add_sarg (it, ex (PT_EQ, col (it, "kind"), hv (0, 7)));
  QO_TERM *lt = add_join (it, mi, ex (PT_LT, col (it, "id"), col (mi, "itid", NF_MI)), 0.3, QO_TERM_SINGLE_PRED);
  QO_TERM *expr_side = add_join (it, mi, ex (PT_EQ, col (it, "id"), ex (PT_PLUS, col (mi, "itid"), lit (1))), 0.04);
  QO_TERM *range_join = add_join (it, mi, ex (PT_RANGE, col (it, "id"), eq_ranges ({ col (mi, "itid", NF_MI) })),
				  0.05);
  QO_TERM *others = add_join (mi, mc, ex (PT_EQ, col (mi, "itid", NF_MI), col (mc, "ctid")), 0.06);
  QO_TERM *good = add_join (it, mi, ex (PT_EQ, col (it, "id"), col (mi, "itid", NF_MI)), 0.01);
  std_hist ();
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (lt->selectivity == 0.3);
  CHECK (expr_side->selectivity == 0.04);
  CHECK (range_join->selectivity == 0.05);
  CHECK (others->selectivity == 0.06);
  CHECK (sarg->selectivity == 0.25);
  CHECK (good->selectivity == 0.09375);
  CHECK (from_snapshot (good));
  CHECK_FALSE ((from_snapshot (lt) || from_snapshot (expr_side) || from_snapshot (range_join)
		|| from_snapshot (others) || from_snapshot (sarg)));
  require_clean ();
  reset ();
}

TEST_CASE ("combination rule: the snapshot value replaces the histogram estimate whether it is higher or lower "
	   "(no min/max with the old value)", "[contract]")
{
  for (double before: { 1e-6, 0.9})
    {
      std_scene s = std_build ();
      s.join->selectivity = before;
      snapshot_rows ("100");
      unitseam_snap_apply (g_env);
      INFO ("histogram estimate " << before);
      CHECK (s.join->selectivity == 0.09375);
      require_clean ();
    }
  reset ();
}

TEST_CASE ("null fraction of the other column: unknown (< 0) and 0 apply no factor, 0.75 keeps a quarter; 1.0 "
	   "applies no factor either (current behaviour)", "[contract]")
{
  const std::pair<double, double> table[] = {
    /* nf_other -> (0.25 + 0.125) / 2 * factor */
    { -1.0, 0.1875 }, { 0.0, 0.1875 }, { 0.75, 0.046875 }, { 1.0, 0.1875 }
  };
  for (auto & e:table)
    {
      std_scene s = std_build ();
      s.join->pt_expr->info.expr.arg2->info.name.null_frequency = e.first;
      snapshot_rows ("100");
      unitseam_snap_apply (g_env);
      INFO ("nf_other " << e.first);
      CHECK (s.join->selectivity == e.second);
      require_clean ();
    }
  reset ();
}

TEST_CASE ("upper bound: every survivor's key priced 1.0 with no NULLs on the other side gives exactly 1.0",
	   "[contract]")
{
  std_scene s = std_build ();
  g_hist["itid"] = fake_hist { 1.0, {} };
  s.join->pt_expr->info.expr.arg2->info.name.null_frequency = 0.0;
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  CHECK (s.join->selectivity == 1.0);
  CHECK (from_snapshot (s.join));
  require_clean ();
  reset ();
}

TEST_CASE ("two snapshot nodes joined to each other: the later node (higher idx) writes the term last "
	   "(current behaviour -- order dependent)", "[contract]")
{
  scene_begin (4, 16, 0, 2);
  QO_NODE *a = add_node ("a", 4), *b = add_node ("b", 4);
  add_row (a, { { "x", 1 }, { "f", 1 } });
  add_row (a, { { "x", 2 }, { "f", 0 } });
  add_row (b, { { "y", 2 }, { "g", 1 } });
  add_row (b, { { "y", 3 }, { "g", 0 } });
  add_sarg (a, ex (PT_EQ, col (a, "f"), hv (0, 1)));
  add_sarg (b, ex (PT_EQ, col (b, "g"), hv (1, 1)));
  QO_TERM *t = add_join (a, b, ex (PT_EQ, col (a, "x", 0.0), col (b, "y", 0.0)), 0.01);
  g_hist["y"] = fake_hist { 0.125, {} };	/* a's survivor key 1 priced on b.y */
  g_hist["x"] = fake_hist { 0.5, {} };	/* b's survivor key 2 priced on a.x */
  snapshot_rows ("100");
  unitseam_snap_apply (g_env);
  /* node a (idx 0) wrote 0.125, node b (idx 1) overwrote it with 0.5; the exact answer for the survivors
   * (a.x = 1, b.y = 2) is "no pair matches" */
  CHECK (a->snapshot_card == 1);
  CHECK (b->snapshot_card == 1);
  CHECK (t->selectivity == 0.5);
  CHECK (g_hist_keys == std::vector<int> ({ 1, 2 }));
  require_clean ();
  reset ();
}
