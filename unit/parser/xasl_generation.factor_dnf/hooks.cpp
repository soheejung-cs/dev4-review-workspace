
/* ---- unitseam appendix (unit/parser/xasl_generation.factor_dnf): reach the static DNF helpers of
 * pt_to_pred_expr_factor_dnf () from the test, as the TU calls them ---- */

/* pt_factor_dnf_node () over the PT_FACTOR_DNF_MAX_DISJUNCTS buffer pt_to_pred_expr_factor_dnf () gives it; the leaves
 * of disjunct d go to leaves[] in order and their count to lens[d] */
int
ut_factor_dnf (PT_NODE * node, PT_NODE ** leaves, int *lens, int max_leaves)
{
  PT_FACTOR_DNF_TERM *dnf;
  int n, d, i, k = 0;

  dnf = (PT_FACTOR_DNF_TERM *) malloc (PT_FACTOR_DNF_MAX_DISJUNCTS * sizeof (PT_FACTOR_DNF_TERM));
  if (dnf == NULL)
    {
      return -2;
    }
  n = pt_factor_dnf_node (node, dnf, PT_FACTOR_DNF_MAX_DISJUNCTS);
  for (d = 0; d < n; d++)
    {
      lens[d] = dnf[d].n;
      for (i = 0; i < dnf[d].n && k < max_leaves; i++)
	{
	  leaves[k++] = dnf[d].leaf[i];
	}
    }
  free (dnf);
  return n;
}

int
ut_factor_dnf_top (PT_NODE * node)
{
  return pt_factor_dnf_top (node);
}

/* the walk pt_to_pred_expr_factor_dnf () runs, its next cut */
bool
ut_factor_dnf_unsafe (PARSER_CONTEXT * parser, PT_NODE * factor)
{
  PT_FACTOR_DNF_CHECK check = { 0, false };
  PT_NODE *save_next = factor->next;

  factor->next = NULL;
  (void) parser_walk_tree (parser, factor, pt_factor_dnf_is_unsafe, &check, NULL, NULL);
  factor->next = save_next;
  return check.unsafe;
}

PRED_EXPR *
ut_to_pred_expr_factor_dnf (PARSER_CONTEXT * parser, PT_NODE * factor, int *argp)
{
  return pt_to_pred_expr_factor_dnf (parser, factor, argp);
}
