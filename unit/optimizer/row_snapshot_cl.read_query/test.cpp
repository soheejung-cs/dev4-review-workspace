/*
 * PR#8242 (CBRD-27478) -- row_snapshot_read (src/optimizer/histogram/row_snapshot_cl.cpp, cs mode): the read
 * the optimizer makes while it plans, built from a node's constant filters.
 *
 *   query     SELECT [col], ... FROM [class] WHERE [f1] IN (?, ...) AND [f2] IN (?) ... LIMIT ?
 *             binds = every filter value in order, then max_rows + 1
 *   gates     no filter column leads a usable index (INDEX / UNIQUE / PK / FK, normal status, not a function
 *             or filtered index) and no hinted scan -> no read; the rows the read goes through
 *             (n_card x the smallest selectivity of an indexed filter, n_card for a hinted scan) past
 *             10 x max_rows (at least 1000 for a hinted scan) -> no read
 *   rows      0 rows or more than max_rows -> NULL; up to max_rows -> the snapshot (n_attrs x n_rows)
 *   state     during the read: optimization level 1, no plan dump, no lock wait, no trace, the statement's
 *             XASL supp info detached; afterwards every one of them, the trigger flag and the statement's
 *             error are back, on every exit
 *   errors    a deadlock abort or a lost server is reported in *error and left set; any other failure
 *             of the read only means "no snapshot" (*error NO_ERROR, the statement's error untouched)
 *   nested    a read started while a read runs (planning the read itself) returns NULL at once
 *
 * The TU is real; everything it calls in other TUs is faked here, each after the real function's branch
 * (suite.json "fakes"): the client session API (db_open_buffer_local ... db_close_session_local) plays a
 * server whose rows the case sets, the error stack follows error_context.cpp (push = new empty level,
 * pop = drop the top, pop_and_keep_error = move the top's error down), sm_ch_name is its body,
 * qo_get/set_optimization_param keep the level, tran_reset_wait_times sets tm_Tran_wait_msecs and can fail
 * like log_reset_wait_msecs, pt_detach/attach_xasl_supp_info hand a token back and forth.
 * Every case is [contract]; row_snapshot_cl.cpp is new in PR#8242 (no base comparison).
 */
#include "catch.hpp"

#include "row_snapshot_cl.hpp"
#include "class_object.h"
#include "db.h"
#include "db_query.h"
#include "db_session.h"
#include "dbtype.h"
#include "error_manager.h"
#include "execute_statement.h"
#include "object_primitive.h"
#include "optimizer.h"
#include "parser.h"
#include "schema_manager.h"
#include "transaction_cl.h"
#include "xasl_generation.h"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes: error stack (error_context.cpp) */

static std::vector<int> g_err_levels = { NO_ERROR };	/* [0] = base level, back () = current */
static int g_max_depth;

static void
set_err (int err)		/* er_set (..., err, ...) on the current level */
{
  g_err_levels.back () = err;
}

int
er_errid (void)
{
  return g_err_levels.back ();
}

void
er_clear (void)
{
  g_err_levels.back () = NO_ERROR;
}

void
er_stack_push (void)
{
  g_err_levels.push_back (NO_ERROR);
  g_max_depth = std::max (g_max_depth, (int) g_err_levels.size ());
}

void
er_stack_pop (void)
{
  REQUIRE (g_err_levels.size () > 1);	/* bad pop */
  g_err_levels.pop_back ();
}

void
er_stack_pop_and_keep_error (void)
{
  REQUIRE (g_err_levels.size () > 1);
  int top = g_err_levels.back ();
  g_err_levels.pop_back ();
  if (top != NO_ERROR)
    {
      g_err_levels.back () = top;
    }
}

/* ---------------------------------------------------------------- fakes: process state the read saves */

char *query_Plan_dump_filename;
FILE *query_Plan_dump_fp;
bool query_Plan_dump_fp_open;
CUB_THREAD_LOCAL bool do_Trigger_involved = false;
CUB_THREAD_LOCAL int tm_Tran_wait_msecs = -1;

static int g_level;		/* PRM_ID_OPTIMIZATION_LEVEL */

/* query_graph.c:289, QO_PARAM_LEVEL */
void
qo_get_optimization_param (void *retval, int param, ...)
{
  if (param == QO_PARAM_LEVEL)
    {
      *(int *) retval = g_level;
    }
}

/* query_graph.c:336, QO_PARAM_LEVEL */
void
qo_set_optimization_param (void *retval, int param, ...)
{
  va_list args;

  va_start (args, param);
  if (param == QO_PARAM_LEVEL)
    {
      if (retval)
	{
	  *(int *) retval = g_level;
	}
      g_level = va_arg (args, int);
    }
  va_end (args);
}

/* transaction_cl.c tran_reset_wait_times: set the client's wait, then log_reset_wait_msecs asks the
 * server, which can fail (an error set; the read checks er_errid, not the return) */
static int g_wait_fail_err;	/* the error the failing request sets */
static int g_wait_fail_on;	/* 1-based request that fails; 0 = none */
static std::vector<int> g_wait_calls;

int
tran_reset_wait_times (int wait_in_msecs)
{
  int old = tm_Tran_wait_msecs;

  g_wait_calls.push_back (wait_in_msecs);
  tm_Tran_wait_msecs = wait_in_msecs;	/* the client's value is set first, whatever the server says */
  if (g_wait_fail_on == (int) g_wait_calls.size ())
    {
      set_err (g_wait_fail_err);
      return -1;
    }
  return old;
}

/* xasl_generation.c pt_detach_xasl_supp_info / pt_attach_xasl_supp_info: the statement's gathered
 * classes go out of the way and come back; NULL (out of memory, er_set) when the case says so */
static int g_supp_token;
static bool g_supp_attached = true;
static bool g_detach_fails;
static int g_detaches, g_attaches;

struct xasl_supp_info *
pt_detach_xasl_supp_info (void)
{
  g_detaches++;
  if (g_detach_fails)
    {
      set_err (ER_OUT_OF_VIRTUAL_MEMORY);
      return NULL;
    }
  g_supp_attached = false;
  return (struct xasl_supp_info *) &g_supp_token;
}

void
pt_attach_xasl_supp_info (struct xasl_supp_info *saved)
{
  g_attaches++;
  CHECK (saved == (struct xasl_supp_info *) &g_supp_token);
  g_supp_attached = true;
}

/* schema_manager.c:5234, body */
const char *
sm_ch_name (const MOBJ clobj)
{
  return (clobj != NULL) ? ((SM_CLASS_HEADER *) clobj)->ch_name : NULL;
}

/* object_primitive.c: an INTEGER owns no memory */
int
pr_clear_value (DB_VALUE *var)
{
  if (var != NULL)
    {
      db_make_null (var);
    }
  return NO_ERROR;
}

/* ---------------------------------------------------------------- fakes: the client session API */

/* what the "server" does with the read */
struct server
{
  std::vector<std::vector<int>> rows;	/* rows that pass every filter, in the order it returns them */
  int compile_err = NO_ERROR;		/* db_compile_statement_local fails with it */
  int exec_err = NO_ERROR;		/* db_execute_statement_local fails with it (lock refusal, abort, ...) */
  int fetch_err_at = -1;		/* db_query_next_tuple fails at this cursor position */
  int fetch_err = NO_ERROR;
  bool nested = false;			/* execution plans another statement that reads too */
};
static server g_srv;

/* what the read did, as seen from the server */
struct seen
{
  int opens = 0, closes = 0, ends = 0;
  std::string query;
  std::vector<std::string> binds;	/* "i:<n>" or "null" */
  int level = -99, wait = -99;
  bool trace = true, dump_fp_null = false, dump_open = true, dump_name_null = false, supp_attached = true;
  int nested_result = -1;		/* 1 = the nested read returned NULL, 0 = it returned rows */
  int nested_error = -1;
};
static seen g_seen;

static int g_cursor;
static int g_result_token;

DB_SESSION *
db_open_buffer_local (const char *buffer)
{
  g_seen.opens++;
  g_seen.query = buffer;
  DB_SESSION *s = (DB_SESSION *) calloc (1, sizeof (DB_SESSION));
  s->parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
  s->parser->query_trace = true;	/* the session has trace on: the read must turn it off */
  return s;
}

int
db_compile_statement_local (DB_SESSION *session)
{
  (void) session;
  if (g_srv.compile_err != NO_ERROR)
    {
      set_err (g_srv.compile_err);
      return g_srv.compile_err;
    }
  return 1;
}

int
db_push_values (DB_SESSION *session, int count, DB_VALUE *in_values)
{
  (void) session;
  for (int i = 0; i < count; i++)
    {
      g_seen.binds.push_back (DB_IS_NULL (&in_values[i]) ? std::string ("null")
			      : "i:" + std::to_string (db_get_int (&in_values[i])));
    }
  return NO_ERROR;
}

/* forward: the nested read the server-side planning of the read would make */
static ROW_SNAPSHOT *read_again (int *error);

int
db_execute_statement_local (DB_SESSION *session, int stmt, DB_QUERY_RESULT **result)
{
  (void) stmt;
  g_seen.level = g_level;
  g_seen.wait = tm_Tran_wait_msecs;
  g_seen.trace = session->parser->query_trace;
  g_seen.dump_fp_null = (query_Plan_dump_fp == NULL);
  g_seen.dump_open = query_Plan_dump_fp_open;
  g_seen.dump_name_null = (query_Plan_dump_filename == NULL);
  g_seen.supp_attached = g_supp_attached;
  do_Trigger_involved = false;	/* executing a statement clears it */
  if (g_srv.nested)
    {
      int err = -1;
      ROW_SNAPSHOT *again = read_again (&err);
      g_seen.nested_result = (again == NULL) ? 1 : 0;
      g_seen.nested_error = err;
      row_snapshot_free (again);
    }
  if (g_srv.exec_err != NO_ERROR)
    {
      set_err (g_srv.exec_err);
      *result = NULL;
      return g_srv.exec_err;
    }
  *result = (DB_QUERY_RESULT *) &g_result_token;
  return (int) g_srv.rows.size ();
}

int
db_query_first_tuple (DB_QUERY_RESULT *result)
{
  (void) result;
  g_cursor = 0;
  return g_srv.rows.empty () ? DB_CURSOR_END : DB_CURSOR_SUCCESS;
}

int
db_query_next_tuple (DB_QUERY_RESULT *result)
{
  (void) result;
  g_cursor++;
  if (g_cursor == g_srv.fetch_err_at)
    {
      set_err (g_srv.fetch_err);
      return g_srv.fetch_err;
    }
  return (g_cursor < (int) g_srv.rows.size ()) ? DB_CURSOR_SUCCESS : DB_CURSOR_END;
}

int
db_query_get_tuple_value (DB_QUERY_RESULT *result, int tuple_index, DB_VALUE *value)
{
  (void) result;
  db_make_int (value, g_srv.rows[g_cursor][tuple_index]);
  return NO_ERROR;
}

int
db_query_end (DB_QUERY_RESULT *result)
{
  (void) result;
  g_seen.ends++;
  return NO_ERROR;
}

void
db_close_session_local (DB_SESSION *session)
{
  g_seen.closes++;
  free (session->parser);
  free (session);
}

/* ---------------------------------------------------------------- the class and its filters */

/* class t (a INT id 1, b INT id 2, c INT id 3, d INT id 4); constraints added per case */
struct klass
{
  SM_CLASS c;
  SM_ATTRIBUTE att[4];
  std::vector<SM_CLASS_CONSTRAINT *> cons;
  int mop_token;

  klass ()
  {
    static const char *names[4] = { "a", "b", "c", "d" };
    memset (&c, 0, sizeof (c));
    memset (att, 0, sizeof (att));
    c.header.ch_type = SM_META_CLASS;
    c.header.ch_name = "t";
    c.class_type = SM_CLASS_CT;
    for (int i = 0; i < 4; i++)
      {
	att[i].header.name = names[i];
	att[i].id = i + 1;
	att[i].header.next = (i < 3) ? (SM_COMPONENT *) &att[i + 1] : NULL;
      }
    c.attributes = &att[0];
  }
  /* an index whose key starts with the attributes given (ids 1..4) */
  SM_CLASS_CONSTRAINT *index (std::vector<int> ids, SM_CONSTRAINT_TYPE type = SM_CONSTRAINT_INDEX)
  {
    SM_CLASS_CONSTRAINT *k = (SM_CLASS_CONSTRAINT *) calloc (1, sizeof (SM_CLASS_CONSTRAINT));
    k->type = type;
    k->index_status = SM_NORMAL_INDEX;
    k->attributes = (SM_ATTRIBUTE **) calloc (ids.size () + 1, sizeof (SM_ATTRIBUTE *));
    for (size_t i = 0; i < ids.size (); i++)
      {
	k->attributes[i] = &att[ids[i] - 1];
      }
    k->next = c.constraints;
    c.constraints = k;
    cons.push_back (k);
    return k;
  }
  MOP mop ()
  {
    return (MOP) &mop_token;
  }
};

/* the columns a read returns: every attribute (row_snapshot_columns would give these for INT columns) */
struct cols
{
  ROW_SNAPSHOT s;
  int ids[4] = { 1, 2, 3, 4 };

  explicit cols (int n = 4)
  {
    memset (&s, 0, sizeof (s));
    s.n_attrs = n;
    s.attr_ids = ids;
  }
};

/* one filter: column slot, integer values, the optimizer's selectivity */
struct filt
{
  std::vector<DB_VALUE> vals;
  std::vector<DB_VALUE *> ptrs;
  ROW_SNAPSHOT_FILTER f;

  filt (int column, std::vector<int> values, double sel)
  {
    for (int v : values)
      {
	DB_VALUE x;
	db_make_int (&x, v);
	vals.push_back (x);
      }
    for (DB_VALUE &x : vals)
      {
	ptrs.push_back (&x);
      }
    memset (&f, 0, sizeof (f));
    f.column = column;
    f.n_values = (int) ptrs.size ();
    f.values = ptrs.empty () ? NULL : ptrs.data ();
    f.selectivity = sel;
  }
};

/* the process state the statement being planned has when the read starts */
static char g_dump_name[] = "plan.dump";
static FILE *const g_dump_fp = (FILE *) &g_dump_name;

static void
reset_world ()
{
  g_srv = server ();
  g_seen = seen ();
  g_err_levels = { NO_ERROR };
  g_max_depth = 1;
  g_level = 0x101;		/* plan dump on, level 1 */
  query_Plan_dump_filename = g_dump_name;
  query_Plan_dump_fp = g_dump_fp;
  query_Plan_dump_fp_open = true;
  do_Trigger_involved = true;	/* planning a statement a trigger action runs */
  tm_Tran_wait_msecs = 3000;	/* lock_timeout 3 s */
  g_wait_fail_err = NO_ERROR;
  g_wait_fail_on = 0;
  g_wait_calls.clear ();
  g_supp_attached = true;
  g_detach_fails = false;
  g_detaches = g_attaches = 0;
}

/* everything the read saved is back */
static void
check_restored (int statement_error = NO_ERROR)
{
  CHECK (g_level == 0x101);
  CHECK (query_Plan_dump_filename == g_dump_name);
  CHECK (query_Plan_dump_fp == g_dump_fp);
  CHECK (query_Plan_dump_fp_open);
  CHECK (do_Trigger_involved);
  CHECK (tm_Tran_wait_msecs == 3000);
  CHECK (g_supp_attached);
  CHECK (g_detaches == g_attaches);
  CHECK (g_err_levels.size () == 1);
  CHECK (g_seen.opens == g_seen.closes);
  CHECK (er_errid () == statement_error);
}

/* the read of the nested case: the same class and filters */
static klass *g_k;
static cols *g_cols;
static filt *g_filt;

static ROW_SNAPSHOT *
read_again (int *error)
{
  return row_snapshot_read (g_k->mop (), &g_k->c, &g_cols->s, &g_filt->f, 1, 10, false, 100.0, error);
}

/* ================================================================ 1. the query */

TEST_CASE ("PR8242 read: the query selects the snapshot columns from the class, ANDs an IN list per filter and "
	   "binds every filter value in order, then LIMIT max_rows + 1", "[contract][query]")
{
  reset_world ();
  klass k;
  k.index ({ 1 });
  cols c;
  filt f0 (0, { 10, 20 }, 0.01), f1 (1, { 7 }, 0.5);
  ROW_SNAPSHOT_FILTER fs[2] = { f0.f, f1.f };
  g_srv.rows = { { 10, 7, 100, 1000 }, { 20, 7, 200, 2000 } };
  int error = -1;

  ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, fs, 2, 50, false, 1000.0, &error);
  CHECK (g_seen.query == "SELECT [a], [b], [c], [d] FROM [t] WHERE [a] IN (?, ?) AND [b] IN (?) LIMIT ?");
  CHECK (g_seen.binds == std::vector<std::string> ({ "i:10", "i:20", "i:7", "i:51" }));
  CHECK (error == NO_ERROR);
  REQUIRE (s != NULL);
  CHECK (s->n_attrs == 4);
  CHECK (s->n_rows == 2);
  CHECK (s->attr_ids != c.s.attr_ids);	/* its own copy */
  for (int i = 0; i < 4; i++)
    {
      CHECK (s->attr_ids[i] == i + 1);
    }
  CHECK (db_get_int (&s->values[0 * 4 + 0]) == 10);
  CHECK (db_get_int (&s->values[1 * 4 + 3]) == 2000);
  CHECK (g_seen.ends == 1);
  row_snapshot_free (s);
  check_restored ();
}

TEST_CASE ("PR8242 read boundary: the columns come in the snapshot's slot order and a filter names its column "
	   "through the slot; a single value is one marker", "[contract][query]")
{
  reset_world ();
  klass k;
  k.index ({ 3 });
  cols c;
  c.ids[0] = 3;
  c.ids[1] = 1;
  c.s.n_attrs = 2;
  filt f0 (0, { 5 }, 0.001);
  g_srv.rows = { { 5, 1 } };
  int error = -1;

  ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 1, false, 1000.0, &error);
  CHECK (g_seen.query == "SELECT [c], [a] FROM [t] WHERE [c] IN (?) LIMIT ?");
  CHECK (g_seen.binds == std::vector<std::string> ({ "i:5", "i:2" }));
  REQUIRE (s != NULL);
  CHECK (s->attr_ids[0] == 3);
  row_snapshot_free (s);
  check_restored ();
}

/* ================================================================ 2. the gates */

TEST_CASE ("PR8242 read gate: without a usable index led by a filter column (and no hinted scan) there is no read -- "
	   "no index, an index led by another column, a function, filtered, invisible or reverse index", "[contract][gate]")
{
  for (int variant = 0; variant < 7; variant++)
    {
      INFO ("variant " << variant);
      reset_world ();
      klass k;
      switch (variant)
	{
	case 0:
	  break;		/* no index at all */
	case 1:
	  k.index ({ 2, 1 });	/* (b, a): a does not lead */
	  break;
	case 2:
	  k.index ({ 1 })->func_index_info = (SM_FUNCTION_INFO *) &k;
	  break;
	case 3:
	  k.index ({ 1 })->filter_predicate = (SM_PREDICATE_INFO *) &k;
	  break;
	case 4:
	  k.index ({ 1 })->index_status = SM_INVISIBLE_INDEX;
	  break;
	case 5:
	  k.index ({ 1 })->index_status = SM_ONLINE_INDEX_BUILDING_IN_PROGRESS;
	  break;
	case 6:
	  k.index ({ 1 }, SM_CONSTRAINT_REVERSE_INDEX);
	  break;
	}
      cols c;
      filt f0 (0, { 1 }, 0.001);
      g_srv.rows = { { 1, 2, 3, 4 } };
      int error = -1;
      CHECK (row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 50, false, 1000.0, &error) == NULL);
      CHECK (error == NO_ERROR);
      CHECK (g_seen.opens == 0);
      CHECK (g_detaches == 0);
      check_restored ();
    }
}

TEST_CASE ("PR8242 read gate: an INDEX, UNIQUE, PRIMARY KEY or FOREIGN KEY led by one filter column is enough; the "
	   "other filters only filter", "[contract][gate]")
{
  for (SM_CONSTRAINT_TYPE t : { SM_CONSTRAINT_INDEX, SM_CONSTRAINT_UNIQUE, SM_CONSTRAINT_PRIMARY_KEY,
				SM_CONSTRAINT_FOREIGN_KEY })
    {
      INFO ("constraint type " << (int) t);
      reset_world ();
      klass k;
      k.index ({ 2 }, t);
      cols c;
      filt f0 (0, { 1 }, 0.5), f1 (1, { 2 }, 0.001);	/* a not indexed, b indexed */
      ROW_SNAPSHOT_FILTER fs[2] = { f0.f, f1.f };
      g_srv.rows = { { 1, 2, 3, 4 } };
      int error = -1;
      ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, fs, 2, 50, false, 1000.0, &error);
      CHECK (s != NULL);
      CHECK (g_seen.opens == 1);
      row_snapshot_free (s);
      check_restored ();
    }
}

TEST_CASE ("PR8242 read gate: the rows the index range is expected to hold (n_card x the smallest indexed "
	   "selectivity) may reach 10 x max_rows, not pass it", "[contract][gate]")
{
  struct
  {
    double n_card, sel_a, sel_b;
    bool b_indexed, read;
  } rows[] =
  {
    { 10000, 0.05, 0.5, false, true },		/* 500 = 10 x 50: at the limit */
    { 10000, 0.0501, 0.5, false, false },	/* 501 */
    { 10000, 0.5, 0.01, true, true },		/* b's 100 rows decide, not a's 5000 */
    { 10000, 0.5, 0.06, true, false },		/* 600 and 5000: both past */
  };
  for (auto &r : rows)
    {
      INFO ("n_card " << r.n_card << " sel_a " << r.sel_a << " sel_b " << r.sel_b);
      reset_world ();
      klass k;
      k.index ({ 1 });
      if (r.b_indexed)
	{
	  k.index ({ 2 });
	}
      cols c;
      filt f0 (0, { 1 }, r.sel_a), f1 (1, { 2 }, r.sel_b);
      ROW_SNAPSHOT_FILTER fs[2] = { f0.f, f1.f };
      g_srv.rows = { { 1, 2, 3, 4 } };
      int error = -1;
      ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, fs, 2, 50, false, r.n_card, &error);
      CHECK ((s != NULL) == r.read);
      CHECK (g_seen.opens == (r.read ? 1 : 0));
      CHECK (error == NO_ERROR);
      row_snapshot_free (s);
      check_restored ();
    }
}

TEST_CASE ("PR8242 read gate: a hinted scan reads a class without a filter index when the statistics count at most "
	   "max (10 x max_rows, 1000) rows; an indexed filter narrows what it goes through", "[contract][gate]")
{
  struct
  {
    int max_rows;
    double n_card;
    bool indexed;
    double sel;
    bool read;
  } rows[] =
  {
    { 50, 1000, false, 1.0, true },	/* limit 1000 (500 lifted) */
    { 50, 1001, false, 1.0, false },
    { 200, 2000, false, 1.0, true },	/* limit 2000 */
    { 200, 2001, false, 1.0, false },
    { 50, 100000, true, 0.005, true },	/* 500 through the index */
    { 50, 100000, true, 0.0101, false },	/* 1010 */
  };
  for (auto &r : rows)
    {
      INFO ("max_rows " << r.max_rows << " n_card " << r.n_card << " indexed " << r.indexed);
      reset_world ();
      klass k;
      if (r.indexed)
	{
	  k.index ({ 1 });
	}
      cols c;
      filt f0 (0, { 1 }, r.sel);
      g_srv.rows = { { 1, 2, 3, 4 } };
      int error = -1;
      ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, r.max_rows, true, r.n_card, &error);
      CHECK ((s != NULL) == r.read);
      CHECK (g_seen.opens == (r.read ? 1 : 0));
      row_snapshot_free (s);
      check_restored ();
    }
}

/* ================================================================ 3. the rows */

TEST_CASE ("PR8242 read rows: max_rows rows make the snapshot, one more or none make no snapshot", "[contract][rows]")
{
  for (int n : { 0, 1, 3, 4 })
    {
      INFO (n << " rows pass, max_rows 3");
      reset_world ();
      klass k;
      k.index ({ 1 });
      cols c;
      filt f0 (0, { 1 }, 0.001);
      for (int r = 0; r < n; r++)
	{
	  g_srv.rows.push_back ({ 1, r, r * 10, r * 100 });
	}
      int error = -1;
      ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 3, false, 1000.0, &error);
      CHECK (g_seen.binds.back () == "i:4");
      CHECK (error == NO_ERROR);
      if (n >= 1 && n <= 3)
	{
	  REQUIRE (s != NULL);
	  CHECK (s->n_rows == n);
	  CHECK (db_get_int (&s->values[(n - 1) * 4 + 3]) == (n - 1) * 100);
	}
      else
	{
	  CHECK (s == NULL);
	}
      row_snapshot_free (s);
      check_restored ();
    }
}

/* ================================================================ 4. state and errors */

TEST_CASE ("PR8242 read state: the read runs at level 1 with no plan dump, no lock wait, no trace and the statement's "
	   "XASL supp info detached; everything is back afterwards, the trigger flag and the statement's error included",
	   "[contract][state]")
{
  reset_world ();
  set_err (ER_QPROC_INVALID_XASLNODE);	/* an error the statement being planned already carries */
  klass k;
  k.index ({ 1 });
  cols c;
  filt f0 (0, { 1 }, 0.001);
  g_srv.rows = { { 1, 2, 3, 4 } };
  int error = -1;

  ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error);
  REQUIRE (s != NULL);
  CHECK (g_seen.level == 1);
  CHECK (g_seen.wait == 0);
  CHECK_FALSE (g_seen.trace);
  CHECK (g_seen.dump_fp_null);
  CHECK_FALSE (g_seen.dump_open);
  CHECK (g_seen.dump_name_null);
  CHECK_FALSE (g_seen.supp_attached);
  CHECK (g_wait_calls == std::vector<int> ({ 0, 3000 }));
  CHECK (error == NO_ERROR);
  row_snapshot_free (s);
  check_restored (ER_QPROC_INVALID_XASLNODE);
}

TEST_CASE ("PR8242 read state boundary: with no lock wait set (lock_timeout 0) the wait is not touched at all",
	   "[contract][state]")
{
  reset_world ();
  tm_Tran_wait_msecs = 0;
  klass k;
  k.index ({ 1 });
  cols c;
  filt f0 (0, { 1 }, 0.001);
  g_srv.rows = { { 1, 2, 3, 4 } };
  int error = -1;
  ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error);
  CHECK (s != NULL);
  CHECK (g_wait_calls.empty ());
  CHECK (tm_Tran_wait_msecs == 0);
  row_snapshot_free (s);
}

TEST_CASE ("PR8242 read errors: a failure of the read itself (lock refused, compile error, fetch error) means no "
	   "snapshot -- *error NO_ERROR, the statement's error and state as they were", "[contract][errors]")
{
  for (int variant = 0; variant < 3; variant++)
    {
      INFO ("variant " << variant);
      reset_world ();
      klass k;
      k.index ({ 1 });
      cols c;
      filt f0 (0, { 1 }, 0.001);
      g_srv.rows = { { 1, 2, 3, 4 }, { 1, 3, 4, 5 } };
      if (variant == 0)
	{
	  g_srv.exec_err = ER_LK_OBJECT_TIMEOUT_SIMPLE_MSG;	/* the no-wait lock request refused */
	}
      else if (variant == 1)
	{
	  g_srv.compile_err = ER_FAILED;
	}
      else
	{
	  g_srv.fetch_err_at = 1;
	  g_srv.fetch_err = ER_FAILED;
	}
      int error = -1;
      CHECK (row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error) == NULL);
      CHECK (error == NO_ERROR);
      check_restored (NO_ERROR);
    }
}

TEST_CASE ("PR8242 read errors: a deadlock abort or a lost server during the read is reported -- NULL, *error set and "
	   "left as the current error, the rest of the state restored", "[contract][errors]")
{
  for (int err : { (int) ER_LK_UNILATERALLY_ABORTED, (int) ER_TM_SERVER_DOWN_UNILATERALLY_ABORTED,
		   (int) ER_NET_SERVER_CRASHED, (int) ER_OBJ_NO_CONNECT })
    {
      INFO ("error " << err);
      reset_world ();
      klass k;
      k.index ({ 1 });
      cols c;
      filt f0 (0, { 1 }, 0.001);
      g_srv.exec_err = err;
      int error = -1;
      CHECK (row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error) == NULL);
      CHECK (error == err);
      check_restored (err);
    }
}

TEST_CASE ("PR8242 read errors boundary: giving the lock wait back fails after a good read because the server is "
	   "gone -- that error is reported and no snapshot is kept; the read's own no-wait request failing that way "
	   "is reported too", "[contract][errors]")
{
  for (int failing : { 2, 1 })
    {
      INFO ("request " << failing << " of 2 fails");
      reset_world ();
      klass k;
      k.index ({ 1 });
      cols c;
      filt f0 (0, { 1 }, 0.001);
      g_srv.rows = { { 1, 2, 3, 4 } };
      g_wait_fail_on = failing;
      g_wait_fail_err = ER_NET_SERVER_CRASHED;
      int error = -1;

      CHECK (row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error) == NULL);
      CHECK (error == ER_NET_SERVER_CRASHED);
      CHECK (g_seen.opens == (failing == 1 ? 0 : 1));	/* a failed first request opens no session */
      check_restored (ER_NET_SERVER_CRASHED);
    }
}

/* ================================================================ 5. guards and nesting */

TEST_CASE ("PR8242 read guards: no class, no columns, no filters, max_rows 0, a column or filter the class does not "
	   "have, or a filter with no value -- NULL, *error NO_ERROR, nothing detached or opened", "[contract][guards]")
{
  for (int variant = 0; variant < 8; variant++)
    {
      INFO ("variant " << variant);
      reset_world ();
      klass k;
      k.index ({ 1 });
      cols c;
      filt f0 (0, { 1 }, 0.001), empty (0, {}, 0.001);
      MOP mop = k.mop ();
      SM_CLASS *cls = &k.c;
      ROW_SNAPSHOT *cs = &c.s;
      ROW_SNAPSHOT_FILTER *fs = &f0.f;
      int n_filters = 1, max_rows = 10;
      switch (variant)
	{
	case 0:
	  mop = NULL;
	  break;
	case 1:
	  cls = NULL;
	  break;
	case 2:
	  cs = NULL;
	  break;
	case 3:
	  n_filters = 0;
	  break;
	case 4:
	  max_rows = 0;
	  break;
	case 5:
	  c.ids[2] = 99;	/* a column id the class does not have */
	  break;
	case 6:
	  fs = &empty.f;
	  break;
	case 7:
	  k.c.header.ch_name = NULL;
	  break;
	}
      g_srv.rows = { { 1, 2, 3, 4 } };
      int error = -1;
      CHECK (row_snapshot_read (mop, cls, cs, fs, n_filters, max_rows, false, 1000.0, &error) == NULL);
      CHECK (error == NO_ERROR);
      CHECK (g_seen.opens == 0);
      CHECK (g_detaches == 0);
      check_restored ();
    }
}

TEST_CASE ("PR8242 read nesting: a read started while a read runs (planning the read's own query) returns NULL at "
	   "once, and the outer read still completes; a read after it works again", "[contract][guards]")
{
  reset_world ();
  klass k;
  k.index ({ 1 });
  cols c;
  filt f0 (0, { 1 }, 0.001);
  g_k = &k;
  g_cols = &c;
  g_filt = &f0;
  g_srv.rows = { { 1, 2, 3, 4 } };
  g_srv.nested = true;
  int error = -1;

  ROW_SNAPSHOT *s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error);
  CHECK (g_seen.nested_result == 1);
  CHECK (g_seen.nested_error == NO_ERROR);
  CHECK (g_seen.opens == 1);			/* the nested one opened nothing */
  REQUIRE (s != NULL);
  row_snapshot_free (s);
  check_restored ();

  g_srv.nested = false;
  s = row_snapshot_read (k.mop (), &k.c, &c.s, &f0.f, 1, 10, false, 1000.0, &error);
  CHECK (s != NULL);
  row_snapshot_free (s);
}
