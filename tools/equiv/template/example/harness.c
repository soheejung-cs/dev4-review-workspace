/* the same two strings (length 0..4, any bytes) into both functions; the answers must agree */
#include <assert.h>
#include "stubs.h"
#define MAXN 4
unsigned char nondet_uchar (void);
int nondet_int (void);
bool eb_str_eq_binary__orig (const unsigned char *a, int na, const unsigned char *b, int nb);
bool eb_str_eq_binary__variant (const unsigned char *a, int na, const unsigned char *b, int nb);
int
main (void)
{
  unsigned char a[MAXN], b[MAXN];
  int na = nondet_int (), nb = nondet_int (), i;
  __CPROVER_assume (na >= 0 && na <= MAXN && nb >= 0 && nb <= MAXN);
  for (i = 0; i < MAXN; i++)
    {
      a[i] = nondet_uchar ();
      b[i] = nondet_uchar ();
    }
  assert (eb_str_eq_binary__orig (a, na, b, nb) == eb_str_eq_binary__variant (a, na, b, nb));
  return 0;
}
