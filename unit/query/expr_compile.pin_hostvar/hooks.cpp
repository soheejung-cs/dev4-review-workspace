/* unitseam appendix for src/query/expr_compile.c (CBRD-28165, PR#8661): the scan-filter leaf (EXPR_PRED), its pin of
 * a host variable and the functions that convert it per execution are TU-static.  The leaves here are what
 * expr_scan_pred_build () makes for "col < ?" when it pins the host variable: kind COMP_FETCH, the right holder wired to
 * pinned_rhs (as expr_pred_bind () wires it), the term's decided comparison in compare. */

bool
unitseam_pin_safe (DB_TYPE to, DB_TYPE from)
{
  return expr_pin_hostvar_safe (to, from);
}

/* a host variable operand of the given position and domain type */
void *
unitseam_hostvar_regu (int val_pos, DB_TYPE type)
{
  REGU_VARIABLE *r = (REGU_VARIABLE *) calloc (1, sizeof (REGU_VARIABLE));

  r->type = TYPE_POS_VALUE;
  r->value.val_pos = val_pos;
  r->domain = tp_domain_resolve_default (type);
  /* fetch_peek_dbval ()'s inline path reads the bound value at its position (fetch.c is not linked) */
  REGU_VARIABLE_SET_FLAG (r, REGU_VARIABLE_FAST_PEEK);
  return r;
}

/* the term's decided comparison as the pin needs it: the right side converted by conv into the target type */
void *
unitseam_compare (TP_VALUE_CONVERTER conv, DB_TYPE target)
{
  DOMAIN_COMPARE *c = (DOMAIN_COMPARE *) calloc (1, sizeof (DOMAIN_COMPARE));

  c->conv[1] = conv;
  c->target[1] = tp_domain_resolve_default (target);
  return c;
}

/* a scan-filter leaf whose right side is a pinned host variable (pin true) or a plain fetched one */
void *
unitseam_leaf (void *rhs_regu, void *compare, DB_TYPE fast_type, bool pin)
{
  EXPR_PRED *p = (EXPR_PRED *) calloc (1, sizeof (EXPR_PRED));

  p->kind = EXPR_PRED_COMP_FETCH;
  p->fetch_rhs = (REGU_VARIABLE *) rhs_regu;
  p->compare = (const DOMAIN_COMPARE *) compare;
  p->fast_type = fast_type;
  p->rel_op = R_LT;
  db_make_null (&p->pinned_rhs);
  p->arg1p = &p->fetched1;
  p->arg2p = &p->fetched2;
  if (pin)
    {
      p->rhs_pinned = true;
      p->rhs_pin_hostvar = true;
      p->fetched2 = &p->pinned_rhs;
    }
  return p;
}

void *
unitseam_and (void *l, void *r)
{
  EXPR_PRED *p = (EXPR_PRED *) calloc (1, sizeof (EXPR_PRED));

  p->kind = EXPR_PRED_AND;
  p->lhs = (EXPR_PRED *) l;
  p->rhs = (EXPR_PRED *) r;
  return p;
}

/* a value descriptor of n bound values; query_id 0 leaves it without an execution identity (no xasl_state) */
void *
unitseam_vd (const DB_VALUE * vals, int n, long long query_id)
{
  val_descr *vd = (val_descr *) calloc (1, sizeof (val_descr));

  vd->dbval_ptr = vals;
  vd->dbval_cnt = n;
  if (query_id != 0)
    {
      XASL_STATE *xs = (XASL_STATE *) calloc (1, sizeof (XASL_STATE));

      xs->query_id = (QUERY_ID) query_id;
      vd->xasl_state = xs;
    }
  return vd;
}

void
unitseam_vd_free (void *v)
{
  val_descr *vd = (val_descr *) v;

  free (vd->xasl_state);
  free (vd);
}

/* the row path's use of the leaf's right side (expr_scan_side_value ()) */
int
unitseam_rhs_use (void *pred, void *vd)
{
  EXPR_EVAL_CTX ctx;

  memset (&ctx, 0, sizeof (ctx));
  ctx.vd = (val_descr *) vd;
  return expr_scan_side_value ((EXPR_PRED *) pred, true, &ctx);
}

/* the value the leaf compares against: *arg2p, the holder expr_pred_bind () wired */
const DB_VALUE *
unitseam_rhs_value (void *pred)
{
  return *((EXPR_PRED *) pred)->arg2p;
}

int
unitseam_prepare (void *root, void *vd)
{
  return expr_scan_pred_prepare (root, NULL, (val_descr *) vd);
}

void
unitseam_reset (void *root)
{
  expr_scan_pred_reset (root);
}

void
unitseam_free (void *root)
{
  expr_scan_pred_free (root);
}
