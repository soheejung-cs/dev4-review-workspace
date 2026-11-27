/*
 * PR#8696 (CBRD-28231) -- pt_to_false_subquery () of src/parser/type_checking.c (CS_MODE): a SELECT known to return
 * no row (pt_false_where ()) is rewritten. On the outer side of a join, as a sort spec or as the source of
 * INSERT ... SELECT (has_outer_spec / is_sort_spec / is_insert_select) it stays a query, "SELECT NULL, ... FROM
 * table({}) WHERE false"; anywhere else it becomes a NULL value marked is_false_where. Caller: pt_eval_type ()'s
 * PT_SELECT branch (type_checking.c:8357-8362 at bb9575015), after pt_where_type () on the WHERE.
 *
 * The contract the PR sets (type_checking.c:7386-7400, 7457-7461 at bb9575015):
 *   - the WHERE becomes one false constant unless pt_false_search_condition () already takes it as false, in which
 *     case it is kept as it is (bb9575015 "Keep a WHERE that is already false as it is"): the query may be false by
 *     another clause (HAVING, a folded derived table or CTE in its FROM), and the WHERE it had then refers to the
 *     FROM list the rewrite removed
 *   - PT_SELECT_INFO_HAS_AGG and PT_SELECT_INFO_HAS_ANALYTIC are cleared (pt_has_aggregate () / pt_has_analytic ()
 *     take the rewritten query as a plain one); the other flags stay
 *   - unchanged: select list -> one NULL per visible column, FROM -> table({}) av6749(av_1), GROUP BY / HAVING /
 *     CONNECT BY / START WITH / ORDER BY cleared; the other path -> NULL value keeping next, position, alias, hidden
 * base (a617e19b7) keeps the WHERE and both flags: the cases that expect them changed FAIL there (the fix).
 *
 * Not here: a single-row aggregate (no GROUP BY) folded through a false START WITH -- review finding G1-01, checked
 * with SQL separately.
 *
 * Real code: type_checking.c (the TU: pt_to_false_subquery, pt_eval_type, pt_where_type, pt_false_where,
 * pt_false_search_condition -- through hooks.cpp), parser_support.c (pt_name, pt_has_aggregate, pt_has_analytic),
 * parse_dbi.c (pt_value_to_db), parse_tree.c + parse_tree_cl.c (parser_create_parser, parser_new_node,
 * parser_free_tree, parser_append_node, pt_length_of_select_list, parser_walk_tree), language_support.c +
 * intl_support.c + chartype.c (en_US.utf8), object_domain.c + object_domain_convert.cpp + object_primitive.c +
 * numeric_opfunc.c, domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Fakes (suite.json "fakes"): pt_is_single_tuple (xasl_generation.c:4261 verbatim: GROUP BY -> false, else the real
 * pt_has_aggregate ()), er_* (one error slot), pt_parser_line_col (no lexer: position 0), pt_free_dblink_remote_cols,
 * init_builtin_calendar_names / db_date_locale_init, envvar_get / envvar_trim_char, cubregex::get_engine_name.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "system_parameter.h"
#include "error_manager.h"
#include "error_code.h"
#include "language_support.h"
#include "intl_support.h"
#include "environment_variable.h"
#include "string_regex.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

/* the appendix (hooks.cpp, same TU as pt_to_false_subquery / pt_eval_type) */
PT_NODE *ut_to_false_subquery (PARSER_CONTEXT * parser, PT_NODE * node);
PT_NODE *ut_eval_type_node (PARSER_CONTEXT * parser, PT_NODE * node);
PT_NODE *ut_fold_const_expr (PARSER_CONTEXT * parser, PT_NODE * expr);

/* schema_manager.c sm_remove_qualifier_name () (C++ linkage): the printing at the end of pt_fold_const_expr () strips the
 * owner from a resolved name -- the part after the first '.', or the name as it is (PR#8803 D1 aborted on the auto stub) */
/* keyword.c pt_is_keyword () (C linkage, parser.h): the printing quotes a name that is a keyword -- the names of these
 * cases (c, t, next_term) are none */
extern "C" bool
pt_is_keyword (const char *s)
{
  return false;
}

const char *
sm_remove_qualifier_name (const char *name)
{
  const char *dot = (name != NULL) ? strchr (name, '.') : NULL;
  return (dot != NULL) ? dot + 1 : name;
}

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

/* xasl_generation.c:4261 pt_is_single_tuple () verbatim: a SELECT with GROUP BY is not single-row, else it is when it
 * is an aggregate one (the real pt_has_aggregate () of parser_support.c) */
int
pt_is_single_tuple (PARSER_CONTEXT * parser, PT_NODE * select_node)
{
  if (select_node->info.query.q.select.group_by != NULL)
    {
      return false;
    }

  return pt_has_aggregate (parser, select_node);
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

    /* a resolved column reference of the given type */
    PT_NODE *name (const char *n, PT_TYPE_ENUM t = PT_TYPE_INTEGER)
    {
      PT_NODE *x = parser_new_node (parser, PT_NAME);
      x->info.name.original = pt_append_string (parser, NULL, n);
      x->info.name.resolved = "t";
      x->type_enum = t;
      return x;
    }

    PT_NODE *hidden (PT_NODE * x)
    {
      x->flag.is_hidden_column = 1;
      return x;
    }

    PT_NODE *ival (int v)
    {
      PT_NODE *x = parser_new_node (parser, PT_VALUE);
      x->type_enum = PT_TYPE_INTEGER;
      x->info.value.data_value.i = v;
      return x;
    }

    /* the single false conjunct pt_where_type () leaves for a constant-false search condition (always_false:) */
    PT_NODE *false_value ()
    {
      PT_NODE *w = parser_new_node (parser, PT_VALUE);
      w->type_enum = PT_TYPE_LOGICAL;
      w->info.value.data_value.i = 0;
      (void) pt_value_to_db (parser, w);
      return w;
    }

    /* a typed comparison <col> > <v>, as the type checking leaves a predicate that is not constant */
    PT_NODE *gt (PT_NODE * col, int v)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = PT_GT;
      e->info.expr.arg1 = col;
      e->info.expr.arg2 = ival (v);
      e->type_enum = PT_TYPE_LOGICAL;
      return e;
    }

    PT_NODE *sort_spec (PT_NODE * expr)
    {
      PT_NODE *s = parser_new_node (parser, PT_SORT_SPEC);
      s->info.sort_spec.expr = expr;
      s->info.sort_spec.asc_or_desc = PT_ASC;
      return s;
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
      s->info.spec.entity_name = name (table, PT_TYPE_NONE);
      s->info.spec.range_var = name (table, PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* the NULL value pt_to_false_subquery () leaves for a query that is not kept a query (is_false_where) */
    PT_NODE *folded_query ()
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_NULL;
      v->info.value.is_false_where = true;
      return v;
    }

    /* a derived table whose query was folded into that value (PT_IS_FALSE_WHERE_VALUE) */
    PT_NODE *folded_derived (const char *alias)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.derived_table = folded_query ();
      s->info.spec.derived_table_type = PT_IS_SUBQUERY;
      s->info.spec.range_var = name (alias, PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* a reference to a CTE whose non-recursive part was folded into that value */
    PT_NODE *folded_cte_ref (const char *cte_name)
    {
      PT_NODE *cte = parser_new_node (parser, PT_CTE);
      cte->info.cte.name = name (cte_name, PT_TYPE_NONE);
      cte->info.cte.non_recursive_part = folded_query ();
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.cte_pointer = cte;
      s->info.spec.range_var = name (cte_name, PT_TYPE_NONE);
      s->info.spec.id = (UINTPTR) s;
      return s;
    }

    /* a subquery typed as pt_eval_type () leaves it: the type of its first column */
    PT_NODE *select (PT_NODE * list, PT_NODE * from, PT_NODE * where)
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = list;
      q->info.query.q.select.from = from;
      q->info.query.q.select.where = where;
      q->info.query.is_subquery = PT_IS_SUBQUERY;
      q->info.query.all_distinct = PT_ALL;
      q->type_enum = list->type_enum;
      return q;
    }

    PT_NODE *to_false (PT_NODE * q)
    {
      return ut_to_false_subquery (parser, q);
    }

    PT_NODE *eval (PT_NODE * q)
    {
      return ut_eval_type_node (parser, q);
    }
  };

  /* one false constant: the WHERE the PR gives the rewritten query */
  bool is_false_constant (const PT_NODE * w)
  {
    return w != NULL && w->node_type == PT_VALUE && w->type_enum == PT_TYPE_LOGICAL && w->info.value.data_value.i == 0
      && w->next == NULL && w->or_next == NULL;
  }

  /* FROM table({}) av6749(av_1): the empty derived table of the rewrite */
  bool is_empty_table (const PT_NODE * from)
  {
    if (from == NULL || from->node_type != PT_SPEC || from->next != NULL)
      {
	return false;
      }
    const PT_NODE *set = from->info.spec.derived_table;
    const PT_NODE *rv = from->info.spec.range_var;
    return from->info.spec.derived_table_type == PT_IS_SET_EXPR && set != NULL && set->node_type == PT_VALUE
      && set->type_enum == PT_TYPE_SEQUENCE && rv != NULL && rv->node_type == PT_NAME
      && strcmp (rv->info.name.original, "av6749") == 0;
  }

  int count_nulls (const PT_NODE * list)
  {
    int n = 0;
    for (; list != NULL; list = list->next)
      {
	if (list->node_type != PT_VALUE || list->type_enum != PT_TYPE_NULL)
	  {
	    return -1;
	  }
	n++;
      }
    return n;
  }
}

/* ---------------------------------------------------------------- PR#8742 (CBRD-28224) */

TEST_CASE ("PR8742 C7: the NULL value pt_to_false_subquery () makes holds an initialized NULL DB_VALUE, so that it "
	   "prints as null after a view spec gives the column another type (pt_check_create_view ())", "[contract]")
{
  REQUIRE (boot ());
  tree t;
  PT_NODE *q = t.select (t.chain ({t.name ("a")}), t.entity ("t"), t.false_value ());

  PT_NODE *r = t.to_false (q);

  REQUIRE (r != NULL);
  REQUIRE (r->node_type == PT_VALUE);
  CHECK (r->info.value.is_false_where);
  CHECK (r->info.value.db_value_is_initialized);
  CHECK (DB_IS_NULL (&r->info.value.db_value));
  /* base 31d909869: no DB_VALUE, and a VARCHAR column printed the value as '' -- the view read '' back for NULL */
  r->type_enum = PT_TYPE_VARCHAR;
  PARSER_VARCHAR *printed = pt_print_bytes (t.parser, r);
  REQUIRE (printed != NULL);
  CHECK (strncmp ((const char *) printed->bytes, "null", 5) == 0);
}
