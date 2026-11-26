/*
 * PR#8440 (CBRD-27351) D2-01 -- mq_share_settle_groups () of src/parser/view_transform.c (CS_MODE) run on a subquery
 * before the whole statement.
 *
 * With the result cache on (max_query_cache_entries and query_cache_size_in_pages > 0), do_prepare_statement () first
 * prepares each uncorrelated subquery with the QUERY_CACHE hint by itself (execute_statement.c:4168 -> 4140 ->
 * do_prepare_subquery_pre () 4101-4106 -> do_prepare_subquery () 16969): do_prepare_select () gets the subquery node of
 * the statement tree, next kept (restored at 16957), and settles the share groups of that node (16523-16526); on an
 * XASL cache miss parser_generate_xasl () settles the node again, next cut (xasl_generation.c:29506-29507, 29543) and
 * keeps the groups in its share_groups. Then the statement is prepared: do_prepare_select () and
 * parser_generate_xasl () settle the whole statement.
 *
 * mq_share_collect_pre () leaves out only correlated subqueries (view_transform.c:16914), so the references in the
 * QUERY_CACHE subquery are members of the statement's group. Settled alone, the subquery counts its own members only:
 * n_kept == n_members, and mq_share_restore_pre () narrows them to the columns they read (17478-17480).
 * mq_share_narrow_cols () picks from the current as_attr_list in its order (17348-17357), so the later settle of the
 * statement cannot widen them back. The donor is the member the walk meets first (17290); pt_apply_select () walks the
 * select list before FROM (parse_tree_cl.c:15139-15140), so a subquery in the select list holds the donor. A member
 * reads the donor's result at the positions of its own as_attr_list (xasl_generation.c:5610-5611, 17582-17595).
 *
 * Statement of the cases (the SQL of the finding's proposal (3)):
 *   create view v (k, sx, sy) as select k, sum (x), sum (y) from t group by k;
 *   select (select [/\*+ query_cache *\/] count (*) from v i1, v i2 where i1.k = i2.sy) c, sum (o.sx) s from v o;
 * and the WHERE form of proposal (4):
 *   select sum (o.sx) from v o where o.k in (select /\*+ query_cache *\/ i1.k from v i1, v i2 where i1.k = i2.k);
 *
 * Steps (real code of the TU): mq_share_view_refs () on the statement (mq_translate_helper (), 10035); each view
 * reference rewritten as a derived table in the order mq_translate_local () takes the SELECTs (post order: the
 * subquery's, then the statement's), by the NON_PUSHABLE branch of mq_substitute_subquery_in_statement () (3088-3097:
 * mq_is_removable_select_list () of the body, mq_rewrite_vclass_spec_as_derived ()); then the settles, named after
 * their callers:
 *   T      mq_translate_helper () on the statement (10038)
 *   P-sub  do_prepare_select () on the subquery node, next kept (execute_statement.c:16525 via 16969)
 *   X-sub  parser_generate_xasl () on the subquery node, next cut (xasl_generation.c:29543), on an XASL cache miss
 *   P      do_prepare_select () on the statement (16525)
 *   X      parser_generate_xasl () on the statement (29543)
 * The body of the view is not merged into the derived table (mq_substitute_select_in_statement () at 3128 is not run):
 * the narrowing reads only the width of the derived table's select list, its as_attr_list and the removability checks
 * (no hint, DISTINCT, CONNECT BY, :=, WITH or CTE in this body), which the merge does not change.
 *
 * Real code: view_transform.c (the TU), parse_tree.c + parse_tree_cl.c (parser_create_parser, parser_new_node,
 * parser_copy_tree, parser_walk_tree, parser_append_node, parser_free_tree, parser_free_parser), parser_support.c
 * (pt_name, pt_name_equal, pt_has_define_vars, pt_has_analytic, pt_has_aggregate, pt_make_integer_value, ...),
 * language_support.c + intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c,
 * memory_alloc.c, system_parameter.c.
 * Seams (same TU): mq_fetch_attributes (the view cache's attrs, the oid first -- mq_virtual_queries ()) and
 * mq_fetch_subqueries (the view cache's translated query) -- both return what the cases give. Fakes: db_is_vclass
 * (db_virt.c: 1 for the view object of the cases), pt_str_compare (name_resolution.c body), pt_is_result_reuse_safe
 * (xasl_generation.c: true -- the body holds no expression it refuses), pt_true_search_condition (type_checking.c:7318
 * body), and the language / error fakes of parser/view_transform.share_spec_ids.
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
#include <strings.h>
#include <string>

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

/* name_resolution.c pt_free_dblink_remote_cols (): a parser that resolved no dblink table has none */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

/* error_context.cpp: a base level and a stack of levels; er_set / er_errid / er_clear work on the current level */
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

/* xasl_generation.c pt_is_result_reuse_safe (): the body of v has no random value, serial, session variable or
 * nondeterministic stored function */
bool
pt_is_result_reuse_safe (PARSER_CONTEXT * parser, PT_NODE * tree)
{
  return true;
}

/* type_checking.c:7318 pt_true_search_condition () body (mq_is_pushable_subquery () asks it of the WHERE) */
bool
pt_true_search_condition (PARSER_CONTEXT * parser, const PT_NODE * node)
{
  while (node)
    {
      if (node->or_next != NULL || node->node_type != PT_VALUE || node->type_enum != PT_TYPE_LOGICAL
	  || node->info.value.data_value.i != 1)
	{
	  return false;
	}
      node = node->next;
    }
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

/* appendix (hooks.cpp) */
void ut_share_view_refs (PARSER_CONTEXT * parser, PT_NODE * statement);
int ut_view_is_merged (PARSER_CONTEXT * parser, PT_NODE * select, PT_NODE * spec);
PT_NODE *ut_rewrite_view_spec (PARSER_CONTEXT * parser, PT_NODE * select, PT_NODE * spec);

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

  enum form
  {
    /* select (select count (*) from v i1, v i2 where i1.k = i2.sy) c, sum (o.sx) s from v o */
    SELECT_LIST_PLAIN,
    /* the same with select / *+ query_cache * / in the subquery (proposal (3)) */
    SELECT_LIST_QUERY_CACHE,
    /* select sum (o.sx) from v o where o.k in (select / *+ query_cache * / i1.k from v i1, v i2 where i1.k = i2.k)
     * (proposal (4)) */
    WHERE_QUERY_CACHE
  };

  struct stmt
  {
    PARSER_CONTEXT *parser;
    PT_NODE *root;		/* the statement */
    PT_NODE *sub;		/* the subquery */
    PT_NODE *o, *i1, *i2;	/* the references to v: o in the statement, i1 and i2 in the subquery */
    int merged;			/* references whose body mq_is_pushable_subquery () would merge */

    explicit stmt (form f)
    {
      parser = parser_create_parser ();
      merged = 0;

      /* the view cache of v: "select * from [v]" with the row oid first (mq_virtual_queries ()) */
      g_view_attrs = name ("v", 0, PT_OID_ATTR, PT_TYPE_OBJECT);
      g_view_attrs = parser_append_node (name ("k", 0, PT_NORMAL, PT_TYPE_INTEGER), g_view_attrs);
      g_view_attrs = parser_append_node (name ("sx", 0, PT_NORMAL, PT_TYPE_BIGINT), g_view_attrs);
      g_view_attrs = parser_append_node (name ("sy", 0, PT_NORMAL, PT_TYPE_BIGINT), g_view_attrs);

      /* its translated query: select k, sum (x), sum (y) from t group by k */
      PT_NODE *t = parser_new_node (parser, PT_SPEC);
      t->info.spec.id = (UINTPTR) t;
      t->info.spec.meta_class = PT_CLASS;
      t->info.spec.entity_name = name ("t", t->info.spec.id, PT_CLASS, PT_TYPE_NONE);
      t->info.spec.range_var = name ("t", t->info.spec.id, PT_NORMAL, PT_TYPE_NONE);
      g_view_body = parser_new_node (parser, PT_SELECT);
      g_view_body->info.query.q.select.list = ref (t, "k", PT_TYPE_INTEGER);
      g_view_body->info.query.q.select.list =
	parser_append_node (aggregate (PT_SUM, ref (t, "x", PT_TYPE_INTEGER), PT_TYPE_BIGINT),
			    g_view_body->info.query.q.select.list);
      g_view_body->info.query.q.select.list =
	parser_append_node (aggregate (PT_SUM, ref (t, "y", PT_TYPE_INTEGER), PT_TYPE_BIGINT),
			    g_view_body->info.query.q.select.list);
      g_view_body->info.query.q.select.from = t;
      PT_NODE *group_by = parser_new_node (parser, PT_SORT_SPEC);
      group_by->info.sort_spec.expr = ref (t, "k", PT_TYPE_INTEGER);
      group_by->info.sort_spec.asc_or_desc = PT_ASC;
      g_view_body->info.query.q.select.group_by = group_by;
      PT_SELECT_INFO_SET_FLAG (g_view_body, PT_SELECT_INFO_HAS_AGG);

      o = view_spec ("o");
      i1 = view_spec ("i1");
      i2 = view_spec ("i2");

      sub = parser_new_node (parser, PT_SELECT);
      sub->info.query.is_subquery = PT_IS_SUBQUERY;
      sub->info.query.correlation_level = 0;
      if (f != SELECT_LIST_PLAIN)
	{
	  /* scanner_support.c:613-616: the QUERY_CACHE hint of a query goes to info.query.hint */
	  sub->info.query.hint = PT_HINT_QUERY_CACHE;
	}
      i1->next = i2;
      sub->info.query.q.select.from = i1;
      PT_NODE *eq = parser_new_node (parser, PT_EXPR);
      eq->info.expr.op = PT_EQ;
      eq->info.expr.arg1 = ref (i1, "k", PT_TYPE_INTEGER);
      eq->info.expr.arg2 = ref (i2, (f == WHERE_QUERY_CACHE) ? "k" : "sy", PT_TYPE_INTEGER);
      eq->type_enum = PT_TYPE_LOGICAL;
      sub->info.query.q.select.where = eq;

      root = parser_new_node (parser, PT_SELECT);
      root->info.query.q.select.from = o;
      PT_NODE *sum = aggregate (PT_SUM, ref (o, "sx", PT_TYPE_BIGINT), PT_TYPE_NUMERIC);
      PT_SELECT_INFO_SET_FLAG (root, PT_SELECT_INFO_HAS_AGG);
      if (f == WHERE_QUERY_CACHE)
	{
	  /* select i1.k from v i1, v i2 where i1.k = i2.k */
	  sub->info.query.q.select.list = ref (i1, "k", PT_TYPE_INTEGER);
	  sub->type_enum = PT_TYPE_INTEGER;
	  root->info.query.q.select.list = sum;
	  PT_NODE *in = parser_new_node (parser, PT_EXPR);
	  in->info.expr.op = PT_IS_IN;
	  in->info.expr.arg1 = ref (o, "k", PT_TYPE_INTEGER);
	  in->info.expr.arg2 = sub;
	  in->type_enum = PT_TYPE_LOGICAL;
	  root->info.query.q.select.where = in;
	}
      else
	{
	  /* select count (*) from v i1, v i2 where i1.k = i2.sy, then sum (o.sx) after it in the select list */
	  sub->info.query.q.select.list = aggregate (PT_COUNT_STAR, NULL, PT_TYPE_INTEGER);
	  PT_SELECT_INFO_SET_FLAG (sub, PT_SELECT_INFO_HAS_AGG);
	  sub->type_enum = PT_TYPE_INTEGER;
	  sub->next = sum;
	  root->info.query.q.select.list = sub;
	}
    }

    ~stmt ()
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

    /* a column of spec as name resolution leaves it: spec_id is the spec's id, resolved its range variable */
    PT_NODE *ref (PT_NODE * spec, const char *column, PT_TYPE_ENUM type)
    {
      return name (column, spec->info.spec.id, PT_NORMAL, type, NULL, spec->info.spec.range_var->info.name.original);
    }

    PT_NODE *aggregate (FUNC_CODE code, PT_NODE * arg, PT_TYPE_ENUM type)
    {
      PT_NODE *f = parser_new_node (parser, PT_FUNCTION);
      f->info.function.function_type = code;
      f->info.function.arg_list = arg;
      f->info.function.all_or_distinct = PT_ALL;
      f->type_enum = type;
      return f;
    }

    /* a spec of view v as name resolution leaves it: its id is its address, and its entity name, flat entity name and
     * range variable carry the id (name_resolution.c:4912, 7869-7893) */
    PT_NODE *view_spec (const char *range)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.id = (UINTPTR) s;
      s->info.spec.meta_class = PT_CLASS;
      s->info.spec.entity_name = name ("v", s->info.spec.id, PT_CLASS, PT_TYPE_NONE, ut_view ());
      s->info.spec.flat_entity_list = name ("v", s->info.spec.id, PT_CLASS, PT_TYPE_NONE, ut_view ());
      s->info.spec.range_var = name (range, s->info.spec.id, PT_NORMAL, PT_TYPE_NONE);
      return s;
    }

    bool rewrite (PT_NODE * select, PT_NODE * spec)
    {
      merged += ut_view_is_merged (parser, select, spec);
      return ut_rewrite_view_spec (parser, select, spec) == spec && !pt_has_error (parser);
    }

    /* mq_translate_helper (): mq_share_view_refs () on the statement (10035), then mq_translate_local () in post
     * order -- the subquery's SELECT first, then the statement's */
    bool translate ()
    {
      ut_share_view_refs (parser, root);
      return !pt_has_error (parser) && rewrite (sub, i1) && rewrite (sub, i2) && rewrite (root, o);
    }

    PT_SHARE_GROUP *settle_statement ()
    {
      return mq_share_settle_groups (parser, root);
    }

    /* do_prepare_subquery () -> do_prepare_select (): the subquery node of the statement tree with its next (the
     * select list item after it), marked first as a subquery prepared for the result cache
     * (execute_statement.c:16881) */
    PT_SHARE_GROUP *settle_subquery_prepare ()
    {
      sub->info.query.flag.subquery_cached = 1;
      return mq_share_settle_groups (parser, sub);
    }

    /* parser_generate_xasl () on the subquery node, within the same do_prepare_subquery (): next cut while it
     * settles */
    PT_SHARE_GROUP *settle_subquery_xasl ()
    {
      PT_NODE *next = sub->next;
      sub->info.query.flag.subquery_cached = 1;
      sub->next = NULL;
      PT_SHARE_GROUP *groups = mq_share_settle_groups (parser, sub);
      sub->next = next;
      return groups;
    }

    const char *label (PT_NODE * spec)
    {
      return spec == o ? "o" : spec == i1 ? "i1" : spec == i2 ? "i2" : (spec == NULL ? "-" : "?");
    }
  };

  std::string names (PT_NODE * list)
  {
    std::string s;
    for (PT_NODE * n = list; n != NULL; n = n->next)
      {
	if (!s.empty ())
	  {
	    s += ",";
	  }
	s += (n->node_type == PT_NAME) ? n->info.name.original : "?";
      }
    return s;
  }

  /* the columns of a member's derived table */
  std::string cols (PT_NODE * spec)
  {
    return names (spec->info.spec.as_attr_list);
  }

  int width (PT_NODE * spec)
  {
    int n = 0;
    PT_NODE *q = spec->info.spec.derived_table;
    for (PT_NODE * c = (q != NULL) ? q->info.query.q.select.list : NULL; c != NULL; c = c->next)
      {
	n++;
      }
    return n;
  }

  /* the select list of a member's derived table: what its XASL (the donor's: the body of the group's CTE) gives */
  std::string select_names (PT_NODE * spec)
  {
    PT_NODE *q = spec->info.spec.derived_table;
    return names ((q != NULL) ? q->info.query.q.select.list : NULL);
  }

  /* 1-based position of column in spec's as_attr_list, 0 for none */
  int position (PT_NODE * spec, const char *column)
  {
    int i = 1;
    for (PT_NODE * a = spec->info.spec.as_attr_list; a != NULL; a = a->next, i++)
      {
	if (a->node_type == PT_NAME && strcasecmp (a->info.name.original, column) == 0)
	  {
	    return i;
	  }
      }
    return 0;
  }

  const char *column_at (PT_NODE * spec, int pos)
  {
    int i = 1;
    for (PT_NODE * a = (spec != NULL) ? spec->info.spec.as_attr_list : NULL; a != NULL; a = a->next, i++)
      {
	if (i == pos)
	  {
	    return (a->node_type == PT_NAME) ? a->info.name.original : "?";
	  }
      }
    return NULL;
  }

  /* the PR's invariant (view_transform.c:17214-17216): the members of a group select the same columns in one order */
  bool members_agree (stmt & s)
  {
    PT_NODE *m[3] = { s.o, s.i1, s.i2 };
    for (int a = 0; a < 3; a++)
      {
	for (int b = a + 1; b < 3; b++)
	  {
	    if (m[a]->info.spec.share_group != NULL && m[a]->info.spec.share_group == m[b]->info.spec.share_group
		&& cols (m[a]) != cols (m[b]))
	      {
		return false;
	      }
	  }
      }
    return true;
  }

  /* what a member reads from its group's result: each column it reads (share_cols) at the position of that column in
   * its own as_attr_list (xasl_generation.c:5610-5611), in the donor's derived table (17582-17595) */
  bool reads_own_columns (PT_NODE * member)
  {
    PT_SHARE_GROUP *g = member->info.spec.share_group;
    if (g == NULL)
      {
	return true;		/* not a member: it reads its own derived table */
      }
    for (PT_NODE * c = member->info.spec.share_cols; c != NULL; c = c->next)
      {
	if (c->node_type != PT_NAME)
	  {
	    continue;
	  }
	int pos = position (member, c->info.name.original);
	const char *got = column_at (g->donor, pos);
	if (pos == 0 || got == NULL || strcasecmp (got, c->info.name.original) != 0)
	  {
	    return false;
	  }
      }
    return true;
  }

  void dump (const char *step, stmt & s)
  {
    PT_SHARE_GROUP *g = s.o->info.spec.share_group;
    if (g == NULL)
      {
	g = s.i1->info.spec.share_group;
      }
    fprintf (stderr, "  %-34s group %p n_members %d donor %-2s |", step, (void *) g, g ? g->n_members : 0,
	     s.label (g ? g->donor : NULL));
    PT_NODE *m[3] = { s.o, s.i1, s.i2 };
    for (int i = 0; i < 3; i++)
      {
	PT_NODE *q = m[i]->info.spec.derived_table;
	fprintf (stderr, " %s%s(%s) w%d%s", s.label (m[i]), m[i]->info.spec.share_group ? "" : "[-]",
		 cols (m[i]).c_str (), width (m[i]), (q && q->info.query.flag.shared_view_body) ? " shared" : "");
      }
    fprintf (stderr, "\n");
  }
}

TEST_CASE ("C0 premise: the aggregate body of v is not merged into a SELECT reading it (mq_is_pushable_subquery () -> "
	   "NON_PUSHABLE, view_transform.c:2432), so each reference is rewritten as a derived table (3088-3097); "
	   "mq_share_view_refs () puts o, i1, i2 in one group of k, sx, sy, each keeping the columns it reads",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_PLAIN);
  REQUIRE (s.translate ());
  fprintf (stderr, "C0:\n");
  dump ("translated (before T)", s);

  CHECK (s.merged == 0);
  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  CHECK (s.i1->info.spec.share_group == g);
  CHECK (s.i2->info.spec.share_group == g);
  CHECK_FALSE (g->all_columns);
  CHECK (names (s.o->info.spec.share_cols) == "sx");
  CHECK (names (s.i1->info.spec.share_cols) == "k");
  CHECK (names (s.i2->info.spec.share_cols) == "sy");
  CHECK (cols (s.o) == "k,sx,sy");
  CHECK (cols (s.i1) == "k,sx,sy");
  CHECK (cols (s.i2) == "k,sx,sy");
  CHECK (width (s.o) == 3);
}

TEST_CASE ("C1 control: the same statement with a plain subquery, settled only as a whole (T, P, X) -- one group of "
	   "three, the donor met first (i1, the select list before FROM), every member selects k, sx, sy and reads "
	   "its own columns", "[contract]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_PLAIN);
  REQUIRE (s.translate ());
  fprintf (stderr, "C1:\n");
  PT_SHARE_GROUP *t = s.settle_statement ();
  dump ("T", s);
  PT_SHARE_GROUP *p = s.settle_statement ();
  dump ("P", s);
  PT_SHARE_GROUP *x = s.settle_statement ();
  dump ("X", s);

  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  CHECK ((t == g && p == g && x == g));
  CHECK (g->n_members == 3);
  CHECK (g->donor == s.i1);
  CHECK (s.i1->info.spec.share_group == g);
  CHECK (s.i2->info.spec.share_group == g);
  CHECK (cols (s.o) == "k,sx,sy");
  CHECK (cols (s.i1) == "k,sx,sy");
  CHECK (cols (s.i2) == "k,sx,sy");
  CHECK ((width (s.o) == 3 && width (s.i1) == 3 && width (s.i2) == 3));
  CHECK (s.i1->info.spec.derived_table->info.query.flag.shared_view_body);
  CHECK (members_agree (s));
  CHECK ((reads_own_columns (s.o) && reads_own_columns (s.i1) && reads_own_columns (s.i2)));
}

TEST_CASE ("C2 control for D2-01: the statement with the QUERY_CACHE subquery settled only as a whole (T, P, X -- "
	   "step (2) alone): the members of a group select the same columns and each reads its own", "[contract]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "C2:\n");
  (void) s.settle_statement ();
  dump ("T", s);
  (void) s.settle_statement ();
  dump ("P", s);
  (void) s.settle_statement ();
  dump ("X", s);

  CHECK (members_agree (s));
  CHECK (reads_own_columns (s.o));
  CHECK (reads_own_columns (s.i1));
  CHECK (reads_own_columns (s.i2));
  CHECK (position (s.o, "sx") > 0);
}

TEST_CASE ("C3 control: the WHERE form of proposal (4), the real order (T, P-sub, X-sub, P, X) -- each member reads "
	   "its own columns even on the PR: the donor is o (FROM before WHERE), which keeps k at position 1, where "
	   "the narrowed subquery members read it (this query is a control for the TC, not a reproduction)",
	   "[contract]")
{
  REQUIRE (boot ());
  stmt s (WHERE_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "C3:\n");
  (void) s.settle_statement ();
  dump ("T", s);
  (void) s.settle_subquery_prepare ();
  dump ("P-sub", s);
  (void) s.settle_subquery_xasl ();
  dump ("X-sub", s);
  (void) s.settle_statement ();
  dump ("P", s);
  (void) s.settle_statement ();
  dump ("X", s);

  CHECK (reads_own_columns (s.o));
  CHECK (reads_own_columns (s.i1));
  CHECK (reads_own_columns (s.i2));
  fprintf (stderr, "  members agree: %s\n", members_agree (s) ? "yes" : "no");
}

TEST_CASE ("PR8440 D2-01 F1a: do_prepare_select () settles the QUERY_CACHE subquery alone (execute_statement.c:16525 "
	   "via 16969): the group counts i1 and i2 only, and narrows both to the columns they read (k, sy) while o "
	   "keeps k, sx, sy (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "F1a:\n");
  (void) s.settle_statement ();
  dump ("T", s);
  (void) s.settle_subquery_prepare ();
  dump ("P-sub", s);

  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  REQUIRE (s.i1->info.spec.share_group == g);
  REQUIRE (s.i2->info.spec.share_group == g);
  CHECK (g->n_members == 2);
  CHECK (g->donor == s.i1);
  CHECK (cols (s.i1) == "k,sy");
  CHECK (cols (s.i2) == "k,sy");
  CHECK (cols (s.o) == "k,sx,sy");
  CHECK ((width (s.i1) == 2 && width (s.o) == 3));
}

TEST_CASE ("PR8440 D2-01 F1b: the settles of the whole statement that follow (do_prepare_select (), "
	   "parser_generate_xasl ()) do not widen them back -- one group of three whose donor i1 selects k, sy while o "
	   "selects k, sx, sy: o reads sx at position 2, which is sy in the donor's result (PASS = defect present)",
	   "[defect]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "F1b:\n");
  (void) s.settle_statement ();
  dump ("T", s);
  (void) s.settle_subquery_prepare ();
  dump ("P-sub", s);
  (void) s.settle_subquery_xasl ();
  dump ("X-sub", s);
  (void) s.settle_statement ();
  dump ("P", s);
  (void) s.settle_statement ();
  dump ("X", s);

  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  REQUIRE (s.i1->info.spec.share_group == g);
  REQUIRE (s.i2->info.spec.share_group == g);
  CHECK (g->n_members == 3);
  REQUIRE (g->donor == s.i1);
  CHECK (cols (s.o) == "k,sx,sy");
  CHECK (cols (s.i1) == "k,sy");
  CHECK (cols (s.i2) == "k,sy");
  CHECK ((width (s.i1) == 2 && width (s.o) == 3));
  CHECK_FALSE (members_agree (s));
  int pos = position (s.o, "sx");
  const char *got = column_at (g->donor, pos);
  fprintf (stderr, "  o reads sx at position %d of the donor %s's result: %s\n", pos, s.label (g->donor),
	   got ? got : "(none)");
  fprintf (stderr, "  derived select lists: o (%s), donor i1 (%s), i2 (%s)\n", select_names (s.o).c_str (),
	   select_names (s.i1).c_str (), select_names (s.i2).c_str ());
  CHECK (pos == 2);
  REQUIRE (got != NULL);
  CHECK (strcmp (got, "sy") == 0);
  CHECK_FALSE (reads_own_columns (s.o));
}

TEST_CASE ("PR8440 D2-01 F1c: (premise of the second CTE, claim (5)) the settle parser_generate_xasl () runs for the "
	   "subquery's own XASL returns the statement's group, which xasl_generation.c:29543 keeps in the subquery "
	   "node's share_groups; the settle of the statement returns the same group, so pt_set_aptr () (18293) finds "
	   "it in both blocks (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "F1c:\n");
  (void) s.settle_statement ();
  (void) s.settle_subquery_prepare ();
  PT_SHARE_GROUP *sub_groups = s.settle_subquery_xasl ();
  dump ("X-sub", s);
  (void) s.settle_statement ();
  PT_SHARE_GROUP *root_groups = s.settle_statement ();
  dump ("X", s);

  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  CHECK (sub_groups == g);
  CHECK (root_groups == g);
  CHECK (g->n_members == 3);
}

TEST_CASE ("PR8440 D2-01 F1d: proposal (2) is not enough -- without do_prepare_select ()'s settle of the subquery "
	   "(its guard), parser_generate_xasl () still settles the subquery alone when the subquery's XASL is not in "
	   "the cache (xasl_generation.c:29543), and the members part the same way (T, X-sub, P, X) (PASS = defect "
	   "present)", "[defect]")
{
  REQUIRE (boot ());
  stmt s (SELECT_LIST_QUERY_CACHE);
  REQUIRE (s.translate ());
  fprintf (stderr, "F1d:\n");
  (void) s.settle_statement ();
  dump ("T", s);
  (void) s.settle_subquery_xasl ();
  dump ("X-sub", s);
  (void) s.settle_statement ();
  dump ("P", s);
  (void) s.settle_statement ();
  dump ("X", s);

  PT_SHARE_GROUP *g = s.o->info.spec.share_group;
  REQUIRE (g != NULL);
  REQUIRE (s.i1->info.spec.share_group == g);
  CHECK (g->donor == s.i1);
  CHECK (cols (s.i1) == "k,sy");
  CHECK (cols (s.o) == "k,sx,sy");
  CHECK_FALSE (members_agree (s));
  CHECK_FALSE (reads_own_columns (s.o));
}
