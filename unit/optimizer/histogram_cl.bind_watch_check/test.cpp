/*
 * PR#8013 -- histogram_bind_watch_check (src/optimizer/histogram/histogram_cl.cpp, cs mode): price
 * the statement's host-variable predicates under the bound values and compare each with the rows
 * recorded when the plan was chosen; true = replan (ws re-recorded). Only caller: db_vdb.c:290
 * db_bind_watch_verdict, band BIND_WATCH_HINT_BAND 1.1 (hint) or BIND_WATCH_BAND 1.5 (first peek)
 * -- histogram_cl.hpp:161/165; floor BIND_WATCH_ROW_FLOOR 1000 (hpp:169).
 *
 * The walk is the real one (bind_term_vector -> bind_term_walk -> bind_term_add, plus the real
 * histogram_split_hv_predicate / bind_card_arg_value); only the histogram reads are fakes:
 *   histogram_get_total_rows          (seam) column has a histogram -> its total rows, else false
 *   histogram_get_equal_selectivity   (seam) per host variable: ok + rows, sel = rows / total
 *   histogram_get_comp_selectivity    (seam) same
 * so a slot's rows come out as MAX (1.0, sel * total) = the rows the test asked for (total is 2^20,
 * exact). BIND_WATCH_STATE is filled by the test or by a first check.
 */
#include "catch.hpp"
#include "dbtype_def.h"
#include "dbtype.h"
#include "parser.h"
#include "parse_tree.h"
#include "query_planner.h"
#include "histogram_cl.hpp"
#include "system_parameter.h"
#include "error_manager.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

/* ---------------------------------------------------------------- fakes */

static const double TOTAL = 1048576.0;	/* 2^20: rows / TOTAL * TOTAL is exact */

static PARSER_CONTEXT *g_parser;	/* the scene's parser: maps a bound DB_VALUE back to its index */
static std::map<std::string, double> g_total;	/* column -> rows its histogram was built from */
struct price
{
  bool ok;
  double rows;
};
static std::map<int, price> g_price;	/* host variable index -> what the histogram says */

static int
hv_index_of (const DB_VALUE *v)
{
  return (g_parser != NULL && v != NULL) ? (int) (v - g_parser->host_variables) : -1;
}

/* histogram_cl.cpp:1526: histogram_init_reader_from_lhs (no usable histogram -> false), then
 * total_rows = reader.total_rows (), true when > 0 */
bool
histogram_get_total_rows (PT_NODE *lhs, double *total_rows)
{
  auto it = g_total.find (lhs->info.name.original);
  if (it == g_total.end ())
    {
      return false;
    }
  *total_rows = it->second;
  return *total_rows > 0.0;
}

/* histogram_cl.cpp:1178: rhs NULL, no histogram, or a key the histogram cannot hold (kind
 * mismatch, 1228-1232) -> success false and selectivity untouched; else selectivity in [0,1] and
 * success true (1357) */
static void
fake_price (PT_NODE *lhs, DB_VALUE *rhs, double *selectivity, bool *success)
{
  auto t = g_total.find (lhs->info.name.original);
  auto p = g_price.find (hv_index_of (rhs));

  if (rhs == NULL || t == g_total.end () || p == g_price.end () || !p->second.ok)
    {
      *success = false;
      return;
    }
  *selectivity = p->second.rows / t->second;
  *success = true;
}

void
histogram_get_equal_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, double *selectivity, bool *success)
{
  fake_price (lhs, rhs_db_value, selectivity, success);
}

void
histogram_get_comp_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, bool is_ge, bool include_equal,
				double *selectivity, bool *success)
{
  fake_price (lhs, rhs_db_value, selectivity, success);
}

/* parse_tree_cl.c:1111 pt_walk_private, for the node kinds built here: pre; then (CONTINUE/LEAF) the
 * sub-trees -- PT_EXPR arg1, arg2, arg3 (pt_apply_expr) -- ; then or_next; then (CONTINUE/LIST) the
 * rest of the `next` list; then post. PT_APPLY_WALK skips NULL and stops on PT_STOP_WALK. */
static PT_NODE *
walk (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE_WALK_FUNCTION pre, void *pre_arg, PT_NODE_WALK_FUNCTION post,
      void *post_arg, int *cont)
{
  int save;

  if (node == NULL || *cont == PT_STOP_WALK)
    {
      return node;
    }
  if (pre != NULL)
    {
      node = pre (parser, node, pre_arg, cont);
      if (node == NULL)
	{
	  return NULL;
	}
    }
  if (*cont != PT_STOP_WALK)
    {
      save = *cont;
      if ((save == PT_CONTINUE_WALK || save == PT_LEAF_WALK) && node->node_type == PT_EXPR)
	{
	  walk (parser, node->info.expr.arg1, pre, pre_arg, post, post_arg, cont);
	  walk (parser, node->info.expr.arg2, pre, pre_arg, post, post_arg, cont);
	  walk (parser, node->info.expr.arg3, pre, pre_arg, post, post_arg, cont);
	}
      if (save == PT_CONTINUE_WALK || save == PT_LEAF_WALK || save == PT_LIST_WALK)
	{
	  walk (parser, node->or_next, pre, pre_arg, post, post_arg, cont);
	}
      if (save == PT_CONTINUE_WALK || save == PT_LIST_WALK)
	{
	  walk (parser, node->next, pre, pre_arg, post, post_arg, cont);
	}
      if (*cont != PT_STOP_WALK)
	{
	  *cont = save;
	}
    }
  if (post != NULL)
    {
      node = post (parser, node, post_arg, cont);
    }
  return node;
}

PT_NODE *
parser_walk_tree (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE_WALK_FUNCTION pre_function, void *pre_argument,
		  PT_NODE_WALK_FUNCTION post_function, void *post_argument)
{
  int cont = PT_CONTINUE_WALK;

  return walk (parser, node, pre_function, pre_argument, post_function, post_argument, &cont);
}

/* parser_support.c:2441, verbatim */
PT_NODE *
pt_get_end_path_node (PT_NODE *node)
{
  while (node != NULL && node->node_type == PT_DOT_)
    {
      node = node->info.dot.arg2;
    }
  return node;
}

/* query_planner.c:12837, the cases these trees reach */
PRED_CLASS
qo_classify (PT_NODE *attr)
{
  switch (attr->node_type)
    {
    case PT_NAME:
    case PT_DOT_:
      return PC_ATTR;
    case PT_VALUE:
      return PT_IS_SET_TYPE (attr) ? PC_SET : (attr->type_enum == PT_TYPE_NULL ? PC_OTHER : PC_CONST);
    case PT_HOST_VAR:
      return PC_HOST_VAR;
    default:
      return PC_OTHER;
    }
}

/* system_parameter.c: prm_get_bool_value (PRM_ID_ER_LOG_DEBUG) is inline over this array (cs mode);
 * only that entry is read on this path. Off unless a case turns the per-check dump on. */
SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };

static void
trace (bool on)
{
  prm_Def[PRM_ID_ER_LOG_DEBUG].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ER_LOG_DEBUG].value.v.b = on;
}

/* error_manager.c: writes the formatted line to the error log. Here: keep the line, and for the
 * two bind-watch formats also the raw arguments. The label points into the checked function's
 * frame (cur.name), so it is copied here; label_at keeps only the address, for comparison. */
struct log_entry
{
  std::string text;
  std::string label;
  const char *label_at;
  double then, now, fold;
};
static std::vector<log_entry> g_log;

void
_er_log_debug (const char *file_name, const int line_no, const char *fmt, ...)
{
  char buf[8192];
  va_list ap, raw;
  log_entry e = { "", "", NULL, 0.0, 0.0, 0.0 };

  va_start (ap, fmt);
  va_copy (raw, ap);
  vsnprintf (buf, sizeof (buf), fmt, ap);
  e.text = buf;
  if (strncmp (fmt, "bind watch replan: ", 19) == 0)
    {
      /* "%s scans %.0f -> %.0f rows (%.1fx %s) over the %.1fx band; %s" */
      e.label_at = va_arg (raw, const char *);
      e.label = e.label_at;
      e.then = va_arg (raw, double);
      e.now = va_arg (raw, double);
      e.fold = va_arg (raw, double);
    }
  else if (strncmp (fmt, "bind watch dump: ", 17) == 0)
    {
      /* "%s scans %.0f rows (plan was chosen at %.0f)" */
      e.label_at = va_arg (raw, const char *);
      e.label = e.label_at;
      e.now = va_arg (raw, double);
      e.then = va_arg (raw, double);
    }
  va_end (raw);
  va_end (ap);
  g_log.push_back (e);
}

static const log_entry *
find_log (const char *prefix)
{
  for (const log_entry &e : g_log)
    {
      if (e.text.compare (0, strlen (prefix), prefix) == 0)
	{
	  return &e;
	}
    }
  return NULL;
}

/* ---------------------------------------------------------------- appendix hook */
int unitseam_bw_term_vector (PARSER_CONTEXT *parser, PT_NODE *statement, double *rows,
			     char (*names)[BIND_WATCH_NAME_LEN]);

/* ---------------------------------------------------------------- builders */

static PT_NODE *
mk (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

static PT_NODE *
col (const char *name)
{
  PT_NODE *n = mk (PT_NAME);
  n->info.name.original = name;
  n->info.name.resolved = "t";
  return n;
}

static PT_NODE *
hv (int index)
{
  PT_NODE *n = mk (PT_HOST_VAR);
  n->info.host_var.index = index;
  return n;
}

static PT_NODE *
cmp (PT_OP_TYPE op, PT_NODE *a, PT_NODE *b)
{
  PT_NODE *n = mk (PT_EXPR);
  n->info.expr.op = op;
  n->info.expr.arg1 = a;
  n->info.expr.arg2 = b;
  return n;
}

/* db_make_int / db_make_null, field by field */
static void
set_int (DB_VALUE *v, int x)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_INTEGER;
  v->domain.general_info.is_null = 0;
  v->data.i = x;
}

static void
set_null (DB_VALUE *v)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_NULL;
  v->domain.general_info.is_null = 1;
}

/* one statement over t: a WHERE list of (column op ?) predicates, `nhv` user host variables */
struct scene
{
  PARSER_CONTEXT *parser;
  PT_NODE *stmt = NULL, *tail = NULL;

  explicit scene (int nhv)
  {
    parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    parser->host_variables = (DB_VALUE *) calloc (nhv, sizeof (DB_VALUE));
    parser->host_var_count = nhv;
    parser->auto_param_count = 0;
    for (int i = 0; i < nhv; i++)
      {
	set_null (&parser->host_variables[i]);
      }
    g_parser = parser;
    g_total.clear ();
    g_price.clear ();
    g_log.clear ();
    trace (false);
  }
  void hist (const char *column)
  {
    g_total[column] = TOTAL;
  }
  void add (PT_NODE *pred)
  {
    if (tail == NULL)
      {
	stmt = pred;
      }
    else
      {
	tail->next = pred;
      }
    tail = pred;
  }
  void bind (int i, double rows)	/* a value the histogram prices at `rows` */
  {
    set_int (&parser->host_variables[i], 1000 + i);
    g_price[i] = { true, rows };
  }
  void bind_unpriceable (int i)	/* a value whose type the histogram does not hold */
  {
    set_int (&parser->host_variables[i], 1000 + i);
    g_price[i] = { false, 0.0 };
  }
  void bind_null (int i)
  {
    set_null (&parser->host_variables[i]);
    g_price.erase (i);
  }
  bool check (BIND_WATCH_STATE *ws, double band, bool *usable)
  {
    g_log.clear ();
    return histogram_bind_watch_check (parser, stmt, ws, band, usable);
  }
};

static BIND_WATCH_STATE *
new_ws ()
{
  BIND_WATCH_STATE *ws = (BIND_WATCH_STATE *) calloc (1, sizeof (BIND_WATCH_STATE));
  ws->terms = -1;		/* nothing recorded yet (db_stmt_bind_watch_state) */
  ws->cur_variant = -1;
  return ws;
}

static void
set_ws (BIND_WATCH_STATE *ws, std::initializer_list<double> rows)
{
  int i = 0;

  for (double r : rows)
    {
      ws->rows[i++] = r;
    }
  ws->terms = i;
}

/* t.a = ?0, t.b > ?1, both columns with a histogram */
static void
two_preds (scene &s)
{
  s.hist ("a");
  s.hist ("b");
  s.add (cmp (PT_EQ, col ("a"), hv (0)));
  s.add (cmp (PT_GT, col ("b"), hv (1)));
}

/* ---------------------------------------------------------------- contract */

TEST_CASE ("first check: records the fingerprint and returns true (the peek replan), with no band verdict logged",
	   "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 5000);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;

  CHECK (s.check (ws, 1.5, &usable));
  CHECK (usable);
  REQUIRE (ws->terms == 2);
  CHECK (ws->rows[0] == 5000.0);
  CHECK (ws->rows[1] == 300.0);
  CHECK (std::string (ws->name[0]) == "t.a=?");
  CHECK (std::string (ws->name[1]) == "t.b>?");
  CHECK (find_log ("bind watch replan") == NULL);
}

TEST_CASE ("nothing priceable: usable false, returns false, ws untouched", "[contract]")
{
  scene s (1);
  s.add (cmp (PT_EQ, col ("nohist"), hv (0)));	/* no histogram: bind_term_add drops it */
  s.add (cmp (PT_EQ, col ("a"), col ("b")));	/* not a host-variable predicate */
  s.bind (0, 5000);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = true;

  CHECK_FALSE (s.check (ws, 1.5, &usable));
  CHECK_FALSE (usable);
  CHECK (ws->terms == -1);
}

TEST_CASE ("in band: no replan and the recorded fingerprint stays", "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 2900);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  set_ws (ws, {2000, 300});
  bool usable = false;

  CHECK_FALSE (s.check (ws, 1.5, &usable));	/* 2900 / 2000 = 1.45 */
  CHECK (usable);
  CHECK (ws->rows[0] == 2000.0);
  CHECK (find_log ("bind watch replan") == NULL);
}

TEST_CASE ("a fold exactly at the band replans (>=) and re-records; the log names the predicate and lists every one",
	   "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 3000);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  set_ws (ws, {2000, 300});
  bool usable = false;

  CHECK (s.check (ws, 1.5, &usable));
  CHECK (ws->rows[0] == 3000.0);
  const log_entry *rep = find_log ("bind watch replan: ");
  REQUIRE (rep != NULL);
  CHECK (rep->label == "t.a=?");
  CHECK (rep->then == 2000.0);
  CHECK (rep->now == 3000.0);
  CHECK (rep->fold == 1.5);
  CHECK (rep->text.find ("(1.5x up)") != std::string::npos);
  CHECK (rep->text.find ("t.a=? 2000->3000, t.b>? 300->300") != std::string::npos);
}

TEST_CASE ("a move down counts the same as a move up", "[contract]")
{
  scene s (1);
  s.hist ("a");
  s.add (cmp (PT_EQ, col ("a"), hv (0)));
  s.bind (0, 1500);
  BIND_WATCH_STATE *ws = new_ws ();
  set_ws (ws, {300000});
  bool usable = false;

  CHECK (s.check (ws, 1.5, &usable));
  const log_entry *rep = find_log ("bind watch replan: ");
  REQUIRE (rep != NULL);
  CHECK (rep->fold == 200.0);
  CHECK (rep->text.find ("down") != std::string::npos);
}

TEST_CASE ("floor: below 1000 on both sides is never compared; at 1000 on one side it is", "[contract]")
{
  scene s (1);
  s.hist ("a");
  s.add (cmp (PT_EQ, col ("a"), hv (0)));
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;

  set_ws (ws, {10});
  s.bind (0, 900);
  CHECK_FALSE (s.check (ws, 1.5, &usable));	/* compared == 0 */
  CHECK (ws->rows[0] == 10.0);			/* nothing re-recorded */

  s.bind (0, 1000);
  CHECK (s.check (ws, 1.5, &usable));		/* 1000 / 10 */
  CHECK (ws->rows[0] == 1000.0);
}

TEST_CASE ("the predicate that moved most is the one the replan log names", "[contract]")
{
  scene s (2);
  s.hist ("a");
  s.hist ("b");
  s.add (cmp (PT_EQ, col ("a"), hv (0)));
  s.add (cmp (PT_GT, hv (1), col ("b")));	/* ? > b  ==  b < ? */
  s.bind (0, 4000);				/* 2x up */
  s.bind (1, 400);				/* 10x down */
  BIND_WATCH_STATE *ws = new_ws ();
  set_ws (ws, {2000, 4000});
  bool usable = false;

  CHECK (s.check (ws, 1.5, &usable));
  const log_entry *rep = find_log ("bind watch replan: ");
  REQUIRE (rep != NULL);
  CHECK (rep->label == "t.b<?");
  CHECK (rep->fold == 10.0);
}

TEST_CASE ("unknown on either side is skipped; with nothing comparable the check keeps the plan and the old record",
	   "[contract]")
{
  scene s (2);
  two_preds (s);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;

  set_ws (ws, {BIND_TERM_UNKNOWN, 5000});
  s.bind (0, 800000);				/* skipped: unknown when recorded */
  s.bind (1, 5100);
  CHECK_FALSE (s.check (ws, 1.1, &usable));

  set_ws (ws, {10, BIND_TERM_UNKNOWN});
  s.bind (0, 900);				/* both below the floor */
  s.bind (1, 5000);				/* unknown when recorded */
  CHECK_FALSE (s.check (ws, 1.1, &usable));	/* compared == 0 */
  CHECK (ws->rows[1] == BIND_TERM_UNKNOWN);	/* still unknown: only a replan re-records */
}

TEST_CASE ("an unpriceable value keeps its slot as BIND_TERM_UNKNOWN (the term set does not change) and is skipped",
	   "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 5000);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;
  REQUIRE (s.check (ws, 1.5, &usable));		/* first record {5000, 300} */

  s.bind_unpriceable (0);
  double rows[BIND_WATCH_MAX_TERMS];
  char names[BIND_WATCH_MAX_TERMS][BIND_WATCH_NAME_LEN];
  REQUIRE (unitseam_bw_term_vector (s.parser, s.stmt, rows, names) == 2);
  CHECK (rows[0] == BIND_TERM_UNKNOWN);
  CHECK (std::string (names[0]) == "t.a=?");

  CHECK_FALSE (s.check (ws, 1.1, &usable));
  CHECK (ws->rows[0] == 5000.0);
  CHECK (find_log ("bind watch replan") == NULL);
}

TEST_CASE ("a column whose histogram went away drops its slot: the term count changes, and that replans with the "
	   "'terms' reason", "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 5000);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;
  REQUIRE (s.check (ws, 1.5, &usable));

  g_total.erase ("b");				/* histogram_get_total_rows fails -> bind_term_add returns */
  CHECK (s.check (ws, 1.5, &usable));
  CHECK (find_log ("bind watch replan (terms): 1 priced predicate(s), the plan was chosen with 2") != NULL);
  CHECK (find_log ("bind watch replan: ") == NULL);
  CHECK (ws->terms == 1);
}

TEST_CASE ("PR8013 F22: a NULL bind value keeps its slot at 1 row, so the term set does not change -- a move to NULL "
	   "replans through the band, not through the 'terms' branch whose comment blames NULL", "[contract]")
{
  scene s (2);
  two_preds (s);
  s.bind (0, 5000);
  s.bind (1, 300);
  BIND_WATCH_STATE *ws = new_ws ();
  bool usable = false;
  REQUIRE (s.check (ws, 1.5, &usable));		/* first record {5000, 300} */

  s.bind_null (0);
  double rows[BIND_WATCH_MAX_TERMS];
  char names[BIND_WATCH_MAX_TERMS][BIND_WATCH_NAME_LEN];
  REQUIRE (unitseam_bw_term_vector (s.parser, s.stmt, rows, names) == 2);
  CHECK (rows[0] == 1.0);			/* bind_term_add (" NULL", 0.0) -> MAX (1.0, 0) */
  CHECK (std::string (names[0]) == "t.a NULL");
  CHECK (rows[1] == 300.0);			/* the next predicate stays in its slot */

  CHECK (s.check (ws, 1.5, &usable));
  CHECK (find_log ("bind watch replan (terms)") == NULL);
  const log_entry *rep = find_log ("bind watch replan: ");
  REQUIRE (rep != NULL);
  CHECK (rep->label == "t.a NULL");
  CHECK (rep->then == 5000.0);
  CHECK (rep->now == 1.0);
  CHECK (ws->terms == 2);
  CHECK (ws->rows[0] == 1.0);
}

/* ---------------------------------------------------------------- defect */

TEST_CASE ("PR8013 F15: with band <= 1.0 and nothing moved the check replans, and its log reads index -1 -- "
	   "cur.name[-1] and ws->rows[-1] (the bytes before rows) (PASS = defect)", "[defect]")
{
  for (double band : { 1.0, 0.5 })
    {
      INFO ("band " << band);
      scene s (1);
      s.hist ("a");
      s.add (cmp (PT_EQ, col ("a"), hv (0)));
      s.bind (0, 5000);
      BIND_WATCH_STATE *ws = new_ws ();
      set_ws (ws, {5000});
      /* make the 8 bytes in front of ws->rows recognizable (today: value_hash) */
      const double marker = 12345.0;
      memcpy (&ws->value_hash, &marker, sizeof (marker));
      double before_rows;
      memcpy (&before_rows, (const char *) ws->rows - sizeof (double), sizeof (double));
      bool usable = false;

      trace (true);					/* the dump hands over &cur.name[i] */
      bool replan = s.check (ws, band, &usable);
      trace (false);

      CHECK (replan);					/* nothing moved: every fold is 1.0 */
      const log_entry *dump = find_log ("bind watch dump: ");
      const log_entry *rep = find_log ("bind watch replan: ");
      REQUIRE (dump != NULL);
      REQUIRE (rep != NULL);
      CHECK (rep->fold == 1.0);
      CHECK ((const void *) rep->label_at == (const void *) (dump->label_at - BIND_WATCH_NAME_LEN));	/* cur.name[-1] */
      CHECK (memcmp (&rep->then, &before_rows, sizeof (double)) == 0);	/* ws->rows[-1] */
      CHECK (rep->then != 5000.0);
    }
}
