/*
 * btree_attrinfo_read_dbvalues (src/storage/btree.c, SERVER_MODE) -- the covered read of an index key into the scan's
 * attribute values: which key element each requested attribute id gets, and which requests are refused.
 *
 * What the inputs model (head 05b46aefe, PR#6466; base 4e389935d is develop):
 *   - Callers (btree_read_curr_key_values btree.c:20747, px_scan_index_leaf_slot_walker.cpp:425) pass
 *     btree_att_ids = the ids of OR_INDEX.atts and btree_num_att = OR_INDEX.n_atts (heap_get_indexinfo_of_btid,
 *     heap_file.c:15108 -> scan_manager.c:3020, :2142), func_index_col_id = func_index_info->col_id for a covered scan
 *     (scan_init_indx_coverage; -1 when not covered), attr_idx_ptr = the scan's position cache whose [0] is -1 until
 *     the first key fills it (btree.c:29784).
 *   - A function index's attribute list is its plain columns in order, then the function's argument columns that are
 *     not among them (semantic_check.c:17695-17700); attr_index_start = n_atts - appended arguments and col_id = the
 *     function's position (execute_schema.c:3328-3329).
 *   - Its key has attr_index_start + 1 elements: the first attr_index_start attributes with the function result put at
 *     col_id (heap_midxkey_key_generate, heap_file.c:14432); with attr_index_start == 0 the key is the function result
 *     alone, not a MIDXKEY (heap_file.c:14613). So (mod(a, b)): atts [a, b], n_atts 2, key = the value mod(a, b).
 *   - FUNC_INDEX_RESULT_ATTRID (-2, storage_common.h, new in PR#6466) is the attribute id a covered scan gives the
 *     function result (xasl_generation.c pt_to_regu_attr_descr).
 *
 * Fakes (object_primitive.c and error_manager.c are not linked; nothing else of btree.c runs):
 *   pr_midxkey_get_element_nocopy -- the key is a test array (DB_MIDXKEY.buf -> fake_key); returns element `index`
 *       and records (which key, index). index >= ncolumns -> -1, as pr_midxkey_get_element_internal (assert (false)
 *       under debug, then error -1).
 *   pr_clone_value -- setval with copy: for the INT values used here a plain copy that owns nothing; counted.
 *   pr_clear_value -- frees nothing for INT, then PRIM_SET_NULL and need_clear = false; counted.
 *   er_set / er_errid / er_clear -- the last error id set.
 *
 * Tags: [contract] holds on develop and on the PR; [defect] reproduces a review finding (PASS = the defect is there).
 * Cases on the function result attribute (PR#6466) are compiled only where FUNC_INDEX_RESULT_ATTRID exists; a
 * wrapper defining UNIT_FUNC_ATTR_LITERAL before including this file forces -2 to show a revision without it.
 */
#include "catch.hpp"
#include "config.h"
#include "btree.h"
#include "heap_attrinfo.h"
#include "object_primitive.h"
#include "error_manager.h"
#include "dbtype.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if !defined (FUNC_INDEX_RESULT_ATTRID) && defined (UNIT_FUNC_ATTR_LITERAL)
#define FUNC_INDEX_RESULT_ATTRID (-2)	/* storage_common.h @05b46aefe */
#endif

/* ---------------------------------------------------------------- fakes */

struct fake_key
{
  const char *tag;
  int n;
  DB_VALUE elems[8];
};

struct key_read
{
  std::string tag;
  int index;
};

static std::vector<key_read> g_reads;
static int g_clones;
static int g_clears;
static int g_er_id = NO_ERROR;

int
pr_midxkey_get_element_nocopy (const DB_MIDXKEY * midxkey, int index, DB_VALUE * value, int *prev_indexp,
			       char **prev_ptrp)
{
  (void) prev_indexp;
  (void) prev_ptrp;
  const fake_key *k = (const fake_key *) midxkey->buf;
  g_reads.push_back ({k->tag, index});
  if (index < 0 || index >= midxkey->ncolumns)
    {
      return -1;
    }
  *value = k->elems[index];
  value->need_clear = false;	/* nocopy: points into the key */
  return NO_ERROR;
}

int
pr_clone_value (const DB_VALUE * src, DB_VALUE * dest)
{
  g_clones++;
  if (dest == NULL)
    {
      return NO_ERROR;
    }
  if (src == NULL)
    {
      db_make_null (dest);
      return NO_ERROR;
    }
  *dest = *src;
  dest->need_clear = false;
  return NO_ERROR;
}

int
pr_clear_value (DB_VALUE * value)
{
  g_clears++;
  if (value == NULL)
    {
      return NO_ERROR;
    }
  value->domain.general_info.is_null = 1;
  value->need_clear = false;
  return NO_ERROR;
}

void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  g_er_id = err_id;
}

int
er_errid (void)
{
  return g_er_id;
}

void
er_clear (void)
{
  g_er_id = NO_ERROR;
}

/* ---------------------------------------------------------------- helpers */

static const int A = 11, B = 12, C = 13;	/* attribute ids of columns a, b, c */

static void
reset_fakes ()
{
  g_reads.clear ();
  g_clones = 0;
  g_clears = 0;
  g_er_id = NO_ERROR;
}

/* a key whose elements are INT values (INT_MIN = a NULL element) */
static fake_key *
mk_key (const char *tag, std::vector<int> elems)
{
  fake_key *k = new fake_key ();
  k->tag = tag;
  k->n = (int) elems.size ();
  for (int i = 0; i < k->n; i++)
    {
      if (elems[i] == INT_MIN)
	{
	  db_make_null (&k->elems[i]);
	}
      else
	{
	  db_make_int (&k->elems[i], elems[i]);
	}
    }
  return k;
}

static void
midxkey_value (DB_VALUE * v, fake_key * k)
{
  DB_MIDXKEY mk;
  memset (&mk, 0, sizeof (mk));
  mk.size = (int) sizeof (*k);
  mk.ncolumns = k->n;
  mk.domain = NULL;		/* the function under test only hands the MIDXKEY to the element reader */
  mk.buf = (char *) k;
  mk.min_max_val.position = -1;
  mk.min_max_val.type = MIN_COLUMN;
  db_make_midxkey (v, &mk);
}

/* the scan's attribute cache: the requested ids, in the order the scan asked for them */
struct attrs
{
  HEAP_CACHE_ATTRINFO *info;

  explicit attrs (std::vector<int> ids)
  {
    info = (HEAP_CACHE_ATTRINFO *) calloc (1, sizeof (HEAP_CACHE_ATTRINFO));
    info->num_values = (int) ids.size ();
    info->values = (HEAP_ATTRVALUE *) calloc (ids.size (), sizeof (HEAP_ATTRVALUE));
    for (size_t i = 0; i < ids.size (); i++)
      {
	info->values[i].attrid = ids[i];
	info->values[i].state = HEAP_UNINIT_ATTRVALUE;
	db_make_null (&info->values[i].dbvalue);
      }
  }
  ~attrs ()
  {
    free (info->values);
    free (info);
  }
  int val (int i) const
  {
    return db_get_int (&info->values[i].dbvalue);
  }
  bool is_null (int i) const
  {
    return DB_IS_NULL (&info->values[i].dbvalue);
  }
  HEAP_ATTRVALUE_STATE state (int i) const
  {
    return info->values[i].state;
  }
};

static int
read_key (DB_VALUE * key, BTREE_SCAN * bts, std::vector<int> att_ids, int num_att, attrs & a, int func_col,
	  int *idx)
{
  return btree_attrinfo_read_dbvalues (NULL, key, bts, att_ids.data (), num_att, a.info, func_col, idx);
}

static BTREE_SCAN *
mk_compressed_bts (fake_key * prefix, int prefix_size)
{
  BTREE_SCAN *bts = (BTREE_SCAN *) calloc (1, sizeof (BTREE_SCAN));
  bts->is_cur_key_compressed = true;
  bts->common_prefix_size = prefix_size;
  midxkey_value (&bts->common_prefix_key, prefix);
  return bts;
}

static std::string
reads_str ()
{
  std::string s;
  for (const key_read & r:g_reads)
    {
      s += r.tag + "[" + std::to_string (r.index) + "] ";
    }
  return s;
}

/* ---------------------------------------------------------------- [contract] develop and PR alike */

TEST_CASE ("K1 plain index (a, b, c): each requested attr gets the key element at its position, attr_idx_ptr records "
	   "the positions", "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  fake_key *k = mk_key ("cur", {100, 200, 300});
  midxkey_value (&key, k);
  attrs a ({C, A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, NULL, {A, B, C}, 3, a, -1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 300);
  CHECK (a.val (1) == 100);
  CHECK (a.state (0) == HEAP_WRITTEN_ATTRVALUE);
  CHECK (a.state (1) == HEAP_WRITTEN_ATTRVALUE);
  CHECK (idx[0] == 2);
  CHECK (idx[1] == 0);
  CHECK (g_clones == 0);	/* MIDXKEY elements are read without a copy */
  delete k;
}

TEST_CASE ("K2 position cache (attr_idx_ptr[0] >= 0): the next key is read at the recorded positions", "[contract]")
{
  reset_fakes ();
  DB_VALUE key1, key2;
  fake_key *k1 = mk_key ("cur", {100, 200, 300});
  fake_key *k2 = mk_key ("cur", {101, 201, 301});
  midxkey_value (&key1, k1);
  midxkey_value (&key2, k2);
  attrs a ({C, A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key1, NULL, {A, B, C}, 3, a, -1, idx) == NO_ERROR);
  reset_fakes ();
  REQUIRE (read_key (&key2, NULL, {A, B, C}, 3, a, -1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 301);
  CHECK (a.val (1) == 101);
  REQUIRE (g_reads.size () == 2);
  CHECK (g_reads[0].index == 2);
  CHECK (g_reads[1].index == 0);
  delete k1;
  delete k2;
}

TEST_CASE ("K3 plain index: an attr that is not in the key is refused (ER_FAILED)", "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  fake_key *k = mk_key ("cur", {100, 200, 300});
  midxkey_value (&key, k);
  attrs a ({A, 99});
  int idx[4] = { -1, -1, -1, -1 };

  CHECK (read_key (&key, NULL, {A, B, C}, 3, a, -1, idx) != NO_ERROR);
  delete k;
}

TEST_CASE ("K4 one-column plain index (key is not a MIDXKEY): its column's attr gets a clone of the key value",
	   "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  db_make_int (&key, 42);
  attrs a ({A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, NULL, {A}, 1, a, -1, idx) == NO_ERROR);
  CHECK (a.val (0) == 42);
  CHECK (a.state (0) == HEAP_WRITTEN_ATTRVALUE);
  CHECK (g_clones == 1);
}

TEST_CASE ("K5 one-value key refuses: two requested values, another column's attr, a plain column attr with "
	   "btree_num_att != 1", "[contract]")
{
  DB_VALUE key;
  db_make_int (&key, 42);
  {
    reset_fakes ();
    attrs a ({A, B});
    CHECK (read_key (&key, NULL, {A}, 1, a, -1, NULL) != NO_ERROR);
  }
  {
    reset_fakes ();
    attrs a ({B});
    CHECK (read_key (&key, NULL, {A}, 1, a, -1, NULL) != NO_ERROR);
  }
  {
    reset_fakes ();
    attrs a ({A});
    CHECK (read_key (&key, NULL, {A, B}, 2, a, -1, NULL) != NO_ERROR);
    CHECK (g_clones == 0);
  }
}

TEST_CASE ("K6 function index (a, f(b), c), plain columns: a -> element 0, c -> element 2 (the function result slot 1 "
	   "is stepped over)", "[contract]")
{
  /* atts [a, c, b] (b appended as the argument), n_atts 3, attr_index_start 2, col_id 1; key [a, f(b), c] */
  reset_fakes ();
  DB_VALUE key;
  fake_key *k = mk_key ("cur", {100, 777, 300});
  midxkey_value (&key, k);
  attrs a ({C, A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, NULL, {A, C, B}, 3, a, 1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 300);
  CHECK (a.val (1) == 100);
  CHECK (idx[0] == 2);
  CHECK (idx[1] == 0);
  delete k;
}

TEST_CASE ("K7 argument that is a key column, (f(a), a): a -> element 1, and the cached position "
	   "j == btree_num_att is accepted", "[contract]")
{
  /* atts [a] (a already a column, nothing appended), n_atts 1, attr_index_start 1, col_id 0; key [f(a), a] */
  reset_fakes ();
  DB_VALUE key1, key2;
  fake_key *k1 = mk_key ("cur", {777, 100});
  fake_key *k2 = mk_key ("cur", {778, 101});
  midxkey_value (&key1, k1);
  midxkey_value (&key2, k2);
  attrs a ({A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key1, NULL, {A}, 1, a, 0, idx) == NO_ERROR);
  CHECK (a.val (0) == 100);
  CHECK (idx[0] == 1);
  REQUIRE (read_key (&key2, NULL, {A}, 1, a, 0, idx) == NO_ERROR);
  CHECK (a.val (0) == 101);
  delete k1;
  delete k2;
}

TEST_CASE ("K8 compressed key (common prefix 2), plain index (a, b, c): positions inside the prefix are read from the "
	   "prefix key, the rest from the current key", "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  fake_key *prefix = mk_key ("prefix", {1100, 1200, INT_MIN});
  fake_key *cur = mk_key ("cur", {INT_MIN, INT_MIN, 300});
  midxkey_value (&key, cur);
  BTREE_SCAN *bts = mk_compressed_bts (prefix, 2);
  attrs a ({C, A, B});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, bts, {A, B, C}, 3, a, -1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 300);
  CHECK (a.val (1) == 1100);
  CHECK (a.val (2) == 1200);
  REQUIRE (g_reads.size () == 3);
  CHECK (g_reads[0].tag == "cur");
  CHECK (g_reads[1].tag == "prefix");
  CHECK (g_reads[2].tag == "prefix");
  free (bts);
  delete prefix;
  delete cur;
}

#if defined (FUNC_INDEX_RESULT_ATTRID)
/* ---------------------------------------------------------------- [contract] the function result attribute (PR#6466) */

TEST_CASE ("K9 (PR6466) function index (a, f(b), c): the function result attr -> element 1 (col_id); mixed request "
	   "[c, f, a] and the cached positions", "[contract]")
{
  reset_fakes ();
  DB_VALUE key1, key2;
  fake_key *k1 = mk_key ("cur", {100, 777, 300});
  fake_key *k2 = mk_key ("cur", {101, 778, 301});
  midxkey_value (&key1, k1);
  midxkey_value (&key2, k2);
  attrs a ({C, FUNC_INDEX_RESULT_ATTRID, A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key1, NULL, {A, C, B}, 3, a, 1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 300);
  CHECK (a.val (1) == 777);
  CHECK (a.val (2) == 100);
  CHECK (idx[0] == 2);
  CHECK (idx[1] == 1);
  CHECK (idx[2] == 0);
  REQUIRE (read_key (&key2, NULL, {A, C, B}, 3, a, 1, idx) == NO_ERROR);
  CHECK (a.val (0) == 301);
  CHECK (a.val (1) == 778);
  CHECK (a.val (2) == 101);
  delete k1;
  delete k2;
}

TEST_CASE ("K10 (PR6466) function index (f(a), b): the function result -> element 0, b -> element 1", "[contract]")
{
  /* atts [b, a] (a appended), n_atts 2, attr_index_start 1, col_id 0; key [f(a), b] */
  reset_fakes ();
  DB_VALUE key;
  fake_key *k = mk_key ("cur", {49, 200});
  midxkey_value (&key, k);
  attrs a ({FUNC_INDEX_RESULT_ATTRID, B});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, NULL, {B, A}, 2, a, 0, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 49);
  CHECK (a.val (1) == 200);
  CHECK (idx[0] == 0);
  CHECK (idx[1] == 1);
  delete k;
}

TEST_CASE ("K11 (PR6466) (a, f(a)): the function result -> element 1 == btree_num_att, first key and cached",
	   "[contract]")
{
  /* atts [a], n_atts 1, attr_index_start 1, col_id 1; key [a, f(a)] */
  reset_fakes ();
  DB_VALUE key1, key2;
  fake_key *k1 = mk_key ("cur", {100, 777});
  fake_key *k2 = mk_key ("cur", {101, 778});
  midxkey_value (&key1, k1);
  midxkey_value (&key2, k2);
  attrs a ({FUNC_INDEX_RESULT_ATTRID, A});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key1, NULL, {A}, 1, a, 1, idx) == NO_ERROR);
  CHECK (a.val (0) == 777);
  CHECK (a.val (1) == 100);
  CHECK (idx[0] == 1);
  CHECK (idx[1] == 0);
  REQUIRE (read_key (&key2, NULL, {A}, 1, a, 1, idx) == NO_ERROR);
  CHECK (a.val (0) == 778);
  CHECK (a.val (1) == 101);
  delete k1;
  delete k2;
}

TEST_CASE ("K12 (PR6466) one-column function key with one argument, (f(b)): the function result attr gets a clone of "
	   "the whole key value", "[contract]")
{
  /* atts [b], n_atts 1, attr_index_start 0, col_id 0; key = f(b), not a MIDXKEY */
  reset_fakes ();
  DB_VALUE key;
  db_make_int (&key, 49);
  attrs a ({FUNC_INDEX_RESULT_ATTRID});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, NULL, {B}, 1, a, 0, idx) == NO_ERROR);
  CHECK (a.val (0) == 49);
  CHECK (a.state (0) == HEAP_WRITTEN_ATTRVALUE);
  CHECK (g_clones == 1);
}

TEST_CASE ("K13 (PR6466) compressed key (common prefix 2), (a, f(b), c): the function result slot 1 is read from the "
	   "prefix key, c from the current key", "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  fake_key *prefix = mk_key ("prefix", {1100, 1777, INT_MIN});
  fake_key *cur = mk_key ("cur", {INT_MIN, INT_MIN, 300});
  midxkey_value (&key, cur);
  BTREE_SCAN *bts = mk_compressed_bts (prefix, 2);
  attrs a ({FUNC_INDEX_RESULT_ATTRID, C});
  int idx[4] = { -1, -1, -1, -1 };

  REQUIRE (read_key (&key, bts, {A, C, B}, 3, a, 1, idx) == NO_ERROR);
  CAPTURE (reads_str ());
  CHECK (a.val (0) == 1777);
  CHECK (a.val (1) == 300);
  REQUIRE (g_reads.size () == 2);
  CHECK (g_reads[0].tag == "prefix");
  CHECK (g_reads[1].tag == "cur");
  free (bts);
  delete prefix;
  delete cur;
}

TEST_CASE ("K14 (PR6466) one-column function key with two arguments, (mod(a, b)): a read that succeeds gives the whole "
	   "key value (whether it succeeds is the G4-01 [defect] case)", "[contract]")
{
  reset_fakes ();
  DB_VALUE key;
  db_make_int (&key, 1);	/* mod(7, 3) */
  attrs a ({FUNC_INDEX_RESULT_ATTRID});
  int idx[4] = { -1, -1, -1, -1 };

  int rc = read_key (&key, NULL, {A, B}, 2, a, 0, idx);
  CAPTURE (rc);
  if (rc == NO_ERROR)
    {
      CHECK (a.val (0) == 1);
      CHECK (a.state (0) == HEAP_WRITTEN_ATTRVALUE);
      CHECK (g_clones == 1);
    }
  else
    {
      SUCCEED ("refused -- see PR6466 G4-01");
    }
}

/* ---------------------------------------------------------------- [defect] review findings (PASS = defect present) */

TEST_CASE ("PR6466 G4-01: one-column function key with two arguments (mod(a, b)) -- key not a MIDXKEY, "
	   "btree_num_att = 2, one function result attr -- is refused with ER_FAILED", "[defect]")
{
  /* atts [a, b] (both appended as arguments), n_atts 2, attr_index_start 0, col_id 0; key = mod(a, b) */
  reset_fakes ();
  DB_VALUE key;
  db_make_int (&key, 1);	/* mod(7, 3) */
  attrs a ({FUNC_INDEX_RESULT_ATTRID});
  int idx[4] = { -1, -1, -1, -1 };

  int rc = read_key (&key, NULL, {A, B}, 2, a, 0, idx);
  CAPTURE (rc);
  CHECK (rc != NO_ERROR);
  CHECK (a.state (0) != HEAP_WRITTEN_ATTRVALUE);
  CHECK (g_clones == 0);
}

TEST_CASE ("PR6466 G4-01b: the one-value key refusal returns ER_FAILED without er_set -- the caller's ASSERT_ERROR "
	   "(btree.c:30630) aborts a debug server", "[defect]")
{
  /* a refusal the proposed fix keeps: a plain column attr that is not the key's column */
  reset_fakes ();
  DB_VALUE key;
  db_make_int (&key, 42);
  attrs a ({B});

  int rc = read_key (&key, NULL, {A}, 1, a, -1, NULL);
  CAPTURE (rc);
  CAPTURE (g_er_id);
  CHECK (rc != NO_ERROR);
  CHECK (g_er_id == NO_ERROR);
}
#endif /* FUNC_INDEX_RESULT_ATTRID */
