/*
 * PR#8013 F4 -- bind_target_spec_is_unique_pinned (src/optimizer/histogram/histogram_cl.cpp, cs mode).
 * A UNIQUE-family constraint whose attributes are all matched by an equality term "pins" the spec,
 * and histogram_bind_watch_candidate then skips that column. The claim: function-based and filtered
 * unique indexes are treated the same, though a plain column equality cannot use either.
 *
 * Catalog fakes return SM_CLASS_CONSTRAINT lists built the way the schema manager builds them:
 *   UNIQUE (LOWER (a))      -> attributes [a], func_index_info {col_id 0, attr_index_start 0}
 *                              (semantic_check.c pt_check_function_index_expr moves the expression's
 *                               argument columns to the key tail; execute_schema.c:3329
 *                               attr_index_start = nnames - func_no_args)
 *   UNIQUE (a) WHERE b > 0  -> attributes [a], filter_predicate {pred_string "b > 0", att_ids [b]}
 * NOTE the grammar (csql_grammar.y 2701, 2739, 9337) rejects both with UNIQUE, so the two [defect]
 * inputs cannot be created by SQL today -- see suite.json "note" (latent defect).
 */
#include "catch.hpp"
#include "dbtype_def.h"
#include "db.h"
#include "parser.h"
#include "class_object.h"
#include "schema_manager.h"
#include "intl_support.h"
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <map>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes */

struct fake_class
{
  std::string name;
  SM_CLASS_CONSTRAINT *constraints;
};
static std::map<std::string, fake_class *> g_classes;	/* catalog: class name -> constraints */
static std::map<UINTPTR, std::string> g_spec_class;	/* statement: spec id -> class name */

/* seam (static in the TU): real walks the statement for the PT_SPEC whose info.spec.id equals
 * name->info.name.spec_id and returns its entity_name->info.name.original; NULL when statement is
 * NULL or spec_id is 0 (histogram_cl.cpp:605-616). Same lookup over the test's spec table. */
const char *
histogram_spec_class_name (PARSER_CONTEXT *, PT_NODE *statement, PT_NODE *name)
{
  if (statement == NULL || name->info.name.spec_id == 0)
    {
      return NULL;
    }
  auto it = g_spec_class.find (name->info.name.spec_id);
  return it == g_spec_class.end () ? NULL : it->second.c_str ();
}

/* seam (static in the TU): real reads the column histogram and returns mcv_count () > 0. true =
 * the column has MCVs, the only case in which histogram_bind_watch_candidate asks the pinned question. */
bool
bind_target_column_has_mcv (PT_NODE *)
{
  return true;
}

/* db_info.c: db_find_class -> sm_find_class_with_purpose (name, false); NULL for an unknown class */
DB_OBJECT *
db_find_class (const char *name)
{
  auto it = g_classes.find (name);
  return it == g_classes.end () ? NULL : (DB_OBJECT *) it->second;
}

/* schema_manager.c: au_fetch_class (READ, SELECT) then class_->constraints */
SM_CLASS_CONSTRAINT *
sm_class_constraints (MOP classop)
{
  return classop == NULL ? NULL : ((fake_class *) classop)->constraints;
}

/* intl_support.c: lang-aware case-insensitive identifier compare; every identifier here is ASCII,
 * where it equals strcasecmp */
int
intl_identifier_casecmp (const char *str1, const char *str2)
{
  return strcasecmp (str1, str2);
}

/* parser_support.c:2441, verbatim */
PT_NODE *
pt_get_end_path_node (PT_NODE *node)
{
  while (node != NULL && node->node_type == PT_DOT_)
    {
      node = node->info.dot.arg2;
    }
  return node;
}

/* ---------------------------------------------------------------- appendix hooks */
void *unitseam_bt_new (PARSER_CONTEXT *parser);
void unitseam_bt_free (void *ctx);
void unitseam_bt_visit (PARSER_CONTEXT *parser, void *ctx, PT_NODE *node);
int unitseam_bt_terms (void *ctx);
bool unitseam_bt_term_equality (void *ctx, int i);
bool unitseam_bt_unique_pinned (PARSER_CONTEXT *parser, PT_NODE *statement, void *ctx, UINTPTR spec_id);

/* ---------------------------------------------------------------- builders */
static const UINTPTR SPEC_T = 0x7001;

static PT_NODE *
mk (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

static PT_NODE *
col (const char *name, UINTPTR spec)
{
  PT_NODE *n = mk (PT_NAME);
  n->info.name.original = name;
  n->info.name.spec_id = spec;
  return n;
}

static PT_NODE *
hv (int index)
{
  PT_NODE *n = mk (PT_HOST_VAR);
  n->info.host_var.index = index;
  return n;
}

static PT_NODE *
cmp (PT_OP_TYPE op, PT_NODE *a, PT_NODE *b)
{
  PT_NODE *n = mk (PT_EXPR);
  n->info.expr.op = op;
  n->info.expr.arg1 = a;
  n->info.expr.arg2 = b;
  return n;
}

static SM_ATTRIBUTE *
attr (const char *name)
{
  SM_ATTRIBUTE *a = (SM_ATTRIBUTE *) calloc (1, sizeof (SM_ATTRIBUTE));
  a->header.name = name;
  return a;
}

static SM_CLASS_CONSTRAINT *
cons (SM_CONSTRAINT_TYPE type, const char *name, std::vector<const char *> cols)
{
  SM_CLASS_CONSTRAINT *c = (SM_CLASS_CONSTRAINT *) calloc (1, sizeof (SM_CLASS_CONSTRAINT));
  c->type = type;
  c->name = name;
  c->attributes = (SM_ATTRIBUTE **) calloc (cols.size () + 1, sizeof (SM_ATTRIBUTE *));
  for (size_t i = 0; i < cols.size (); i++)
    {
      c->attributes[i] = attr (cols[i]);
    }
  return c;
}

/* UNIQUE (LOWER (a)): key columns after semantic check = [a], the function at key position 0 */
static SM_CLASS_CONSTRAINT *
func_unique_lower_a ()
{
  SM_CLASS_CONSTRAINT *c = cons (SM_CONSTRAINT_UNIQUE, "u_lower_a", {"a"});
  c->func_index_info = (SM_FUNCTION_INFO *) calloc (1, sizeof (SM_FUNCTION_INFO));
  c->func_index_info->expr_str = (char *) "lower([a])";
  c->func_index_info->col_id = 0;
  c->func_index_info->attr_index_start = 0;
  return c;
}

/* UNIQUE (a) WHERE b > 0 */
static SM_CLASS_CONSTRAINT *
filter_unique_a ()
{
  SM_CLASS_CONSTRAINT *c = cons (SM_CONSTRAINT_UNIQUE, "u_a_where_b", {"a"});
  c->filter_predicate = (SM_PREDICATE_INFO *) calloc (1, sizeof (SM_PREDICATE_INFO));
  c->filter_predicate->pred_string = (char *) "b > 0";
  c->filter_predicate->att_ids = (int *) calloc (1, sizeof (int));
  c->filter_predicate->att_ids[0] = 2;	/* b */
  c->filter_predicate->num_attrs = 1;
  return c;
}

/* one class t (a, b, k) with the given constraints; the statement's spec SPEC_T is t */
struct scene
{
  PARSER_CONTEXT *parser;
  PT_NODE *statement;
  void *ctx;

  explicit scene (SM_CLASS_CONSTRAINT *constraints)
  {
    g_classes.clear ();
    g_spec_class.clear ();
    fake_class *t = new fake_class { "t", constraints };
    g_classes["t"] = t;
    g_spec_class[SPEC_T] = "t";

    parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    parser->host_var_count = 4;		/* user host variables 0..3 */
    statement = mk (PT_SELECT);		/* only its non-NULL-ness reaches the fake */
    ctx = unitseam_bt_new (parser);
  }
  ~scene ()
  {
    unitseam_bt_free (ctx);
  }
  /* WHERE ... AND t.<c> <op> ? */
  void where (const char *c, PT_OP_TYPE op, int hv_index)
  {
    unitseam_bt_visit (parser, ctx, cmp (op, col (c, SPEC_T), hv (hv_index)));
  }
  bool pinned ()
  {
    return unitseam_bt_unique_pinned (parser, statement, ctx, SPEC_T);
  }
};

/* ---------------------------------------------------------------- cases */

TEST_CASE ("plain UNIQUE(a) with t.a = ? pins the spec", "[contract]")
{
  scene s (cons (SM_CONSTRAINT_UNIQUE, "u_a", {"a"}));
  s.where ("a", PT_EQ, 0);
  REQUIRE (unitseam_bt_terms (s.ctx) == 1);
  REQUIRE (unitseam_bt_term_equality (s.ctx, 0));
  CHECK (s.pinned () == true);
}

TEST_CASE ("plain UNIQUE(a,b) with only t.a = ? does not pin", "[contract]")
{
  scene s (cons (SM_CONSTRAINT_UNIQUE, "u_ab", {"a", "b"}));
  s.where ("a", PT_EQ, 0);
  REQUIRE (unitseam_bt_terms (s.ctx) == 1);
  CHECK (s.pinned () == false);
}

TEST_CASE ("plain UNIQUE(a) with t.a > ? (range, not equality) does not pin", "[contract]")
{
  scene s (cons (SM_CONSTRAINT_UNIQUE, "u_a", {"a"}));
  s.where ("a", PT_GT, 0);
  REQUIRE (unitseam_bt_terms (s.ctx) == 1);
  REQUIRE_FALSE (unitseam_bt_term_equality (s.ctx, 0));
  CHECK (s.pinned () == false);
}

TEST_CASE ("non-unique INDEX(a) with t.a = ? does not pin", "[contract]")
{
  scene s (cons (SM_CONSTRAINT_INDEX, "i_a", {"a"}));
  s.where ("a", PT_EQ, 0);
  CHECK (s.pinned () == false);
}

TEST_CASE ("F4: function-based UNIQUE(LOWER(a)) is judged pinned by t.a = ? (PASS = defect)", "[defect]")
{
  scene s (func_unique_lower_a ());
  s.where ("a", PT_EQ, 0);
  REQUIRE (unitseam_bt_terms (s.ctx) == 1);
  REQUIRE (unitseam_bt_term_equality (s.ctx, 0));
  CHECK (s.pinned () == true);
}

TEST_CASE ("F4: filtered UNIQUE(a) WHERE b > 0 is judged pinned by t.a = ? (PASS = defect)", "[defect]")
{
  scene s (filter_unique_a ());
  s.where ("a", PT_EQ, 0);
  REQUIRE (unitseam_bt_terms (s.ctx) == 1);
  REQUIRE (unitseam_bt_term_equality (s.ctx, 0));
  CHECK (s.pinned () == true);
}
