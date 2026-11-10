/* appended to src/parser/type_checking.c (same TU): pt_eval_type () is static there. One node's post-order step of
 * the type checking walk of pt_semantic_type (), as parser_walk_tree () calls it -- the sc_info of a semantic check
 * (folding on) or of name resolution (donot_fold). */
PT_NODE *
ut_eval_type_node (PARSER_CONTEXT * parser, PT_NODE * node, bool donot_fold)
{
  SEMANTIC_CHK_INFO sc_info;
  int continue_walk = PT_CONTINUE_WALK;

  memset (&sc_info, 0, sizeof (sc_info));
  sc_info.top_node = node;
  sc_info.donot_fold = donot_fold;
  return pt_eval_type (parser, node, &sc_info, &continue_walk);
}
