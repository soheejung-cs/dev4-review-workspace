/*
 * PR#8375 (CBRD-27855) -- qo_extract_or_restrictions () of src/optimizer/rewriter/query_rewrite_term.c (CS_MODE), the
 * derivation of single-spec restrictions from an OR factor pt_cnf () now keeps whole (query_rewrite.c:484).
 *
 * The PR moves the "must not be evaluated twice" judgment (a subquery, rand () ...) from the factor down to the
 * conjunct (query_rewrite_term.c:5322: the factor gate only asks whether the factor touches the spec; build_for_spec
 * leaves an unsafe conjunct out). The gate's walk is still qo_or_extract_check_ref_pre (), which stops the whole walk at
 * the first unsafe node (query_rewrite_term.c:4712) -- so the spec columns that come after it in walk order are not
 * seen.
 *
 *   G1 [contract] a multi-spec factor without unsafe pieces derives a restriction on each spec (develop and head)
 *   G2 [contract] with a subquery in a branch, the conjunct order `o.c = 1 and i.x in (...)` derives the restriction on
 *                 o (head: per-conjunct judgment; develop skipped the whole factor)
 *   D1 [defect]   G4-04: the same factor with the conjuncts the other way round, `i.x in (...) and o.c = 1`, derives
 *                 nothing on o -- the result depends on the conjunct order (PASS = the two orders differ)
 *
 * qo_rewrite_terms () is a seam (no-op fake): it normalizes the derived terms (RANGE, comparison pairs), which does not
 * decide whether a term on o is derived; everything before it is the real code.
 * Real code: query_rewrite_term.c (the TU), parse_tree.c + parse_tree_cl.c, parser_support.c (pt_and, pt_name_equal),
 * language_support.c + intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c,
 * memory_alloc.c, system_parameter.c.
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
#include <vector>

/* query_rewrite.h (included by the TU only) */
void qo_rewrite_terms (PARSER_CONTEXT * parser, PT_NODE * nodes, PT_NODE ** terms);
void qo_extract_or_restrictions (PARSER_CONTEXT * parser, PT_NODE * spec_list, PT_NODE ** wherep);

/* ---------------------------------------------------------------- fakes */

namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

const char *
envvar_get (const char *)
{
  return NULL;
}

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

void
init_builtin_calendar_names (LANG_LOCALE_DATA * lld)
{
}

void
db_date_locale_init (void)
{
}

/* scanner_support.c: no lexer, position 0 */
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

/* name_resolution.c: a parser that resolved no dblink table has no columns to free */
void
pt_free_dblink_remote_cols (PARSER_CONTEXT * parser)
{
}

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

void
er_stack_push (void)
{
}

void
er_stack_pop (void)
{
}

/* semantic_check.c:16138 pt_count_input_markers () -- the body as is */
PT_NODE *
pt_count_input_markers (PARSER_CONTEXT * parser, PT_NODE * node, void *arg, int *continue_walk)
{
  int *num_markers = (int *) arg;

  if (pt_is_input_hostvar (node))
    {
      if (*num_markers < node->info.host_var.index + 1)
	{
	  *num_markers = node->info.host_var.index + 1;
	}
    }
  return node;
}

/* keyword.c:1064 pt_is_keyword (): the printer quotes an identifier that is a keyword; the names here (a, b, c, x, y, z,
 * id) are none, so pt_find_keyword () finds no record */
bool
pt_is_keyword (const char *s)
{
  return false;
}

/* name_resolution.c:8441 pt_str_compare () -- the body as is */
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
  if (case_flag == CASE_INSENSITIVE)
    {
      return intl_identifier_casecmp (p, q);
    }
  return intl_identifier_cmp (p, q);
}

/* seam: query_rewrite_term.c qo_rewrite_terms () -- the term-level normalization of the derived terms; whether a
 * term on a spec is derived is decided before it */
void
qo_rewrite_terms (PARSER_CONTEXT * parser, PT_NODE * nodes, PT_NODE ** terms)
{
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

  const UINTPTR SPEC_O = 1, SPEC_I = 2, SPEC_S = 3;	/* o outer, i inner, s only in the subquery (not joined) */

  struct stmt
  {
    PARSER_CONTEXT *parser;

      stmt ()
    {
      parser = parser_create_parser ();
    }
     ~stmt ()
    {
      parser_free_parser (parser);
    }

    PT_NODE *col (const char *name, UINTPTR spec)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.resolved = spec == SPEC_O ? "o" : (spec == SPEC_I ? "i" : "s");
      n->info.name.spec_id = spec;
      n->info.name.meta_class = PT_NORMAL;
      n->type_enum = PT_TYPE_INTEGER;
      return n;
    }

    PT_NODE *ival (int x)
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_INTEGER;
      v->info.value.data_value.i = x;
      return v;
    }

    PT_NODE *expr (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->type_enum = PT_TYPE_LOGICAL;
      return e;
    }

    PT_NODE *cmp (PT_OP_TYPE op, const char *c, UINTPTR spec, int v)
    {
      return expr (op, col (c, spec), ival (v));
    }

    /* `i.x in (select x from s)`: the subquery is a PT_SELECT operand (s is not joined) */
    PT_NODE *in_subquery ()
    {
      PT_NODE *q = parser_new_node (parser, PT_SELECT);
      q->info.query.q.select.list = col ("x", SPEC_S);
      return expr (PT_IS_IN, col ("x", SPEC_I), q);
    }

    PT_NODE *spec (UINTPTR id)
    {
      PT_NODE *s = parser_new_node (parser, PT_SPEC);
      s->info.spec.id = id;
      return s;
    }

    /* the WHERE as pt_cnf () leaves it: o.k = i.k, then the factor kept whole (CNF_DONE) */
    PT_NODE *where (PT_NODE * factor)
    {
      PT_NODE *join = expr (PT_EQ, col ("k", SPEC_O), col ("k", SPEC_I));
      PT_EXPR_INFO_SET_FLAG (join, PT_EXPR_INFO_CNF_DONE);
      PT_EXPR_INFO_SET_FLAG (factor, PT_EXPR_INFO_CNF_DONE);
      factor->info.expr.paren_type = 1;
      join->next = factor;
      return join;
    }

    PT_NODE *specs ()
    {
      PT_NODE *o = spec (SPEC_O);
      o->next = spec (SPEC_I);
      return o;
    }
  };

  /* every column below e (operands and or_next) belongs to `spec` */
  bool only_spec (PT_NODE * e, UINTPTR spec)
  {
    for (PT_NODE * d = e; d != NULL; d = d->or_next)
      {
	if (d->node_type == PT_NAME)
	  {
	    if (d->info.name.spec_id != spec)
	      {
		return false;
	      }
	    continue;
	  }
	if (d->node_type == PT_EXPR)
	  {
	    if ((d->info.expr.arg1 && !only_spec (d->info.expr.arg1, spec))
		|| (d->info.expr.arg2 && !only_spec (d->info.expr.arg2, spec)))
	      {
		return false;
	      }
	  }
      }
    return true;
  }

  /* a term qo_extract_or_restrictions () appended (PT_EXPR_INFO_OR_DERIVED) over the spec alone */
  int derived_on (PT_NODE * where, UINTPTR spec)
  {
    int n = 0;
    for (PT_NODE * t = where; t != NULL; t = t->next)
      {
	if (t->node_type == PT_EXPR && PT_EXPR_INFO_IS_FLAGED (t, PT_EXPR_INFO_OR_DERIVED) && only_spec (t, spec))
	  {
	    n++;
	  }
      }
    return n;
  }

  int run (bool subquery_first)
  {
    stmt s;
    PT_NODE *b1 = subquery_first ? s.expr (PT_AND, s.in_subquery (), s.cmp (PT_EQ, "c", SPEC_O, 1))
      : s.expr (PT_AND, s.cmp (PT_EQ, "c", SPEC_O, 1), s.in_subquery ());
    PT_NODE *b2 = s.expr (PT_AND, s.cmp (PT_EQ, "c", SPEC_O, 2), s.cmp (PT_EQ, "y", SPEC_I, 3));
    PT_NODE *w = s.where (s.expr (PT_OR, b1, b2));
    qo_extract_or_restrictions (s.parser, s.specs (), &w);
    return derived_on (w, SPEC_O);
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("G1 a multi-spec factor without unsafe pieces derives a restriction on each spec", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  /* o.k = i.k and ((o.c = 1 and i.x = 7) or (o.c = 2 and i.y = 3)) */
  PT_NODE *b1 = s.expr (PT_AND, s.cmp (PT_EQ, "c", SPEC_O, 1), s.cmp (PT_EQ, "x", SPEC_I, 7));
  PT_NODE *b2 = s.expr (PT_AND, s.cmp (PT_EQ, "c", SPEC_O, 2), s.cmp (PT_EQ, "y", SPEC_I, 3));
  PT_NODE *w = s.where (s.expr (PT_OR, b1, b2));
  qo_extract_or_restrictions (s.parser, s.specs (), &w);
  CHECK (derived_on (w, SPEC_O) == 1);
  CHECK (derived_on (w, SPEC_I) == 1);
}

TEST_CASE ("G2 with a subquery after the column of o in its branch, the restriction on o is derived", "[contract]")
{
  REQUIRE (boot ());
  /* o.k = i.k and ((o.c = 1 and i.x in (select x from s)) or (o.c = 2 and i.y = 3)) */
  CHECK (run (false) == 1);
}

/* ---------------------------------------------------------------- defects (PASS = the defect is there) */

TEST_CASE ("D1 G4-04 the restriction on o depends on the conjunct order: the gate stops at the subquery", "[defect]")
{
  REQUIRE (boot ());
  int column_first = run (false);
  int subquery_first = run (true);
  INFO ("restrictions on o: `o.c = 1 and i.x in (...)` " << column_first << ", `i.x in (...) and o.c = 1` "
	<< subquery_first);
  CHECK (column_first != subquery_first);
}
