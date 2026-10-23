/*
 * PR#8194 (CBRD-27663) -- stats_log_update_progress (classop, phase, state, seq, total, class_start_msec,
 *                                                     run_start_msec, error)
 *
 * Unit under test: src/communication/network_interface_cl.c (CS_MODE) -- the client half of the progress
 * report of UPDATE STATISTICS ON ALL CLASSES. It packs class OID + 7 ints and sends
 * NET_SERVER_QST_LOG_STATS_PROGRESS; the server half (sqst_log_stats_progress) unpacks them in the same order.
 * The report must never change the caller's error: the request is wrapped in er_stack_push/er_stack_pop.
 *
 * Fakes: net_client_request records the request number, the bytes and the size and returns an injected
 * code; er_stack_push/er_stack_pop record their order relative to the request. stats_progress_clock_msec is
 * the real steady clock of the TU, so durations are checked as lower bounds and the clamp with a start far
 * in the past.
 */
#include "catch.hpp"
#include "config.h"

#include <climits>
#include <cstring>
#include <string>
#include <vector>

#include "error_code.h"
#include "dbtype_def.h"
#include "work_space.h"
#include "object_representation.h"
#include "network.h"
#include "statistics.h"
#include "network_interface_cl.h"

static std::vector < std::string > g_seq;
static int g_req = -1, g_argsize = -1, g_replysize = -1, g_net_rc;
static std::vector < unsigned char >g_arg;

int
net_client_request (int request, char *argbuf, int argsize, char *replybuf, int replysize, char *databuf,
		    int datasize, char *replydata, int replydatasize)
{
  g_seq.push_back ("request");
  g_req = request;
  g_argsize = argsize;
  g_replysize = replysize;
  g_arg.assign ((unsigned char *) argbuf, (unsigned char *) argbuf + argsize);
  return g_net_rc;
}

void
er_stack_push (void)
{
  g_seq.push_back ("push");
}

void
er_stack_pop (void)
{
  g_seq.push_back ("pop");
}

static void
reset ()
{
  g_seq.clear ();
  g_req = g_argsize = g_replysize = -1;
  g_net_rc = NO_ERROR;
  g_arg.clear ();
}

static int
int_at (int k)			/* k-th int after the OID, network order */
{
  int v;
  (void) or_unpack_int ((char *) g_arg.data () + OR_OID_SIZE + k * OR_INT_SIZE, &v);
  return v;
}

static DB_OBJECT g_mop;

static void
mop_init ()
{
  memset (&g_mop, 0, sizeof (g_mop));
  g_mop.oid_info.oid.volid = 3;
  g_mop.oid_info.oid.pageid = 4321;
  g_mop.oid_info.oid.slotid = 17;
}

TEST_CASE ("finished: OID then phase, state, seq, total, class_ms, elapsed_ms, error -- the server's unpack order",
	   "[contract]")
{
  reset ();
  mop_init ();
  INT64 now = stats_progress_clock_msec ();
  stats_log_update_progress (&g_mop, STATS_PROGRESS_HISTOGRAM, STATS_PROGRESS_FINISHED, 5, 34, now - 1500,
			     now - 60000, ER_FAILED);

  CHECK (g_req == NET_SERVER_QST_LOG_STATS_PROGRESS);
  REQUIRE (g_argsize == OR_OID_SIZE + OR_INT_SIZE * 7);
  CHECK (g_replysize == OR_INT_SIZE);
  OID oid;
  (void) or_unpack_oid ((char *) g_arg.data (), &oid);
  CHECK (oid.volid == 3);
  CHECK (oid.pageid == 4321);
  CHECK (oid.slotid == 17);
  CHECK (int_at (0) == STATS_PROGRESS_HISTOGRAM);
  CHECK (int_at (1) == STATS_PROGRESS_FINISHED);
  CHECK (int_at (2) == 5);
  CHECK (int_at (3) == 34);
  CHECK (int_at (4) >= 1500);
  CHECK (int_at (4) < 1500 + 5000);
  CHECK (int_at (5) >= 60000);
  CHECK (int_at (5) < 60000 + 5000);
  CHECK (int_at (6) == ER_FAILED);
}

TEST_CASE ("a run longer than INT_MAX ms is clamped, not wrapped negative", "[contract]")
{
  reset ();
  mop_init ();
  INT64 now = stats_progress_clock_msec ();
  stats_log_update_progress (&g_mop, STATS_PROGRESS_STATISTICS, STATS_PROGRESS_STARTED, 1, 1, now,
			     now - (INT64) INT_MAX * 3, NO_ERROR);

  REQUIRE (g_argsize == OR_OID_SIZE + OR_INT_SIZE * 7);
  CHECK (int_at (5) == INT_MAX);
  CHECK (int_at (4) >= 0);
}

TEST_CASE ("the request sits inside er_stack_push/er_stack_pop, and its failure is not returned", "[contract]")
{
  reset ();
  mop_init ();
  g_net_rc = ER_NET_SERVER_CRASHED;
  INT64 now = stats_progress_clock_msec ();
  stats_log_update_progress (&g_mop, STATS_PROGRESS_STATISTICS, STATS_PROGRESS_STARTED, 1, 2, now, now, NO_ERROR);

  REQUIRE (g_seq.size () == 3);
  CHECK (g_seq[0] == "push");
  CHECK (g_seq[1] == "request");
  CHECK (g_seq[2] == "pop");
}
