/* appended to src/compat/db_vdb.c (same TU): do_process_prepare_statement () is static there -- the handler of an
 * SQL-level 'PREPARE name FROM ...' (db_execute_and_keep_statement_local (), db_vdb.c:3378 at bb9575015) */
int
ut_process_prepare (DB_SESSION * session, PT_NODE * statement)
{
  return do_process_prepare_statement (session, statement);
}
