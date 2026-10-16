/* appended to histogram_cl.cpp: reach the TU's static bind-target code from the test.
 * bind_target_ctx is a TU-local struct, so the test holds it as void *. Terms are filled by the
 * real bind_target_walk, one call per node -- what parser_walk_tree does for each node of the
 * statement -- so equality is decided by the real code (op == PT_EQ, or an all-BETWEEN_EQ_NA range). */
void *
unitseam_bt_new (PARSER_CONTEXT *parser)
{
  bind_target_ctx *ctx = new bind_target_ctx ();

  ctx->parser = parser;
  ctx->specs = 0;
  ctx->terms = 0;
  ctx->overflow = false;
  return ctx;
}

void
unitseam_bt_free (void *ctx)
{
  delete (bind_target_ctx *) ctx;
}

void
unitseam_bt_visit (PARSER_CONTEXT *parser, void *ctx, PT_NODE *node)
{
  int continue_walk = PT_CONTINUE_WALK;

  (void) bind_target_walk (parser, node, ctx, &continue_walk);
}

int
unitseam_bt_terms (void *ctx)
{
  return ((bind_target_ctx *) ctx)->terms;
}

bool
unitseam_bt_term_equality (void *ctx, int i)
{
  return ((bind_target_ctx *) ctx)->term[i].equality;
}

bool
unitseam_bt_unique_pinned (PARSER_CONTEXT *parser, PT_NODE *statement, void *ctx, UINTPTR spec_id)
{
  return bind_target_spec_is_unique_pinned (parser, statement, (bind_target_ctx *) ctx, spec_id);
}
