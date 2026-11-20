/* variant: src/query/expr_batch.c eb_str_eq_binary at 2e67e1d94, renamed only (cbmc_equiv.py compares it) */
#include "stubs.h"

bool
eb_str_eq_binary__variant (const unsigned char *a, int na, const unsigned char *b, int nb)
{
  int n = (na < nb) ? na : nb, i;
  const unsigned char *tail;

  if (memcmp (a, b, (size_t) n) != 0)
    {
      return false;
    }
  tail = (na > nb) ? a + n : b + n;
  for (i = 0; i < ((na > nb) ? na - nb : nb - na); i++)
    {
      if (tail[i] != 0x00)
	{
	  return false;
	}
    }
  return true;
}
