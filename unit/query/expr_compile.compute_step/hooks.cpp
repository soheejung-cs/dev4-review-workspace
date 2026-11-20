/* unitseam appendix for src/query/expr_compile.c: EXPR_PRED and the kernel ids are TU-static */

/* CBRD-28249: the program's generic step that owns a slot -- the one that computes once per execution; -1 when none */
int
unitseam_generic_slot_step (const EXPR_PROG * prog)
{
  for (int i = 0; i < prog->n_steps; i++)
    {
      if (prog->steps[i].kernel_id == EXPR_KID_generic && prog->steps[i].out != NULL)
	{
	  return i;
	}
    }
  return -1;
}

/* expr_scan_pred_compute_step () over a filter root that holds the program, as a compiled scan filter's root does */
int
unitseam_compute_step (EXPR_PROG * prog, int step, cubthread::entry * thread_p, val_descr * vd)
{
  EXPR_PRED root;

  memset (&root, 0, sizeof (root));
  root.prog = prog;
  return expr_scan_pred_compute_step (&root, step, thread_p, vd);
}
