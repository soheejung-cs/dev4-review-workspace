/*
 * PR#8696 (CBRD-28231) -- do_process_prepare_statement () of src/compat/db_vdb.c (CS_MODE), the handler of an SQL-level
 * 'PREPARE name FROM ...': it compiles the text in a subsession, packs the prepare info and registers it with the
 * server (csession_create_prepared_statement). After the walk that collects the cached subqueries
 * (do_process_prepare_subquery_pre) it checks the error context for a failure of that walk.
 *
 * The contract the PR sets (db_vdb.c:4278-4283 at bb9575015): only an error (ER_FATAL_ERROR / ER_ERROR /
 * ER_SYNTAX_ERROR severity, er_errid_if_has_error ()) fails the PREPARE there. A warning the compilation left -- name
 * resolution of an ALL class without subclasses leaves ER_OBJ_NO_COMPONENTS (db_get_subclasses (), db_info.c:701,
 * called from name_resolution.c:7663), and a statement folded to false does not go on to clear it -- is not a failure
 * of the walk. A real failure of the walk (set_prepare_subquery_info (), db_vdb.c:4095: ER_OUT_OF_VIRTUAL_MEMORY) still
 * fails it, after a warning too, and the name the server may hold is deleted.
 * base (a617e19b7) checks er_errid (): the warning fails the PREPARE (P2 FAILs there -- the fix).
 *
 * Real code: db_vdb.c (the TU: do_process_prepare_statement -- through hooks.cpp --, db_prepared_tree_remove /
 * _unlink / _close on the empty kept-tree list, db_check_single_query, is_allowed_as_prepared_statement(_with_hv),
 * set_prepare_info_into_list, do_process_prepare_subquery_pre).
 * Seams (same TU): db_open_buffer_local (hands out the case's subsession), db_compile_statement_local (leaves what the
 * case says in the error context and returns 1, the statement index), db_close_session_local (counts).
 * Fakes (suite.json "fakes"): the error context of error_manager.c / error_context.cpp -- one current er_message
 * (err_id, severity) with a stack: er_set = set_error, er_errid = its err_id (error_manager.c:1868-1884, initialized),
 * er_errid_if_has_error = its err_id when er_is_error_severity () (error_manager.c:1894-1901, 3278-3293) else NO_ERROR,
 * er_clear = clear_error (NO_ERROR, ER_WARNING_SEVERITY: error_context.cpp:141-148), er_stack_push = a new cleared
 * level, er_stack_pop = back to the level below; parser_walk_tree (parse_tree_cl.c:1252: here the pre function on the
 * statement node only -- these statements have no subquery; a case can make the walk fail as set_prepare_subquery_info
 * () does); pt_node_to_cmd_type (parse_dbi.c:2756-2790); db_init_prepare_info (db_query.c:414-435 verbatim);
 * db_pack_prepare_info (db_query.c:444: a malloc'ed buffer and its size); csession_create_prepared_statement /
 * csession_delete_prepared_statement (network_interface_cl.c: record the call; the delete leaves an error of its own,
 * as the server answers for a name it does not hold).
 */
#include "catch.hpp"
#include "config.h"
#include "dbi.h"
#include "db.h"
#include "db_query.h"
#include "parser.h"
#include "parse_tree.h"
#include "db_session.h"
#include "error_manager.h"
#include "error_code.h"
#include "network_interface_cl.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int ut_process_prepare (DB_SESSION * session, PT_NODE * statement);

/* ---------------------------------------------------------------- the error context */
namespace
{
  struct ut_er_level
  {
    int err_id;
    int severity;
  };
  std::vector < ut_er_level > g_er_stack (1, ut_er_level { NO_ERROR, ER_WARNING_SEVERITY });

  ut_er_level & cur ()
  {
    return g_er_stack.back ();
  }

  bool is_error_severity (int severity)
  {
    switch (severity)
      {
      case ER_FATAL_ERROR_SEVERITY:
      case ER_ERROR_SEVERITY:
      case ER_SYNTAX_ERROR_SEVERITY:
	return true;
      default:
	return false;
      }
  }
}

void
er_set (int severity, const char *, const int, int err_id, int, ...)
{
  cur ().err_id = err_id;
  cur ().severity = severity;
}

int
er_errid (void)
{
  return cur ().err_id;
}

int
er_errid_if_has_error (void)
{
  return is_error_severity (cur ().severity) ? cur ().err_id : NO_ERROR;
}

void
er_clear (void)
{
  cur ().err_id = NO_ERROR;
  cur ().severity = ER_WARNING_SEVERITY;
}

void
er_stack_push (void)
{
  g_er_stack.push_back (ut_er_level { NO_ERROR, ER_WARNING_SEVERITY });
}

void
er_stack_pop (void)
{
  if (g_er_stack.size () > 1)
    {
      g_er_stack.pop_back ();
    }
}

/* ---------------------------------------------------------------- what the case sets */
namespace
{
  enum class leftover
  { NONE, WARNING };

  DB_SESSION *g_sub = NULL;	/* the subsession db_open_buffer_local () hands out */
  leftover g_compile_leaves = leftover::NONE;
  bool g_walk_fails = false;
  std::string g_opened_text;
  int g_compile_calls = 0, g_walk_calls = 0, g_pack_calls = 0, g_create_calls = 0, g_delete_calls = 0;
  int g_close_calls = 0;
  const DB_SESSION *g_closed = NULL;
  std::string g_created_name, g_deleted_name;
  int g_created_len = -1;
  int g_stack_depth_at_delete = 0;
}

/* seam: db_vdb.c:1098 -- the real one parses the text into a new session; here the case's subsession */
DB_SESSION *
db_open_buffer_local (const char *buffer)
{
  g_opened_text = buffer;
  return g_sub;
}

/* seam: db_vdb.c:1609 -- the real one compiles the statement (name resolution, semantic check, XASL) and returns the
 * index of the statement it compiled, 1 here; what the compilation left in the error context is the case's */
int
db_compile_statement_local (DB_SESSION * session)
{
  g_compile_calls++;
  if (g_compile_leaves == leftover::WARNING)
    {
      /* db_info.c:701 db_get_subclasses () for a class without subclasses (ALL class, name_resolution.c:7663) */
      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_OBJ_NO_COMPONENTS, 0);
    }
  session->stmt_ndx = 1;
  return 1;
}

/* seam: db_vdb.c:5874 -- counts instead of freeing the statements and the parser */
void
db_close_session_local (DB_SESSION * session)
{
  g_close_calls++;
  g_closed = session;
}

/* parse_tree_cl.c:1252 parser_walk_tree (): the pre function on the statement node (these statements have no
 * subquery below it); a failing walk is what set_prepare_subquery_info () leaves when its malloc fails
 * (db_vdb.c:4095) -- the pre function stops the walk */
PT_NODE *
parser_walk_tree (PARSER_CONTEXT * parser, PT_NODE * node, PT_NODE_WALK_FUNCTION pre_function, void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  g_walk_calls++;
  int continue_walk = PT_CONTINUE_WALK;
  if (pre_function != NULL && node != NULL)
    {
      node = pre_function (parser, node, pre_argument, &continue_walk);
    }
  if (g_walk_fails)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, (size_t) 128);
    }
  return node;
}

/* parse_dbi.c:2756-2790 */
CUBRID_STMT_TYPE
pt_node_to_cmd_type (PT_NODE * node)
{
  if (node == NULL)
    {
      return CUBRID_STMT_NONE;
    }
  switch (node->node_type)
    {
    case PT_DIFFERENCE:
    case PT_INTERSECTION:
    case PT_UNION:
    case PT_SELECT:
      return CUBRID_STMT_SELECT;
    default:
      return (CUBRID_STMT_TYPE) node->node_type;
    }
}

/* db_query.c:414-435 verbatim */
void
db_init_prepare_info (DB_PREPARE_INFO * info)
{
  info->statement = NULL;
  info->columns = NULL;
  info->host_variables.size = 0;
  info->host_variables.vals = NULL;
  info->host_var_expected_domains = NULL;
  info->host_var_derived_from = NULL;
  info->host_var_derived_pads = NULL;
  info->host_var_derived_count = 0;
  info->host_var_format_classes = NULL;
  info->auto_param_count = 0;
  info->recompile = 0;
  info->oids_included = 0;
  info->into_list = NULL;
  info->into_count = 0;
  info->subquery_num = 0;
  info->subquery_info = NULL;
}

/* db_query.c:444: the packed size, the buffer malloc'ed (the caller frees it with free_and_init) */
int
db_pack_prepare_info (const DB_PREPARE_INFO * info, char **buffer)
{
  g_pack_calls++;
  *buffer = (char *) malloc (24);
  memset (*buffer, 0, 24);
  return 24;
}

/* network_interface_cl.c: the server keeps the prepared statement under the name */
int
csession_create_prepared_statement (const char *name, const char *alias_print, char *stmt_info, int info_length)
{
  g_create_calls++;
  g_created_name = name;
  g_created_len = info_length;
  return NO_ERROR;
}

/* network_interface_cl.c: the server drops the name; for a name it does not hold it answers with an error, which the
 * caller's er_stack_push () / er_stack_pop () keep out of its own */
int
csession_delete_prepared_statement (const char *name)
{
  g_delete_calls++;
  g_deleted_name = name;
  g_stack_depth_at_delete = (int) g_er_stack.size ();
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_FAILED, 0);
  return ER_FAILED;
}

/* ---------------------------------------------------------------- builders */
namespace
{
  PT_NODE *node (PT_NODE_TYPE t)
  {
    PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
    n->node_type = t;
    return n;
  }

  /* PREPARE <name> FROM '<text>': info.prepare.name a PT_NAME, info.prepare.statement a string PT_VALUE */
  PT_NODE *prepare_stmt (const char *name, const char *text)
  {
    PT_NODE *p = node (PT_PREPARE_STATEMENT);
    PT_NODE *n = node (PT_NAME);
    n->info.name.original = name;
    PT_NODE *v = node (PT_VALUE);
    size_t len = strlen (text);
    PARSER_VARCHAR *str = (PARSER_VARCHAR *) calloc (1, sizeof (PARSER_VARCHAR) + len + 1);
    str->length = (int) len;
    memcpy (str->bytes, text, len + 1);
    v->type_enum = PT_TYPE_CHAR;
    v->info.value.data_value.str = str;
    p->info.prepare.name = n;
    p->info.prepare.statement = v;
    return p;
  }

  /* the subsession db_open_buffer_local () makes for the text: one SELECT, no host variable */
  DB_SESSION *subsession ()
  {
    DB_SESSION *s = (DB_SESSION *) calloc (1, sizeof (DB_SESSION));
    s->parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    s->statements = (PT_NODE **) calloc (1, sizeof (PT_NODE *));
    s->type_list = (DB_QUERY_TYPE **) calloc (1, sizeof (DB_QUERY_TYPE *));
    s->statements[0] = node (PT_SELECT);
    s->dimension = 1;
    return s;
  }

  void reset (leftover compile_leaves, bool walk_fails)
  {
    g_er_stack.assign (1, ut_er_level { NO_ERROR, ER_WARNING_SEVERITY });
    g_sub = subsession ();
    g_compile_leaves = compile_leaves;
    g_walk_fails = walk_fails;
    g_opened_text.clear ();
    g_compile_calls = g_walk_calls = g_pack_calls = g_create_calls = g_delete_calls = g_close_calls = 0;
    g_closed = NULL;
    g_created_name.clear ();
    g_deleted_name.clear ();
    g_created_len = -1;
    g_stack_depth_at_delete = 0;
  }

  DB_SESSION *client ()
  {
    return (DB_SESSION *) calloc (1, sizeof (DB_SESSION));
  }
}

/* ---------------------------------------------------------------- contracts */

TEST_CASE ("PR8696 P1 (control): a compilation that leaves nothing -- the PREPARE packs the info, registers the name "
	   "with the server, deletes nothing and closes its subsession once", "[contract]")
{
  reset (leftover::NONE, false);
  PT_NODE *stmt = prepare_stmt ("stmt1", "select * from t where a = ?");

  int err = ut_process_prepare (client (), stmt);

  CHECK (err == NO_ERROR);
  CHECK (g_opened_text == "select * from t where a = ?");
  CHECK (g_compile_calls == 1);
  CHECK (g_walk_calls == 1);
  CHECK (g_pack_calls == 1);
  CHECK (g_create_calls == 1);
  CHECK (g_created_name == "stmt1");
  CHECK (g_created_len == 24);
  CHECK (g_delete_calls == 0);
  CHECK (g_close_calls == 1);
  CHECK (g_closed == g_sub);
}

TEST_CASE ("PR8696 P2: a compilation that leaves the warning ER_OBJ_NO_COMPONENTS (ALL class without subclasses, "
	   "statement folded to false) -- the warning is not a failure of the walk: the PREPARE succeeds as in P1",
	   "[contract]")
{
  reset (leftover::WARNING, false);
  PT_NODE *stmt = prepare_stmt ("stmt1", "select * from all t where 1 = 0");

  int err = ut_process_prepare (client (), stmt);

  CHECK (err == NO_ERROR);
  CHECK (g_walk_calls == 1);
  CHECK (g_pack_calls == 1);
  CHECK (g_create_calls == 1);
  CHECK (g_created_name == "stmt1");
  CHECK (g_delete_calls == 0);
  CHECK (g_close_calls == 1);
}

TEST_CASE ("PR8696 P3 (boundary): the walk itself fails (ER_OUT_OF_VIRTUAL_MEMORY, as set_prepare_subquery_info () "
	   "leaves it) -- the PREPARE fails with that error, packs and registers nothing, deletes the name once and "
	   "closes its subsession once", "[contract]")
{
  reset (leftover::NONE, true);
  PT_NODE *stmt = prepare_stmt ("stmt1", "select * from t");

  int err = ut_process_prepare (client (), stmt);

  CHECK (err == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (g_pack_calls == 0);
  CHECK (g_create_calls == 0);
  CHECK (g_delete_calls == 1);
  CHECK (g_deleted_name == "stmt1");
  CHECK (g_stack_depth_at_delete == 2);	/* the delete runs on a pushed level ... */
  CHECK (er_errid () == ER_OUT_OF_VIRTUAL_MEMORY);	/* ... so its own error does not replace the caller's */
  CHECK (g_close_calls == 1);
}

TEST_CASE ("PR8696 P4 (boundary): a warning left by the compilation, then a failing walk -- the error, not the "
	   "warning, fails the PREPARE: only the warning is let through", "[contract]")
{
  reset (leftover::WARNING, true);
  PT_NODE *stmt = prepare_stmt ("stmt1", "select * from all t where 1 = 0");

  int err = ut_process_prepare (client (), stmt);

  CHECK (err == ER_OUT_OF_VIRTUAL_MEMORY);
  CHECK (g_pack_calls == 0);
  CHECK (g_create_calls == 0);
  CHECK (g_delete_calls == 1);
  CHECK (g_close_calls == 1);
}
