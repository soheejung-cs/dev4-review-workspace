/*
 * PR#8375 (CBRD-27855) -- pt_cnf () of src/parser/cnf.c (CS_MODE), the conversion every SELECT/UPDATE/DELETE WHERE,
 * HAVING, START WITH and CONNECT BY goes through (query_rewrite.c:322-354, name_resolution.c:2459).
 *
 * The contract the PR sets (cnf.c:888-897, 655-679, 713-720, 1059-1062):
 *   - an OR one of whose sides is a conjunction (or a factor already kept whole) stays one PT_OR factor -- not
 *     distributed into (A or C) and (A or D) ... -- unless every name in it is one column (pt_cnf_over_one_column)
 *   - inside a factor kept whole, a disjunction is a PT_OR operand (paren_type 1, the location of the chain), never an
 *     or_next chain (pt_cnf_or_list_to_tree) -- qo_extract_or_restrictions () walks arg1/arg2 only
 *   - the common conjuncts of the branches are still taken out, an AND-free `a or b` is still an or_next chain, a factor
 *     over one column is still distributed
 *   - an AND/OR node that already sits in a CNF list (a CASE condition converted again) keeps the rest of that list
 *   - the factor keeps the location of the OR it replaces (an ON condition's)
 *
 * Defects (review G4, PASS = the defect is there):
 *   D1 G4-01  `(x = 1 and y = 1) or a > 1 or ... or a > 450` becomes a left-deep PT_OR 450 levels deep; the predicate
 *             pt_to_pred_expr () makes of it recurses in eval_pred () along its left operands past
 *             max_recursion_sql_depth (400) -> ER_MAX_RECURSION_SQL_DEPTH. develop distributes it into two or_next
 *             chains, which eval_pred () walks iteratively.
 *   D2 G4-02  `o.id = i.id(+) and ((i.y(+) = 10 and i.z(+) = 20) or i.y(+) > 99)`: the kept PT_OR carries no
 *             LEFT/RIGHT_OUTER flag, so pt_bind_names () (name_resolution.c:2479-2491), which moves only flagged
 *             top-level terms to the ON condition, leaves those (+) predicates in WHERE. develop's distributed chains
 *             have flagged heads.
 *
 * The predicate model (pred_of) mirrors pt_to_pred_expr_with_arg () (xasl_generation.c:2212-2290: a CNF list into a
 * right-linear B_AND chain, each or_next chain into a right-linear B_OR chain) and pt_to_pred_expr_local_with_arg ()
 * (xasl_generation.c:1683-1698: PT_AND -> B_AND (arg1, arg2), PT_OR -> B_OR (arg1, arg2), each operand converted as a
 * list); forced_depth is the recursion eval_pred () always makes (query_evaluator.c:2066-2140: it increments the
 * thread's depth on entry, fails when the depth before the increment is over max_recursion_sql_depth, and evaluates
 * the first lhs of a B_AND/B_OR chain recursively before iterating along rhs).
 *
 * Real code: cnf.c (the TU), parse_tree.c + parse_tree_cl.c (parser_create_parser, parser_new_node, parser_walk_tree,
 * parser_copy_tree_list, parser_free_tree, parser_append_node, parser_print_tree), parser_support.c (pt_and,
 * pt_name_equal, pt_datatypes_match), language_support.c + intl_support.c + chartype.c (intl_identifier_casecmp,
 * en_US.utf8), object_domain.c + object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Fakes (suite.json "fakes"): pt_count_input_markers (semantic_check.c), pt_str_compare (name_resolution.c),
 * er_*, pt_parser_line_col, pt_free_dblink_remote_cols, init_builtin_calendar_names, db_date_locale_init,
 * envvar_get / envvar_trim_char, cubregex::get_engine_name.
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
#include <memory>
#include <vector>

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

/* ---------------------------------------------------------------- builders */
namespace
{
  const int UT_MAX_RECURSION_SQL_DEPTH = 400;	/* system_parameter.c:3494, max_recursion_sql_depth default */

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

  /* one search condition, its nodes made as csql_grammar.y and the semantic check leave them */
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

    /* a column of spec `spec` (spec ids as pt_bind_names gives them: one per FROM entry) */
    PT_NODE *col (const char *name, UINTPTR spec = 1)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.resolved = spec == 1 ? "t" : (spec == 2 ? "u" : "v");
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

    /* parser_make_expression () + the logical type the semantic check gives a predicate */
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

    PT_NODE *and_ (PT_NODE * a, PT_NODE * b)
    {
      return expr (PT_AND, a, b);
    }

    PT_NODE *or_ (PT_NODE * a, PT_NODE * b)
    {
      return expr (PT_OR, a, b);
    }

    PT_NODE *cnf (PT_NODE * c)
    {
      return pt_cnf (parser, c);
    }
  };

  int list_len (PT_NODE * l)
  {
    int n = 0;
    for (; l != NULL; l = l->next)
      {
	n++;
      }
    return n;
  }

  int chain_len (PT_NODE * e)
  {
    int n = 0;
    for (; e != NULL; e = e->or_next)
      {
	n++;
      }
    return n;
  }

  bool is_op (PT_NODE * e, PT_OP_TYPE op)
  {
    return e != NULL && e->node_type == PT_EXPR && e->info.expr.op == op;
  }

  /* no node below `e` (its operands, recursively) heads an or_next chain or carries next */
  bool no_lists_inside (PT_NODE * e)
  {
    if (e == NULL || e->node_type != PT_EXPR)
      {
	return true;
      }
    PT_NODE *ops[] = { e->info.expr.arg1, e->info.expr.arg2 };
    for (PT_NODE * o:ops)
      {
	if (o == NULL)
	  {
	    continue;
	  }
	if (o->or_next != NULL || o->next != NULL || !no_lists_inside (o))
	  {
	    return false;
	  }
      }
    return true;
  }

  /* every PT_OR operand inside `e`: paren_type 1, LOGICAL, location `loc` */
  bool inner_ors_marked (PT_NODE * e, short loc, bool top = true)
  {
    if (e == NULL || e->node_type != PT_EXPR)
      {
	return true;
      }
    if (!top && e->info.expr.op == PT_OR
	&& (e->info.expr.paren_type != 1 || e->type_enum != PT_TYPE_LOGICAL || e->info.expr.location != loc))
      {
	return false;
      }
    if (e->info.expr.op != PT_AND && e->info.expr.op != PT_OR)
      {
	return true;
      }
    return inner_ors_marked (e->info.expr.arg1, loc, false) && inner_ors_marked (e->info.expr.arg2, loc, false);
  }

  /* ---- the predicate pt_to_pred_expr () makes, and eval_pred ()'s recursion over it */
  struct P
  {
    char op;			/* 'A' B_AND, 'O' B_OR, 'L' an evaluation term */
    P *lhs, *rhs;
  };

  struct pred_model
  {
    std::vector < std::unique_ptr < P >> pool;

    P *mk (char op, P * l = NULL, P * r = NULL)
    {
      pool.emplace_back (new P { op, l, r });
      return pool.back ().get ();
    }

    /* pt_to_pred_expr_local_with_arg () */
    P *local (PT_NODE * n)
    {
      if (is_op (n, PT_AND))
	{
	  return mk ('A', list (n->info.expr.arg1), list (n->info.expr.arg2));
	}
      if (is_op (n, PT_OR))
	{
	  return mk ('O', list (n->info.expr.arg1), list (n->info.expr.arg2));
	}
      return mk ('L');
    }

    /* pt_to_pred_expr_with_arg (): dnf chains right-linear in order, the CNF list right-linear newest first */
    P *list (PT_NODE * l)
    {
      P *cnf = NULL;
      for (PT_NODE * c = l; c != NULL; c = c->next)
	{
	  std::vector < PT_NODE * >dnf;
	  for (PT_NODE * d = c; d != NULL; d = d->or_next)
	    {
	      dnf.push_back (d);
	    }
	  P *dp = NULL;
	  for (size_t i = dnf.size (); i-- > 0;)
	    {
	      P *t = local (dnf[i]);
	      dp = dp ? mk ('O', t, dp) : t;
	    }
	  cnf = cnf ? mk ('A', dp, cnf) : dp;
	}
      return cnf;
    }
  };

  /* eval_pred () enters the first lhs of every B_AND/B_OR before anything else: the depth it always reaches */
  int forced_depth (const P * p)
  {
    return (p->op == 'L') ? 1 : 1 + forced_depth (p->lhs);
  }

  /* the depth reached when every operand is evaluated (no short-circuit), iterating rhs as eval_pred () does */
  int full_depth (const P * p)
  {
    if (p->op == 'L')
      {
	return 1;
      }
    int m = 0;
    const P *t = p;
    for (; t->op == p->op; t = t->rhs)
      {
	m = std::max (m, full_depth (t->lhs));
      }
    m = std::max (m, full_depth (t));
    return 1 + m;
  }

  /* pt_bind_names () moves a top-level CNF term to ON only when the term itself carries the (+) flag */
  bool has_outer_mark (PT_NODE * e)
  {
    return e != NULL && e->node_type == PT_EXPR
      && PT_EXPR_INFO_IS_FLAGED (e, PT_EXPR_INFO_LEFT_OUTER | PT_EXPR_INFO_RIGHT_OUTER);
  }

  bool contains_outer_mark (PT_NODE * e)
  {
    for (PT_NODE * d = e; d != NULL; d = d->or_next)
      {
	if (has_outer_mark (d))
	  {
	    return true;
	  }
	if (d->node_type == PT_EXPR
	    && (contains_outer_mark (d->info.expr.arg1) || contains_outer_mark (d->info.expr.arg2)))
	  {
	    return true;
	  }
      }
    return false;
  }

  /* a top-level term that holds a (+) predicate below it but is not flagged itself */
  int hidden_outer_terms (PT_NODE * l)
  {
    int n = 0;
    for (PT_NODE * c = l; c != NULL; c = c->next)
      {
	if (!has_outer_mark (c) && contains_outer_mark (c))
	  {
	    n++;
	  }
      }
    return n;
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("K1 an OR of conjunctions over two columns stays one PT_OR factor, no or_next inside", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* (a = 1 and b = 1) or (a > 2 and b > 2) -- the operators differ across the branches so the common-conjunct search
   * never needs to print (it compares printed text only when the shapes match) */
  PT_NODE *r = s.cnf (s.or_ (s.and_ (s.cmp (PT_EQ, "a", 1), s.cmp (PT_EQ, "b", 1)),
			     s.and_ (s.cmp (PT_GT, "a", 2), s.cmp (PT_GT, "b", 2))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  CHECK (is_op (r, PT_OR));
  CHECK (r->or_next == NULL);
  CHECK (is_op (r->info.expr.arg1, PT_AND));
  CHECK (is_op (r->info.expr.arg2, PT_AND));
  CHECK (no_lists_inside (r));
}

TEST_CASE ("K2 an OR of conjunctions over one column is still distributed into or_next chains", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* (a > 1 and a < 5) or (a >= 10 and a <= 20) */
  PT_NODE *r = s.cnf (s.or_ (s.and_ (s.cmp (PT_GT, "a", 1), s.cmp (PT_LT, "a", 5)),
			     s.and_ (s.cmp (PT_GE, "a", 10), s.cmp (PT_LE, "a", 20))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 4);
  for (PT_NODE * c = r; c != NULL; c = c->next)
    {
      CHECK (chain_len (c) == 2);
      CHECK (!is_op (c, PT_OR));
    }
}

TEST_CASE ("K3 the common conjunct of the branches is still taken out", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* (a = 1 and b > 2) or (a = 1 and c < 3) => a = 1 and (b > 2 or c < 3) -- the common list comes first
   * (parser_append_node (list, common_list) appends the rest to it) */
  PT_NODE *r = s.cnf (s.or_ (s.and_ (s.cmp (PT_EQ, "a", 1), s.cmp (PT_GT, "b", 2)),
			     s.and_ (s.cmp (PT_EQ, "a", 1), s.cmp (PT_LT, "c", 3))));
  REQUIRE (r != NULL);
  REQUIRE (list_len (r) == 2);
  int common = 0, chain = 0;
  for (PT_NODE * c = r; c != NULL; c = c->next)
    {
      common += (is_op (c, PT_EQ) && c->or_next == NULL) ? 1 : 0;
      chain += (is_op (c, PT_GT) && chain_len (c) == 2 && is_op (c->or_next, PT_LT)) ? 1 : 0;
    }
  CHECK (common == 1);
  CHECK (chain == 1);
}

TEST_CASE ("K4 an AND-free disjunction is still an or_next chain", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.or_ (s.or_ (s.cmp (PT_GT, "a", 1), s.cmp (PT_LT, "b", 2)), s.cmp (PT_EQ, "c", 3)));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  CHECK (chain_len (r) == 3);
  CHECK (!is_op (r, PT_OR));
}

TEST_CASE ("K5 a disjunction inside a kept factor becomes a PT_OR operand (paren, logical, location)", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* (x = 1 and (a = 1 or a > 5)) or (y = 2 and b > 3), all from an ON condition at location 2 */
  PT_NODE *inner = s.or_ (s.cmp (PT_EQ, "a", 1), s.cmp (PT_GT, "a", 5));
  PT_NODE *top = s.or_ (s.and_ (s.cmp (PT_EQ, "x", 1), inner), s.and_ (s.cmp (PT_EQ, "y", 2), s.cmp (PT_GT, "b", 3)));
  /* pt_mark_location () marks every node of an ON condition */
  std::vector < PT_NODE * >stack { top };
  while (!stack.empty ())
    {
      PT_NODE *n = stack.back ();
      stack.pop_back ();
      if (n->node_type == PT_EXPR)
	{
	  n->info.expr.location = 2;
	  if (n->info.expr.arg1)
	    stack.push_back (n->info.expr.arg1);
	  if (n->info.expr.arg2)
	    stack.push_back (n->info.expr.arg2);
	}
    }
  PT_NODE *r = s.cnf (top);
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  CHECK (is_op (r, PT_OR));
  CHECK (r->info.expr.location == 2);	/* the factor keeps the ON condition's location */
  CHECK (no_lists_inside (r));
  CHECK (inner_ors_marked (r, 2));
}

TEST_CASE ("K6 an OR node already in a CNF list (a CASE condition converted again) keeps the rest of the list",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* case when <kept factor> and z > 9 then 1 else 0 end = 1, the condition as a first pass leaves it:
   * [ (a = 1 and b = 1) or (a > 2 and b > 2) ] -next-> [ z > 9 ] */
  PT_NODE *kept = s.or_ (s.and_ (s.cmp (PT_EQ, "a", 1), s.cmp (PT_EQ, "b", 1)),
			 s.and_ (s.cmp (PT_GT, "a", 2), s.cmp (PT_GT, "b", 2)));
  PT_NODE *rest = s.cmp (PT_GT, "z", 9);
  kept->next = rest;
  PT_NODE *c = s.expr (PT_CASE, s.ival (1), s.ival (0), kept);
  c->type_enum = PT_TYPE_INTEGER;
  PT_NODE *r = s.cnf (s.expr (PT_EQ, c, s.ival (1)));
  REQUIRE (r != NULL);
  REQUIRE (is_op (r, PT_EQ));
  PT_NODE *cond_list = r->info.expr.arg1->info.expr.arg3;
  bool rest_kept = false;
  for (PT_NODE * e = cond_list; e != NULL; e = e->next)
    {
      rest_kept = rest_kept || (is_op (e, PT_GT) && e->info.expr.arg1->node_type == PT_NAME
				&& strcmp (e->info.expr.arg1->info.name.original, "z") == 0);
    }
  CHECK (rest_kept);
}

TEST_CASE ("K7 the control of D1: the conjunction last, the disjunction stays right-linear", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* a > 1 or a > 2 or ... or a > 450 or (x = 1 and y = 1) */
  PT_NODE *e = s.cmp (PT_GT, "a", 1);
  for (int k = 2; k <= 450; k++)
    {
      e = s.or_ (e, s.cmp (PT_GT, "a", k));
    }
  e = s.or_ (e, s.and_ (s.cmp (PT_EQ, "x", 1), s.cmp (PT_EQ, "y", 1)));
  PT_NODE *r = s.cnf (e);
  REQUIRE (r != NULL);
  pred_model m;
  P *p = m.list (r);
  CHECK (forced_depth (p) <= UT_MAX_RECURSION_SQL_DEPTH + 1);
  CHECK (full_depth (p) <= UT_MAX_RECURSION_SQL_DEPTH + 1);
}

TEST_CASE ("K8 a (+) predicate outside any OR stays a flagged top-level term", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *j = s.expr (PT_EQ, s.col ("id", 1), s.col ("id", 2));
  PT_EXPR_INFO_SET_FLAG (j, PT_EXPR_INFO_LEFT_OUTER);
  PT_NODE *y = s.expr (PT_EQ, s.col ("y", 2), s.ival (10));
  PT_EXPR_INFO_SET_FLAG (y, PT_EXPR_INFO_RIGHT_OUTER);
  PT_NODE *r = s.cnf (s.and_ (j, y));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 2);
  CHECK (hidden_outer_terms (r) == 0);
}

/* ---------------------------------------------------------------- defects (PASS = the defect is there) */

TEST_CASE ("D1 G4-01 a conjunction first and 450 single disjuncts after it: eval_pred () recursion past 400",
	   "[defect]")
{
  REQUIRE (boot ());
  cond s;
  /* (x = 1 and y = 1) or a > 1 or a > 2 or ... or a > 450, left-associative as csql_grammar.y:17669 parses it */
  PT_NODE *e = s.and_ (s.cmp (PT_EQ, "x", 1), s.cmp (PT_EQ, "y", 1));
  for (int k = 1; k <= 450; k++)
    {
      e = s.or_ (e, s.cmp (PT_GT, "a", k));
    }
  PT_NODE *r = s.cnf (e);
  REQUIRE (r != NULL);
  pred_model m;
  P *p = m.list (r);
  INFO ("CNF terms " << list_len (r) << ", forced eval_pred depth " << forced_depth (p));
  /* the call that sees depth 401 before its increment fails: the 402nd nested eval_pred () */
  CHECK (forced_depth (p) > UT_MAX_RECURSION_SQL_DEPTH + 1);
}

TEST_CASE ("D2 G4-02 (+) predicates inside a kept OR-of-AND are hidden from the move to ON", "[defect]")
{
  REQUIRE (boot ());
  cond s;
  /* o.id = i.id(+) and ((i.y(+) = 10 and i.z(+) = 20) or i.y(+) > 99) -- spec 1 is o, spec 2 is i */
  PT_NODE *j = s.expr (PT_EQ, s.col ("id", 1), s.col ("id", 2));
  PT_EXPR_INFO_SET_FLAG (j, PT_EXPR_INFO_LEFT_OUTER);
  PT_NODE *y10 = s.cmp (PT_EQ, "y", 10, 2);
  PT_NODE *z20 = s.cmp (PT_EQ, "z", 20, 2);
  PT_NODE *y99 = s.cmp (PT_GT, "y", 99, 2);
  PT_EXPR_INFO_SET_FLAG (y10, PT_EXPR_INFO_RIGHT_OUTER);
  PT_EXPR_INFO_SET_FLAG (z20, PT_EXPR_INFO_RIGHT_OUTER);
  PT_EXPR_INFO_SET_FLAG (y99, PT_EXPR_INFO_RIGHT_OUTER);
  PT_NODE *r = s.cnf (s.and_ (j, s.or_ (s.and_ (y10, z20), y99)));
  REQUIRE (r != NULL);
  INFO ("CNF terms " << list_len (r) << ", hidden (+) terms " << hidden_outer_terms (r));
  CHECK (hidden_outer_terms (r) > 0);
}
