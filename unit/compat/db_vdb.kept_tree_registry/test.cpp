/*
 * Contract suite -- the client's registry of kept SQL-level prepared trees in src/compat/db_vdb.c (CS_MODE):
 * db_Kept_trees and db_prepared_tree_unlink / _close / _remove / _register / _free_all, plus
 *   PR#8314 (CBRD-27231) head: db_prepared_tree_take (checked out = off the list) / db_prepared_tree_put_back,
 *                              the list CUB_THREAD_LOCAL
 *   before it (base cbe8d181b): db_prepared_tree_lookup (re-linked at the front -- stays on the list), one list
 * Real code: every registry function above (through hooks.cpp, appended to the TU).
 * Fakes (each copies the branch of the real function this path takes):
 *   db_close_session_local (seam, db_vdb.c:5782) -- the real one closes session->signature_plans (each recursively),
 *       then the transient `next` chain (assert is_subsession_for_prepared), then frees the statements and the parser.
 *       The fake walks the same two chains in the same order and counts closes per session instead of freeing, so a
 *       session closed twice or closed while it is executing is visible. Sessions stay in the test's pool.
 *   db_get_session_id (db_admin.c:3446) -- returns db_Session_id, which is CUB_THREAD_LOCAL: the fake returns a
 *       thread_local the test sets (a "server session change" = a new value + db_prepared_tree_free_all, as
 *       db_set_session_id does).
 *   pt_host_vars_match_signature (parse_dbi.c:3429) -- false when parser->host_var_compiled_domains is NULL or
 *       count < host_var_count (verbatim); else "the values' bind type signature is the one this parser was compiled
 *       for": the test gives each compiled parser and each host-variable array a signature number and compares them.
 * The EXECUTE choreography (checkout / return / stale) is hooks.cpp unitseam_exec_*, copied line by line from each
 * revision's db_execute_and_keep_statement_local -- not the real caller.
 *
 * Tags: [contract] = the registry's contract (a FAIL at head is a regression candidate);
 *       [defect]   = a reproduction (PASS = the condition is present).
 * Names that say "(head design)" assert the behaviour PR#8314 chose and documents; a base FAIL there is the change.
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
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

/* ---- appendix hooks (compiled inside db_vdb.c) ---- */
DB_SESSION *unitseam_take (const char *name, const char *text, bool *present);
void unitseam_put_back (DB_SESSION * kept, bool *present);
int unitseam_shape (void);
DB_SESSION *unitseam_exec_checkout (const char *name, const char *text);
void unitseam_exec_return_ok (DB_SESSION * kept);
void unitseam_exec_stale (DB_SESSION * kept, const char *name, bool signature_miss);
DB_SESSION *unitseam_unlink (const char *name);
void unitseam_close (DB_SESSION * kept);
void unitseam_remove (const char *name);
bool unitseam_register (DB_SESSION * owner, const char *name, DB_SESSION * subsession);
DB_SESSION *unitseam_list_head (void);
void unitseam_list_forget (void);

#define ABSENT_BEFORE_8314(what) FAIL (what " absent at this revision (before PR#8314)")

/* ---------------------------------------------------------------- fake sessions */

struct fake_sess
{
  DB_SESSION *s;
  int closes;
  int sig;			/* 0 = no compiled bind domains */
  int id;			/* creation order, for messages */
};

static std::vector<fake_sess *> g_pool;
static std::map<const DB_SESSION *, fake_sess *> g_by_sess;
static std::map<const void *, int> g_sig_of;	/* parser or host-variable array -> signature */
static std::vector<int> g_close_log;		/* fake ids in close order */
static thread_local SESSION_ID g_session_id = 7;
static TP_DOMAIN *g_dummy_domains[4];		/* a non-NULL host_var_compiled_domains */

static fake_sess *
F (const DB_SESSION * s)
{
  auto it = g_by_sess.find (s);
  return it == g_by_sess.end () ? nullptr : it->second;
}

/* a compiled subsession: parser over `text` (NULL = no original buffer), compiled for bind signature `sig` */
static fake_sess *
mk (const char *text, int sig = 0, bool subsession = true)
{
  fake_sess *f = new fake_sess ();
  f->s = (DB_SESSION *) calloc (1, sizeof (DB_SESSION));
  PARSER_CONTEXT *p = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
  DB_VALUE *hv = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
  p->original_buffer = text ? strdup (text) : NULL;
  p->host_var_count = 1;
  p->host_variables = hv;
  if (sig != 0)
    {
      p->host_var_compiled_domains = g_dummy_domains;
    }
  g_sig_of[p] = sig;
  g_sig_of[hv] = sig;
  f->s->parser = p;
  f->s->is_subsession_for_prepared = subsession;
  f->s->kept_session_id = DB_EMPTY_SESSION;
  f->sig = sig;
  f->id = (int) g_pool.size ();
  g_pool.push_back (f);
  g_by_sess[f->s] = f;
  return f;
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
  fake_sess *f = F (session);
  if (f != nullptr)
    {
      f->closes++;
      g_close_log.push_back (f->id);
    }
}

/* fake of db_admin.c db_get_session_id: db_Session_id is CUB_THREAD_LOCAL */
SESSION_ID
db_get_session_id (void)
{
  return g_session_id;
}

/* fake of parse_dbi.c pt_host_vars_match_signature: its NULL / count exit verbatim, else signature equality */
bool
pt_host_vars_match_signature (const PARSER_CONTEXT * parser, int count, const DB_VALUE * values)
{
  if (parser->host_var_compiled_domains == NULL || count < parser->host_var_count)
    {
      return false;
    }
  return g_sig_of[parser] == g_sig_of[values];
}

static void
reset (void)
{
  unitseam_list_forget ();	/* the sessions are the pool's; nothing to close */
  g_pool.clear ();		/* leaked on purpose: a closed session's memory stays readable for the checks */
  g_by_sess.clear ();
  g_sig_of.clear ();
  g_close_log.clear ();
  g_session_id = 7;
}

/* PREPARE ... EXECUTE: the subsession on the owner's transient chain, then registered under `name` */
static fake_sess *
keep (const char *name, const char *text, int sig = 0)
{
  fake_sess *owner = mk (text, 0, false);
  fake_sess *sub = mk (text, sig);
  owner->s->next = sub->s;
  bool ok = unitseam_register (owner->s, name, sub->s);
  REQUIRE (ok);
  return sub;
}

static std::vector<DB_SESSION *>
list (void)
{
  std::vector<DB_SESSION *> v;
  for (DB_SESSION * s = unitseam_list_head (); s != NULL && v.size () < 10000; s = s->next)
    {
      v.push_back (s);
    }
  return v;
}

static int
count_name (const char *name)
{
  int n = 0;
  for (DB_SESSION * s:list ())
    {
      n += (s->kept_stmt_name != NULL && strcmp (s->kept_stmt_name, name) == 0);
    }
  return n;
}

static bool
on_list (const fake_sess * f)
{
  for (DB_SESSION * s:list ())
    {
      if (s == f->s)
	{
	  return true;
	}
    }
  return false;
}

static int
chain_len (DB_SESSION * s)
{
  int n = 0;
  for (; s != NULL; s = s->next)
    {
      n++;
    }
  return n;
}

/* ---------------------------------------------------------------- register / remove / free_all */

TEST_CASE ("KT-R1 register: the subsession leaves the owner's transient chain and heads the list under a copied name "
	   "and the current server session", "[contract][register]")
{
  reset ();
  fake_sess *owner = mk ("select 1", 0, false);
  fake_sess *a = mk ("a"), *sub = mk ("select 1"), *b = mk ("b");
  owner->s->next = a->s;
  a->s->next = sub->s;
  sub->s->next = b->s;
  const char name[] = "p1";
  g_session_id = 41;

  REQUIRE (unitseam_register (owner->s, name, sub->s));
  CHECK (owner->s->next == a->s);
  CHECK (a->s->next == b->s);	/* the rest of the transient chain stays */
  CHECK (b->s->next == NULL);
  REQUIRE (list ().size () == 1);
  CHECK (unitseam_list_head () == sub->s);
  CHECK (sub->s->next == NULL);
  REQUIRE (sub->s->kept_stmt_name != NULL);
  CHECK (sub->s->kept_stmt_name != name);
  CHECK (std::string (sub->s->kept_stmt_name) == "p1");
  CHECK (sub->s->kept_session_id == 41);
  CHECK (g_close_log.empty ());
}

TEST_CASE ("KT-R2 register of a kept name without compiled bind domains closes the earlier tree once; one entry per name",
	   "[contract][register]")
{
  reset ();
  fake_sess *old = keep ("p", "select 1");
  fake_sess *other = keep ("q", "select 2");
  fake_sess *nw = keep ("p", "select 1");

  CHECK (old->closes == 1);
  CHECK (nw->closes == 0);
  CHECK (other->closes == 0);
  CHECK (count_name ("p") == 1);
  CHECK (count_name ("q") == 1);
  CHECK (on_list (nw));
  CHECK_FALSE (on_list (old));
}

TEST_CASE ("KT-R3 register of a kept name for another bind type signature keeps the earlier tree as the new one's "
	   "signature plan, unnamed and open", "[contract][register][signature]")
{
  reset ();
  fake_sess *s1 = keep ("p", "select ?", 1);
  fake_sess *s2 = keep ("p", "select ?", 2);

  CHECK (s1->closes == 0);
  CHECK (s2->s->signature_plans == s1->s);
  CHECK (s1->s->kept_stmt_name == NULL);
  CHECK (count_name ("p") == 1);
  CHECK (on_list (s2));
  CHECK_FALSE (on_list (s1));
}

TEST_CASE ("KT-R4 register of a kept name for the same signature closes the earlier tree", "[contract][register][signature]")
{
  reset ();
  fake_sess *s1 = keep ("p", "select ?", 1);
  fake_sess *s1b = keep ("p", "select ?", 1);

  CHECK (s1->closes == 1);
  CHECK (s1b->s->signature_plans == NULL);
  CHECK (count_name ("p") == 1);
}

TEST_CASE ("KT-R5 the signature plans kept beside a name stop at DB_MAX_SIGNATURE_PLANS (16): the oldest is closed",
	   "[contract][register][signature]")
{
  reset ();
  std::vector<fake_sess *> v;
  for (int sig = 1; sig <= 18; sig++)
    {
      v.push_back (keep ("p", "select ?", sig));
    }
  CHECK (count_name ("p") == 1);
  CHECK (unitseam_list_head () == v[17]->s);
  CHECK (chain_len (v[17]->s->signature_plans) == 16);
  CHECK (v[0]->closes == 1);
  for (int i = 1; i < 18; i++)
    {
      INFO ("signature " << i + 1);
      CHECK (v[i]->closes == 0);
    }
}

TEST_CASE ("KT-R6 remove closes the named entry once and leaves the others; an unknown name is a no-op",
	   "[contract][remove]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  fake_sess *q = keep ("q", "select 2");
  fake_sess *r = keep ("r", "select 3");

  unitseam_remove ("q");
  CHECK (q->closes == 1);
  CHECK (p->closes == 0);
  CHECK (r->closes == 0);
  CHECK (list ().size () == 2);
  CHECK (count_name ("q") == 0);

  unitseam_remove ("nope");
  unitseam_remove ("q");
  CHECK (list ().size () == 2);
  CHECK (q->closes == 1);
}

TEST_CASE ("KT-R7 free_all closes every entry and its signature plans exactly once and empties the list",
	   "[contract][free_all]")
{
  reset ();
  fake_sess *p1 = keep ("p", "select ?", 1);
  fake_sess *p2 = keep ("p", "select ?", 2);
  fake_sess *q = keep ("q", "select 2");
  fake_sess *r = keep ("r", "select 3");

  db_prepared_tree_free_all ();
  CHECK (list ().empty ());
  CHECK (p1->closes == 1);
  CHECK (p2->closes == 1);
  CHECK (q->closes == 1);
  CHECK (r->closes == 1);
  db_prepared_tree_free_all ();	/* idempotent on an empty list */
  CHECK (g_close_log.size () == 4);
}

/* ---------------------------------------------------------------- the EXECUTE checkout (take at head, lookup at base) */

TEST_CASE ("KT-C1 checkout of an entry of this server session and this text returns it open", "[contract][checkout]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  keep ("q", "select 2");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  CHECK (k == p->s);
  CHECK (p->closes == 0);
  CHECK (count_name ("q") == 1);
}

TEST_CASE ("KT-C2 checkout of an entry kept in another server session closes it and returns NULL",
	   "[contract][checkout]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  g_session_id = 8;		/* another CAS's server session, or a reconnect */

  CHECK (unitseam_exec_checkout ("p", "select 1") == NULL);
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

TEST_CASE ("KT-C3 checkout with another statement text (the name was prepared again) closes the entry and returns NULL",
	   "[contract][checkout]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  CHECK (unitseam_exec_checkout ("p", "select 2") == NULL);
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

TEST_CASE ("KT-C4 checkout of an entry whose parser has no original buffer closes it and returns NULL",
	   "[contract][checkout]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  free ((void *) p->s->parser->original_buffer);
  p->s->parser->original_buffer = NULL;

  CHECK (unitseam_exec_checkout ("p", "select 1") == NULL);
  CHECK (p->closes == 1);
}

TEST_CASE ("KT-C5 checkout of a name with no entry returns NULL and touches nothing", "[contract][checkout]")
{
  reset ();
  fake_sess *q = keep ("q", "select 2");

  CHECK (unitseam_exec_checkout ("p", "select 1") == NULL);
  CHECK (q->closes == 0);
  CHECK (list ().size () == 1);
}

TEST_CASE ("KT-C6 the name match is exact: a prefix or another case is another name", "[contract][checkout]")
{
  reset ();
  fake_sess *p = keep ("p1", "select 1");

  CHECK (unitseam_exec_checkout ("p", "select 1") == NULL);
  CHECK (unitseam_exec_checkout ("P1", "select 1") == NULL);
  CHECK (unitseam_exec_checkout ("p10", "select 1") == NULL);
  CHECK (p->closes == 0);
  CHECK (count_name ("p1") == 1);
}

/* ---------------------------------------------------------------- while it executes (nested SQL) */

TEST_CASE ("KT-X1 a tree checked out for execution is off the list until it returns", "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  CHECK_FALSE (on_list (p));
  CHECK (count_name ("p") == 0);
  unitseam_exec_return_ok (k);
  CHECK (on_list (p));
  CHECK (count_name ("p") == 1);
  CHECK (p->closes == 0);
}

TEST_CASE ("KT-X2 DEALLOCATE / re-PREPARE of the name nested in its execution does not close the executing tree",
	   "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_remove ("p");	/* do_process_deallocate_prepare / do_process_prepare_statement, nested */
  CHECK (p->closes == 0);
  unitseam_exec_return_ok (k);
}

TEST_CASE ("KT-X3 an EXECUTE of the name nested in its execution that keeps a tree does not close the executing tree",
	   "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  fake_sess *nested = keep ("p", "select 1");	/* nested EXECUTE p: no tree to take -> compiles and registers */
  CHECK (p->closes == 0);
  CHECK (nested->closes == 0);
  unitseam_exec_return_ok (k);
}

TEST_CASE ("KT-X4 after a nested EXECUTE kept a tree under the name, the newer tree is the name's one entry and the "
	   "executed one is closed exactly once", "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  fake_sess *nested = keep ("p", "select 1");
  unitseam_exec_return_ok (k);

  CHECK (count_name ("p") == 1);
  CHECK (on_list (nested));
  CHECK (nested->closes == 0);
  CHECK (p->closes == 1);
}

TEST_CASE ("KT-X5 (head design) a name deallocated by nested SQL comes back with the executed tree when it returns",
	   "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_remove ("p");
  unitseam_exec_return_ok (k);
  CHECK (on_list (p));
  CHECK (p->closes == 0);
}

TEST_CASE ("KT-X6 nested DEALLOCATE, return, then PREPARE of the name: the tree is closed exactly once and the name "
	   "has no entry", "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_remove ("p");
  unitseam_exec_return_ok (k);
  unitseam_remove ("p");	/* PREPARE p FROM ... after the statement */
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

TEST_CASE ("KT-X7 nested DEALLOCATE + PREPARE of the name with another text: the next EXECUTE gets no tree and the old "
	   "one is closed exactly once", "[contract][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_remove ("p");	/* DEALLOCATE p */
  unitseam_remove ("p");	/* PREPARE p FROM 'select 2' */
  unitseam_exec_return_ok (k);

  CHECK (unitseam_exec_checkout ("p", "select 2") == NULL);
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

TEST_CASE ("PR8314 U1: (latent) a re-PREPARE of the name with the same text nested in its own EXECUTE does not "
	   "release the kept tree -- the next EXECUTE reuses the tree compiled before that PREPARE",
	   "[defect][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_remove ("p");	/* nested PREPARE p FROM 'select 1': "re-PREPARE of the same name invalidates the tree" */
  unitseam_exec_return_ok (k);

  DB_SESSION *next = unitseam_exec_checkout ("p", "select 1");	/* the next EXECUTE p */
  CHECK (next == p->s);
  CHECK (p->closes == 0);
}

/* ---------------------------------------------------------------- put_back (head API) */

TEST_CASE ("KT-P1 put_back of a name with no entry puts the tree at the front, open", "[contract][put_back]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  keep ("q", "select 2");
  bool present = false;

  DB_SESSION *k = unitseam_take ("p", "select 1", &present);
  if (!present)
    {
      ABSENT_BEFORE_8314 ("db_prepared_tree_take");
    }
  REQUIRE (k == p->s);
  unitseam_put_back (k, &present);
  CHECK (unitseam_list_head () == p->s);
  CHECK (p->closes == 0);
  CHECK (count_name ("p") == 1);
  CHECK (list ().size () == 2);
}

TEST_CASE ("KT-P2 put_back of a name that has an entry closes the returned tree once and leaves the entry",
	   "[contract][put_back]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  bool present = false;

  DB_SESSION *k = unitseam_take ("p", "select 1", &present);
  if (!present)
    {
      ABSENT_BEFORE_8314 ("db_prepared_tree_take");
    }
  REQUIRE (k == p->s);
  fake_sess *nw = keep ("p", "select 1");
  unitseam_put_back (k, &present);
  CHECK (p->closes == 1);
  CHECK (nw->closes == 0);
  CHECK (count_name ("p") == 1);
  CHECK (on_list (nw));
}

TEST_CASE ("KT-P3 (head design) a put_back that loses to a newer entry closes the returned tree's other-signature "
	   "plans with it -- they are not carried to the newer entry", "[contract][put_back][signature]")
{
  reset ();
  fake_sess *s1 = keep ("p", "select ?", 1);
  fake_sess *s2 = keep ("p", "select ?", 2);	/* s2 heads, s1 its signature plan */
  bool present = false;

  DB_SESSION *k = unitseam_take ("p", "select ?", &present);
  if (!present)
    {
      ABSENT_BEFORE_8314 ("db_prepared_tree_take");
    }
  REQUIRE (k == s2->s);
  fake_sess *s3 = keep ("p", "select ?", 3);	/* nested EXECUTE with a third signature */
  CHECK (s3->s->signature_plans == NULL);	/* s2 was off the list: nothing to chain */
  unitseam_put_back (k, &present);
  CHECK (s2->closes == 1);
  CHECK (s1->closes == 1);
  CHECK (s3->closes == 0);
  CHECK (count_name ("p") == 1);
}

/* ---------------------------------------------------------------- stale kept tree */

TEST_CASE ("KT-S1 a stale kept tree (no signature miss) is closed exactly once and the name has no entry",
	   "[contract][stale]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  unitseam_exec_stale (k, "p", false);
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

TEST_CASE ("KT-S2 discarding a stale kept tree does not close a tree nested SQL kept under the name meanwhile",
	   "[contract][stale]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  fake_sess *nested = keep ("p", "select 1");
  unitseam_exec_stale (k, "p", false);
  CHECK (nested->closes == 0);
  CHECK (on_list (nested));
  CHECK (p->closes == 1);
}

TEST_CASE ("KT-S3 a signature miss keeps the tree for its own signature: back on the list open, and the fresh "
	   "compilation of the other signature chains it", "[contract][stale][signature]")
{
  reset ();
  fake_sess *s1 = keep ("p", "select ?", 1);

  DB_SESSION *k = unitseam_exec_checkout ("p", "select ?");
  REQUIRE (k == s1->s);
  unitseam_exec_stale (k, "p", true);
  CHECK (on_list (s1));
  CHECK (s1->closes == 0);
  fake_sess *s2 = keep ("p", "select ?", 2);	/* the compilation from the text registers */
  CHECK (s2->s->signature_plans == s1->s);
  CHECK (s1->closes == 0);
  CHECK (count_name ("p") == 1);
}

/* ---------------------------------------------------------------- free_all and the server session */

TEST_CASE ("KT-F1 free_all while a tree executes does not close the executing tree", "[contract][free_all][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");
  fake_sess *q = keep ("q", "select 2");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  db_prepared_tree_free_all ();
  CHECK (p->closes == 0);
  CHECK (q->closes == 1);
  unitseam_exec_return_ok (k);
}

TEST_CASE ("KT-F2 a server session change during an execution: the returned tree serves no EXECUTE of the new session "
	   "and is closed exactly once", "[contract][free_all][execute]")
{
  reset ();
  fake_sess *p = keep ("p", "select 1");

  DB_SESSION *k = unitseam_exec_checkout ("p", "select 1");
  REQUIRE (k == p->s);
  g_session_id = 9;		/* db_set_session_id: new id + db_prepared_tree_free_all */
  db_prepared_tree_free_all ();
  unitseam_exec_return_ok (k);

  CHECK (unitseam_exec_checkout ("p", "select 1") == NULL);
  CHECK (p->closes == 1);
  CHECK (count_name ("p") == 0);
}

/* ---------------------------------------------------------------- threads
 * db_Kept_trees is matched by db_Session_id (db_get_session_id); both are CUB_THREAD_LOCAL at head, which is
 * THREAD_LOCAL only in CS_MODE with MULTI_CONN_TO_A_SERVER (db_multi_threads_connections.h, CMake option, default OFF)
 * and empty otherwise. The contract: the list has the scope of the session id it is matched by. */

static CUB_THREAD_LOCAL int g_scope_probe = 0;	/* the same specifier as db_Session_id (db.h) */

static bool
session_id_is_per_thread (void)
{
  g_scope_probe = 1;
  int seen = -1;
  std::thread t ([&] ()
		 {
		 seen = g_scope_probe;});
  t.join ();
  g_scope_probe = 0;
  return seen == 0;
}

TEST_CASE ("KT-T1 the list has the scope of db_Session_id: another thread sees this thread's entries exactly when the "
	   "session id is process-wide (CUB_THREAD_LOCAL empty, MULTI_CONN_TO_A_SERVER off)", "[contract][thread]")
{
  reset ();
  const bool per_thread = session_id_is_per_thread ();
  fake_sess *p = keep ("p", "select 1");
  DB_SESSION *seen_head = (DB_SESSION *) 1;

  std::thread t ([&] ()
		 {
		 seen_head = unitseam_list_head ();});
  t.join ();
  INFO ("CUB_THREAD_LOCAL in this build: " << (per_thread ? "thread-local" : "empty (process-wide)"));
  CHECK ((seen_head == NULL) == per_thread);
  CHECK (p->closes == 0);
}

TEST_CASE ("KT-T2 free_all on another thread releases this thread's entries exactly when the session id is process-wide",
	   "[contract][thread][free_all]")
{
  reset ();
  const bool per_thread = session_id_is_per_thread ();
  fake_sess *p = keep ("p", "select 1");

  std::thread t ([] ()
		 {
		 db_prepared_tree_free_all ();});
  t.join ();
  INFO ("CUB_THREAD_LOCAL in this build: " << (per_thread ? "thread-local" : "empty (process-wide)"));
  CHECK ((p->closes == 0) == per_thread);
  CHECK (on_list (p) == per_thread);
}

/* ---------------------------------------------------------------- invariants over random sequences */

namespace
{
  struct frame
  {
    std::string name;
    DB_SESSION *kept;
  };

  /* returns "" or the first broken invariant */
  std::string check_invariants (const std::vector<frame> &stack, const std::string &step)
  {
    std::set<std::string> names;
    std::set<const DB_SESSION *> seen;
    for (DB_SESSION * s:list ())
      {
	if (s->kept_stmt_name == NULL)
	  {
	    return step + ": an entry without a name";
	  }
	if (!names.insert (s->kept_stmt_name).second)
	  {
	    return step + ": two entries for " + s->kept_stmt_name;
	  }
	for (DB_SESSION * c = s; c != NULL; c = (c == s ? c->signature_plans : c->next))
	  {
	    fake_sess *f = F (c);
	    if (f != nullptr && f->closes > 0)
	      {
		return step + ": a closed session reachable from the list (" + s->kept_stmt_name + ")";
	      }
	    if (!seen.insert (c).second)
	      {
		return step + ": a session twice on the list";
	      }
	  }
      }
    for (const frame & fr:stack)
      {
	fake_sess *f = fr.kept ? F (fr.kept) : nullptr;
	if (f != nullptr && f->closes > 0)
	  {
	    return step + ": the tree executing under " + fr.name + " was closed";
	  }
      }
    for (fake_sess * f:g_pool)
      {
	if (f->closes > 1)
	  {
	    return step + ": a session closed " + std::to_string (f->closes) + " times";
	  }
      }
    return "";
  }
}

TEST_CASE ("KT-I1 random PREPARE / EXECUTE (nested up to 3) / DEALLOCATE / stale / session change sequences: one entry "
	   "per name, no closed session reachable from the list, nothing closed twice, no executing tree closed",
	   "[contract][invariant]")
{
  const char *names[] = { "p", "q", "r" };
  const char *texts[] = { "select ?", "select ? + 1" };
  int broken = 0;
  std::string first;

  for (unsigned seed = 1; seed <= 300 && broken == 0; seed++)
    {
      reset ();
      std::mt19937 rng (seed);
      std::map<std::string, int> text_of;	/* the server's text per name */
      std::vector<frame> stack;
      std::ostringstream trail;
      for (int step = 0; step < 80; step++)
	{
	  const std::string name = names[rng () % 3];
	  const int op = rng () % 7;
	  if (text_of.find (name) == text_of.end ())
	    {
	      text_of[name] = 0;
	    }
	  const char *text = texts[text_of[name]];
	  switch (op)
	    {
	    case 0:		/* PREPARE name (maybe another text) */
	      unitseam_remove (name.c_str ());
	      text_of[name] = rng () % 2;
	      trail << " prep(" << name << ")";
	      break;
	    case 1:		/* DEALLOCATE */
	      unitseam_remove (name.c_str ());
	      trail << " dealloc(" << name << ")";
	      break;
	    case 2:		/* EXECUTE begins (nested in the executing one, if any) */
	      if (stack.size () < 3)
		{
		  DB_SESSION *k = unitseam_exec_checkout (name.c_str (), text);
		  stack.push_back ({name, k});
		  trail << " exec(" << name << (k ? ",kept" : "") << ")";
		}
	      break;
	    case 3:		/* EXECUTE ends without error */
	    case 4:		/* EXECUTE ends: the kept tree is stale */
	      if (!stack.empty ())
		{
		  frame fr = stack.back ();
		  stack.pop_back ();
		  const bool miss = rng () % 2;
		  if (fr.kept != NULL && op == 3)
		    {
		      unitseam_exec_return_ok (fr.kept);
		    }
		  else
		    {
		      if (fr.kept != NULL)
			{
			  unitseam_exec_stale (fr.kept, fr.name.c_str (), miss);
			}
		      keep (fr.name.c_str (), texts[text_of[fr.name]], (int) (rng () % 3));	/* the fresh compile */
		    }
		  trail << " end(" << fr.name << (op == 3 ? ",ok" : miss ? ",miss" : ",stale") << ")";
		}
	      break;
	    case 5:		/* PREPARE + first EXECUTE outside any other */
	      if (stack.empty ())
		{
		  unitseam_remove (name.c_str ());
		  keep (name.c_str (), text, (int) (rng () % 3));
		  trail << " keep(" << name << ")";
		}
	      break;
	    case 6:		/* server session change */
	      if (rng () % 4 == 0)
		{
		  g_session_id++;
		  db_prepared_tree_free_all ();
		  trail << " session";
		}
	      break;
	    }
	  std::string why = check_invariants (stack, "seed " + std::to_string (seed) + " step " + std::to_string (step));
	  if (!why.empty ())
	    {
	      broken++;
	      first = why + " after" + trail.str ();
	      break;
	    }
	}
    }
  INFO (first);
  CHECK (broken == 0);
}
