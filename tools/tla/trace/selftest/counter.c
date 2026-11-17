/* selftest — 공유 카운터를 스레드 둘이 올린다. locked: 잠금 안에서 c++ · racy: 읽고 잠깐 돈 뒤 쓴다(갱신을 잃는다).
   시작 장벽으로 두 스레드를 같이 출발시킨다 — 안 그러면 한 스레드가 다 끝난 뒤 다음이 떠서 엇갈림이 없다. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../tla_trace.h"

static int c;
static int racy;
static int per_thread;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t start;
static volatile int sink;

static void *
worker (void *arg)
{
  int i, v;
  (void) arg;
  pthread_barrier_wait (&start);
  for (i = 0; i < per_thread; i++)
    {
      TLA_TRACE_BEGIN (t);
      if (racy)
	{
	  int k;
	  v = c;
	  for (k = 0; k < 2000; k++)
	    sink++;
	  c = v + 1;
	  v = v + 1;
	}
      else
	{
	  pthread_mutex_lock (&m);
	  v = ++c;
	  pthread_mutex_unlock (&m);
	}
      TLA_TRACE_END_ST (t, "Inc", "\"c\":%d", v);
    }
  return NULL;
}

int
main (int argc, char **argv)
{
  int n = 2, i;
  pthread_t th[16];
  racy = argc > 1 && strcmp (argv[1], "racy") == 0;
  per_thread = argc > 2 ? atoi (argv[2]) : 30;
  pthread_barrier_init (&start, NULL, n);
  for (i = 0; i < n; i++)
    pthread_create (&th[i], NULL, worker, NULL);
  for (i = 0; i < n; i++)
    pthread_join (th[i], NULL);
  printf ("c=%d expected=%d\n", c, n * per_thread);
  return 0;
}
