/* appended to histogram_cl.cpp (same TU): reach TU statics.
 *
 * unitseam_split_hv_predicate   histogram_split_hv_predicate, same signature at base and head.
 * unitseam_fp_active_parser /   the ambient context histogram_init_reader_from_lhs falls back to the
 * unitseam_fp_active_statement  class cache with (bind_fp_active_parser / _statement, present at base and
 *                               head): the histogram-read fakes in test.cpp read it the way the real
 *                               reader does. Since 2364bcc7a histogram_stmt_has_hv_predicate sets it too.
 *
 * (adc0adf4a's histogram_column_has_histogram is gone in 2364bcc7a -- hv_pred_walk now asks
 *  histogram_get_total_rows, the test bind_term_add gives a slot by.) */
bool
unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
			     bool *out_reversed)
{
  return histogram_split_hv_predicate (parser, node, out_name, out_hv, out_reversed);
}

PARSER_CONTEXT *
unitseam_fp_active_parser (void)
{
  return bind_fp_active_parser;
}

PT_NODE *
unitseam_fp_active_statement (void)
{
  return bind_fp_active_statement;
}
