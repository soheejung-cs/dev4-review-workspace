/* unitseam appendix for src/query/expr_compile.c: the kernel table and the kernel names are TU-static */

EXPR_KERNEL_FN
unitseam_arith_kernel (int opcode, int type, bool numeric_pure)
{
  return expr_arith_kernel ((OPERATOR_TYPE) opcode, (DB_TYPE) type, numeric_pure);
}

const char *
unitseam_kernel_name (EXPR_KERNEL_FN fn)
{
  return expr_kernel_name (fn);
}
