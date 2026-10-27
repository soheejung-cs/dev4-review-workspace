/* unitseam appendix for src/query/expr_batch.c: eb_run_steps (), eb_num_get () and eb_pack_dbv () are TU-static.
 *
 * One NUMERIC arithmetic step as eb_fused_tree () / eb_build_program () emit it: a decoded NUMERIC column vector
 * (EB_VK_COL, EB_N64, every element at col_scale) op a NUMERIC scalar (EB_VK_SCALAR, EB_NUM, at k_scale) into a slot
 * vector (EB_VK_SLOT, EB_NUM, a scale byte per element, as expr_batch_init () carves it).  After eb_run_steps () each
 * selected row's element is read back the way its consumers read it: eb_num_get () (the SUM loop, eb_num_load ()) and
 * eb_pack_dbv () (expr_batch_agg_next_value (), MIN/MAX, the projection). */
int
unitseam_eb_num_step (int op, const int64_t * col, int n, int col_scale, __int128 k, int k_scale,
		      NUMERIC_AGG_EXPR_VAL * got, DB_VALUE * packed)
{
  EXPR_BATCH_PLAN plan;
  EXPR_BATCH batch;
  EB_VEC vecs[3];
  EB_VEC_SRC vsrc[3];
  EB_STEP step;
  int64_t cdata[64];
  unsigned char cnull[64];
  __int128 kdata[1];
  unsigned char knull[1];
  __int128 odata[64];
  unsigned char onull[64];
  short oscales[64];		/* wide enough for either element type of EB_VEC.scales */
  int sel[64], i, rc;

  if (n <= 0 || n > 64)
    {
      return -100;
    }
  for (i = 0; i < n; i++)
    {
      cdata[i] = col[i];
      cnull[i] = 0;
      onull[i] = 1;
      sel[i] = i;
    }
  kdata[0] = k;
  knull[0] = 0;
  memset (vecs, 0, sizeof (vecs));
  memset (vsrc, 0, sizeof (vsrc));
  vecs[0].data = cdata;
  vecs[0].isnull = cnull;
  vecs[0].kind = EB_VK_COL;
  vecs[0].etype = EB_N64;
  vecs[0].dbtype = DB_TYPE_NUMERIC;
  vecs[0].scale = (short) col_scale;
  vsrc[0].precision = 18;
  vsrc[0].scale = (short) col_scale;
  vecs[1].data = kdata;
  vecs[1].isnull = knull;
  vecs[1].kind = EB_VK_SCALAR;
  vecs[1].etype = EB_NUM;
  vecs[1].dbtype = DB_TYPE_NUMERIC;
  vecs[1].scale = (short) k_scale;
  vecs[2].data = odata;
  vecs[2].isnull = onull;
  memset (oscales, 0, sizeof (oscales));
  vecs[2].scales = (decltype (vecs[2].scales)) oscales;
  vecs[2].kind = EB_VK_SLOT;
  vecs[2].etype = EB_NUM;
  vecs[2].dbtype = DB_TYPE_NUMERIC;
  memset (&step, 0, sizeof (step));
  step.op = (unsigned char) op;
  step.in1 = 0;
  step.in2 = 1;
  step.out = 2;
  step.etype = EB_NUM;
  step.result_type = DB_TYPE_NUMERIC;
  memset (&plan, 0, sizeof (plan));
  plan.vecs = vecs;
  plan.vsrc = vsrc;
  plan.n_vecs = 3;
  plan.steps = &step;
  plan.n_steps = 1;
  memset (&batch, 0, sizeof (batch));
  batch.n_rows = n;
  batch.cap = n;
  batch.sel = sel;
  batch.n_sel = n;
  rc = eb_run_steps (&plan, &batch);
  if (rc != NO_ERROR)
    {
      return rc;
    }
  for (i = 0; i < n; i++)
    {
      eb_num_get (&vecs[2], i, &got[i]);
      eb_pack_dbv (&plan, &vecs[2], i, &packed[i]);
    }
  return NO_ERROR;
}

int
unitseam_eb_op_add (void)
{
  return EB_OP_ADD;
}

int
unitseam_eb_op_mul (void)
{
  return EB_OP_MUL;
}
