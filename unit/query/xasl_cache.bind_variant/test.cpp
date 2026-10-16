#include "catch.hpp"
#include "xasl_cache.h"
#include "bind_variant.h"
#include <cstring>

/* fakes for the seams: the cache lookup returns the one entry under test */
static XASL_CACHE_ENTRY *g_entry;
int xcache_find_sha1 (THREAD_ENTRY *, const SHA1Hash *, const XASL_CACHE_SEARCH_MODE, XASL_CACHE_ENTRY **e,
		      xasl_cache_rt_check_result *)
{
  *e = g_entry;
  return NO_ERROR;
}
void xcache_unfix (THREAD_ENTRY *, XASL_CACHE_ENTRY *) {}

/* appendix hooks (compiled inside xasl_cache.c) */
void unitseam_xcache_enable (bool on);
int unitseam_dir_pending (XASL_CACHE_ENTRY *e);

static BIND_VARIANT_REQUEST req (int op, double rows, int variant = -1, UINT64 sig = 0)
{
  BIND_VARIANT_REQUEST r;
  memset (&r, 0, sizeof r);
  r.time_stored = g_entry->xasl_id.time_stored;
  r.op = op;
  r.limit = 2;
  r.variant = variant;
  r.band = 1.5;
  r.floor = 1000.0;
  r.plan_sig = sig;
  r.fp.terms = 1;
  r.fp.rows[0] = rows;
  return r;
}

static BIND_VARIANT_REPLY ask (const BIND_VARIANT_REQUEST &r)
{
  BIND_VARIANT_REPLY rep;
  memset (&rep, 0, sizeof rep);
  REQUIRE (xcache_bind_variant (NULL, &r, &rep) == NO_ERROR);
  return rep;
}

static void fresh_entry ()
{
  g_entry = new XASL_CACHE_ENTRY ();
  g_entry->xasl_id.time_stored.sec = 1234;
  g_entry->xasl_id.time_stored.usec = 5;
  unitseam_xcache_enable (true);
}

TEST_CASE ("every reserved variant registered -> the query settles (limit 2)", "[contract]")
{
  fresh_entry ();
  BIND_VARIANT_REPLY a = ask (req (BIND_VARIANT_OP_CHECK, 1e4));
  REQUIRE (a.result == BIND_VARIANT_COMPILE);
  ask (req (BIND_VARIANT_OP_REGISTER, 1e4, a.variant, 111));
  BIND_VARIANT_REPLY b = ask (req (BIND_VARIANT_OP_CHECK, 1e6));
  REQUIRE (b.result == BIND_VARIANT_COMPILE);
  BIND_VARIANT_REPLY rb = ask (req (BIND_VARIANT_OP_REGISTER, 1e6, b.variant, 222));
  CHECK (rb.result == BIND_VARIANT_NEW);
  CHECK (rb.state == BIND_VARIANT_STATE_SELECT);
  CHECK (unitseam_dir_pending (g_entry) == 0);
}

TEST_CASE ("PR8013 F1: a COMPILE reservation never REGISTERed pins the directory in WATCH", "[defect]")
{
  fresh_entry ();
  BIND_VARIANT_REPLY a = ask (req (BIND_VARIANT_OP_CHECK, 1e4));	/* client A */
  REQUIRE (a.result == BIND_VARIANT_COMPILE);
  ask (req (BIND_VARIANT_OP_REGISTER, 1e4, a.variant, 111));
  BIND_VARIANT_REPLY b = ask (req (BIND_VARIANT_OP_CHECK, 1e6));	/* client B reserves ... */
  REQUIRE (b.result == BIND_VARIANT_COMPILE);			/* ... and dies before REGISTER */

  /* client C keeps arriving with values no variant covers */
  for (int i = 0; i < 50; i++)
    {
      BIND_VARIANT_REPLY c = ask (req (BIND_VARIANT_OP_CHECK, 1e8 + i));
      INFO ("check #" << i);
      REQUIRE (c.result == BIND_VARIANT_FROZEN);		/* limit 2 counts the dead slot as a plan */
      REQUIRE (c.state == BIND_VARIANT_STATE_WATCH);		/* never DONE/SELECT: pending stays 1 */
      REQUIRE (c.plans == 1);					/* the query learned 1 of its 2 plans */
      REQUIRE (c.pending == 1);
    }
  CHECK (unitseam_dir_pending (g_entry) == 1);
}
