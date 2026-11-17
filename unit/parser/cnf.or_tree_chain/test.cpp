/*
 * PR#8517 (CBRD-28076) review G3 -- pt_cnf_where () of src/parser/cnf.c (CS_MODE) after c8e8d4f7f
 * (pt_cnf_list_has_or_tree ()): an OR whose operand already holds a factor kept whole (a PT_OR tree) is kept whole too,
 * unless the OR holds a (+) predicate.
 *
 * Shape checked: a PT_OR (or PT_AND) node that carries an or_next.  pt_fold_const_expr () (type_checking.c) saves only
 * expr->next; when an AND/OR node folds into one of its operands (one operand a value) it returns a copy of that
 * operand and frees the node with its or_next -- the disjuncts chained after the tree are lost (the commit message of
 * c8e8d4f7f: "the constant folding replaced the tree node and dropped its or_next").
 *
 * [contract] head: no such node for the shapes the fix targets (Case 20 and its variants).
 * [defect]   (PASS = the shape is there) the (+) carve-out: the OR is distributed although an operand holds a kept
 *            tree, so the tree heads (or sits inside) an or_next chain.
 *
 * Real code: cnf.c (TU), parse_tree.c + parse_tree_cl.c, parser_support.c, language/intl/chartype, object_domain/
 * primitive, domain_rules, db_macro, memory_alloc, system_parameter (as cnf.keep_or_whole).
 * Fakes: those of cnf.keep_or_whole, plus pt_negate_op = type_checking.c:21001 body (for NOT).
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
#include <string>

/* ---------------------------------------------------------------- fakes (as cnf.keep_or_whole) */

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

/* semantic_check.c pt_count_input_markers () -- the body as is */
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

bool
pt_is_keyword (const char *s)
{
  return false;
}

/* name_resolution.c pt_str_compare () -- the body as is */
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

/* type_checking.c:21001 pt_negate_op () -- the body as is */
PT_OP_TYPE
pt_negate_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
      return PT_NE;
    case PT_NE:
      return PT_EQ;
    case PT_SETEQ:
      return PT_SETNEQ;
    case PT_SETNEQ:
      return PT_SETEQ;
    case PT_GT:
      return PT_LE;
    case PT_GE:
      return PT_LT;
    case PT_LT:
      return PT_GE;
    case PT_LE:
      return PT_GT;
    case PT_BETWEEN:
      return PT_NOT_BETWEEN;
    case PT_NOT_BETWEEN:
      return PT_BETWEEN;
    case PT_IS_IN:
      return PT_IS_NOT_IN;
    case PT_IS_NOT_IN:
      return PT_IS_IN;
    case PT_LIKE:
      return PT_NOT_LIKE;
    case PT_NOT_LIKE:
      return PT_LIKE;
    case PT_RLIKE:
      return PT_NOT_RLIKE;
    case PT_NOT_RLIKE:
      return PT_RLIKE;
    case PT_RLIKE_BINARY:
      return PT_NOT_RLIKE_BINARY;
    case PT_NOT_RLIKE_BINARY:
      return PT_RLIKE_BINARY;
    case PT_IS_NULL:
      return PT_IS_NOT_NULL;
    case PT_IS_NOT_NULL:
      return PT_IS_NULL;
    case PT_EQ_SOME:
      return PT_NE_ALL;
    case PT_NE_SOME:
      return PT_EQ_ALL;
    case PT_GT_SOME:
      return PT_LE_ALL;
    case PT_GE_SOME:
      return PT_LT_ALL;
    case PT_LT_SOME:
      return PT_GE_ALL;
    case PT_LE_SOME:
      return PT_GT_ALL;
    case PT_EQ_ALL:
      return PT_NE_SOME;
    case PT_NE_ALL:
      return PT_EQ_SOME;
    case PT_GT_ALL:
      return PT_LE_SOME;
    case PT_GE_ALL:
      return PT_LT_SOME;
    case PT_LT_ALL:
      return PT_GE_SOME;
    case PT_LE_ALL:
      return PT_GT_SOME;
    case PT_IS:
      return PT_IS_NOT;
    case PT_IS_NOT:
      return PT_IS;
    default:
      return (PT_OP_TYPE) 0;
    }
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

  struct cond
  {
    PARSER_CONTEXT *parser;

      cond ()
    {
      parser = parser_create_parser ();
    }
     ~cond ()
    {
      parser_free_parser (parser);
    }

    /* spec 1 = o, spec 2 = i */
    PT_NODE *col (const char *name, UINTPTR spec = 1)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.resolved = spec == 1 ? "o" : "i";
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

    PT_NODE *expr (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL, PT_NODE * a3 = NULL)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->info.expr.arg3 = a3;
      e->type_enum = PT_TYPE_LOGICAL;
      return e;
    }

    PT_NODE *cmp (PT_OP_TYPE op, const char *c, int v, UINTPTR spec = 1)
    {
      return expr (op, col (c, spec), ival (v));
    }

    /* a predicate over constants only -- reads no column (the Case 20 constants, cast (5 as varchar) <> '6', are
     * no column either; what folds them later does not matter to pt_cnf) */
    PT_NODE *konst (PT_OP_TYPE op, int a, int b)
    {
      return expr (op, ival (a), ival (b));
    }

    PT_NODE *knull (int a)
    {
      return expr (PT_IS_NULL, ival (a));
    }

    /* a (+) predicate: csql_grammar.y:17887-17891 flags the comparison */
    PT_NODE *outer (PT_NODE * e, bool right = true)
    {
      PT_EXPR_INFO_SET_FLAG (e, right ? PT_EXPR_INFO_RIGHT_OUTER : PT_EXPR_INFO_LEFT_OUTER);
      return e;
    }

    PT_NODE *and_ (PT_NODE * a, PT_NODE * b)
    {
      return expr (PT_AND, a, b);
    }

    PT_NODE *or_ (PT_NODE * a, PT_NODE * b)
    {
      return expr (PT_OR, a, b);
    }

    PT_NODE *not_ (PT_NODE * a)
    {
      return expr (PT_NOT, a);
    }

    PT_NODE *where (PT_NODE * c)
    {
      return pt_cnf_where (parser, c);
    }

    /* ((5 <> 6 and 5 = 6) or 7 is null) -- the inner factor of Case 20, kept whole (no column) */
    PT_NODE *case20_inner ()
    {
      return or_ (and_ (konst (PT_NE, 5, 6), konst (PT_EQ, 5, 6)), knull (7));
    }

    /* (o.a = 1 and o.b = 2) or (o.a = 3 and o.b = 4) -- kept whole (two columns) */
    PT_NODE *two_col_inner ()
    {
      return or_ (and_ (cmp (PT_EQ, "a", 1), cmp (PT_EQ, "b", 2)), and_ (cmp (PT_EQ, "a", 3), cmp (PT_EQ, "b", 4)));
    }
  };

  bool is_bool_tree (const PT_NODE * e)
  {
    return e != NULL && e->node_type == PT_EXPR && (e->info.expr.op == PT_OR || e->info.expr.op == PT_AND);
  }

  /* AND/OR nodes that carry an or_next: on a chain (any position), or anywhere below a chain element */
  int trees_with_or_next_below (const PT_NODE * e)
  {
    if (e == NULL || e->node_type != PT_EXPR)
      {
	return 0;
      }
    int n = 0;
    const PT_NODE *ops[] = { e->info.expr.arg1, e->info.expr.arg2 };
    for (const PT_NODE * o:ops)
      {
	for (const PT_NODE * d = o; d != NULL; d = d->or_next)
	  {
	    if (is_bool_tree (d) && d->or_next != NULL)
	      {
		n++;
	      }
	    n += trees_with_or_next_below (d);
	  }
      }
    return n;
  }

  int trees_with_or_next (const PT_NODE * list)
  {
    int n = 0;
    for (const PT_NODE * c = list; c != NULL; c = c->next)
      {
	for (const PT_NODE * d = c; d != NULL; d = d->or_next)
	  {
	    if (is_bool_tree (d) && d->or_next != NULL)
	      {
		n++;
	      }
	    n += trees_with_or_next_below (d);
	  }
      }
    return n;
  }

  bool flagged (const PT_NODE * e)
  {
    return e != NULL && e->node_type == PT_EXPR
      && PT_EXPR_INFO_IS_FLAGED (e, PT_EXPR_INFO_LEFT_OUTER | PT_EXPR_INFO_RIGHT_OUTER);
  }

  /* a CNF term headed by a tree whose or_next chain holds a (+) predicate: pt_bind_names () leaves it in WHERE
   * (name_resolution.c:2484-2495 reads the head only), and the fold that replaces the head drops the (+) disjunct */
  int tree_headed_terms_with_outer (const PT_NODE * list)
  {
    int n = 0;
    for (const PT_NODE * c = list; c != NULL; c = c->next)
      {
	if (!is_bool_tree (c) || c->or_next == NULL)
	  {
	    continue;
	  }
	for (const PT_NODE * d = c->or_next; d != NULL; d = d->or_next)
	  {
	    if (flagged (d))
	      {
		n++;
		break;
	      }
	  }
      }
    return n;
  }

  int list_len (const PT_NODE * l)
  {
    int n = 0;
    for (; l != NULL; l = l->next)
      {
	n++;
      }
    return n;
  }

  /* pt_print of the CNF list as next / or_next, for INFO */
  std::string shape (const PT_NODE * e)
  {
    if (e == NULL)
      {
	return "null";
      }
    if (e->node_type == PT_VALUE)
      {
	return std::to_string (e->info.value.data_value.i);
      }
    if (e->node_type == PT_NAME)
      {
	return std::string (e->info.name.resolved) + "." + e->info.name.original;
      }
    if (e->node_type != PT_EXPR)
      {
	return "?";
      }
    std::string s;
    switch (e->info.expr.op)
      {
      case PT_AND:
	s = "AND(" + shape (e->info.expr.arg1) + ", " + shape (e->info.expr.arg2) + ")";
	break;
      case PT_OR:
	s = "OR(" + shape (e->info.expr.arg1) + ", " + shape (e->info.expr.arg2) + ")";
	break;
      case PT_EQ:
	s = shape (e->info.expr.arg1) + "=" + shape (e->info.expr.arg2);
	break;
      case PT_NE:
	s = shape (e->info.expr.arg1) + "<>" + shape (e->info.expr.arg2);
	break;
      case PT_GT:
	s = shape (e->info.expr.arg1) + ">" + shape (e->info.expr.arg2);
	break;
      case PT_LT:
	s = shape (e->info.expr.arg1) + "<" + shape (e->info.expr.arg2);
	break;
      case PT_LE:
	s = shape (e->info.expr.arg1) + "<=" + shape (e->info.expr.arg2);
	break;
      case PT_GE:
	s = shape (e->info.expr.arg1) + ">=" + shape (e->info.expr.arg2);
	break;
      case PT_IS_NULL:
	s = shape (e->info.expr.arg1) + " is null";
	break;
      case PT_IS_NOT_NULL:
	s = shape (e->info.expr.arg1) + " is not null";
	break;
      default:
	s = "op" + std::to_string ((int) e->info.expr.op);
	break;
      }
    if (flagged (e))
      {
	s += "(+)";
      }
    if (e->info.expr.arg1 && e->info.expr.arg1->node_type == PT_EXPR && e->info.expr.arg1->or_next)
      {
	s += "[arg1 has or_next]";
      }
    return s;
  }

  std::string list_shape (const PT_NODE * l)
  {
    std::string s;
    for (const PT_NODE * c = l; c != NULL; c = c->next)
      {
	if (!s.empty ())
	  {
	    s += "  &  ";
	  }
	for (const PT_NODE * d = c; d != NULL; d = d->or_next)
	  {
	    if (d != c)
	      {
		s += " |or_next| ";
	      }
	    s += shape (d);
	  }
      }
    return s;
  }
}

/* ---------------------------------------------------------------- contracts: the shapes c8e8d4f7f targets */

TEST_CASE ("C1 Case 20: ((A and B) or C) or o.c0 > 0 with constant A, B, C keeps no tree with an or_next",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.case20_inner (), s.cmp (PT_GT, "c0", 0)));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C2 four disjuncts: ((A and B) or C) or D or o.c0 > 0 keeps no tree with an or_next", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.or_ (s.case20_inner (), s.knull (8)), s.cmp (PT_GT, "c0", 0)));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C3 nested under AND: (((A and B) or C) and o.c0 < 5) or o.c0 > 10 keeps no tree with an or_next",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.and_ (s.case20_inner (), s.cmp (PT_LT, "c0", 5)), s.cmp (PT_GT, "c0", 10)));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C4 column first: o.c0 > 0 or (A and B) or C keeps no tree with an or_next", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.or_ (s.cmp (PT_GT, "c0", 0), s.and_ (s.konst (PT_NE, 5, 6), s.konst (PT_EQ, 5, 6))),
			       s.knull (7)));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C5 NOT over Case 20 keeps no tree with an or_next", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.not_ (s.or_ (s.case20_inner (), s.cmp (PT_GT, "c0", 0))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C6 the tree in the middle: (o.c0 = 1 or <kept> ) or o.c0 = 5 keeps no tree with an or_next",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.or_ (s.cmp (PT_EQ, "c0", 1), s.case20_inner ()), s.cmp (PT_EQ, "c0", 5)));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C7 control without (+): o.a = 1 and ((two-column kept) or i.c > 0) is one PT_OR factor", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.and_ (s.cmp (PT_EQ, "a", 1), s.or_ (s.two_col_inner (), s.cmp (PT_GT, "c", 0, 2))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (list_len (r) == 2);
  CHECK (trees_with_or_next (r) == 0);
}

TEST_CASE ("C8 (+) first: i.c(+) = 1 or <two-column kept> -- the (+) predicate heads the term, the tree is last",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.outer (s.cmp (PT_EQ, "c", 1, 2)), s.two_col_inner ()));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (flagged (r));
  CHECK (trees_with_or_next (r) == 0);
}

/* ---------------------------------------------------------------- defects: the (+) carve-out (PASS = the shape is there) */

TEST_CASE ("PR8517 G3-F1a: o.a = 1 and o.id = i.id(+) and ((two-column kept) or i.c(+) > 0) -- the kept tree heads an "
	   "or_next chain that holds the (+) predicate, a term pt_bind_names () leaves in WHERE", "[defect]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *j = s.outer (s.expr (PT_EQ, s.col ("id", 1), s.col ("id", 2)), false);
  PT_NODE *r = s.where (s.and_ (s.and_ (s.cmp (PT_EQ, "a", 1), j),
				s.or_ (s.two_col_inner (), s.outer (s.cmp (PT_GT, "c", 0, 2)))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) > 0);
  CHECK (tree_headed_terms_with_outer (r) > 0);
}

TEST_CASE ("PR8517 G3-F1b: i.c(+) = 1 or (constant kept) or i.c(+) = 5 -- the kept tree sits inside an or_next chain "
	   "headed by the (+) predicate (a term moved to ON)", "[defect]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.or_ (s.outer (s.cmp (PT_EQ, "c", 1, 2)), s.case20_inner ()),
			       s.outer (s.cmp (PT_EQ, "c", 5, 2))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (flagged (r));
  CHECK (trees_with_or_next (r) > 0);
}

TEST_CASE ("PR8517 G3-F1c: (constant kept) or i.c(+) > 0 -- Case 20 with a (+) disjunct: the carve-out keeps the shape "
	   "c8e8d4f7f removes for c0 > 0", "[defect]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.case20_inner (), s.outer (s.cmp (PT_GT, "c", 0, 2))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) > 0);
}

TEST_CASE ("PR8517 G3-F1d: (two-column kept) or (i.c(+) > 0 and i.d(+) < 9) -- distributed over two clauses, the "
	   "kept tree heads both", "[defect]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.where (s.or_ (s.two_col_inner (),
			       s.and_ (s.outer (s.cmp (PT_GT, "c", 0, 2)), s.outer (s.cmp (PT_LT, "d", 9, 2)))));
  REQUIRE (r != NULL);
  INFO (list_shape (r));
  CHECK (trees_with_or_next (r) >= 2);
}
