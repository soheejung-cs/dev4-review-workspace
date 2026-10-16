/* appended to histogram_cl.cpp: the slot vector histogram_bind_watch_check compares, straight from
 * the TU-static walk it runs (bind_term_vector -> bind_term_walk -> bind_term_add, want_names on).
 * bind_term_ctx is TU-local, so the test gets the rows and labels copied out. */
int
unitseam_bw_term_vector (PARSER_CONTEXT *parser, PT_NODE *statement, double *rows,
			 char (*names)[BIND_WATCH_NAME_LEN])
{
  bind_term_ctx ctx;
  int i;

  if (!bind_term_vector (parser, statement, &ctx, true))
    {
      return 0;
    }
  for (i = 0; i < ctx.count; i++)
    {
      rows[i] = ctx.rows[i];
      memcpy (names[i], ctx.name[i], BIND_WATCH_NAME_LEN);
    }
  return ctx.count;
}
