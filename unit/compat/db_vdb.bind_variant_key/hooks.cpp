/* appended to db_vdb.c: reach the TU's statics (db_bind_variant_switch/step, the key slot) from the test */
int
unitseam_vdb_switch (PARSER_CONTEXT * parser, PT_NODE * statement, BIND_WATCH_STATE * ws, int variant, bool compile)
{
  return db_bind_variant_switch (parser, statement, ws, variant, compile);
}

int
unitseam_vdb_step (PARSER_CONTEXT * parser, PT_NODE * statement)
{
  return db_bind_variant_step (parser, statement);
}

const char *
unitseam_vdb_key (PT_NODE * statement)
{
  const char **p = db_stmt_bind_variant_key_ptr (statement);
  return (p != NULL) ? *p : NULL;
}
