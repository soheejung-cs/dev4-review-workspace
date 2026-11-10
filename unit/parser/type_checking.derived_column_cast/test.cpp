/*
 * PR#8435 (CBRD-27962) -- pt_cast_derived_columns_to_bound_types () of src/parser/type_checking.c (CS_MODE), reached
 * through pt_eval_type ()'s PT_SELECT branch: the post step of the type checking walk calls it for every derived table
 * of the FROM list whose derived_table_type is PT_IS_SUBQUERY (type_checking.c:8402-8407 at 332926a50).
 *
 * The contract read off the function (type_checking.c:8168-8245 at 332926a50) and its callers:
 *   - an item of the derived table's select list whose type_enum is not that of its column (spec as_attr_list, paired
 *     by position with hidden items included) is wrapped in CAST (item AS the column type), with the column's data
 *     type; the cast takes the item's alias, and a cast of the first item also gives the query its type
 *   - NULL, NA, MAYBE and NONE on either side, an OBJECT column and a collection on either side are left alone
 *   - the same type_enum with another precision is left alone (a string is not cut)
 *   - items past the last column (hidden ORDER BY items appended after name resolution) are left alone
 *   - a set operation is cast operand by operand against the same columns, nested operations included
 *   - a VALUES query and a derived table of another kind are left alone
 * develop (dcbab24d0) has no such step: the cases that expect a cast fail there (the fix), the others pass.
 *
 * [defect] D1 is the review finding: an operand of a set operation already carries the DOUBLE cast that
 * pt_check_union_compatibility () gave it when a folded string met a number (pt_common_type: number + string =
 * DOUBLE), and the step wraps that cast instead of replacing it, so a BIGINT or a wide NUMERIC goes through DOUBLE.
 *
 * Real code: type_checking.c (the TU: pt_eval_type, pt_cast_derived_columns_to_bound_types, pt_wrap_with_cast_op,
 * pt_common_type, pt_where_type), semantic_check.c (pt_check_union_is_foldable, pt_check_union_compatibility,
 * pt_get_compatible_info, pt_to_compatible_cast, pt_make_cast_with_compatible_info), query_result.c
 * (pt_get_select_list), parse_tree.c + parse_tree_cl.c (parser_create_parser, parser_new_node, parser_copy_tree_list,
 * parser_free_tree), object_domain.c + object_domain_convert.cpp + object_primitive.c + numeric_opfunc.c
 * (tp_value_cast DOUBLE <-> BIGINT / NUMERIC), language_support.c + intl_support.c + chartype.c (en_US.utf8),
 * domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Fakes (suite.json "fakes"): er_* (error context), pt_parser_line_col (no lexer), init_builtin_calendar_names /
 * db_date_locale_init (locale side tables), envvar_get / envvar_trim_char, cubregex::get_engine_name,
 * pt_free_dblink_remote_cols. A folded item (COALESCE (c, NULL) after folding) is a PT_NAME of the folded type: the
 * step reads only type_enum and data_type.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "dbi.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "memory_alloc.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "numeric_opfunc.h"
#include "string_regex.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

/* the appendix (hooks.cpp, same TU as pt_eval_type) */
PT_NODE *ut_eval_type_node (PARSER_CONTEXT * parser, PT_NODE * node, bool donot_fold);

/* ---------------------------------------------------------------- fakes */

/* system_parameter.c static initializers (the regex engine keyword table) */
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* environment_variable.c: no CUBRID_* variable is set */
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

/* string_opfunc.c: calendar names of a built-in locale; nothing here reads them */
void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

/* db_date.c: the local AM/PM strings of date parsing; not read here */
void
db_date_locale_init (void)
{
}

/* scanner_support.c pt_parser_line_col (): no lexer, position 0 */
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

/* name_resolution.c: a parser that resolved no dblink table has no column lists to free */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

/* xasl_generation.c:3777 pt_is_single_tuple (): no GROUP BY -> pt_has_aggregate (); pt_false_where () asks it for each
 * operand of a set operation (pt_check_union_is_foldable ()). No tree here has an aggregate. */
int
pt_is_single_tuple (PARSER_CONTEXT * parser, PT_NODE * select_node)
{
  if (select_node->info.query.q.select.group_by != NULL)
    {
      return false;
    }
  return false;
}

/* error_context.cpp: one level is enough here (nothing pushes) */
namespace
{
  int g_er = NO_ERROR;
}

void
er_set (int, const char *, const int, int err_id, int, ...)
{
  g_er = err_id;
}

int
er_errid (void)
{
  return g_er;
}

void
er_clear (void)
{
  g_er = NO_ERROR;
}

/* ---------------------------------------------------------------- builders */
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
    g_er = NO_ERROR;
    return lang_charset () == INTL_CODESET_UTF8;
  }

  struct tree
  {
    PARSER_CONTEXT *parser;

    tree ()
    {
      parser = parser_create_parser ();
    }
    ~tree ()
    {
      parser_free_parser (parser);
    }

    /* a PT_DATA_TYPE as name resolution copies onto an item or a column */
    PT_NODE *dt (PT_TYPE_ENUM t, int p = TP_FLOATING_PRECISION_VALUE, int s = 0)
    {
      PT_NODE *d = parser_new_node (parser, PT_DATA_TYPE);
      d->type_enum = t;
      d->info.data_type.precision = p;
      d->info.data_type.dec_precision = s;
      if (PT_HAS_COLLATION (t))
	{
	  d->info.data_type.units = LANG_SYS_CODESET;
	  d->info.data_type.collation_id = LANG_SYS_COLLATION;
	}
      return d;
    }

    /* a column reference, or an item standing for a folded expression: a PT_NAME of that type */
    PT_NODE *name (const char *n, PT_TYPE_ENUM t, PT_NODE * d = NULL)
    {
      PT_NODE *x = parser_new_node (parser, PT_NAME);
      x->info.name.original = pt_append_string (parser, NULL, n);
      x->info.name.resolved = "t";
      x->type_enum = t;
      x->data_type = d;
      return x;
    }

    PT_NODE *hidden (PT_NODE * x)
    {
      x->flag.is_hidden_column = 1;
      return x;
    }

    static PT_NODE *chain (std::initializer_list < PT_NODE * >nodes)
    {
      PT_NODE *head = NULL, *tail = NULL;
      for (PT_NODE * n:nodes)
	{
	  if (head == NULL)
	    {
	      head = n;
	    }
	  else
	    {
	      tail->next = n;
	    }
	  tail = n;
	}
      return head;
    }

    PT_NODE *entity (const char *table)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, table);
      s->info.spec.entity_name = n;
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* a SELECT typed as pt_eval_type () leaves it: the type of its first column */
    PT_NODE *select (PT_NODE * list)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = list;
      q->info.query.q.select.from = entity ("t");
      q->info.query.is_subquery = PT_IS_SUBQUERY;
      q->type_enum = list->type_enum;
      q->data_type = parser_copy_tree_list (parser, list->data_type);
      return q;
    }

    PT_NODE *set_op (PT_NODE_TYPE op, PT_NODE * a, PT_NODE * b)
    {
      PT_NODE *u = parser_new_node (parser, op);
      u->info.query.q.union_.arg1 = a;
      u->info.query.q.union_.arg2 = b;
      u->info.query.all_distinct = PT_ALL;
      u->info.query.is_subquery = PT_IS_SUBQUERY;
      u->type_enum = a->type_enum;
      return u;
    }

    PT_NODE *derived (PT_NODE * query, PT_NODE * attrs, PT_MISC_TYPE kind = PT_IS_SUBQUERY)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.derived_table = query;
      s->info.spec.derived_table_type = kind;
      s->info.spec.as_attr_list = attrs;
      s->info.spec.range_var = name ("x", PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* the enclosing query: SELECT x.<first column> FROM <spec> */
    PT_NODE *outer (PT_NODE * from)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      PT_NODE *a = from->info.spec.as_attr_list;
      q->info.query.q.select.list = name ("a", a != NULL ? a->type_enum : PT_TYPE_INTEGER);
      q->info.query.q.select.from = from;
      return q;
    }

    PT_NODE *eval (PT_NODE * n, bool donot_fold = false)
    {
      return ut_eval_type_node (parser, n, donot_fold);
    }
  };

  bool is_cast_to (const PT_NODE * n, PT_TYPE_ENUM t)
  {
    return n != NULL && n->node_type == PT_EXPR && n->info.expr.op == PT_CAST && n->info.expr.cast_type != NULL
      && n->info.expr.cast_type->type_enum == t && n->type_enum == t;
  }

  PT_NODE *nth (PT_NODE * list, int i)
  {
    while (list != NULL && i-- > 0)
      {
	list = list->next;
      }
    return list;
  }

  PT_NODE *items (PT_NODE * query)
  {
    return query->info.query.q.select.list;
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("PR8435 C1: a VARCHAR item (COALESCE (c, NULL) after folding) under the INTEGER column name resolution gave it "
	   "is cast back to INTEGER; the cast keeps the item under it, takes its alias and gives the query its type",
	   "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *item = t.name ("coalesce", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  item->alias_print = "a";
  PT_NODE *q = t.select (item);
  PT_NODE *spec = t.derived (q, t.name ("a", PT_TYPE_INTEGER));
  PT_NODE *o = t.outer (spec);

  REQUIRE (t.eval (o) == o);
  PT_NODE *first = items (spec->info.spec.derived_table);
  CHECK (is_cast_to (first, PT_TYPE_INTEGER));
  CHECK (first->info.expr.arg1 == item);
  CHECK ((first->alias_print != NULL && strcmp (first->alias_print, "a") == 0));
  CHECK (first->next == NULL);
  CHECK (spec->info.spec.derived_table->type_enum == PT_TYPE_INTEGER);
  CHECK (spec->info.spec.as_attr_list->type_enum == PT_TYPE_INTEGER);	/* the column keeps its type */
}

TEST_CASE ("PR8435 C1 boundary: a later item is cast in place -- the list stays linked and the query keeps the type of "
	   "its first column", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *i1 = t.name ("c", PT_TYPE_INTEGER);
  PT_NODE *i2 = t.name ("nvl", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *i3 = t.name ("d", PT_TYPE_DOUBLE);
  PT_NODE *q = t.select (tree::chain ({i1, i2, i3}));
  PT_NODE *spec = t.derived (q, tree::chain ({t.name ("c", PT_TYPE_INTEGER),
					      t.name ("b", PT_TYPE_BIGINT), t.name ("d", PT_TYPE_DOUBLE)}));
  PT_NODE *o = t.outer (spec);

  REQUIRE (t.eval (o) == o);
  PT_NODE *list = items (spec->info.spec.derived_table);
  CHECK (list == i1);
  CHECK (is_cast_to (nth (list, 1), PT_TYPE_BIGINT));
  CHECK (nth (list, 1)->info.expr.arg1 == i2);
  CHECK (nth (list, 2) == i3);
  CHECK (nth (list, 3) == NULL);
  CHECK (spec->info.spec.derived_table->type_enum == PT_TYPE_INTEGER);
}

TEST_CASE ("PR8435 C2: an item of its column's type is left as it is", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *i1 = t.name ("c", PT_TYPE_INTEGER);
  PT_NODE *i2 = t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10));
  PT_NODE *q = t.select (tree::chain ({i1, i2}));
  PT_NODE *spec = t.derived (q, tree::chain ({t.name ("c", PT_TYPE_INTEGER),
					      t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10))}));
  PT_NODE *o = t.outer (spec);

  REQUIRE (t.eval (o) == o);
  CHECK (items (spec->info.spec.derived_table) == i1);
  CHECK (i1->next == i2);
  CHECK (i2->next == NULL);
}

TEST_CASE ("PR8435 C3: NULL, NA, MAYBE and NONE fit any column, and a column of those types takes any item", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  const PT_TYPE_ENUM loose[] = { PT_TYPE_NULL, PT_TYPE_NA, PT_TYPE_MAYBE, PT_TYPE_NONE };
  for (PT_TYPE_ENUM l:loose)
    {
      PT_NODE *item = t.name ("i", l);
      PT_NODE *spec = t.derived (t.select (item), t.name ("a", PT_TYPE_INTEGER));
      REQUIRE (t.eval (t.outer (spec)) != NULL);
      CHECK (items (spec->info.spec.derived_table) == item);

      PT_NODE *item2 = t.name ("j", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
      PT_NODE *spec2 = t.derived (t.select (item2), t.name ("b", l));
      REQUIRE (t.eval (t.outer (spec2)) != NULL);
      CHECK (items (spec2->info.spec.derived_table) == item2);
    }
}

TEST_CASE ("PR8435 C4: objects and collections keep their own domains -- an OBJECT column, a collection column "
	   "(COALESCE (sequence, NULL) folds to VARCHAR too) and a collection item are not cast", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *i1 = t.name ("o", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *s1 = t.derived (t.select (i1), t.name ("o", PT_TYPE_OBJECT));
  REQUIRE (t.eval (t.outer (s1)) != NULL);
  CHECK (items (s1->info.spec.derived_table) == i1);

  PT_NODE *i2 = t.name ("q", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *s2 = t.derived (t.select (i2), t.name ("q", PT_TYPE_SEQUENCE));
  REQUIRE (t.eval (t.outer (s2)) != NULL);
  CHECK (items (s2->info.spec.derived_table) == i2);

  PT_NODE *i3 = t.name ("m", PT_TYPE_SET);
  PT_NODE *s3 = t.derived (t.select (i3), t.name ("m", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR)));
  REQUIRE (t.eval (t.outer (s3)) != NULL);
  CHECK (items (s3->info.spec.derived_table) == i3);
}

TEST_CASE ("PR8435 C5: the same type with another precision is left alone -- VARCHAR (10) under VARCHAR (20), "
	   "NUMERIC (10, 2) under NUMERIC (5, 1)", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *i1 = t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10));
  PT_NODE *i2 = t.name ("n", PT_TYPE_NUMERIC, t.dt (PT_TYPE_NUMERIC, 10, 2));
  PT_NODE *spec = t.derived (t.select (tree::chain ({i1, i2})),
			     tree::chain ({t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 20)),
					   t.name ("n", PT_TYPE_NUMERIC, t.dt (PT_TYPE_NUMERIC, 5, 1))}));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  CHECK (items (spec->info.spec.derived_table) == i1);
  CHECK (i1->next == i2);
}

TEST_CASE ("PR8435 C6: items pair with columns by position, hidden items included -- the sorted derived table of "
	   "mq_rewrite_order_dependent_query () (hidden sort item k first, with its own column) keeps every item; the "
	   "first commit 73ae5c347 skipped k and cast s to INTEGER", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *k = t.hidden (t.name ("k", PT_TYPE_INTEGER));
  PT_NODE *s = t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10));
  PT_NODE *d = t.name ("d", PT_TYPE_NUMERIC, t.dt (PT_TYPE_NUMERIC, 5, 1));
  PT_NODE *spec = t.derived (t.select (tree::chain ({k, s, d})),
			     tree::chain ({t.name ("k", PT_TYPE_INTEGER),
					   t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10)),
					   t.name ("d", PT_TYPE_NUMERIC, t.dt (PT_TYPE_NUMERIC, 5, 1))}));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  PT_NODE *list = items (spec->info.spec.derived_table);
  CHECK (list == k);
  CHECK (nth (list, 1) == s);
  CHECK (nth (list, 2) == d);
}

TEST_CASE ("PR8435 C6 boundary: behind a hidden first item, a changed item is cast to its own column's type (VARCHAR "
	   "(10)), not to the column before it", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *k = t.hidden (t.name ("k", PT_TYPE_INTEGER));
  PT_NODE *s = t.name ("s", PT_TYPE_INTEGER);
  PT_NODE *spec = t.derived (t.select (tree::chain ({k, s})),
			     tree::chain ({t.name ("k", PT_TYPE_INTEGER),
					   t.name ("s", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR, 10))}));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  PT_NODE *list = items (spec->info.spec.derived_table);
  CHECK (list == k);
  PT_NODE *c = nth (list, 1);
  CHECK (is_cast_to (c, PT_TYPE_VARCHAR));
  CHECK ((c != NULL && c->info.expr.cast_type != NULL && c->info.expr.cast_type->info.data_type.precision == 10));
}

TEST_CASE ("PR8435 C7: hidden ORDER BY items appended after name resolution fall past the last column and are left "
	   "alone", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *a = t.name ("coalesce", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *h1 = t.hidden (t.name ("h1", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR)));
  PT_NODE *h2 = t.hidden (t.name ("h2", PT_TYPE_DOUBLE));
  PT_NODE *spec = t.derived (t.select (tree::chain ({a, h1, h2})), t.name ("a", PT_TYPE_INTEGER));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  PT_NODE *list = items (spec->info.spec.derived_table);
  CHECK (is_cast_to (list, PT_TYPE_INTEGER));
  CHECK (nth (list, 1) == h1);
  CHECK (nth (list, 2) == h2);
  CHECK (nth (list, 3) == NULL);
}

TEST_CASE ("PR8435 C8: a set operation is cast operand by operand against the same columns, nested operations and "
	   "DIFFERENCE / INTERSECTION included", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *a1 = t.name ("coalesce", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *a2 = t.name ("c", PT_TYPE_INTEGER);
  PT_NODE *a3 = t.name ("nvl", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *q1 = t.select (a1), *q2 = t.select (a2), *q3 = t.select (a3);
  PT_NODE *u = t.set_op (PT_UNION, t.set_op (PT_DIFFERENCE, q1, q2), q3);
  PT_NODE *spec = t.derived (u, t.name ("a", PT_TYPE_INTEGER));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  CHECK (is_cast_to (items (q1), PT_TYPE_INTEGER));
  CHECK (items (q2) == a2);
  CHECK (is_cast_to (items (q3), PT_TYPE_INTEGER));
  CHECK (q1->type_enum == PT_TYPE_INTEGER);
  CHECK (q3->type_enum == PT_TYPE_INTEGER);

  PT_NODE *b1 = t.name ("ifnull", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *r1 = t.select (b1), *r2 = t.select (t.name ("c", PT_TYPE_INTEGER));
  PT_NODE *spec2 = t.derived (t.set_op (PT_INTERSECTION, r1, r2), t.name ("a", PT_TYPE_INTEGER));
  REQUIRE (t.eval (t.outer (spec2)) != NULL);
  CHECK (is_cast_to (items (r1), PT_TYPE_INTEGER));
}

TEST_CASE ("PR8435 C8 pipeline: the TC's UNION ALL (CTP Case 3) -- after the set operation's own step unified a folded "
	   "VARCHAR and 1 to DOUBLE, every operand item ends with the INTEGER type of the column, and 123456789 comes back "
	   "whole through the casts", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *a1 = t.name ("coalesce", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *a2 = t.name ("one", PT_TYPE_INTEGER);
  PT_NODE *u = t.set_op (PT_UNION, t.select (a1), t.select (a2));
  REQUIRE (t.eval (u) != NULL);	/* pt_semantic_type () of the UNION after its operands folded */
  REQUIRE (is_cast_to (items (u->info.query.q.union_.arg1), PT_TYPE_DOUBLE));
  REQUIRE (is_cast_to (items (u->info.query.q.union_.arg2), PT_TYPE_DOUBLE));

  PT_NODE *spec = t.derived (u, t.name ("a", PT_TYPE_INTEGER));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  CHECK (items (u->info.query.q.union_.arg1)->type_enum == PT_TYPE_INTEGER);
  CHECK (items (u->info.query.q.union_.arg2)->type_enum == PT_TYPE_INTEGER);

  DB_VALUE v, d, r;
  db_make_int (&v, 123456789);
  REQUIRE (tp_value_cast (&v, &d, tp_domain_resolve_default (DB_TYPE_DOUBLE), false) == DOMAIN_COMPATIBLE);
  REQUIRE (tp_value_cast (&d, &r, tp_domain_resolve_default (DB_TYPE_INTEGER), false) == DOMAIN_COMPATIBLE);
  CHECK (db_get_int (&r) == 123456789);
}

TEST_CASE ("PR8435 C9: a VALUES query and a derived table of another kind are left alone", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *i1 = t.name ("v", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *q1 = t.select (i1);
  PT_SET_VALUE_QUERY (q1);
  PT_NODE *s1 = t.derived (q1, t.name ("v", PT_TYPE_INTEGER));
  REQUIRE (t.eval (t.outer (s1)) != NULL);
  CHECK (items (q1) == i1);

  PT_NODE *i2 = t.name ("w", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *q2 = t.select (i2);
  PT_NODE *s2 = t.derived (q2, t.name ("w", PT_TYPE_INTEGER), PT_IS_SHOWSTMT);
  REQUIRE (t.eval (t.outer (s2)) != NULL);
  CHECK (items (q2) == i2);
}

/* ---------------------------------------------------------------- defect */

TEST_CASE ("PR8435 D1: a set operation operand already unified to DOUBLE keeps that cast under the column cast -- "
	   "(b BIGINT) UNION ALL (folded VARCHAR) under a BIGINT column becomes CAST (CAST (b AS DOUBLE) AS BIGINT), so "
	   "2^53 + 1 comes back as 2^53 and the largest BIGINT does not come back (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *a1 = t.name ("b", PT_TYPE_BIGINT);
  PT_NODE *a2 = t.name ("coalesce", PT_TYPE_VARCHAR, t.dt (PT_TYPE_VARCHAR));
  PT_NODE *u = t.set_op (PT_UNION, t.select (a1), t.select (a2));
  REQUIRE (t.eval (u) != NULL);	/* number + string unify to DOUBLE (pt_common_type) */
  PT_NODE *c1 = items (u->info.query.q.union_.arg1);
  PT_NODE *c2 = items (u->info.query.q.union_.arg2);
  REQUIRE (is_cast_to (c1, PT_TYPE_DOUBLE));
  REQUIRE (is_cast_to (c2, PT_TYPE_DOUBLE));
  REQUIRE (c1->etc != NULL);	/* the mark of a cast the set operation added (pt_make_cast_with_compatible_info) */

  PT_NODE *spec = t.derived (u, t.name ("b", PT_TYPE_BIGINT));
  REQUIRE (t.eval (t.outer (spec)) != NULL);
  PT_NODE *f1 = items (u->info.query.q.union_.arg1);
  PT_NODE *f2 = items (u->info.query.q.union_.arg2);
  CHECK (is_cast_to (f1, PT_TYPE_BIGINT));
  CHECK ((f1 != NULL && f1->node_type == PT_EXPR && f1->info.expr.arg1 == c1));
  CHECK (is_cast_to (f2, PT_TYPE_BIGINT));
  CHECK ((f2 != NULL && f2->node_type == PT_EXPR && f2->info.expr.arg1 == c2));

  /* what that pair of casts does to a value (tp_value_cast, the cast the executor runs for a PT_CAST) */
  DB_VALUE v, d, r;
  db_make_bigint (&v, 9007199254740993LL);
  REQUIRE (tp_value_cast (&v, &d, tp_domain_resolve_default (DB_TYPE_DOUBLE), false) == DOMAIN_COMPATIBLE);
  REQUIRE (tp_value_cast (&d, &r, tp_domain_resolve_default (DB_TYPE_BIGINT), false) == DOMAIN_COMPATIBLE);
  CHECK (db_get_bigint (&r) == 9007199254740992LL);

  DB_VALUE vm, dm, rm;
  db_make_bigint (&vm, INT64_MAX);
  REQUIRE (tp_value_cast (&vm, &dm, tp_domain_resolve_default (DB_TYPE_DOUBLE), false) == DOMAIN_COMPATIBLE);
  db_make_null (&rm);
  TP_DOMAIN_STATUS st = tp_value_cast (&dm, &rm, tp_domain_resolve_default (DB_TYPE_BIGINT), false);
  INFO ("INT64_MAX -> DOUBLE -> BIGINT: status " << (int) st << ", value "
	<< (DB_IS_NULL (&rm) ? 0LL : (long long) db_get_bigint (&rm)));
  CHECK ((st != DOMAIN_COMPATIBLE || db_get_bigint (&rm) != INT64_MAX));
}
