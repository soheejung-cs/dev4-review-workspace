/* unitseam appendix for unit/optimizer/query_rewrite_term.null_eval -- appended to the end of
 * src/optimizer/rewriter/query_rewrite_term.c, so it reaches the static qo_null_eval () and the QO_NULL_EVAL_* macros. */

int
unit_null_eval (PARSER_CONTEXT * parser, PT_NODE * node, UINTPTR spec_id)
{
  QO_NULL_TARGET target;

  target.spec_id = spec_id;
  target.name = NULL;
  return qo_null_eval (parser, node, &target);
}

int
unit_null_eval_expr (PARSER_CONTEXT * parser, PT_NODE * expr, UINTPTR spec_id)
{
  QO_NULL_TARGET target;

  target.spec_id = spec_id;
  target.name = NULL;
  return qo_null_eval_expr (parser, expr, &target);
}

/* the path of this TU copy in the worktree of the revision under test: src/optimizer/rewriter/<copy>, from which the
 * test reads src/parser/parse_tree.h of the same revision */
const char *
unit_null_eval_tu_path (void)
{
  return __FILE__;
}

int unit_null_eval_true = QO_NULL_EVAL_TRUE;
int unit_null_eval_false = QO_NULL_EVAL_FALSE;
int unit_null_eval_unknown = QO_NULL_EVAL_UNKNOWN;
