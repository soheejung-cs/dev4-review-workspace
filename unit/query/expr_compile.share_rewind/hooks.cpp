/* unitseam appendix for src/query/expr_compile.c (PR#8469, CBRD-27994): the build context, its mark and rewind, the
 * compile of one node, the filter's share registry and the projection list's keep rule are TU-static */

/* a compiled data filter's root holding only its share registry -- what expr_scan_pred_share_build () leaves on the
 * root for the scan's other consumers: share[i] the node regus[i] and the result slot slots[i] it publishes into.
 * Freed by expr_scan_pred_free () (the registry, then the root). */
void *
unitseam_filter_with_share (const REGU_VARIABLE * const *regus, DB_VALUE * const *slots, int n)
{
  EXPR_PRED *root = (EXPR_PRED *) malloc (sizeof (EXPR_PRED));
  int i;

  if (root == NULL)
    {
      return NULL;
    }
  memset (root, 0, sizeof (*root));
  root->kind = EXPR_PRED_COMP;
  root->share = (struct expr_share *) malloc (sizeof (struct expr_share) * n);
  if (root->share == NULL)
    {
      free (root);
      return NULL;
    }
  for (i = 0; i < n; i++)
    {
      root->share[i].regu = regus[i];
      root->share[i].slot = slots[i];
      root->share[i].cell = i;
    }
  root->n_share = n;
  return root;
}

/* a build context set up as expr_prog_compile_roots_impl () sets it up on the server (expr_prog_compile_roots ()):
 * zeroed, the server's environment without a value descriptor, the scan's filter attached (expr_share_attach ()) */
void *
unitseam_bctx_new (const void *share_spec)
{
  EXPR_BUILD_CTX *bctx = (EXPR_BUILD_CTX *) malloc (sizeof (EXPR_BUILD_CTX));

  if (bctx == NULL)
    {
      return NULL;
    }
  memset (bctx, 0, sizeof (*bctx));
  bctx->env = expr_server_env (NULL, NULL);
  bctx->force_nest_depth = 0;
  expr_share_attach (bctx, share_spec);
  return bctx;
}

void
unitseam_bctx_free (void *bctx)
{
  expr_build_free_preds ((EXPR_BUILD_CTX *) bctx);
  free (bctx);
}

/* one root as expr_prog_compile_roots_impl () compiles it: expr_compile_node () at depth 0, the table-full flag
 * cleared after it; the cell, or -1 when the compiler rejects the node */
int
unitseam_compile (void *p, REGU_VARIABLE * regu)
{
  EXPR_BUILD_CTX *bctx = (EXPR_BUILD_CTX *) p;
  bool compiled_something = false;
  int cell;

  bctx->side_effect = false;
  cell = expr_compile_node (bctx, regu, &compiled_something);
  bctx->full = false;
  return cell;
}

void *
unitseam_mark (const void *bctx)
{
  EXPR_BUILD_MARK *mark = (EXPR_BUILD_MARK *) malloc (sizeof (EXPR_BUILD_MARK));

  if (mark != NULL)
    {
      expr_build_mark ((const EXPR_BUILD_CTX *) bctx, mark);
    }
  return mark;
}

void
unitseam_rewind (void *bctx, const void *mark)
{
  expr_build_rewind ((EXPR_BUILD_CTX *) bctx, (const EXPR_BUILD_MARK *) mark);
}

void
unitseam_mark_free (void *mark)
{
  free (mark);
}

int
unitseam_n_cells (const void *bctx)
{
  return ((const EXPR_BUILD_CTX *) bctx)->n_cells;
}

int
unitseam_n_shared (const void *bctx)
{
  return ((const EXPR_BUILD_CTX *) bctx)->n_shared;
}

/* the flag of an index, live or rewound (cell_shared[] has EXPR_MAX_STEPS entries) */
bool
unitseam_cell_shared (const void *bctx, int cell)
{
  return cell >= 0 && cell < EXPR_MAX_STEPS && ((const EXPR_BUILD_CTX *) bctx)->cell_shared[cell];
}

int
unitseam_cell_src (const void *bctx, int cell)
{
  return ((const EXPR_BUILD_CTX *) bctx)->cell_src[cell];
}

const DB_VALUE *
unitseam_cell_addr (const void *bctx, int cell)
{
  return ((const EXPR_BUILD_CTX *) bctx)->cells[cell];
}

/* the live cells that read a filter slot */
int
unitseam_n_share_cells (const void *p)
{
  const EXPR_BUILD_CTX *bctx = (const EXPR_BUILD_CTX *) p;
  int i, n = 0;

  for (i = 0; i < bctx->n_cells; i++)
    {
      n += (bctx->cell_src[i] == EXPR_CSRC_SHARE) ? 1 : 0;
    }
  return n;
}

/* the projection list's keep rule, as both expr_valptr_prog_image_at_generation () and expr_valptr_prog_settle ()
 * apply it to the compiled program */
EXPR_PROG *
unitseam_valptr_prog_keep (EXPR_PROG * prog, const int *idx, int n)
{
  return expr_valptr_prog_keep (prog, idx, n);
}

/* the aggregate operand list's keep rule (a program that covers a root is kept, wired-only included), as both
 * expr_agg_prog_image_at_generation () and expr_agg_prog_settle () apply it -- added 2026-11-10, head 1de189701 */
EXPR_PROG *
unitseam_agg_prog_keep (EXPR_PROG * prog, const int *idx, int n)
{
  return expr_agg_prog_keep (prog, idx, n);
}

/* the size of the build context's tables (EXPR_MAX_STEPS is private to the TU) */
int
unitseam_max_cells (void)
{
  return EXPR_MAX_STEPS;
}
