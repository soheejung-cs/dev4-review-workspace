/*
 * PR#8666 (CBRD-28078) -- qo_converse_sarg_terms () of src/optimizer/rewriter/query_rewrite_term.c (CS_MODE), the loop
 * that takes the unary minus signs off a comparison ("filter out unary minus nodes"): `-x op -y` becomes `x op' y`,
 * op' = pt_converse_op (op), and the loop writes op' over the term (dnf_node->info.expr.op = op_type).
 *
 * pt_converse_op () gives 0 for an operator without a converse, such as the null-safe <=> (PT_NULLSAFE_EQ). The two
 * other cases of the loop (`-attr op const`, `const op -attr`) required op' != 0; the case with a minus on both sides did
 * not, so `(- a) <=> (- b)` lost its minus signs and its operator became 0 -- "unknown opcode operator is not defined",
 * and a debug client aborted in pt_show_binopcode (). 2f0601515 requires op' != 0 there too: the term keeps its minus
 * signs.
 *
 *   C1 [contract] (- a) <=> (- b): the operator stays <=>, both minus signs stay (base: the operator becomes 0)
 *   C2 [contract] - (- a) <=> - (- b): unchanged (base: both levels taken off, the operator 0)
 *   C3 [contract] (- a) < (- b) becomes a > b, and (- a) = (- b) becomes a = b -- develop's rewrite, kept
 *   C4 [contract] (- a) <=> 1 and 1 <=> (- a): unchanged, as in develop (those cases already required op' != 0)
 *
 * The test builds the WHERE terms by hand (no lexer) and calls qo_converse_sarg_terms () through the appendix
 * (hooks.cpp). Real code: query_rewrite_term.c (the TU), parse_tree.c + parse_tree_cl.c (pt_converse_op), parser_support.c,
 * language_support.c + intl_support.c + chartype.c, object_domain.c + object_primitive.c + domain_rules.c, db_macro.c,
 * memory_alloc.c, system_parameter.c -- the scaffolding is query_rewrite_term.or_extract_gate's.
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

/* type_checking.c:20897 pt_converse_op () -- the body as is (the operator for the term with its operands swapped; 0:
 * none, as for <=>) */
PT_OP_TYPE
pt_converse_op (PT_OP_TYPE op)
{
  switch (op)
    {
    case PT_EQ:
      return PT_EQ;
    case PT_LT:
      return PT_GT;
    case PT_LE:
      return PT_GE;
    case PT_GT:
      return PT_LT;
    case PT_GE:
      return PT_LE;
    case PT_NE:
      return PT_NE;
    default:
      return (PT_OP_TYPE) 0;
    }
}

/* semantic_check.c pt_check_path_eq () -- its PT_NAME case (spec_id, original and resolved names equal: 0); the terms
 * here compare only names (STEP 1 counts a column's occurrences), anything else is "different" (1) */
int
pt_check_path_eq (PARSER_CONTEXT * parser, const PT_NODE * p, const PT_NODE * q)
{
  (void) parser;
  if (p == NULL && q == NULL)
    {
      return 0;
    }
  if (p == NULL || q == NULL || p->node_type != PT_NAME || q->node_type != PT_NAME)
    {
      return 1;
    }
  if (p->info.name.spec_id != q->info.name.spec_id)
    {
      return 1;
    }
  return (strcmp (p->info.name.original, q->info.name.original) != 0
	  || strcmp (p->info.name.resolved, q->info.name.resolved) != 0) ? 1 : 0;
}

/* hooks.cpp, appended to the TU */
void unitseam_converse_sarg_terms (PARSER_CONTEXT * parser, PT_NODE * where);

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

  const UINTPTR SPEC_T = 1;

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

    PT_NODE *col (const char *name)
    {
      PT_NODE *n = parser_new_node (parser, PT_NAME);
      n->info.name.original = pt_append_string (parser, NULL, name);
      n->info.name.resolved = "t";
      n->info.name.spec_id = SPEC_T;
      n->info.name.meta_class = PT_NORMAL;
      n->type_enum = PT_TYPE_DOUBLE;
      return n;
    }

    PT_NODE *ival (int x)
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->type_enum = PT_TYPE_INTEGER;
      v->info.value.data_value.i = x;
      return v;
    }

    PT_NODE *neg (PT_NODE * x)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = PT_UNARY_MINUS;
      e->info.expr.arg1 = x;
      e->type_enum = x->type_enum;
      return e;
    }

    PT_NODE *cmp (PT_OP_TYPE op, PT_NODE * a1, PT_NODE * a2)
    {
      PT_NODE *e = parser_new_node (parser, PT_EXPR);
      e->info.expr.op = op;
      e->info.expr.arg1 = a1;
      e->info.expr.arg2 = a2;
      e->type_enum = PT_TYPE_LOGICAL;
      return e;
    }
  };

  bool is_col (PT_NODE * n, const char *name)
  {
    return n != NULL && n->node_type == PT_NAME && strcmp (n->info.name.original, name) == 0;
  }

  bool is_neg_of (PT_NODE * n, const char *name)
  {
    return n != NULL && n->node_type == PT_EXPR && n->info.expr.op == PT_UNARY_MINUS && is_col (n->info.expr.arg1, name);
  }

  bool is_int (PT_NODE * n, int x)
  {
    return n != NULL && n->node_type == PT_VALUE && n->info.value.data_value.i == x;
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("C1 (- a) <=> (- b) keeps its operator and both minus signs (base: the minus signs are taken off and the operator becomes 0)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *t = s.cmp (PT_NULLSAFE_EQ, s.neg (s.col ("a")), s.neg (s.col ("b")));
  unitseam_converse_sarg_terms (s.parser, t);

  CHECK (t->info.expr.op == PT_NULLSAFE_EQ);
  CHECK (is_neg_of (t->info.expr.arg1, "a"));
  CHECK (is_neg_of (t->info.expr.arg2, "b"));
}

TEST_CASE ("C2 - (- a) <=> - (- b) is left as it is (base: both levels taken off, the operator 0)", "[contract]")
{
  REQUIRE (boot ());
  stmt s;
  PT_NODE *t = s.cmp (PT_NULLSAFE_EQ, s.neg (s.neg (s.col ("a"))), s.neg (s.neg (s.col ("b"))));
  unitseam_converse_sarg_terms (s.parser, t);

  CHECK (t->info.expr.op == PT_NULLSAFE_EQ);
  PT_NODE *a1 = t->info.expr.arg1, *a2 = t->info.expr.arg2;
  REQUIRE (a1 != NULL);
  REQUIRE (a2 != NULL);
  /* base: both levels are gone and a1, a2 are the columns -- stop before reading them as expressions */
  REQUIRE (a1->node_type == PT_EXPR);
  REQUIRE (a2->node_type == PT_EXPR);
  CHECK (a1->info.expr.op == PT_UNARY_MINUS);
  CHECK (is_neg_of (a1->info.expr.arg1, "a"));
  CHECK (a2->info.expr.op == PT_UNARY_MINUS);
  CHECK (is_neg_of (a2->info.expr.arg1, "b"));
}

TEST_CASE ("C3 (- a) < (- b) becomes a > b and (- a) = (- b) becomes a = b (develop's rewrite, kept)", "[contract]")
{
  REQUIRE (boot ());
  {
    stmt s;
    PT_NODE *t = s.cmp (PT_LT, s.neg (s.col ("a")), s.neg (s.col ("b")));
    unitseam_converse_sarg_terms (s.parser, t);
    /* a > b, or the same written b < a */
    bool gt = t->info.expr.op == PT_GT && is_col (t->info.expr.arg1, "a") && is_col (t->info.expr.arg2, "b");
    bool lt = t->info.expr.op == PT_LT && is_col (t->info.expr.arg1, "b") && is_col (t->info.expr.arg2, "a");
    CHECK ((gt || lt));
  }
  {
    stmt s;
    PT_NODE *t = s.cmp (PT_EQ, s.neg (s.col ("a")), s.neg (s.col ("b")));
    unitseam_converse_sarg_terms (s.parser, t);
    CHECK (t->info.expr.op == PT_EQ);
    bool ab = is_col (t->info.expr.arg1, "a") && is_col (t->info.expr.arg2, "b");
    bool ba = is_col (t->info.expr.arg1, "b") && is_col (t->info.expr.arg2, "a");
    CHECK ((ab || ba));
  }
}

TEST_CASE ("C4 (- a) <=> 1 and 1 <=> (- a) are left as they are (those cases already required a converse operator)", "[contract]")
{
  REQUIRE (boot ());
  {
    stmt s;
    PT_NODE *t = s.cmp (PT_NULLSAFE_EQ, s.neg (s.col ("a")), s.ival (1));
    unitseam_converse_sarg_terms (s.parser, t);
    CHECK (t->info.expr.op == PT_NULLSAFE_EQ);
    CHECK (is_neg_of (t->info.expr.arg1, "a"));
    CHECK (is_int (t->info.expr.arg2, 1));
  }
  {
    stmt s;
    PT_NODE *t = s.cmp (PT_NULLSAFE_EQ, s.ival (1), s.neg (s.col ("a")));
    unitseam_converse_sarg_terms (s.parser, t);
    CHECK (t->info.expr.op == PT_NULLSAFE_EQ);
    CHECK (is_int (t->info.expr.arg1, 1));
    CHECK (is_neg_of (t->info.expr.arg2, "a"));
  }
}
