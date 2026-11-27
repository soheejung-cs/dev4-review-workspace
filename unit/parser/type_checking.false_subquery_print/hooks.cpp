/* appended to src/parser/type_checking.c (same TU): pt_to_false_subquery () and pt_eval_type () are static there. */

/* the rewrite itself, as pt_eval_type () calls it once pt_false_where () says the query returns no row */
PT_NODE *
ut_to_false_subquery (PARSER_CONTEXT * parser, PT_NODE * node)
{
  return pt_to_false_subquery (parser, node);
}

/* one node's post-order step of the type checking walk of pt_semantic_type (), as parser_walk_tree () calls it -- the
 * sc_info of a semantic check (folding on): pt_where_type () on the WHERE, then the pt_false_where () gate */
PT_NODE *
ut_eval_type_node (PARSER_CONTEXT * parser, PT_NODE * node)
{
  SEMANTIC_CHK_INFO sc_info;
  int continue_walk = PT_CONTINUE_WALK;

  memset (&sc_info, 0, sizeof (sc_info));
  sc_info.top_node = node;
  sc_info.donot_fold = false;
  return pt_eval_type (parser, node, &sc_info, &continue_walk);
}

/* the constant folding of one expression node, as pt_fold_constants_post () calls it (CBRD-28455) */
PT_NODE *
ut_fold_const_expr (PARSER_CONTEXT * parser, PT_NODE * expr)
{
  return pt_fold_const_expr (parser, expr, NULL);
}
