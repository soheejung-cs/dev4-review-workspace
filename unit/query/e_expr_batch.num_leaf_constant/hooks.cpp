/* unitseam appendix for src/query/expr_batch.c: eb_num_leaf_constant () and eb_raw_kid () are TU-static.
 *
 * A one-column chunk: a decoded (EB_VK_COL) NUMERIC vector of n rows whose int128 elements are the column's
 * coefficients at col_scale, and one raw NUMERIC leaf over it -- the constant k brought to the column's scale by
 * eb_num_leaf_constant () (as eb_load_leaf_constants () does once a chunk), then the leaf run by eb_raw_kid () over
 * every row.  kept[] gets the rows the leaf keeps (a NULL row stays, marked UNKNOWN, as eb_raw_kid () leaves it). */
int
unitseam_eb_num_leaf (const __int128 * col_vals, const unsigned char *col_null, int n, int col_scale,
		      const DB_VALUE * k, int rel_op, int *kept, int *n_kept, int *k_all, int *k_op, __int128 * kn)
{
  EXPR_BATCH_PLAN plan;
  EXPR_BATCH batch;
  EB_VEC vec;
  EB_VEC_SRC vsrc;
  EB_COL col;
  EB_LEAF leaf;
  int sel[256];
  unsigned char unk[256];
  __int128 data[256];
  unsigned char isnull[256];
  int i, rc;

  if (n <= 0 || n > 256)
    {
      return -100;
    }
  for (i = 0; i < n; i++)
    {
      data[i] = col_vals[i];
      isnull[i] = (col_null != NULL) ? col_null[i] : 0;
      sel[i] = i;
      unk[i] = 0;
    }
  memset (&vec, 0, sizeof (vec));
  vec.data = data;
  vec.isnull = isnull;
  vec.kind = EB_VK_COL;
  vec.etype = EB_NUM;
  vec.dbtype = DB_TYPE_NUMERIC;
  vec.scale = (short) col_scale;
  vec.col = 0;
  memset (&vsrc, 0, sizeof (vsrc));
  vsrc.precision = 38;
  vsrc.scale = (short) col_scale;
  memset (&col, 0, sizeof (col));
  col.vec = 0;
  col.type = DB_TYPE_NUMERIC;
  col.is_fixed = 0;
  col.scale = (short) col_scale;
  memset (&leaf, 0, sizeof (leaf));
  leaf.vec = 0;
  leaf.col = 0;
  leaf.vec2 = -1;
  leaf.col2 = -1;
  leaf.etype = EB_NUM;
  leaf.k_type = DB_TYPE_NUMERIC;
  leaf.rel_op = (unsigned char) rel_op;
  memset (&plan, 0, sizeof (plan));
  plan.vecs = &vec;
  plan.vsrc = &vsrc;
  plan.n_vecs = 1;
  plan.cols = &col;
  plan.n_cols = 1;
  plan.leaves = &leaf;
  plan.n_leaves = 1;
  plan.unk = unk;

  /* eb_load_leaf_constants (): eb_num_leaf_constant (leaf, k, plan->vsrc[leaf->vec].scale) */
  rc = eb_num_leaf_constant (&leaf, k, plan.vsrc[leaf.vec].scale);
  *k_all = leaf.k_all;
  *k_op = leaf.k_op;
  *kn = leaf.k.n;
  if (rc != NO_ERROR)
    {
      return rc;
    }
  memset (&batch, 0, sizeof (batch));
  batch.rows = NULL;
  batch.n_rows = n;
  batch.cap = n;
  batch.sel = sel;
  batch.n_sel = n;
  rc = eb_raw_kid (&plan, &batch, &leaf);
  *n_kept = batch.n_sel;
  for (i = 0; i < batch.n_sel; i++)
    {
      kept[i] = batch.sel[i];
    }
  return rc;
}
