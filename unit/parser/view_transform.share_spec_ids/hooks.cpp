/* appended to view_transform.c: mq_share_view_refs () is static */
void
ut_share_view_refs (PARSER_CONTEXT * parser, PT_NODE * statement)
{
  mq_share_view_refs (parser, statement);
}

/* mq_translate_tree () is static: expand the specs of one SELECT, as mq_translate_select () does */
PT_NODE *
ut_translate_tree (PARSER_CONTEXT * parser, PT_NODE * select_statement)
{
  return mq_translate_tree (parser, select_statement, select_statement->info.query.q.select.from, NULL,
			    DB_AUTH_SELECT);
}
