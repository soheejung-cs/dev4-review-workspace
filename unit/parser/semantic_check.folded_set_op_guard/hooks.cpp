/* appended to src/parser/semantic_check.c (same TU): pt_semantic_check_local () is static there. One node's post-order
 * step of the semantic check walk (parser_walk_tree (parser, node, NULL, NULL, pt_semantic_check_local, sc_info),
 * semantic_check.c:13498), with the sc_info of pt_semantic_check (): top_node = the statement, folding on. */
PT_NODE *
ut_semantic_check_local (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE * top)
{
  SEMANTIC_CHK_INFO sc_info;
  int continue_walk = PT_CONTINUE_WALK;

  memset (&sc_info, 0, sizeof (sc_info));
  sc_info.top_node = top;
  sc_info.donot_fold = false;
  return pt_semantic_check_local (parser, node, &sc_info, &continue_walk);
}
