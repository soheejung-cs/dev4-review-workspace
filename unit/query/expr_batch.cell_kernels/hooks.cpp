/* unitseam appendix for src/query/expr_batch.c: the cell readers and the string equality kernels are TU-static */

bool
unitseam_eb_str_eq_class (const unsigned char *a, int na, const unsigned char *b, int nb, bool ti)
{
  return eb_str_eq_class (a, na, b, nb, ti);
}

bool
unitseam_eb_str_eq_binary (const unsigned char *a, int na, const unsigned char *b, int nb)
{
  return eb_str_eq_binary (a, na, b, nb);
}

/* a VARCHAR column whose long-format scratch is max_bytes per row (0: none, as for a column wider than the scratch) */
int
unitseam_eb_cell_str_long (int max_bytes, char *scratch, unsigned char *scratch_set, int r, const unsigned char *d,
			   int avail, const char **str, int *len)
{
  EB_COL col;

  memset (&col, 0, sizeof (col));
  col.type = DB_TYPE_VARCHAR;
  col.max_bytes = max_bytes;
  col.scratch = scratch;
  col.scratch_set = scratch_set;
  return eb_cell_str_long (&col, r, d, avail, str, len);
}

bool
unitseam_eb_num_from_disk (const unsigned char *disk, int avail, NUMERIC_AGG_EXPR_VAL * num)
{
  return eb_num_from_disk (disk, avail, num);
}
