/*
 * CBRD-27659 (prototype) -- a Bloom filter of the first-materialized hash join input's keys, checked before the second
 * input writes a row to its list (src/query/query_hash_join.c hjoin_bloom_*, server mode). New code: the base has no
 * hjoin_bloom_*, so the suite runs on head only (requires: hjoin_bloom_attach). Written for v1 5a8c0b7d8 (now branch
 * CBRD-27659-v1), moved to the v2 layout (e55956083 + fdc3fcd32, now branch CBRD-27659, first named CBRD-27659-v2):
 * the trace counters live in HASHJOIN_PROC_NODE.bloom_stats (HASHJOIN_BLOOM_STATS used / keys / tested / skipped), not
 * in the stats_group. v3 (1a7bccdcc) adds B6-B8 for the parallel paths; they are registered only where that API links
 * (weak references), so one suite runs on fdc3fcd32 (B1-B5, F1-F4) and on 1a7bccdcc and later (all).
 *
 *   B1  no false negative: 100,000 random INT first-input keys; each of them as a second-input row is kept
 *       (+ a two-column key INT x VARCHAR)
 *   B2  coercion pairs hjoin_init_domain_info resolves to a common domain - INT x BIGINT, NUMERIC(10,2) x NUMERIC(20,4),
 *       CHAR x VARCHAR (trailing blanks), utf8_en_ci ('A' vs 'a'), each way round, + NUMERIC(38,0) x NUMERIC(38,10)
 *       (the default-precision path) and INT x DOUBLE: a second row the join matches is kept. "The join matches" is
 *       the join's own test, taken from the TU: the in-memory table is keyed by qdata_hash_scan_key, the probe then
 *       compares with hjoin_fetch_key (compare_key); qdata_hscan_key_eq is reported next to it.
 *   B3  a NULL-key second row is skipped; a first-input NULL key adds nothing; keys absent from the first input are
 *       mostly skipped (false-positive rate near (1 - e^(-kn/m))^k, wide bounds); the bit array size
 *   B4  where it must not attach or act: parameter off, a preserved second side (RIGHT: inner, LEFT: outer, FULL), first
 *       status other than XASL_SUCCESS, first 0 rows / more than 10,000,000, a second input with GROUP BY, analytic,
 *       hash GROUP BY, LIMIT, OFFSET, DISTINCT, CONNECT BY, ORDERBY_NUM () (ordbynum_pred / ordbynum_val), a top-N sort
 *       limit (orderby_limit), click counter, the SELECT of an UPDATE / DELETE (upd_del_class_cnt), a subquery result
 *       cache (sub_xasl_id), top-most, not a BUILDLIST, not an input of this join; a thread other than the owner and a
 *       cloned node let every row through; release on a clone frees nothing; a failed build lets every row through
 *   B5  detach moves the counters to the hash join's bloom_stats and frees the filter; attach clears bloom_stats at
 *       each execution, whether or not it attaches (the aptr loop calls it for the first input too)
 *   F2  (a contract since v2) the BLOOM counters detach wrote survive qexec_hash_join's hjoin_init_manager under trace:
 *       bloom_stats is outside the stats_group it clears. On 5a8c0b7d8 they were in the stats_group, so the trace line
 *       never printed - the layout change closed it, so this is no longer a [defect] case.
 *   F1  [defect] hjoin_bloom_skip_descr / hjoin_bloom_skip_tuple hand hjoin_fetch_key a QFILE_TUPLE_RECORD whose layout
 *       binding (type_list) and position cache are never set: what the stack held decides - a crash, or a matching row
 *       read through another list's layout and dropped
 *   F3  [defect] (latent) the build takes "no error id" as "the first input was read to the end": a scan that stops on
 *       S_ERROR without an error id leaves a partial filter on, and the rows past the stop are dropped
 *   F4  [defect] (quality) false-positive rate about 2.3x the theory: the second probe stride ((h >> 16) | (h << 16)) | 1
 *       shares hash bits with the first position h & mask
 *   F1, F3, F4 are fixed in fdc3fcd32 (suite.json defects_fixed_in): they reproduce on 5a8c0b7d8 and e55956083 and FAIL
 *   on a fixed head; a PASS there is a recurrence.
 *   B6  (1a7bccdcc) hjoin_bloom_wants_serial: false with the parameter off; on the input the aptr list runs second it
 *       decides as attach does (preserved side, second's shape) and does not read the first input; the aptr order
 *       picks the second (reversing it changes the answer, other aptr nodes are passed over); an input missing from
 *       the aptr list or XASL_LINK_TO_REGU_VARIABLE, not a hash join, NULL -> false
 *   B7  (1a7bccdcc) hjoin_bloom_prepare: the owner thread builds once and gets the filter (the owner's later rows do not
 *       build again - the first input is scanned once); another thread, a clone, no filter, NULL, a failed build -> NULL
 *   B8  (1a7bccdcc) hjoin_bloom_worker_*: two workers share the filter's bits with a key and a row buffer each; a
 *       worker decides as the owner does (INT + NULL rows, both entry paths, B2's coercion pairs with the second
 *       coerced); worker_end adds the counts (owner's included) and frees the worker's own; a fetch failure (or a row
 *       buffer that cannot grow) turns off that worker alone; a begin without memory returns NULL and leaks nothing;
 *       NULL bloom / worker / tuple; two workers on two std::threads read the bits and end at the same time - the
 *       counts are exact sums (200 rounds)
 *
 * Real code at the revision under test, linked as --extra TUs: query_hash_scan.c (keys, hash, key_eq), memory_hash.c
 * (mht_get_hash_number, the collations' hash), qfile_tuple_layout.c (layout, slot walker), and the type system
 * (system_parameter.c, language_support.c, intl_support.c, chartype.c, mprec.c, dtoa.c, object_primitive.c,
 * object_domain.c, object_domain_convert.cpp, numeric_opfunc.c, db_date.c, db_macro.c, string_opfunc.c). Tuples are
 * written by the real assembler (qfile_tuple_size_from_values / qfile_tuple_fill_from_values, the T_NORMAL path of
 * qfile_generate_tuple_into_list). Faked: the list file page iteration (qfile_open_list_scan / qfile_scan_list_next /
 * qfile_close_scan - list_file.c reads pages through the query manager's temp files), the error manager (per thread),
 * the private allocator (locked; a fault knob fails the n-th allocation as the real malloc branch does - B8) and the
 * area allocator, boot-time globals.
 *
 * The suite names these TU-internal symbols (hooks.cpp): struct hjoin_bloom fields built, disabled, first_is_outer,
 * hj_xasl, owner_xasl, owner_thread, bits, row_buf, key, mask, keys, tested, skipped; static hjoin_bloom_build,
 * hjoin_bloom_ready, hjoin_bloom_skip_record, hjoin_bloom_may_contain, hjoin_init_domain_info, hjoin_init_manager;
 * (1a7bccdcc) struct hjoin_bloom_worker fields bloom, key, row_buf, row_buf_size, disabled, tested, skipped.
 * A rewrite that renames them needs only hooks.cpp adjusted; test.cpp uses the public hjoin_bloom_* API.
 */
#include "catch.hpp"
#include "query_hash_join.h"
#include "query_hash_scan.h"
#include "xasl.h"
#include "query_list.h"
#include "qfile_tuple_layout.h"
#include "list_file.h"
#include "object_domain.h"
#include "object_primitive.h"
#include "dbtype.h"
#include "language_support.h"
#include "system_parameter.h"
#include "boot_sr.h"
#include "thread_entry.hpp"
#include "error_manager.h"
#include "memory_alloc.h"
#include "area_alloc.h"
#include "environment_variable.h"
#include "string_regex.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* ---------------------------------------------------------------- hooks.cpp (appended to query_hash_join.c) */

struct unitseam_bloom_view
{
  bool present;
  bool built;
  bool disabled;
  bool first_is_outer;
  const void *hj_xasl;
  const void *owner_xasl;
  const void *owner_thread;
  const void *bits;
  const void *row_buf;
  const void *key;
  unsigned int mask;
  long long keys;
  long long tested;
  long long skipped;
};

int unitseam_hj_init_domain_info (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl);
void unitseam_bloom_view_of (XASL_NODE * xasl, unitseam_bloom_view * v);
void unitseam_bloom_build (THREAD_ENTRY * thread_p, XASL_NODE * xasl);
int unitseam_bloom_skip_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record);
bool unitseam_bloom_may_contain (XASL_NODE * xasl, unsigned int hash);
int unitseam_hj_join_pair (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, bool first_is_outer,
			   QFILE_TUPLE_RECORD * first_rec, QFILE_TUPLE_RECORD * second_rec, bool *first_null,
			   bool *second_null, unsigned int *first_hash, unsigned int *second_hash, bool *key_eq,
			   bool *compare_eq);
int unitseam_hj_init_manager (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl);

/* v3 (1a7bccdcc): struct hjoin_bloom_worker read through hooks.cpp; present = false on a revision without it */
struct unitseam_worker_view
{
  bool present;
  const void *bloom;
  const void *key;
  const void *row_buf;
  int row_buf_size;
  bool disabled;
  long long tested;
  long long skipped;
};
void unitseam_worker_view_of (const void *worker, unitseam_worker_view * v);

/* ---------------------------------------------------------------- fakes: what a booted server process has */

/* system_parameter.c's static initializers (prm_Def's parallelism defaults, the regex engine keyword table) */
namespace cubthread
{
  std::size_t system_core_count (void)
  {
    return 8;
  }
}
namespace cubregex
{
  const char *get_engine_name (const engine_type & type)
  {
    return type == engine_type::LIB_RE2 ? "re2" : "cppstd";
  }
}

/* boot_sr.h BO_IS_SERVER_RESTARTED () reads this; DOWN keeps prm_get_value on the global values */
BOOT_SERVER_STATUS boot_Server_status = BOOT_SERVER_DOWN;

/* environment_variable.c: no CUBRID_* variable is set (lang_init_builtin reads CUBRID_MSG_LANG) */
const char *
envvar_get (const char *)
{
  return NULL;
}

/* environment_variable.c envvar_trim_char body */
void
envvar_trim_char (char *env_val, const int c)
{
  if (env_val == NULL)
    {
      return;
    }
  size_t size = strlen (env_val);
  if (*env_val == c && size > 2)
    {
      memmove (env_val, env_val + 1, size - 2);
      env_val[size - 2] = '\0';
    }
}

/* error_manager.c: the last error id er_set raised, as er_errid () reads it; er_clear () resets it. Per thread, as the
 * real error context is (B8 runs parallel scan workers on threads of their own). */
static thread_local int fake_er_id = NO_ERROR;
static thread_local int fake_er_sets = 0;
void
er_set (int, const char *, const int, int err_id, int, ...)
{
  fake_er_id = err_id;
  fake_er_sets++;
}

int
er_errid (void)
{
  return fake_er_id;
}

void
er_clear (void)
{
  fake_er_id = NO_ERROR;
}

/*
 * memory_alloc.c: a thread's private heap is malloc here; live blocks are tracked to see what a release frees (under a
 * lock: B8 frees from two worker threads at once). Fault knob (B8): fail_nth_alloc (n) makes the n-th allocation or
 * reallocation from now fail as the real malloc branch of db_private_alloc_release does - NULL and
 * ER_OUT_OF_VIRTUAL_MEMORY; a failed realloc leaves the old block as it was.
 */
static std::set<void *> *g_live = nullptr;
static std::mutex g_live_mutex;
static std::atomic<int> g_alloc_countdown (0);
static std::set<void *> &
live ()
{
  if (g_live == nullptr)
    {
      g_live = new std::set<void *> ();
    }
  return *g_live;
}

static size_t
live_count ()
{
  std::lock_guard<std::mutex> g (g_live_mutex);
  return live ().size ();
}

static void
fail_nth_alloc (int n)
{
  g_alloc_countdown = n;
}

static bool
alloc_fails_now (size_t size)
{
  int c = g_alloc_countdown.load ();
  while (c > 0 && !g_alloc_countdown.compare_exchange_weak (c, c - 1))
    {
    }
  if (c == 1)
    {
      er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
      return true;
    }
  return false;
}

void *
db_private_alloc_release (THREAD_ENTRY *, size_t size, bool)
{
  if (alloc_fails_now (size))
    {
      return NULL;
    }
  void *p = malloc (size);
  if (p != NULL)
    {
      std::lock_guard<std::mutex> g (g_live_mutex);
      live ().insert (p);
    }
  return p;
}

void
db_private_free_release (THREAD_ENTRY *, void *ptr, bool)
{
  if (ptr != NULL)
    {
      std::lock_guard<std::mutex> g (g_live_mutex);
      live ().erase (ptr);
    }
  free (ptr);
}

void *
db_private_realloc_release (THREAD_ENTRY *, void *ptr, size_t size, bool)
{
  if (alloc_fails_now (size))
    {
      return NULL;
    }
  std::lock_guard<std::mutex> g (g_live_mutex);
  if (ptr != NULL)
    {
      live ().erase (ptr);
    }
  void *p = realloc (ptr, size);
  if (p != NULL)
    {
      live ().insert (p);
    }
  else if (ptr != NULL)
    {
      live ().insert (ptr);
    }
  return p;
}

/* area_alloc.c: tp_init's domain area hands out zeroed element-sized blocks */
struct fake_area
{
  size_t element_size;
};
AREA *
area_create (const char *, size_t element_size, size_t)
{
  return (AREA *) new fake_area { element_size };
}

void *
area_alloc (AREA * area)
{
  return calloc (1, ((fake_area *) area)->element_size);
}

int
area_free (AREA *, void *ptr)
{
  free (ptr);
  return NO_ERROR;
}

/*
 * list_file.c page iteration. A fake list is identified by list_id->first_vpid.pageid (a copied list id keeps it).
 * Same contract as the real functions (list_file.c qfile_open_list_scan / qfile_scan_list_next / qfile_retrieve_tuple /
 * qfile_close_scan): open copies the list id and binds scan_id->tplrec to the copy's type list; next with PEEK points
 * the caller's record at the stored tuple and binds it to &scan_id->list_id.type_list, size 0 (not owned); S_END after
 * the last tuple; close marks the scan closed. Knobs for B4/F3: fail_open (open returns ER_FAILED with an error id),
 * stop_at (S_ERROR at that tuple, with or without an error id).
 */
struct fake_list
{
  QFILE_LIST_ID *list_id;
  std::vector<char *> tuples;
  bool fail_open = false;
  int stop_at = -1;
  bool stop_sets_error = true;
  int opens = 0;
};
static std::vector<fake_list *> g_lists;

static fake_list *
fake_list_of (const QFILE_LIST_ID * list_id)
{
  int h = list_id->first_vpid.pageid;
  if (h <= 0 || h > (int) g_lists.size ())
    {
      return nullptr;
    }
  return g_lists[h - 1];
}

int
qfile_open_list_scan (QFILE_LIST_ID * list_id_p, QFILE_LIST_SCAN_ID * scan_id_p)
{
  fake_list *fl = fake_list_of (list_id_p);
  if (fl == nullptr || fl->fail_open)
    {
      er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_FAILED, 0);
      return ER_FAILED;
    }
  fl->opens++;
  memset (scan_id_p, 0, sizeof (*scan_id_p));
  scan_id_p->status = S_OPENED;
  scan_id_p->position = S_BEFORE;
  scan_id_p->curr_tplno = -1;
  scan_id_p->list_id = *list_id_p;
  scan_id_p->tplrec = QFILE_TUPLE_RECORD_INITIALIZER;
  qfile_slot_set_layout (&scan_id_p->tplrec, &scan_id_p->list_id.type_list);
  return NO_ERROR;
}

SCAN_CODE
qfile_scan_list_next (THREAD_ENTRY *, QFILE_LIST_SCAN_ID * scan_id_p, QFILE_TUPLE_RECORD * tuple_record_p, int peek)
{
  fake_list *fl = fake_list_of (&scan_id_p->list_id);
  if (fl == nullptr)
    {
      er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_FAILED, 0);
      return S_ERROR;
    }
  int next = scan_id_p->curr_tplno + 1;
  if (fl->stop_at >= 0 && next == fl->stop_at)
    {
      if (fl->stop_sets_error)
	{
	  er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_FAILED, 0);
	}
      return S_ERROR;
    }
  if (next >= (int) fl->tuples.size ())
    {
      scan_id_p->position = S_AFTER;
      return S_END;
    }
  scan_id_p->curr_tplno = next;
  scan_id_p->position = S_ON;
  char *t = fl->tuples[next];
  if (peek)
    {
      qfile_slot_set_tuple_ptr_and_layout (tuple_record_p, t, 0, &scan_id_p->list_id.type_list);
    }
  else
    {
      int len = QFILE_GET_TUPLE_LENGTH (t);
      if (tuple_record_p->size < len)
	{
	  tuple_record_p->tpl = (char *) realloc (tuple_record_p->size ? tuple_record_p->tpl : NULL, len);
	  tuple_record_p->size = len;
	}
      memcpy (tuple_record_p->tpl, t, len);
      qfile_slot_set_tuple_ptr_and_layout (tuple_record_p, tuple_record_p->tpl, tuple_record_p->size,
					   &scan_id_p->list_id.type_list);
    }
  return S_SUCCESS;
}

void
qfile_close_scan (THREAD_ENTRY *, QFILE_LIST_SCAN_ID * scan_id_p)
{
  scan_id_p->status = S_CLOSED;
}

/* The boot order of a server (boot_restart_server): built-in collations, the type module, then the database's
 * charset and language - en_US.utf8 here - which also gives the string domains their codeset (tp_apply_sys_charset) */
static void
boot_types ()
{
  static bool done = false;
  if (!done)
    {
      done = true;
      lang_init_builtin ();
      REQUIRE (tp_init () == NO_ERROR);
      /* lang_init () would also load user locales; lang_set_charset_lang () only asserts lang_Initialized (static) */
      REQUIRE (lang_set_charset_lang ("en_US.utf8") == NO_ERROR);
    }
  er_clear ();
}

/* a THREAD_ENTRY is only compared by address and read for on_trace by the code under test */
static THREAD_ENTRY *
new_thread ()
{
  size_t sz = (sizeof (cubthread::entry) + 63) / 64 * 64;
  void *p = aligned_alloc (64, sz);
  memset (p, 0, sz);
  return (THREAD_ENTRY *) p;
}

static THREAD_ENTRY *
owner_thread ()
{
  static THREAD_ENTRY *t = new_thread ();
  return t;
}

static THREAD_ENTRY *
other_thread ()
{
  static THREAD_ENTRY *t = new_thread ();
  return t;
}

static void
set_bloom_param (bool on)
{
  prm_set_bool_value (PRM_ID_HASH_JOIN_BLOOM_FILTER, on);
  REQUIRE (prm_get_bool_value (PRM_ID_HASH_JOIN_BLOOM_FILTER) == on);
}

/* ---------------------------------------------------------------- domains and values */

static TP_DOMAIN *
dom_int ()
{
  return tp_domain_resolve_default (DB_TYPE_INTEGER);
}

static TP_DOMAIN *
dom_bigint ()
{
  return tp_domain_resolve_default (DB_TYPE_BIGINT);
}

static TP_DOMAIN *
dom_double ()
{
  return tp_domain_resolve_default (DB_TYPE_DOUBLE);
}

static TP_DOMAIN *
dom_numeric (int p, int s)
{
  return tp_domain_resolve (DB_TYPE_NUMERIC, NULL, p, s, NULL, 0);
}

static TP_DOMAIN *
dom_string (DB_TYPE t, int p, int coll)
{
  TP_DOMAIN *d = tp_domain_resolve (t, NULL, p, 0, NULL, coll);
  REQUIRE (d != NULL);
  REQUIRE (TP_DOMAIN_COLLATION (d) == coll);
  return d;
}

/* a value of exactly the column's domain, as a scan of that column produces it (CHAR(n) blank-padded to n) */
static DB_VALUE
cast_text (const char *text, TP_DOMAIN * d)
{
  DB_VALUE src, v;
  db_make_varchar (&src, DB_MAX_VARCHAR_PRECISION, text, (int) strlen (text), INTL_CODESET_UTF8,
		   TP_DOMAIN_TYPE (d) == DB_TYPE_CHAR || TP_DOMAIN_TYPE (d) == DB_TYPE_VARCHAR
		   ? TP_DOMAIN_COLLATION (d) : LANG_COLL_UTF8_BINARY);
  db_make_null (&v);
  TP_DOMAIN_STATUS st = tp_value_cast (&src, &v, d, false);
  INFO ("cast '" << text << "' to " << pr_type_name (TP_DOMAIN_TYPE (d)));
  REQUIRE (st == DOMAIN_COMPATIBLE);
  return v;
}

/* ---------------------------------------------------------------- lists and tuples */

static QFILE_LIST_ID *
new_list (const std::vector<TP_DOMAIN *> &doms, int hdr = QFILE_TUPLE_HDR_SIZE_FORWARD)
{
  QFILE_LIST_ID *l = (QFILE_LIST_ID *) calloc (1, sizeof (QFILE_LIST_ID));
  REQUIRE (qfile_type_list_alloc (&l->type_list, (int) doms.size (), hdr) == NO_ERROR);
  for (size_t i = 0; i < doms.size (); i++)
    {
      l->type_list.domp[i] = doms[i];
    }
  qfile_set_layout (&l->type_list);
  fake_list *fl = new fake_list ();
  fl->list_id = l;
  g_lists.push_back (fl);
  l->first_vpid.pageid = (int) g_lists.size ();
  l->first_vpid.volid = 0;
  return l;
}

/* the T_NORMAL tuple of qfile_generate_tuple_into_list: size pass, then fill pass, into an owned buffer */
static char *
make_tuple (QFILE_LIST_ID * l, std::vector<DB_VALUE> &vals)
{
  int n = l->type_list.type_cnt;
  REQUIRE ((int) vals.size () == n);
  std::vector<DB_VALUE *> vp (n);
  std::vector<int> lens (n);
  for (int i = 0; i < n; i++)
    {
      vp[i] = &vals[i];
    }
  bool has_null = false;
  int sz = qfile_tuple_size_from_values (&l->type_list, vp.data (), lens.data (), n, &has_null);
  REQUIRE (sz > 0);
  char *t = (char *) aligned_alloc (8, (sz + 7) / 8 * 8);
  memset (t, 0, (sz + 7) / 8 * 8);
  REQUIRE (qfile_tuple_fill_from_values (&l->type_list, vp.data (), lens.data (), n, t, sz, has_null) == NO_ERROR);
  return t;
}

static void
add_row (QFILE_LIST_ID * l, std::vector<DB_VALUE> vals)
{
  fake_list *fl = fake_list_of (l);
  fl->tuples.push_back (make_tuple (l, vals));
  l->tuple_cnt++;
}

/* stage a row in the list's tuple descriptor as qexec_generate_tuple_descriptor's size pass leaves it */
static void
stage_descr (QFILE_LIST_ID * l, std::vector<DB_VALUE> &vals)
{
  int n = l->type_list.type_cnt;
  QFILE_TUPLE_DESCRIPTOR *d = &l->tpl_descr;
  if (d->f_valp == NULL)
    {
      d->f_valp = (DB_VALUE **) calloc (n, sizeof (DB_VALUE *));
      d->f_len = (int *) calloc (n, sizeof (int));
    }
  for (int i = 0; i < n; i++)
    {
      d->f_valp[i] = &vals[i];
    }
  d->f_cnt = n;
  bool has_null = false;
  d->tpl_size = qfile_tuple_size_from_values (&l->type_list, d->f_valp, d->f_len, n, &has_null);
  d->has_null = has_null;
  REQUIRE (d->tpl_size > 0);
}

/* a record bound to a list's layout, as qfile_retrieve_tuple leaves a PEEKed one */
static QFILE_TUPLE_RECORD
bound (QFILE_LIST_ID * l, char *tuple)
{
  QFILE_TUPLE_RECORD r = QFILE_TUPLE_RECORD_INITIALIZER;
  qfile_slot_set_tuple_ptr_and_layout (&r, tuple, 0, &l->type_list);
  return r;
}

/* ---------------------------------------------------------------- a hash join and its two inputs */

struct join
{
  XASL_NODE *hj;
  XASL_NODE *outer;
  XASL_NODE *inner;
  bool first_is_outer;
  XASL_NODE *first () const
  {
    return first_is_outer ? outer : inner;
  }
  XASL_NODE *second () const
  {
    return first_is_outer ? inner : outer;
  }
};

static XASL_NODE *
new_input (QFILE_LIST_ID * l)
{
  XASL_NODE *x = (XASL_NODE *) calloc (1, sizeof (XASL_NODE));
  x->type = BUILDLIST_PROC;
  x->option = Q_ALL;
  x->status = XASL_INITIALIZED;
  x->list_id = l;
  return x;
}

/*
 * The hash join as stream_to_xasl leaves it (domain_info arrays sized ls_column_cnt, value_indexes = the merge
 * columns) with aptr_list outer -> inner (pt_to_hashjoin_proc). keys: pairs (outer column, inner column).
 */
static join
new_join (JOIN_TYPE jt, const std::vector<TP_DOMAIN *> &outer_doms, const std::vector<TP_DOMAIN *> &inner_doms,
	  const std::vector<std::pair<int, int>> &keys, bool first_is_outer = true)
{
  join j;
  j.outer = new_input (new_list (outer_doms));
  j.inner = new_input (new_list (inner_doms));
  j.outer->next = j.inner;
  j.first_is_outer = first_is_outer;

  XASL_NODE *hj = (XASL_NODE *) calloc (1, sizeof (XASL_NODE));
  hj->type = HASHJOIN_PROC;
  hj->status = XASL_INITIALIZED;
  hj->aptr_list = j.outer;
  HASHJOIN_PROC_NODE *p = &hj->proc.hashjoin;
  p->outer.xasl = j.outer;
  p->inner.xasl = j.inner;
  int k = (int) keys.size ();
  p->merge_info.join_type = jt;
  p->merge_info.ls_column_cnt = k;
  p->merge_info.ls_outer_column = (int *) calloc (k, sizeof (int));
  p->merge_info.ls_inner_column = (int *) calloc (k, sizeof (int));
  for (int i = 0; i < k; i++)
    {
      p->merge_info.ls_outer_column[i] = keys[i].first;
      p->merge_info.ls_inner_column[i] = keys[i].second;
    }
  /* the join's output: outer column 0, inner column 0 */
  p->merge_info.ls_pos_cnt = 2;
  p->merge_info.ls_pos_list = (int *) calloc (2, sizeof (int));
  p->merge_info.ls_outer_inner_list = (int *) calloc (2, sizeof (int));
  p->merge_info.ls_outer_inner_list[0] = QFILE_OUTER_LIST;
  p->merge_info.ls_outer_inner_list[1] = QFILE_INNER_LIST;
  TP_DOMAIN **all = (TP_DOMAIN **) calloc (3 * k + 1, sizeof (TP_DOMAIN *));
  p->domain_info.outer.domains = all;
  p->domain_info.inner.domains = all + k;
  p->domain_info.coerce_domains = all + 2 * k;
  p->domain_info.outer.value_indexes = p->merge_info.ls_outer_column;
  p->domain_info.inner.value_indexes = p->merge_info.ls_inner_column;
  j.hj = hj;
  return j;
}

/* the first input has run to the end (qexec_execute_mainblock: XASL_SUCCESS, list closed) */
static void
first_done (const join & j)
{
  j.first ()->status = XASL_SUCCESS;
}

static unitseam_bloom_view
view (XASL_NODE * x)
{
  unitseam_bloom_view v;
  unitseam_bloom_view_of (x, &v);
  return v;
}

static std::vector<DB_VALUE>
ints (std::initializer_list<int> xs)
{
  std::vector<DB_VALUE> v;
  for (int x : xs)
    {
      DB_VALUE d;
      db_make_int (&d, x);
      v.push_back (d);
    }
  return v;
}

static DB_VALUE
v_int (int x)
{
  DB_VALUE d;
  db_make_int (&d, x);
  return d;
}

static DB_VALUE
v_null ()
{
  DB_VALUE d;
  db_make_null (&d);
  return d;
}

/* the decision on one second-input row, through a record bound to the second list (the hook's well-defined path) */
static int
decide (const join & j, std::vector<DB_VALUE> vals, THREAD_ENTRY * t = nullptr)
{
  QFILE_LIST_ID *l = j.second ()->list_id;
  char *tpl = make_tuple (l, vals);
  QFILE_TUPLE_RECORD r = bound (l, tpl);
  int d = unitseam_bloom_skip_bound (t ? t : owner_thread (), j.second (), &r);
  free (tpl);
  return d;
}

/* an INNER join of INT keys whose first input (outer) holds the given keys, filter attached to the inner */
static join
int_join_with (const std::vector<int> &first_keys, JOIN_TYPE jt = JOIN_INNER)
{
  join j = new_join (jt, { dom_int () }, { dom_int () }, { {0, 0} });
  for (int k : first_keys)
    {
      add_row (j.first ()->list_id, { v_int (k) });
    }
  first_done (j);
  return j;
}

/* ================================================================ B1: no false negative */

TEST_CASE ("B1: every first-input key, as a second-input row, is kept - 100,000 random INT keys", "[contract]")
{
  boot_types ();
  set_bloom_param (true);

  std::mt19937 rng (27659);
  std::vector<int> keys;
  std::unordered_set<int> member;
  while (keys.size () < 100000)
    {
      int k = (int) rng ();
      keys.push_back (k);
      member.insert (k);
    }
  join j = int_join_with (keys);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (view (j.second ()).present);
  REQUIRE_FALSE (view (j.second ()).built);

  int dropped = 0, inactive = 0;
  for (int k : keys)
    {
      int d = decide (j, { v_int (k) });
      dropped += (d == 1);
      inactive += (d == -1);
    }
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.built);
  CHECK_FALSE (v.disabled);
  CHECK (v.keys == 100000);
  CHECK (inactive == 0);
  CHECK (dropped == 0);
  CHECK (v.tested == 100000);
  CHECK (v.skipped == 0);

  hjoin_bloom_release (owner_thread (), j.second ());
  CHECK (j.second ()->hj_bloom == NULL);
}

TEST_CASE ("B1: a two-column key (INT, VARCHAR) - every first-input key pair is kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *vc = dom_string (DB_TYPE_VARCHAR, 40, LANG_COLL_UTF8_BINARY);
  /* outer (a INT, s VARCHAR), inner (s VARCHAR, a INT): keys outer.a = inner.a, outer.s = inner.s */
  join j = new_join (JOIN_INNER, { dom_int (), vc }, { vc, dom_int () }, { {0, 1}, {1, 0} });
  std::mt19937 rng (7);
  std::vector<std::pair<int, std::string>> rows;
  for (int i = 0; i < 5000; i++)
    {
      int a = (int) (rng () % 1000) - 500;
      std::string s = "k" + std::to_string (rng () % 997);
      rows.push_back ({ a, s });
      add_row (j.first ()->list_id, { v_int (a), cast_text (s.c_str (), vc) });
    }
  first_done (j);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (view (j.second ()).present);

  int dropped = 0;
  for (auto &r : rows)
    {
      dropped += decide (j, { cast_text (r.second.c_str (), vc), v_int (r.first) }) == 1;
    }
  CHECK (dropped == 0);
  /* the same pair with one column off is mostly skipped */
  int skipped = 0;
  for (int i = 0; i < 1000; i++)
    {
      skipped += decide (j, { cast_text ("absent", vc), v_int (i) }) == 1;
    }
  CHECK (skipped > 900);
  hjoin_bloom_release (owner_thread (), j.second ());
}

/* ================================================================ B2: coercion pairs */

struct pair_spec
{
  const char *name;
  TP_DOMAIN *first_dom;
  TP_DOMAIN *second_dom;
  std::vector<DB_VALUE> first_vals;
  std::vector<DB_VALUE> second_vals;
  std::vector<std::string> second_text;
};

struct pair_result
{
  int join_matches = 0;		/* second rows some first row joins with */
  int kept_matches = 0;
  int dropped_matches = 0;	/* false negatives */
  int keq_hash_differs = 0;	/* qdata_hscan_key_eq but different hashes (the join itself cannot match them) */
  int keq_not_compare = 0;	/* qdata_hscan_key_eq and the probe's compare disagree */
  int non_matches = 0;
  int non_matches_skipped = 0;
  bool need_coerce = false;
  std::string coerce_dom;
  std::vector<std::string> dropped;
};

static pair_result
run_pair (pair_spec & ps)
{
  /* first = outer, second = inner: INNER join, outer executed first */
  join j = new_join (JOIN_INNER, { ps.first_dom }, { ps.second_dom }, { {0, 0} });
  for (auto &v : ps.first_vals)
    {
      add_row (j.first ()->list_id, { v });
    }
  first_done (j);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (view (j.second ()).present);

  pair_result r;
  QFILE_LIST_ID *fl = j.first ()->list_id, *sl = j.second ()->list_id;
  std::vector<char *> ft;
  for (auto &v : ps.first_vals)
    {
      std::vector<DB_VALUE> one { v };
      ft.push_back (make_tuple (fl, one));
    }
  for (size_t si = 0; si < ps.second_vals.size (); si++)
    {
      std::vector<DB_VALUE> one { ps.second_vals[si] };
      char *st = make_tuple (sl, one);
      QFILE_TUPLE_RECORD srec = bound (sl, st);
      int d = unitseam_bloom_skip_bound (owner_thread (), j.second (), &srec);
      REQUIRE (d != -1);
      if (si == 0)
	{
	  REQUIRE (unitseam_hj_init_domain_info (owner_thread (), j.hj) == NO_ERROR);
	  r.need_coerce = j.hj->proc.hashjoin.domain_info.need_coerce_domains;
	  TP_DOMAIN *cd = j.hj->proc.hashjoin.domain_info.coerce_domains[0];
	  r.coerce_dom = cd ? std::string (pr_type_name (TP_DOMAIN_TYPE (cd))) + "(" + std::to_string (cd->precision)
	    + "," + std::to_string (cd->scale) + ")" : "-";
	}
      bool matched = false;
      for (char *f : ft)
	{
	  QFILE_TUPLE_RECORD frec = bound (fl, f);
	  QFILE_TUPLE_RECORD srec2 = bound (sl, st);
	  bool fnull, snull, keq, ceq;
	  unsigned int fh, sh;
	  REQUIRE (unitseam_hj_join_pair (owner_thread (), j.hj, true, &frec, &srec2, &fnull, &snull, &fh, &sh, &keq, &ceq)
		   == NO_ERROR);
	  if (keq && fh != sh)
	    {
	      r.keq_hash_differs++;
	    }
	  if (keq != ceq)
	    {
	      r.keq_not_compare++;
	    }
	  if (fh == sh && ceq)
	    {
	      matched = true;
	    }
	}
      if (matched)
	{
	  r.join_matches++;
	  if (d == 0)
	    {
	      r.kept_matches++;
	    }
	  else
	    {
	      r.dropped_matches++;
	      r.dropped.push_back (ps.second_text[si]);
	    }
	}
      else
	{
	  r.non_matches++;
	  r.non_matches_skipped += (d == 1);
	}
      free (st);
    }
  for (char *f : ft)
    {
      free (f);
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  return r;
}

static void
check_pair (pair_spec & ps, int min_matches)
{
  pair_result r = run_pair (ps);
  std::string dropped;
  for (auto &s : r.dropped)
    {
      dropped += "[" + s + "] ";
    }
  INFO (ps.name << ": coerce " << (r.need_coerce ? "yes " : "no ") << r.coerce_dom << ", join matches "
	<< r.join_matches << ", kept " << r.kept_matches << ", non-matches " << r.non_matches << " (skipped "
	<< r.non_matches_skipped << "), key_eq-but-hash-differs " << r.keq_hash_differs << ", key_eq!=compare "
	<< r.keq_not_compare << ", dropped matches: " << dropped);
  CHECK (r.join_matches >= min_matches);
  CHECK (r.dropped_matches == 0);
  /* the join's hash and its comparison agree on these values (else the join itself would miss them) */
  CHECK (r.keq_hash_differs == 0);
  WARN (ps.name << ": coerce " << (r.need_coerce ? "yes " : "no ") << r.coerce_dom << ", join matches "
	<< r.join_matches << "/" << ps.second_vals.size () << ", dropped " << r.dropped_matches << ", non-matches skipped "
	<< r.non_matches_skipped << "/" << r.non_matches);
}

static pair_spec
spec_ints (const char *name, TP_DOMAIN * fd, TP_DOMAIN * sd, std::vector<long long> fv, std::vector<long long> sv)
{
  pair_spec ps { name, fd, sd, {}, {}, {} };
  auto mk = [] (TP_DOMAIN * d, long long x)
  {
    DB_VALUE v;
    switch (TP_DOMAIN_TYPE (d))
      {
      case DB_TYPE_INTEGER:
	db_make_int (&v, (int) x);
	break;
      case DB_TYPE_BIGINT:
	db_make_bigint (&v, x);
	break;
      case DB_TYPE_DOUBLE:
	db_make_double (&v, (double) x);
	break;
      default:
	FAIL ("type");
      }
    return v;
  };
  for (long long x : fv)
    {
      ps.first_vals.push_back (mk (fd, x));
    }
  for (long long x : sv)
    {
      ps.second_vals.push_back (mk (sd, x));
      ps.second_text.push_back (std::to_string (x));
    }
  return ps;
}

static pair_spec
spec_text (const char *name, TP_DOMAIN * fd, TP_DOMAIN * sd, std::vector<const char *> fv,
	   std::vector<const char *> sv)
{
  pair_spec ps { name, fd, sd, {}, {}, {} };
  for (const char *x : fv)
    {
      ps.first_vals.push_back (cast_text (x, fd));
    }
  for (const char *x : sv)
    {
      ps.second_vals.push_back (cast_text (x, sd));
      ps.second_text.push_back (x);
    }
  return ps;
}

TEST_CASE ("B2: INT x BIGINT, each way round - a second row the join matches is kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<long long> iv { 0, 1, -1, 5, -5, 123456, -123456, 2147483647, -2147483647 - 1 };
  std::vector<long long> bv { 0, 1, -1, 5, -5, 123456, -123456, 2147483647, -2147483647LL - 1, 7, -7,
    2147483648LL, -2147483649LL, 4294967301LL, -4294967301LL };
  pair_spec a = spec_ints ("INT first x BIGINT second", dom_int (), dom_bigint (), iv, bv);
  check_pair (a, 9);
  std::vector<long long> iv2 { 0, 1, -1, 5, -5, 7, -7, 123456, -123456, 2147483647, -2147483647 - 1, 42 };
  pair_spec b = spec_ints ("BIGINT first x INT second", dom_bigint (), dom_int (), bv, iv2);
  check_pair (b, 11);
}

TEST_CASE ("B2: NUMERIC(10,2) x NUMERIC(20,4), each way round - a second row the join matches is kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *n102 = dom_numeric (10, 2), *n204 = dom_numeric (20, 4);
  pair_spec a = spec_text ("NUMERIC(10,2) first x NUMERIC(20,4) second", n102, n204,
			   { "1.50", "-1.50", "0.00", "12345678.99", "-0.01", "100.10", "-99999999.99" },
			   { "1.5000", "-1.5", "0", "12345678.9900", "-0.0100", "100.1", "-99999999.99", "1.5001",
			   "-0.0001", "123456789012.3456", "100.1100" });
  check_pair (a, 7);
  pair_spec b = spec_text ("NUMERIC(20,4) first x NUMERIC(10,2) second", n204, n102,
			   { "1.5000", "-1.5", "0", "12345678.99", "-0.01", "100.1", "-99999999.99", "1.5001",
			   "123456789012.3456" },
			   { "1.50", "-1.50", "0.00", "12345678.99", "-0.01", "100.10", "-99999999.99", "1.51",
			   "-0.02" });
  check_pair (b, 7);
}

TEST_CASE ("B2: NUMERIC(38,0) x NUMERIC(38,10) (common domain beyond 38 digits: the default-precision path)",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *n380 = dom_numeric (38, 0), *n3810 = dom_numeric (38, 10);
  pair_spec a = spec_text ("NUMERIC(38,0) first x NUMERIC(38,10) second", n380, n3810,
			   { "12345", "-1", "0", "9999999999999999999999999999", "100" },
			   { "12345.0000000000", "-1", "0", "9999999999999999999999999999.0", "100.0000000001", "12345.5",
			   "7" });
  check_pair (a, 4);
  pair_spec b = spec_text ("NUMERIC(38,10) first x NUMERIC(38,0) second", n3810, n380,
			   { "12345.0000000000", "-1", "0", "9999999999999999999999999999.0", "100.0000000001",
			   "12345.5" },
			   { "12345", "-1", "0", "9999999999999999999999999999", "100", "7" });
  check_pair (b, 4);
}

TEST_CASE ("B2: INT x DOUBLE, each way round - a second row the join matches is kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<long long> v { 0, 1, -1, 5, -5, 123456, -123456, 2147483647 };
  std::vector<long long> w { 0, 1, -1, 5, -5, 123456, -123456, 2147483647, 3, -3 };
  pair_spec a = spec_ints ("INT first x DOUBLE second", dom_int (), dom_double (), v, w);
  check_pair (a, 8);
  pair_spec b = spec_ints ("DOUBLE first x INT second", dom_double (), dom_int (), v, w);
  check_pair (b, 8);
}

TEST_CASE ("B2: CHAR(10) x VARCHAR(20) with trailing blanks, each way round - a second row the join matches is kept",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *c10 = dom_string (DB_TYPE_CHAR, 10, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *v20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_BINARY);
  /* the join compares CHAR (blank-padded to 10) and VARCHAR with trailing blanks significant (the coerced VARCHAR keeps
   * the pad), while mht_get_hash_number ignores trailing ASCII blanks: 'ab' x 'ab' do not join, 'ab' x 'ab        ' do */
  pair_spec a = spec_text ("CHAR(10) first x VARCHAR(20) second", c10, v20,
			   { "ab", "a", "abc  ", "", "x y", " lead" },
			   { "ab", "ab ", "ab        ", "a", "a         ", "abc", "abc ", "abc       ", "", " ",
			   "          ", "x y", "x y       ", " lead", " lead     ", "AB", "b", "x  y" });
  check_pair (a, 5);
  pair_spec b = spec_text ("VARCHAR(20) first x CHAR(10) second", v20, c10,
			   { "ab", "ab ", "ab        ", "a", "abc   ", "", "x y", "x y       ", " lead", " lead     " },
			   { "ab", "a", "abc", "", "x y", " lead", "AB", "b" });
  check_pair (b, 3);
}

TEST_CASE ("B2: utf8_en_ci ('A' vs 'a'), same and different precision, CHAR x VARCHAR - a second row the join matches "
	   "is kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *v20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_EN_CI);
  TP_DOMAIN *v10 = dom_string (DB_TYPE_VARCHAR, 10, LANG_COLL_UTF8_EN_CI);
  TP_DOMAIN *v30 = dom_string (DB_TYPE_VARCHAR, 30, LANG_COLL_UTF8_EN_CI);
  TP_DOMAIN *c10 = dom_string (DB_TYPE_CHAR, 10, LANG_COLL_UTF8_EN_CI);
  std::vector<const char *> fv { "A", "abc", "Hello", "MiXeD cAsE", "z" };
  /* "Hello " does not join "Hello": trailing blanks are significant in the comparison (not in the hash) */
  std::vector<const char *> sv { "a", "A", "ABC", "aBc", "hello", "HELLO", "Hello ", "mixed case", "Z", "b", "abcd" };
  pair_spec a = spec_text ("VARCHAR(20) utf8_en_ci x VARCHAR(20) utf8_en_ci", v20, v20, fv, sv);
  check_pair (a, 8);
  pair_spec b = spec_text ("VARCHAR(10) utf8_en_ci first x VARCHAR(30) utf8_en_ci second", v10, v30, fv, sv);
  check_pair (b, 8);
  pair_spec c = spec_text ("VARCHAR(30) utf8_en_ci first x VARCHAR(10) utf8_en_ci second", v30, v10, fv, sv);
  check_pair (c, 8);
  /* CHAR(10) values are blank-padded: the padded VARCHAR spellings are the ones that join */
  std::vector<const char *> sv_pad (sv);
  sv_pad.insert (sv_pad.end (), { "a         ", "HELLO     ", "ABC       " });
  pair_spec d = spec_text ("CHAR(10) utf8_en_ci first x VARCHAR(20) utf8_en_ci second", c10, v20, fv, sv_pad);
  check_pair (d, 4);
  std::vector<const char *> fv_pad (fv);
  fv_pad.insert (fv_pad.end (), { "abc       ", "z         ", "hello     " });
  pair_spec e = spec_text ("VARCHAR(20) utf8_en_ci first x CHAR(10) utf8_en_ci second", v20, c10, fv_pad, sv);
  check_pair (e, 5);
}

/* ================================================================ B3: NULL keys, false positives, sizing */

TEST_CASE ("B3: a NULL-key second row is skipped and counted; a NULL first-input key adds nothing", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
  add_row (j.first ()->list_id, { v_int (1) });
  add_row (j.first ()->list_id, { v_null () });
  add_row (j.first ()->list_id, { v_int (2) });
  first_done (j);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  CHECK (decide (j, { v_null () }) == 1);
  CHECK (decide (j, { v_int (1) }) == 0);
  CHECK (decide (j, { v_int (2) }) == 0);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.keys == 2);
  CHECK (v.tested == 3);
  CHECK (v.skipped == 1);
  hjoin_bloom_release (owner_thread (), j.second ());

  /* every first-input key NULL: no second row can join, every one is skipped */
  join n = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
  add_row (n.first ()->list_id, { v_null () });
  add_row (n.first ()->list_id, { v_null () });
  first_done (n);
  hjoin_bloom_attach (owner_thread (), n.hj, n.second ());
  CHECK (decide (n, { v_int (0) }) == 1);
  CHECK (decide (n, { v_int (1) }) == 1);
  CHECK (view (n.second ()).keys == 0);
  hjoin_bloom_release (owner_thread (), n.second ());
}

/* 100,000 random distinct INT keys and 100,000 random absent probes; 1..100,000 and 100,001..200,000 */
struct fpr_measure
{
  double theory;
  double random_keys;
  double sequential_keys;
  unsigned long long bits;
};

static fpr_measure
measure_fpr ()
{
  fpr_measure f;
  std::mt19937 rng (16);
  std::unordered_set<int> member;
  std::vector<int> keys;
  while (keys.size () < 100000)
    {
      int k = (int) rng ();
      if (member.insert (k).second)
	{
	  keys.push_back (k);
	}
    }
  join j = int_join_with (keys);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (view (j.second ()).present);
  int probes = 0, passed = 0;
  while (probes < 100000)
    {
      int k = (int) rng ();
      if (member.count (k))
	{
	  continue;
	}
      probes++;
      passed += decide (j, { v_int (k) }) == 0;
    }
  unitseam_bloom_view v = view (j.second ());
  f.bits = (unsigned long long) v.mask + 1;
  f.theory = pow (1.0 - exp (-3.0 * 100000.0 / (double) f.bits), 3.0);
  f.random_keys = (double) passed / probes;
  hjoin_bloom_release (owner_thread (), j.second ());

  std::vector<int> seq;
  for (int i = 1; i <= 100000; i++)
    {
      seq.push_back (i);
    }
  join s = int_join_with (seq);
  hjoin_bloom_attach (owner_thread (), s.hj, s.second ());
  int spassed = 0;
  for (int i = 100001; i <= 200000; i++)
    {
      spassed += decide (s, { v_int (i) }) == 0;
    }
  f.sequential_keys = spassed / 100000.0;
  hjoin_bloom_release (owner_thread (), s.second ());
  return f;
}

TEST_CASE ("B3: keys absent from the first input are mostly skipped - false-positive rate under 5% at 100,000 keys "
	   "(2^20 bits, k=3; theory 1.54%)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  fpr_measure f = measure_fpr ();
  WARN ("bits " << f.bits << " (" << f.bits / 100000.0 << " per key), theory " << f.theory << ", measured: random keys "
	<< f.random_keys << ", sequential keys " << f.sequential_keys);
  CHECK (f.bits == (1ull << 20));
  CHECK (f.random_keys > f.theory / 3);
  CHECK (f.random_keys < 0.05);
  CHECK (f.sequential_keys < 0.05);
}

TEST_CASE ("PR27659 F4: the false-positive rate is about twice the theory - the second probe stride rotl16(hash)|1 shares "
	   "bits with the first position (hash & mask)", "[defect]")
{
  boot_types ();
  set_bloom_param (true);
  fpr_measure f = measure_fpr ();
  WARN ("theory " << f.theory << ", measured random " << f.random_keys << " (" << f.random_keys / f.theory
	<< "x), sequential " << f.sequential_keys << " (" << f.sequential_keys / f.theory << "x)");
  CHECK (f.random_keys > 1.8 * f.theory);
  CHECK (f.sequential_keys > 1.8 * f.theory);
}

TEST_CASE ("B3: bit array = the power of two >= 10 x first-input row count, at least 64 bits (10,000,000 rows: 2^27 bits "
	   "= 16 MiB)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  struct
  {
    long long rows;
    unsigned int bits;
  } cases[] = { {1, 64}, {6, 64}, {7, 128}, {1000, 16384}, {100000, 1u << 20}, {10000000, 1u << 27} };
  for (auto &c : cases)
    {
      join j = int_join_with ({ 1, 2, 3 });
      j.first ()->list_id->tuple_cnt = c.rows;	/* the sizing reads tuple_cnt; the scan reads the three tuples */
      hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
      REQUIRE (view (j.second ()).present);
      unitseam_bloom_build (owner_thread (), j.second ());
      INFO ("rows " << c.rows);
      CHECK ((unsigned long long) view (j.second ()).mask + 1 == c.bits);
      CHECK (decide (j, { v_int (2) }) == 0);
      hjoin_bloom_release (owner_thread (), j.second ());
    }
}

/* ================================================================ B4: where it must not attach or act */

struct attach_case
{
  const char *what;
  bool expect;
  std::function<void (join &)> setup;
};

static bool
attached_after (join & j)
{
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  bool a = j.second ()->hj_bloom != NULL;
  if (a)
    {
      unitseam_bloom_view v = view (j.second ());
      CHECK (v.owner_xasl == j.second ());
      CHECK (v.owner_thread == owner_thread ());
      CHECK (v.hj_xasl == j.hj);
      CHECK (v.first_is_outer == j.first_is_outer);
      CHECK_FALSE (v.built);
      hjoin_bloom_release (owner_thread (), j.second ());
    }
  return a;
}

static join
attach_join (JOIN_TYPE jt, bool first_is_outer)
{
  join j = new_join (jt, { dom_int () }, { dom_int () }, { {0, 0} }, first_is_outer);
  add_row (j.first ()->list_id, { v_int (1) });
  add_row (j.first ()->list_id, { v_int (2) });
  first_done (j);
  return j;
}

TEST_CASE ("B4: attach follows the preserved side - INNER either way, LEFT inner second, RIGHT outer second; never a "
	   "preserved second (RIGHT inner, LEFT outer, FULL either)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  struct
  {
    JOIN_TYPE jt;
    bool first_is_outer;
    bool expect;
    const char *what;
  } cases[] = {
    {JOIN_INNER, true, true, "INNER, outer first, inner second"},
    {JOIN_INNER, false, true, "INNER, inner first, outer second"},
    {JOIN_LEFT, true, true, "LEFT, outer first, inner second (not preserved)"},
    {JOIN_LEFT, false, false, "LEFT, inner first, outer second (preserved)"},
    {JOIN_RIGHT, true, false, "RIGHT, outer first, inner second (preserved)"},
    {JOIN_RIGHT, false, true, "RIGHT, inner first, outer second (not preserved)"},
    {JOIN_OUTER, true, false, "FULL, outer first"},
    {JOIN_OUTER, false, false, "FULL, inner first"},
  };
  for (auto &c : cases)
    {
      join j = attach_join (c.jt, c.first_is_outer);
      INFO (c.what);
      CHECK (attached_after (j) == c.expect);
    }
}

TEST_CASE ("B4: attach needs the parameter, a finished first input (XASL_SUCCESS) with 1..10,000,000 rows, and a second "
	   "input that writes each row straight to its list (no ORDERBY_NUM / top-N limit, not an UPDATE/DELETE select, no "
	   "subquery cache)", "[contract]")
{
  boot_types ();
  /* attach only tests these pointers for NULL */
  alignas (64) static char dummy[4096];
  REGU_VARIABLE *regu = (REGU_VARIABLE *) dummy;
  SORT_LIST *sl = (SORT_LIST *) dummy;
  ANALYTIC_EVAL_TYPE *ae = (ANALYTIC_EVAL_TYPE *) dummy;
  XASL_NODE *cb = (XASL_NODE *) dummy;
  SELUPD_LIST *su = (SELUPD_LIST *) dummy;
  PRED_EXPR *pe = (PRED_EXPR *) dummy;
  DB_VALUE *dv = (DB_VALUE *) dummy;
  XASL_ID *xid = (XASL_ID *) dummy;

  std::vector<attach_case> cases = {
    {"control: INNER, first 2 rows done", true, [] (join &) {}},
    {"parameter off", false, [] (join &) { set_bloom_param (false); }},
    {"first status XASL_INITIALIZED (not run yet)", false, [] (join & j) { j.first ()->status = XASL_INITIALIZED; }},
    {"first status XASL_CLEARED", false, [] (join & j) { j.first ()->status = XASL_CLEARED; }},
    {"first status XASL_FAILURE", false, [] (join & j) { j.first ()->status = XASL_FAILURE; }},
    {"first status XASL_BUILD", false, [] (join & j) { j.first ()->status = XASL_BUILD; }},
    {"first 0 rows", false, [] (join & j) { j.first ()->list_id->tuple_cnt = 0; }},
    {"first exactly 10,000,000 rows", true, [] (join & j) { j.first ()->list_id->tuple_cnt = 10000000; }},
    {"first 10,000,001 rows", false, [] (join & j) { j.first ()->list_id->tuple_cnt = 10000001; }},
    {"first list never opened (type_cnt 0)", false, [] (join & j) { j.first ()->list_id->type_list.type_cnt = 0; }},
    {"first list_id NULL", false, [] (join & j) { j.first ()->list_id = NULL; }},
    {"second GROUP BY", false, [&] (join & j) { j.second ()->proc.buildlist.groupby_list = sl; }},
    {"second analytic", false, [&] (join & j) { j.second ()->proc.buildlist.a_eval_list = ae; }},
    {"second hash GROUP BY", false, [] (join & j) { j.second ()->proc.buildlist.g_hash_eligible = 1; }},
    {"second LIMIT", false, [&] (join & j) { j.second ()->limit_row_count = regu; }},
    {"second OFFSET", false, [&] (join & j) { j.second ()->limit_offset = regu; }},
    {"second DISTINCT", false, [] (join & j) { j.second ()->option = Q_DISTINCT; }},
    {"second CONNECT BY", false, [&] (join & j) { j.second ()->connect_by_ptr = cb; }},
    {"second click counter (selected_upd_list)", false, [&] (join & j) { j.second ()->selected_upd_list = su; }},
    {"second ORDERBY_NUM () predicate (ordbynum_pred)", false, [&] (join & j) { j.second ()->ordbynum_pred = pe; }},
    {"second ORDERBY_NUM () value (ordbynum_val)", false, [&] (join & j) { j.second ()->ordbynum_val = dv; }},
    {"second top-N sort limit (orderby_limit)", false, [&] (join & j) { j.second ()->orderby_limit = regu; }},
    {"second is the SELECT of an UPDATE/DELETE (upd_del_class_cnt 1)", false,
     [] (join & j) { j.second ()->upd_del_class_cnt = 1; }},
    {"second list from the subquery result cache (sub_xasl_id)", false,
     [&] (join & j) { j.second ()->sub_xasl_id = xid; }},
    {"second top-most", false, [] (join & j) { XASL_SET_FLAG (j.second (), XASL_TOP_MOST_XASL); }},
    {"second not a BUILDLIST (BUILDVALUE)", false, [] (join & j) { j.second ()->type = BUILDVALUE_PROC; }},
    {"hash join node not a HASHJOIN_PROC (MERGELIST)", false, [] (join & j) { j.hj->type = MERGELIST_PROC; }},
  };
  for (auto &c : cases)
    {
      set_bloom_param (true);
      join j = attach_join (JOIN_INNER, true);
      c.setup (j);
      INFO (c.what);
      CHECK (attached_after (j) == c.expect);
    }
  set_bloom_param (true);

  /* an XASL that is not an input of this join, and a second that already carries a filter */
  join j = attach_join (JOIN_INNER, true);
  XASL_NODE *stranger = new_input (new_list ({ dom_int () }));
  hjoin_bloom_attach (owner_thread (), j.hj, stranger);
  CHECK (stranger->hj_bloom == NULL);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  struct hjoin_bloom *first_filter = j.second ()->hj_bloom;
  REQUIRE (first_filter != NULL);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  CHECK (j.second ()->hj_bloom == first_filter);
  hjoin_bloom_release (owner_thread (), j.second ());
  /* NULL arguments */
  hjoin_bloom_attach (owner_thread (), NULL, j.second ());
  hjoin_bloom_attach (owner_thread (), j.hj, NULL);
  CHECK (j.second ()->hj_bloom == NULL);
}

TEST_CASE ("B4: a thread other than the owner and a cloned node let every row through; release on a clone frees nothing",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = int_join_with ({ 10, 20, 30 });
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (view (j.second ()).present);

  std::vector<DB_VALUE> absent = ints ({ 99 });
  QFILE_LIST_ID *sl = j.second ()->list_id;
  stage_descr (sl, absent);
  char *tpl = make_tuple (sl, absent);

  /* another thread: no build, no decision (these return before reading the row) */
  CHECK_FALSE (hjoin_bloom_skip_descr (other_thread (), j.second ()));
  CHECK_FALSE (hjoin_bloom_skip_tuple (other_thread (), j.second (), tpl));
  CHECK (decide (j, ints ({ 99 }), other_thread ()) == -1);
  CHECK_FALSE (view (j.second ()).built);
  CHECK (view (j.second ()).tested == 0);

  /* the owner builds and decides */
  CHECK (decide (j, ints ({ 99 })) == 1);
  CHECK (decide (j, ints ({ 20 })) == 0);
  unitseam_bloom_view before = view (j.second ());
  REQUIRE (before.built);

  /* a clone carrying the pointer (a parallel worker's node) */
  XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
  memcpy (clone, j.second (), sizeof (XASL_NODE));
  REQUIRE (clone->hj_bloom == j.second ()->hj_bloom);
  CHECK_FALSE (hjoin_bloom_skip_descr (owner_thread (), clone));
  CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), clone, tpl));
  {
    QFILE_TUPLE_RECORD r = bound (sl, tpl);
    CHECK (unitseam_bloom_skip_bound (owner_thread (), clone, &r) == -1);
  }
  /* detach on the clone moves nothing */
  HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  g->used = false;
  g->keys = -7;
  hjoin_bloom_detach (owner_thread (), clone);
  CHECK (g->keys == -7);
  CHECK_FALSE (g->used);
  /* release on the clone: the clone forgets it, nothing is freed */
  hjoin_bloom_release (owner_thread (), clone);
  CHECK (clone->hj_bloom == NULL);
  struct hjoin_bloom *b = j.second ()->hj_bloom;
  REQUIRE (b != NULL);
  CHECK (live ().count (b) == 1);
  CHECK (live ().count ((void *) before.bits) == 1);
  CHECK (live ().count ((void *) before.key) == 1);
  CHECK (view (j.second ()).tested == before.tested);
  CHECK (decide (j, ints ({ 30 })) == 0);

  /* release on the owner frees the filter, its bits, its key and its row buffer */
  CHECK (hjoin_bloom_skip_descr (other_thread (), j.second ()) == false);
  unitseam_bloom_view last = view (j.second ());
  hjoin_bloom_release (owner_thread (), j.second ());
  CHECK (j.second ()->hj_bloom == NULL);
  CHECK (live ().count (b) == 0);
  CHECK (live ().count ((void *) last.bits) == 0);
  CHECK (live ().count ((void *) last.key) == 0);
  if (last.row_buf != NULL)
    {
      CHECK (live ().count ((void *) last.row_buf) == 0);
    }
  free (clone);
  free (tpl);
}

TEST_CASE ("B4: a build that cannot complete lets every row through (open fails, scan error with an error id)",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  {
    join j = int_join_with ({ 1, 2, 3 });
    fake_list_of (j.first ()->list_id)->fail_open = true;
    hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
    CHECK (decide (j, ints ({ 99 })) == -1);
    CHECK (decide (j, ints ({ 1 })) == -1);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.built);
    CHECK (v.disabled);
    CHECK (er_errid () == NO_ERROR);	/* the filter is an optimization: its failure is cleared */
    hjoin_bloom_detach (owner_thread (), j.second ());
    CHECK_FALSE (j.hj->proc.hashjoin.bloom_stats.used);
  }
  {
    join j = int_join_with ({ 1, 2, 3, 4, 5, 6 });
    fake_list_of (j.first ()->list_id)->stop_at = 3;
    fake_list_of (j.first ()->list_id)->stop_sets_error = true;
    hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
    CHECK (decide (j, ints ({ 5 })) == -1);
    CHECK (view (j.second ()).disabled);
    CHECK (er_errid () == NO_ERROR);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
}

/* ================================================================ B5: detach */

TEST_CASE ("B5: detach moves keys / tested / skipped to the hash join's bloom_stats, marks it used, frees the filter",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = int_join_with ({ 1, 2, 3, 4 });
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  CHECK (decide (j, ints ({ 2 })) == 0);
  CHECK (decide (j, ints ({ 4 })) == 0);
  CHECK (decide (j, { v_null () }) == 1);
  int extra = 0;
  for (int k = 100; k < 140; k++)
    {
      extra += decide (j, ints ({ k })) == 1;
    }
  unitseam_bloom_view v = view (j.second ());
  struct hjoin_bloom *b = j.second ()->hj_bloom;
  hjoin_bloom_detach (owner_thread (), j.second ());
  HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  CHECK (g->used);
  CHECK (g->keys == 4);
  CHECK (g->tested == 43);
  CHECK (g->skipped == 1 + extra);
  CHECK (g->tested == v.tested);
  CHECK (g->skipped == v.skipped);
  CHECK (j.second ()->hj_bloom == NULL);
  CHECK (live ().count (b) == 0);

  /* attached but no row came: not used, nothing counted */
  join e = int_join_with ({ 1 });
  hjoin_bloom_attach (owner_thread (), e.hj, e.second ());
  hjoin_bloom_detach (owner_thread (), e.second ());
  CHECK_FALSE (e.hj->proc.hashjoin.bloom_stats.used);
  CHECK (e.hj->proc.hashjoin.bloom_stats.keys == 0);
  CHECK (e.second ()->hj_bloom == NULL);
}

/* a re-execution must not print the previous execution's BLOOM line: attach starts the trace empty */
static void
stale_bloom_stats (const join & j)
{
  HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  g->used = true;
  g->keys = 5;
  g->tested = 6;
  g->skipped = 7;
}

static bool
bloom_stats_clear (const join & j)
{
  const HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  return !g->used && g->keys == 0 && g->tested == 0 && g->skipped == 0;
}

TEST_CASE ("B5: attach clears the hash join's bloom_stats at each execution, also when it attaches nothing (the aptr "
	   "loop's call for the first input, parameter off, a preserved second side)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  {
    /* the aptr loop reaches attach for the outer first, while the inner has not run */
    join j = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
    add_row (j.inner->list_id, { v_int (1) });
    stale_bloom_stats (j);
    hjoin_bloom_attach (owner_thread (), j.hj, j.outer);
    CHECK (j.outer->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    join j = int_join_with ({ 1, 2 });
    stale_bloom_stats (j);
    set_bloom_param (false);
    hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
    set_bloom_param (true);
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    join j = int_join_with ({ 1, 2 }, JOIN_RIGHT);	/* outer first: the inner is RIGHT's preserved side */
    stale_bloom_stats (j);
    hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    /* attached: cleared too, and the detach of this execution fills it */
    join j = int_join_with ({ 1, 2 });
    stale_bloom_stats (j);
    hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
    REQUIRE (j.second ()->hj_bloom != NULL);
    CHECK (bloom_stats_clear (j));
    CHECK (decide (j, ints ({ 2 })) == 0);
    hjoin_bloom_detach (owner_thread (), j.second ());
    CHECK (j.hj->proc.hashjoin.bloom_stats.used);
    CHECK (j.hj->proc.hashjoin.bloom_stats.keys == 2);
    CHECK (j.hj->proc.hashjoin.bloom_stats.tested == 1);
  }
}

/* ================================================================ F2: the trace counters survive the join's start */

TEST_CASE ("F2 contract: the BLOOM counters detach wrote survive hjoin_init_manager under trace - bloom_stats is "
	   "outside the stats_group it clears (PR27659 F2 on 5a8c0b7d8)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = int_join_with ({ 1, 2, 3 });
  add_row (j.second ()->list_id, ints ({ 2 }));	/* the second input's list has its rows when the join starts */
  j.second ()->status = XASL_SUCCESS;
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  CHECK (decide (j, ints ({ 2 })) == 0);
  CHECK (decide (j, ints ({ 77 })) == 1);
  hjoin_bloom_detach (owner_thread (), j.hj->proc.hashjoin.inner.xasl);
  HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  REQUIRE (g->used);		/* the aptr loop's detach did its part */
  REQUIRE (g->keys == 3);
  REQUIRE (g->tested == 2);
  REQUIRE (g->skipped == 1);

  /* qexec_execute_mainblock_internal: aptr loop (attach .. detach) -> HASHJOIN_PROC -> qexec_hash_join ->
   * hjoin_init_manager, which memsets proc->stats_group when the thread is on trace. A sentinel in the stats_group
   * shows the trace branch ran (otherwise the counters would survive for the wrong reason). */
  j.hj->proc.hashjoin.stats_group.context_cnt = 99;
  owner_thread ()->on_trace = true;
  int rc = unitseam_hj_init_manager (owner_thread (), j.hj);
  owner_thread ()->on_trace = false;
  REQUIRE (rc == NO_ERROR);
  REQUIRE (j.hj->proc.hashjoin.stats_group.context_cnt == 0);
  /* qdump_print_hashjoin_stats_text prints BLOOM when proc->bloom_stats.used */
  CHECK (g->used);
  CHECK (g->keys == 3);
  CHECK (g->tested == 2);
  CHECK (g->skipped == 1);
}

/* ================================================================ F3 (latent): a partial filter */

TEST_CASE ("PR27659 F3 (latent): a first-input scan that stops on S_ERROR without an error id leaves a partial filter "
	   "on - first-input keys past the stop are dropped", "[defect]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys;
  for (int i = 1; i <= 50; i++)
    {
      keys.push_back (i * 1000);
    }
  join j = int_join_with (keys);
  fake_list_of (j.first ()->list_id)->stop_at = 10;
  fake_list_of (j.first ()->list_id)->stop_sets_error = false;
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  int dropped = 0;
  for (int k : keys)
    {
      dropped += decide (j, ints ({ k })) == 1;
    }
  unitseam_bloom_view v = view (j.second ());
  WARN ("filter on after S_ERROR without an error id: disabled=" << v.disabled << " keys=" << v.keys << " dropped "
	<< dropped << " of 50 first-input keys");
  CHECK_FALSE (v.disabled);
  CHECK (v.keys == 10);
  CHECK (dropped > 30);
  hjoin_bloom_release (owner_thread (), j.second ());
}

/* ================================================================ F1: the unbound record */

/*
 * Fill the stack below the caller with a pattern, so that the next call made from the same frame starts with those
 * bytes in its locals (the TU and this file are compiled at -O0 -fno-inline by unitseam).
 *   ZERO   all zero: a record whose type_list is NULL
 *   DECOY  every 8-byte word = DECOY_ADDR, where the child maps a layout-ready type list of a different list (8-byte
 *          backward header, two INT columns): a record whose type_list points at another list's layout - what a stack
 *          slot that last held a bound QFILE_TUPLE_RECORD of another scan looks like. DECOY_ADDR's low 16 bits are
 *          0x8000, so the stale position cache reads as "not started" and the walker starts it from the decoy layout.
 */
enum poison_kind
{ POISON_ZERO, POISON_DECOY };
static const uintptr_t DECOY_ADDR = 0x40008000;

__attribute__ ((noinline)) static void
poison_stack (poison_kind kind)
{
  volatile uint64_t buf[4096];
  for (int i = 0; i < 4096; i++)
    {
      buf[i] = kind == POISON_ZERO ? 0 : (uint64_t) DECOY_ADDR;
    }
  asm volatile ("":::"memory");
}

static bool
map_decoy ()
{
  void *p = mmap ((void *) DECOY_ADDR, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
		  -1, 0);
  if (p != (void *) DECOY_ADDR)
    {
      return false;
    }
  QFILE_TUPLE_VALUE_TYPE_LIST *d = (QFILE_TUPLE_VALUE_TYPE_LIST *) p;
  memset (d, 0, sizeof (*d));
  d->type_cnt = 2;
  d->hdr_size = QFILE_TUPLE_HDR_SIZE_BACKWARD;
  d->domp = (TP_DOMAIN **) ((char *) p + 512);
  d->column_layout_array = (QFILE_COL_LAYOUT *) (d->domp + 2);
  d->domp[0] = dom_int ();
  d->domp[1] = dom_int ();
  qfile_set_layout (d);
  return d->layout_ready;
}

enum entry_kind
{ ENTRY_DESCR, ENTRY_TUPLE, ENTRY_BOUND };

/*
 * Run one decision in a child: 0 kept, 1 skipped, 3 setup failed, 100 + signal if it died.
 */
static int
poisoned_decision (poison_kind kind, entry_kind entry, XASL_NODE * second, char *tuple)
{
  fflush (stdout);
  fflush (stderr);
  pid_t pid = fork ();
  if (pid == 0)
    {
      /* the child must die as the server would, not through Catch2's fatal-signal reporter (it would write into
       * the parent's report) */
      for (int sig : { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL })
	{
	  signal (sig, SIG_DFL);
	}
      if (kind == POISON_DECOY && !map_decoy ())
	{
	  _exit (3);
	}
      QFILE_TUPLE_RECORD r = bound (second->list_id, tuple);
      bool s = false;
      int d = 0;
      poison_stack (kind);
      switch (entry)
	{
	case ENTRY_DESCR:
	  s = hjoin_bloom_skip_descr (owner_thread (), second);
	  break;
	case ENTRY_TUPLE:
	  s = hjoin_bloom_skip_tuple (owner_thread (), second, tuple);
	  break;
	case ENTRY_BOUND:
	  d = unitseam_bloom_skip_bound (owner_thread (), second, &r);
	  s = d == 1;
	  if (d == -1)
	    {
	      _exit (3);
	    }
	  break;
	}
      _exit (s ? 1 : 0);
    }
  int st = 0;
  waitpid (pid, &st, 0);
  if (WIFSIGNALED (st))
    {
      return 100 + WTERMSIG (st);
    }
  return WEXITSTATUS (st);
}

/* the scene: first input keys 1..100 (INT); second input rows (key INT, payload INT); the row under test has key 42 (a
 * first-input key: must be kept) and payload 900000 (not a first-input key: the filter skips a row with that key) */
struct f1_scene
{
  join j;
  std::vector<DB_VALUE> *row;	/* staged in the second list's tuple descriptor: must not move */
  char *tuple;
};

static f1_scene
f1_setup ()
{
  boot_types ();
  set_bloom_param (true);
  f1_scene s;
  s.j = new_join (JOIN_INNER, { dom_int () }, { dom_int (), dom_int () }, { {0, 0} });
  for (int i = 1; i <= 100; i++)
    {
      add_row (s.j.first ()->list_id, ints ({ i }));
    }
  first_done (s.j);
  hjoin_bloom_attach (owner_thread (), s.j.hj, s.j.second ());
  REQUIRE (s.j.second ()->hj_bloom != NULL);
  unitseam_bloom_build (owner_thread (), s.j.second ());
  REQUIRE_FALSE (view (s.j.second ()).disabled);
  /* the precondition: through a bound record, key 42 is kept and key 900000 is skipped */
  REQUIRE (decide (s.j, ints ({ 42, 900000 })) == 0);
  REQUIRE (decide (s.j, ints ({ 900000, 42 })) == 1);
  s.row = new std::vector<DB_VALUE> (ints ({ 42, 900000 }));
  stage_descr (s.j.second ()->list_id, *s.row);
  s.tuple = make_tuple (s.j.second ()->list_id, *s.row);
  return s;
}

static std::string
outcome (int rc)
{
  if (rc == 0)
    {
      return "kept";
    }
  if (rc == 1)
    {
      return "skipped (a matching row dropped)";
    }
  if (rc == 3)
    {
      return "setup failed";
    }
  if (rc >= 100)
    {
      return std::string ("died: ") + strsignal (rc - 100);
    }
  return "exit " + std::to_string (rc);
}

TEST_CASE ("F1 control: on the same poisoned stacks, a record bound to the second list's layout reads the row right "
	   "(key 42 kept)", "[contract]")
{
  f1_scene s = f1_setup ();
  int z = poisoned_decision (POISON_ZERO, ENTRY_BOUND, s.j.second (), s.tuple);
  int d = poisoned_decision (POISON_DECOY, ENTRY_BOUND, s.j.second (), s.tuple);
  INFO ("zeroed stack: " << outcome (z) << "; stale layout on the stack: " << outcome (d));
  REQUIRE (d != 3);		/* the decoy page could not be mapped at DECOY_ADDR: the scene is void, not passed */
  CHECK (z == 0);
  CHECK (d == 0);
}

TEST_CASE ("PR27659 F1: hjoin_bloom_skip_descr reads the row through a QFILE_TUPLE_RECORD it never binds - zeroed "
	   "stack: crash; another list's layout on the stack: the matching row is dropped", "[defect]")
{
  f1_scene s = f1_setup ();
  int z = poisoned_decision (POISON_ZERO, ENTRY_DESCR, s.j.second (), s.tuple);
  int d = poisoned_decision (POISON_DECOY, ENTRY_DESCR, s.j.second (), s.tuple);
  WARN ("hjoin_bloom_skip_descr, key 42 (a first-input key): zeroed stack -> " << outcome (z)
	<< "; stale layout on the stack -> " << outcome (d));
  REQUIRE (d != 3);		/* decoy not mapped: the scene is void */
  CHECK (z >= 100);
  CHECK (d == 1);
}

TEST_CASE ("PR27659 F1: hjoin_bloom_skip_tuple (big-record / set path) - the same unbound record", "[defect]")
{
  f1_scene s = f1_setup ();
  int z = poisoned_decision (POISON_ZERO, ENTRY_TUPLE, s.j.second (), s.tuple);
  int d = poisoned_decision (POISON_DECOY, ENTRY_TUPLE, s.j.second (), s.tuple);
  WARN ("hjoin_bloom_skip_tuple, key 42 (a first-input key): zeroed stack -> " << outcome (z)
	<< "; stale layout on the stack -> " << outcome (d));
  REQUIRE (d != 3);		/* decoy not mapped: the scene is void */
  CHECK (z >= 100);
  CHECK (d == 1);
}

/* ================================================================ v3 (1a7bccdcc): serial inputs, prepare, workers */

/*
 * The parallel-path API 1a7bccdcc added. Declared weak so the suite still links on a revision without it
 * (fdc3fcd32): there the addresses are null and the B6-B8 cases are not registered at all ("not applicable" - the case
 * is absent, so unit_regress shows no base result for it), while on 1a7bccdcc and later they resolve to the TU's
 * definitions. The typedefs repeat query_hash_join.h of 1a7bccdcc (a repeated typedef of the same type is allowed).
 */
typedef struct hjoin_bloom HJOIN_BLOOM;
typedef struct hjoin_bloom_worker HJOIN_BLOOM_WORKER;
bool hjoin_bloom_wants_serial (XASL_NODE * hj_xasl) __attribute__ ((weak));
HJOIN_BLOOM *hjoin_bloom_prepare (THREAD_ENTRY * thread_p, XASL_NODE * xasl) __attribute__ ((weak));
HJOIN_BLOOM_WORKER *hjoin_bloom_worker_begin (THREAD_ENTRY * thread_p, HJOIN_BLOOM * bloom) __attribute__ ((weak));
bool hjoin_bloom_worker_skip_descr (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker, QFILE_LIST_ID * list_id)
  __attribute__ ((weak));
bool hjoin_bloom_worker_skip_tuple (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker, QFILE_LIST_ID * list_id,
				    char *tuple) __attribute__ ((weak));
void hjoin_bloom_worker_end (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker) __attribute__ ((weak));

static int
v3_api_linked_count ()
{
  return (hjoin_bloom_wants_serial != nullptr) + (hjoin_bloom_prepare != nullptr) + (hjoin_bloom_worker_begin != nullptr)
    + (hjoin_bloom_worker_skip_descr != nullptr) + (hjoin_bloom_worker_skip_tuple != nullptr)
    + (hjoin_bloom_worker_end != nullptr);
}

static const int V3_API_COUNT = 6;

/* a Catch2 case registered only when the whole v3 API links (what TEST_CASE expands to, behind a condition) */
#define V3_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = v3_api_linked_count () == V3_API_COUNT \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* registered on every revision: a partial v3 API (a rename) would silently drop B6-B8, so it fails then */
TEST_CASE ("B6-B8 guard: the parallel-path API (hjoin_bloom_wants_serial / _prepare / _worker_begin / _skip_descr / "
	   "_skip_tuple / _end) links all or none - B6-B8 are registered only when all of it does", "[contract]")
{
  int n = v3_api_linked_count ();
  WARN ("v3 parallel-path API: " << n << " of " << V3_API_COUNT << " linked"
	<< (n == V3_API_COUNT ? " - B6-B8 registered" : n == 0 ? " - B6-B8 not applicable on this revision" : ""));
  CHECK ((n == 0 || n == V3_API_COUNT));
}

static unitseam_worker_view
wview (HJOIN_BLOOM_WORKER * w)
{
  unitseam_worker_view v;
  unitseam_worker_view_of (w, &v);
  return v;
}

static bool
is_live (const void *p)
{
  std::lock_guard<std::mutex> g (g_live_mutex);
  return live ().count ((void *) p) == 1;
}

/* the aptr list in the order the inputs run (pt_to_hashjoin_proc: outer -> inner; inner first: inner -> outer) */
static void
aptr_in_run_order (join & j)
{
  XASL_NODE *a = j.first (), *b = j.second ();
  j.hj->aptr_list = a;
  a->next = b;
  b->next = NULL;
}

static void
aptr_list_of (join & j, std::vector<XASL_NODE *> nodes)
{
  j.hj->aptr_list = nodes.empty ()? NULL : nodes[0];
  for (size_t i = 0; i < nodes.size (); i++)
    {
      nodes[i]->next = i + 1 < nodes.size ()? nodes[i + 1] : NULL;
    }
}

/* ================================================================ B6: hjoin_bloom_wants_serial */

V3_TEST_CASE (b6_join_types, "B6: wants_serial = attach's decision on the input the aptr list runs second - INNER "
	      "either order, LEFT inner second, RIGHT outer second; RIGHT inner second (preserved), LEFT outer second, "
	      "FULL -> false; parameter off -> false", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  struct
  {
    JOIN_TYPE jt;
    bool first_is_outer;
    bool expect;
    const char *what;
  } cases[] = {
    {JOIN_INNER, true, true, "INNER, aptr outer -> inner"},
    {JOIN_INNER, false, true, "INNER, aptr inner -> outer"},
    {JOIN_LEFT, true, true, "LEFT, inner second (not preserved)"},
    {JOIN_LEFT, false, false, "LEFT, outer second (preserved)"},
    {JOIN_RIGHT, true, false, "RIGHT, inner second (preserved)"},
    {JOIN_RIGHT, false, true, "RIGHT, outer second (not preserved)"},
    {JOIN_OUTER, true, false, "FULL, inner second"},
    {JOIN_OUTER, false, false, "FULL, outer second"},
  };
  for (auto &c : cases)
    {
      join j = attach_join (c.jt, c.first_is_outer);
      aptr_in_run_order (j);
      INFO (c.what);
      bool ws = hjoin_bloom_wants_serial (j.hj);
      CHECK (ws == c.expect);
      CHECK (attached_after (j) == ws);	/* the two agree once the first input is done */
    }

  join j = attach_join (JOIN_INNER, true);
  aptr_in_run_order (j);
  set_bloom_param (false);
  CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
  set_bloom_param (true);
  CHECK (hjoin_bloom_wants_serial (j.hj));
}

V3_TEST_CASE (b6_second_shape, "B6: wants_serial is false when the second input does not write each row straight to "
	      "its list (GROUP BY, analytic, hash GROUP BY, LIMIT, OFFSET, DISTINCT, CONNECT BY, click counter, "
	      "ORDERBY_NUM, top-N, UPDATE/DELETE select, subquery cache, top-most, not a BUILDLIST) - the same list as "
	      "attach; it does not read the first input's state or shape", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  alignas (64) static char dummy[4096];
  REGU_VARIABLE *regu = (REGU_VARIABLE *) dummy;
  SORT_LIST *sl = (SORT_LIST *) dummy;
  ANALYTIC_EVAL_TYPE *ae = (ANALYTIC_EVAL_TYPE *) dummy;
  XASL_NODE *cb = (XASL_NODE *) dummy;
  SELUPD_LIST *su = (SELUPD_LIST *) dummy;
  PRED_EXPR *pe = (PRED_EXPR *) dummy;
  DB_VALUE *dv = (DB_VALUE *) dummy;
  XASL_ID *xid = (XASL_ID *) dummy;

  std::vector<attach_case> second_shape = {
    {"control: INNER, plain BUILDLIST second", true, [] (join &) {}},
    {"second GROUP BY", false, [&] (join & j) { j.second ()->proc.buildlist.groupby_list = sl; }},
    {"second analytic", false, [&] (join & j) { j.second ()->proc.buildlist.a_eval_list = ae; }},
    {"second hash GROUP BY", false, [] (join & j) { j.second ()->proc.buildlist.g_hash_eligible = 1; }},
    {"second LIMIT", false, [&] (join & j) { j.second ()->limit_row_count = regu; }},
    {"second OFFSET", false, [&] (join & j) { j.second ()->limit_offset = regu; }},
    {"second DISTINCT", false, [] (join & j) { j.second ()->option = Q_DISTINCT; }},
    {"second CONNECT BY", false, [&] (join & j) { j.second ()->connect_by_ptr = cb; }},
    {"second click counter (selected_upd_list)", false, [&] (join & j) { j.second ()->selected_upd_list = su; }},
    {"second ORDERBY_NUM () predicate", false, [&] (join & j) { j.second ()->ordbynum_pred = pe; }},
    {"second ORDERBY_NUM () value", false, [&] (join & j) { j.second ()->ordbynum_val = dv; }},
    {"second top-N sort limit (orderby_limit)", false, [&] (join & j) { j.second ()->orderby_limit = regu; }},
    {"second is the SELECT of an UPDATE/DELETE", false, [] (join & j) { j.second ()->upd_del_class_cnt = 1; }},
    {"second list from the subquery result cache", false, [&] (join & j) { j.second ()->sub_xasl_id = xid; }},
    {"second top-most", false, [] (join & j) { XASL_SET_FLAG (j.second (), XASL_TOP_MOST_XASL); }},
    {"second not a BUILDLIST (BUILDVALUE)", false, [] (join & j) { j.second ()->type = BUILDVALUE_PROC; }},
  };
  for (auto &c : second_shape)
    {
      join j = attach_join (JOIN_INNER, true);
      aptr_in_run_order (j);
      c.setup (j);
      INFO (c.what);
      bool ws = hjoin_bloom_wants_serial (j.hj);
      CHECK (ws == c.expect);
      CHECK (attached_after (j) == ws);
    }

  /* asked before the aptr loop runs anything: the first input's status, rows and shape are not its business */
  std::vector<attach_case> first_side = {
    {"first not run yet (XASL_INITIALIZED, 0 rows)", true,
     [] (join & j) { j.first ()->status = XASL_INITIALIZED; j.first ()->list_id->tuple_cnt = 0; }},
    {"first more than 10,000,000 rows", true, [] (join & j) { j.first ()->list_id->tuple_cnt = 10000001; }},
    {"first GROUP BY", true, [&] (join & j) { j.first ()->proc.buildlist.groupby_list = sl; }},
    {"first LIMIT", true, [&] (join & j) { j.first ()->limit_row_count = regu; }},
    {"first not a BUILDLIST", true, [] (join & j) { j.first ()->type = BUILDVALUE_PROC; }},
  };
  for (auto &c : first_side)
    {
      join j = attach_join (JOIN_INNER, true);
      aptr_in_run_order (j);
      c.setup (j);
      INFO (c.what);
      CHECK (hjoin_bloom_wants_serial (j.hj) == c.expect);
    }
}

V3_TEST_CASE (b6_aptr_order, "B6: the aptr list order picks the second input - reversing it changes the answer, other "
	      "aptr nodes are passed over; an input missing from the aptr list, an input linked to a regu variable "
	      "(XASL_LINK_TO_REGU_VARIABLE), a node that is not a hash join, NULL -> false", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  alignas (64) static char dummy[4096];
  SORT_LIST *sl = (SORT_LIST *) dummy;

  /* the same join, the same inputs: only the aptr order differs */
  {
    join j = attach_join (JOIN_LEFT, true);
    aptr_list_of (j, { j.outer, j.inner });
    CHECK (hjoin_bloom_wants_serial (j.hj));	/* LEFT, inner second: not preserved */
    aptr_list_of (j, { j.inner, j.outer });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));	/* LEFT, outer second: preserved */
  }
  {
    join j = attach_join (JOIN_RIGHT, true);
    aptr_list_of (j, { j.outer, j.inner });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    aptr_list_of (j, { j.inner, j.outer });
    CHECK (hjoin_bloom_wants_serial (j.hj));
  }
  {
    /* the shape that matters is the one of the input run second */
    join j = attach_join (JOIN_INNER, true);
    j.inner->proc.buildlist.groupby_list = sl;
    aptr_list_of (j, { j.outer, j.inner });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    aptr_list_of (j, { j.inner, j.outer });
    CHECK (hjoin_bloom_wants_serial (j.hj));
  }
  {
    /* other subqueries in the aptr list (uncorrelated subqueries of the join's predicates) are passed over */
    join j = attach_join (JOIN_LEFT, true);
    XASL_NODE *x1 = new_input (new_list ({ dom_int () }));
    XASL_NODE *x2 = new_input (new_list ({ dom_int () }));
    XASL_NODE *x3 = new_input (new_list ({ dom_int () }));
    aptr_list_of (j, { x1, j.outer, x2, j.inner, x3 });
    CHECK (hjoin_bloom_wants_serial (j.hj));
    aptr_list_of (j, { x1, j.inner, x2, j.outer, x3 });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    /* an input missing from the aptr list: nothing to serialize */
    aptr_list_of (j, { x1, j.outer, x2 });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    aptr_list_of (j, { x1, x2 });
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    aptr_list_of (j, {});
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
  }
  {
    join j = attach_join (JOIN_INNER, true);
    aptr_in_run_order (j);
    REQUIRE (hjoin_bloom_wants_serial (j.hj));
    XASL_SET_FLAG (j.second (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    XASL_CLEAR_FLAG (j.second (), XASL_LINK_TO_REGU_VARIABLE);
    XASL_SET_FLAG (j.first (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
    XASL_CLEAR_FLAG (j.first (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK (hjoin_bloom_wants_serial (j.hj));
    j.hj->type = MERGELIST_PROC;
    CHECK_FALSE (hjoin_bloom_wants_serial (j.hj));
  }
  CHECK_FALSE (hjoin_bloom_wants_serial (NULL));
}

/* ================================================================ B7: hjoin_bloom_prepare */

V3_TEST_CASE (b7_prepare, "B7: prepare on the owner thread builds the filter and returns it, once - the owner's later "
	      "rows use it without a second build; another thread, a cloned node, a node without a filter, NULL and a "
	      "build that fails -> NULL", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys;
  for (int i = 1; i <= 100; i++)
    {
      keys.push_back (i * 7);
    }
  join j = int_join_with (keys);
  fake_list *first_scan = fake_list_of (j.first ()->list_id);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (j.second ()->hj_bloom != NULL);
  REQUIRE (first_scan->opens == 0);

  /* not the owner: no build, no filter */
  CHECK (hjoin_bloom_prepare (other_thread (), j.second ()) == NULL);
  XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
  memcpy (clone, j.second (), sizeof (XASL_NODE));
  CHECK (hjoin_bloom_prepare (owner_thread (), clone) == NULL);
  CHECK_FALSE (view (j.second ()).built);
  CHECK (first_scan->opens == 0);

  /* the owner: built now, before any row */
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  CHECK (b == j.second ()->hj_bloom);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.built);
  CHECK_FALSE (v.disabled);
  CHECK (v.keys == 100);
  CHECK (v.tested == 0);
  CHECK (first_scan->opens == 1);

  /* again: the same filter, no second scan of the first input */
  CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == b);
  CHECK (first_scan->opens == 1);

  /* the owner's own rows (rows a serial part of the scan writes on the owner thread) use the prepared bits */
  std::vector<DB_VALUE> present = ints ({ 7 * 50 }), absent = ints ({ 3 });
  QFILE_LIST_ID *sl = j.second ()->list_id;
  stage_descr (sl, present);
  CHECK_FALSE (hjoin_bloom_skip_descr (owner_thread (), j.second ()));
  stage_descr (sl, absent);
  CHECK (hjoin_bloom_skip_descr (owner_thread (), j.second ()));
  char *t = make_tuple (sl, absent);
  CHECK (hjoin_bloom_skip_tuple (owner_thread (), j.second (), t));
  free (t);
  unitseam_bloom_view after = view (j.second ());
  CHECK (first_scan->opens == 1);
  CHECK (after.bits == v.bits);
  CHECK (after.keys == 100);
  CHECK (after.tested == 3);
  CHECK (after.skipped == 2);

  /* no filter on the node, or no node */
  CHECK (hjoin_bloom_prepare (owner_thread (), j.first ()) == NULL);
  CHECK (hjoin_bloom_prepare (owner_thread (), NULL) == NULL);
  free (clone);
  hjoin_bloom_release (owner_thread (), j.second ());

  /* a build that cannot complete: NULL, and the owner lets every row through */
  join f = int_join_with ({ 1, 2, 3 });
  fake_list_of (f.first ()->list_id)->fail_open = true;
  hjoin_bloom_attach (owner_thread (), f.hj, f.second ());
  REQUIRE (f.second ()->hj_bloom != NULL);
  CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == NULL);
  CHECK (view (f.second ()).built);
  CHECK (view (f.second ()).disabled);
  CHECK (er_errid () == NO_ERROR);
  CHECK (decide (f, ints ({ 99 })) == -1);
  hjoin_bloom_release (owner_thread (), f.second ());
}

/* ================================================================ B8: parallel scan workers */

static THREAD_ENTRY *
worker_thread (int i)
{
  static THREAD_ENTRY *t[4] = { new_thread (), new_thread (), new_thread (), new_thread () };
  return t[i];
}

/* a parallel scan worker's list: its own QFILE_LIST_ID with the second input's type list (the clone's valptr types) */
static QFILE_LIST_ID *
worker_list (const join & j)
{
  QFILE_TUPLE_VALUE_TYPE_LIST *tl = &j.second ()->list_id->type_list;
  std::vector<TP_DOMAIN *> doms (tl->domp, tl->domp + tl->type_cnt);
  return new_list (doms);
}

/* one row through a worker: the descriptor path (write_initialize's qfile_generate_tuple_into_list) or the tuple path
 * (the big-record / set path: qfile_add_tuple_to_list) */
static bool
worker_skip (HJOIN_BLOOM_WORKER * w, THREAD_ENTRY * t, QFILE_LIST_ID * l, std::vector<DB_VALUE> &row, bool by_descr)
{
  if (by_descr)
    {
      stage_descr (l, row);
      return hjoin_bloom_worker_skip_descr (t, w, l);
    }
  char *tpl = make_tuple (l, row);
  bool s = hjoin_bloom_worker_skip_tuple (t, w, l, tpl);
  free (tpl);
  return s;
}

static bool
owner_skip (const join & j, std::vector<DB_VALUE> &row)
{
  stage_descr (j.second ()->list_id, row);
  return hjoin_bloom_skip_descr (owner_thread (), j.second ());
}

/* 3,000 random INT first-input keys (+ two NULL keys); rows: those keys, 3,000 absent keys and 20 NULLs, shuffled */
struct int_scene
{
  join j;
  HJOIN_BLOOM *b;
  std::vector<int> present;
  std::vector<std::vector<DB_VALUE>> rows;
  std::vector<bool> row_present;
};

static int_scene
int_scene_setup (int nkeys, unsigned seed)
{
  int_scene s;
  std::mt19937 rng (seed);
  std::unordered_set<int> member;
  while ((int) s.present.size () < nkeys)
    {
      int k = (int) rng ();
      if (member.insert (k).second)
	{
	  s.present.push_back (k);
	}
    }
  s.j = int_join_with (s.present);
  add_row (s.j.first ()->list_id, { v_null () });
  add_row (s.j.first ()->list_id, { v_null () });
  hjoin_bloom_attach (owner_thread (), s.j.hj, s.j.second ());
  REQUIRE (s.j.second ()->hj_bloom != NULL);
  s.b = hjoin_bloom_prepare (owner_thread (), s.j.second ());
  REQUIRE (s.b != NULL);
  REQUIRE (view (s.j.second ()).keys == nkeys);

  std::vector<std::pair<std::vector<DB_VALUE>, bool>> rows;
  for (int k : s.present)
    {
      rows.push_back ({ ints ({ k }), true });
    }
  int absent = 0;
  while (absent < nkeys)
    {
      int k = (int) rng ();
      if (!member.count (k))
	{
	  rows.push_back ({ ints ({ k }), false });
	  absent++;
	}
    }
  for (int i = 0; i < 20; i++)
    {
      rows.push_back ({ { v_null () }, false });
    }
  std::shuffle (rows.begin (), rows.end (), rng);
  for (auto &r : rows)
    {
      s.rows.push_back (r.first);
      s.row_present.push_back (r.second);
    }
  return s;
}

V3_TEST_CASE (b8_share, "B8: two workers on one prepared filter share its bits (the same hjoin_bloom, nothing copied) "
	      "and each has a key and a row buffer of its own, apart from each other's and the owner's", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  int_scene s = int_scene_setup (500, 11);
  HJOIN_BLOOM_WORKER *w1 = hjoin_bloom_worker_begin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = hjoin_bloom_worker_begin (worker_thread (1), s.b);
  REQUIRE (w1 != NULL);
  REQUIRE (w2 != NULL);
  CHECK (w1 != w2);
  unitseam_bloom_view bv = view (s.j.second ());
  unitseam_worker_view v1 = wview (w1), v2 = wview (w2);
  REQUIRE (v1.present);
  CHECK (v1.bloom == (void *) s.b);
  CHECK (v2.bloom == (void *) s.b);
  CHECK (v1.key != NULL);
  CHECK (v2.key != NULL);
  CHECK (v1.key != v2.key);
  CHECK (v1.key != bv.key);
  CHECK (v2.key != bv.key);
  CHECK (v1.row_buf == NULL);
  CHECK (v1.tested == 0);
  CHECK_FALSE (v1.disabled);
  CHECK (bv.bits != NULL);

  /* one row each through the descriptor path, and one through the owner: three row buffers */
  QFILE_LIST_ID *l1 = worker_list (s.j), *l2 = worker_list (s.j);
  std::vector<DB_VALUE> r = ints ({ s.present[0] });
  CHECK_FALSE (worker_skip (w1, worker_thread (0), l1, r, true));
  CHECK_FALSE (worker_skip (w2, worker_thread (1), l2, r, true));
  CHECK_FALSE (owner_skip (s.j, r));
  v1 = wview (w1);
  v2 = wview (w2);
  bv = view (s.j.second ());
  CHECK (v1.row_buf != NULL);
  CHECK (v2.row_buf != NULL);
  CHECK (v1.row_buf != v2.row_buf);
  CHECK (v1.row_buf != bv.row_buf);
  CHECK (v2.row_buf != bv.row_buf);
  /* the shared part did not move */
  CHECK (wview (w1).bloom == (void *) s.b);
  CHECK (view (s.j.second ()).bits == bv.bits);

  hjoin_bloom_worker_end (worker_thread (0), w1);
  hjoin_bloom_worker_end (worker_thread (1), w2);
  CHECK (is_live (bv.bits));	/* ending a worker leaves the shared filter alone */
  CHECK (is_live (s.b));
  hjoin_bloom_release (owner_thread (), s.j.second ());
  CHECK_FALSE (is_live (bv.bits));
}

V3_TEST_CASE (b8_same_decision, "B8: a worker decides as the owner does - on the same rows (3,000 first-input keys, "
	      "3,000 absent, NULLs) two workers, through the descriptor and the tuple path, skip exactly the rows the "
	      "owner skips; no first-input key is skipped", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  int_scene s = int_scene_setup (3000, 27659);
  HJOIN_BLOOM_WORKER *w1 = hjoin_bloom_worker_begin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = hjoin_bloom_worker_begin (worker_thread (1), s.b);
  REQUIRE (w1 != NULL);
  REQUIRE (w2 != NULL);
  QFILE_LIST_ID *l1 = worker_list (s.j), *l2 = worker_list (s.j);
  int differ = 0, false_neg = 0, owner_skipped = 0, absent_skipped = 0, absent = 0, null_kept = 0;
  for (size_t i = 0; i < s.rows.size (); i++)
    {
      bool o = owner_skip (s.j, s.rows[i]);
      bool a = worker_skip (w1, worker_thread (0), l1, s.rows[i], i % 2 == 0);
      bool c = worker_skip (w2, worker_thread (1), l2, s.rows[i], i % 2 == 1);
      differ += (a != o) + (c != o);
      false_neg += s.row_present[i] && (o || a || c);
      owner_skipped += o;
      if (!s.row_present[i] && !DB_IS_NULL (&s.rows[i][0]))
	{
	  absent++;
	  absent_skipped += o;
	}
      null_kept += DB_IS_NULL (&s.rows[i][0]) && !(o && a && c);
    }
  WARN ("rows " << s.rows.size () << ", owner skipped " << owner_skipped << ", absent keys skipped " << absent_skipped
	<< "/" << absent << ", worker/owner disagreements " << differ);
  CHECK (differ == 0);
  CHECK (false_neg == 0);
  CHECK (null_kept == 0);
  CHECK (absent_skipped > absent * 9 / 10);
  unitseam_worker_view v1 = wview (w1), v2 = wview (w2);
  CHECK (v1.tested == (long long) s.rows.size ());
  CHECK (v2.tested == (long long) s.rows.size ());
  CHECK (v1.skipped == owner_skipped);
  CHECK (v2.skipped == owner_skipped);
  CHECK_FALSE (v1.disabled);
  CHECK_FALSE (v2.disabled);
  hjoin_bloom_worker_end (worker_thread (0), w1);
  hjoin_bloom_worker_end (worker_thread (1), w2);
  hjoin_bloom_release (owner_thread (), s.j.second ());
}

/* B2's pairs, the second input's rows checked by a worker: the decision equals the owner's and keeps every join match */
struct worker_pair_result
{
  int join_matches = 0;
  int dropped_matches = 0;
  int differ = 0;
  int non_matches = 0;
  int non_matches_skipped = 0;
  bool need_coerce = false;
};

static worker_pair_result
run_pair_workers (pair_spec & ps)
{
  join j = new_join (JOIN_INNER, { ps.first_dom }, { ps.second_dom }, { {0, 0} });
  for (auto &v : ps.first_vals)
    {
      add_row (j.first ()->list_id, { v });
    }
  first_done (j);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  REQUIRE (j.second ()->hj_bloom != NULL);
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  HJOIN_BLOOM_WORKER *w = hjoin_bloom_worker_begin (worker_thread (0), b);
  REQUIRE (w != NULL);
  QFILE_LIST_ID *wl = worker_list (j);

  worker_pair_result r;
  r.need_coerce = j.hj->proc.hashjoin.domain_info.need_coerce_domains;
  QFILE_LIST_ID *fl = j.first ()->list_id, *sl = j.second ()->list_id;
  std::vector<char *> ft;
  for (auto &v : ps.first_vals)
    {
      std::vector<DB_VALUE> one { v };
      ft.push_back (make_tuple (fl, one));
    }
  for (size_t si = 0; si < ps.second_vals.size (); si++)
    {
      std::vector<DB_VALUE> one { ps.second_vals[si] };
      char *st = make_tuple (sl, one);
      QFILE_TUPLE_RECORD srec = bound (sl, st);
      int d = unitseam_bloom_skip_bound (owner_thread (), j.second (), &srec);
      REQUIRE (d != -1);
      bool by_tuple = worker_skip (w, worker_thread (0), wl, one, false);
      bool by_descr = worker_skip (w, worker_thread (0), wl, one, true);
      r.differ += (by_tuple != (d == 1)) + (by_descr != (d == 1));
      bool matched = false;
      for (char *f : ft)
	{
	  QFILE_TUPLE_RECORD frec = bound (fl, f);
	  QFILE_TUPLE_RECORD srec2 = bound (sl, st);
	  bool fnull, snull, keq, ceq;
	  unsigned int fh, sh;
	  REQUIRE (unitseam_hj_join_pair (owner_thread (), j.hj, true, &frec, &srec2, &fnull, &snull, &fh, &sh, &keq, &ceq)
		   == NO_ERROR);
	  if (fh == sh && ceq)
	    {
	      matched = true;
	    }
	}
      if (matched)
	{
	  r.join_matches++;
	  r.dropped_matches += by_tuple || by_descr;
	}
      else
	{
	  r.non_matches++;
	  r.non_matches_skipped += by_tuple;
	}
      free (st);
    }
  for (char *f : ft)
    {
      free (f);
    }
  CHECK_FALSE (wview (w).disabled);
  hjoin_bloom_worker_end (worker_thread (0), w);
  hjoin_bloom_release (owner_thread (), j.second ());
  return r;
}

static void
check_pair_workers (pair_spec & ps, int min_matches)
{
  worker_pair_result r = run_pair_workers (ps);
  INFO (ps.name << ": coerce " << (r.need_coerce ? "yes" : "no") << ", join matches " << r.join_matches
	<< ", dropped by the worker " << r.dropped_matches << ", worker/owner disagreements " << r.differ
	<< ", non-matches skipped " << r.non_matches_skipped << "/" << r.non_matches);
  CHECK (r.need_coerce);
  CHECK (r.join_matches >= min_matches);
  CHECK (r.dropped_matches == 0);
  CHECK (r.differ == 0);
}

V3_TEST_CASE (b8_coercion, "B8: a worker coerces the second input's key as the owner does - BIGINT x INT, NUMERIC(20,4) "
	      "x NUMERIC(10,2), NUMERIC(38,10) x NUMERIC(38,0), DOUBLE x INT, VARCHAR(20) x CHAR(10), utf8_en_ci "
	      "VARCHAR(30) x VARCHAR(10) (first x second): same decision, every join match kept", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<long long> bv { 0, 1, -1, 5, -5, 123456, -123456, 2147483647, -2147483647LL - 1, 7, -7,
    2147483648LL, -2147483649LL, 4294967301LL, -4294967301LL };
  std::vector<long long> iv2 { 0, 1, -1, 5, -5, 7, -7, 123456, -123456, 2147483647, -2147483647 - 1, 42 };
  pair_spec a = spec_ints ("BIGINT first x INT second", dom_bigint (), dom_int (), bv, iv2);
  check_pair_workers (a, 11);

  TP_DOMAIN *n102 = dom_numeric (10, 2), *n204 = dom_numeric (20, 4);
  pair_spec b = spec_text ("NUMERIC(20,4) first x NUMERIC(10,2) second", n204, n102,
			   { "1.5000", "-1.5", "0", "12345678.99", "-0.01", "100.1", "-99999999.99", "1.5001",
			   "123456789012.3456" },
			   { "1.50", "-1.50", "0.00", "12345678.99", "-0.01", "100.10", "-99999999.99", "1.51",
			   "-0.02" });
  check_pair_workers (b, 7);

  TP_DOMAIN *n380 = dom_numeric (38, 0), *n3810 = dom_numeric (38, 10);
  pair_spec c = spec_text ("NUMERIC(38,10) first x NUMERIC(38,0) second", n3810, n380,
			   { "12345.0000000000", "-1", "0", "9999999999999999999999999999.0", "100.0000000001",
			   "12345.5" },
			   { "12345", "-1", "0", "9999999999999999999999999999", "100", "7" });
  check_pair_workers (c, 4);

  std::vector<long long> v { 0, 1, -1, 5, -5, 123456, -123456, 2147483647 };
  std::vector<long long> w { 0, 1, -1, 5, -5, 123456, -123456, 2147483647, 3, -3 };
  pair_spec d = spec_ints ("DOUBLE first x INT second", dom_double (), dom_int (), v, w);
  check_pair_workers (d, 8);

  TP_DOMAIN *c10 = dom_string (DB_TYPE_CHAR, 10, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *v20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_BINARY);
  pair_spec e = spec_text ("VARCHAR(20) first x CHAR(10) second", v20, c10,
			   { "ab", "ab ", "ab        ", "a", "abc   ", "", "x y", "x y       ", " lead", " lead     " },
			   { "ab", "a", "abc", "", "x y", " lead", "AB", "b" });
  check_pair_workers (e, 3);

  TP_DOMAIN *ci10 = dom_string (DB_TYPE_VARCHAR, 10, LANG_COLL_UTF8_EN_CI);
  TP_DOMAIN *ci30 = dom_string (DB_TYPE_VARCHAR, 30, LANG_COLL_UTF8_EN_CI);
  pair_spec f = spec_text ("VARCHAR(30) utf8_en_ci first x VARCHAR(10) utf8_en_ci second", ci30, ci10,
			   { "A", "abc", "Hello", "MiXeD cAsE", "z" },
			   { "a", "A", "ABC", "aBc", "hello", "HELLO", "Hello ", "mixed case", "Z", "b", "abcd" });
  check_pair_workers (f, 8);
}

V3_TEST_CASE (b8_end_sums, "B8: worker_end adds each worker's tested / skipped to the shared filter (the owner's own "
	      "rows included) and frees the worker's key, row buffer and state; detach then reports the total",
	      "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  int_scene s = int_scene_setup (1000, 5);
  HJOIN_BLOOM_WORKER *w1 = hjoin_bloom_worker_begin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = hjoin_bloom_worker_begin (worker_thread (1), s.b);
  REQUIRE (w1 != NULL);
  REQUIRE (w2 != NULL);
  QFILE_LIST_ID *l1 = worker_list (s.j), *l2 = worker_list (s.j);

  /* the rows split three ways: worker 1 the first third, worker 2 the second, the owner the rest */
  size_t n = s.rows.size (), third = n / 3;
  long long t1 = 0, s1 = 0, t2 = 0, s2 = 0, t0 = 0, s0 = 0;
  for (size_t i = 0; i < n; i++)
    {
      if (i < third)
	{
	  t1++;
	  s1 += worker_skip (w1, worker_thread (0), l1, s.rows[i], i % 2 == 0);
	}
      else if (i < 2 * third)
	{
	  t2++;
	  s2 += worker_skip (w2, worker_thread (1), l2, s.rows[i], i % 2 == 0);
	}
      else
	{
	  t0++;
	  s0 += owner_skip (s.j, s.rows[i]);
	}
    }
  REQUIRE (s1 > 0);
  REQUIRE (s2 > 0);
  /* until a worker ends, its counts are its own */
  unitseam_bloom_view v = view (s.j.second ());
  CHECK (v.tested == t0);
  CHECK (v.skipped == s0);
  unitseam_worker_view v1 = wview (w1), v2 = wview (w2);
  CHECK (v1.tested == t1);
  CHECK (v1.skipped == s1);
  CHECK (v2.tested == t2);
  CHECK (v2.skipped == s2);

  hjoin_bloom_worker_end (worker_thread (0), w1);
  v = view (s.j.second ());
  CHECK (v.tested == t0 + t1);
  CHECK (v.skipped == s0 + s1);
  CHECK_FALSE (is_live (w1));
  CHECK_FALSE (is_live (v1.key));
  CHECK_FALSE (is_live (v1.row_buf));
  CHECK (is_live (v2.key));

  hjoin_bloom_worker_end (worker_thread (1), w2);
  v = view (s.j.second ());
  CHECK (v.tested == t0 + t1 + t2);
  CHECK (v.skipped == s0 + s1 + s2);
  CHECK_FALSE (is_live (w2));
  CHECK_FALSE (is_live (v2.key));
  CHECK_FALSE (is_live (v2.row_buf));

  hjoin_bloom_detach (owner_thread (), s.j.second ());
  HASHJOIN_BLOOM_STATS *g = &s.j.hj->proc.hashjoin.bloom_stats;
  CHECK (g->used);
  CHECK (g->keys == 1000);
  CHECK (g->tested == (long long) n);
  CHECK (g->tested == t0 + t1 + t2);
  CHECK (g->skipped == s0 + s1 + s2);
  CHECK (s.j.second ()->hj_bloom == NULL);
}

V3_TEST_CASE (b8_failure, "B8: a worker whose key fetch fails lets its rows through from then on, alone - the other "
	      "worker and the owner keep skipping; the error is cleared; a worker that cannot grow its row buffer does "
	      "the same; a begin that cannot allocate returns NULL and leaks nothing", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  /* first VARCHAR(20), second VARCHAR(5): the second's key is coerced to the common VARCHAR(20), which makes one
   * allocation per row (measured: the only one of a tuple-path check) - the fault knob fails it inside hjoin_fetch_key.
   * (INT x VARCHAR would not do: tp_value_coerce refuses INT -> VARCHAR, so every row fails, the owner's too.) */
  TP_DOMAIN *v20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *v5 = dom_string (DB_TYPE_VARCHAR, 5, LANG_COLL_UTF8_BINARY);
  join j = new_join (JOIN_INNER, { v20 }, { v5 }, { {0, 0} });
  for (int i = 0; i < 200; i++)
    {
      add_row (j.first ()->list_id, { cast_text (std::to_string (i).c_str (), v20) });
    }
  first_done (j);
  hjoin_bloom_attach (owner_thread (), j.hj, j.second ());
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  REQUIRE (j.hj->proc.hashjoin.domain_info.need_coerce_domains);
  HJOIN_BLOOM_WORKER *w1 = hjoin_bloom_worker_begin (worker_thread (0), b);
  HJOIN_BLOOM_WORKER *w2 = hjoin_bloom_worker_begin (worker_thread (1), b);
  REQUIRE (w1 != NULL);
  REQUIRE (w2 != NULL);
  QFILE_LIST_ID *l1 = worker_list (j), *l2 = worker_list (j);

  /* first-input keys '0'..'199'; absent keys 'x0', 'x1', ... */
  std::vector<DB_VALUE> present { cast_text ("42", v5) };
  auto absent = [&] (int i) { return std::vector<DB_VALUE> { cast_text (("x" + std::to_string (i)).c_str (), v5) }; };
  {
    std::vector<DB_VALUE> a0 = absent (0);
    REQUIRE_FALSE (worker_skip (w1, worker_thread (0), l1, present, false));
    REQUIRE (worker_skip (w1, worker_thread (0), l1, a0, false));
    REQUIRE (worker_skip (w2, worker_thread (1), l2, a0, false));
    REQUIRE (owner_skip (j, a0));
  }
  long long w1_tested = wview (w1).tested;

  /* worker 1's next fetch fails (the row is built first: the knob counts every allocation) */
  std::vector<DB_VALUE> a1 = absent (1);
  fail_nth_alloc (1);
  bool s = worker_skip (w1, worker_thread (0), l1, a1, false);
  int left = g_alloc_countdown.load ();
  fail_nth_alloc (0);
  CHECK (left == 0);		/* the failure was spent inside the check */
  CHECK_FALSE (s);		/* an absent key let through: the worker no longer filters */
  CHECK (wview (w1).disabled);
  CHECK (wview (w1).tested == w1_tested);	/* the failed row is not counted */
  CHECK (er_errid () == NO_ERROR);
  CHECK_FALSE (view (j.second ()).disabled);	/* the shared filter is untouched */

  int w1_skipped = 0, w2_skipped = 0, owner_skipped = 0, skipped_key = -1;
  for (int i = 2; i < 52; i++)
    {
      std::vector<DB_VALUE> a = absent (i);
      w1_skipped += worker_skip (w1, worker_thread (0), l1, a, i % 2 == 0);
      bool s2 = worker_skip (w2, worker_thread (1), l2, a, i % 2 == 0);
      w2_skipped += s2;
      owner_skipped += owner_skip (j, a);
      if (s2 && skipped_key < 0)
	{
	  skipped_key = i;
	}
    }
  CHECK (w1_skipped == 0);
  CHECK (w2_skipped > 40);
  CHECK (owner_skipped == w2_skipped);
  CHECK (wview (w1).tested == w1_tested);
  CHECK_FALSE (wview (w2).disabled);

  /* worker 2's row buffer cannot grow (its first row on the descriptor path): the same, for worker 2 only */
  HJOIN_BLOOM_WORKER *w3 = hjoin_bloom_worker_begin (worker_thread (2), b);
  REQUIRE (w3 != NULL);
  QFILE_LIST_ID *l3 = worker_list (j);
  REQUIRE (skipped_key >= 0);
  std::vector<DB_VALUE> a9 = absent (skipped_key);	/* a key the filter skips (not a false positive) */
  fail_nth_alloc (1);
  bool s3 = worker_skip (w3, worker_thread (2), l3, a9, true);
  left = g_alloc_countdown.load ();
  fail_nth_alloc (0);
  CHECK (left == 0);
  CHECK_FALSE (s3);
  CHECK (wview (w3).disabled);
  CHECK (wview (w3).row_buf == NULL);
  CHECK (er_errid () == NO_ERROR);
  CHECK (worker_skip (w2, worker_thread (1), l2, a9, true));

  long long before = view (j.second ()).tested;
  long long add = wview (w1).tested + wview (w2).tested + wview (w3).tested;
  hjoin_bloom_worker_end (worker_thread (0), w1);
  hjoin_bloom_worker_end (worker_thread (1), w2);
  hjoin_bloom_worker_end (worker_thread (2), w3);
  CHECK (view (j.second ()).tested == before + add);

  /* begin without memory: NULL at every allocation it makes, nothing left behind, the error cleared */
  int failing = 0;
  for (int n = 1; n <= 16; n++)
    {
      size_t before_live = live_count ();
      fail_nth_alloc (n);
      HJOIN_BLOOM_WORKER *w = hjoin_bloom_worker_begin (worker_thread (3), b);
      bool spent = g_alloc_countdown.load () == 0;
      fail_nth_alloc (0);
      if (w != NULL)
	{
	  CHECK_FALSE (spent);	/* it made fewer than n allocations */
	  hjoin_bloom_worker_end (worker_thread (3), w);
	  break;
	}
      INFO ("allocation " << n << " of begin fails");
      CHECK (spent);
      CHECK (live_count () == before_live);
      CHECK (er_errid () == NO_ERROR);
      failing++;
    }
  WARN ("worker_begin makes " << failing << " allocations (1 key column)");
  CHECK (failing >= 2);
  hjoin_bloom_release (owner_thread (), j.second ());
}

V3_TEST_CASE (b8_null, "B8: no filter or a worker that could not begin - begin (NULL) returns NULL, a NULL worker's "
	      "skip_descr / skip_tuple return false (the row goes through), end (NULL) does nothing; skip_tuple of a NULL "
	      "tuple returns false", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  CHECK (hjoin_bloom_worker_begin (worker_thread (0), NULL) == NULL);
  int_scene s = int_scene_setup (100, 3);
  QFILE_LIST_ID *l = worker_list (s.j);
  std::vector<DB_VALUE> a = ints ({ 0x7fff0000 });
  stage_descr (l, a);
  CHECK_FALSE (hjoin_bloom_worker_skip_descr (worker_thread (0), NULL, l));
  char *t = make_tuple (l, a);
  CHECK_FALSE (hjoin_bloom_worker_skip_tuple (worker_thread (0), NULL, l, t));
  hjoin_bloom_worker_end (worker_thread (0), NULL);
  HJOIN_BLOOM_WORKER *w = hjoin_bloom_worker_begin (worker_thread (0), s.b);
  REQUIRE (w != NULL);
  CHECK_FALSE (hjoin_bloom_worker_skip_tuple (worker_thread (0), w, l, NULL));
  CHECK (wview (w).tested == 0);
  hjoin_bloom_worker_end (worker_thread (0), w);
  CHECK (view (s.j.second ()).tested == 0);
  free (t);
  hjoin_bloom_release (owner_thread (), s.j.second ());
}

/* a spin barrier for the two worker threads and the test thread */
struct spin_barrier
{
  std::atomic<int> arrived { 0 };
  std::atomic<int> generation { 0 };
  int parties;
  explicit spin_barrier (int n):parties (n)
  {
  }
  void wait ()
  {
    int gen = generation.load ();
    if (arrived.fetch_add (1) + 1 == parties)
      {
	arrived = 0;
	generation++;
      }
    else
      {
	while (generation.load () == gen)
	  {
	    std::this_thread::yield ();
	  }
      }
  }
};

V3_TEST_CASE (b8_threads, "B8: two workers on two threads read the same bits at once and end at once - each skips "
	      "what the owner would, no first-input key is skipped, and the shared tested / skipped are the exact sums "
	      "(200 rounds)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  int_scene s = int_scene_setup (5000, 99);
  size_t n = s.rows.size ();

  /* the owner's decision on every row, first (counted in the filter: subtracted below) */
  std::vector<char> ref (n);
  for (size_t i = 0; i < n; i++)
    {
      ref[i] = owner_skip (s.j, s.rows[i]);
    }
  long long ref_skipped = 0;
  for (char c : ref)
    {
      ref_skipped += c;
    }
  QFILE_LIST_ID *lists[2] = { worker_list (s.j), worker_list (s.j) };
  /* each worker's rows serialized in advance (make_tuple REQUIREs - Catch2 assertions stay on the test thread) */
  std::vector<std::vector<char *>> tuples (2);
  for (int k = 0; k < 2; k++)
    {
      for (size_t i = 0; i < n; i++)
	{
	  tuples[k].push_back (make_tuple (lists[k], s.rows[i]));
	}
    }

  const int rounds = 200;
  int bad_sum = 0, differ = 0, false_neg = 0, begin_failed = 0;
  for (int round = 0; round < rounds; round++)
    {
      unitseam_bloom_view before = view (s.j.second ());
      spin_barrier start (3), end (3);
      long long tested[2] = { 0, 0 }, skipped[2] = { 0, 0 };
      int d[2] = { 0, 0 }, fn[2] = { 0, 0 };
      bool began[2] = { false, false };
      auto body = [&] (int k)
      {
	THREAD_ENTRY *t = worker_thread (k);
	HJOIN_BLOOM_WORKER *w = hjoin_bloom_worker_begin (t, s.b);
	began[k] = w != NULL;
	start.wait ();
	/* worker k takes rows k, k + 2, ... and, from round to round, the other half too: both read every bit */
	for (size_t i = (round % 2 == 0 ? k : 0); i < n; i += (round % 2 == 0 ? 2 : 1))
	  {
	    bool sk = hjoin_bloom_worker_skip_tuple (t, w, lists[k], tuples[k][i]);
	    tested[k]++;
	    skipped[k] += sk;
	    d[k] += sk != (bool) ref[i];
	    fn[k] += sk && s.row_present[i];
	  }
	end.wait ();
	hjoin_bloom_worker_end (t, w);
      };
      std::thread th0 (body, 0), th1 (body, 1);
      start.wait ();
      end.wait ();
      th0.join ();
      th1.join ();
      begin_failed += !began[0] + !began[1];
      differ += d[0] + d[1];
      false_neg += fn[0] + fn[1];
      unitseam_bloom_view after = view (s.j.second ());
      bad_sum += after.tested != before.tested + tested[0] + tested[1];
      bad_sum += after.skipped != before.skipped + skipped[0] + skipped[1];
    }
  unitseam_bloom_view last = view (s.j.second ());
  WARN ("rows " << n << " x " << rounds << " rounds x 2 threads: filter tested " << last.tested << ", skipped "
	<< last.skipped << " (owner reference " << ref_skipped << " per pass)");
  CHECK (begin_failed == 0);
  CHECK (differ == 0);
  CHECK (false_neg == 0);
  CHECK (bad_sum == 0);
  /* rounds alternate halves (n) and full passes (2n) for the two workers together */
  long long expect_tested = (long long) n + (long long) (rounds / 2) * (long long) n
    + (long long) (rounds - rounds / 2) * 2 * (long long) n;
  long long expect_skipped = ref_skipped + (long long) (rounds / 2) * ref_skipped
    + (long long) (rounds - rounds / 2) * 2 * ref_skipped;
  CHECK (last.tested == expect_tested);
  CHECK (last.skipped == expect_skipped);
  for (auto &v : tuples)
    {
      for (char *t : v)
	{
	  free (t);
	}
    }
  hjoin_bloom_release (owner_thread (), s.j.second ());
}
