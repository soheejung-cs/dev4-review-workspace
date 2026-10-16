/*
 * PR#8013 F2 -- db_bind_variant_switch () builds the variant key with pt_append_string (parser, NULL, buf)
 * on every switch to a different variant. The parser's string blocks are returned only by
 * parser_free_parser (pt_free_string_blocks), so a prepared statement whose executions alternate
 * between two variants grows the parser arena by one key per switch.
 *
 * Unit under test: src/compat/db_vdb.c (CS_MODE) -- db_bind_variant_step / db_bind_variant_switch,
 * reached through appendix hooks. Fakes below stand in for other TUs; each mimics the branch of the
 * real function that this path takes (see the comment on each).
 */
#include "catch.hpp"
#include "config.h"
#include "parser.h"
#include "parse_tree.h"
#include "histogram_cl.hpp"
#include "bind_variant.h"
#include "xasl.h"
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

/* ---- appendix hooks (compiled inside db_vdb.c) ---- */
int unitseam_vdb_switch (PARSER_CONTEXT * parser, PT_NODE * statement, BIND_WATCH_STATE * ws, int variant, bool compile);
int unitseam_vdb_step (PARSER_CONTEXT * parser, PT_NODE * statement);
const char *unitseam_vdb_key (PT_NODE * statement);

/* ---- the parser string arena, as seen by the test ---- */
static std::vector<char *> g_arena;	/* every string pt_append_string handed out, freed only at "parser free" */
static size_t g_arena_bytes;		/* bytes those strings occupy in the string blocks (strlen + 1 each) */
static int g_replans;			/* do_replan_statement_internal calls (prepare round trips) */
static UINT64 g_value_hash;		/* what histogram_bind_value_hash returns for "the bound values" */
static double g_rows;			/* the one priced predicate's expected rows under "the bound values" */

/*
 * fake of parse_tree.c pt_append_string, branch old_string == NULL (the only one db_bind_variant_key uses):
 * parser_allocate_string_buffer (parser, strlen (new_tail), 1) + strcpy. The real buffer is carved from the
 * parser's string blocks (last_string_start = last_string_end + 1, i.e. strlen + 1 bytes per string) and is
 * never handed back before parser_free_parser. new_tail == NULL returns old_string; old_string != NULL
 * concatenates into a new string (pt_append_string_for, copy branch).
 */
char *
pt_append_string (const PARSER_CONTEXT * parser, const char *old_string, const char *new_tail)
{
  (void) parser;
  if (new_tail == NULL)
    {
      return (char *) old_string;
    }
  size_t n = (old_string ? strlen (old_string) : 0) + strlen (new_tail);
  char *s = (char *) malloc (n + 1);
  s[0] = '\0';
  if (old_string)
    {
      strcpy (s, old_string);
    }
  strcat (s, new_tail);
  g_arena.push_back (s);
  g_arena_bytes += n + 1;
  return s;
}

/* fake of parser_support.c pt_free_statement_xasl_id, non-DELETE branch: free_and_init (statement->xasl_id) */
void
pt_free_statement_xasl_id (PT_NODE * statement)
{
  if (statement != NULL && statement->xasl_id != NULL)
    {
      free (statement->xasl_id);
      statement->xasl_id = NULL;
    }
}

/*
 * seam: db_vdb.c do_replan_statement_internal, success branch: free the statement's XASL_ID, then
 * do_prepare_statement () leaves a fresh XASL_ID -- the cache entry named by the hash text plus the
 * variant key now on the statement (do_append_bind_variant_key). The fake names the entry by the key:
 * time_stored.usec = base usec + 1 + variant, base entry (no key) = base usec. The real prepare also
 * prints the hash text into the parser arena; that is not counted here (it is not line 402).
 */
int
do_replan_statement_internal (PARSER_CONTEXT * parser, PT_NODE * statement, bool force_compile)
{
  (void) parser;
  (void) force_compile;
  g_replans++;
  if (statement->xasl_id != NULL)
    {
      pt_free_statement_xasl_id (statement);
    }
  XASL_ID *id = (XASL_ID *) calloc (1, sizeof (XASL_ID));
  const char *key = unitseam_vdb_key (statement);
  int variant = -1;
  if (key != NULL)
    {
      const char *h = strrchr (key, '#');
      variant = h ? atoi (h + 1) : -1;
    }
  id->time_stored.sec = 1760000000;
  id->time_stored.usec = 123456 + 1 + variant;
  statement->xasl_id = id;
  return NO_ERROR;
}

/* fake of histogram_cl.cpp histogram_bind_value_hash: the hash of the bound values (0 = none) */
UINT64
histogram_bind_value_hash (PARSER_CONTEXT * parser)
{
  (void) parser;
  return g_value_hash;
}

/*
 * fake of histogram_cl.cpp histogram_bind_fingerprint, branch "the statement has one histogram-backed
 * predicate (col op ?)": terms = 1, rows[0] = its expected rows under the bound values, return true.
 */
bool
histogram_bind_fingerprint (PARSER_CONTEXT * parser, PT_NODE * statement, BIND_FINGERPRINT * fp,
			    char (*names)[BIND_WATCH_NAME_LEN])
{
  (void) parser;
  (void) statement;
  memset (fp, 0, sizeof (*fp));
  fp->terms = 1;
  fp->rows[0] = g_rows;
  if (names != NULL)
    {
      strcpy (names[0], "t.c <");
    }
  return true;
}

/* ---- fixture ---- */
struct stmt_fixture
{
  PARSER_CONTEXT *parser;
  PT_NODE *stmt;
  BIND_WATCH_STATE *ws;
};

static void
arena_reset ()		/* parser_free_parser: the string blocks go back */
{
  for (char *s:g_arena)
    {
      free (s);
    }
  g_arena.clear ();
  g_arena_bytes = 0;
  g_replans = 0;
}

static stmt_fixture
fresh_select ()
{
  arena_reset ();
  stmt_fixture f;
  f.parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
  f.stmt = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  f.stmt->node_type = PT_SELECT;
  f.ws = (BIND_WATCH_STATE *) calloc (1, sizeof (BIND_WATCH_STATE));
  f.ws->terms = -1;
  f.ws->state = BIND_VARIANT_STATE_WATCH;
  f.ws->cur_variant = -1;
  f.ws->base_known = true;
  f.ws->base_time.sec = 1760000000;
  f.ws->base_time.usec = 123456;
  f.stmt->info.query.bind_watch = f.ws;
  /* the base plan in hand */
  f.stmt->xasl_id = (XASL_ID *) calloc (1, sizeof (XASL_ID));
  f.stmt->xasl_id->time_stored = f.ws->base_time;
  return f;
}

/* what the client holds after learning two plans: variant 0 planned at 1e4 rows, variant 1 at 1e6
 * (both compiled through switch(compile=true) -> remembered XASL_IDs), query done learning (SELECT). */
static void
learn_two (stmt_fixture & f)
{
  REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, 0, true) == NO_ERROR);
  REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, 1, true) == NO_ERROR);
  REQUIRE (f.ws->id_known[0]);
  REQUIRE (f.ws->id_known[1]);
  f.ws->state = BIND_VARIANT_STATE_SELECT;
  f.ws->n_records = 2;
  f.ws->records[0].variant = 0;
  f.ws->records[0].fp.terms = 1;
  f.ws->records[0].fp.rows[0] = 1e4;
  f.ws->records[1].variant = 1;
  f.ws->records[1].fp.terms = 1;
  f.ws->records[1].fp.rows[0] = 1e6;
}

/* one execution of the prepared statement with new bound values (db_execute_and_keep_statement_local
 * calls db_bind_variant_step before do_execute_statement) */
static void
execute (stmt_fixture & f, UINT64 hash, double rows)
{
  g_value_hash = hash;
  g_rows = rows;
  REQUIRE (unitseam_vdb_step (f.parser, f.stmt) == NO_ERROR);
  REQUIRE (f.stmt->xasl_id != NULL);
}

static const int N = 1000;

TEST_CASE ("F2 control: executions whose values stay with one variant add no key allocation", "[contract]")
{
  stmt_fixture f = fresh_select ();
  learn_two (f);
  execute (f, 1, 1e4);		/* move to variant 0 once */
  REQUIRE (f.ws->cur_variant == 0);
  size_t keys0 = g_arena.size ();
  int replans0 = g_replans;

  for (int i = 0; i < N; i++)
    {
      /* different values each time (hash changes), all nearest to variant 0 */
      execute (f, (i % 2) ? 1 : 2, (i % 2) ? 1e4 : 1.2e4);
      REQUIRE (f.ws->cur_variant == 0);
      REQUIRE (f.stmt->xasl_id->time_stored.usec == 123456 + 1 + 0);
    }
  CHECK (g_arena.size () - keys0 == 0);
  CHECK (g_replans - replans0 == 0);
}

TEST_CASE ("F2 control: switch to the variant in hand returns before building a key; base (-1) builds none",
	   "[contract]")
{
  stmt_fixture f = fresh_select ();
  learn_two (f);
  REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, 0, false) == NO_ERROR);
  size_t keys0 = g_arena.size ();
  for (int i = 0; i < N; i++)
    {
      REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, 0, false) == NO_ERROR);
    }
  CHECK (g_arena.size () - keys0 == 0);

  /* to the base entry: the key slot is cleared, nothing allocated */
  keys0 = g_arena.size ();
  REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, -1, false) == NO_ERROR);
  CHECK (unitseam_vdb_key (f.stmt) == NULL);
  CHECK (g_arena.size () - keys0 == 0);
}

TEST_CASE ("F2: executions alternating between two variants (A/B) allocate a new key in the parser arena each time "
	   "(PASS = defect present)", "[defect]")
{
  stmt_fixture f = fresh_select ();
  learn_two (f);
  execute (f, 1, 1e4);		/* at variant 0 */
  REQUIRE (f.ws->cur_variant == 0);
  std::string key0 = unitseam_vdb_key (f.stmt);
  size_t keys0 = g_arena.size ();
  size_t bytes0 = g_arena_bytes;
  int replans0 = g_replans;
  std::set < const char *>ptrs;

  for (int i = 0; i < N; i++)
    {
      bool to_b = (i % 2) == 0;
      execute (f, to_b ? 2 : 1, to_b ? 1e6 : 1e4);
      REQUIRE (f.ws->cur_variant == (to_b ? 1 : 0));
      /* the plan in hand is the remembered one of that variant: no prepare round trip */
      REQUIRE (f.stmt->xasl_id->time_stored.usec == 123456 + 1 + (to_b ? 1 : 0));
      ptrs.insert (unitseam_vdb_key (f.stmt));
      if (!to_b)
	{
	  /* the same text every time -- only the copy is new */
	  REQUIRE (key0 == unitseam_vdb_key (f.stmt));
	}
    }
  CHECK (g_replans - replans0 == 0);			/* every switch took the id_known path */
  CHECK (g_arena.size () - keys0 == (size_t) N);	/* ... and still built a key: one per switch */
  CHECK (ptrs.size () == (size_t) N);			/* N distinct live copies of 2 key texts */
  CHECK (g_arena_bytes - bytes0 == (size_t) N * (key0.size () + 1));
  WARN ("key text '" << key0 << "' = " << key0.size () + 1 << " B; " << N << " alternations held "
	<< (g_arena_bytes - bytes0) << " B of parser arena until parser_free_parser");
}

TEST_CASE ("F2: db_bind_variant_switch alternating 0/1 builds a key on every call -- line 398 skips only the same "
	   "variant (PASS = defect present)", "[defect]")
{
  stmt_fixture f = fresh_select ();
  learn_two (f);
  size_t keys0 = g_arena.size ();
  int replans0 = g_replans;
  for (int i = 0; i < N; i++)
    {
      REQUIRE (unitseam_vdb_switch (f.parser, f.stmt, f.ws, i % 2, false) == NO_ERROR);
    }
  CHECK (g_replans - replans0 == 0);
  CHECK (g_arena.size () - keys0 == (size_t) N);
}
