/*
 * PR#8440 (CBRD-27351) A-01 -- mq_share_view_refs () of src/parser/view_transform.c (CS_MODE). Line numbers of
 * view_transform.c are of 14b4f25b5 (name_resolution.c, parse_tree_cl.c and parser_support.c are as at 2472da664).
 *
 * mq_share_view_attrs () (view_transform.c:17109-17155) gathers the columns each reference to a view reads with
 * mq_get_references (parser, statement, ref->spec) (17123), statement being the WHOLE statement (mq_share_view_refs ()
 * passes its root, 17187). mq_get_references () -> mq_reset_ids_and_references_helper () -> mq_reset_ids () binds to
 * the spec every PT_NAME of that statement whose spec_id equals the spec's id (mq_reset_all_ids (), 10577-10609), and
 * then sets the spec id to the spec's own address (10651). parser_walk_tree visits the entity name, the range variable
 * and the flat entity names of a spec too (pt_apply_spec (), parse_tree_cl.c).
 *
 * A spec id is set once, at name resolution (name_resolution.c:4912: "the address of the node may be changed through
 * copying, but this id won't"); parser_copy_tree () keeps it (*new_node = *old_node, parse_tree_cl.c:996). The CTE
 * inlining that runs right before mq_share_view_refs () in mq_translate_helper () (10027 -> mq_inline_cte_pre (),
 * 7117) copies the CTE body once per reference -- a CTE with the INLINE hint referenced twice (6986-6990) leaves two
 * copies whose view specs have one id. develop calls mq_get_references () only with the SELECT that holds the spec
 * (mq_rewrite_vclass_spec_as_derived () at 3097 / 8165 / 8182 / 8203, statement = that SELECT), so each copy's names
 * stay with its own spec; mq_substitute_subquery_in_statement () then finds the spec by the flat entity name's spec_id
 * (pt_find_spec (), parser_support.c:831-842; view_transform.c:3075-3079, "class spec not found").
 *
 * Real code: view_transform.c (the TU), parse_tree.c + parse_tree_cl.c (parser_create_parser, parser_new_node,
 * parser_copy_tree, parser_walk_tree, parser_append_node, parser_free_tree, parser_free_parser), parser_support.c
 * (pt_name, pt_find_spec, pt_name_equal, pt_has_define_vars, pt_make_integer_value), language_support.c +
 * intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c,
 * system_parameter.c.
 * Seams (same TU): mq_fetch_attributes (14875: the view cache's attrs, the oid first) and mq_fetch_subqueries (8777:
 * the view cache's translated query) -- both return what the cases give. Fakes: db_is_vclass (db_virt.c: 1 for the
 * view object of the cases), pt_str_compare (name_resolution.c body: intl_identifier_casecmp for CASE_INSENSITIVE),
 * pt_is_result_reuse_safe (xasl_generation.c: true -- the cases' view body holds no expression it refuses), and the
 * language / error fakes of parser/xasl_generation.session_variable_mark.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "dbi.h"
#include "parser.h"
#include "parse_tree.h"
#include "view_transform.h"
#include "system_parameter.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "string_regex.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---------------------------------------------------------------- fakes */


/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c envvar_trim_char body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* string_opfunc.c: lang_init_builtin () gives each built-in locale its calendar names; identifier comparison does not
 * read them */
void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

/* db_date.c: lang_set_charset_lang () refreshes the local AM/PM strings of date parsing; not read here */
void
db_date_locale_init (void)
{
}

/* scanner_support.c pt_parser_line_col () (C linkage, csql_grammar_scan.h): parser_new_node () stamps the lexer's
 * token position on a node; there is no lexer here, so the position is 0 */
extern "C" void
pt_parser_line_col (PT_NODE * node)
{
  if (node == NULL)
    {
      return;
    }
  node->line_number = 0;
  node->column_number = 0;
}

/* name_resolution.c pt_free_dblink_remote_cols (): parser_free_parser () frees the dblink column lists; a parser that
 * resolved no dblink table has none (parser->dblink_remote NULL), and the real loop does nothing */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

/* error_context.cpp: a base level and a stack of levels; push makes a new empty level on top (err_id NO_ERROR), pop
 * drops the top, er_set / er_errid / er_clear work on the current level (the top, else the base) */
namespace
{
  int g_er_base = NO_ERROR;
  int g_er_stack[64];
  int g_er_depth = 0;

  int &er_current ()
  {
    return g_er_depth > 0 ? g_er_stack[g_er_depth - 1] : g_er_base;
  }
}

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  er_current () = err_id;
}

int
er_errid (void)
{
  return er_current ();
}

void
er_clear (void)
{
  er_current () = NO_ERROR;
}

void
er_stack_push (void)
{
  REQUIRE (g_er_depth < 64);
  g_er_stack[g_er_depth++] = NO_ERROR;
}

void
er_stack_pop (void)
{
  REQUIRE (g_er_depth > 0);
  g_er_depth--;
}

/* error_context.cpp er_has_error () / er_errid_if_has_error () on the current level (mq_translate_tree () and
 * mq_substitute_subquery_list_in_statement () read them after a substitution) */
bool
er_has_error (void)
{
  return er_current () != NO_ERROR;
}

int
er_errid_if_has_error (void)
{
  return er_current ();
}

namespace
{
  char g_view_storage[64];	/* the view object of the cases: only its address is read */
  PT_NODE *g_view_attrs;	/* what mq_fetch_attributes () gives */
  PT_NODE *g_view_body;		/* what mq_fetch_subqueries () gives */

  DB_OBJECT *ut_view ()
  {
    return (DB_OBJECT *) g_view_storage;
  }
}

/* db_virt.c db_is_vclass (): the cases read one view */
int
db_is_vclass (DB_OBJECT * op)
{
  return op == ut_view ()? 1 : 0;
}

/* name_resolution.c pt_str_compare () body */
int
pt_str_compare (const char *p, const char *q, CASE_SENSITIVENESS case_flag)
{
  if (!p && !q)
    {
      return 0;
    }
  if (!p || !q)
    {
      return 1;
    }
  return case_flag == CASE_INSENSITIVE ? intl_identifier_casecmp (p, q) : intl_identifier_cmp (p, q);
}

/* xasl_generation.c pt_is_result_reuse_safe (): the cases' view body has no random value, serial, session variable or
 * nondeterministic stored function */
bool
pt_is_result_reuse_safe (PARSER_CONTEXT * parser, PT_NODE * tree)
{
  return true;
}

/* seams of the TU */
PT_NODE *
mq_fetch_attributes (PARSER_CONTEXT * parser, PT_NODE * class_)
{
  return g_view_attrs;
}

PT_NODE *
mq_fetch_subqueries (PARSER_CONTEXT * parser, PT_NODE * class_)
{
  return g_view_body;
}

/* appendix (hooks.cpp): mq_share_view_refs () is static */
void ut_share_view_refs (PARSER_CONTEXT * parser, PT_NODE * statement);
PT_NODE *ut_translate_tree (PARSER_CONTEXT * parser, PT_NODE * select_statement);

namespace
{
  bool boot ()
  {
    static bool done = false;
    if (!done)
      {
	done = true;
	lang_init_builtin ();
	if (lang_set_charset_lang ("en_US.utf8") != NO_ERROR)
	  {
	    return false;
	  }
      }
    g_er_base = NO_ERROR;
    g_er_depth = 0;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  struct tree
  {
    PARSER_CONTEXT *parser;

    tree ()
    {
      parser = parser_create_parser ();
      /* the view cache: "select * from [v]" with the row oid first (mq_virtual_queries (), 9542 and 9578-9582) */
      g_view_attrs = name ("v", 0, PT_OID_ATTR, PT_TYPE_OBJECT);
      g_view_attrs = parser_append_node (name ("k", 0, PT_NORMAL, PT_TYPE_INTEGER), g_view_attrs);
      g_view_attrs = parser_append_node (name ("s", 0, PT_NORMAL, PT_TYPE_INTEGER), g_view_attrs);
      /* its translated query: select k, s from t -- one SELECT, no DISTINCT, hint, CTE or := */
      PT_NODE *t = parser_new_node (parser, PT_SPEC);
      t->info.spec.id = (UINTPTR) t;
      t->info.spec.entity_name = name ("t", t->info.spec.id, PT_CLASS, PT_TYPE_NONE);
      t->info.spec.range_var = name ("t", t->info.spec.id, PT_NORMAL, PT_TYPE_NONE);
      g_view_body = parser_new_node (parser, PT_SELECT);
      g_view_body->info.query.q.select.list = name ("k", t->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER);
      g_view_body->info.query.q.select.list =
	parser_append_node (name ("s", t->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER), g_view_body->info.query.q.select.list);
      g_view_body->info.query.q.select.from = t;
    }

    ~tree ()
    {
      parser_free_parser (parser);
      g_view_attrs = g_view_body = NULL;
    }

    PT_NODE *name (const char *text, UINTPTR spec_id, PT_MISC_TYPE meta, PT_TYPE_ENUM type, DB_OBJECT * obj = NULL,
		   const char *resolved = NULL)
    {
      PT_NODE *n = pt_name (parser, text);
      n->info.name.spec_id = spec_id;
      n->info.name.meta_class = meta;
      n->info.name.db_object = obj;
      n->info.name.resolved = resolved;
      n->type_enum = type;
      return n;
    }

    /* a spec of view v as name resolution leaves it: its id is its address, and its entity name, flat entity name and
     * range variable carry the id (name_resolution.c:4912, 7869-7893) */
    PT_NODE *view_spec ()
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.id = (UINTPTR) s;
      s->info.spec.meta_class = PT_CLASS;
      s->info.spec.entity_name = name ("v", s->info.spec.id, PT_CLASS, PT_TYPE_NONE, ut_view ());
      s->info.spec.flat_entity_list = name ("v", s->info.spec.id, PT_CLASS, PT_TYPE_NONE, ut_view ());
      s->info.spec.range_var = name ("v", s->info.spec.id, PT_NORMAL, PT_TYPE_NONE);
      return s;
    }

    /* select k [, extra] from v -- the body of CTE c */
    PT_NODE *select_k_from_v (const char *extra = NULL)
    {
      PT_NODE *spec = view_spec ();
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = name ("k", spec->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER, NULL, "v");
      if (extra != NULL)
	{
	  q->info.query.q.select.list =
	    parser_append_node (name (extra, spec->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER, NULL, "v"),
				q->info.query.q.select.list);
	}
      q->info.query.q.select.from = spec;
      return q;
    }

    /* (q) range -- a derived table, as mq_inline_cte_pre () makes it of a copy of the CTE body (7117-7132) */
    PT_NODE *derived (PT_NODE * q, const char *range)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.id = (UINTPTR) s;
      s->info.spec.derived_table = q;
      s->info.spec.derived_table_type = PT_IS_SUBQUERY;
      s->info.spec.range_var = name (range, s->info.spec.id, PT_NORMAL, PT_TYPE_NONE);
      q->info.query.is_subquery = PT_IS_SUBQUERY;
      return s;
    }

    /* select a.k, b.k from (qa) a, (qb) b */
    PT_NODE *root (PT_NODE * qa, PT_NODE * qb)
    {
      PT_NODE *a = derived (qa, "a");
      PT_NODE *b = derived (qb, "b");
      PT_NODE *r = parser_new_node (parser, PT_SELECT);
      r->info.query.q.select.list = name ("k", a->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER, NULL, "a");
      r->info.query.q.select.list =
	parser_append_node (name ("k", b->info.spec.id, PT_NORMAL, PT_TYPE_INTEGER, NULL, "b"), r->info.query.q.select.list);
      a->next = b;
      r->info.query.q.select.from = a;
      return r;
    }
  };

  PT_NODE *spec_of (PT_NODE * q)
  {
    return q->info.query.q.select.from;
  }

  /* the copy's names are the copy's: what mq_substitute_subquery_in_statement () needs to find the spec it expands
   * (pt_find_spec () by the flat entity name, view_transform.c:3075), and what its select list reads */
  bool bound_to_own_spec (tree & t, PT_NODE * q)
  {
    PT_NODE *spec = spec_of (q);
    bool found = pt_find_spec (t.parser, spec, spec->info.spec.flat_entity_list) == spec;
    bool names = true;
    for (PT_NODE * col = q->info.query.q.select.list; col != NULL; col = col->next)
      {
	names = names && col->info.name.spec_id == spec->info.spec.id;
      }
    return found && names && spec->info.spec.entity_name->info.name.spec_id == spec->info.spec.id;
  }

  void report (const char *label, tree & t, PT_NODE * qa, PT_NODE * qb)
  {
    PT_NODE *ra = spec_of (qa), *rb = spec_of (qb);
    fprintf (stderr, "%s: spec a %p id %p flat %p col %p | spec b %p id %p flat %p col %p | group a %p b %p\n", label,
	     (void *) ra, (void *) ra->info.spec.id, (void *) ra->info.spec.flat_entity_list->info.name.spec_id,
	     (void *) qa->info.query.q.select.list->info.name.spec_id, (void *) rb, (void *) rb->info.spec.id,
	     (void *) rb->info.spec.flat_entity_list->info.name.spec_id,
	     (void *) qb->info.query.q.select.list->info.name.spec_id, (void *) ra->info.spec.share_group,
	     (void *) rb->info.spec.share_group);
  }
}

TEST_CASE ("C0 premise: a parser_copy_tree () copy keeps the spec id and its names' spec_id (as mq_inline_cte_pre () "
	   "copies a CTE body per reference)", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *body = t.select_k_from_v ();
  PT_NODE *qa = parser_copy_tree (t.parser, body);
  PT_NODE *qb = parser_copy_tree (t.parser, body);

  REQUIRE (qa != NULL);
  REQUIRE (qb != NULL);
  CHECK (spec_of (qa) != spec_of (qb));
  CHECK (spec_of (qa)->info.spec.id == spec_of (body)->info.spec.id);
  CHECK (spec_of (qb)->info.spec.id == spec_of (body)->info.spec.id);
  CHECK (qa->info.query.q.select.list->info.name.spec_id == spec_of (body)->info.spec.id);
  CHECK (spec_of (qa)->info.spec.flat_entity_list->info.name.spec_id == spec_of (body)->info.spec.id);
}

TEST_CASE ("C1 control: two references of v with their own spec ids -- after mq_share_view_refs () they share one "
	   "group and each copy's names stay with its spec", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *qa = t.select_k_from_v ();
  PT_NODE *qb = t.select_k_from_v ();
  PT_NODE *r = t.root (qa, qb);

  ut_share_view_refs (t.parser, r);
  report ("C1", t, qa, qb);

  CHECK_FALSE (pt_has_error (t.parser));
  REQUIRE (spec_of (qa)->info.spec.share_group != NULL);
  CHECK (spec_of (qa)->info.spec.share_group == spec_of (qb)->info.spec.share_group);
  CHECK (bound_to_own_spec (t, qa));
  CHECK (bound_to_own_spec (t, qb));
}

TEST_CASE ("C2 control: develop's own call, mq_get_references () with the SELECT that holds the spec (as "
	   "mq_rewrite_vclass_spec_as_derived ()), leaves the other copy of the same id bound to its spec",
	   "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *body = t.select_k_from_v ();
  PT_NODE *qa = parser_copy_tree (t.parser, body);
  PT_NODE *qb = parser_copy_tree (t.parser, body);
  (void) t.root (qa, qb);

  PT_NODE *refs = mq_get_references (t.parser, qa, spec_of (qa));
  CHECK (refs != NULL);
  parser_free_tree (t.parser, refs);
  CHECK (bound_to_own_spec (t, qa));
  CHECK (bound_to_own_spec (t, qb));
}

TEST_CASE ("PR8440 A-01 D1: a CTE with the INLINE hint read twice (two copies of its body, one spec id) -- "
	   "mq_share_view_refs () binds the names of one copy, its flat entity name among them, to the spec of the other "
	   "copy, so that copy's spec is not found by its own entity name (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *body = t.select_k_from_v ();
  PT_NODE *qa = parser_copy_tree (t.parser, body);
  PT_NODE *qb = parser_copy_tree (t.parser, body);
  PT_NODE *r = t.root (qa, qb);

  ut_share_view_refs (t.parser, r);
  report ("D1", t, qa, qb);

  CHECK_FALSE (pt_has_error (t.parser));
  CHECK (spec_of (qa)->info.spec.share_group != NULL);
  REQUIRE_FALSE ((bound_to_own_spec (t, qa) && bound_to_own_spec (t, qb)));
  /* the copy processed second keeps no name: its flat entity name points at the other copy's spec */
  bool a_to_b = spec_of (qa)->info.spec.flat_entity_list->info.name.spec_id == (UINTPTR) spec_of (qb);
  bool b_to_a = spec_of (qb)->info.spec.flat_entity_list->info.name.spec_id == (UINTPTR) spec_of (qa);
  CHECK ((a_to_b || b_to_a));
}

TEST_CASE ("PR8440 A-01 D2: the same two copies when the view is not shared (one copy also reads a name that is not a "
	   "column of v, mq_share_view_attrs () gives up) -- no group, but the names are already bound across the copies "
	   "(PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *body = t.select_k_from_v ("z");
  PT_NODE *qa = parser_copy_tree (t.parser, body);
  PT_NODE *qb = parser_copy_tree (t.parser, body);
  PT_NODE *r = t.root (qa, qb);

  ut_share_view_refs (t.parser, r);
  report ("D2", t, qa, qb);

  CHECK (spec_of (qa)->info.spec.share_group == NULL);
  CHECK (spec_of (qb)->info.spec.share_group == NULL);
  REQUIRE_FALSE ((bound_to_own_spec (t, qa) && bound_to_own_spec (t, qb)));
}

TEST_CASE ("PR8440 A-01 D3: the copy whose names went to the other copy fails to translate -- mq_translate_tree () "
	   "(the step mq_translate_local () takes for each SELECT: 8537 -> mq_translate_select () 7348) returns NULL with "
	   "the internal error 'class spec not found' of mq_substitute_subquery_in_statement () (3075-3079) (PASS = "
	   "defect present)",
	   "[defect]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *body = t.select_k_from_v ();
  PT_NODE *qa = parser_copy_tree (t.parser, body);
  PT_NODE *qb = parser_copy_tree (t.parser, body);
  PT_NODE *r = t.root (qa, qb);

  ut_share_view_refs (t.parser, r);
  REQUIRE_FALSE (pt_has_error (t.parser));

  PT_NODE *broken = !bound_to_own_spec (t, qa) ? qa : (!bound_to_own_spec (t, qb) ? qb : NULL);
  REQUIRE (broken != NULL);
  PT_NODE *result = ut_translate_tree (t.parser, broken);
  const char *msg = (t.parser->error_msgs != NULL) ? t.parser->error_msgs->info.error_msg.error_message : "(none)";
  fprintf (stderr, "D3: translate of the broken copy -> %p, error: %s\n", (void *) result, msg);
  CHECK (result == NULL);
  REQUIRE (pt_has_error (t.parser));
  CHECK (strstr (msg, "class spec not found") != NULL);
}
