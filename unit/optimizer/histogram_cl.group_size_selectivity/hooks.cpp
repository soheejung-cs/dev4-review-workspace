/* appended to src/optimizer/histogram/histogram_cl.cpp (CS_MODE): reach the TU statics PR#8325 (CBRD-27811) changed.
 *
 * unitseam_split_hv_predicate   histogram_split_hv_predicate (), same signature at base and head (the head admits a
 *                               SUM / AVG / MIN / MAX operand through histogram_hv_operand_column ()).
 * unitseam_bind_target          bind_target_walk () on one node: the first bind target it adds and its equality flag
 *                               (bind_target_ctx is the same at base and head).
 * unitseam_bind_term_rows       bind_term_walk () on one node without a memo: the rows of the slot it adds
 *                               (bind_term_ctx is the same at base and head). */
bool
unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
			     bool *out_reversed)
{
  return histogram_split_hv_predicate (parser, node, out_name, out_hv, out_reversed);
}

int
unitseam_bind_target (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **name, bool *equality)
{
  bind_target_ctx ctx;
  int continue_walk = PT_CONTINUE_WALK;

  ctx.parser = parser;
  ctx.specs = 0;
  ctx.terms = 0;
  (void) bind_target_walk (parser, node, &ctx, &continue_walk);
  if (ctx.terms > 0)
    {
      *name = ctx.term[0].name;
      *equality = ctx.term[0].equality;
    }
  return ctx.terms;
}

int
unitseam_bind_term_rows (PARSER_CONTEXT *parser, PT_NODE *node, double *rows)
{
  bind_term_ctx ctx;
  int continue_walk = PT_CONTINUE_WALK;

  ctx.parser = parser;
  ctx.want_names = false;
  ctx.memo = NULL;
  ctx.count = 0;
  (void) bind_term_walk (parser, node, &ctx, &continue_walk);
  if (ctx.count > 0)
    {
      *rows = ctx.rows[0];
    }
  return ctx.count;
}
