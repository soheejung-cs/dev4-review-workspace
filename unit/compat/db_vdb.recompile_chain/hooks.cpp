/* appended to db_vdb.c (same TU): reach the static do_recompile_and_execute_prepared_statement from the test.
 * The test makes db_open_buffer_copy_local (seam) throw, so a call runs only what the function does before it opens
 * the subsession of this execution -- since CBRD-27852 (PR#8343), closing the subsessions earlier executions of the
 * session left on its transient chain. */
struct unitseam_open_reached
{
};

/* true when the call reached db_open_buffer_copy_local (the fake throws there) */
bool
unitseam_recompile_until_open (DB_SESSION * session, PT_NODE * statement)
{
  DB_QUERY_RESULT *result = NULL;
  try
  {
    (void) do_recompile_and_execute_prepared_statement (session, statement, &result);
  }
  catch (const unitseam_open_reached &)
  {
    return true;
  }
  return false;
}

/* the fake of db_open_buffer_copy_local throws this */
void
unitseam_throw_open_reached (void)
{
  throw unitseam_open_reached ();
}
