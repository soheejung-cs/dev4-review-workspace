/*
 * PR#8006 (CBRD-27143) -- LIKE / NOT LIKE pattern host variables in the per-predicate bind fingerprint,
 * and the unpeeked flag limited to columns that have a histogram
 * (src/optimizer/histogram/histogram_cl.cpp, cs mode). Successor of histogram_cl.like_fingerprint, which
 * was bound to bind_fp_walk (gone since CBRD-27490 620ceaf6a replaced the hashed fingerprint with the
 * per-predicate row vector bind_term_walk / bind_term_vector).
 *
 *   histogram_split_hv_predicate (TU static, hooks.cpp)  the candidate test both walks share
 *   (adc0adf4a had histogram_column_has_histogram; 2364bcc7a dropped it for histogram_get_total_rows,
 *    the same test bind_term_add gives a slot by, with the class-cache context set during the walk)
 *   histogram_bind_fingerprint -> bind_term_vector -> bind_term_walk -> bind_term_add
 *                                                        one slot per predicate: rows = MAX (1, sel * total),
 *                                                        BIND_TERM_UNKNOWN when unpriced, " NULL" slot of 1 row
 *   histogram_stmt_has_hv_predicate -> hv_pred_walk      the unpeeked flag
 *
 * Real: the walks, the candidate test, bind_term_add / bind_term_key / memo, bind_card_arg_value,
 * bind_card_range_selectivity. Fakes (see suite.json "fakes"):
 *   seams   histogram_get_like/equal/comp_selectivity, histogram_get_total_rows,
 *           histogram_blob_from_class_cache
 *   others  parser_walk_tree, pt_get_end_path_node, qo_classify, qo_between_range_histogram_selectivity,
 *           or_packed_value_size / or_pack_value (the value image bind_term_key hashes)
 *
 * Tags: [contract] expected behaviour (head FAIL + base PASS = regression candidate);
 *       [defect] reproduces a defect (PASS = the defect is there).
 */
#include "catch.hpp"
#include "dbtype_def.h"
#include "parser.h"
#include "parse_tree.h"
#include "query_planner.h"
#include "object_representation.h"
#include "histogram_cl.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

/* ---------------------------------------------------------------- fakes */

static const double TOTAL = 1048576.0;	/* 2^20 rows on every column with a histogram: sel * TOTAL is exact */

static PARSER_CONTEXT *g_parser;	/* the scene's parser: maps a bound DB_VALUE back to its index */
static DB_VALUE g_blob;		/* stands for a histogram blob; only its address is used */
static std::set<std::string> g_cache;	/* columns whose class cache holds a histogram */
static std::map<std::string, double> g_rows;	/* column -> rows its histogram was built from (default TOTAL) */
static PT_NODE *g_cache_stmt;	/* statement the last class-cache probe was given */
static std::map<int, double> g_sel;	/* host variable index -> the estimate the histogram reports */
static std::deque<std::string> g_text;	/* backing store of bound strings (stable addresses) */

static int g_cache_calls;	/* histogram_blob_from_class_cache calls */
static int g_like_calls;	/* histogram_get_like_selectivity calls */
static std::string g_like_column;	/* column node the last LIKE probe received */
static int g_like_hv;		/* host variable index of the DB_VALUE the last LIKE probe received */
static int g_cmp_calls;		/* histogram_get_equal/comp_selectivity calls */

static int
hv_index_of (const DB_VALUE *v)
{
  return (g_parser != NULL && v != NULL) ? (int) (v - g_parser->host_variables) : -1;
}

/* parser_support.c pt_get_end_path_node, verbatim */
PT_NODE *
pt_get_end_path_node (PT_NODE *node)
{
  while (node != NULL && node->node_type == PT_DOT_)
    {
      node = node->info.dot.arg2;
    }
  return node;
}

PARSER_CONTEXT *unitseam_fp_active_parser (void);
PT_NODE *unitseam_fp_active_statement (void);
DB_VALUE *histogram_blob_from_class_cache (PARSER_CONTEXT *parser, PT_NODE *statement, PT_NODE *name);

/* histogram_init_reader_from_lhs (histogram_cl.cpp:724): end of the path, a PT_NAME, the name-node
 * annotation; else, when bind_fp_active_statement is set (a fingerprint walk, and since 2364bcc7a the
 * unpeeked-flag walk), histogram_blob_from_class_cache, annotating the name on a hit (the real one
 * annotates a parser-arena copy). *rows = the histogram's total rows (header). */
static bool
reader_init (PT_NODE *lhs, double *rows)
{
  PT_NODE *name = pt_get_end_path_node (lhs);

  if (name == NULL || name->node_type != PT_NAME)
    {
      return false;
    }
  if (name->info.name.histogram == NULL && unitseam_fp_active_statement () != NULL)
    {
      DB_VALUE *blob = histogram_blob_from_class_cache (unitseam_fp_active_parser (),
							unitseam_fp_active_statement (), name);
      if (blob != NULL && unitseam_fp_active_parser () != NULL)
	{
	  name->info.name.histogram = blob;
	}
    }
  if (name->info.name.histogram == NULL)
    {
      return false;
    }
  auto r = g_rows.find (name->info.name.original != NULL ? name->info.name.original : "");
  *rows = (r == g_rows.end ()) ? TOTAL : r->second;
  return true;
}

/* a histogram the estimators can price with: present and built over some rows (the LIKE / equality
 * probes give up on total_rows <= 0) */
static bool
column_has_histogram (PT_NODE *lhs)
{
  double rows = 0.0;

  return reader_init (lhs, &rows) && rows > 0.0;
}

/* db_value_type: a NULL value reports DB_TYPE_NULL whatever its type field */
static DB_TYPE
value_type (const DB_VALUE *v)
{
  return v->domain.general_info.is_null ? DB_TYPE_NULL : (DB_TYPE) v->domain.general_info.type;
}

/* histogram_blob_from_class_cache (histogram_cl.cpp:666 at head): spec -> class -> au_fetch_class ->
 * class_->histogram (stats_get_histogram fetches once and leaves attr_ids -1 for columns without
 * one) -> the column's blob, else NULL */
DB_VALUE *
histogram_blob_from_class_cache (PARSER_CONTEXT *parser, PT_NODE *statement, PT_NODE *name)
{
  g_cache_calls++;
  g_cache_stmt = statement;
  if (name == NULL || name->info.name.original == NULL)
    {
      return NULL;
    }
  return g_cache.count (name->info.name.original) > 0 ? &g_blob : NULL;
}

/* histogram_get_total_rows (histogram_cl.cpp:1541): reader init fails -> false; else the header's
 * total rows, true when > 0 */
bool
histogram_get_total_rows (PT_NODE *lhs, double *total_rows)
{
  double rows = 0.0;

  if (!reader_init (lhs, &rows))
    {
      return false;
    }
  *total_rows = rows;
  return *total_rows > 0.0;
}

/* histogram_get_like_selectivity (histogram_cl.cpp:2110 at head). Its "cannot price" exits: rhs NULL,
 * no histogram, DB_VALUE_TYPE neither STRING nor CHAR (a NULL value, an uncast INTEGER), empty
 * pattern. Otherwise success with the estimate -- here the test's value per host variable. The real
 * estimate is NOT clamped to 1 (MCV sum / nonnull fraction, then MAX (1/N, sel)). */
void
histogram_get_like_selectivity (PT_NODE *lhs, DB_VALUE *rhs_db_value, double *selectivity, bool *success)
{
  PT_NODE *name = pt_get_end_path_node (lhs);

  g_like_calls++;
  g_like_column = (name != NULL && name->node_type == PT_NAME && name->info.name.original != NULL)
    ? name->info.name.original : "";
  g_like_hv = hv_index_of (rhs_db_value);

  *success = false;
  if (rhs_db_value == NULL || !column_has_histogram (lhs))
    {
      return;
    }
  if (value_type (rhs_db_value) != DB_TYPE_STRING && value_type (rhs_db_value) != DB_TYPE_CHAR)
    {
      return;
    }
  if (rhs_db_value->data.ch.medium.size <= 0)
    {
      return;
    }
  auto p = g_sel.find (g_like_hv);
  if (p == g_sel.end ())
    {
      return;
    }
  *selectivity = p->second;
  *success = true;
}

/* histogram_get_equal/comp_selectivity (histogram_cl.cpp:1181/1377 at head): rhs NULL, no histogram, or
 * a value histogram_extract_key cannot key (DB_TYPE_NULL falls to its default) -> success false */
static void
fake_price (PT_NODE *lhs, DB_VALUE *rhs, double *selectivity, bool *success)
{
  g_cmp_calls++;
  *success = false;
  if (rhs == NULL || !column_has_histogram (lhs) || value_type (rhs) == DB_TYPE_NULL)
    {
      return;
    }
  auto p = g_sel.find (hv_index_of (rhs));
  if (p == g_sel.end ())
    {
      return;
    }
  *selectivity = p->second;
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

/* query_planner.c qo_between_range_histogram_selectivity: the one-sided operators are a single
 * comparison probe on arg1 (GT_INF / GE_INF / INF_LT / INF_LE); only those are built here */
bool
qo_between_range_histogram_selectivity (PT_NODE *lhs, PT_OP_TYPE op_type, DB_VALUE *arg1_val, DB_VALUE *arg2_val,
					double *out_sel)
{
  bool ok = false;

  switch (op_type)
    {
    case PT_BETWEEN_GT_INF:
    case PT_BETWEEN_GE_INF:
    case PT_BETWEEN_INF_LT:
    case PT_BETWEEN_INF_LE:
      fake_price (lhs, arg1_val, out_sel, &ok);
      return ok;
    default:
      return false;
    }
}

/* query_planner.c qo_classify, the cases these trees reach */
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

/* object_representation.c: the packed image bind_value_image_mix hashes (bind_term_key, the memo
 * key). Only "equal values -> equal image, different values -> different image" matters here: the
 * type, then the string bytes or the integer. */
int
or_packed_value_size (const DB_VALUE *value, int collapse_null, int include_domain, int include_domain_classoids)
{
  DB_TYPE t = value_type (value);

  if (t == DB_TYPE_STRING || t == DB_TYPE_CHAR)
    {
      return 8 + value->data.ch.medium.size;
    }
  return 8 + (int) sizeof (int);
}

char *
or_pack_value (char *buf, DB_VALUE *value)
{
  DB_TYPE t = value_type (value);
  int ti = (int) t;

  memcpy (buf, &ti, sizeof (ti));
  if (t == DB_TYPE_STRING || t == DB_TYPE_CHAR)
    {
      memcpy (buf + 8, value->data.ch.medium.buf, value->data.ch.medium.size);
      return buf + 8 + value->data.ch.medium.size;
    }
  memcpy (buf + 8, &value->data.i, sizeof (int));
  return buf + 8 + sizeof (int);
}

/* parse_tree_cl.c pt_walk_private, for the node kinds built here: pre; then (CONTINUE/LEAF) the
 * sub-trees -- PT_EXPR arg1, arg2, arg3 -- ; then or_next; then (CONTINUE/LIST) the rest of the
 * `next` list; then post. PT_APPLY_WALK skips NULL and stops on PT_STOP_WALK. */
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

/* ---------------------------------------------------------------- appendix hooks */
bool unitseam_split_hv_predicate (PARSER_CONTEXT *parser, PT_NODE *node, PT_NODE **out_name, PT_NODE **out_hv,
				  bool *out_reversed);

/* ---------------------------------------------------------------- builders */

static PT_NODE *
mk (PT_NODE_TYPE t)
{
  PT_NODE *n = (PT_NODE *) calloc (1, sizeof (PT_NODE));
  n->node_type = t;
  return n;
}

/* a resolved column of spec 1; `annotated` = the planner left the histogram blob on the node */
static PT_NODE *
col (const char *name, bool annotated = false)
{
  PT_NODE *n = mk (PT_NAME);
  n->info.name.original = name;
  n->info.name.resolved = "t";
  n->info.name.spec_id = 1;
  n->info.name.histogram = annotated ? &g_blob : NULL;
  return n;
}

/* t.col kept as a path chain */
static PT_NODE *
dot (const char *name, bool annotated = false)
{
  PT_NODE *d = mk (PT_DOT_);
  d->info.dot.arg1 = col ("t");
  d->info.dot.arg2 = col (name, annotated);
  return d;
}

static PT_NODE *
hv (int index)
{
  PT_NODE *n = mk (PT_HOST_VAR);
  n->info.host_var.index = index;
  return n;
}

static PT_NODE *
literal ()
{
  PT_NODE *n = mk (PT_VALUE);
  n->type_enum = PT_TYPE_VARCHAR;
  return n;
}

static PT_NODE *
ex (PT_OP_TYPE op, PT_NODE *a, PT_NODE *b, PT_NODE *c = NULL)
{
  PT_NODE *n = mk (PT_EXPR);
  n->info.expr.op = op;
  n->info.expr.arg1 = a;
  n->info.expr.arg2 = b;
  n->info.expr.arg3 = c;
  return n;
}

/* `col LIKE ? ESCAPE c` as the type checker leaves it: PT_LIKE (col, PT_LIKE_ESCAPE (?, c)) */
static PT_NODE *
like_escape (PT_OP_TYPE op, PT_NODE *column, PT_NODE *pattern)
{
  return ex (op, column, ex (PT_LIKE_ESCAPE, pattern, literal ()));
}

/* `col RANGE (? GT_INF max)` as qo_convert_to_range makes of `col > ?` */
static PT_NODE *
range_gt (PT_NODE *column, PT_NODE *bound)
{
  return ex (PT_RANGE, column, ex (PT_BETWEEN_GT_INF, bound, NULL));
}

static void
set_null (DB_VALUE *v)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_NULL;
  v->domain.general_info.is_null = 1;
}

static void
set_int (DB_VALUE *v, int x)
{
  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_INTEGER;
  v->data.i = x;
}

/* db_value_domain_init (VARCHAR) + db_make_db_char: fields set directly */
static void
set_str (DB_VALUE *v, const char *s)
{
  g_text.emplace_back (s);
  const std::string &k = g_text.back ();

  memset (v, 0, sizeof (*v));
  v->domain.general_info.type = DB_TYPE_STRING;
  v->domain.general_info.is_null = 0;
  v->domain.char_info.length = DB_MAX_VARCHAR_PRECISION;
  v->data.ch.info.style = MEDIUM_STRING;
  v->data.ch.medium.size = (int) k.size ();
  v->data.ch.medium.buf = k.data ();
  v->data.ch.medium.compressed_buf = NULL;
  v->data.ch.medium.compressed_size = DB_NOT_YET_COMPRESSED;
  v->data.ch.medium.length = -1;
}

/* the slot vector of one fingerprint */
struct slots
{
  bool ok = false;
  int n = 0;
  double rows[BIND_WATCH_MAX_TERMS] = { };
  std::string name[BIND_WATCH_MAX_TERMS];
};

/* one statement over t: a WHERE list of predicates, `nhv` user host variables (+ `nauto`
 * auto-parameterized literals after them) */
struct scene
{
  PARSER_CONTEXT *parser;
  PT_NODE *stmt = NULL, *tail = NULL;

  explicit scene (int nhv, int nauto = 0)
  {
    parser = (PARSER_CONTEXT *) calloc (1, sizeof (PARSER_CONTEXT));
    parser->host_variables = (DB_VALUE *) calloc (nhv + nauto + 1, sizeof (DB_VALUE));
    parser->host_var_count = nhv;
    parser->auto_param_count = nauto;
    for (int i = 0; i < nhv + nauto; i++)
      {
	set_null (&parser->host_variables[i]);
      }
    g_parser = parser;
    g_cache.clear ();
    g_rows.clear ();
    g_sel.clear ();
    reset_counters ();
  }
  static void reset_counters ()
  {
    g_cache_calls = 0;
    g_cache_stmt = NULL;
    g_like_calls = 0;
    g_cmp_calls = 0;
    g_like_column.clear ();
    g_like_hv = -2;
  }
  void hist (const char *column)	/* the class cache holds a histogram for this column */
  {
    g_cache.insert (column);
  }
  void rows (const char *column, double n)	/* the column's histogram was built over n rows */
  {
    g_rows[column] = n;
  }
  PT_NODE *add (PT_NODE *pred)
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
    return pred;
  }
  void bind (int i, const char *v, double sel)	/* a value the histogram prices at sel */
  {
    set_str (&parser->host_variables[i], v);
    g_sel[i] = sel;
  }
  void bind_null (int i)
  {
    set_null (&parser->host_variables[i]);
    g_sel.erase (i);
  }
  void bind_int (int i, int x, double sel)
  {
    set_int (&parser->host_variables[i], x);
    g_sel[i] = sel;
  }
  slots fp (BIND_TERM_MEMO *memo = NULL)
  {
    slots s;
    BIND_FINGERPRINT f;
    char names[BIND_WATCH_MAX_TERMS][BIND_WATCH_NAME_LEN];

    memset (&f, 0, sizeof (f));
    memset (names, 0, sizeof (names));
    s.ok = histogram_bind_fingerprint (parser, stmt, &f, names, memo);
    s.n = f.terms;
    for (int i = 0; i < f.terms && i < BIND_WATCH_MAX_TERMS; i++)
      {
	s.rows[i] = f.rows[i];
	s.name[i] = names[i];
      }
    return s;
  }
  bool flagged ()
  {
    return histogram_stmt_has_hv_predicate (parser, stmt);
  }
};

/* the single slot of `s <op> ?` (s has a histogram) with the LIKE estimator reporting like_sel */
static slots
like_one (PT_OP_TYPE op, double like_sel, const char *pattern = "abc%")
{
  scene s (1);
  s.hist ("s");
  s.add (ex (op, col ("s"), hv (0)));
  s.bind (0, pattern, like_sel);
  return s.fp ();
}

/* ================================================================ A. histogram_split_hv_predicate */

TEST_CASE ("PR8006 split: `col LIKE ?` and `col NOT LIKE ?` with a user host variable are candidates "
	   "(column node, host variable node, not reversed)", "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      PT_NODE *c = col ("s"), *h = hv (0), *name = NULL, *var = NULL;
      bool rev = true;

      INFO ("op " << (op == PT_LIKE ? "LIKE" : "NOT LIKE"));
      CHECK (unitseam_split_hv_predicate (s.parser, ex (op, c, h), &name, &var, &rev));
      CHECK (name == c);
      CHECK (var == h);
      CHECK_FALSE (rev);
    }
}

TEST_CASE ("PR8006 split: `t.col LIKE ?` (path chain) hands back the end-of-path column", "[contract][split]")
{
  scene s (1);
  PT_NODE *d = dot ("s"), *name = NULL, *var = NULL;

  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_NOT_LIKE, d, hv (0)), &name, &var, NULL));
  CHECK (name == d->info.dot.arg2);
}

TEST_CASE ("PR8006 split: `? LIKE col` / `? NOT LIKE t.col` (the column is the pattern) is not a candidate, "
	   "out-params reset", "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      for (int path = 0; path < 2; path++)
	{
	  PT_NODE *name = (PT_NODE *) 1, *var = (PT_NODE *) 1;
	  bool rev = true;

	  INFO ("op " << (op == PT_LIKE ? "LIKE" : "NOT LIKE") << " path " << path);
	  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, hv (0), path ? dot ("s") : col ("s")), &name,
						    &var, &rev));
	  CHECK (name == NULL);
	  CHECK (var == NULL);
	  CHECK_FALSE (rev);
	}
    }
}

TEST_CASE ("PR8006 split: `col LIKE ? ESCAPE c` (arg2 = PT_LIKE_ESCAPE (?, c)) is not a candidate, nor is the "
	   "escape node itself", "[contract][split]")
{
  scene s (1);
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      PT_NODE *e = like_escape (op, col ("s"), hv (0));

      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, e, NULL, NULL, NULL));
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, e->info.expr.arg2, NULL, NULL, NULL));
    }
}

TEST_CASE ("PR8006 split: LIKE with a literal pattern, an auto-parameterized literal, another column or two "
	   "host variables is not a candidate", "[contract][split]")
{
  scene s (2, 1);		/* ?0 ?1 user, index 2 auto-parameterized */

  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, col ("s"), literal ()), NULL, NULL, NULL));
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, col ("s"), hv (2)), NULL, NULL, NULL));
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, col ("s"), col ("u")), NULL, NULL, NULL));
      CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (op, hv (0), hv (1)), NULL, NULL, NULL));
    }
}

TEST_CASE ("PR8006 split: RLIKE / NOT RLIKE stay outside the candidate set", "[contract][split]")
{
  scene s (1);

  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_RLIKE, col ("s"), hv (0)), NULL, NULL, NULL));
  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_NOT_RLIKE, col ("s"), hv (0)), NULL, NULL, NULL));
}

TEST_CASE ("PR8006 split control: comparisons unchanged -- `col = ?` plain, `? < col` reversed, NE not a candidate",
	   "[contract][split]")
{
  scene s (1);
  PT_NODE *c = col ("a"), *h = hv (0), *name = NULL, *var = NULL;
  bool rev = true;

  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_EQ, c, h), &name, &var, &rev));
  CHECK (name == c);
  CHECK_FALSE (rev);

  PT_NODE *c2 = col ("a"), *h2 = hv (0);
  CHECK (unitseam_split_hv_predicate (s.parser, ex (PT_LT, h2, c2), &name, &var, &rev));
  CHECK (name == c2);
  CHECK (var == h2);
  CHECK (rev);

  CHECK_FALSE (unitseam_split_hv_predicate (s.parser, ex (PT_NE, col ("a"), hv (0)), NULL, NULL, NULL));
}

/* ================================================================ B. flag column test (hv_pred_walk) */

TEST_CASE ("PR8006 flag: an unannotated column resolves through the class cache during the flag walk (given "
	   "the statement), the annotation is used without a probe, and the walk leaves no ambient context",
	   "[contract][flag]")
{
  {
    scene s (1);
    s.hist ("b");
    s.add (ex (PT_EQ, col ("b"), hv (0)));
    CHECK (s.flagged ());
    CHECK (g_cache_calls == 1);
    CHECK (g_cache_stmt == s.stmt);
    CHECK (unitseam_fp_active_statement () == NULL);
    CHECK (unitseam_fp_active_parser () == NULL);
  }
  {
    scene s (1);
    s.add (ex (PT_EQ, col ("a", true), hv (0)));
    CHECK (s.flagged ());
    CHECK (g_cache_calls == 0);
  }
}

TEST_CASE ("PR8006 flag: a path chain resolves to its end node (`t.a = ?`, `? < t.a`, `t.s LIKE ?`)",
	   "[contract][flag]")
{
  for (int k = 0; k < 3; k++)
    {
      scene s (1);
      s.add (k == 0 ? ex (PT_EQ, dot ("a", true), hv (0))
	     : k == 1 ? ex (PT_LT, hv (0), dot ("a", true)) : ex (PT_LIKE, dot ("s", true), hv (0)));
      INFO ("shape " << k);
      CHECK (s.flagged ());
    }
}

TEST_CASE ("PR8006 flag: a candidate on a column whose histogram was collected over no rows (total_rows 0) does "
	   "not flag -- annotated or from the class cache -- and takes no slot", "[contract][flag]")
{
  for (int annotated = 0; annotated < 2; annotated++)
    {
      for (PT_OP_TYPE op : { PT_EQ, PT_LIKE, PT_RANGE })
	{
	  scene s (1);
	  s.hist ("z");
	  s.rows ("z", 0.0);
	  s.add (op == PT_RANGE ? range_gt (col ("z", annotated), hv (0)) : ex (op, col ("z", annotated), hv (0)));
	  s.bind (0, "abc%", 0.25);
	  INFO ("annotated " << annotated << " op " << (int) op);
	  CHECK_FALSE (s.flagged ());
	  slots r = s.fp ();
	  CHECK_FALSE (r.ok);
	  CHECK (r.n == 0);
	}
    }
}

TEST_CASE ("PR8006 flag: a statement is flagged exactly when its fingerprint gets a slot (usable) -- no "
	   "histogram, empty histogram, annotated, class cache, NULL / unpriceable / priced values, ESCAPE, "
	   "`? LIKE col`, auto-parameterized, RLIKE, range, mixed", "[contract][flag]")
{
  for (int k = 0; k < 16; k++)
    {
      scene s (2, 1);		/* ?0 ?1 user, index 2 auto-parameterized */
      s.hist ("c");
      s.hist ("z");
      s.rows ("z", 0.0);
      s.bind (0, "abc%", 0.25);
      s.bind (1, "k", 0.5);
      s.bind (2, "lit%", 0.125);
      switch (k)
	{
	case 0:
	  s.add (ex (PT_EQ, col ("n"), hv (0)));
	  break;		/* no histogram */
	case 1:
	  s.add (ex (PT_LIKE, col ("z"), hv (0)));
	  break;		/* histogram over no rows */
	case 2:
	  s.add (ex (PT_LIKE, col ("a", true), hv (0)));
	  break;		/* annotated */
	case 3:
	  s.add (ex (PT_NOT_LIKE, col ("c"), hv (0)));
	  break;		/* class cache */
	case 4:
	  s.add (ex (PT_LIKE, col ("c"), hv (0)));
	  s.bind_null (0);
	  break;		/* NULL pattern */
	case 5:
	  s.add (ex (PT_LIKE, col ("c"), hv (0)));
	  s.bind_int (0, 7, 0.25);
	  break;		/* unpriceable: UNKNOWN slot */
	case 6:
	  s.add (like_escape (PT_LIKE, col ("c"), hv (0)));
	  break;
	case 7:
	  s.add (ex (PT_LIKE, hv (0), col ("c")));
	  break;
	case 8:
	  s.add (ex (PT_LIKE, col ("c"), hv (2)));
	  break;		/* auto-parameterized */
	case 9:
	  s.add (ex (PT_RLIKE, col ("c"), hv (0)));
	  break;
	case 10:
	  s.add (range_gt (col ("c"), hv (1)));
	  break;
	case 11:
	  s.add (range_gt (col ("z"), hv (1)));
	  break;
	case 12:
	  s.add (range_gt (col ("c"), hv (2)));
	  break;		/* range on an auto-parameterized bound only */
	case 13:
	  s.add (ex (PT_EQ, col ("n"), hv (1)));
	  s.add (ex (PT_GT, hv (0), col ("z")));
	  break;		/* only unusable columns */
	case 14:
	  s.add (ex (PT_EQ, col ("n"), hv (1)));
	  s.add (ex (PT_LIKE, col ("c"), hv (0)));
	  break;		/* a usable one after unusable */
	default:
	  s.add (ex (PT_GE, hv (1), col ("c")));
	  s.bind_null (1);
	  break;		/* reversed comparison with NULL */
	}
      bool flag = s.flagged ();
      slots r = s.fp ();

      INFO ("scene " << k << " flagged " << flag << " slots " << r.n);
      CHECK (flag == (r.n > 0));
    }
}

/* ================================================================ C. bind_term_walk (per-predicate rows) */

TEST_CASE ("PR8006 term: `s LIKE ?` takes a slot priced by histogram_get_like_selectivity on the column node and "
	   "host_variables[index] -- rows = sel * total, label ` LIKE ?`", "[contract][term]")
{
  scene s (2);
  s.hist ("s");
  s.add (ex (PT_LIKE, col ("s"), hv (1)));
  s.bind (1, "abc%", 0.25);
  slots r = s.fp ();

  CHECK (r.ok);
  REQUIRE (r.n == 1);
  CHECK (r.rows[0] == 0.25 * TOTAL);
  CHECK (r.name[0] == "t.s LIKE ?");
  CHECK (g_like_calls == 1);
  CHECK (g_like_column == "s");
  CHECK (g_like_hv == 1);
}

TEST_CASE ("PR8006 term: `s NOT LIKE ?` is priced on 1 - LIKE (the complement qo_expr_selectivity applies), "
	   "label ` NOT LIKE ?`", "[contract][term]")
{
  for (double like : { 0.25, 0.75, 1.0 / 1024 })
    {
      slots r = like_one (PT_NOT_LIKE, like);

      INFO ("LIKE " << like);
      CHECK (r.ok);
      REQUIRE (r.n == 1);
      CHECK (r.rows[0] == (1.0 - like) * TOTAL);
      CHECK (r.name[0] == "t.s NOT LIKE ?");
    }
}

TEST_CASE ("PR8006 term: a LIKE estimate tiny enough to round below one row is floored at 1 row "
	   "(LIKE 2^-30, NOT LIKE of a pattern every row matches: 1 - 1.0 = 0)", "[contract][term][G1]")
{
  slots tiny = like_one (PT_LIKE, std::ldexp (1.0, -30));
  slots none = like_one (PT_NOT_LIKE, 1.0);

  REQUIRE (tiny.n == 1);
  REQUIRE (none.n == 1);
  CHECK (tiny.rows[0] == 1.0);
  CHECK (none.rows[0] == 1.0);	/* not BIND_TERM_UNKNOWN: priced, matches nothing */
}

TEST_CASE ("PR8006 U1: NOT LIKE of a LIKE estimate just above 1.0 (1 + 2^-52) takes an UNKNOWN slot, while the "
	   "planner clamps LIKE to 1 first and prices it at 0 -- [defect] latent, PASS = present",
	   "[defect][term][G1]")
{
  /* reachability: histogram_get_like_selectivity returns (Σ matched MCV freq + non-MCV mass * x) /
   * (1 - nullfrac), then MAX (1/N, ·) -- no clamp to 1. A column with no NULLs whose every value is an
   * MCV and whose frequencies (count / sample, histogram_sampler_sr.cpp:357) sum above 1 by rounding --
   * three values with counts 9, 18, 1 of 28 rows, summed in value order -- has nonmcv_mass clamped to 0
   * and a pattern matching every value (`%`) returns that sum. qo_like_selectivity clamps it to 1
   * (query_planner.c MIN (selectivity, 1.0)) and qo_expr_selectivity takes 1 - 1 = 0; bind_term_walk
   * takes 1 - sum < 0, which bind_term_add records as BIND_TERM_UNKNOWN. */
  double sum = 0.0;
  for (int c : { 9, 18, 1 })
    {
      sum += (double) c / 28.0;
    }
  REQUIRE (sum > 1.0);
  REQUIRE (sum == std::nextafter (1.0, 2.0));

  slots r = like_one (PT_NOT_LIKE, sum, "%");

  REQUIRE (r.n == 1);
  CHECK (r.rows[0] == BIND_TERM_UNKNOWN);
}

TEST_CASE ("PR8006 term: a LIKE estimate above 1 (1 + 2^-52) is clamped as qo_like_selectivity does -- LIKE "
	   "= every row, NOT LIKE = 0 -> 1 row", "[contract][term][G1]")
{
  const double over = std::nextafter (1.0, 2.0);
  slots like = like_one (PT_LIKE, over, "%");
  slots not_like = like_one (PT_NOT_LIKE, over, "%");

  REQUIRE (like.n == 1);
  REQUIRE (not_like.n == 1);
  CHECK (like.rows[0] == TOTAL);
  CHECK (not_like.rows[0] == 1.0);
}

TEST_CASE ("PR8006 term: a NULL pattern takes a ` NULL` slot of 1 row for LIKE and NOT LIKE, and the predicate "
	   "after it keeps its slot", "[contract][term][null]")
{
  for (PT_OP_TYPE op : { PT_LIKE, PT_NOT_LIKE })
    {
      scene s (2);
      s.hist ("s");
      s.hist ("a");
      s.add (ex (op, col ("s"), hv (0)));
      s.add (ex (PT_EQ, col ("a"), hv (1)));
      s.bind_null (0);
      s.bind (1, "k", 0.125);
      slots r = s.fp ();

      INFO ("op " << (op == PT_LIKE ? "LIKE" : "NOT LIKE"));
      REQUIRE (r.n == 2);
      CHECK (r.rows[0] == 1.0);
      CHECK (r.name[0] == "t.s NULL");
      CHECK (r.rows[1] == 0.125 * TOTAL);
      CHECK (r.name[1] == "t.a=?");
      CHECK (g_like_calls == 0);	/* a NULL is not sent to the estimator */
    }
}

TEST_CASE ("PR8006 term: a pattern the estimator cannot price (uncast INTEGER, empty string) keeps an UNKNOWN "
	   "slot -- asked once, declined", "[contract][term][null]")
{
  for (int variant = 0; variant < 2; variant++)
    {
      scene s (1);
      s.hist ("s");
      s.add (ex (PT_LIKE, col ("s"), hv (0)));
      if (variant == 0)
	{
	  s.bind_int (0, 123, 0.25);
	}
      else
	{
	  s.bind (0, "", 0.25);
	}
      slots r = s.fp ();

      INFO ("variant " << (variant == 0 ? "INTEGER" : "empty string"));
      CHECK (r.ok);
      REQUIRE (r.n == 1);
      CHECK (r.rows[0] == BIND_TERM_UNKNOWN);
      CHECK (r.name[0] == "t.s LIKE ?");
      CHECK (g_like_calls == 1);
    }
}

TEST_CASE ("PR8006 term: the ESCAPE form, `? LIKE col` and an auto-parameterized pattern never reach the LIKE "
	   "estimator and take no slot", "[contract][term]")
{
  {
    scene s (1);
    s.hist ("s");
    s.add (like_escape (PT_LIKE, col ("s"), hv (0)));
    s.bind (0, "a\\%%", 0.25);
    slots r = s.fp ();
    CHECK_FALSE (r.ok);
    CHECK (r.n == 0);
    CHECK (g_like_calls == 0);
  }
  {
    scene s (1);
    s.hist ("s");
    s.add (ex (PT_NOT_LIKE, hv (0), col ("s")));
    s.bind (0, "abc", 0.25);
    slots r = s.fp ();
    CHECK_FALSE (r.ok);
    CHECK (g_like_calls == 0);
  }
  {
    scene s (0, 1);
    s.hist ("s");
    s.add (ex (PT_LIKE, col ("s"), hv (0)));
    s.bind (0, "abc%", 0.25);
    slots r = s.fp ();
    CHECK_FALSE (r.ok);
    CHECK (g_like_calls == 0);
  }
}

TEST_CASE ("PR8006 term: LIKE on a column without a histogram takes no slot, priced or NULL", "[contract][term]")
{
  {
    scene s (1);
    s.add (ex (PT_LIKE, col ("s"), hv (0)));
    s.bind (0, "abc%", 0.25);
    slots r = s.fp ();
    CHECK_FALSE (r.ok);
    CHECK (r.n == 0);
  }
  {
    scene s (1);
    s.add (ex (PT_NOT_LIKE, col ("s"), hv (0)));
    s.bind_null (0);
    slots r = s.fp ();
    CHECK_FALSE (r.ok);
    CHECK (r.n == 0);
  }
}

TEST_CASE ("PR8006 term: slots follow tree order -- `a = ?0 AND t.s LIKE ?1 AND b NOT LIKE ?2 AND c > ?3`",
	   "[contract][term]")
{
  scene s (4);
  for (const char *c : { "a", "s", "b", "c" })
    {
      s.hist (c);
    }
  s.add (ex (PT_EQ, col ("a"), hv (0)));
  s.add (ex (PT_LIKE, dot ("s"), hv (1)));
  s.add (ex (PT_NOT_LIKE, col ("b"), hv (2)));
  s.add (range_gt (col ("c"), hv (3)));
  s.bind (0, "k", 0.0625);
  s.bind (1, "ab%", 0.5);
  s.bind (2, "%z", 0.375);
  s.bind (3, "m", 0.125);
  slots r = s.fp ();

  REQUIRE (r.n == 4);
  CHECK (r.name[0] == "t.a=?");
  CHECK (r.rows[0] == 0.0625 * TOTAL);
  CHECK (r.name[1] == "t.s LIKE ?");
  CHECK (r.rows[1] == 0.5 * TOTAL);
  CHECK (r.name[2] == "t.b NOT LIKE ?");
  CHECK (r.rows[2] == 0.625 * TOTAL);
  CHECK (r.name[3] == "t.c RANGE");
  CHECK (r.rows[3] == 0.125 * TOTAL);
}

TEST_CASE ("PR8006 term: the memo reuses a LIKE slot priced under the same pattern and re-prices a changed one",
	   "[contract][term][memo]")
{
  scene s (1);
  s.add (ex (PT_LIKE, col ("s", true), hv (0)));
  BIND_TERM_MEMO *memo = (BIND_TERM_MEMO *) calloc (1, sizeof (BIND_TERM_MEMO));

  s.bind (0, "abc%", 0.25);
  slots first = s.fp (memo);
  REQUIRE (first.n == 1);
  CHECK (g_like_calls == 1);

  g_sel[0] = 0.5;		/* same pattern: the estimate is not asked again */
  slots again = s.fp (memo);
  REQUIRE (again.n == 1);
  CHECK (g_like_calls == 1);
  CHECK (again.rows[0] == 0.25 * TOTAL);
  CHECK (again.name[0] == "t.s LIKE ?");

  s.bind (0, "abd%", 0.5);
  slots changed = s.fp (memo);
  REQUIRE (changed.n == 1);
  CHECK (g_like_calls == 2);
  CHECK (changed.rows[0] == 0.5 * TOTAL);
  free (memo);
}

TEST_CASE ("PR8006 term: LIKE and NOT LIKE of the same pattern on the same slot do not share a memo entry "
	   "(the operator is in the key)", "[contract][term][memo]")
{
  BIND_TERM_MEMO *memo = (BIND_TERM_MEMO *) calloc (1, sizeof (BIND_TERM_MEMO));
  PT_NODE *c = col ("s", true);
  PT_NODE *like = ex (PT_LIKE, c, hv (0));
  PT_NODE *not_like = ex (PT_NOT_LIKE, c, hv (0));
  scene s (1);

  s.bind (0, "abc%", 0.25);
  s.stmt = like;
  slots a = s.fp (memo);
  s.stmt = not_like;
  slots b = s.fp (memo);

  REQUIRE (a.n == 1);
  REQUIRE (b.n == 1);
  CHECK (g_like_calls == 2);
  CHECK (a.rows[0] == 0.25 * TOTAL);
  CHECK (b.rows[0] == 0.75 * TOTAL);
  free (memo);
}

TEST_CASE ("PR8006 term control: comparison slots unchanged -- `a = ?` exact rows, `? < b` labelled `>?`, "
	   "NULL comparison 1 row, unpriceable UNKNOWN", "[contract][term]")
{
  scene s (4);
  s.hist ("a");
  s.hist ("b");
  s.add (ex (PT_EQ, col ("a"), hv (0)));
  s.add (ex (PT_LT, hv (1), col ("b")));
  s.add (ex (PT_GE, col ("a"), hv (2)));
  s.add (ex (PT_LE, col ("b"), hv (3)));
  s.bind (0, "k", 0.0078125);
  s.bind (1, "m", 0.5);
  s.bind_null (2);
  set_int (&s.parser->host_variables[3], 7);	/* no estimate for ?3: the probe declines */
  slots r = s.fp ();

  REQUIRE (r.n == 4);
  CHECK (r.rows[0] == 0.0078125 * TOTAL);
  CHECK (r.name[0] == "t.a=?");
  CHECK (r.rows[1] == 0.5 * TOTAL);
  CHECK (r.name[1] == "t.b>?");
  CHECK (r.rows[2] == 1.0);
  CHECK (r.name[2] == "t.a NULL");
  CHECK (r.rows[3] == BIND_TERM_UNKNOWN);
  CHECK (r.name[3] == "t.b<=?");
  CHECK (g_like_calls == 0);
}

/* ================================================================ D. hv_pred_walk (unpeeked flag) */

TEST_CASE ("PR8006 G2: a candidate on a column with a histogram flags the plan -- from the name-node annotation "
	   "without a catalog probe, else from the class cache", "[contract][G2]")
{
  {
    scene s (1);
    s.add (ex (PT_EQ, col ("a", true), hv (0)));
    CHECK (s.flagged ());
    CHECK (g_cache_calls == 0);
  }
  {
    scene s (1);
    s.hist ("a");
    s.add (ex (PT_EQ, col ("a"), hv (0)));
    CHECK (s.flagged ());
  }
}

TEST_CASE ("PR8006 G2: a candidate on a column without a histogram does not flag (`a = ?`, `? < a`, "
	   "`b RANGE (? GT_INF)`, `s LIKE ?`, `s NOT LIKE ?`)", "[contract][G2]")
{
  for (int k = 0; k < 5; k++)
    {
      scene s (1);
      switch (k)
	{
	case 0:
	  s.add (ex (PT_EQ, col ("a"), hv (0)));
	  break;
	case 1:
	  s.add (ex (PT_LT, hv (0), col ("a")));
	  break;
	case 2:
	  s.add (range_gt (col ("b"), hv (0)));
	  break;
	case 3:
	  s.add (ex (PT_LIKE, col ("s"), hv (0)));
	  break;
	default:
	  s.add (ex (PT_NOT_LIKE, col ("s"), hv (0)));
	  break;
	}
      INFO ("shape " << k);
      CHECK_FALSE (s.flagged ());
    }
}

TEST_CASE ("PR8006 G2: the walk goes past a histogram-less candidate to a later one that has a histogram",
	   "[contract][G2]")
{
  scene s (2);
  s.hist ("b");
  s.add (ex (PT_EQ, col ("a"), hv (0)));
  s.add (range_gt (col ("b"), hv (1)));

  CHECK (s.flagged ());
}

TEST_CASE ("PR8006 G2: each unannotated histogram-less candidate costs one class-cache probe; the walk stops at "
	   "the first flagged one", "[contract][G2]")
{
  scene s (4);
  s.hist ("c");
  s.hist ("d");
  s.add (ex (PT_EQ, col ("a"), hv (0)));
  s.add (ex (PT_LIKE, col ("b"), hv (1)));
  s.add (ex (PT_GT, hv (2), col ("c")));
  s.add (ex (PT_EQ, col ("d"), hv (3)));

  CHECK (s.flagged ());
  CHECK (g_cache_calls == 3);	/* a, b, c -- not d */
}

TEST_CASE ("PR8006 G2: `s LIKE ?` / `s NOT LIKE ?` / `t.s LIKE ?` on a column with a histogram flag the plan; the "
	   "ESCAPE form, `? LIKE s` and an auto-parameterized pattern do not", "[contract][G2]")
{
  for (int k = 0; k < 3; k++)
    {
      scene s (1);
      s.hist ("s");
      s.add (ex (k == 1 ? PT_NOT_LIKE : PT_LIKE, k == 2 ? dot ("s") : col ("s"), hv (0)));
      INFO ("shape " << k);
      CHECK (s.flagged ());
    }
  {
    scene s (1, 1);
    s.hist ("s");
    s.add (like_escape (PT_LIKE, col ("s"), hv (0)));
    s.add (ex (PT_LIKE, hv (0), col ("s")));
    s.add (ex (PT_NOT_LIKE, col ("s"), hv (1)));
    CHECK_FALSE (s.flagged ());
  }
}

TEST_CASE ("PR8006 G2 vs term: `s LIKE ?` on a column with a histogram is flagged, and a NULL pattern still "
	   "yields a fingerprint (one ` NULL` slot of 1 row), so the first execution keeps watching",
	   "[contract][G2][null]")
{
  scene s (1);
  s.hist ("s");
  s.add (ex (PT_LIKE, col ("s"), hv (0)));
  s.bind_null (0);

  CHECK (s.flagged ());
  slots r = s.fp ();
  CHECK (r.ok);
  REQUIRE (r.n == 1);
  CHECK (r.rows[0] == 1.0);
}
