/* appended to view_transform.c: the static steps of the translation of a view reference that the cases take */

/* mq_share_view_refs () is static (mq_translate_helper () calls it on the whole statement, view_transform.c:10035) */
void
ut_share_view_refs (PARSER_CONTEXT * parser, PT_NODE * statement)
{
  mq_share_view_refs (parser, statement);
}

/* the question mq_substitute_subquery_in_statement () asks of the view's body for spec read in select (3085):
 * 1 when the body would be merged into select, 0 when spec is rewritten as a derived table */
int
ut_view_is_merged (PARSER_CONTEXT * parser, PT_NODE * select, PT_NODE * spec)
{
  PT_NODE *query_spec = mq_fetch_subqueries (parser, spec->info.spec.flat_entity_list);

  return mq_is_pushable_subquery (parser, query_spec, select, spec, true, NULL,
				  spec->info.spec.flat_entity_list) == PUSHABLE;
}

/* the NON_PUSHABLE branch of mq_substitute_subquery_in_statement () (3088-3097): the view reference spec of select is
 * rewritten as a derived table, its select list removable as mq_is_removable_select_list () judges of the body */
PT_NODE *
ut_rewrite_view_spec (PARSER_CONTEXT * parser, PT_NODE * select, PT_NODE * spec)
{
  PT_NODE *query_spec = mq_fetch_subqueries (parser, spec->info.spec.flat_entity_list);
  bool is_removable_select_list = mq_is_removable_select_list (parser, query_spec, select) == PUSHABLE;

  return mq_rewrite_vclass_spec_as_derived (parser, select, spec, query_spec, is_removable_select_list);
}
