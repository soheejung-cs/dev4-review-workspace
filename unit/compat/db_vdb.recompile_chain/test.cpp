/*
 * Contract suite -- CBRD-27852 (PR#8343): do_recompile_and_execute_prepared_statement (src/compat/db_vdb.c, CS_MODE)
 * closes, before it opens the subsession of this execution, the subsessions that earlier executions of the same client
 * session left on its transient `next` chain; a session that is itself a subsession (a kept tree, whose `next` links
 * the kept-tree list) keeps its chain.
 * Real code: do_recompile_and_execute_prepared_statement up to its db_open_buffer_copy_local call (hooks.cpp).
 * Fakes:
 *   db_open_buffer_copy_local (seam, db_vdb.c) -- throws, so the call stops where this execution's subsession would
 *       be opened; the test sees only what ran before it.
 *   db_close_session_local (seam, db_vdb.c:5761 @ec36a772f) -- the real one closes session->signature_plans, then the
 *       transient `next` chain, then frees the statements and the parser. The fake walks the same chains in the same
 *       order and counts closes per session instead of freeing.
 * Tags: [contract] = the contract (a FAIL at head is a regression candidate). Before PR#8343 (base 604ab8b22) R1 FAILs:
 * the chain stays until the client session closes.
 */
#include "catch.hpp"
#include "config.h"
#include "dbi.h"
#include "db.h"
#include "parser.h"
#include "parse_tree.h"
#include "db_session.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

bool unitseam_recompile_until_open (DB_SESSION * session, PT_NODE * statement);
void unitseam_throw_open_reached (void);

static std::map<const DB_SESSION *, int> g_closes;
static std::vector<const DB_SESSION *> g_close_log;

/* fake of db_vdb.c db_open_buffer_copy_local: stop the call where this execution's subsession would be opened */
DB_SESSION *
db_open_buffer_copy_local (const char *buffer, size_t len)
{
  (void) buffer;
  (void) len;
  unitseam_throw_open_reached ();
  return NULL;
}

/* fake of db_vdb.c db_close_session_local: the chains the real one walks, in its order; counts instead of freeing */
void
db_close_session_local (DB_SESSION * session)
{
  if (!session)
    {
      return;
    }
  while (session->signature_plans != NULL)
    {
      DB_SESSION *plan = session->signature_plans;
      session->signature_plans = plan->next;
      plan->next = NULL;
      db_close_session_local (plan);
    }
  DB_SESSION *prepared = session->next;
  while (prepared)
    {
      DB_SESSION *next = prepared->next;
      prepared->next = NULL;
      db_close_session_local (prepared);
      prepared = next;
    }
  g_closes[session]++;
  g_close_log.push_back (session);
}

static DB_SESSION *
mk (bool subsession)
{
  DB_SESSION *s = (DB_SESSION *) calloc (1, sizeof (DB_SESSION));
  s->is_subsession_for_prepared = subsession;
  return s;
}

/* an EXECUTE statement whose prepared text is `text` (PT_VALUE of PT_TYPE_CHAR, as the function asserts) */
static PT_NODE *
mk_execute (const char *text)
{
  PT_NODE *stmt = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  PT_NODE *query = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  size_t len = strlen (text);
  PARSER_VARCHAR *str = (PARSER_VARCHAR *) calloc (1, sizeof (PARSER_VARCHAR) + len + 1);
  str->length = (int) len;
  memcpy (str->bytes, text, len + 1);
  query->node_type = PT_VALUE;
  query->type_enum = PT_TYPE_CHAR;
  query->info.value.data_value.str = str;
  stmt->node_type = PT_EXECUTE_PREPARE;
  stmt->info.execute.query = query;
  return stmt;
}

static void
reset (void)
{
  g_closes.clear ();
  g_close_log.clear ();
}

TEST_CASE ("R1 the subsessions earlier executions left on a client session's chain are closed once each, in chain "
	   "order, before this execution opens its own", "[contract]")
{
  reset ();
  DB_SESSION *client = mk (false);
  DB_SESSION *a = mk (true), *b = mk (true), *c = mk (true);
  client->next = a;
  a->next = b;
  b->next = c;

  CHECK (unitseam_recompile_until_open (client, mk_execute ("select 1")));
  CHECK (client->next == NULL);
  CHECK (g_closes[a] == 1);
  CHECK (g_closes[b] == 1);
  CHECK (g_closes[c] == 1);
  CHECK (g_closes[client] == 0);
  REQUIRE (g_close_log.size () == 3);
  CHECK (g_close_log[0] == a);
  CHECK (g_close_log[1] == b);
  CHECK (g_close_log[2] == c);
}

TEST_CASE ("R2 a session that is itself a subsession (a kept tree: its next links the kept-tree list) keeps its "
	   "chain: nothing is closed", "[contract]")
{
  reset ();
  DB_SESSION *kept = mk (true);
  DB_SESSION *k2 = mk (true), *k3 = mk (true);
  kept->next = k2;
  k2->next = k3;

  CHECK (unitseam_recompile_until_open (kept, mk_execute ("select 1")));
  CHECK (kept->next == k2);
  CHECK (k2->next == k3);
  CHECK (g_close_log.empty ());
}

TEST_CASE ("R3 a client session with no earlier subsession closes nothing and goes on to open this execution's",
	   "[contract]")
{
  reset ();
  DB_SESSION *client = mk (false);

  CHECK (unitseam_recompile_until_open (client, mk_execute ("select 1")));
  CHECK (client->next == NULL);
  CHECK (g_close_log.empty ());
}

TEST_CASE ("R4 executed again and again, each execution closes what the one before it left: the chain never holds "
	   "more than one execution's subsessions, and each is closed exactly once", "[contract]")
{
  reset ();
  DB_SESSION *client = mk (false);
  std::vector<DB_SESSION *> left;

  for (int i = 0; i < 50; i++)
    {
      CHECK (unitseam_recompile_until_open (client, mk_execute ("select 1")));
      CHECK (client->next == NULL);
      /* what this execution would hang on the chain: its subsession and, for a second compilation of the signature,
       * the first one (bind_domains_differ) */
      DB_SESSION *first = mk (true), *second = mk (true);
      second->next = first;
      client->next = second;
      left.push_back (first);
      left.push_back (second);
    }
  /* the 50th execution's two are still on the chain; every earlier one closed once */
  for (size_t k = 0; k + 2 < left.size (); k++)
    {
      CHECK (g_closes[left[k]] == 1);
    }
  CHECK (g_closes[left[left.size () - 1]] == 0);
  CHECK (g_closes[left[left.size () - 2]] == 0);
  CHECK (g_close_log.size () == 98);
}
