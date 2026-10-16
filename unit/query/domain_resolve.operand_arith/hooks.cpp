/*
 * unitseam appendix for src/query/domain_resolve.c (compiled at the end of that TU): its statics, for
 * unit/query/domain_resolve.operand_arith.
 */
int
unitseam_resolve_late_bind_node (const DOMAIN_PLAN * plan, int index, RESOLVED_DOMAIN_TABLE * resolved)
{
  /* the XASL is read only for its query alias, in an unresolved-domain error */
  static char xasl_buf[sizeof (xasl_node)];
  memset (xasl_buf, 0, sizeof (xasl_buf));
  DOMAIN_DEFERRED_ERRORS deferred = { NULL, 0, 0 };
  const int error = qexec_resolve_late_bind_node (NULL, (const xasl_node *) xasl_buf, plan, index, *resolved, deferred);
  if (deferred.errors != NULL)
    {
      db_private_free (NULL, deferred.errors);
    }
  return error;
}

bool
unitseam_resolve_operand (const DOMAIN_PLAN * plan, const RESOLVED_DOMAIN_TABLE * resolved, int link, int arg_index,
			  DOMAIN_CTX context, int opcode, DOMAIN_OPERAND * operand)
{
  return qexec_resolve_operand (NULL, plan, *resolved, &plan->late_bind_links[link], arg_index, context, opcode,
				operand);
}
