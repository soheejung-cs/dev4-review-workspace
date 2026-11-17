/*
 * PR#8517 (CBRD-28076) review G5 -- pt_to_pred_expr_factor_dnf () of src/parser/xasl_generation.c (CS_MODE) and the
 * helpers it reads a factor with (pt_factor_dnf_node / _list / _and / _top / _top_list / _is_unsafe), reached from
 * pt_to_pred_expr_with_arg () for every CNF term that is a PT_OR without or_next.
 *
 * The contract read off the code (xasl_generation.c:2316-2664, head c8e8d4f7f):
 *   - a factor kept whole, (A and (B or D)) or C, is written as its disjunctive normal form (A and B) or (A and D) or C,
 *     disjuncts and leaves in the order written, right-linear chains as pt_to_pred_expr_with_arg () builds them
 *   - an operand is read as a CNF list: next is AND, or_next is OR (the restrictions qo_extract_or_restrictions ()
 *     derives), and a PT_OR tree operand (pt_cnf_or_list_to_tree ()) is an OR too
 *   - the form is not taken when it has as many disjuncts as the factor is written with (no OR under an AND), past
 *     PT_FACTOR_DNF_MAX_DISJUNCTS (16) disjuncts, or when the factor reads a second spec, a subquery, a method call
 *     or a random number (pt_factor_dnf_is_unsafe ())
 *   - pt_to_pred_expr_with_arg () ANDs the form with the other terms and sets the inst_num () continue flags as for an
 *     OR
 * [defect] G5-F1 (PASS = present): a leaf that can fail, guarded by the disjunct before it in a nested OR, is taken
 *          into the form, in a disjunct without its guard -- the row path then evaluates it where the written factor
 *          did not (SQL: OUT/unit/G5-F1/repro, `Attempt to divide by zero.` on the PR only)
 * [defect] G5-F2 (PASS = present): a form of 33 leaves is taken although the page path takes an OR term of 32 leaves
 *          in all (expr_compile.c:4349)
 *
 * Real code: xasl_generation.c (the TU: pt_to_pred_expr_with_arg, pt_to_pred_expr_factor_dnf and its helpers,
 * pt_make_pred_expr_pred, pt_numbering_set_continue_post), parse_tree.c + parse_tree_cl.c (parser_create_parser,
 * parser_new_node, parser_walk_tree), language_support.c + intl_support.c + chartype.c, object_domain.c +
 * object_primitive.c + domain_rules.c, db_macro.c, memory_alloc.c, system_parameter.c.
 * Seam: pt_to_pred_expr_local_with_arg -- a fake that makes one T_EVAL_TERM per leaf and remembers its node (the real
 * one builds the regu variables of the comparison; the form is about which nodes become leaves and how they chain).
 * Fakes: pt_alloc_packing_buf (calloc, parser_support.c:4578 hands out zeroed packing memory), regu_init (pred_expr)
 * (xasl_regu_alloc.cpp:276: T_NOT_TERM, no program), and the boot fakes of xasl_generation.session_variable_mark.
 */
#include "catch.hpp"
#include "config.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "parser_support.h"
#include "xasl_generation.h"
#include "xasl_predicate.hpp"
#include "xasl_regu_alloc.hpp"
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
#include <map>
#include <vector>

/* ---------------------------------------------------------------- boot fakes (as xasl_generation.session_variable_mark) */

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

/* ---------------------------------------------------------------- XASL fakes */

namespace
{
  std::vector < void *>g_allocs;
  std::map < const PRED_EXPR *, PT_NODE * >g_leaf_of;
  int g_leaf_calls = 0;
  int g_spec_t, g_spec_u;	/* their addresses are the spec ids of t and u */
}

/* parser_support.c:4578: zeroed memory of the packing heap, released with the XASL */
char *
pt_alloc_packing_buf (int size)
{
  void *p;

  if (size <= 0)
    {
      return NULL;
    }
  p = calloc (1, size);
  g_allocs.push_back (p);
  return (char *) p;
}

/* xasl_regu_alloc.cpp:276 */
void
regu_init (cubxasl::pred_expr & pr)
{
  memset (&pr, 0, sizeof (pr));
  pr.type = T_NOT_TERM;
}

/* the seam: one leaf predicate per node, remembered */
PRED_EXPR *
pt_to_pred_expr_local_with_arg (PARSER_CONTEXT * parser, PT_NODE * node, int *argp)
{
  PRED_EXPR *p = (PRED_EXPR *) pt_alloc_packing_buf ((int) sizeof (PRED_EXPR));

  p->type = T_EVAL_TERM;
  g_leaf_of[p] = node;
  g_leaf_calls++;
  return p;
}

/* the appendix (hooks.cpp) */
int ut_factor_dnf (PT_NODE * node, PT_NODE ** leaves, int *lens, int max_leaves);
int ut_factor_dnf_top (PT_NODE * node);
bool ut_factor_dnf_unsafe (PARSER_CONTEXT * parser, PT_NODE * factor);
PRED_EXPR *ut_to_pred_expr_factor_dnf (PARSER_CONTEXT * parser, PT_NODE * factor, int *argp);

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
    return true;
  }

  struct fx
  {
    PARSER_CONTEXT *parser;

      fx ()
    {
      parser = parser_create_parser ();
      g_leaf_of.clear ();
      g_leaf_calls = 0;
    }
     ~fx ()
    {
      parser_free_parser (parser);
      for (void *p:g_allocs)
	{
	  free (p);
	}
      g_allocs.clear ();
    }

    /* a column of t (or of the spec given), resolved */
    PT_NODE *col (const char *name, void *spec = &g_spec_t)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.spec_id = (UINTPTR) spec;
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

    PT_NODE *host ()
    {
      PT_NODE *h = parser_new_node (parser, PT_HOST_VAR);
      h->type_enum = PT_TYPE_INTEGER;
      return h;
    }

    /* parser_make_expression () */
    PT_NODE *expr (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2 = NULL)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->type_enum = (op == PT_DIVIDE || op == PT_DRANDOM) ? PT_TYPE_INTEGER : PT_TYPE_LOGICAL;
      return e;
    }

    PT_NODE *eq (const char *c, int v, void *spec = &g_spec_t)
    {
      return expr (PT_EQ, col (c, spec), ival (v));
    }

    /* a factor pt_cnf_where () keeps whole: a PT_OR over two CNF lists */
    PT_NODE *kept (PT_NODE * l1, PT_NODE * l2)
    {
      PT_NODE *o = expr (PT_OR, l1, l2);
      o->info.expr.paren_type = 1;
      return o;
    }

    /* a disjunction among the operands of a kept factor: a PT_OR tree, right-nested (pt_cnf_or_list_to_tree ()) */
    PT_NODE *ortree (std::vector < PT_NODE * >ds)
    {
      PT_NODE *rest = ds.back ();
      for (int i = (int) ds.size () - 2; i >= 0; i--)
	{
	  rest = kept (ds[i], rest);
	}
      return rest;
    }

    /* a CNF list: next is AND */
    static PT_NODE *all (std::vector < PT_NODE * >ts)
    {
      for (size_t i = 0; i + 1 < ts.size (); i++)
	{
	  ts[i]->next = ts[i + 1];
	}
      return ts[0];
    }

    /* an or_next chain: OR */
    static PT_NODE *any (std::vector < PT_NODE * >ts)
    {
      for (size_t i = 0; i + 1 < ts.size (); i++)
	{
	  ts[i]->or_next = ts[i + 1];
	}
      return ts[0];
    }
  };

  typedef std::vector < std::vector < PT_NODE * >>SHAPE;

  /* the disjuncts of the right-linear OR chain, each the leaves of a right-linear AND chain, as their nodes */
  SHAPE shape (const PRED_EXPR * p)
  {
    SHAPE out;
    auto conj =[&](const PRED_EXPR * c)
    {
      std::vector < PT_NODE * >v;
      while (c->type == T_PRED && c->pe.m_pred.bool_op == B_AND)
	{
	  v.push_back (g_leaf_of[c->pe.m_pred.lhs]);
	  c = c->pe.m_pred.rhs;
	}
      v.push_back (g_leaf_of[c]);
      return v;
    };
    while (p->type == T_PRED && p->pe.m_pred.bool_op == B_OR)
      {
	out.push_back (conj (p->pe.m_pred.lhs));
	p = p->pe.m_pred.rhs;
      }
    out.push_back (conj (p));
    return out;
  }

  int leaves_in (const SHAPE & s)
  {
    int n = 0;
    for (const auto & d:s)
      {
	n += (int) d.size ();
      }
    return n;
  }
}

/* ---------------------------------------------------------------- [contract] */

TEST_CASE ("C1 (A and (B or D)) or C is written as (A and B) or (A and D) or C, in the order written, one disjunct "
	   "more than the two it is written with", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *A = f.eq ("a", 1), *B = f.eq ("b", 1), *D = f.eq ("b", 2), *C = f.eq ("c", 1);
  PT_NODE *factor = f.kept (fx::all ({A, f.ortree ({B, D})}), C);
  PT_NODE *leaves[64];
  int lens[16];

  CHECK (ut_factor_dnf_top (factor) == 2);
  REQUIRE (ut_factor_dnf (factor, leaves, lens, 64) == 3);
  CHECK ((lens[0] == 2 && lens[1] == 2 && lens[2] == 1));
  CHECK ((leaves[0] == A && leaves[1] == B && leaves[2] == A && leaves[3] == D && leaves[4] == C));

  int arg = 0;
  PRED_EXPR *p = ut_to_pred_expr_factor_dnf (f.parser, factor, &arg);
  REQUIRE (p != NULL);
  SHAPE s = shape (p);
  REQUIRE (s.size () == 3);
  CHECK ((s[0] == std::vector < PT_NODE * >{A, B}));
  CHECK ((s[1] == std::vector < PT_NODE * >{A, D}));
  CHECK ((s[2] == std::vector < PT_NODE * >{C}));
  CHECK (g_leaf_calls == 5);
}

TEST_CASE ("C2 a factor with no OR under an AND keeps its form: (A and B) or (C and D)", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *factor = f.kept (fx::all ({f.eq ("a", 1), f.eq ("b", 1)}), fx::all ({f.eq ("a", 2), f.eq ("c", 1)}));
  PT_NODE *leaves[64];
  int lens[16];
  int arg = 0;

  CHECK (ut_factor_dnf (factor, leaves, lens, 64) == 2);
  CHECK (ut_factor_dnf_top (factor) == 2);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, factor, &arg) == NULL);
  CHECK (g_leaf_calls == 0);
}

TEST_CASE ("C3 an or_next chain in an operand is an OR: A next (B or_next D), or C gives the disjuncts of C1",
	   "[contract]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *A = f.eq ("a", 1), *B = f.eq ("b", 1), *D = f.eq ("b", 2), *C = f.eq ("c", 1);
  PT_NODE *factor = f.kept (fx::all ({A, fx::any ({B, D})}), C);
  int arg = 0;

  CHECK (ut_factor_dnf_top (factor) == 2);
  PRED_EXPR *p = ut_to_pred_expr_factor_dnf (f.parser, factor, &arg);
  REQUIRE (p != NULL);
  SHAPE s = shape (p);
  REQUIRE (s.size () == 3);
  CHECK ((s[0] == std::vector < PT_NODE * >{A, B}));
  CHECK ((s[1] == std::vector < PT_NODE * >{A, D}));
  CHECK ((s[2] == std::vector < PT_NODE * >{C}));
}

TEST_CASE ("C4 the bound: a form of 16 disjuncts is taken, one of 17 is not", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  std::vector < PT_NODE * >b15, b16;
  int arg = 0;

  for (int i = 0; i < 15; i++)
    {
      b15.push_back (f.eq ("b", i));
    }
  for (int i = 0; i < 16; i++)
    {
      b16.push_back (f.eq ("b", i));
    }
  PT_NODE *f16 = f.kept (fx::all ({f.eq ("a", 1), f.ortree (b15)}), f.eq ("z", 1));
  PT_NODE *f17 = f.kept (fx::all ({f.eq ("a", 1), f.ortree (b16)}), f.eq ("z", 1));

  PRED_EXPR *p = ut_to_pred_expr_factor_dnf (f.parser, f16, &arg);
  REQUIRE (p != NULL);
  CHECK (shape (p).size () == 16);
  CHECK (leaves_in (shape (p)) == 31);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, f17, &arg) == NULL);
}

TEST_CASE ("C5 a factor reading a second spec, a subquery, a method call or a random number keeps its form; "
	   "host variables do not stop the form", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  int arg = 0;

  /* ((a = 1 and (b = 1 or b = 2)) or <third>) with the third disjunct varied */
  auto factor_with =[&](PT_NODE * third)
  {
    return f.kept (fx::all ({f.eq ("a", 1), f.ortree ({f.eq ("b", 1), f.eq ("b", 2)})}), third);
  };

  PT_NODE *second_spec = factor_with (f.eq ("x", 1, &g_spec_u));
  PT_NODE *q = parser_new_node (f.parser, PT_SELECT);
  PT_NODE *subquery = factor_with (f.expr (PT_EXISTS, q));
  PT_NODE *m = parser_new_node (f.parser, PT_METHOD_CALL);
  PT_NODE *method = factor_with (f.expr (PT_EQ, m, f.ival (1)));
  PT_NODE *random = factor_with (f.expr (PT_LT, f.expr (PT_DRANDOM, NULL), f.ival (0)));
  PT_NODE *hostvar = factor_with (f.expr (PT_EQ, f.col ("c"), f.host ()));

  CHECK (ut_factor_dnf_unsafe (f.parser, second_spec));
  CHECK (ut_factor_dnf_unsafe (f.parser, subquery));
  CHECK (ut_factor_dnf_unsafe (f.parser, method));
  CHECK (ut_factor_dnf_unsafe (f.parser, random));
  CHECK_FALSE (ut_factor_dnf_unsafe (f.parser, hostvar));

  CHECK (ut_to_pred_expr_factor_dnf (f.parser, second_spec, &arg) == NULL);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, random, &arg) == NULL);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, hostvar, &arg) != NULL);
}

TEST_CASE ("C6 a factor not kept whole is not read: a PT_OR with or_next, or no PT_OR", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  int arg = 0;
  PT_NODE *factor = f.kept (fx::all ({f.eq ("a", 1), f.ortree ({f.eq ("b", 1), f.eq ("b", 2)})}), f.eq ("c", 1));

  factor->or_next = f.eq ("d", 1);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, factor, &arg) == NULL);
  CHECK (ut_to_pred_expr_factor_dnf (f.parser, f.eq ("a", 1), &arg) == NULL);
  CHECK (g_leaf_calls == 0);
}

TEST_CASE ("C7 pt_to_pred_expr_with_arg () ANDs the form with the other terms, keeps the list, and sets the "
	   "inst_num () continue flags as for an OR", "[contract]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *X = f.eq ("x", 7);
  PT_NODE *A = f.eq ("a", 1), *B = f.eq ("b", 1), *D = f.eq ("b", 2), *C = f.eq ("c", 1);
  PT_NODE *factor = f.kept (fx::all ({A, f.ortree ({B, D})}), C);
  PT_NODE *list = fx::all ({X, factor});
  int flag = 0;

  PRED_EXPR *p = pt_to_pred_expr_with_arg (f.parser, list, &flag);
  REQUIRE (p != NULL);
  CHECK (list->next == factor);
  CHECK (factor->next == NULL);
  REQUIRE ((p->type == T_PRED && p->pe.m_pred.bool_op == B_AND));
  CHECK (g_leaf_of[p->pe.m_pred.rhs] == X);
  SHAPE s = shape (p->pe.m_pred.lhs);
  REQUIRE (s.size () == 3);
  CHECK ((s[1] == std::vector < PT_NODE * >{A, D}));
  CHECK ((flag & PT_PRED_ARG_INSTNUM_CONTINUE) != 0);
  CHECK ((flag & PT_PRED_ARG_ORDBYNUM_CONTINUE) != 0);
  CHECK (g_leaf_calls == 6);
}

/* ---------------------------------------------------------------- [defect] */

TEST_CASE ("PR8517 G5-F1: ((b = 0 or 10 / b > 1) and a = 1) or a = 3 is taken into the form, and 10 / b > 1 heads a "
	   "disjunct without its guard b = 0 (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *guard = f.eq ("b", 0);
  PT_NODE *fallible = f.expr (PT_GT, f.expr (PT_DIVIDE, f.ival (10), f.col ("b")), f.ival (1));
  PT_NODE *A = f.eq ("a", 1), *C = f.eq ("a", 3);
  PT_NODE *factor = f.kept (fx::all ({f.ortree ({guard, fallible}), A}), C);
  int arg = 0;

  PRED_EXPR *p = ut_to_pred_expr_factor_dnf (f.parser, factor, &arg);
  REQUIRE (p != NULL);
  SHAPE s = shape (p);
  REQUIRE (s.size () == 3);
  CHECK ((s[0] == std::vector < PT_NODE * >{guard, A}));
  CHECK ((s[1] == std::vector < PT_NODE * >{fallible, A}));
}

TEST_CASE ("PR8517 G5-F2: a form of 33 leaves -- 8 disjuncts of 4 and one of 1 -- is taken, past the 32 leaves in all "
	   "the page path takes (PASS = defect present)", "[defect]")
{
  REQUIRE (boot ());
  fx f;
  PT_NODE *factor =
    f.kept (fx::all ({f.eq ("a", 1), f.ortree ({f.eq ("b", 1), f.eq ("c", 1)}), f.ortree ({f.eq ("d", 1),
											  f.eq ("e", 1)}),
		      f.ortree ({f.eq ("f", 1), f.eq ("g", 1)})}), f.eq ("h", 1));
  int arg = 0;

  PRED_EXPR *p = ut_to_pred_expr_factor_dnf (f.parser, factor, &arg);
  REQUIRE (p != NULL);
  CHECK (shape (p).size () == 9);
  CHECK (leaves_in (shape (p)) == 33);
}
