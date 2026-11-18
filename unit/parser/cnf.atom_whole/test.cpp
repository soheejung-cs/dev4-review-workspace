/*
 * PR#8590 (CBRD-28027) -- pt_transform_cnf_pre () of src/parser/cnf.c (CS_MODE): the CNF walk of pt_cnf () /
 * pt_cnf_where () goes through PT_AND and PT_OR only and stops at an atom of the condition.
 *
 * The contract the PR sets (cnf.c:513-557 at d2f562e58):
 *   - an atom -- a non-PT_EXPR node other than PT_SELECT, or a PT_EXPR whose op is not PT_AND/PT_OR (comparison,
 *     IS [NOT] TRUE/FALSE, CASE, IF, XOR, a function) -- sets PT_LIST_WALK: the walk does not go under it, so an AND
 *     among its operands stays an AND tree and is not turned into a `next` list
 *   - PT_AND and PT_OR reset PT_CONTINUE_WALK: pt_walk_private () (parse_tree_cl.c:1121-1180) restores the
 *     continue_walk an operand's pre left (save_continue) after walking it, so an atom as arg1 leaves PT_LIST_WALK
 *     for arg2; without the reset an AND/OR as arg2 is not walked into and an AND/OR below it is never converted
 *
 * Why it matters (the bug): develop converts the AND inside `(a > 0 and a = 1) is true` into the list
 * `a > 0 -next-> a = 1`; pt_print_expr () prints an operand with pt_print_bytes () (one node, no next), so the atom
 * prints as `(a > 0) is true`.  The common-factor search of an OR (pt_transform_cnf_post, PT_OR) compares printed
 * forms, finds the two IS TRUE atoms of `((a>0 and a=1) is true) or ((a>0 and a=2) is true)` equal and keeps one.
 *
 * Cases ([contract], PASS on head = the contract holds; base results in suite.json origin.result):
 *   C1  IS TRUE atoms keep their AND operand whole, both branches survive (pt_cnf, pt_cnf_where)
 *   C2  boundary: an AND/OR after an atom is still walked into (the PT_CONTINUE_WALK reset); two-level shapes are the
 *       spec, three-level shapes are the ones that detect a missing reset (the inner AND of a two-level operand is
 *       posted even unwalked, since its own operands are atoms)
 *   C3  a CASE / IF / XOR condition inside an atom keeps its AND tree and prints both conjuncts
 *
 * Real code and fakes: as unit/parser/cnf.keep_or_whole (suite.json "fakes").
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
#include <string>

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

    /* the WHERE entry (query_rewrite.c): TRANSFORM_CNF_AND_OR_KEEP */
    PT_NODE *cnf_where (PT_NODE * c)
    {
      return pt_cnf_where (parser, c);
    }

    /* the `true` of `IS TRUE` (csql_grammar.y `boolean`: a LOGICAL value, text "true", i = 1) */
    PT_NODE *true_ ()
    {
      PT_NODE *v = parser_new_node (parser, PT_VALUE);
      v->info.value.text = "true";
      v->info.value.data_value.i = 1;
      v->type_enum = PT_TYPE_LOGICAL;
      return v;
    }

    /* `( <a> and <b> )`: the parenthesized conjunction an operand of an atom is */
    PT_NODE *pand (PT_NODE * a, PT_NODE * b)
    {
      PT_NODE *e = and_ (a, b);
      e->info.expr.paren_type = 1;
      return e;
    }

    /* `<c> is true` (boolean_term_is: parser_make_expression (PT_IS, c, true)) */
    PT_NODE *is_true (PT_NODE * c)
    {
      return expr (PT_IS, c, true_ ());
    }

    /* searched `case when <c> then 1 else 0 end` (searched_when_clause: arg3 = condition, arg1 = then, arg2 = else) */
    PT_NODE *case_ (PT_NODE * c)
    {
      PT_NODE *e = expr (PT_CASE, ival (1), ival (0), c);
      e->info.expr.qualifier = PT_SEARCHED_CASE;
      e->type_enum = PT_TYPE_INTEGER;
      return e;
    }

    /* `if (<c>, 1, 0)` (parser_make_expression (PT_IF, cond, a, b)) */
    PT_NODE *if_ (PT_NODE * c)
    {
      PT_NODE *e = expr (PT_IF, c, ival (1), ival (0));
      e->type_enum = PT_TYPE_INTEGER;
      return e;
    }

    std::string print (PT_NODE * n)
    {
      PT_NODE *save = n->next, *save_or = n->or_next;
      n->next = n->or_next = NULL;
      const char *t = parser_print_tree (parser, n);
      n->next = save;
      n->or_next = save_or;
      return t ? std::string (t) : std::string ();
    }

    /* the form the common factor search of an OR compares (pt_transform_cnf_post: custom_print |= PT_CONVERT_RANGE) */
    std::string print_range (PT_NODE * n)
    {
      unsigned int save = parser->custom_print;
      parser->custom_print |= PT_CONVERT_RANGE;
      std::string t = print (n);
      parser->custom_print = save;
      return t;
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


  /* every PT_EXPR below `e` (operands, recursively): no next / or_next on an operand */
  bool no_lists_inside (PT_NODE * e)
  {
    if (e == NULL || e->node_type != PT_EXPR)
      {
	return true;
      }
    PT_NODE *ops[] = { e->info.expr.arg1, e->info.expr.arg2, e->info.expr.arg3 };
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

  /* every element of the CNF list and of each or_next chain is a plain comparison (no AND/OR left) */
  bool all_atoms_eq (PT_NODE * l)
  {
    for (PT_NODE * c = l; c != NULL; c = c->next)
      {
	for (PT_NODE * d = c; d != NULL; d = d->or_next)
	  {
	    if (!is_op (d, PT_EQ))
	      {
		return false;
	      }
	  }
      }
    return true;
  }

  int value_of (PT_NODE * cmp)
  {
    return cmp->info.expr.arg2->info.value.data_value.i;
  }

  /* `( a > 0 and a = k ) is true`, the AND as the operand and its right conjunct a = k, with no list */
  bool is_true_whole (PT_NODE * d, int k)
  {
    if (!is_op (d, PT_IS))
      {
	return false;
      }
    PT_NODE *a = d->info.expr.arg1;
    return is_op (a, PT_AND) && a->next == NULL && a->or_next == NULL
      && is_op (a->info.expr.arg1, PT_GT) && a->info.expr.arg1->next == NULL
      && is_op (a->info.expr.arg2, PT_EQ) && value_of (a->info.expr.arg2) == k;
  }

  bool has (const std::string & s, const char *sub)
  {
    return s.find (sub) != std::string::npos;
  }
}

/* ---------------------------------------------------------------- C1 IS TRUE atoms */

static void
check_c1 (cond & s, PT_NODE * r)
{
  REQUIRE (r != NULL);
  INFO ("list " << list_len (r) << " chain " << chain_len (r) << " head " << s.print (r));
  CHECK (list_len (r) == 1);
  REQUIRE (chain_len (r) == 2);	/* both branches: one or_next chain of the two atoms */
  CHECK (is_true_whole (r, 11));
  CHECK (is_true_whole (r->or_next, 22));
  /* each prints both of its conjuncts, so the common factor search told them apart.  (On develop the search, at
   * pt_transform_cnf_post time, compared "t.a>0 is true" with "t.a>0 is true": the converted operand list is not yet
   * tagged is_cnf_start, so the printer shows its first conjunct only -- gdb at pt_str_compare, unit/contract
   * summary.  The final tree is tagged and prints the whole list, so this print alone does not tell develop apart.) */
  std::string p1 = s.print_range (r), p2 = s.print_range (r->or_next);
  INFO ("p1 " << p1 << " / p2 " << p2);
  CHECK ((has (p1, "11") && has (p1, " and ")));
  CHECK ((has (p2, "22") && has (p2, " and ")));
  CHECK (p1 != p2);
}

TEST_CASE ("C1a pt_cnf: ((a > 0 and a = 11) is true) or ((a > 0 and a = 22) is true) keeps both atoms whole",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *c = s.or_ (s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 11))),
		      s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 22))));
  check_c1 (s, s.cnf (c));
}

TEST_CASE ("C1b pt_cnf_where: ((a > 0 and a = 11) is true) or ((a > 0 and a = 22) is true) keeps both atoms whole",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *c = s.or_ (s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 11))),
		      s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 22))));
  check_c1 (s, s.cnf_where (c));
}

TEST_CASE ("C1c a lone IS TRUE atom: its AND operand stays a tree (no next list under the atom)", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 11))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  CHECK (chain_len (r) == 1);
  CHECK (is_true_whole (r, 11));
  CHECK (no_lists_inside (r));
}

/* ---------------------------------------------------------------- C2 AND/OR after an atom is still walked */

TEST_CASE ("C2a x = 1 and (y = 1 and z = 1): the inner AND is flattened, 3 conjuncts", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.and_ (s.cmp (PT_EQ, "x", 1), s.pand (s.cmp (PT_EQ, "y", 1), s.cmp (PT_EQ, "z", 1))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 3);
  CHECK (all_atoms_eq (r));
  for (PT_NODE * c = r; c != NULL; c = c->next)
    {
      CHECK (chain_len (c) == 1);
    }
}

TEST_CASE ("C2b x = 1 or (y = 1 and z = 1) via pt_cnf: distributed, 2 conjuncts of 2-chains", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.or_ (s.cmp (PT_EQ, "x", 1), s.pand (s.cmp (PT_EQ, "y", 1), s.cmp (PT_EQ, "z", 1))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 2);
  CHECK (all_atoms_eq (r));
  for (PT_NODE * c = r; c != NULL; c = c->next)
    {
      CHECK (chain_len (c) == 2);
    }
}

TEST_CASE ("C2c x = 1 and (y = 1 and (z = 1 and w = 1)): every AND under the second operand is flattened, 4 conjuncts",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.and_ (s.cmp (PT_EQ, "x", 1),
			      s.pand (s.cmp (PT_EQ, "y", 1), s.pand (s.cmp (PT_EQ, "z", 1), s.cmp (PT_EQ, "w", 1)))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 4);
  CHECK (all_atoms_eq (r));
}

TEST_CASE ("C2d x = 1 or (y = 1 and (z = 1 and w = 1)) via pt_cnf: 3 conjuncts of 2-chains, no AND left", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.or_ (s.cmp (PT_EQ, "x", 1),
			     s.pand (s.cmp (PT_EQ, "y", 1), s.pand (s.cmp (PT_EQ, "z", 1), s.cmp (PT_EQ, "w", 1)))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 3);
  CHECK (all_atoms_eq (r));
  for (PT_NODE * c = r; c != NULL; c = c->next)
    {
      CHECK (chain_len (c) == 2);
    }
}

TEST_CASE ("C2e x = 1 or (y = 1 and z = 1) via pt_cnf_where: one PT_OR factor kept whole (columns differ)",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf_where (s.or_ (s.cmp (PT_EQ, "x", 1), s.pand (s.cmp (PT_EQ, "y", 1), s.cmp (PT_EQ, "z", 1))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  CHECK (chain_len (r) == 1);
  CHECK (is_op (r, PT_OR));
  CHECK (is_op (r->info.expr.arg2, PT_AND));
  CHECK (no_lists_inside (r));
}

TEST_CASE ("C2f ((a > 0 and a = 11) is true) and (x = 1 and (y = 1 and z = 1)): the ANDs after an atom are flattened",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  /* the atom first leaves PT_LIST_WALK for its sibling, the AND after it must still be walked into */
  PT_NODE *r = s.cnf (s.and_ (s.is_true (s.pand (s.cmp (PT_GT, "a", 0), s.cmp (PT_EQ, "a", 11))),
			      s.pand (s.cmp (PT_EQ, "x", 1), s.pand (s.cmp (PT_EQ, "y", 1), s.cmp (PT_EQ, "z", 1)))));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 4);
  CHECK (is_true_whole (r, 11));
  int eqs = 0;
  for (PT_NODE * c = r->next; c != NULL; c = c->next)
    {
      eqs += (is_op (c, PT_EQ) && c->or_next == NULL) ? 1 : 0;
    }
  CHECK (eqs == 3);
}

/* ---------------------------------------------------------------- C3 CASE / IF / XOR conditions */

TEST_CASE ("C3a (case when a > 71 and b = 93 then 1 else 0 end) = 1: the CASE condition stays an AND tree",
	   "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.expr (PT_EQ, s.case_ (s.and_ (s.cmp (PT_GT, "a", 71), s.cmp (PT_EQ, "b", 93))), s.ival (1)));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  REQUIRE (is_op (r, PT_EQ));
  PT_NODE *cs = r->info.expr.arg1;
  REQUIRE (is_op (cs, PT_CASE));
  CHECK (is_op (cs->info.expr.arg3, PT_AND));
  CHECK (cs->info.expr.arg3->next == NULL);
  std::string p = s.print (r);
  INFO (p);
  CHECK (has (p, "71"));
  CHECK (has (p, "93"));
}

TEST_CASE ("C3b if (a > 71 and b = 93, 1, 0) = 1: the IF condition stays an AND tree", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.expr (PT_EQ, s.if_ (s.and_ (s.cmp (PT_GT, "a", 71), s.cmp (PT_EQ, "b", 93))), s.ival (1)));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  REQUIRE (is_op (r, PT_EQ));
  PT_NODE *f = r->info.expr.arg1;
  REQUIRE (is_op (f, PT_IF));
  CHECK (is_op (f->info.expr.arg1, PT_AND));
  CHECK (f->info.expr.arg1->next == NULL);
  std::string p = s.print (r);
  INFO (p);
  CHECK (has (p, "71"));
  CHECK (has (p, "93"));
}

TEST_CASE ("C3c (a > 71 and b = 93) xor c = 5: the XOR operand stays an AND tree", "[contract]")
{
  REQUIRE (boot ());
  cond s;
  PT_NODE *r = s.cnf (s.expr (PT_XOR, s.pand (s.cmp (PT_GT, "a", 71), s.cmp (PT_EQ, "b", 93)), s.cmp (PT_EQ, "c", 5)));
  REQUIRE (r != NULL);
  CHECK (list_len (r) == 1);
  REQUIRE (is_op (r, PT_XOR));
  CHECK (is_op (r->info.expr.arg1, PT_AND));
  CHECK (r->info.expr.arg1->next == NULL);
  std::string p = s.print (r);
  INFO (p);
  CHECK (has (p, "71"));
  CHECK (has (p, "93"));
}
