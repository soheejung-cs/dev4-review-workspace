/* unitseam appendix for src/query/expr_compile.c: the kernel ids are TU-static */

/* CBRD-28120: the generic steps of a program that own a slot -- the ones that compute once per execution */
int
unitseam_generic_slots (const EXPR_PROG * prog)
{
  int n = 0;

  for (int i = 0; i < prog->n_steps; i++)
    {
      n += (prog->steps[i].kernel_id == EXPR_KID_generic && prog->steps[i].out != NULL) ? 1 : 0;
    }
  return n;
}
