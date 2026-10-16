/* appended to histogram_cl.cpp: the candidate test both bind walks share (bind_fp_walk for the
 * fingerprint, hv_pred_walk for the unpeeked flag) is TU-static, so the test reaches it here.
 * Same signature at the PR base (9ee8bac8b) and head. */
bool
unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
			     bool *out_reversed)
{
  return histogram_split_hv_predicate (parser, node, out_name, out_hv, out_reversed);
}
