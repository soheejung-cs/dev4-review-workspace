/*
 * CBRD-27659 (prototype) -- a Bloom filter of the first-materialized hash join input's keys, checked before the second
 * input writes a row to its list (src/query/query_hash_join.c hjoin_bloom_*, server mode). New code: the base has no
 * hjoin_bloom_*, so the suite runs on head only (requires: hjoin_bloom_attach). Written for v1 5a8c0b7d8 (now branch
 * CBRD-27659-v1), moved to the v2 layout (e55956083 + fdc3fcd32, now branch CBRD-27659, first named CBRD-27659-v2):
 * the trace counters live in HASHJOIN_PROC_NODE.bloom_stats, not in the stats_group. v3 (1a7bccdcc) adds B6-B8 for the
 * parallel paths; they are registered only where that API links (weak references). v4 (e8f629fcc) makes the filter a
 * state machine - set up at the second input's first row, filled (one pass over the first input) only once the second
 * input has given threshold = max (1,024, first rows / 2) rows, turned off when its first 4,096 checked rows drop
 * under 50%, filled by the owner while it polls for a parallel scan's workers (hjoin_bloom_poll) - and adds B9-B12,
 * registered only where hjoin_bloom_poll links. hooks.cpp reads both struct layouts (HJOIN_BLOOM_SEEN_BATCH tells them
 * apart) and maps the earlier one onto the staged one (built = set up; threshold 1: built at the first row), so one
 * suite runs on e55956083 / fdc3fcd32 (B1-B5, F1-F4), 1a7bccdcc (+ B6-B8) and e8f629fcc on (all). Where the two
 * protocols differ, a case asks C ().staged and expects each its own.
 * v5: 8ad5ee2df attaches before the inputs run (attach (hj): both inputs carry the filter) and lets them run at once -
 * the first input's end publishes its row count (hjoin_bloom_input_done), the owner is bound at first use and fills
 * (set-up inside the fill) once that is published and threshold rows came, finish reports and frees; 3726fd95d checks a
 * row before its projection from its key columns' regu variables (hjoin_bloom_skip_row, key_regus) and fills in an
 * error stack frame. The public API of every revision is declared weak at the top and called through wrappers
 * (attach_second, detach_second, wbegin, serial_answer) that pick the form that links - B1-B12 keep their meaning on
 * every revision (C ().concurrent: the earlier attach's 'first input done' is the first input's input_done, its
 * owner binding a prepare by the attaching thread, detach is input_done (second) + finish, B6's wants_serial is attach
 * (hj)'s own decision); B13-B16 are registered only where the 8ad5ee2df / 3726fd95d API links.
 *
 *   B1  no false negative: 100,000 random INT first-input keys; each of them as a second-input row is kept by the
 *       filled filter (+ a two-column key INT x VARCHAR)
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
 *       cloned node let every row through; release on a clone frees nothing; a failed fill lets every row through
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
 *   B7  (1a7bccdcc) hjoin_bloom_prepare: the owner thread sets up once and gets the filter (staged: no pass over the
 *       first input until the threshold; earlier: built at once); another thread, a clone, no filter, NULL, a failed
 *       set-up or a filter turned off -> NULL
 *   B8  (1a7bccdcc) hjoin_bloom_worker_*: two workers share the filter's bits with a key and a row buffer each; a
 *       worker decides as the owner does (INT + NULL rows, both entry paths, B2's coercion pairs with the second
 *       coerced); worker_end adds the counts (owner's included) and frees the worker's own; a fetch failure (or a row
 *       buffer that cannot grow) turns off that worker alone; a begin without memory returns NULL and leaks nothing;
 *       NULL bloom / worker / tuple; two workers on two std::threads read the bits and end at the same time - the
 *       counts are exact sums (200 rounds). The filter is filled before the workers begin (what hjoin_bloom_poll does
 *       once they have given threshold rows) and the rows drop about 2/3, far from the 50% sample bound.
 *   B9  (e8f629fcc) the owner's rows: the threshold formula; rows before it pass unchecked and are counted, the first
 *       sets up without reading the first input; the threshold-th row fills and is checked, no false negative after;
 *       the 4,096-row sample: 2,047 dropped -> OFF_LOW_SKIP, 2,048 -> stays, judged once
 *   B10 (e8f629fcc) hjoin_bloom_poll: fills only on the owner thread, set up, waiting, seen >= threshold; nothing
 *       otherwise (another thread, before set-up, under the threshold, NULL, ready, off - a failed fill not retried)
 *   B11 (e8f629fcc) workers: unchecked until a batch boundary after the fill (256 rows), seen handed over per batch and
 *       at end, a low sample of one worker turns the filter off for all (the owner at once, workers at their next
 *       batch); two worker threads + a poll thread: seen exact, one fill, no row dropped by unfilled bits (200 rounds)
 *   B12 (e8f629fcc) detach's report per outcome (NOT_BUILT, CHECKED, OFF_LOW_SKIP, OFF_FAILED; seen incl. workers);
 *       set-up and fill failures -> OFF_FAILED with every row through, error cleared, no re-read; the first off
 *       reason is kept (8ad5ee2df on: the set-up failures at the threshold row's fill, every allocation of the fill)
 *   B13 (3726fd95d) (K) the key before the projection = the list tuple's key: per row, hjoin_bloom_fetch_key_values's
 *       key hash against the real projection (qdata_generate_tuple_desc_for_valptr_list + qfile_tuple_fill_from_values)
 *       read back by hjoin_fetch_key - INT/INT, INT/BIGINT, NUMERIC(10,2)/(20,4) both ways, NUMERIC of other domains,
 *       CHAR(10)/VARCHAR, VARCHAR/CHAR(10), utf8_en_ci, DATE/DATE, DATE/DATETIME, DOUBLE/INT, INT/DOUBLE, NULL; 10,000
 *       rows each, values of another domain through key_cast; the decision of the bits and skip_row the same
 *   F5  [defect] (latent) a string of the column's type in another codeset is transcoded before the hash, the list
 *       keeps its bytes: the keys differ (reachability of such a value under a plain column reference not shown)
 *   B14 (3726fd95d) (L) attach's key_regus (list positions past hidden columns; plain kinds only, else none); skip_row
 *       only with them, the late entries stand aside then (no double count); a worker's own key regu from its clone list
 *   B15 (8ad5ee2df) (M) the first input late: rows only counted until its end is published; input_done publishes,
 *       FIRST_LATE, OFF_FIRST_ROWS, NOT_BUILT, ENDED_FIRST (3726fd95d), CHECKED from finish; what each thread frees;
 *       owner bound at first use; the end published from another thread while the owner gives rows (200 rounds)
 *   B16 (3726fd95d) (N) the fill keeps an error the query had set (error stack frame), fails or not
 *
 * Real code at the revision under test, linked as --extra TUs: query_hash_scan.c (keys, hash, key_eq), memory_hash.c
 * (mht_get_hash_number, the collations' hash), qfile_tuple_layout.c (layout, slot walker), and the type system
 * (system_parameter.c, language_support.c, intl_support.c, chartype.c, mprec.c, dtoa.c, object_primitive.c,
 * object_domain.c, object_domain_convert.cpp, numeric_opfunc.c, db_date.c, db_macro.c, string_opfunc.c). Tuples are
 * written by the real assembler (qfile_tuple_size_from_values / qfile_tuple_fill_from_values, the T_NORMAL path of
 * qfile_generate_tuple_into_list). Faked: the list file page iteration (qfile_open_list_scan / qfile_scan_list_next /
 * qfile_close_scan - list_file.c reads pages through the query manager's temp files), the error manager (per thread),
 * the private allocator (locked; a fault knob fails the n-th allocation as the real malloc branch does - B8, B12) and
 * the area allocator, boot-time globals.
 *
 * v5 adds to the real code: fetch.c (fetch_peek_dbval_slow), query_opfunc.c (the projection:
 * qdata_generate_tuple_desc_for_valptr_list, qdata_get_valptr_type_list), domain_resolve.c (qexec_consumer_domain) and
 * storage_common.c (db_User_page_size: the projection's big-record bound); faked in addition: er_stack_push / pop.
 *
 * The suite names TU-internal symbols only in hooks.cpp (struct hjoin_bloom and hjoin_bloom_worker fields, the static
 * hjoin_bloom_* functions, the TU's state enum and HJOIN_BLOOM_* constants); test.cpp uses the public hjoin_bloom_* API
 * and the hooks' views. A rewrite that renames them needs only hooks.cpp adjusted.
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
#include "query_executor.h"
#include "query_opfunc.h"
#include "regu_var.hpp"

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

struct unitseam_bloom_consts
{
  bool staged;			/* the filter waits for a threshold (e8f629fcc on); false: built at the first row */
  int waiting;
  int ready;
  int off;
  int out_not_built;		/* enum hashjoin_bloom_outcome (earlier: stand-ins, never equal to a staged value) */
  int out_checked;
  int out_off_low_skip;
  int out_off_failed;
  long long min_second_rows;	/* HJOIN_BLOOM_MIN_SECOND_ROWS (earlier 1) */
  long long first_per_second;	/* HJOIN_BLOOM_FIRST_PER_SECOND (earlier 0) */
  long long sample_rows;	/* HJOIN_BLOOM_SAMPLE_ROWS (earlier 0) */
  long long min_skip_percent;	/* HJOIN_BLOOM_MIN_SKIP_PERCENT (earlier 0) */
  long long seen_batch;		/* HJOIN_BLOOM_SEEN_BATCH (earlier 0) */
  /* v5 */
  bool concurrent;		/* 8ad5ee2df on: attach (hj) before the inputs run, input_done, finish (struct: first_done) */
  bool early;			/* 3726fd95d on: the row checked before its projection (struct: key_regus) */
  int out_first_late;		/* HASHJOIN_BLOOM_FIRST_LATE (absent: a stand-in) */
  int out_off_first_rows;	/* HASHJOIN_BLOOM_OFF_FIRST_ROWS */
  int out_ended_first;		/* HASHJOIN_BLOOM_ENDED_FIRST (3726fd95d) */
  long long max_first_rows;	/* HJOIN_BLOOM_MAX_FIRST_ROWS */
};

struct unitseam_bloom_view
{
  bool present;
  bool staged;
  bool set_up;			/* staged: set_up; earlier: built */
  int state;			/* waiting / ready / off (earlier: !built / built && !disabled / built && disabled) */
  long long off_reason;		/* when off: HASHJOIN_BLOOM_OFF_* (earlier: out_off_failed); else -1 */
  long long first_rows;		/* staged; -1 earlier */
  long long threshold;		/* staged; 1 earlier */
  long long seen;		/* staged; -1 earlier */
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
  /* v5 */
  const void *first_xasl;	/* concurrent; NULL earlier */
  long long first_done;		/* concurrent: 0 / 1; -1 earlier */
  const void *key_regus;	/* early: the key columns' regu variables (NULL: a computed key, checked after) */
  const void *key_cast;		/* early */
};

struct unitseam_stats_view
{
  bool staged;
  bool shown;			/* the BLOOM trace line prints: staged attached, earlier used */
  int outcome;			/* staged: state; earlier: out_checked when used, else out_not_built */
  long long keys;
  long long threshold;		/* -1 earlier */
  long long seen;		/* -1 earlier */
  long long tested;
  long long skipped;
};

int unitseam_hj_init_domain_info (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl);
void unitseam_bloom_consts_of (unitseam_bloom_consts * c);
void unitseam_bloom_view_of (XASL_NODE * xasl, unitseam_bloom_view * v);
void unitseam_bloom_fill_now (THREAD_ENTRY * thread_p, XASL_NODE * xasl);
void unitseam_bloom_set_seen (XASL_NODE * xasl, long long seen);
int unitseam_bloom_skip_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record);
int unitseam_bloom_check_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record);
bool unitseam_bloom_may_contain (XASL_NODE * xasl, unsigned int hash);
void unitseam_bloom_stats_of (XASL_NODE * hj_xasl, unitseam_stats_view * s);
void unitseam_bloom_stats_stale (XASL_NODE * hj_xasl);
int unitseam_hj_join_pair (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, bool first_is_outer,
			   QFILE_TUPLE_RECORD * first_rec, QFILE_TUPLE_RECORD * second_rec, bool *first_null,
			   bool *second_null, unsigned int *first_hash, unsigned int *second_hash, bool *key_eq,
			   bool *compare_eq);
int unitseam_hj_init_manager (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl);
/* v5 */
const void *unitseam_bloom_key_regu (XASL_NODE * xasl, int k);
int unitseam_bloom_early_key (THREAD_ENTRY * thread_p, XASL_NODE * xasl, VAL_DESCR * vd, unsigned int *hash,
			      int *cast_mask, int *decision);
int unitseam_hj_second_key (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, bool first_is_outer,
			    QFILE_TUPLE_RECORD * record, unsigned int *hash);

/* v3 (1a7bccdcc): struct hjoin_bloom_worker read through hooks.cpp; present = false on a revision without it.
 * v4: ready (the state as of the worker's last batch) and seen_unflushed; earlier ready = true, seen_unflushed = -1. */
struct unitseam_worker_view
{
  bool present;
  const void *bloom;
  const void *key;
  const void *row_buf;
  int row_buf_size;
  bool disabled;
  bool ready;
  long long seen_unflushed;
  long long tested;
  long long skipped;
  /* v5 (3726fd95d): the first key column's regu variable found in the worker's clone output list; NULL earlier */
  const void *key_regu0;
  const void *key_cast;
};
void unitseam_worker_view_of (const void *worker, unitseam_worker_view * v);

/* ---------------------------------------------------------------- the public API across revisions */

/*
 * Every revision's form, declared weak: only what the revision under test defines links, the rest is a null address.
 * Calls go through the wrappers below (attach_second, detach_second, detach_node, wbegin, serial_answer), which pick
 * the form that links; a case of a protocol a revision does not have is not registered (V3/V4/V5 test cases).
 *   up to e8f629fcc  attach (hj, second) once the first input is done; detach (second); wants_serial (1a7bccdcc on)
 *   8ad5ee2df on     attach (hj) before the inputs run; input_done (input, success); finish (hj)
 *   3726fd95d on     skip_row (xasl, vd); worker_begin (bloom, outptr_list); worker_skip_row (worker, vd)
 * The typedefs repeat query_hash_join.h (a repeated typedef of the same type is allowed).
 */
typedef struct hjoin_bloom HJOIN_BLOOM;
typedef struct hjoin_bloom_worker HJOIN_BLOOM_WORKER;
void hjoin_bloom_attach (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, XASL_NODE * second_xasl) __attribute__ ((weak));
void hjoin_bloom_attach (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl) __attribute__ ((weak));
void hjoin_bloom_detach (THREAD_ENTRY * thread_p, XASL_NODE * xasl) __attribute__ ((weak));
void hjoin_bloom_input_done (THREAD_ENTRY * thread_p, XASL_NODE * xasl, bool success) __attribute__ ((weak));
void hjoin_bloom_finish (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl) __attribute__ ((weak));
bool hjoin_bloom_skip_row (THREAD_ENTRY * thread_p, XASL_NODE * xasl, VAL_DESCR * vd) __attribute__ ((weak));
bool hjoin_bloom_wants_serial (XASL_NODE * hj_xasl) __attribute__ ((weak));
HJOIN_BLOOM *hjoin_bloom_prepare (THREAD_ENTRY * thread_p, XASL_NODE * xasl) __attribute__ ((weak));
HJOIN_BLOOM_WORKER *hjoin_bloom_worker_begin (THREAD_ENTRY * thread_p, HJOIN_BLOOM * bloom) __attribute__ ((weak));
HJOIN_BLOOM_WORKER *hjoin_bloom_worker_begin (THREAD_ENTRY * thread_p, HJOIN_BLOOM * bloom, OUTPTR_LIST * outptr_list)
  __attribute__ ((weak));
bool hjoin_bloom_worker_skip_row (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker, VAL_DESCR * vd)
  __attribute__ ((weak));
bool hjoin_bloom_worker_skip_descr (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker, QFILE_LIST_ID * list_id)
  __attribute__ ((weak));
bool hjoin_bloom_worker_skip_tuple (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker, QFILE_LIST_ID * list_id,
				    char *tuple) __attribute__ ((weak));
void hjoin_bloom_worker_end (THREAD_ENTRY * thread_p, HJOIN_BLOOM_WORKER * worker) __attribute__ ((weak));
void hjoin_bloom_poll (THREAD_ENTRY * thread_p, HJOIN_BLOOM * bloom) __attribute__ ((weak));

struct bloom_api
{
  bool attach3, attach2, detach, input_done, finish, skip_row, wants_serial, prepare, wbegin2, wbegin3, wskip_row,
    wskip_descr, wskip_tuple, wend, poll;
};

static const bloom_api &
API ()
{
  static bloom_api a;
  static bool done = false;
  if (!done)
    {
      a.attach3 = static_cast < void (*)(THREAD_ENTRY *, XASL_NODE *, XASL_NODE *) > (&hjoin_bloom_attach) != nullptr;
      a.attach2 = static_cast < void (*)(THREAD_ENTRY *, XASL_NODE *) > (&hjoin_bloom_attach) != nullptr;
      a.detach = &hjoin_bloom_detach != nullptr;
      a.input_done = &hjoin_bloom_input_done != nullptr;
      a.finish = &hjoin_bloom_finish != nullptr;
      a.skip_row = &hjoin_bloom_skip_row != nullptr;
      a.wants_serial = &hjoin_bloom_wants_serial != nullptr;
      a.prepare = &hjoin_bloom_prepare != nullptr;
      a.wbegin2 = static_cast < HJOIN_BLOOM_WORKER * (*)(THREAD_ENTRY *, HJOIN_BLOOM *) > (&hjoin_bloom_worker_begin)
	!= nullptr;
      a.wbegin3 =
	static_cast < HJOIN_BLOOM_WORKER * (*)(THREAD_ENTRY *, HJOIN_BLOOM *, OUTPTR_LIST *) > (&hjoin_bloom_worker_begin)
	!= nullptr;
      a.wskip_row = &hjoin_bloom_worker_skip_row != nullptr;
      a.wskip_descr = &hjoin_bloom_worker_skip_descr != nullptr;
      a.wskip_tuple = &hjoin_bloom_worker_skip_tuple != nullptr;
      a.wend = &hjoin_bloom_worker_end != nullptr;
      a.poll = &hjoin_bloom_poll != nullptr;
      done = true;
    }
  return a;
}

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

/* query_executor.c qexec_execute_mainblock, only for the scalar subquery a TYPE_CONSTANT regu links (B14's observation:
 * fetch.c's EXECUTE_REGU_VARIABLE_XASL runs it while its status is initial): as qexec_execute_mainblock_internal
 * leaves it - success: the single tuple's value set, status XASL_SUCCESS; failure (a subquery giving more than one
 * row): ER_QPROC_INVALID_QRY_SINGLE_TUPLE set and returned, status XASL_FAILURE */
static int fake_subquery_fails = 0;
static int fake_subquery_runs = 0;
static int fake_subquery_value = 0;
int
qexec_execute_mainblock (THREAD_ENTRY *, xasl_node * xasl, xasl_state *, UPDDEL_CLASS_INSTANCE_LOCK_INFO *)
{
  fake_subquery_runs++;
  if (fake_subquery_fails)
    {
      er_set (ER_ERROR_SEVERITY, __FILE__, __LINE__, ER_QPROC_INVALID_QRY_SINGLE_TUPLE, 0);
      xasl->status = XASL_FAILURE;
      return ER_QPROC_INVALID_QRY_SINGLE_TUPLE;
    }
  db_make_int (xasl->single_tuple->valp->val, fake_subquery_value);
  xasl->status = XASL_SUCCESS;
  return NO_ERROR;
}

/* error_manager.c er_stack_push / er_stack_pop (error_context.cpp push_error_stack / pop_error_stack_and_destroy): push
 * starts a fresh error level (no error id) on top of the current one, pop drops the top level - whatever was set in it -
 * and the level below is current again. 3726fd95d fills the bits in such a frame. Per thread, as the error context. */
static thread_local std::vector<int> fake_er_stack;
static thread_local int fake_er_pushes = 0;
void
er_stack_push (void)
{
  fake_er_stack.push_back (fake_er_id);
  fake_er_id = NO_ERROR;
  fake_er_pushes++;
}

void
er_stack_pop (void)
{
  if (fake_er_stack.empty ())
    {
      fake_er_id = NO_ERROR;	/* a bad pop: the real one asserts and leaves the base level */
      return;
    }
  fake_er_id = fake_er_stack.back ();
  fake_er_stack.pop_back ();
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
  int opens = 0;		/* successful opens */
  int open_calls = 0;		/* every call, failed ones too (v4: a failed fill is not retried) */
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
  if (fl != nullptr)
    {
      fl->open_calls++;
    }
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

/* the TU's constants and whether it stages the filter (hooks.cpp) */
static const unitseam_bloom_consts &
C ()
{
  static unitseam_bloom_consts c;
  static bool done = false;
  if (!done)
    {
      unitseam_bloom_consts_of (&c);
      done = true;
    }
  return c;
}

/* the threshold attach gives a first input of first_rows rows: staged max (1,024, first rows / 2); earlier 1 (built at
 * the second input's first row) */
static long long
expected_threshold (long long first_rows)
{
  if (!C ().staged)
    {
      return 1;
    }
  return std::max (C ().min_second_rows, first_rows / C ().first_per_second);
}

static unitseam_stats_view
stats (const join & j)
{
  unitseam_stats_view s;
  unitseam_bloom_stats_of (j.hj, &s);
  return s;
}

/* ---------------------------------------------------------------- the aptr loop's calls, per revision */

/* the thread that ran the first input (8ad5ee2df on: a parallel job's, which calls hjoin_bloom_input_done) */
static THREAD_ENTRY *
first_runner ()
{
  static THREAD_ENTRY *t = new_thread ();
  return t;
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

/* how a revision of 8ad5ee2df on runs the first input's end: the status the test gave the first input says what
 * happened to it - XASL_SUCCESS: done (input_done (true)), XASL_FAILURE: failed (input_done (false)), a status not run
 * yet (BUILD / INITIALIZED / CLEARED): still running, nothing */
static void
first_input_end (join & j)
{
  if (j.first ()->status == XASL_SUCCESS)
    {
      hjoin_bloom_input_done (first_runner (), j.first (), true);
    }
  else if (j.first ()->status == XASL_FAILURE)
    {
      hjoin_bloom_input_done (first_runner (), j.first (), false);
    }
}

/*
 * The aptr loop's attach for the join's second input, as the revision has it:
 *   up to e8f629fcc: attach (hj, second) - the first input has run (its status says how); the attaching thread is the
 *     owner.
 *   8ad5ee2df on: attach (hj) before either input runs - the aptr list in run order, both inputs not run yet (their
 *     statuses set back to XASL_INITIALIZED around the call, restored after) - then the first input's end as its status
 *     says (first_input_end). The owner is bound to t as the earlier attach bound it, through hjoin_bloom_prepare: its
 *     binding is the one the owner's first row makes (hjoin_bloom_is_owner); B15 binds at first use on the raw API.
 *     A filter already on the inputs is left as it is (attach does nothing then).
 */
static void
attach_second (join & j, THREAD_ENTRY * t = nullptr)
{
  t = t != nullptr ? t : owner_thread ();
  if (!C ().concurrent)
    {
      hjoin_bloom_attach (t, j.hj, j.second ());
      return;
    }
  bool had = j.second ()->hj_bloom != NULL || j.first ()->hj_bloom != NULL;
  aptr_in_run_order (j);
  XASL_STATUS fs = j.first ()->status, ss = j.second ()->status;
  j.first ()->status = XASL_INITIALIZED;
  j.second ()->status = XASL_INITIALIZED;
  hjoin_bloom_attach (t, j.hj);
  j.first ()->status = fs;
  j.second ()->status = ss;
  if (had || j.second ()->hj_bloom == NULL)
    {
      return;
    }
  REQUIRE (j.first ()->hj_bloom == j.second ()->hj_bloom);
  (void) hjoin_bloom_prepare (t, j.second ());
  first_input_end (j);
}

/* the raw 8ad5ee2df attach: the inputs' statuses as they are, no first-input end, no owner bound */
static void
attach_raw (XASL_NODE * hj, THREAD_ENTRY * t = nullptr)
{
  REQUIRE (API ().attach2);
  hjoin_bloom_attach (t != nullptr ? t : owner_thread (), hj);
}

/* the second input is done and the filter's counts go to the hash join's bloom_stats: up to e8f629fcc detach
 * (second); 8ad5ee2df on input_done (second, true) on the owner's thread, then finish (hj) on the hash join's (the same
 * thread here, as when the aptr loop runs the inputs inline) */
static void
detach_second (join & j, THREAD_ENTRY * t = nullptr)
{
  t = t != nullptr ? t : owner_thread ();
  if (!C ().concurrent)
    {
      hjoin_bloom_detach (t, j.second ());
      return;
    }
  hjoin_bloom_input_done (t, j.second (), true);
  hjoin_bloom_finish (t, j.hj);
}

/* the same call on a node that is not the input (a cloned node carrying the pointer) */
static void
detach_node (XASL_NODE * x, THREAD_ENTRY * t = nullptr)
{
  t = t != nullptr ? t : owner_thread ();
  if (!C ().concurrent)
    {
      hjoin_bloom_detach (t, x);
      return;
    }
  hjoin_bloom_input_done (t, x, true);
}

/* a parallel scan worker's begin: 3726fd95d passes the clone's output list */
static HJOIN_BLOOM_WORKER *
wbegin (THREAD_ENTRY * t, HJOIN_BLOOM * b, OUTPTR_LIST * outptr = nullptr)
{
  if (API ().wbegin3)
    {
      return hjoin_bloom_worker_begin (t, b, outptr);
    }
  return hjoin_bloom_worker_begin (t, b);
}

/*
 * B6's question - will the input the aptr list runs second take a filter? 1a7bccdcc .. e8f629fcc:
 * hjoin_bloom_wants_serial (hj). 8ad5ee2df on has no separate question: attach (hj) decides before either input runs,
 * on the input the aptr list runs second - asked here with both inputs not run yet (statuses restored after): true when
 * it attached a filter whose checked input is that one (then released).
 */
static bool
serial_answer (XASL_NODE * hj)
{
  if (API ().wants_serial)
    {
      return hjoin_bloom_wants_serial (hj);
    }
  if (hj == NULL)
    {
      hjoin_bloom_attach (owner_thread (), (XASL_NODE *) NULL);
      return false;
    }
  XASL_NODE *o = hj->proc.hashjoin.outer.xasl, *i = hj->proc.hashjoin.inner.xasl;
  XASL_STATUS os = o->status, is = i->status;
  o->status = XASL_INITIALIZED;
  i->status = XASL_INITIALIZED;
  hjoin_bloom_attach (owner_thread (), hj);
  o->status = os;
  i->status = is;
  XASL_NODE *run_second = NULL;
  int inputs = 0;
  for (XASL_NODE * x = hj->aptr_list; x != NULL; x = x->next)
    {
      if ((x == o || x == i) && ++inputs == 2)
	{
	  run_second = x;
	  break;
	}
    }
  bool attached = o->hj_bloom != NULL || i->hj_bloom != NULL;
  bool on_second = run_second != NULL && run_second->hj_bloom != NULL && view (run_second).owner_xasl == run_second;
  CHECK (attached == on_second);
  if (attached)
    {
      hjoin_bloom_release (owner_thread (), o->hj_bloom != NULL ? o : i);
    }
  CHECK (o->hj_bloom == NULL);
  CHECK (i->hj_bloom == NULL);
  return on_second;
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

/* the decision on one second-input row, through a record bound to the second list (the hook's well-defined path):
 * the owner's per-row path - counted, set up / filled as the owner's rows do it, the sample judged.
 * -1 not checked, 0 kept, 1 skipped */
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

/* the filled filter's decision alone - the bits and the key, nothing counted, no sample judged (B1, B3's false-positive
 * rate: more than 4,096 rows that must not turn it off). -1 not ready, -2 fetch failed, 0 kept, 1 skipped */
static int
check_only (const join & j, std::vector<DB_VALUE> vals)
{
  QFILE_LIST_ID *l = j.second ()->list_id;
  char *tpl = make_tuple (l, vals);
  QFILE_TUPLE_RECORD r = bound (l, tpl);
  int d = unitseam_bloom_check_bound (owner_thread (), j.second (), &r);
  free (tpl);
  return d;
}

/*
 * The owner's rows before the filter is filled: staged, the second input's rows up to threshold - 1 are counted and go
 * through unchecked (NULL keys here: they would be dropped if checked); a revision that built the filter at the first
 * row has none. The next owner row fills the filter and is checked. Returns how many rows went through.
 */
static long long
owner_rows_before_fill (const join & j)
{
  unitseam_bloom_view v = view (j.second ());
  REQUIRE (v.present);
  long long n = v.threshold - 1 - (v.seen > 0 ? v.seen : 0);
  long long through = 0;
  for (long long i = 0; i < n; i++)
    {
      through += decide (j, { v_null () }) == -1;
    }
  REQUIRE (through == n);
  if (C ().staged)
    {
      REQUIRE (view (j.second ()).state == C ().waiting);
    }
  return n;
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
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  REQUIRE_FALSE (view (j.second ()).set_up);
  unitseam_bloom_fill_now (owner_thread (), j.second ());

  int dropped = 0, inactive = 0;
  for (int k : keys)
    {
      int d = check_only (j, { v_int (k) });
      dropped += (d == 1);
      inactive += (d < 0);
    }
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.set_up);
  CHECK (v.state == C ().ready);
  CHECK (v.keys == 100000);
  CHECK (inactive == 0);
  CHECK (dropped == 0);

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
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);

  int dropped = 0;
  for (auto &r : rows)
    {
      dropped += check_only (j, { cast_text (r.second.c_str (), vc), v_int (r.first) }) == 1;
    }
  CHECK (dropped == 0);
  /* the same pair with one column off is mostly skipped */
  int skipped = 0;
  for (int i = 0; i < 1000; i++)
    {
      skipped += check_only (j, { cast_text ("absent", vc), v_int (i) }) == 1;
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
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  unitseam_bloom_fill_now (owner_thread (), j.second ());

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
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
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
  attach_second (n);
  unitseam_bloom_fill_now (owner_thread (), n.second ());
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
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  int probes = 0, passed = 0;
  while (probes < 100000)
    {
      int k = (int) rng ();
      if (member.count (k))
	{
	  continue;
	}
      probes++;
      passed += check_only (j, { v_int (k) }) == 0;
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
  attach_second (s);
  unitseam_bloom_fill_now (owner_thread (), s.second ());
  int spassed = 0;
  for (int i = 100001; i <= 200000; i++)
    {
      spassed += check_only (s, { v_int (i) }) == 0;
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
      attach_second (j);
      REQUIRE (view (j.second ()).present);
      unitseam_bloom_fill_now (owner_thread (), j.second ());
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
  int concurrent_expect = -1;	/* 8ad5ee2df on (B4): one of the C_* outcomes below; -1: as expect */
};

/* what a filter attached before the inputs ran (8ad5ee2df on) comes to once the first input ends as its status says */
enum
{
  C_NONE,			/* nothing attached */
  C_FILLS,			/* first input done: the filter waits for the second's rows and fills */
  C_PENDING,			/* first input still running: the filter waits, nothing to fill from yet */
  C_OFF_FIRST_ROWS,		/* first input failed, empty or too large: off (OFF_FIRST_ROWS) */
  C_FILL_FAILS			/* first input done, but the fill cannot set up: off (OFF_FAILED) */
};

static int
concurrent_outcome (join & j)
{
  attach_second (j);
  if (j.second ()->hj_bloom == NULL)
    {
      CHECK (j.first ()->hj_bloom == NULL);
      return C_NONE;
    }
  unitseam_bloom_view v = view (j.second ());
  int r;
  if (v.state == C ().off)
    {
      CHECK (v.off_reason == C ().out_off_first_rows);
      r = v.off_reason == C ().out_off_first_rows ? C_OFF_FIRST_ROWS : -2;
    }
  else if (v.first_done == 0)
    {
      r = C_PENDING;
    }
  else
    {
      unitseam_bloom_fill_now (owner_thread (), j.second ());
      v = view (j.second ());
      r = v.state == C ().ready ? C_FILLS : v.state == C ().off && v.off_reason == C ().out_off_failed ? C_FILL_FAILS
	: -3;
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  CHECK (j.first ()->hj_bloom == NULL);
  return r;
}

static bool
attached_after (join & j)
{
  attach_second (j);
  bool a = j.second ()->hj_bloom != NULL;
  if (a)
    {
      unitseam_bloom_view v = view (j.second ());
      CHECK (v.owner_xasl == j.second ());
      CHECK (v.owner_thread == owner_thread ());
      CHECK (v.hj_xasl == j.hj);
      CHECK (v.first_is_outer == j.first_is_outer);
      CHECK_FALSE (v.set_up);
      CHECK (v.state == C ().waiting);
      CHECK (v.bits == NULL);
      if (C ().staged)
	{
	  CHECK (v.first_rows == j.first ()->list_id->tuple_cnt);
	  CHECK (v.threshold == expected_threshold (j.first ()->list_id->tuple_cnt));
	  CHECK (v.seen == 0);
	}
      if (C ().concurrent)
	{
	  /* both inputs carry it; the first input's end published (its status is XASL_SUCCESS here) */
	  CHECK (v.first_xasl == j.first ());
	  CHECK (j.first ()->hj_bloom == j.second ()->hj_bloom);
	  CHECK (v.first_done == 1);
	}
      hjoin_bloom_release (owner_thread (), j.second ());
      CHECK (j.first ()->hj_bloom == NULL);
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
	   "subquery cache) - 8ad5ee2df on: attach before the inputs run takes the second's shape, the first input's end "
	   "decides the rest (failed / 0 / over 10,000,000 rows -> OFF_FIRST_ROWS, still running -> waits), an input "
	   "already run takes none", "[contract]")
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
    {"control: INNER, first 2 rows done", true, [] (join &) {}, C_FILLS},
    {"parameter off", false, [] (join &) { set_bloom_param (false); }},
    {"first status XASL_INITIALIZED (not run yet)", false, [] (join & j) { j.first ()->status = XASL_INITIALIZED; }, C_PENDING},
    {"first status XASL_CLEARED", false, [] (join & j) { j.first ()->status = XASL_CLEARED; }, C_PENDING},
    {"first status XASL_FAILURE", false, [] (join & j) { j.first ()->status = XASL_FAILURE; }, C_OFF_FIRST_ROWS},
    {"first status XASL_BUILD", false, [] (join & j) { j.first ()->status = XASL_BUILD; }, C_PENDING},
    {"first 0 rows", false, [] (join & j) { j.first ()->list_id->tuple_cnt = 0; }, C_OFF_FIRST_ROWS},
    {"first exactly 10,000,000 rows", true, [] (join & j) { j.first ()->list_id->tuple_cnt = 10000000; }, C_FILLS},
    {"first 10,000,001 rows", false, [] (join & j) { j.first ()->list_id->tuple_cnt = 10000001; }, C_OFF_FIRST_ROWS},
    {"first list never opened (type_cnt 0)", false, [] (join & j) { j.first ()->list_id->type_list.type_cnt = 0; }, C_FILL_FAILS},
    {"first list_id NULL", false, [] (join & j) { j.first ()->list_id = NULL; }, C_OFF_FIRST_ROWS},
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
      if (!C ().concurrent)
	{
	  CHECK (attached_after (j) == c.expect);
	}
      else
	{
	  int expect = c.concurrent_expect >= 0 ? c.concurrent_expect : c.expect ? C_FILLS : C_NONE;
	  CHECK (concurrent_outcome (j) == expect);
	}
    }
  set_bloom_param (true);

  if (C ().concurrent)
    {
      /* 8ad5ee2df on: attach runs before the inputs - an input that already ran takes none (its rows are written), and
       * neither does an aptr list that holds one input only */
      join a = attach_join (JOIN_INNER, true);
      aptr_in_run_order (a);
      REQUIRE (a.first ()->status == XASL_SUCCESS);
      attach_raw (a.hj);
      CHECK (a.first ()->hj_bloom == NULL);
      CHECK (a.second ()->hj_bloom == NULL);
      a.first ()->status = XASL_INITIALIZED;
      a.second ()->status = XASL_SUCCESS;
      attach_raw (a.hj);
      CHECK (a.first ()->hj_bloom == NULL);
      CHECK (a.second ()->hj_bloom == NULL);
      a.second ()->status = XASL_INITIALIZED;
      XASL_NODE *stranger = new_input (new_list ({ dom_int () }));
      a.hj->aptr_list = stranger;
      stranger->next = a.first ();
      a.first ()->next = NULL;
      attach_raw (a.hj);
      CHECK (a.first ()->hj_bloom == NULL);
      CHECK (stranger->hj_bloom == NULL);
      aptr_in_run_order (a);
      attach_raw (a.hj);
      CHECK (a.first ()->hj_bloom != NULL);
      CHECK (a.second ()->hj_bloom == a.first ()->hj_bloom);
      hjoin_bloom_release (owner_thread (), a.first ());	/* an error path's clear of the first input: frees it all */
      CHECK (a.first ()->hj_bloom == NULL);
      CHECK (a.second ()->hj_bloom == NULL);
    }

  /* an XASL that is not an input of this join, and a second that already carries a filter */
  join j = attach_join (JOIN_INNER, true);
  if (!C ().concurrent)
    {
      XASL_NODE *stranger = new_input (new_list ({ dom_int () }));
      hjoin_bloom_attach (owner_thread (), j.hj, stranger);
      CHECK (stranger->hj_bloom == NULL);
    }
  attach_second (j);
  struct hjoin_bloom *first_filter = j.second ()->hj_bloom;
  REQUIRE (first_filter != NULL);
  attach_second (j);
  CHECK (j.second ()->hj_bloom == first_filter);
  hjoin_bloom_release (owner_thread (), j.second ());
  /* NULL arguments */
  if (!C ().concurrent)
    {
      hjoin_bloom_attach (owner_thread (), NULL, j.second ());
      hjoin_bloom_attach (owner_thread (), j.hj, NULL);
    }
  else
    {
      hjoin_bloom_attach (owner_thread (), (XASL_NODE *) NULL);
    }
  CHECK (j.second ()->hj_bloom == NULL);
}

TEST_CASE ("B4: a thread other than the owner and a cloned node let every row through; release on a clone frees nothing",
	   "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = int_join_with ({ 10, 20, 30 });
  attach_second (j);
  REQUIRE (view (j.second ()).present);

  std::vector<DB_VALUE> absent = ints ({ 99 });
  QFILE_LIST_ID *sl = j.second ()->list_id;
  stage_descr (sl, absent);
  char *tpl = make_tuple (sl, absent);

  /* another thread: no set-up, no row counted, no decision (these return before reading the row) */
  CHECK_FALSE (hjoin_bloom_skip_descr (other_thread (), j.second ()));
  CHECK_FALSE (hjoin_bloom_skip_tuple (other_thread (), j.second (), tpl));
  CHECK (decide (j, ints ({ 99 }), other_thread ()) == -1);
  CHECK_FALSE (view (j.second ()).set_up);
  CHECK (view (j.second ()).tested == 0);
  if (C ().staged)
    {
      CHECK (view (j.second ()).seen == 0);
    }

  /* the owner: its rows up to the threshold go through unchecked (staged), then it fills and decides */
  owner_rows_before_fill (j);
  CHECK (decide (j, ints ({ 99 })) == 1);
  CHECK (decide (j, ints ({ 20 })) == 0);
  unitseam_bloom_view before = view (j.second ());
  REQUIRE (before.state == C ().ready);

  /* a clone carrying the pointer (a parallel worker's node) */
  XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
  memcpy (clone, j.second (), sizeof (XASL_NODE));
  REQUIRE (clone->hj_bloom == j.second ()->hj_bloom);
  long long seen_before = view (j.second ()).seen;
  CHECK_FALSE (hjoin_bloom_skip_descr (owner_thread (), clone));
  CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), clone, tpl));
  {
    QFILE_TUPLE_RECORD r = bound (sl, tpl);
    CHECK (unitseam_bloom_skip_bound (owner_thread (), clone, &r) == -1);
  }
  CHECK (view (j.second ()).seen == seen_before);	/* a clone's rows are not the owner's */
  /* detach on the clone moves nothing */
  HASHJOIN_BLOOM_STATS *g = &j.hj->proc.hashjoin.bloom_stats;
  unitseam_bloom_stats_stale (j.hj);
  HASHJOIN_BLOOM_STATS snap;
  memcpy (&snap, g, sizeof (snap));
  detach_node (clone);
  CHECK (memcmp (&snap, g, sizeof (snap)) == 0);
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

TEST_CASE ("B4: a filter that cannot be filled lets every row through (open fails, scan error with an error id) - "
	   "from the row that fills it (staged: the threshold-th; earlier: the first), tried once", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  {
    join j = int_join_with ({ 1, 2, 3 });
    fake_list *fl = fake_list_of (j.first ()->list_id);
    fl->fail_open = true;
    attach_second (j);
    owner_rows_before_fill (j);
    CHECK (fl->open_calls == 0);
    CHECK (decide (j, ints ({ 99 })) == -1);	/* the row that fills it: the fill fails */
    CHECK (decide (j, ints ({ 1 })) == -1);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.set_up);
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_off_failed);
    CHECK (fl->open_calls == 1);	/* not retried */
    CHECK (er_errid () == NO_ERROR);	/* the filter is an optimization: its failure is cleared */
    detach_second (j);
    unitseam_stats_view s = stats (j);
    if (C ().staged)
      {
	CHECK (s.shown);	/* "BLOOM (off: could not be built)" */
	CHECK (s.outcome == C ().out_off_failed);
      }
    else
      {
	CHECK_FALSE (s.shown);
      }
  }
  {
    join j = int_join_with ({ 1, 2, 3, 4, 5, 6 });
    fake_list_of (j.first ()->list_id)->stop_at = 3;
    fake_list_of (j.first ()->list_id)->stop_sets_error = true;
    attach_second (j);
    owner_rows_before_fill (j);
    CHECK (decide (j, ints ({ 5 })) == -1);
    CHECK (view (j.second ()).state == C ().off);
    CHECK (er_errid () == NO_ERROR);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
}

/* ================================================================ B5: detach */

TEST_CASE ("B5: detach moves keys / tested / skipped to the hash join's bloom_stats, marks it shown (staged: with the "
	   "outcome, threshold and seen), frees the filter; a filter no row reached is reported not built (staged) or "
	   "not at all (earlier)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  join j = int_join_with ({ 1, 2, 3, 4 });
  attach_second (j);
  long long unchecked = owner_rows_before_fill (j);
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
  detach_second (j);
  unitseam_stats_view g = stats (j);
  CHECK (g.shown);
  CHECK (g.outcome == C ().out_checked);
  CHECK (g.keys == 4);
  CHECK (g.tested == 43);
  CHECK (g.skipped == 1 + extra);
  CHECK (g.tested == v.tested);
  CHECK (g.skipped == v.skipped);
  if (C ().staged)
    {
      CHECK (g.threshold == v.threshold);
      CHECK (g.threshold == expected_threshold (4));
      CHECK (g.seen == unchecked + 43);
    }
  CHECK (j.second ()->hj_bloom == NULL);
  CHECK (live ().count (b) == 0);

  /* attached but no row came: nothing filled, nothing counted */
  join e = int_join_with ({ 1 });
  attach_second (e);
  detach_second (e);
  unitseam_stats_view es = stats (e);
  if (C ().staged)
    {
      CHECK (es.shown);		/* "BLOOM (not built: second input gave 0 rows, threshold 1024)" */
      CHECK (es.outcome == C ().out_not_built);
      CHECK (es.seen == 0);
      CHECK (es.threshold == expected_threshold (1));
    }
  else
    {
      CHECK_FALSE (es.shown);
    }
  CHECK (es.keys == 0);
  CHECK (es.tested == 0);
  CHECK (e.second ()->hj_bloom == NULL);
}

/* a re-execution must not print the previous execution's BLOOM line: attach starts the trace empty */
static void
stale_bloom_stats (const join & j)
{
  unitseam_bloom_stats_stale (j.hj);
}

static bool
bloom_stats_clear (const join & j)
{
  static const char zero[sizeof (HASHJOIN_BLOOM_STATS)] = { 0 };
  return memcmp (&j.hj->proc.hashjoin.bloom_stats, zero, sizeof (zero)) == 0;
}

TEST_CASE ("B5: attach clears the hash join's bloom_stats at each execution, also when it attaches nothing (the aptr "
	   "loop's call for the first input, parameter off, a preserved second side)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  {
    join j = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
    add_row (j.inner->list_id, { v_int (1) });
    stale_bloom_stats (j);
    if (!C ().concurrent)
      {
	/* the aptr loop reaches attach for the outer first, while the inner has not run */
	hjoin_bloom_attach (owner_thread (), j.hj, j.outer);
      }
    else
      {
	/* 8ad5ee2df on: one attach before the inputs run - an input already run takes none */
	j.inner->status = XASL_SUCCESS;
	attach_raw (j.hj);
	CHECK (j.inner->hj_bloom == NULL);
      }
    CHECK (j.outer->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    join j = int_join_with ({ 1, 2 });
    stale_bloom_stats (j);
    set_bloom_param (false);
    attach_second (j);
    set_bloom_param (true);
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    join j = int_join_with ({ 1, 2 }, JOIN_RIGHT);	/* outer first: the inner is RIGHT's preserved side */
    stale_bloom_stats (j);
    attach_second (j);
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK (bloom_stats_clear (j));
  }
  {
    /* attached: cleared too, and the detach of this execution fills it */
    join j = int_join_with ({ 1, 2 });
    stale_bloom_stats (j);
    attach_second (j);
    REQUIRE (j.second ()->hj_bloom != NULL);
    CHECK (bloom_stats_clear (j));
    unitseam_bloom_fill_now (owner_thread (), j.second ());
    CHECK (decide (j, ints ({ 2 })) == 0);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_checked);
    CHECK (s.keys == 2);
    CHECK (s.tested == 1);
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
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  CHECK (decide (j, ints ({ 2 })) == 0);
  CHECK (decide (j, ints ({ 77 })) == 1);
  detach_second (j);
  unitseam_stats_view g = stats (j);
  REQUIRE (g.shown);		/* the aptr loop's detach did its part */
  REQUIRE (g.keys == 3);
  REQUIRE (g.tested == 2);
  REQUIRE (g.skipped == 1);

  /* qexec_execute_mainblock_internal: aptr loop (attach .. detach) -> HASHJOIN_PROC -> qexec_hash_join ->
   * hjoin_init_manager, which memsets proc->stats_group when the thread is on trace. A sentinel in the stats_group
   * shows the trace branch ran (otherwise the counters would survive for the wrong reason). */
  j.hj->proc.hashjoin.stats_group.context_cnt = 99;
  owner_thread ()->on_trace = true;
  int rc = unitseam_hj_init_manager (owner_thread (), j.hj);
  owner_thread ()->on_trace = false;
  REQUIRE (rc == NO_ERROR);
  REQUIRE (j.hj->proc.hashjoin.stats_group.context_cnt == 0);
  /* qdump_print_hashjoin_stats_text prints BLOOM when proc->bloom_stats is shown (staged: attached; earlier: used) */
  g = stats (j);
  CHECK (g.shown);
  CHECK (g.outcome == C ().out_checked);
  CHECK (g.keys == 3);
  CHECK (g.tested == 2);
  CHECK (g.skipped == 1);
  if (C ().staged)
    {
      CHECK (g.seen == 2);
      CHECK (g.threshold == expected_threshold (3));
    }
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
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  int dropped = 0;
  for (int k : keys)
    {
      dropped += decide (j, ints ({ k })) == 1;
    }
  unitseam_bloom_view v = view (j.second ());
  WARN ("filter state after S_ERROR without an error id: " << v.state << " (ready " << C ().ready << ", off " << C ().off
	<< ") keys=" << v.keys << " dropped " << dropped << " of 50 first-input keys");
  CHECK (v.state == C ().ready);
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
/* below the non-PIE image (0x400000) so the heap, which ASLR starts at a random address after the image, never
 * covers it - 0x40008000 collided now and then (MAP_FIXED_NOREPLACE failed: about 2.6% of full runs) */
static const uintptr_t DECOY_ADDR = 0x00208000;

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
  attach_second (s.j);
  REQUIRE (s.j.second ()->hj_bloom != NULL);
  unitseam_bloom_fill_now (owner_thread (), s.j.second ());
  REQUIRE (view (s.j.second ()).state == C ().ready);
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
 * The parallel-path API 1a7bccdcc added (declared weak at the top, with the rest of the API across revisions): on a
 * revision without it (fdc3fcd32) B6-B8 are not registered at all ("not applicable" - the case is absent, so
 * unit_regress shows no base result for it). 8ad5ee2df dropped hjoin_bloom_wants_serial (attach (hj) decides before
 * the inputs run) and 3726fd95d gave hjoin_bloom_worker_begin the clone's output list: the v3 API is "linked" when
 * prepare, a worker_begin, skip_descr, skip_tuple and end link, and either wants_serial or the 8ad5ee2df attach does -
 * B6 then asks attach (hj) through serial_answer ().
 */
static int
v3_api_linked_count ()
{
  return (API ().wants_serial || API ().attach2) + API ().prepare + (API ().wbegin2 || API ().wbegin3)
    + API ().wskip_descr + API ().wskip_tuple + API ().wend;
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
      bool ws = serial_answer (j.hj);
      CHECK (ws == c.expect);
      CHECK (attached_after (j) == ws);	/* the two agree once the first input is done */
    }

  join j = attach_join (JOIN_INNER, true);
  aptr_in_run_order (j);
  set_bloom_param (false);
  CHECK_FALSE (serial_answer (j.hj));
  set_bloom_param (true);
  CHECK (serial_answer (j.hj));
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
      bool ws = serial_answer (j.hj);
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
      CHECK (serial_answer (j.hj) == c.expect);
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
    CHECK (serial_answer (j.hj));	/* LEFT, inner second: not preserved */
    aptr_list_of (j, { j.inner, j.outer });
    CHECK_FALSE (serial_answer (j.hj));	/* LEFT, outer second: preserved */
  }
  {
    join j = attach_join (JOIN_RIGHT, true);
    aptr_list_of (j, { j.outer, j.inner });
    CHECK_FALSE (serial_answer (j.hj));
    aptr_list_of (j, { j.inner, j.outer });
    CHECK (serial_answer (j.hj));
  }
  {
    /* the shape that matters is the one of the input run second */
    join j = attach_join (JOIN_INNER, true);
    j.inner->proc.buildlist.groupby_list = sl;
    aptr_list_of (j, { j.outer, j.inner });
    CHECK_FALSE (serial_answer (j.hj));
    aptr_list_of (j, { j.inner, j.outer });
    CHECK (serial_answer (j.hj));
  }
  {
    /* other subqueries in the aptr list (uncorrelated subqueries of the join's predicates) are passed over */
    join j = attach_join (JOIN_LEFT, true);
    XASL_NODE *x1 = new_input (new_list ({ dom_int () }));
    XASL_NODE *x2 = new_input (new_list ({ dom_int () }));
    XASL_NODE *x3 = new_input (new_list ({ dom_int () }));
    aptr_list_of (j, { x1, j.outer, x2, j.inner, x3 });
    CHECK (serial_answer (j.hj));
    aptr_list_of (j, { x1, j.inner, x2, j.outer, x3 });
    CHECK_FALSE (serial_answer (j.hj));
    /* an input missing from the aptr list: nothing to serialize */
    aptr_list_of (j, { x1, j.outer, x2 });
    CHECK_FALSE (serial_answer (j.hj));
    aptr_list_of (j, { x1, x2 });
    CHECK_FALSE (serial_answer (j.hj));
    aptr_list_of (j, {});
    CHECK_FALSE (serial_answer (j.hj));
  }
  {
    join j = attach_join (JOIN_INNER, true);
    aptr_in_run_order (j);
    REQUIRE (serial_answer (j.hj));
    XASL_SET_FLAG (j.second (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK_FALSE (serial_answer (j.hj));
    XASL_CLEAR_FLAG (j.second (), XASL_LINK_TO_REGU_VARIABLE);
    XASL_SET_FLAG (j.first (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK_FALSE (serial_answer (j.hj));
    XASL_CLEAR_FLAG (j.first (), XASL_LINK_TO_REGU_VARIABLE);
    CHECK (serial_answer (j.hj));
    j.hj->type = MERGELIST_PROC;
    CHECK_FALSE (serial_answer (j.hj));
  }
  CHECK_FALSE (serial_answer (NULL));
}

/* ================================================================ B7: hjoin_bloom_prepare */

V3_TEST_CASE (b7_prepare, "B7: prepare on the owner thread sets the filter up and returns it, once - staged: domains "
	      "and key only, the owner's rows fill it at the threshold (one pass over the first input); earlier: built at "
	      "once; another thread, a cloned node, a node without a filter, NULL, a set-up that fails and a filter turned "
	      "off (a failed fill) -> NULL", "[contract]")
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
  attach_second (j);
  REQUIRE (j.second ()->hj_bloom != NULL);
  REQUIRE (first_scan->open_calls == 0);

  /* not the owner: no set-up, no filter */
  CHECK (hjoin_bloom_prepare (other_thread (), j.second ()) == NULL);
  XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
  memcpy (clone, j.second (), sizeof (XASL_NODE));
  CHECK (hjoin_bloom_prepare (owner_thread (), clone) == NULL);
  CHECK_FALSE (view (j.second ()).set_up);
  CHECK (first_scan->open_calls == 0);

  /* the owner: set up now, before any row (8ad5ee2df on: the set-up is part of the fill - prepare only binds) */
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  CHECK (b == j.second ()->hj_bloom);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.set_up == !C ().concurrent);
  CHECK ((v.key != NULL) == !C ().concurrent);
  CHECK (v.tested == 0);
  if (C ().staged)
    {
      /* the bits wait for threshold rows */
      CHECK (v.state == C ().waiting);
      CHECK (v.bits == NULL);
      CHECK (v.keys == 0);
      CHECK (v.seen == 0);
      CHECK (first_scan->open_calls == 0);
    }
  else
    {
      CHECK (v.state == C ().ready);
      CHECK (v.keys == 100);
      CHECK (first_scan->open_calls == 1);
    }
  int opens_after_prepare = first_scan->open_calls;

  /* again: the same filter, nothing more done */
  CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == b);
  CHECK (first_scan->open_calls == opens_after_prepare);

  /* the owner's own rows (rows a serial part of the scan writes on the owner thread): up to the threshold through
   * unchecked (staged), then the filled bits - one pass over the first input in all */
  owner_rows_before_fill (j);
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
  CHECK (first_scan->open_calls == 1);
  CHECK (after.state == C ().ready);
  CHECK (after.keys == 100);
  CHECK (after.tested == 3);
  CHECK (after.skipped == 2);
  CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == b);	/* still on: still the filter */
  CHECK (first_scan->open_calls == 1);

  /* no filter on the node, or no node */
  CHECK (hjoin_bloom_prepare (owner_thread (), j.first ()) == NULL);
  CHECK (hjoin_bloom_prepare (owner_thread (), NULL) == NULL);
  free (clone);
  hjoin_bloom_release (owner_thread (), j.second ());

  /* a set-up that cannot complete (the second list is not open yet - type_cnt 0): NULL, off, every row through
   * (8ad5ee2df on: prepare gives the filter, the set-up fails at the threshold row's fill) */
  {
    join f = int_join_with ({ 1, 2, 3 });
    attach_second (f);
    REQUIRE (f.second ()->hj_bloom != NULL);
    int saved = f.second ()->list_id->type_list.type_cnt;
    if (!C ().concurrent)
      {
	f.second ()->list_id->type_list.type_cnt = 0;
	CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == NULL);
      }
    else
      {
	CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == f.second ()->hj_bloom);
	CHECK_FALSE (view (f.second ()).set_up);
	owner_rows_before_fill (f);
	std::vector<DB_VALUE> row = ints ({ 2 });
	char *t = make_tuple (f.second ()->list_id, row);
	f.second ()->list_id->type_list.type_cnt = 0;
	CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), f.second (), t));
	free (t);
      }
    f.second ()->list_id->type_list.type_cnt = saved;
    unitseam_bloom_view fv = view (f.second ());
    CHECK (fv.set_up);
    CHECK (fv.state == C ().off);
    CHECK (fv.off_reason == C ().out_off_failed);
    CHECK (fake_list_of (f.first ()->list_id)->open_calls == 0);
    CHECK (er_errid () == NO_ERROR);
    CHECK (decide (f, ints ({ 99 })) == -1);
    CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == NULL);
    hjoin_bloom_release (owner_thread (), f.second ());
  }

  /* a fill that cannot complete (the first list does not open): earlier at prepare (NULL); staged at the fill - prepare
   * gave the filter, the threshold row's fill turns it off, prepare gives NULL from then on */
  {
    join f = int_join_with ({ 1, 2, 3 });
    fake_list_of (f.first ()->list_id)->fail_open = true;
    attach_second (f);
    HJOIN_BLOOM *fb = hjoin_bloom_prepare (owner_thread (), f.second ());
    if (C ().staged)
      {
	CHECK (fb == f.second ()->hj_bloom);
	owner_rows_before_fill (f);
	CHECK (decide (f, ints ({ 99 })) == -1);	/* the threshold row: the fill fails */
      }
    else
      {
	CHECK (fb == NULL);
      }
    unitseam_bloom_view fv = view (f.second ());
    CHECK (fv.state == C ().off);
    CHECK (fv.off_reason == C ().out_off_failed);
    CHECK (fake_list_of (f.first ()->list_id)->open_calls == 1);
    CHECK (er_errid () == NO_ERROR);
    CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == NULL);
    CHECK (decide (f, ints ({ 99 })) == -1);
    hjoin_bloom_release (owner_thread (), f.second ());
  }
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

/* nkeys random INT first-input keys (+ two NULL keys); rows: those keys, 2 x nkeys absent keys and 20 NULLs, shuffled -
 * about 2/3 dropped, far from the 50% bound of a staged filter's 4,096-row sample (a sample near it would turn the
 * filter off for some checkers and not others). The filter is prepared and filled before any worker begins (staged:
 * what hjoin_bloom_poll does once the workers have given threshold rows; earlier: prepare builds it). */
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
  attach_second (s.j);
  REQUIRE (s.j.second ()->hj_bloom != NULL);
  s.b = hjoin_bloom_prepare (owner_thread (), s.j.second ());
  REQUIRE (s.b != NULL);
  unitseam_bloom_fill_now (owner_thread (), s.j.second ());
  REQUIRE (view (s.j.second ()).state == C ().ready);
  REQUIRE (view (s.j.second ()).keys == nkeys);

  std::vector<std::pair<std::vector<DB_VALUE>, bool>> rows;
  for (int k : s.present)
    {
      rows.push_back ({ ints ({ k }), true });
    }
  int absent = 0;
  while (absent < 2 * nkeys)
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
  HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), s.b);
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

V3_TEST_CASE (b8_same_decision, "B8: a worker decides as the owner does - on the same rows (2,000 first-input keys, "
	      "4,000 absent, NULLs) two workers, through the descriptor and the tuple path, skip exactly the rows the "
	      "owner skips; no first-input key is skipped", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  int_scene s = int_scene_setup (2000, 27659);
  HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), s.b);
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
  attach_second (j);
  REQUIRE (j.second ()->hj_bloom != NULL);
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  unitseam_bloom_fill_now (owner_thread (), j.second ());	/* what hjoin_bloom_poll does at the threshold */
  HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
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
  HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), s.b);
  HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), s.b);
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

  detach_second (s.j);
  unitseam_stats_view g = stats (s.j);
  CHECK (g.shown);
  CHECK (g.outcome == C ().out_checked);
  CHECK (g.keys == 1000);
  CHECK (g.tested == (long long) n);
  CHECK (g.tested == t0 + t1 + t2);
  CHECK (g.skipped == s0 + s1 + s2);
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
  attach_second (j);
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  unitseam_bloom_fill_now (owner_thread (), j.second ());	/* what hjoin_bloom_poll does at the threshold */
  REQUIRE (j.hj->proc.hashjoin.domain_info.need_coerce_domains);
  HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), b);
  HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), b);
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
  CHECK (view (j.second ()).state == C ().ready);	/* the shared filter is untouched */

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
  HJOIN_BLOOM_WORKER *w3 = wbegin (worker_thread (2), b);
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
      HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (3), b);
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
  CHECK (wbegin (worker_thread (0), NULL) == NULL);
  int_scene s = int_scene_setup (100, 3);
  QFILE_LIST_ID *l = worker_list (s.j);
  std::vector<DB_VALUE> a = ints ({ 0x7fff0000 });
  stage_descr (l, a);
  CHECK_FALSE (hjoin_bloom_worker_skip_descr (worker_thread (0), NULL, l));
  char *t = make_tuple (l, a);
  CHECK_FALSE (hjoin_bloom_worker_skip_tuple (worker_thread (0), NULL, l, t));
  hjoin_bloom_worker_end (worker_thread (0), NULL);
  HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), s.b);
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
  int_scene s = int_scene_setup (3000, 99);
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
	HJOIN_BLOOM_WORKER *w = wbegin (t, s.b);
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

/* ================================================================ v4 (e8f629fcc): waiting -> ready / off */

/*
 * e8f629fcc fills the bits only once the second input has given threshold rows, judges the first 4,096 checked rows,
 * and has the owner fill a parallel scan's filter while it polls for the workers (hjoin_bloom_poll). Weak, as the v3
 * API: on a revision without hjoin_bloom_poll, B9-B12 are not registered (not applicable - no base result).
 */
#include <chrono>

#define V4_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = hjoin_bloom_poll != nullptr \
    && v3_api_linked_count () == V3_API_COUNT \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* registered on every revision: hjoin_bloom_poll and the staged struct (hooks: HJOIN_BLOOM_SEEN_BATCH) come together */
TEST_CASE ("B9-B12 guard: hjoin_bloom_poll links exactly on a revision whose filter waits for a threshold (the TU "
	   "defines HJOIN_BLOOM_SEEN_BATCH) - B9-B12 are registered only there", "[contract]")
{
  bool poll = hjoin_bloom_poll != nullptr;
  WARN ("hjoin_bloom_poll " << (poll ? "linked" : "absent") << ", staged filter " << (C ().staged ? "yes" : "no")
	<< (poll && C ().staged ? " - B9-B12 registered" : !poll && !C ().staged ?
	    " - B9-B12 not applicable on this revision" : ""));
  CHECK (poll == C ().staged);
  if (poll)
    {
      CHECK (v3_api_linked_count () == V3_API_COUNT);
    }
}

static std::vector<DB_VALUE>
null_row ()
{
  return { v_null () };
}

/* one owner row through a public entry: path 0 the descriptor (hjoin_bloom_skip_descr: qexec_end_one_iteration),
 * 1 the tuple (hjoin_bloom_skip_tuple: the big-record / set path) */
static bool
owner_entry (const join & j, std::vector<DB_VALUE> row, int path)
{
  QFILE_LIST_ID *sl = j.second ()->list_id;
  if (path == 0)
    {
      stage_descr (sl, row);
      return hjoin_bloom_skip_descr (owner_thread (), j.second ());
    }
  char *t = make_tuple (sl, row);
  bool s = hjoin_bloom_skip_tuple (owner_thread (), j.second (), t);
  free (t);
  return s;
}

static std::vector<int>
int_range (int from, int to)
{
  std::vector<int> v;
  for (int i = from; i <= to; i++)
    {
      v.push_back (i);
    }
  return v;
}

/* ================================================================ B9: the owner's rows - threshold, fill, sample */

V4_TEST_CASE (b9_threshold, "B9: the threshold is max (1,024, first rows / 2) - 1, 2,048 and 2,049 first rows -> "
	      "1,024, 2,050 -> 1,025, 3,000 -> 1,500, 10,000,000 -> 5,000,000; the owner's rows before it go through "
	      "unchecked through both entries (a NULL key, an absent key, a first-input key alike), each counted in seen; "
	      "the first sets up the domains and the key without reading the first input - no bits, no keys, nothing "
	      "tested; another thread's rows and a NULL tuple are not counted", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  struct
  {
    long long rows;
    long long threshold;
  } cases[] = { {1, 1024}, {2048, 1024}, {2049, 1024}, {2050, 1025}, {3000, 1500}, {10000000, 5000000} };
  for (auto &c : cases)
    {
      join j = int_join_with ({ 1, 2, 3 });
      j.first ()->list_id->tuple_cnt = c.rows;	/* attach reads tuple_cnt */
      attach_second (j);
      unitseam_bloom_view v = view (j.second ());
      INFO ("first rows " << c.rows);
      REQUIRE (v.present);
      CHECK (v.first_rows == c.rows);
      CHECK (v.threshold == c.threshold);
      CHECK (v.threshold == expected_threshold (c.rows));
      CHECK (v.state == C ().waiting);
      CHECK_FALSE (v.set_up);
      CHECK (v.seen == 0);
      hjoin_bloom_release (owner_thread (), j.second ());
    }

  std::vector<int> keys;
  for (int i = 1; i <= 3000; i++)
    {
      keys.push_back (i * 7);
    }
  join j = int_join_with (keys);
  fake_list *first = fake_list_of (j.first ()->list_id);
  attach_second (j);
  REQUIRE (view (j.second ()).threshold == 1500);

  /* the first row: set up, the first input not read (8ad5ee2df on: nothing set up before the fill) */
  CHECK_FALSE (owner_entry (j, null_row (), 0));
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.set_up == !C ().concurrent);
  CHECK ((v.key != NULL) == !C ().concurrent);
  CHECK (v.state == C ().waiting);
  CHECK (v.seen == 1);
  CHECK (first->open_calls == 0);

  /* rows 2 .. 1,499 go through unchecked */
  int through = 0;
  for (int i = 2; i <= 1499; i++)
    {
      std::vector<DB_VALUE> row = i % 3 == 0 ? null_row () : i % 3 == 1 ? ints ({ i * 7 + 1 }) : ints ({ keys[i] });
      through += !owner_entry (j, row, i % 2);
    }
  CHECK (through == 1498);
  /* not rows of the owner: a NULL tuple, another thread's row */
  CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), j.second (), NULL));
  {
    std::vector<DB_VALUE> row = ints ({ 7 });
    stage_descr (j.second ()->list_id, row);
    CHECK_FALSE (hjoin_bloom_skip_descr (other_thread (), j.second ()));
  }
  v = view (j.second ());
  CHECK (v.seen == 1499);
  CHECK (v.state == C ().waiting);
  CHECK (v.bits == NULL);
  CHECK (v.keys == 0);
  CHECK (v.tested == 0);
  CHECK (v.skipped == 0);
  CHECK (v.row_buf == NULL);	/* no row was serialized for a check */
  CHECK (first->open_calls == 0);
  hjoin_bloom_release (owner_thread (), j.second ());
}

V4_TEST_CASE (b9_fill, "B9: the threshold-th owner row fills the bits - one pass over the first input - and is itself "
	      "checked (a NULL key: dropped); after it no first-input key is dropped (2,000 rows, both entries) and absent "
	      "keys mostly are (2,000; 4,001 checked rows in all, under the sample), and the first input is not read "
	      "again", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::mt19937 rng (59);
  std::vector<int> keys;
  std::unordered_set<int> member;
  while (keys.size () < 3000)
    {
      int k = (int) rng ();
      if (member.insert (k).second)
	{
	  keys.push_back (k);
	}
    }
  std::vector<int> absent;
  while (absent.size () < 3499)
    {
      int k = (int) rng ();
      if (!member.count (k))
	{
	  absent.push_back (k);
	}
    }
  join j = int_join_with (keys);
  fake_list *first = fake_list_of (j.first ()->list_id);
  attach_second (j);
  REQUIRE (view (j.second ()).threshold == 1500);

  /* rows 1 .. 1,499: absent keys, through unchecked */
  int through = 0;
  for (int i = 0; i < 1499; i++)
    {
      through += !owner_entry (j, ints ({ absent[i] }), i % 2);
    }
  CHECK (through == 1499);
  CHECK (first->open_calls == 0);
  CHECK (view (j.second ()).state == C ().waiting);

  /* row 1,500: filled, then checked */
  CHECK (owner_entry (j, null_row (), 0));
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.state == C ().ready);
  CHECK (first->open_calls == 1);
  CHECK (v.keys == 3000);
  CHECK (v.bits != NULL);
  CHECK (v.seen == 1500);
  CHECK (v.tested == 1);
  CHECK (v.skipped == 1);

  int dropped = 0, skipped_absent = 0;
  for (int i = 0; i < 2000; i++)
    {
      dropped += owner_entry (j, ints ({ keys[i] }), i % 2);
      skipped_absent += owner_entry (j, ints ({ absent[1499 + i] }), (i + 1) % 2);
    }
  v = view (j.second ());
  WARN ("after the fill: first-input keys dropped " << dropped << "/2000, absent keys dropped " << skipped_absent
	<< "/2000");
  CHECK (dropped == 0);
  CHECK (skipped_absent > 1900);
  CHECK (v.tested == 4001);
  CHECK (v.skipped == 1 + skipped_absent);
  CHECK (v.seen == 5500);
  CHECK (v.state == C ().ready);
  CHECK (first->open_calls == 1);
  hjoin_bloom_release (owner_thread (), j.second ());
}

V4_TEST_CASE (b9_sample, "B9: the first 4,096 checked rows decide - 2,047 dropped (under 50%) turns the filter off "
	      "(OFF_LOW_SKIP) at the 4,096th, not before; the rows after go through unchecked and uncounted in tested; "
	      "2,048 dropped (50%) keeps it on, and it is not judged again (3,000 more kept rows, still checked)",
	      "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 3000);
  /* NULL keys are always dropped and first-input keys never: the sample's drop count is exact */
  for (int nulls : { 2047, 2048 })
    {
      INFO ("NULL rows in the sample: " << nulls);
      join j = int_join_with (keys);
      attach_second (j);
      unitseam_bloom_fill_now (owner_thread (), j.second ());
      REQUIRE (view (j.second ()).state == C ().ready);
      int kept = 0, dropped = 0;
      for (int i = 0; i < 4096 - nulls; i++)
	{
	  kept += decide (j, ints ({ keys[i % 3000] })) == 0;
	}
      for (int i = 0; i < nulls - 1; i++)
	{
	  dropped += decide (j, null_row ()) == 1;
	}
      CHECK (kept == 4096 - nulls);
      CHECK (dropped == nulls - 1);
      unitseam_bloom_view v = view (j.second ());
      CHECK (v.tested == 4095);
      CHECK (v.state == C ().ready);	/* not judged before the 4,096th */
      CHECK (decide (j, null_row ()) == 1);	/* the 4,096th: dropped, then judged */
      v = view (j.second ());
      CHECK (v.tested == 4096);
      CHECK (v.skipped == nulls);
      if (nulls == 2047)
	{
	  CHECK (v.state == C ().off);
	  CHECK (v.off_reason == C ().out_off_low_skip);
	  int through = 0;
	  for (int i = 0; i < 100; i++)
	    {
	      through += decide (j, null_row ()) == -1;
	    }
	  CHECK (through == 100);
	  v = view (j.second ());
	  CHECK (v.tested == 4096);
	  CHECK (v.skipped == 2047);
	  CHECK (v.seen == 4196);
	  detach_second (j);
	  unitseam_stats_view s = stats (j);
	  CHECK (s.outcome == C ().out_off_low_skip);
	  CHECK (s.tested == 4096);
	  CHECK (s.skipped == 2047);
	}
      else
	{
	  CHECK (v.state == C ().ready);
	  int kept2 = 0;
	  for (int i = 0; i < 3000; i++)
	    {
	      kept2 += decide (j, ints ({ keys[i] })) == 0;
	    }
	  CHECK (kept2 == 3000);
	  CHECK (decide (j, null_row ()) == 1);
	  v = view (j.second ());
	  CHECK (v.state == C ().ready);
	  CHECK (v.tested == 7097);
	  CHECK (v.skipped == 2049);
	  hjoin_bloom_release (owner_thread (), j.second ());
	}
    }
}

/* ================================================================ B10: hjoin_bloom_poll */

V4_TEST_CASE (b10_poll, "B10: hjoin_bloom_poll fills only on the owner thread, on a filter set up, waiting, whose seen "
	      "has reached the threshold - nothing before set-up (even past the threshold), at seen 0 or 1,023 of 1,024, "
	      "while a worker's 255 rows are still its own, on another thread, with NULL, once ready, once off; a failed "
	      "fill is not retried and prepare then gives NULL", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);
  join j = int_join_with (keys);
  fake_list *first = fake_list_of (j.first ()->list_id);
  if (C ().concurrent)
    {
      j.first ()->status = XASL_INITIALIZED;	/* 8ad5ee2df on: the first input still running at the attach */
    }
  attach_second (j);
  HJOIN_BLOOM *raw = j.second ()->hj_bloom;
  REQUIRE (raw != NULL);
  REQUIRE (view (j.second ()).threshold == 1024);

  /* not set up yet (8ad5ee2df on: the first input not done yet): nothing, even with seen past the threshold (a state
   * injection) */
  unitseam_bloom_set_seen (j.second (), 5000);
  hjoin_bloom_poll (owner_thread (), raw);
  CHECK_FALSE (view (j.second ()).set_up);
  CHECK (view (j.second ()).state == C ().waiting);
  CHECK (first->open_calls == 0);
  unitseam_bloom_set_seen (j.second (), 0);
  if (C ().concurrent)
    {
      CHECK (view (j.second ()).first_done == 0);
      j.first ()->status = XASL_SUCCESS;
      first_input_end (j);
      REQUIRE (view (j.second ()).first_done == 1);
      REQUIRE (view (j.second ()).threshold == 1024);
    }

  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b == raw);
  hjoin_bloom_poll (owner_thread (), NULL);
  hjoin_bloom_poll (owner_thread (), b);	/* seen 0 */
  CHECK (first->open_calls == 0);

  /* 1,023 of 1,024: the owner's rows, each counted at once */
  for (int i = 0; i < 1023; i++)
    {
      REQUIRE (decide (j, null_row ()) == -1);
    }
  REQUIRE (view (j.second ()).seen == 1023);
  hjoin_bloom_poll (owner_thread (), b);
  CHECK (view (j.second ()).state == C ().waiting);
  CHECK (first->open_calls == 0);

  /* a worker's rows count in batches: 255 still its own, the 256th hands them over -> 1,279 */
  HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
  REQUIRE (w != NULL);
  QFILE_LIST_ID *wl = worker_list (j);
  std::vector<DB_VALUE> nul = null_row ();
  for (int i = 0; i < 255; i++)
    {
      REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, nul, false));
    }
  hjoin_bloom_poll (owner_thread (), b);
  CHECK (view (j.second ()).seen == 1023);
  CHECK (first->open_calls == 0);
  REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, nul, false));
  CHECK (view (j.second ()).seen == 1279);

  /* not the owner thread: nothing */
  hjoin_bloom_poll (other_thread (), b);
  hjoin_bloom_poll (worker_thread (0), b);
  CHECK (view (j.second ()).state == C ().waiting);
  CHECK (first->open_calls == 0);

  /* the owner: filled, once */
  hjoin_bloom_poll (owner_thread (), b);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.state == C ().ready);
  CHECK (v.keys == 1000);
  CHECK (first->open_calls == 1);
  hjoin_bloom_poll (owner_thread (), b);
  CHECK (first->open_calls == 1);
  CHECK (view (j.second ()).bits == v.bits);
  CHECK (view (j.second ()).keys == 1000);

  /* off (the owner's sample: 4,096 kept rows): nothing either */
  for (int i = 0; i < 4096; i++)
    {
      REQUIRE (decide (j, ints ({ keys[i % 1000] })) == 0);
    }
  REQUIRE (view (j.second ()).state == C ().off);
  hjoin_bloom_poll (owner_thread (), b);
  CHECK (view (j.second ()).state == C ().off);
  CHECK (view (j.second ()).off_reason == C ().out_off_low_skip);
  CHECK (first->open_calls == 1);
  hjoin_bloom_worker_end (worker_thread (0), w);
  hjoin_bloom_release (owner_thread (), j.second ());

  /* a fill that fails from poll: off, not retried, prepare then gives NULL */
  {
    join f = int_join_with (keys);
    fake_list *ff = fake_list_of (f.first ()->list_id);
    ff->fail_open = true;
    attach_second (f);
    HJOIN_BLOOM *fb = hjoin_bloom_prepare (owner_thread (), f.second ());
    REQUIRE (fb != NULL);
    unitseam_bloom_set_seen (f.second (), 1024);
    hjoin_bloom_poll (owner_thread (), fb);
    unitseam_bloom_view fv = view (f.second ());
    CHECK (fv.state == C ().off);
    CHECK (fv.off_reason == C ().out_off_failed);
    CHECK (ff->open_calls == 1);
    CHECK (er_errid () == NO_ERROR);
    hjoin_bloom_poll (owner_thread (), fb);
    CHECK (ff->open_calls == 1);
    CHECK (hjoin_bloom_prepare (owner_thread (), f.second ()) == NULL);
    hjoin_bloom_release (owner_thread (), f.second ());
  }

  /* observation, not a contract: poll goes by seen alone, so it fills when the last batches arrive at worker_end -
   * after every row is written (px_scan's reader polls once more after the last worker reports done). 2,200 first
   * rows: threshold 1,100; two workers x 600 rows hand over 1,024 while running, 1,200 at end */
  {
    join o = int_join_with (int_range (1, 2200));
    fake_list *of = fake_list_of (o.first ()->list_id);
    attach_second (o);
    HJOIN_BLOOM *ob = hjoin_bloom_prepare (owner_thread (), o.second ());
    REQUIRE (ob != NULL);
    HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), ob);
    HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), ob);
    QFILE_LIST_ID *l1 = worker_list (o), *l2 = worker_list (o);
    for (int i = 0; i < 600; i++)
      {
	worker_skip (w1, worker_thread (0), l1, nul, false);
	worker_skip (w2, worker_thread (1), l2, nul, false);
      }
    hjoin_bloom_poll (owner_thread (), ob);
    long long seen_running = view (o.second ()).seen;
    int fills_running = of->open_calls;
    hjoin_bloom_worker_end (worker_thread (0), w1);
    hjoin_bloom_worker_end (worker_thread (1), w2);
    hjoin_bloom_poll (owner_thread (), ob);
    unitseam_bloom_view ov = view (o.second ());
    WARN ("observation: two workers x 600 rows (threshold " << ov.threshold << ") - seen while running "
	  << seen_running << ", fills " << fills_running << "; after both ended seen " << ov.seen << ", poll -> state "
	  << ov.state << " (ready " << C ().ready << "), first input read " << of->open_calls
	  << " time(s), rows checked " << ov.tested
	  << " - a fill with no row left to check");
    hjoin_bloom_release (owner_thread (), o.second ());
  }
}

/* ================================================================ B11: parallel scan workers */

V4_TEST_CASE (b11_workers, "B11: a worker lets its rows through unchecked until it sees the filter ready - it hands "
	      "its row count over every 256 rows and reads the state then (and at begin); a worker that begins after the fill "
	      "checks from its first row; a worker whose own 4,096-row sample drops under 50% turns the filter off for all "
	      "- the owner at once, every worker (itself too) from its next batch; worker_end hands over the rest of the "
	      "count; detach reports the owner's and every worker's rows", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);
  join j = int_join_with (keys);
  attach_second (j);
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  REQUIRE (view (j.second ()).threshold == 1024);
  REQUIRE (C ().seen_batch == 256);
  HJOIN_BLOOM_WORKER *w1 = wbegin (worker_thread (0), b);
  HJOIN_BLOOM_WORKER *w2 = wbegin (worker_thread (1), b);
  REQUIRE (w1 != NULL);
  REQUIRE (w2 != NULL);
  CHECK_FALSE (wview (w1).ready);
  CHECK_FALSE (wview (w2).ready);
  QFILE_LIST_ID *l1 = worker_list (j), *l2 = worker_list (j), *l3 = worker_list (j);
  std::vector<DB_VALUE> nul = null_row ();
  long long rows0 = 0, rows1 = 0, rows2 = 0, rows3 = 0;
  /* one row through worker k, alternating the descriptor and the tuple path */
  auto feed = [&] (HJOIN_BLOOM_WORKER * w, int k, QFILE_LIST_ID * l, std::vector<DB_VALUE> row, long long &rows)
  {
    rows++;
    return worker_skip (w, worker_thread (k), l, row, rows % 2 == 0);
  };

  /* waiting: 255 rows through unchecked (NULL keys: dropped if checked), nothing handed over */
  int through = 0;
  for (int i = 0; i < 255; i++)
    {
      through += !feed (w1, 0, l1, nul, rows1);
    }
  CHECK (through == 255);
  CHECK (wview (w1).seen_unflushed == 255);
  CHECK (wview (w1).tested == 0);
  CHECK (view (j.second ()).seen == 0);
  /* the 256th: the batch handed over; still waiting, so still unchecked */
  CHECK_FALSE (feed (w1, 0, l1, nul, rows1));
  CHECK (wview (w1).seen_unflushed == 0);
  CHECK (view (j.second ()).seen == 256);
  CHECK_FALSE (wview (w1).ready);

  /* w2: three batches -> seen 1,024, the threshold */
  for (int i = 0; i < 768; i++)
    {
      REQUIRE_FALSE (feed (w2, 1, l2, nul, rows2));
    }
  CHECK (view (j.second ()).seen == 1024);
  CHECK (wview (w2).tested == 0);

  /* the owner polls: filled */
  hjoin_bloom_poll (owner_thread (), b);
  REQUIRE (view (j.second ()).state == C ().ready);

  /* w1 learns it at its next batch: 255 rows still through unchecked, the 256th checked (dropped) */
  through = 0;
  for (int i = 0; i < 255; i++)
    {
      through += !feed (w1, 0, l1, nul, rows1);
    }
  CHECK (through == 255);
  CHECK (wview (w1).tested == 0);
  CHECK (feed (w1, 0, l1, nul, rows1));
  CHECK (wview (w1).ready);
  CHECK (wview (w1).tested == 1);
  CHECK (wview (w1).skipped == 1);
  CHECK (view (j.second ()).seen == 1280);

  /* a worker that begins now checks from its first row */
  HJOIN_BLOOM_WORKER *w3 = wbegin (worker_thread (2), b);
  REQUIRE (w3 != NULL);
  CHECK (wview (w3).ready);
  CHECK (feed (w3, 2, l3, nul, rows3));
  long long t3 = wview (w3).tested, s3 = wview (w3).skipped;
  CHECK (t3 == 1);
  hjoin_bloom_worker_end (worker_thread (2), w3);
  CHECK (view (j.second ()).seen == 1281);

  /* w2 to its next batch (first-input keys: kept whether checked or not), then 100 checked rows */
  for (int i = 0; i < 256; i++)
    {
      REQUIRE_FALSE (feed (w2, 1, l2, ints ({ keys[i] }), rows2));
    }
  CHECK (wview (w2).ready);
  CHECK (wview (w2).tested == 1);
  for (int i = 0; i < 100; i++)
    {
      REQUIRE_FALSE (feed (w2, 1, l2, ints ({ keys[i] }), rows2));
    }
  CHECK (wview (w2).tested == 101);
  long long u2 = wview (w2).seen_unflushed;
  CHECK (u2 == 100);

  /* the owner checks its own rows too (the shared tested already holds w3's, added at its end) */
  CHECK (view (j.second ()).tested == t3);
  rows0++;
  CHECK (decide (j, nul) == 1);
  rows0++;
  CHECK (decide (j, ints ({ 5 })) == 0);
  long long owner_tested = view (j.second ()).tested - t3;
  CHECK (owner_tested == 2);

  /* w1's own sample: first-input keys only (1 of 4,096 dropped) - off at its 4,096th checked row, not before */
  for (int i = 0; i < 4094; i++)
    {
      REQUIRE_FALSE (feed (w1, 0, l1, ints ({ keys[i % 1000] }), rows1));
    }
  CHECK (wview (w1).tested == 4095);
  CHECK (view (j.second ()).state == C ().ready);
  REQUIRE_FALSE (feed (w1, 0, l1, ints ({ keys[0] }), rows1));
  CHECK (wview (w1).tested == 4096);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.state == C ().off);
  CHECK (v.off_reason == C ().out_off_low_skip);

  /* the owner: at once */
  rows0++;
  CHECK (decide (j, nul) == -1);
  CHECK (view (j.second ()).tested == owner_tested + t3);

  /* w2: its cached state until its next batch - 255 - u2 more rows checked (NULL keys: dropped), then through */
  int checked2 = 0;
  for (long long i = 0; i < 255 - u2; i++)
    {
      checked2 += feed (w2, 1, l2, nul, rows2);
    }
  CHECK (checked2 == 255 - u2);
  CHECK_FALSE (feed (w2, 1, l2, nul, rows2));	/* the batch boundary: off seen */
  CHECK_FALSE (wview (w2).ready);
  through = 0;
  for (int i = 0; i < 300; i++)
    {
      through += !feed (w2, 1, l2, nul, rows2);
    }
  CHECK (through == 300);
  CHECK (wview (w2).tested == 101 + 255 - u2);

  /* w1 itself: the same */
  long long u1 = wview (w1).seen_unflushed;
  int checked1 = 0;
  for (long long i = 0; i < 255 - u1; i++)
    {
      checked1 += feed (w1, 0, l1, nul, rows1);
    }
  CHECK (checked1 == 255 - u1);
  CHECK_FALSE (feed (w1, 0, l1, nul, rows1));
  CHECK_FALSE (wview (w1).ready);

  /* end: the rest of each count handed over; detach reports all of it */
  long long t1 = wview (w1).tested, s1 = wview (w1).skipped, t2 = wview (w2).tested, s2 = wview (w2).skipped;
  hjoin_bloom_worker_end (worker_thread (0), w1);
  hjoin_bloom_worker_end (worker_thread (1), w2);
  v = view (j.second ());
  CHECK (v.seen == rows0 + rows1 + rows2 + rows3);
  CHECK (v.tested == owner_tested + t1 + t2 + t3);
  CHECK (v.skipped == 1 + s1 + s2 + s3);
  detach_second (j);
  unitseam_stats_view s = stats (j);
  CHECK (s.shown);
  CHECK (s.outcome == C ().out_off_low_skip);
  CHECK (s.seen == v.seen);
  CHECK (s.tested == v.tested);
  CHECK (s.skipped == v.skipped);
  CHECK (s.keys == 1000);
  CHECK (s.threshold == 1024);
}

/* a scan's own work on a row (a page read, the predicates) before the filter sees it: about 1.5 us. Without it a row
 * the filter lets through unchecked costs nanoseconds here, the workers end before the owner's first poll, and no fill
 * ever races them. */
static void
scan_work ()
{
  std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now () + std::chrono::nanoseconds (1500);
  while (std::chrono::steady_clock::now () < until)
    {
    }
}

V4_TEST_CASE (b11_threads, "B11: two workers on two threads and the owner polling on a third every 50 us (as px_scan's "
	      "reader does while it waits for them; each row costs a worker about 1.5 us of scan work): every worker row "
	      "counted once in seen, at most one fill per filter, published only filled - no first-input key dropped, "
	      "every dropped row one the filled filter drops, no row checked without the fill; the counts are exact sums "
	      "(20,000 first-input keys, 2 x 8,000 rows, 200 rounds)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::mt19937 rng (31337);
  std::unordered_set<int> member;
  std::vector<int> present;
  while (present.size () < 20000)
    {
      int k = (int) rng ();
      if (member.insert (k).second)
	{
	  present.push_back (k);
	}
    }
  /* 20,000 first-input keys: threshold 10,000, reached when each worker has given about 5,000 rows; the fill is a pass
   * over 20,000 keys, long enough to span several of a worker's 256-row batches */
  join j = int_join_with (present);
  fake_list *first = fake_list_of (j.first ()->list_id);
  /* each worker: 2,667 first-input keys and 5,333 absent ones, shuffled, serialized in advance (make_tuple REQUIREs -
   * Catch2 assertions stay on the test thread); fewer than 4,096 of them checked, so no worker's sample is judged */
  QFILE_LIST_ID *lists[2] = { worker_list (j), worker_list (j) };
  std::vector<std::vector<char *>> tuples (2);
  std::vector<std::vector<char>> is_present (2), ref (2);
  for (int k = 0; k < 2; k++)
    {
      std::vector<std::pair<int, bool>> rows;
      for (int i = 0; i < 2667; i++)
	{
	  rows.push_back ({ present[k * 2667 + i], true });
	}
      while (rows.size () < 8000)
	{
	  int x = (int) rng ();
	  if (!member.count (x))
	    {
	      rows.push_back ({ x, false });
	    }
	}
      std::shuffle (rows.begin (), rows.end (), rng);
      for (auto &r : rows)
	{
	  std::vector<DB_VALUE> v = ints ({ r.first });
	  tuples[k].push_back (make_tuple (lists[k], v));
	  is_present[k].push_back (r.second);
	}
    }
  /* the filled filter's decision on every row (an attach of its own, filled at once) */
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);
  for (int k = 0; k < 2; k++)
    {
      for (char *t : tuples[k])
	{
	  QFILE_TUPLE_RECORD r = bound (lists[k], t);
	  int d = unitseam_bloom_check_bound (owner_thread (), j.second (), &r);
	  REQUIRE (d >= 0);
	  ref[k].push_back (d == 1);
	}
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  const long long n = (long long) tuples[0].size ();

  const int rounds = 200;
  int no_filter = 0, begin_failed = 0, bad_seen = 0, bad_sum = 0, bad_fill = 0, bad_state = 0, unfilled_checks = 0;
  long long wrong_drop = 0, false_neg = 0, checked = 0;
  int filled_while_running = 0, filled_after_rows = 0, never_filled = 0;
  for (int round = 0; round < rounds; round++)
    {
      attach_second (j);
      HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
      if (b == NULL)
	{
	  no_filter++;
	  hjoin_bloom_release (owner_thread (), j.second ());
	  continue;
	}
      int opens_before = first->open_calls;
      std::atomic<int> active (2);
      spin_barrier start (4);
      long long tested[2] = { 0, 0 }, skipped[2] = { 0, 0 }, drops[2] = { 0, 0 }, wd[2] = { 0, 0 }, fn[2] = { 0, 0 };
      bool began[2] = { false, false };
      auto worker = [&] (int k)
      {
	THREAD_ENTRY *t = worker_thread (k);
	HJOIN_BLOOM_WORKER *w = wbegin (t, b);
	began[k] = w != NULL;
	start.wait ();
	for (long long i = 0; i < n; i++)
	  {
	    scan_work ();
	    bool sk = hjoin_bloom_worker_skip_tuple (t, w, lists[k], tuples[k][i]);
	    drops[k] += sk;
	    wd[k] += sk && !ref[k][i];
	    fn[k] += sk && is_present[k][i];
	  }
	unitseam_worker_view wv = wview (w);
	tested[k] = wv.tested;
	skipped[k] = wv.skipped;
	hjoin_bloom_worker_end (t, w);
	active.fetch_sub (1);
      };
      auto poller = [&] ()
      {
	start.wait ();
	while (active.load () != 0)
	  {
	    std::this_thread::sleep_for (std::chrono::microseconds (50));
	    hjoin_bloom_poll (owner_thread (), b);
	  }
      };
      std::thread t0 (worker, 0), t1 (worker, 1), tp (poller);
      start.wait ();
      t0.join ();
      t1.join ();
      tp.join ();

      begin_failed += !began[0] + !began[1];
      wrong_drop += wd[0] + wd[1];
      false_neg += fn[0] + fn[1];
      checked += tested[0] + tested[1];
      unitseam_bloom_view v = view (j.second ());
      bad_seen += v.seen != 2 * n;
      bad_sum += v.tested != tested[0] + tested[1];
      bad_sum += v.skipped != skipped[0] + skipped[1];
      bad_sum += drops[0] != skipped[0];
      bad_sum += drops[1] != skipped[1];
      int fills = first->open_calls - opens_before;
      if (v.state == C ().ready)
	{
	  bad_fill += fills != 1 || v.keys != 20000;
	  if (tested[0] + tested[1] > 0)
	    {
	      filled_while_running++;
	    }
	  else
	    {
	      filled_after_rows++;
	    }
	}
      else if (v.state == C ().waiting)
	{
	  bad_fill += fills != 0;
	  unfilled_checks += tested[0] + tested[1] != 0;
	  never_filled++;
	}
      else
	{
	  bad_state++;		/* off: no failure injected, and no worker reaches a 4,096-row sample */
	  WARN ("round " << round << ": off, reason " << v.off_reason << ", checked " << tested[0] << " + " << tested[1]);
	}
      hjoin_bloom_release (owner_thread (), j.second ());
    }
  WARN ("2 workers x " << n << " rows (first input " << present.size () << " keys) x " << rounds
	<< " rounds: filled while the workers ran " << filled_while_running
	<< ", filled after their last checked row " << filled_after_rows << ", never filled " << never_filled
	<< "; rows checked " << checked << ", dropped rows the filled filter keeps " << wrong_drop
	<< ", first-input keys dropped " << false_neg);
  CHECK (no_filter == 0);
  CHECK (begin_failed == 0);
  CHECK (bad_seen == 0);
  CHECK (bad_sum == 0);
  CHECK (bad_fill == 0);
  CHECK (bad_state == 0);
  CHECK (unfilled_checks == 0);
  CHECK (wrong_drop == 0);
  CHECK (false_neg == 0);
  CHECK (filled_while_running > 0);	/* else no round had the fill race the workers: the round proves nothing */
  for (auto &v : tuples)
    {
      for (char *t : v)
	{
	  free (t);
	}
    }
}

/* ================================================================ B12: detach's report, failures */

V4_TEST_CASE (b12_detach, "B12: detach reports what the filter did and frees it - attached, the outcome (NOT_BUILT: "
	      "seen under the threshold; CHECKED; OFF_LOW_SKIP; OFF_FAILED), the threshold, seen (the owner's rows and "
	      "every worker's, the rest handed over at end), keys, tested, skipped; a clone's detach reports nothing",
	      "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);

  /* NOT_BUILT: 10 owner rows and a worker's 300 (256 handed over at its batch, 44 at end) */
  {
    join j = int_join_with (keys);
    attach_second (j);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    REQUIRE (b != NULL);
    for (int i = 0; i < 10; i++)
      {
	REQUIRE (decide (j, null_row ()) == -1);
      }
    HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
    QFILE_LIST_ID *wl = worker_list (j);
    std::vector<DB_VALUE> nul = null_row ();
    for (int i = 0; i < 300; i++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, nul, i % 2 == 0));
      }
    CHECK (view (j.second ()).seen == 266);
    hjoin_bloom_worker_end (worker_thread (0), w);
    CHECK (view (j.second ()).seen == 310);
    /* a clone's detach: nothing reported, nothing freed */
    XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
    memcpy (clone, j.second (), sizeof (XASL_NODE));
    HASHJOIN_BLOOM_STATS snap;
    memcpy (&snap, &j.hj->proc.hashjoin.bloom_stats, sizeof (snap));
    detach_node (clone);
    CHECK (memcmp (&snap, &j.hj->proc.hashjoin.bloom_stats, sizeof (snap)) == 0);
    CHECK (is_live (b));
    free (clone);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_not_built);
    CHECK (s.keys == 0);
    CHECK (s.threshold == 1024);
    CHECK (s.seen == 310);
    CHECK (s.tested == 0);
    CHECK (s.skipped == 0);
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK_FALSE (is_live (b));
  }

  /* CHECKED: the threshold row fills; 5 kept and 4 dropped rows checked in all */
  {
    join j = int_join_with (keys);
    attach_second (j);
    owner_rows_before_fill (j);
    CHECK (decide (j, null_row ()) == 1);
    for (int i = 0; i < 5; i++)
      {
	CHECK (decide (j, ints ({ keys[i] })) == 0);
      }
    for (int i = 0; i < 3; i++)
      {
	CHECK (decide (j, null_row ()) == 1);
      }
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_checked);
    CHECK (s.keys == 1000);
    CHECK (s.threshold == 1024);
    CHECK (s.seen == 1032);
    CHECK (s.tested == 9);
    CHECK (s.skipped == 4);
  }

  /* OFF_LOW_SKIP: 4,096 kept rows */
  {
    join j = int_join_with (keys);
    attach_second (j);
    unitseam_bloom_fill_now (owner_thread (), j.second ());
    for (int i = 0; i < 4096; i++)
      {
	REQUIRE (decide (j, ints ({ keys[i % 1000] })) == 0);
      }
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_off_low_skip);
    CHECK (s.keys == 1000);
    CHECK (s.seen == 4096);
    CHECK (s.tested == 4096);
    CHECK (s.skipped == 0);
  }

  /* OFF_FAILED: the fill cannot open the first input */
  {
    join j = int_join_with (keys);
    fake_list_of (j.first ()->list_id)->fail_open = true;
    attach_second (j);
    owner_rows_before_fill (j);
    CHECK (decide (j, null_row ()) == -1);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_off_failed);
    CHECK (s.keys == 0);
    CHECK (s.seen == 1024);
    CHECK (s.tested == 0);
  }

  /* observation, not a contract: a filter still waiting whose seen reached the threshold (the workers' last batches
   * came after the reader's last poll, or the reader found them all done and did not poll) is reported NOT_BUILT -
   * "second input gave N rows, threshold T" with N >= T */
  {
    join j = int_join_with (keys);
    attach_second (j);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
    QFILE_LIST_ID *wl = worker_list (j);
    std::vector<DB_VALUE> nul = null_row ();
    for (int i = 0; i < 1100; i++)
      {
	worker_skip (w, worker_thread (0), wl, nul, false);
      }
    hjoin_bloom_worker_end (worker_thread (0), w);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    WARN ("observation: waiting at detach with seen " << s.seen << " >= threshold " << s.threshold << " -> outcome "
	  << s.outcome << " (NOT_BUILT " << C ().out_not_built << ")");
  }
}

/* how many allocations the threshold row's fill makes (counted on a scene of its own: the fault knob counts down every
 * allocation and reallocation; a fill that succeeds with the n-th armed made fewer than n) */
static int
fill_allocations (const std::vector<int> &keys)
{
  for (int n = 1; n <= 64; n++)
    {
      join j = int_join_with (keys);
      attach_second (j);
      owner_rows_before_fill (j);
      fail_nth_alloc (n);
      (void) decide (j, ints ({ 5 }));
      bool spent = g_alloc_countdown.load () == 0;
      fail_nth_alloc (0);
      er_clear ();
      bool ready = view (j.second ()).state == C ().ready;
      hjoin_bloom_release (owner_thread (), j.second ());
      if (!spent && ready)
	{
	  return n - 1;
	}
    }
  return -1;
}

V4_TEST_CASE (b12_failures, "B12: a filter that cannot be set up or filled turns off (OFF_FAILED) and lets every row "
	      "through, the error cleared, the first input not read again, prepare NULL from then on - set-up: the second "
	      "list not open (at the first row), an allocation fails; fill: the first list does not open, its scan stops "
	      "on an error with or without an error id, the bit array allocation fails; a check that fails once ready "
	      "(the owner's row buffer) turns it off too, and the first reason is kept (a worker's low sample right "
	      "after does not overwrite it)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);

  /* set-up: the second list is not open (type_cnt 0) - at the owner's first row (8ad5ee2df on: at the threshold row,
   * whose fill sets up) */
  {
    join j = int_join_with (keys);
    attach_second (j);
    long long before = C ().concurrent ? owner_rows_before_fill (j) : 0;
    QFILE_LIST_ID *sl = j.second ()->list_id;
    std::vector<DB_VALUE> row = ints ({ 5 });
    char *t = make_tuple (sl, row);
    int saved = sl->type_list.type_cnt;
    sl->type_list.type_cnt = 0;
    CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), j.second (), t));
    sl->type_list.type_cnt = saved;
    free (t);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.set_up);
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_off_failed);
    CHECK (v.seen == before + 1);
    CHECK (er_errid () == NO_ERROR);
    CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == NULL);
    int through = 0;
    for (int i = 0; i < 2000; i++)
      {
	through += decide (j, null_row ()) == -1;
      }
    CHECK (through == 2000);	/* past the threshold too: nothing filled */
    CHECK (fake_list_of (j.first ()->list_id)->open_calls == 0);
    detach_second (j);
    CHECK (stats (j).outcome == C ().out_off_failed);
    CHECK (stats (j).seen == before + 2001);
  }

  /* set-up: an allocation fails (domains or the owner's key) - at the first row (8ad5ee2df on: the threshold row's fill) */
  {
    join j = int_join_with (keys);
    attach_second (j);
    if (C ().concurrent)
      {
	owner_rows_before_fill (j);
      }
    fail_nth_alloc (1);
    int d = decide (j, ints ({ 5 }));
    int left = g_alloc_countdown.load ();
    fail_nth_alloc (0);
    CHECK (left == 0);
    CHECK (d == -1);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_off_failed);
    CHECK (er_errid () == NO_ERROR);
    CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == NULL);
    hjoin_bloom_release (owner_thread (), j.second ());
  }

  /* fill: at the threshold row */
  const char *what[] = { "the first list does not open", "its scan stops on S_ERROR with an error id",
    "its scan stops on S_ERROR without an error id", "the bit array allocation fails"
  };
  for (int mode = 0; mode < 4; mode++)
    {
      INFO (what[mode]);
      join j = int_join_with (keys);
      fake_list *fl = fake_list_of (j.first ()->list_id);
      if (mode == 0)
	{
	  fl->fail_open = true;
	}
      else if (mode == 1 || mode == 2)
	{
	  fl->stop_at = 3;
	  fl->stop_sets_error = mode == 1;
	}
      attach_second (j);
      owner_rows_before_fill (j);
      if (mode == 3)
	{
	  /* 8ad5ee2df on the fill sets up first: its first allocations are the set-up's - the bit array's is the last
	   * one before the scan (B12 'every allocation of the fill' below fails each of them) */
	  fail_nth_alloc (C ().concurrent ? fill_allocations (keys) : 1);
	}
      int d = decide (j, ints ({ 5 }));	/* the threshold row */
      int left = g_alloc_countdown.load ();
      fail_nth_alloc (0);
      CHECK (d == -1);
      CHECK (left == 0);
      unitseam_bloom_view v = view (j.second ());
      CHECK (v.state == C ().off);
      CHECK (v.off_reason == C ().out_off_failed);
      CHECK (er_errid () == NO_ERROR);
      int calls = fl->open_calls;
      CHECK (calls == (mode == 3 ? 0 : 1));
      int through = 0;
      for (int i = 0; i < 100; i++)
	{
	  through += decide (j, null_row ()) == -1;
	}
      CHECK (through == 100);
      CHECK (fl->open_calls == calls);	/* not retried */
      CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == NULL);
      detach_second (j);
      CHECK (stats (j).outcome == C ().out_off_failed);
    }

  /* 8ad5ee2df on (the set-up is part of the fill): every allocation the fill makes, failed in turn, turns the filter
   * off (OFF_FAILED) before the first input is read, the error cleared, the rows through, not retried */
  if (C ().concurrent)
    {
      int n_alloc = fill_allocations (keys);
      INFO ("the fill makes " << n_alloc << " allocations");
      CHECK (n_alloc >= 2);	/* the set-up's (domains / key) and the bit array */
      for (int n = 1; n <= n_alloc; n++)
	{
	  INFO ("allocation " << n << " of the fill fails");
	  join j = int_join_with (keys);
	  fake_list *fl = fake_list_of (j.first ()->list_id);
	  attach_second (j);
	  owner_rows_before_fill (j);
	  fail_nth_alloc (n);
	  int d = decide (j, ints ({ 5 }));
	  int left = g_alloc_countdown.load ();
	  fail_nth_alloc (0);
	  CHECK (left == 0);
	  CHECK (d == -1);
	  unitseam_bloom_view v = view (j.second ());
	  CHECK (v.state == C ().off);
	  CHECK (v.off_reason == C ().out_off_failed);
	  CHECK (er_errid () == NO_ERROR);
	  CHECK (fl->open_calls == 0);
	  CHECK (decide (j, null_row ()) == -1);
	  CHECK (fl->open_calls == 0);
	  hjoin_bloom_release (owner_thread (), j.second ());
	}
    }

  /* observation, not a contract: the fill reads any pending error id as its own failure - an error id an earlier row's
   * evaluation left set (e.g. numeric_opfunc.c sets ER_IT_DATA_OVERFLOW at warning severity) turns the filter off at
   * the threshold row and is cleared with it; 1a7bccdcc built at the first row, so only an id pending then did so */
  {
    join j = int_join_with (keys);
    attach_second (j);
    owner_rows_before_fill (j);
    er_set (ER_WARNING_SEVERITY, __FILE__, __LINE__, ER_IT_DATA_OVERFLOW, 1, "INTEGER");
    int d = decide (j, ints ({ 5 }));
    unitseam_bloom_view v = view (j.second ());
    WARN ("observation: error id " << ER_IT_DATA_OVERFLOW << " pending at the threshold row -> row " << d
	  << ", state " << v.state << " (off " << C ().off << "), reason " << v.off_reason << " (failed "
	  << C ().out_off_failed << "), error id after " << er_errid ());
    er_clear ();
    hjoin_bloom_release (owner_thread (), j.second ());
  }

  /* a check that fails once ready - the owner's row buffer cannot grow - and the first reason kept: a worker whose
   * cached state still says ready reaches its 4,096th checked row right after, with nothing dropped */
  {
    join j = int_join_with (keys);
    attach_second (j);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    REQUIRE (b != NULL);
    HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
    REQUIRE (w != NULL);
    QFILE_LIST_ID *wl = worker_list (j);
    std::vector<DB_VALUE> kept = ints ({ 7 });
    for (int i = 0; i < 10; i++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, kept, false));
      }
    unitseam_bloom_fill_now (owner_thread (), j.second ());	/* what hjoin_bloom_poll does at the threshold */
    REQUIRE (view (j.second ()).state == C ().ready);
    for (int i = 0; i < 246; i++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, kept, false));
      }
    REQUIRE (wview (w).tested == 1);
    for (int i = 0; i < 4094; i++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, kept, false));
      }
    REQUIRE (wview (w).tested == 4095);
    REQUIRE (wview (w).seen_unflushed == 254);
    /* the owner's first descriptor row: its row buffer cannot grow */
    std::vector<DB_VALUE> row = ints ({ 7 });
    stage_descr (j.second ()->list_id, row);
    fail_nth_alloc (1);
    bool s = hjoin_bloom_skip_descr (owner_thread (), j.second ());
    int left = g_alloc_countdown.load ();
    fail_nth_alloc (0);
    CHECK (left == 0);
    CHECK_FALSE (s);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_off_failed);
    CHECK (er_errid () == NO_ERROR);
    /* the worker's 4,096th checked row: its sample (none dropped) tries to turn the filter off - already off */
    CHECK_FALSE (worker_skip (w, worker_thread (0), wl, kept, false));
    CHECK (wview (w).tested == 4096);
    v = view (j.second ());
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_off_failed);
    /* its next row is a batch boundary: off seen, through */
    CHECK_FALSE (worker_skip (w, worker_thread (0), wl, kept, false));
    CHECK_FALSE (wview (w).ready);
    hjoin_bloom_worker_end (worker_thread (0), w);
    detach_second (j);
    CHECK (stats (j).outcome == C ().out_off_failed);
  }
}

/* ================================================================ v5 (8ad5ee2df, 3726fd95d) */

/*
 * 8ad5ee2df attaches the filter to both inputs before they run and lets them run at once: the first input's end
 * publishes its row count (hjoin_bloom_input_done: first_done, a release store after first_rows and threshold), the
 * owner - bound at its first use - fills only once that is published and the second input has given threshold rows,
 * the owner's input_done frees what the owner allocated, finish reports and frees the filter on the hash join's thread.
 * 3726fd95d checks a row before its projection from its key columns' values when every key column is a plain
 * reference (hjoin_bloom_skip_row: attach's key_regus; workers: worker_begin (outptr_list) / worker_skip_row), and fills
 * in an error stack frame. B15 is registered where the 8ad5ee2df API links, B13, B14 and B16 where the 3726fd95d one
 * does; on an earlier revision they are absent (not applicable).
 */
static bool
v5_concurrent_linked ()
{
  return API ().attach2 && API ().input_done && API ().finish;
}

static bool
v5_early_linked ()
{
  return v5_concurrent_linked () && API ().skip_row && API ().wskip_row && API ().wbegin3;
}

#define V5_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = v5_concurrent_linked () \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

#define V5E_TEST_CASE(fn, name, tags) \
  static void fn (); \
  static const bool fn##_registered __attribute__ ((unused)) = v5_early_linked () \
    && (Catch::AutoReg (Catch::makeTestInvoker (&fn), CATCH_INTERNAL_LINEINFO, Catch::StringRef (), \
			Catch::NameAndTags { name, tags }), true); \
  static void fn ()

/* registered on every revision: a partial API (a rename) would silently drop B13-B16 */
TEST_CASE ("B13-B16 guard: the 8ad5ee2df API (attach (hj) / input_done / finish) links exactly where the struct has "
	   "first_done and the earlier attach (hj, second) / detach exactly where it has not; the 3726fd95d API "
	   "(skip_row / worker_begin (outptr_list) / worker_skip_row) exactly where it has key_regus - B15, and B13 / B14 "
	   "/ B16, are registered only there", "[contract]")
{
  WARN ("concurrent (8ad5ee2df) " << (C ().concurrent ? "yes" : "no") << ", early check (3726fd95d) "
	<< (C ().early ? "yes" : "no") << " - B15 " << (v5_concurrent_linked ()? "registered" : "not applicable")
	<< ", B13 / B14 / B16 " << (v5_early_linked ()? "registered" : "not applicable"));
  CHECK (v5_concurrent_linked () == C ().concurrent);
  CHECK ((API ().attach2 || API ().input_done || API ().finish) == C ().concurrent);
  CHECK (API ().attach3 == API ().detach);
  CHECK (API ().attach3 == !C ().concurrent);
  CHECK (v5_early_linked () == C ().early);
  CHECK ((API ().skip_row || API ().wskip_row || API ().wbegin3) == C ().early);
  if (C ().early)
    {
      CHECK (C ().concurrent);
      CHECK (C ().out_ended_first >= 0);
    }
  if (C ().concurrent)
    {
      CHECK (C ().out_first_late >= 0);
      CHECK (C ().out_off_first_rows >= 0);
      CHECK_FALSE (API ().wants_serial);
    }
}

/* ---------------------------------------------------------------- a second input that projects its rows */

enum col_kind
{
  COL_CONSTANT,			/* TYPE_CONSTANT over a value the scan holds, REGU_VARIABLE_FAST_PEEK (stx_set_fast_peek) */
  COL_CONSTANT_SLOW,		/* the same without the flag: fetch_peek_dbval_slow (fetch.c) */
  COL_ATTR,			/* TYPE_ATTR_ID with its cached value pointer, fast peek */
  COL_DBVAL,			/* TYPE_DBVAL: a literal */
  COL_POSITION,			/* TYPE_POSITION: a list column */
  COL_ARITH,			/* TYPE_INARITH: computed (never fetched here) */
  COL_POS_VALUE			/* TYPE_POS_VALUE: a host variable (never fetched here) */
};

struct pcol
{
  col_kind kind;
  TP_DOMAIN *dom;
  bool hidden;
};

/*
 * The scan's side of a second input: its output list - regu variables over values a scan holds (cells) - as the stream
 * load leaves it, with no compiled program (eval_prog_state 2: every column on the interpreted fetch - the program,
 * when there is one, casts a column the same way, in place). vd: a value descriptor without an execution state (no
 * plan item on these regus, so their domains are the compiled ones).
 */
struct projected
{
  OUTPTR_LIST *out;
  std::vector<REGU_VARIABLE *> regus;	/* every node's, hidden ones too */
  std::vector<DB_VALUE *> cells;	/* what each regu reads (TYPE_DBVAL: its own value) */
  VAL_DESCR vd;
};

static projected
make_projected (const std::vector<pcol> &cols)
{
  projected p;
  p.out = (OUTPTR_LIST *) calloc (1, sizeof (OUTPTR_LIST));
  p.out->eval_prog_state = 2;
  REGU_VARIABLE_LIST *tail = &p.out->valptrp;
  for (const pcol & c:cols)
    {
      REGU_VARIABLE_LIST node = (REGU_VARIABLE_LIST) calloc (1, sizeof (*node));
      REGU_VARIABLE *r = &node->value;
      DB_VALUE *cell = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
      db_make_null (cell);
      r->domain = c.dom;
      if (c.hidden)
	{
	  r->flags |= REGU_VARIABLE_HIDDEN_COLUMN;
	}
      switch (c.kind)
	{
	case COL_CONSTANT:
	  r->type = TYPE_CONSTANT;
	  r->value.dbvalptr = cell;
	  r->flags |= REGU_VARIABLE_FAST_PEEK;
	  break;
	case COL_CONSTANT_SLOW:
	  r->type = TYPE_CONSTANT;
	  r->value.dbvalptr = cell;
	  break;
	case COL_ATTR:
	  r->type = TYPE_ATTR_ID;
	  r->value.attr_descr.id = 0;
	  r->value.attr_descr.type = TP_DOMAIN_TYPE (c.dom);
	  r->value.attr_descr.cache_dbvalp = cell;
	  r->flags |= REGU_VARIABLE_FAST_PEEK;
	  break;
	case COL_DBVAL:
	  r->type = TYPE_DBVAL;
	  free (cell);
	  cell = &r->value.dbval;
	  db_make_null (cell);
	  r->flags |= REGU_VARIABLE_FAST_PEEK;
	  break;
	case COL_POSITION:
	  r->type = TYPE_POSITION;
	  r->value.pos_descr.pos_no = 0;
	  r->value.pos_descr.dom = c.dom;
	  r->vfetch_to = cell;
	  break;
	case COL_ARITH:
	  r->type = TYPE_INARITH;
	  r->value.arithptr = (ARITH_TYPE *) calloc (1, sizeof (ARITH_TYPE));
	  break;
	case COL_POS_VALUE:
	  r->type = TYPE_POS_VALUE;
	  r->value.val_pos = 0;
	  break;
	}
      p.regus.push_back (r);
      p.cells.push_back (cell);
      *tail = node;
      tail = &node->next;
      p.out->valptr_cnt++;
    }
  memset (&p.vd, 0, sizeof (p.vd));
  return p;
}

/* the list the input writes: opened with the plan's domains (qdata_get_valptr_type_list - hidden columns left out) */
static std::vector<TP_DOMAIN *>
list_domains (projected & p)
{
  QFILE_TUPLE_VALUE_TYPE_LIST tl;
  memset (&tl, 0, sizeof (tl));
  REQUIRE (qdata_get_valptr_type_list (owner_thread (), p.out, &tl, &p.vd) == NO_ERROR);
  std::vector<TP_DOMAIN *> d (tl.domp, tl.domp + tl.type_cnt);
  db_private_free (owner_thread (), tl.domp);
  return d;
}

/* an INNER join whose second input projects through p; keys (outer column, inner column) as new_join takes them */
static join
projected_join (projected & p, const std::vector<TP_DOMAIN *> &first_doms, const std::vector<std::pair<int, int>> &keys,
		bool first_is_outer = true)
{
  std::vector<TP_DOMAIN *> sd = list_domains (p);
  join j = first_is_outer ? new_join (JOIN_INNER, first_doms, sd, keys, true)
    : new_join (JOIN_INNER, sd, first_doms, keys, false);
  j.second ()->outptr_list = p.out;
  return j;
}

/*
 * The row's projection, as qexec_end_one_iteration makes it after the check: qexec_generate_tuple_descriptor's
 * qdata_generate_tuple_desc_for_valptr_list (the real one, query_opfunc.c: each column fetched with fetch_peek_dbval,
 * a value not of its column's domain cast to it - tp_value_auto_cast, in place) and qfile_generate_tuple_into_list's
 * qfile_tuple_fill_from_values. NULL when the descriptor fails (the error id left set).
 */
static char *
project_row (QFILE_LIST_ID * l, projected & p)
{
  QFILE_TUPLE_DESCRIPTOR *d = &l->tpl_descr;
  if (d->f_valp == NULL)
    {
      d->f_valp = (DB_VALUE **) calloc (l->type_list.type_cnt, sizeof (DB_VALUE *));
      d->f_len = (int *) calloc (l->type_list.type_cnt, sizeof (int));
    }
  if (qdata_generate_tuple_desc_for_valptr_list (owner_thread (), p.out, &p.vd, l) != QPROC_TPLDESCR_SUCCESS)
    {
      return nullptr;
    }
  size_t sz = (d->tpl_size + 7) / 8 * 8;
  char *t = (char *) aligned_alloc (8, sz);
  memset (t, 0, sz);
  if (qfile_tuple_fill_from_values (&l->type_list, d->f_valp, d->f_len, d->f_cnt, t, d->tpl_size, d->has_null)
      != NO_ERROR)
    {
      free (t);
      return nullptr;
    }
  return t;
}

/* ================================================================ B13 (K): the key before the projection */

struct k_value
{
  DB_VALUE v;			/* the scan's value of the key column */
  bool foreign;			/* not of the list column's domain: another type, NUMERIC precision / scale, collation */
  std::string text;
};

struct k_spec
{
  const char *name;
  TP_DOMAIN *first_dom;
  TP_DOMAIN *col_dom;
  col_kind kind;
  std::function<k_value (std::mt19937 &, int)> gen;
};

struct k_result
{
  int rows = 0, nulls = 0, foreign = 0;
  int null_mismatch = 0, hash_mismatch = 0, decision_mismatch = 0, early_drops_kept = 0;
  int cast_wrong = 0, cast_used = 0;
  int early_failed_only = 0, late_failed_only = 0, both_failed = 0;
  int public_checked = 0, public_mismatch = 0;
  int kept = 0, dropped = 0, first_keys = 0;
  bool need_coerce = false;
  std::string coerce_dom;
  std::vector<std::string> samples;
};

static std::string
value_text (const DB_VALUE * v)
{
  if (DB_IS_NULL (v))
    {
      return "NULL";
    }
  DB_VALUE s;
  db_make_null (&s);
  TP_DOMAIN *vc = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, DB_MAX_VARCHAR_PRECISION, 0, NULL, LANG_COLL_UTF8_BINARY);
  std::string out = std::string (pr_type_name (DB_VALUE_DOMAIN_TYPE (v)));
  if (DB_VALUE_DOMAIN_TYPE (v) == DB_TYPE_NUMERIC)
    {
      out += "(" + std::to_string (v->domain.numeric_info.precision) + "," + std::to_string (v->domain.numeric_info.scale)
	+ ")";
    }
  if (TP_IS_CHAR_TYPE (DB_VALUE_DOMAIN_TYPE (v)))
    {
      out += "[coll " + std::to_string (db_get_string_collation (v)) + "]";
    }
  if (tp_value_cast (v, &s, vc, false) == DOMAIN_COMPATIBLE && !DB_IS_NULL (&s))
    {
      out += " '" + std::string (db_get_string (&s), db_get_string_size (&s)) + "'";
    }
  pr_clear_value (&s);
  return out;
}

static k_result
run_k (k_spec & ks, int n)
{
  boot_types ();
  set_bloom_param (true);
  k_result r;
  /* a hidden column before the key, a payload after it: list columns (key, payload) */
  projected p = make_projected ({ {COL_CONSTANT, dom_int (), true}, {ks.kind, ks.col_dom, false},
				  {COL_CONSTANT, dom_int (), false} });
  join j = projected_join (p, { ks.first_dom }, { {0, 0} });
  QFILE_LIST_ID *sl = j.second ()->list_id;
  REQUIRE (sl->type_list.type_cnt == 2);
  REQUIRE (sl->type_list.domp[0] == ks.col_dom);

  std::mt19937 rng (std::hash < std::string > () (ks.name) & 0xffffffff);
  std::vector<k_value> vals;
  for (int i = 0; i < n; i++)
    {
      vals.push_back (ks.gen (rng, i));
    }
  /* the first input: every third value as a scan of the first's key column holds it (cast to its domain) */
  for (int i = 0; i < n; i += 3)
    {
      if (DB_IS_NULL (&vals[i].v))
	{
	  continue;
	}
      DB_VALUE fv;
      db_make_null (&fv);
      if (tp_value_cast (&vals[i].v, &fv, ks.first_dom, false) == DOMAIN_COMPATIBLE && !DB_IS_NULL (&fv))
	{
	  add_row (j.first ()->list_id, { fv });
	  r.first_keys++;
	}
      er_clear ();
    }
  first_done (j);
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  REQUIRE (unitseam_bloom_key_regu (j.second (), 0) == (const void *) p.regus[1]);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);
  r.need_coerce = j.hj->proc.hashjoin.domain_info.need_coerce_domains;
  TP_DOMAIN *cd = j.hj->proc.hashjoin.domain_info.coerce_domains[0];
  r.coerce_dom = cd ? std::string (pr_type_name (TP_DOMAIN_TYPE (cd))) + "(" + std::to_string (cd->precision) + ","
    + std::to_string (cd->scale) + ")" : "-";

  for (int i = 0; i < n; i++)
    {
      DB_VALUE *cell = p.cells[1];
      pr_clear_value (cell);
      REQUIRE (pr_clone_value (&vals[i].v, cell) == NO_ERROR);	/* a fresh copy: the projection casts it in place */
      db_make_int (p.cells[0], -i);
      db_make_int (p.cells[2], i);
      r.rows++;
      bool is_null = DB_IS_NULL (cell);
      r.nulls += is_null;
      r.foreign += vals[i].foreign;

      /* (a) before the projection: the owner's key from the regu (hjoin_bloom_fetch_key_values), its hash */
      unsigned int he = 0, hl = 0;
      int mask = 0, de = -1;
      int re = unitseam_bloom_early_key (owner_thread (), j.second (), &p.vd, &he, &mask, &de);
      REQUIRE (re != -3);
      REQUIRE (re != -4);
      if (re == -2)
	{
	  er_clear ();
	}
      if (re == 0)
	{
	  r.cast_used += (mask & 1) != 0;
	  if (((mask & 1) != 0) != vals[i].foreign)
	    {
	      r.cast_wrong++;
	      if (r.samples.size () < 6)
		{
		  r.samples.push_back ("cast " + std::string ((mask & 1) ? "used" : "not used") + " for "
				       + value_text (&vals[i].v));
		}
	    }
	}
      /* the public entry on every tenth row (under the 4,096-row sample): the same decision */
      if (i % 10 == 0 && re >= 0)
	{
	  bool s = hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
	  er_clear ();
	  r.public_checked++;
	  r.public_mismatch += re >= 0 && s != (de == 1);
	}

      /* (b) the projection, the list's tuple, the join's own read of it */
      char *t = project_row (sl, p);
      int rl = -1, dl = -9;
      if (t == nullptr)
	{
	  er_clear ();
	}
      else
	{
	  QFILE_TUPLE_RECORD rec = bound (sl, t);
	  rl = unitseam_hj_second_key (owner_thread (), j.hj, true, &rec, &hl);
	  QFILE_TUPLE_RECORD rec2 = bound (sl, t);
	  dl = unitseam_bloom_check_bound (owner_thread (), j.second (), &rec2);
	  er_clear ();
	}

      if (re == -2 && rl == -1)
	{
	  r.both_failed++;
	}
      else if (re == -2)
	{
	  r.early_failed_only++;
	  if (r.samples.size () < 6)
	    {
	      r.samples.push_back ("early fetch failed only: " + value_text (&vals[i].v));
	    }
	}
      else if (rl == -1)
	{
	  r.late_failed_only++;
	  if (r.samples.size () < 6)
	    {
	      r.samples.push_back ("projection / list read failed only: " + value_text (&vals[i].v));
	    }
	}
      else if (re != rl)
	{
	  r.null_mismatch++;
	  if (r.samples.size () < 6)
	    {
	      r.samples.push_back ("NULL disagreement (early " + std::to_string (re) + ", list " + std::to_string (rl)
				   + "): " + value_text (&vals[i].v));
	    }
	}
      else
	{
	  if (re == 0 && he != hl)
	    {
	      r.hash_mismatch++;
	      if (r.samples.size () < 6)
		{
		  char buf[64];
		  snprintf (buf, sizeof (buf), " - early 0x%08x, list 0x%08x", he, hl);
		  r.samples.push_back ("hash: " + value_text (&vals[i].v) + buf);
		}
	    }
	  if (de != dl)
	    {
	      r.decision_mismatch++;
	      r.early_drops_kept += de == 1 && dl == 0;
	    }
	  r.kept += dl == 0;
	  r.dropped += dl == 1;
	}
      free (t);
      pr_clear_value (cell);
    }
  for (auto &v : vals)
    {
      pr_clear_value (&v.v);
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  return r;
}

static void
check_k (k_spec & ks, int n = 10000, bool expect_foreign = false)
{
  k_result r = run_k (ks, n);
  std::string samples;
  for (auto &s : r.samples)
    {
      samples += "\n    " + s;
    }
  INFO (ks.name << ": coerce " << (r.need_coerce ? "yes " : "no ") << r.coerce_dom << ", rows " << r.rows << " (NULL "
	<< r.nulls << ", foreign " << r.foreign << "), first keys " << r.first_keys << ", hash mismatches "
	<< r.hash_mismatch << ", NULL disagreements " << r.null_mismatch << ", decision mismatches "
	<< r.decision_mismatch << " (early drops a kept row: " << r.early_drops_kept << "), cast used " << r.cast_used
	<< " (wrong " << r.cast_wrong << "), failures early-only " << r.early_failed_only << " / list-only "
	<< r.late_failed_only << " / both " << r.both_failed << ", public skip_row mismatches " << r.public_mismatch
	<< "/" << r.public_checked << ", kept " << r.kept << ", dropped " << r.dropped << samples);
  CHECK (r.rows == n);
  CHECK (r.hash_mismatch == 0);
  CHECK (r.null_mismatch == 0);
  CHECK (r.decision_mismatch == 0);
  CHECK (r.early_drops_kept == 0);
  CHECK (r.cast_wrong == 0);
  CHECK (r.early_failed_only == 0);
  CHECK (r.late_failed_only == 0);
  CHECK (r.public_mismatch == 0);
  CHECK (r.nulls > 0);
  CHECK (r.kept > 0);
  CHECK (r.dropped > r.nulls);	/* absent keys too, not only the NULLs */
  if (expect_foreign)
    {
      CHECK (r.foreign > 0);
      CHECK (r.cast_used == r.foreign);
    }
  WARN (ks.name << ": rows " << r.rows << " (NULL " << r.nulls << ", foreign " << r.foreign << " -> key_cast "
	<< r.cast_used << "), coerce " << r.coerce_dom << ", hash mismatches " << r.hash_mismatch
	<< ", decision mismatches " << r.decision_mismatch << ", kept " << r.kept << ", dropped " << r.dropped
	<< ", both failed " << r.both_failed);
}

/* value makers: a NULL now and then (3%) */
static bool
k_null (std::mt19937 & rng)
{
  return rng () % 100 < 3;
}

static k_value
kv (DB_VALUE v, bool foreign = false)
{
  k_value k;
  k.v = v;
  k.foreign = foreign && !DB_IS_NULL (&v);
  return k;
}

static k_value
k_int (std::mt19937 & rng)
{
  return kv (k_null (rng) ? v_null () : v_int ((int) (rng () % 200001) - 100000));
}

static DB_VALUE
v_bigint (long long x)
{
  DB_VALUE d;
  db_make_bigint (&d, x);
  return d;
}

static DB_VALUE
v_double (double x)
{
  DB_VALUE d;
  db_make_double (&d, x);
  return d;
}

/* a NUMERIC of exactly (p, s): +-(int part).(frac) with the given digit counts, cast from text */
static DB_VALUE
v_numeric (std::mt19937 & rng, int p, int s, int int_digits)
{
  long long ip = 0, lim = 1;
  for (int i = 0; i < int_digits; i++)
    {
      lim *= 10;
    }
  ip = (long long) (rng () % (unsigned long long) lim);
  long long fl = 1;
  for (int i = 0; i < s; i++)
    {
      fl *= 10;
    }
  long long fp = s > 0 ? (long long) (rng () % (unsigned long long) fl) : 0;
  char buf[96];
  if (s > 0)
    {
      snprintf (buf, sizeof (buf), "%s%lld.%0*lld", rng () % 2 ? "-" : "", ip, s, fp);
    }
  else
    {
      snprintf (buf, sizeof (buf), "%s%lld", rng () % 2 ? "-" : "", ip);
    }
  return cast_text (buf, dom_numeric (p, s));
}

/* a string of 0..maxlen characters from alphabet, optionally with trailing blanks */
static std::string
k_text (std::mt19937 & rng, const char *alphabet, int maxlen, bool trailing)
{
  int len = (int) (rng () % (maxlen + 1));
  std::string s;
  size_t na = strlen (alphabet);
  for (int i = 0; i < len; i++)
    {
      s += alphabet[rng () % na];
    }
  if (trailing && len < maxlen && rng () % 4 == 0)
    {
      s += std::string (1 + rng () % (maxlen - len), ' ');
    }
  return s;
}

static DB_VALUE
v_date (std::mt19937 & rng)
{
  DB_VALUE d;
  db_make_date (&d, 1 + rng () % 12, 1 + rng () % 28, 1990 + rng () % 40);
  return d;
}

static DB_VALUE
v_datetime (std::mt19937 & rng)
{
  DB_VALUE dv = v_date (rng);
  DB_DATETIME dt;
  dt.date = *db_get_date (&dv);
  dt.time = (unsigned int) (rng () % (24u * 3600u * 1000u));
  DB_VALUE d;
  db_make_datetime (&d, &dt);
  return d;
}

V5E_TEST_CASE (b13_ints, "B13 (K): the key a row gives before its projection hashes as the list's tuple of that row "
	       "does - INT column x INT first, INT column x BIGINT first (coerced to BIGINT), TYPE_CONSTANT fast peek / "
	       "TYPE_ATTR_ID; 10,000 random rows each (3% NULL - skipped by both): hash mismatches 0, the decision of the "
	       "bits the same, no cast", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  k_spec a { "INT column x INT first", dom_int (), dom_int (), COL_CONSTANT, [] (std::mt19937 & rng, int)
    {
      return k_int (rng);
    }
  };
  check_k (a);
  k_spec b { "INT column x BIGINT first", dom_bigint (), dom_int (), COL_ATTR, [] (std::mt19937 & rng, int)
    {
      return k_int (rng);
    }
  };
  check_k (b);
}

V5E_TEST_CASE (b13_numeric, "B13 (K): NUMERIC(10,2) column x NUMERIC(20,4) first and NUMERIC(20,4) column x "
	       "NUMERIC(10,2) first (each way coerced to the common domain): hash mismatches 0 on 10,000 rows each; a "
	       "NUMERIC(10,2) column fed values of another domain - NUMERIC(12,4), NUMERIC(8,1), NUMERIC(10,3), INT, "
	       "BIGINT, DOUBLE - casts each through key_cast and hashes as the projection's cast does", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *n102 = dom_numeric (10, 2), *n204 = dom_numeric (20, 4);
  k_spec a { "NUMERIC(10,2) column x NUMERIC(20,4) first", n204, n102, COL_CONSTANT, [] (std::mt19937 & rng, int)
    {
      return kv (k_null (rng) ? v_null () : v_numeric (rng, 10, 2, 1 + rng () % 8));
    }
  };
  check_k (a);
  k_spec b { "NUMERIC(20,4) column x NUMERIC(10,2) first", n102, n204, COL_CONSTANT, [] (std::mt19937 & rng, int)
    {
      /* half with two decimals (the first input holds them exactly), half with four */
      DB_VALUE v = v_numeric (rng, 20, 4, 1 + rng () % 7);
      if (rng () % 2)
	{
	  DB_VALUE w;
	  db_make_null (&w);
	  tp_value_cast (&v, &w, dom_numeric (10, 2), false);
	  pr_clear_value (&v);
	  tp_value_cast (&w, &v, dom_numeric (20, 4), false);
	}
      return kv (k_null (rng) ? v_null () : v);
    }
  };
  check_k (b);
  k_spec c { "NUMERIC(10,2) column, values of other domains (key_cast)", n204, n102, COL_CONSTANT,
    [] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      switch (i % 6)
	{
	case 0:
	  return kv (v_numeric (rng, 12, 4, 1 + rng () % 7), true);
	case 1:
	  return kv (v_numeric (rng, 8, 1, 1 + rng () % 7), true);
	case 2:
	  return kv (v_numeric (rng, 10, 3, 1 + rng () % 7), true);
	case 3:
	  return kv (v_int ((int) (rng () % 2000001) - 1000000), true);
	case 4:
	  return kv (v_bigint ((long long) (rng () % 20000001) - 10000000), true);
	default:
	  return kv (v_double (((double) ((long long) (rng () % 2000000001) - 1000000000)) / 1000.0), true);
	}
    }
  };
  check_k (c, 12000, true);
}

V5E_TEST_CASE (b13_strings, "B13 (K): CHAR(10) column x VARCHAR(20) first, VARCHAR(20) column x CHAR(10) first (trailing "
	       "blanks), VARCHAR(20) utf8_en_ci both sides ('ABC' and 'abc' one key) through fetch.c's slow path - "
	       "values of another type or collation cast through key_cast: hash mismatches 0 on 10,000 rows each",
	       "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *c10 = dom_string (DB_TYPE_CHAR, 10, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *v20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *ci20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_EN_CI);
  k_spec a { "CHAR(10) column x VARCHAR(20) first", v20, c10, COL_CONSTANT_SLOW, [c10, v20] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      std::string t = k_text (rng, "ab c", 10, false);
      /* a quarter VARCHAR(20) values in the CHAR(10) column (another type: cast, blank-padded) */
      return i % 4 == 0 ? kv (cast_text (t.c_str (), v20), true) : kv (cast_text (t.c_str (), c10));
    }
  };
  check_k (a);
  k_spec b { "VARCHAR(20) column x CHAR(10) first", c10, v20, COL_CONSTANT, [c10, v20] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      std::string t = k_text (rng, "ab c", 6, true);
      return i % 4 == 0 ? kv (cast_text (t.c_str (), c10), true) : kv (cast_text (t.c_str (), v20));
    }
  };
  check_k (b);
  k_spec c { "VARCHAR(20) utf8_en_ci both sides", ci20, ci20, COL_CONSTANT_SLOW, [v20, ci20] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      /* one of 3,000 words (base 6 digits over "abcdef"), each letter's case at random: case variants of a word
       * meet often, and a third of the words never reach the first input */
      int w = (int) (rng () % 3000);
      std::string t;
      do
	{
	  char ch = "abcdef"[w % 6];
	  t += rng () % 2 ? (char) (ch - 'a' + 'A') : ch;
	  w /= 6;
	}
      while (w > 0);
      /* a quarter the same text with utf8_bin collation in the utf8_en_ci column (another collation: cast) */
      return i % 4 == 0 ? kv (cast_text (t.c_str (), v20), true) : kv (cast_text (t.c_str (), ci20));
    }
  };
  check_k (c);

  /* 'ABC' and 'abc' are one key of the utf8_en_ci join: a first input holding only 'abc' keeps 'ABC', before and after
   * the projection */
  {
    boot_types ();
    projected p = make_projected ({ {COL_CONSTANT, ci20, false} });
    join j = projected_join (p, { ci20 }, { {0, 0} });
    add_row (j.first ()->list_id, { cast_text ("abc", ci20) });
    first_done (j);
    attach_second (j);
    unitseam_bloom_fill_now (owner_thread (), j.second ());
    REQUIRE (view (j.second ()).state == C ().ready);
    for (const char *t : { "ABC", "aBc", "abc", "Abc" })
      {
	INFO (t);
	DB_VALUE v = cast_text (t, ci20);
	pr_clone_value (&v, p.cells[0]);
	unsigned int h = 0;
	int mask = 0, d = -1;
	CHECK (unitseam_bloom_early_key (owner_thread (), j.second (), &p.vd, &h, &mask, &d) == 0);
	CHECK (d == 0);
	CHECK_FALSE (hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd));
	pr_clear_value (p.cells[0]);
	pr_clear_value (&v);
      }
    DB_VALUE v = cast_text ("abd", ci20);
    pr_clone_value (&v, p.cells[0]);
    CHECK (hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd));	/* not a key: dropped (not a false positive) */
    pr_clear_value (p.cells[0]);
    pr_clear_value (&v);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
}

V5E_TEST_CASE (b13_temporal_double, "B13 (K): DATE column x DATE first (TYPE_DBVAL), DATE column x DATETIME first "
	       "(coerced to DATETIME; DATETIME values in the DATE column cast through key_cast), DOUBLE column x INT first "
	       "and INT column x DOUBLE first (INT / DOUBLE / BIGINT values of the other type cast): hash mismatches 0 on "
	       "10,000 rows each", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *date = tp_domain_resolve_default (DB_TYPE_DATE), *datetime = tp_domain_resolve_default (DB_TYPE_DATETIME);
  k_spec a { "DATE column x DATE first", date, date, COL_DBVAL, [] (std::mt19937 & rng, int)
    {
      return kv (k_null (rng) ? v_null () : v_date (rng));
    }
  };
  check_k (a);
  k_spec b { "DATE column x DATETIME first", datetime, date, COL_CONSTANT, [] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      return i % 4 == 0 ? kv (v_datetime (rng), true) : kv (v_date (rng));
    }
  };
  check_k (b);
  k_spec c { "DOUBLE column x INT first", dom_int (), dom_double (), COL_CONSTANT, [] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      int x = (int) (rng () % 200001) - 100000;
      if (i % 4 == 0)
	{
	  return kv (v_int (x), true);
	}
      return kv (v_double (i % 2 ? (double) x : (double) x + 0.5));
    }
  };
  check_k (c);
  k_spec d { "INT column x DOUBLE first", dom_double (), dom_int (), COL_CONSTANT, [] (std::mt19937 & rng, int i)
    {
      if (k_null (rng))
	{
	  return kv (v_null ());
	}
      int x = (int) (rng () % 200001) - 100000;
      if (i % 6 == 0)
	{
	  return kv (v_double ((double) x + (rng () % 2 ? 0.25 : 0.0)), true);
	}
      if (i % 6 == 1)
	{
	  return kv (v_bigint (x), true);
	}
      return kv (v_int (x));
    }
  };
  check_k (d);
}

/*
 * F5 (latent): a value of the list column's string type but another codeset. The projection writes a string of the
 * column's type as it is - qdata_get_dbval_from_constant_regu_variable casts only another type or a NUMERIC of another
 * precision / scale - and the list reads the bytes back under the column's codeset and collation (a relabel). The check
 * before the projection converts it instead (hjoin_bloom_value_in_domain says no: codeset; tp_value_coerce
 * transcodes), so the two keys differ for any non-ASCII character. When the first input holds the same unconverted
 * bytes (its own projection wrote them the same way), the join matches the row and the early check drops it. Whether a
 * plain column reference (TYPE_CONSTANT / TYPE_ATTR_ID / TYPE_DBVAL) can carry a value whose codeset is not its
 * domain's is not established - the producers give a value its regu's domain - so this is latent.
 */
static std::string
latin_word (std::mt19937 & rng)
{
  static const char *chars[] = { "a", "b", "\xc3\xa9" };	/* a, b, e-acute (UTF-8) */
  std::string t;
  int len = 1 + (int) (rng () % 5);
  for (int i = 0; i < len; i++)
    {
      t += chars[rng () % 3];
    }
  return t;
}

struct f5_result
{
  int rows = 0, nonascii = 0, hash_mismatch = 0, join_matches = 0, dropped_matches = 0;
  std::vector<std::string> samples;
};

static f5_result
run_f5 ()
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *u20 = dom_string (DB_TYPE_VARCHAR, 20, LANG_COLL_UTF8_BINARY);
  TP_DOMAIN *iso20 = tp_domain_resolve (DB_TYPE_VARCHAR, NULL, 20, 0, NULL, LANG_COLL_ISO_BINARY);
  REQUIRE (iso20 != NULL);
  REQUIRE (TP_DOMAIN_CODESET (iso20) != TP_DOMAIN_CODESET (u20));
  f5_result r;
  projected p = make_projected ({ {COL_CONSTANT, u20, false} });
  join j = projected_join (p, { u20 }, { {0, 0} });
  QFILE_LIST_ID *fl = j.first ()->list_id, *sl = j.second ()->list_id;
  std::mt19937 rng (5);
  std::vector<DB_VALUE> vals;
  for (int i = 0; i < 2000; i++)
    {
      vals.push_back (cast_text (latin_word (rng).c_str (), iso20));	/* ISO-8859-1 values */
    }
  /* the first input holds every second value as its own projection wrote it under its UTF-8 column: unconverted */
  std::vector<char *> ft;
  for (int i = 0; i < 2000; i += 2)
    {
      std::vector<DB_VALUE> one { vals[i] };
      add_row (fl, one);
      ft.push_back (make_tuple (fl, one));
    }
  first_done (j);
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);
  REQUIRE (unitseam_hj_init_domain_info (owner_thread (), j.hj) == NO_ERROR);
  for (size_t i = 0; i < vals.size (); i++)
    {
      pr_clear_value (p.cells[0]);
      pr_clone_value (&vals[i], p.cells[0]);
      r.rows++;
      unsigned int he = 0, hl = 0;
      int mask = 0, de = -1;
      REQUIRE (unitseam_bloom_early_key (owner_thread (), j.second (), &p.vd, &he, &mask, &de) == 0);
      char *t = project_row (sl, p);
      REQUIRE (t != nullptr);
      QFILE_TUPLE_RECORD rec = bound (sl, t);
      REQUIRE (unitseam_hj_second_key (owner_thread (), j.hj, true, &rec, &hl) == 0);
      bool nonascii = false;
      for (int b = 0; b < db_get_string_size (&vals[i]); b++)
	{
	  nonascii |= (unsigned char) db_get_string (&vals[i])[b] >= 0x80;
	}
      r.nonascii += nonascii;
      if (he != hl)
	{
	  r.hash_mismatch++;
	}
      /* the join's own test: the in-memory table's hash, then the probe's compare */
      bool matched = false;
      for (char *f : ft)
	{
	  QFILE_TUPLE_RECORD frec = bound (fl, f), srec = bound (sl, t);
	  bool fnull, snull, keq, ceq;
	  unsigned int fh, sh;
	  REQUIRE (unitseam_hj_join_pair (owner_thread (), j.hj, true, &frec, &srec, &fnull, &snull, &fh, &sh, &keq, &ceq)
		   == NO_ERROR);
	  matched |= fh == sh && ceq;
	}
      if (matched)
	{
	  r.join_matches++;
	  if (de == 1)
	    {
	      r.dropped_matches++;
	      if (r.samples.size () < 4)
		{
		  char buf[64];
		  snprintf (buf, sizeof (buf), " - early 0x%08x, list 0x%08x", he, hl);
		  r.samples.push_back (value_text (&vals[i]) + buf);
		}
	    }
	}
      free (t);
    }
  pr_clear_value (p.cells[0]);
  for (char *f : ft)
    {
      free (f);
    }
  for (auto &v : vals)
    {
      pr_clear_value (&v);
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  return r;
}

V5E_TEST_CASE (f5_codeset, "PR27659 F5 (latent): a string of the column's type in another codeset (ISO-8859-1 under a "
	       "UTF-8 column) is transcoded before the hash while the list keeps its bytes - the keys differ for "
	       "non-ASCII text, and with the first input holding the same bytes the early check drops rows the join "
	       "matches", "[defect]")
{
  f5_result r = run_f5 ();
  std::string samples;
  for (auto &s : r.samples)
    {
      samples += "\n    " + s;
    }
  WARN ("rows " << r.rows << " (non-ASCII " << r.nonascii << "), early / list hash mismatches " << r.hash_mismatch
	<< ", rows the join matches " << r.join_matches << ", of them dropped before the projection "
	<< r.dropped_matches << samples);
  CHECK (r.hash_mismatch > 0);
  CHECK (r.dropped_matches > 0);
}

/* ================================================================ B14 (L): key_regus and the early entries */

/* attach on a join whose second input projects through p; the regu attach found for key k, or NULL; released after */
static std::vector<const void *>
attach_key_regus (projected & p, const std::vector<TP_DOMAIN *> &first_doms,
		  const std::vector<std::pair<int, int>> &keys, bool first_is_outer = true)
{
  join j = projected_join (p, first_doms, keys, first_is_outer);
  std::vector<DB_VALUE> row;
  for (TP_DOMAIN * d:first_doms)
    {
      (void) d;
      row.push_back (v_int (1));
    }
  add_row (j.first ()->list_id, row);
  first_done (j);
  attach_second (j);
  REQUIRE (view (j.second ()).present);
  std::vector<const void *> r;
  for (size_t k = 0; k < keys.size (); k++)
    {
      r.push_back (unitseam_bloom_key_regu (j.second (), (int) k));
    }
  CHECK ((view (j.second ()).key_regus != NULL) == (r[0] != NULL));
  hjoin_bloom_release (owner_thread (), j.second ());
  return r;
}

V5E_TEST_CASE (b14_find, "B14 (L): attach finds each key column's regu variable by its position in the list - hidden "
	       "columns are not in the list and are passed over (a two-column key, either input second) - and takes "
	       "TYPE_CONSTANT, TYPE_ATTR_ID, TYPE_DBVAL, TYPE_POSITION only; a computed key (TYPE_INARITH), a host "
	       "variable (TYPE_POS_VALUE), an analytic-window or UPDATE/INSERT-list regu, a key past the list, no output "
	       "list, or one computed key of two -> no key_regus (checked after the projection)", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  TP_DOMAIN *i = dom_int ();
  /* nodes: 0 hidden, 1 list 0, 2 hidden computed, 3 list 1, 4 list 2, 5 list 3 */
  std::vector<pcol> cols = { {COL_CONSTANT, i, true}, {COL_CONSTANT, i, false}, {COL_ARITH, i, true},
  {COL_ATTR, i, false}, {COL_DBVAL, i, false}, {COL_CONSTANT_SLOW, i, false}
  };
  {
    projected p = make_projected (cols);
    /* first (outer) (a, b); keys outer.a = inner list 2, outer.b = inner list 0 */
    std::vector<const void *> r = attach_key_regus (p, { i, i }, { {0, 2}, {1, 0} });
    CHECK (r[0] == (const void *) p.regus[4]);
    CHECK (r[1] == (const void *) p.regus[1]);
  }
  {
    projected p = make_projected (cols);
    /* inner first: the outer is second; keys (outer list 3, inner 0), (outer list 1, inner 0) */
    std::vector<const void *> r = attach_key_regus (p, { i }, { {3, 0}, {1, 0} }, false);
    CHECK (r[0] == (const void *) p.regus[5]);
    CHECK (r[1] == (const void *) p.regus[3]);
  }
  {
    projected p = make_projected ({ {COL_POSITION, i, false} });
    CHECK (attach_key_regus (p, { i }, { {0, 0} })[0] == (const void *) p.regus[0]);
  }
  struct
  {
    const char *what;
    std::function<void (projected &)> setup;
    std::vector<pcol> cols;
  } none[] = {
    {"computed key (TYPE_INARITH)", [] (projected &) {}, { {COL_ARITH, i, false} }},
    {"host variable (TYPE_POS_VALUE)", [] (projected &) {}, { {COL_POS_VALUE, i, false} }},
    {"analytic window regu", [] (projected & p) { p.regus[0]->flags |= REGU_VARIABLE_ANALYTIC_WINDOW; },
     { {COL_CONSTANT, i, false} }},
    {"UPDATE / INSERT list regu", [] (projected & p) { p.regus[0]->flags |= REGU_VARIABLE_UPD_INS_LIST; },
     { {COL_CONSTANT, i, false} }},
  };
  for (auto &c : none)
    {
      INFO (c.what);
      projected p = make_projected (c.cols);
      c.setup (p);
      CHECK (attach_key_regus (p, { i }, { {0, 0} })[0] == NULL);
    }
  {
    INFO ("one computed key of two");
    projected p = make_projected ({ {COL_CONSTANT, i, false}, {COL_ARITH, i, false} });
    std::vector<const void *> r = attach_key_regus (p, { i, i }, { {0, 0}, {1, 1} });
    CHECK (r[0] == NULL);
    CHECK (r[1] == NULL);
  }
  {
    INFO ("a key past the output list (the list is longer than the regus that reach it)");
    projected p = make_projected ({ {COL_CONSTANT, i, false}, {COL_CONSTANT, i, true} });
    std::vector<TP_DOMAIN *> sd = { i, i };	/* the list says two columns, the output list gives one */
    join j = new_join (JOIN_INNER, { i }, sd, { {0, 1} });
    j.second ()->outptr_list = p.out;
    add_row (j.first ()->list_id, { v_int (1) });
    first_done (j);
    attach_second (j);
    CHECK (unitseam_bloom_key_regu (j.second (), 0) == NULL);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
  {
    INFO ("no output list");
    join j = int_join_with ({ 1 });
    attach_second (j);
    CHECK (view (j.second ()).key_regus == NULL);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
}

/* a key column a scalar subquery computes, as pt_make_regu_subquery makes it: TYPE_CONSTANT over the subquery's single
 * tuple value, regu->xasl the subquery (XASL_LINK_TO_REGU_VARIABLE), not run yet */
static XASL_NODE *
link_subquery (REGU_VARIABLE * r)
{
  XASL_NODE *sub = new_input (new_list ({ dom_int () }));
  XASL_SET_FLAG (sub, XASL_LINK_TO_REGU_VARIABLE);
  sub->single_tuple = (VAL_LIST *) calloc (1, sizeof (VAL_LIST));
  sub->single_tuple->valp = (QPROC_DB_VALUE_LIST) calloc (1, sizeof (*sub->single_tuple->valp));
  sub->single_tuple->valp->val = (DB_VALUE *) calloc (1, sizeof (DB_VALUE));
  db_make_null (sub->single_tuple->valp->val);
  sub->single_tuple->val_cnt = 1;
  r->type = TYPE_CONSTANT;
  r->flags &= ~REGU_VARIABLE_FAST_PEEK;	/* stx_set_fast_peek leaves a regu with a linked subquery slow */
  r->value.dbvalptr = sub->single_tuple->valp->val;
  r->xasl = sub;
  sub->status = XASL_INITIALIZED;
  return sub;
}

V5E_TEST_CASE (b14_observation_subquery, "B14 (L) observation, not a contract: a key column computed by a scalar "
	       "subquery (TYPE_CONSTANT linked to it) is taken as plain - the check before the projection runs the "
	       "subquery, and when it fails clears its error: the projection then fails without an error id "
	       "(reachability - a hash join input that projects a scalar subquery as its key - not shown)",
	       "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  projected p = make_projected ({ {COL_CONSTANT_SLOW, dom_int (), false} });
  XASL_NODE *sub = link_subquery (p.regus[0]);
  join j = projected_join (p, { dom_int () }, { {0, 0} });
  add_row (j.first ()->list_id, { v_int (7) });
  first_done (j);
  /* without a filter: the projection runs the subquery and reports its error */
  fake_subquery_fails = 1;
  er_clear ();
  char *t0 = project_row (j.second ()->list_id, p);
  int err_without = er_errid ();
  REQUIRE (t0 == nullptr);
  REQUIRE (err_without == ER_QPROC_INVALID_QRY_SINGLE_TUPLE);
  er_clear ();
  /* with the filter filled */
  attach_second (j);
  bool taken = unitseam_bloom_key_regu (j.second (), 0) == (const void *) p.regus[0];
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);
  /* a subquery that succeeds: run once (by the check), its value decides, the projection reads it */
  fake_subquery_fails = 0;
  fake_subquery_value = 7;
  sub->status = XASL_INITIALIZED;	/* the next row's run */
  int runs0 = fake_subquery_runs;
  bool skipped_ok = hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
  char *t1 = project_row (j.second ()->list_id, p);
  int runs_ok = fake_subquery_runs - runs0;
  free (t1);
  /* a subquery that fails */
  fake_subquery_fails = 1;
  sub->status = XASL_INITIALIZED;
  er_clear ();
  bool skipped_bad = hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
  int err_after_check = er_errid ();
  unitseam_bloom_view v = view (j.second ());
  char *t2 = project_row (j.second ()->list_id, p);
  int err_after_projection = er_errid ();
  WARN ("observation: TYPE_CONSTANT key over a linked subquery - key_regus " << (taken ? "taken" : "not taken")
	<< "; a succeeding subquery: kept " << !skipped_ok << ", runs " << runs_ok
	<< "; a failing one: skip_row " << skipped_bad << ", error id after the check " << err_after_check
	<< ", filter state " << v.state << " reason " << v.off_reason << ", projection "
	<< (t2 == nullptr ? "failed" : "succeeded") << " with error id " << err_after_projection
	<< " (without the filter " << err_without << ")");
  free (t2);
  fake_subquery_fails = 0;
  er_clear ();
  hjoin_bloom_release (owner_thread (), j.second ());
}

V5E_TEST_CASE (b14_entries, "B14 (L): skip_row checks only with key_regus - without them it returns false and counts "
	       "nothing (skip_descr / skip_tuple do it after the projection); with them it counts the owner row, fills at "
	       "the threshold and decides from the regu's value, and skip_descr / skip_tuple then return false without "
	       "counting (no row counted twice); another thread's row, a clone, the first input are not the owner's",
	       "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 3000);	/* threshold 1,500 */
  /* without key_regus (no output list): the late path */
  {
    join j = int_join_with (keys);
    attach_second (j);
    VAL_DESCR vd;
    memset (&vd, 0, sizeof (vd));
    for (int k = 0; k < 10; k++)
      {
	CHECK_FALSE (hjoin_bloom_skip_row (owner_thread (), j.second (), &vd));
      }
    CHECK (view (j.second ()).seen == 0);
    CHECK_FALSE (owner_entry (j, null_row (), 0));
    CHECK_FALSE (owner_entry (j, null_row (), 1));
    CHECK (view (j.second ()).seen == 2);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
  /* with key_regus: the early path */
  projected p = make_projected ({ {COL_CONSTANT, dom_int (), true}, {COL_CONSTANT, dom_int (), false} });
  join j = projected_join (p, { dom_int () }, { {0, 0} });
  for (int k : keys)
    {
      add_row (j.first ()->list_id, { v_int (k) });
    }
  first_done (j);
  attach_second (j);
  REQUIRE (view (j.second ()).key_regus != NULL);
  REQUIRE (view (j.second ()).threshold == 1500);
  fake_list *first = fake_list_of (j.first ()->list_id);
  auto row = [&] (DB_VALUE v)
  {
    pr_clear_value (p.cells[1]);
    pr_clone_value (&v, p.cells[1]);
    db_make_int (p.cells[0], 0);
  };
  /* rows 1 .. 1,499: unchecked (NULL keys - dropped if checked), each counted once though both entries are tried */
  int through = 0;
  for (int k = 1; k < 1500; k++)
    {
      row (v_null ());
      through += !hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
      std::vector<DB_VALUE> r = { v_null () };
      CHECK_FALSE (owner_entry (j, r, k % 2));	/* the late entry stands aside */
    }
  CHECK (through == 1499);
  unitseam_bloom_view v = view (j.second ());
  CHECK (v.seen == 1499);
  CHECK (v.state == C ().waiting);
  CHECK (v.row_buf == NULL);	/* no row serialized for a check */
  CHECK (first->open_calls == 0);
  /* another thread, a clone, the first input: nothing */
  row (v_null ());
  CHECK_FALSE (hjoin_bloom_skip_row (other_thread (), j.second (), &p.vd));
  XASL_NODE *clone = (XASL_NODE *) malloc (sizeof (XASL_NODE));
  memcpy (clone, j.second (), sizeof (XASL_NODE));
  CHECK_FALSE (hjoin_bloom_skip_row (owner_thread (), clone, &p.vd));
  CHECK_FALSE (hjoin_bloom_skip_row (owner_thread (), j.first (), &p.vd));
  CHECK (view (j.second ()).seen == 1499);
  free (clone);
  /* row 1,500: filled, and checked (NULL: dropped) */
  CHECK (hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd));
  v = view (j.second ());
  CHECK (v.state == C ().ready);
  CHECK (first->open_calls == 1);
  CHECK (v.seen == 1500);
  CHECK (v.tested == 1);
  CHECK (v.skipped == 1);
  /* then: first-input keys kept, absent keys mostly dropped; the late entries count nothing */
  int dropped_present = 0, dropped_absent = 0;
  for (int k = 0; k < 1000; k++)
    {
      row (v_int (keys[k]));
      dropped_present += hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
      CHECK_FALSE (owner_entry (j, ints ({ keys[k] }), 0));
      row (v_int (1000000 + k));
      dropped_absent += hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
      CHECK_FALSE (owner_entry (j, ints ({ 1000000 + k }), 1));
    }
  v = view (j.second ());
  CHECK (dropped_present == 0);
  CHECK (dropped_absent > 950);
  CHECK (v.seen == 3500);
  CHECK (v.tested == 2001);
  CHECK (v.skipped == 1 + dropped_absent);
  CHECK (v.row_buf == NULL);
  pr_clear_value (p.cells[1]);
  hjoin_bloom_release (owner_thread (), j.second ());
}

V5E_TEST_CASE (b14_workers, "B14 (L): a worker begun with its clone's output list finds its own key regu there and "
	       "decides from it as the owner does; its late entries stand aside uncounted; a worker whose clone list "
	       "gives no plain key (none, computed) lets its rows through; on a filter without key_regus "
	       "worker_skip_row stands aside and the late entries check", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);
  projected p = make_projected ({ {COL_CONSTANT, dom_int (), false} });
  join j = projected_join (p, { dom_int () }, { {0, 0} });
  for (int k : keys)
    {
      add_row (j.first ()->list_id, { v_int (k) });
    }
  first_done (j);
  attach_second (j);
  HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
  REQUIRE (b != NULL);
  unitseam_bloom_fill_now (owner_thread (), j.second ());	/* what hjoin_bloom_poll does at the threshold */
  REQUIRE (view (j.second ()).state == C ().ready);

  /* the worker's clone: its own output list over its own scan values */
  projected wp = make_projected ({ {COL_CONSTANT, dom_int (), false} });
  HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b, wp.out);
  REQUIRE (w != NULL);
  unitseam_worker_view wv = wview (w);
  CHECK (wv.key_regu0 == (const void *) wp.regus[0]);
  CHECK (wv.key_regu0 != unitseam_bloom_key_regu (j.second (), 0));
  CHECK_FALSE (wv.disabled);
  CHECK (wv.ready);
  QFILE_LIST_ID *wl = worker_list (j);
  int differ = 0, false_neg = 0, absent_dropped = 0;
  for (int k = 0; k < 600; k++)
    {
      DB_VALUE v = k % 3 == 0 ? v_int (keys[k]) : k % 3 == 1 ? v_int (5000000 + k) : v_null ();
      pr_clone_value (&v, wp.cells[0]);
      pr_clone_value (&v, p.cells[0]);
      bool ws = hjoin_bloom_worker_skip_row (worker_thread (0), w, &wp.vd);
      bool os = hjoin_bloom_skip_row (owner_thread (), j.second (), &p.vd);
      differ += ws != os;
      false_neg += ws && k % 3 == 0;
      absent_dropped += ws && k % 3 == 1;
      std::vector<DB_VALUE> r = { v };
      CHECK_FALSE (worker_skip (w, worker_thread (0), wl, r, k % 2 == 0));	/* stands aside */
    }
  CHECK (differ == 0);
  CHECK (false_neg == 0);
  CHECK (absent_dropped > 190);
  CHECK (wview (w).tested == 600);
  hjoin_bloom_worker_end (worker_thread (0), w);
  CHECK (view (j.second ()).seen == 1200);	/* the owner's 600 and the worker's 600, each once */

  /* a clone list that gives no plain key: the worker lets its rows through (counted in seen, never checked) */
  for (int variant = 0; variant < 2; variant++)
    {
      INFO ((variant == 0 ? "no output list" : "computed key"));
      projected cp = make_projected ({ {COL_ARITH, dom_int (), false} });
      HJOIN_BLOOM_WORKER *x = wbegin (worker_thread (1), b, variant == 0 ? NULL : cp.out);
      REQUIRE (x != NULL);
      CHECK (wview (x).disabled);
      VAL_DESCR vd;
      memset (&vd, 0, sizeof (vd));
      int through = 0;
      for (int k = 0; k < 300; k++)
	{
	  through += !hjoin_bloom_worker_skip_row (worker_thread (1), x, &vd);
	}
      CHECK (through == 300);
      CHECK (wview (x).tested == 0);
      hjoin_bloom_worker_end (worker_thread (1), x);
    }
  pr_clear_value (p.cells[0]);
  pr_clear_value (wp.cells[0]);
  hjoin_bloom_release (owner_thread (), j.second ());

  /* a filter without key_regus: worker_skip_row stands aside uncounted, the late entries check */
  {
    int_scene t = int_scene_setup (500, 21);
    REQUIRE (view (t.j.second ()).key_regus == NULL);
    projected xp = make_projected ({ {COL_CONSTANT, dom_int (), false} });
    HJOIN_BLOOM_WORKER *x = wbegin (worker_thread (2), t.b, xp.out);
    REQUIRE (x != NULL);
    CHECK (wview (x).key_regu0 == NULL);
    db_make_null (xp.cells[0]);
    CHECK_FALSE (hjoin_bloom_worker_skip_row (worker_thread (2), x, &xp.vd));
    CHECK (wview (x).seen_unflushed == 0);
    QFILE_LIST_ID *xl = worker_list (t.j);
    std::vector<DB_VALUE> nul = null_row ();
    CHECK (worker_skip (x, worker_thread (2), xl, nul, false));
    CHECK (wview (x).tested == 1);
    hjoin_bloom_worker_end (worker_thread (2), x);
    hjoin_bloom_release (owner_thread (), t.j.second ());
  }
}

/* ================================================================ B15 (M): the first input arrives late */

/* a job thread that runs the second input (a parallel subquery job's), and the hash join's own thread */
static THREAD_ENTRY *
job_thread ()
{
  static THREAD_ENTRY *t = new_thread ();
  return t;
}

/* an INNER join of INT keys, first input (outer) holding keys - still running (XASL_INITIALIZED), attached by the raw
 * 8ad5ee2df attach before either input ran: nothing published, no owner yet */
static join
late_join (const std::vector<int> &keys)
{
  join j = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
  for (int k : keys)
    {
      add_row (j.first ()->list_id, { v_int (k) });
    }
  aptr_in_run_order (j);
  attach_raw (j.hj);
  REQUIRE (j.second ()->hj_bloom != NULL);
  REQUIRE (j.first ()->hj_bloom == j.second ()->hj_bloom);
  return j;
}

static void
first_ends (join & j, bool success = true)
{
  j.first ()->status = success ? XASL_SUCCESS : XASL_FAILURE;
  hjoin_bloom_input_done (first_runner (), j.first (), success);
}

V5_TEST_CASE (b15_gate, "B15 (M): until the first input's end is published the second's rows only count - no fill "
	      "past any threshold (owner rows, poll), the provisional threshold 1,024; input_done (first) publishes "
	      "first_rows and the threshold (max (1,024, rows / 2)) and fills nothing itself; the owner's next row fills "
	      "when seen has passed the threshold already, else the threshold-th row does; the poll the same; the owner is "
	      "bound at its first use, not at attach", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 3000);
  {
    join j = late_join (keys);
    fake_list *first = fake_list_of (j.first ()->list_id);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.first_done == 0);
    CHECK (v.threshold == 1024);
    CHECK (v.owner_thread == NULL);	/* bound at first use */
    CHECK (v.first_xasl == j.first ());
    CHECK (v.owner_xasl == j.second ());
    int through = 0;
    for (int k = 0; k < 2000; k++)
      {
	through += decide (j, null_row ()) == -1;
      }
    CHECK (through == 2000);
    v = view (j.second ());
    CHECK (v.owner_thread == (const void *) owner_thread ());
    CHECK (v.seen == 2000);
    CHECK (v.state == C ().waiting);
    CHECK_FALSE (v.set_up);
    CHECK (v.bits == NULL);
    CHECK (first->open_calls == 0);
    hjoin_bloom_poll (owner_thread (), j.second ()->hj_bloom);
    CHECK (first->open_calls == 0);
    /* the first input ends: published, nothing filled yet */
    first_ends (j);
    v = view (j.second ());
    CHECK (v.first_done == 1);
    CHECK (v.first_rows == 3000);
    CHECK (v.threshold == 1500);
    CHECK (v.state == C ().waiting);
    CHECK (first->open_calls == 0);
    /* the owner's next row: seen 2,001 past 1,500 - filled, and itself checked */
    CHECK (decide (j, null_row ()) == 1);
    v = view (j.second ());
    CHECK (v.state == C ().ready);
    CHECK (v.keys == 3000);
    CHECK (v.seen == 2001);
    CHECK (first->open_calls == 1);
    int dropped = 0;
    for (int k = 0; k < 500; k++)
      {
	dropped += decide (j, ints ({ keys[k * 6] })) == 1;
      }
    CHECK (dropped == 0);
    detach_second (j);
    CHECK (stats (j).outcome == C ().out_checked);
  }
  {
    /* seen under the threshold when the first input ends: the threshold-th row fills */
    join j = late_join (keys);
    fake_list *first = fake_list_of (j.first ()->list_id);
    for (int k = 0; k < 100; k++)
      {
	REQUIRE (decide (j, null_row ()) == -1);
      }
    first_ends (j);
    for (int k = 100; k < 1499; k++)
      {
	REQUIRE (decide (j, null_row ()) == -1);
      }
    CHECK (view (j.second ()).state == C ().waiting);
    CHECK (first->open_calls == 0);
    CHECK (decide (j, null_row ()) == 1);	/* row 1,500 */
    CHECK (view (j.second ()).seen == 1500);
    CHECK (first->open_calls == 1);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
  {
    /* the poll: a parallel scan's workers count before the end is published - no fill; after it, poll fills */
    join j = late_join (keys);
    fake_list *first = fake_list_of (j.first ()->list_id);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    REQUIRE (b != NULL);
    CHECK (view (j.second ()).owner_thread == (const void *) owner_thread ());	/* prepare binds too */
    HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
    QFILE_LIST_ID *wl = worker_list (j);
    std::vector<DB_VALUE> nul = null_row ();
    for (int k = 0; k < 2048; k++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, nul, false));
      }
    CHECK (view (j.second ()).seen == 2048);
    hjoin_bloom_poll (owner_thread (), b);
    CHECK (first->open_calls == 0);
    CHECK (view (j.second ()).state == C ().waiting);
    first_ends (j);
    hjoin_bloom_poll (other_thread (), b);	/* not the owner */
    CHECK (first->open_calls == 0);
    hjoin_bloom_poll (owner_thread (), b);
    CHECK (view (j.second ()).state == C ().ready);
    CHECK (first->open_calls == 1);
    hjoin_bloom_worker_end (worker_thread (0), w);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
  {
    /* bound at first use: the first thread that brings an owner row is the owner; another thread's rows then count
     * nothing, its prepare and poll do nothing */
    join j = late_join (keys);
    first_ends (j);
    CHECK (decide (j, null_row (), job_thread ()) == -1);
    CHECK (view (j.second ()).owner_thread == (const void *) job_thread ());
    CHECK (decide (j, null_row (), owner_thread ()) == -1);
    CHECK (view (j.second ()).seen == 1);
    CHECK (hjoin_bloom_prepare (owner_thread (), j.second ()) == NULL);
    CHECK (hjoin_bloom_prepare (job_thread (), j.second ()) == j.second ()->hj_bloom);
    /* the first input's rows never bind (it is not the checked input) */
    join k = late_join (keys);
    CHECK_FALSE (hjoin_bloom_skip_tuple (owner_thread (), k.first (), NULL));
    std::vector<DB_VALUE> r = ints ({ 1 });
    stage_descr (k.first ()->list_id, r);
    CHECK_FALSE (hjoin_bloom_skip_descr (owner_thread (), k.first ()));
    CHECK (view (k.second ()).owner_thread == NULL);
    hjoin_bloom_release (job_thread (), j.second ());
    hjoin_bloom_release (owner_thread (), k.second ());
  }
}

V5_TEST_CASE (b15_outcomes, "B15 (M): what finish reports and frees - FIRST_LATE (the second input done while waiting "
	      "and the first not; kept when the first ends after), OFF_FIRST_ROWS (the first failed, 0 rows, over "
	      "10,000,000, no list; the first reason kept against a later FIRST_LATE), NOT_BUILT (seen under the "
	      "threshold), ENDED_FIRST (3726fd95d: due - first done, seen past the threshold - but never filled; earlier "
	      "NOT_BUILT), CHECKED; threshold / seen / keys / tested / skipped; both inputs unlinked, everything freed; "
	      "finish without a filter, of another join, NULL, not a hash join: nothing", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 3000);
  size_t live0 = live_count ();
  {
    /* FIRST_LATE */
    join j = late_join (keys);
    for (int k = 0; k < 2000; k++)
      {
	REQUIRE (decide (j, null_row ()) == -1);
      }
    struct hjoin_bloom *b = j.second ()->hj_bloom;
    hjoin_bloom_input_done (owner_thread (), j.second (), true);
    unitseam_bloom_view v = view (j.second ());
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_first_late);
    first_ends (j);	/* later: published, but off stays off with its first reason */
    v = view (j.second ());
    CHECK (v.first_done == 1);
    CHECK (v.state == C ().off);
    CHECK (v.off_reason == C ().out_first_late);
    CHECK (fake_list_of (j.first ()->list_id)->open_calls == 0);
    hjoin_bloom_finish (owner_thread (), j.hj);
    unitseam_stats_view s = stats (j);
    CHECK (s.shown);
    CHECK (s.outcome == C ().out_first_late);
    CHECK (s.seen == 2000);
    CHECK (s.keys == 0);
    CHECK (s.tested == 0);
    CHECK (s.threshold == 1500);
    CHECK (j.first ()->hj_bloom == NULL);
    CHECK (j.second ()->hj_bloom == NULL);
    CHECK_FALSE (is_live (b));
  }
  {
    /* OFF_FIRST_ROWS: failed, 0 rows, over the bound, no list; rows through, the first reason kept */
    struct
    {
      const char *what;
      std::function<void (join &)> end;
    } cases[] = {
      {"first failed", [] (join & j) { first_ends (j, false); }},
      {"first 0 rows", [] (join & j) { j.first ()->list_id->tuple_cnt = 0; first_ends (j); }},
      {"first over 10,000,000 rows", [] (join & j) { j.first ()->list_id->tuple_cnt = C ().max_first_rows + 1;
								 first_ends (j); }},
      {"first list_id NULL", [] (join & j) { j.first ()->list_id = NULL; first_ends (j); }},
    };
    for (auto &c : cases)
      {
	INFO (c.what);
	join j = late_join (keys);
	fake_list *first = fake_list_of (j.first ()->list_id);
	c.end (j);
	unitseam_bloom_view v = view (j.second ());
	CHECK (v.state == C ().off);
	CHECK (v.off_reason == C ().out_off_first_rows);
	CHECK (v.first_done == 1);
	int through = 0;
	for (int k = 0; k < 2000; k++)
	  {
	    through += decide (j, null_row ()) == -1;
	  }
	CHECK (through == 2000);
	CHECK (first->open_calls == 0);
	hjoin_bloom_input_done (owner_thread (), j.second (), true);
	CHECK (view (j.second ()).off_reason == C ().out_off_first_rows);
	hjoin_bloom_finish (owner_thread (), j.hj);
	CHECK (stats (j).outcome == C ().out_off_first_rows);
	CHECK (stats (j).seen == 2000);
	CHECK (j.second ()->hj_bloom == NULL);
      }
    /* exactly the bound is not over it */
    join j = late_join (keys);
    j.first ()->list_id->tuple_cnt = C ().max_first_rows;
    first_ends (j);
    CHECK (view (j.second ()).state == C ().waiting);
    CHECK (view (j.second ()).threshold == C ().max_first_rows / 2);
    hjoin_bloom_release (owner_thread (), j.second ());
  }
  {
    /* NOT_BUILT: first done, seen under the threshold */
    join j = late_join (keys);
    first_ends (j);
    for (int k = 0; k < 1499; k++)
      {
	REQUIRE (decide (j, null_row ()) == -1);
      }
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.outcome == C ().out_not_built);
    CHECK (s.seen == 1499);
    CHECK (s.threshold == 1500);
  }
  {
    /* due but never filled: a parallel scan's last batches arrive at worker_end, no poll after them */
    join j = late_join (int_range (1, 2000));
    first_ends (j);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    HJOIN_BLOOM_WORKER *w = wbegin (worker_thread (0), b);
    QFILE_LIST_ID *wl = worker_list (j);
    std::vector<DB_VALUE> nul = null_row ();
    for (int k = 0; k < 1100; k++)
      {
	REQUIRE_FALSE (worker_skip (w, worker_thread (0), wl, nul, false));
      }
    hjoin_bloom_worker_end (worker_thread (0), w);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.seen == 1100);
    CHECK (s.threshold == 1024);
    if (C ().early)
      {
	CHECK (s.outcome == C ().out_ended_first);
      }
    else
      {
	CHECK (s.outcome == C ().out_not_built);
      }
  }
  {
    /* CHECKED, counts */
    join j = late_join (int_range (1, 2000));
    first_ends (j);
    owner_rows_before_fill (j);
    CHECK (decide (j, null_row ()) == 1);
    CHECK (decide (j, ints ({ 7 })) == 0);
    CHECK (decide (j, ints ({ 7000000 })) == 1);
    detach_second (j);
    unitseam_stats_view s = stats (j);
    CHECK (s.outcome == C ().out_checked);
    CHECK (s.keys == 2000);
    CHECK (s.seen == 1026);
    CHECK (s.tested == 3);
    CHECK (s.skipped == 2);
  }
  CHECK (live_count () == live0);	/* everything the filters allocated is freed */
  {
    /* finish that has nothing to do */
    join j = int_join_with ({ 1, 2 });
    unitseam_bloom_stats_stale (j.hj);
    HASHJOIN_BLOOM_STATS snap;
    memcpy (&snap, &j.hj->proc.hashjoin.bloom_stats, sizeof (snap));
    hjoin_bloom_finish (owner_thread (), j.hj);	/* no filter */
    CHECK (memcmp (&snap, &j.hj->proc.hashjoin.bloom_stats, sizeof (snap)) == 0);
    hjoin_bloom_finish (owner_thread (), NULL);
    join other = late_join (keys);
    j.hj->proc.hashjoin.outer.xasl->hj_bloom = other.second ()->hj_bloom;	/* another join's filter on its input */
    hjoin_bloom_finish (owner_thread (), j.hj);
    CHECK (memcmp (&snap, &j.hj->proc.hashjoin.bloom_stats, sizeof (snap)) == 0);
    CHECK (is_live (other.second ()->hj_bloom));
    j.hj->proc.hashjoin.outer.xasl->hj_bloom = NULL;
    other.hj->type = MERGELIST_PROC;
    hjoin_bloom_finish (owner_thread (), other.hj);
    CHECK (other.second ()->hj_bloom != NULL);
    other.hj->type = HASHJOIN_PROC;
    hjoin_bloom_finish (owner_thread (), other.hj);
    CHECK (other.second ()->hj_bloom == NULL);
  }
}

V5_TEST_CASE (b15_threads_free, "B15 (M): what each thread frees - the owner's input_done frees the owner's key, bits "
	      "and row buffer on its own thread; finish on the hash join's thread frees the filter and never what another "
	      "thread owns (an owner not done yet keeps its blocks); an owner never bound leaves finish to free all; a "
	      "second input's input_done on another thread frees nothing", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 2000);
  size_t live0 = live_count ();
  {
    /* the second input on a job thread: its owner's blocks freed by its input_done; finish then frees the rest */
    join j = late_join (keys);
    first_ends (j);
    for (int k = 0; k < 1100; k++)
      {
	std::vector<DB_VALUE> r = null_row ();
	stage_descr (j.second ()->list_id, r);
	hjoin_bloom_skip_descr (job_thread (), j.second ());	/* the descriptor path: the owner's row buffer */
      }
    unitseam_bloom_view v = view (j.second ());
    REQUIRE (v.owner_thread == (const void *) job_thread ());
    REQUIRE (v.state == C ().ready);
    REQUIRE (v.key != NULL);
    REQUIRE (v.bits != NULL);
    REQUIRE (v.row_buf != NULL);
    hjoin_bloom_input_done (other_thread (), j.second (), true);	/* not the owner's thread: frees nothing */
    CHECK (is_live (v.key));
    CHECK (is_live (v.bits));
    CHECK (is_live (v.row_buf));
    hjoin_bloom_input_done (job_thread (), j.second (), true);
    CHECK_FALSE (is_live (v.key));
    CHECK_FALSE (is_live (v.bits));
    CHECK_FALSE (is_live (v.row_buf));
    unitseam_bloom_view after = view (j.second ());
    CHECK (after.key == NULL);
    CHECK (after.bits == NULL);
    CHECK (after.row_buf == NULL);
    CHECK (after.state == C ().ready);	/* done, not late */
    struct hjoin_bloom *b = j.second ()->hj_bloom;
    hjoin_bloom_finish (owner_thread (), j.hj);
    CHECK (stats (j).outcome == C ().out_checked);
    CHECK_FALSE (is_live (b));
    CHECK (j.first ()->hj_bloom == NULL);
    CHECK (j.second ()->hj_bloom == NULL);
  }
  CHECK (live_count () == live0);
  {
    /* finish before the owner's input_done (an owner on another thread): the owner's blocks are not finish's */
    join j = late_join (keys);
    first_ends (j);
    for (int k = 0; k < 1100; k++)
      {
	REQUIRE (decide (j, null_row (), job_thread ()) != -2);
      }
    unitseam_bloom_view v = view (j.second ());
    REQUIRE (v.bits != NULL);
    hjoin_bloom_finish (owner_thread (), j.hj);
    CHECK (is_live (v.key));
    CHECK (is_live (v.bits));
    CHECK (j.second ()->hj_bloom == NULL);
    /* (the blocks belong to the job thread's heap; the test frees them as that thread would have - those still live) */
    if (is_live (v.bits))
      {
	db_private_free (job_thread (), (void *) v.bits);
      }
    if (is_live (v.key))
      {
	qdata_free_hscan_key (job_thread (), (HASH_SCAN_KEY *) v.key, 1);
      }
  }
  CHECK (live_count () == live0);
  {
    /* an owner never bound (no row came): finish frees all; and the release of the first input on an error path */
    join j = late_join (keys);
    first_ends (j);
    struct hjoin_bloom *b = j.second ()->hj_bloom;
    detach_second (j);
    CHECK_FALSE (is_live (b));
    join k = late_join (keys);
    first_ends (k);
    unitseam_bloom_fill_now (owner_thread (), k.second ());	/* filled on the hash join's thread (owner not bound) */
    REQUIRE (view (k.second ()).bits != NULL);
    hjoin_bloom_release (owner_thread (), k.first ());
    CHECK (k.first ()->hj_bloom == NULL);
    CHECK (k.second ()->hj_bloom == NULL);
  }
  CHECK (live_count () == live0);
}

V5_TEST_CASE (b15_threads, "B15 (M): the first input ends on its own thread while the owner gives rows on another "
	      "(each row about 1.5 us of scan work; the end after a random 0..12 ms, 200 rounds) - a fill reads all 4,000 "
	      "first-input keys once, only past the published threshold 2,000, never before the end; no first-input key "
	      "dropped, every drop one the filled filter makes; an end after the owner's input_done is FIRST_LATE with "
	      "no fill", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::mt19937 rng (4242);
  std::unordered_set<int> member;
  std::vector<int> present;
  while (present.size () < 4000)
    {
      int k = (int) rng ();
      if (member.insert (k).second)
	{
	  present.push_back (k);
	}
    }
  join j = new_join (JOIN_INNER, { dom_int () }, { dom_int () }, { {0, 0} });
  for (int k : present)
    {
      add_row (j.first ()->list_id, { v_int (k) });
    }
  aptr_in_run_order (j);
  fake_list *first = fake_list_of (j.first ()->list_id);
  QFILE_LIST_ID *sl = j.second ()->list_id;
  /* the owner's 6,000 rows: 2,000 first-input keys, 4,000 absent, serialized in advance on this thread */
  std::vector<char *> tuples;
  std::vector<char> is_present, ref;
  {
    std::vector<std::pair<int, bool>> rows;
    for (int i = 0; i < 2000; i++)
      {
	rows.push_back ({ present[i * 2], true });
      }
    while (rows.size () < 6000)
      {
	int x = (int) rng ();
	if (!member.count (x))
	  {
	    rows.push_back ({ x, false });
	  }
      }
    std::shuffle (rows.begin (), rows.end (), rng);
    for (auto &r : rows)
      {
	std::vector<DB_VALUE> v = ints ({ r.first });
	tuples.push_back (make_tuple (sl, v));
	is_present.push_back (r.second);
      }
  }
  /* the filled filter's decision on every row */
  j.first ()->status = XASL_SUCCESS;
  attach_second (j);
  unitseam_bloom_fill_now (owner_thread (), j.second ());
  REQUIRE (view (j.second ()).state == C ().ready);
  for (char *t : tuples)
    {
      QFILE_TUPLE_RECORD r = bound (sl, t);
      int d = unitseam_bloom_check_bound (owner_thread (), j.second (), &r);
      REQUIRE (d >= 0);
      ref.push_back (d == 1);
    }
  hjoin_bloom_release (owner_thread (), j.second ());
  const long long n = (long long) tuples.size ();

  const int rounds = 200;
  int filled = 0, late = 0, ended = 0, bad_outcome = 0, bad_fill = 0, early_fill = 0, fill_after_late = 0;
  long long wrong_drop = 0, false_neg = 0, checked_before_fill = 0;
  for (int round = 0; round < rounds; round++)
    {
      j.first ()->status = XASL_INITIALIZED;
      j.second ()->status = XASL_INITIALIZED;
      attach_raw (j.hj);
      REQUIRE (j.second ()->hj_bloom != NULL);
      int opens_before = first->open_calls;
      long delay_us = (long) (rng () % 12000);
      std::atomic<long long> fill_row (-1);
      std::atomic<int> owner_done (0);
      std::atomic<long long> seen_at_end (-1);
      spin_barrier start (3);
      long long wd = 0, fn = 0, cbf = 0;
      auto owner = [&] ()
      {
	THREAD_ENTRY *t = job_thread ();
	start.wait ();
	for (long long i = 0; i < n; i++)
	  {
	    scan_work ();
	    bool sk = hjoin_bloom_skip_tuple (t, j.second (), tuples[i]);
	    if (fill_row.load () < 0 && view (j.second ()).state == C ().ready)
	      {
		fill_row = i + 1;	/* the row (1-based) whose owner_row filled */
	      }
	    wd += sk && !ref[i];
	    fn += sk && is_present[i];
	    cbf += sk && fill_row.load () < 0;
	  }
	owner_done = 1;
	hjoin_bloom_input_done (t, j.second (), true);
      };
      auto ender = [&] ()
      {
	start.wait ();
	std::this_thread::sleep_for (std::chrono::microseconds (delay_us));
	seen_at_end = view (j.second ()).seen;
	hjoin_bloom_input_done (first_runner (), j.first (), true);
      };
      std::thread to (owner), te (ender);
      start.wait ();
      to.join ();
      te.join ();
      wrong_drop += wd;
      false_neg += fn;
      checked_before_fill += cbf;
      unitseam_bloom_view v = view (j.second ());
      int fills = first->open_calls - opens_before;
      if (v.state == C ().ready)
	{
	  filled++;
	  bad_fill += fills != 1 || v.keys != 4000 || v.threshold != 2000;
	  /* row r fills only if its check (right after seen became r) read the end published, which came after the
	   * ender read seen: r >= that read */
	  early_fill += fill_row.load () < 2000 || fill_row.load () < seen_at_end.load ();
	}
      else if (v.state == C ().off && v.off_reason == C ().out_first_late)
	{
	  late++;
	  fill_after_late += fills != 0;
	}
      else if (v.state == C ().waiting && v.first_done == 1)
	{
	  /* the end published after the owner's last row but before its input_done: due, never filled */
	  ended++;
	  fill_after_late += fills != 0;
	}
      else
	{
	  bad_outcome++;
	  WARN ("round " << round << ": state " << v.state << " reason " << v.off_reason << " seen " << v.seen);
	}
      bool was_ended = v.state == C ().waiting;
      hjoin_bloom_finish (owner_thread (), j.hj);
      REQUIRE (j.second ()->hj_bloom == NULL);
      if (was_ended)
	{
	  CHECK (stats (j).outcome == (C ().early ? C ().out_ended_first : C ().out_not_built));
	}
    }
  WARN ("200 rounds: filled " << filled << ", FIRST_LATE " << late << ", ended unfilled " << ended
	<< ", dropped rows the filled filter keeps "
	<< wrong_drop << ", first-input keys dropped " << false_neg);
  CHECK (bad_outcome == 0);
  CHECK (bad_fill == 0);
  CHECK (early_fill == 0);
  CHECK (fill_after_late == 0);
  CHECK (wrong_drop == 0);
  CHECK (false_neg == 0);
  CHECK (checked_before_fill == 0);
  CHECK (filled > 0);
  CHECK (filled + late + ended == rounds);
  for (char *t : tuples)
    {
      free (t);
    }
}

/* ================================================================ B16 (N): the fill keeps the query's error */

V5E_TEST_CASE (b16_error_frame, "B16 (N): the fill runs in an error stack frame - an error id the query had set before "
	       "the threshold row is kept whether the fill fails (open, scan stop with / without an error id, set-up, "
	       "allocation: OFF_FAILED) or succeeds (READY); with none pending the fill's own error is dropped; the "
	       "poll's fill the same", "[contract]")
{
  boot_types ();
  set_bloom_param (true);
  std::vector<int> keys = int_range (1, 1000);
  const int pending = ER_IT_DATA_OVERFLOW;
  const char *what[] = { "the first list does not open", "its scan stops on S_ERROR with an error id",
    "its scan stops on S_ERROR without an error id", "the second list is not open (set-up)", "an allocation fails",
    "the fill succeeds"
  };
  for (int mode = 0; mode < 6; mode++)
    {
      for (int with_pending = 0; with_pending < 2; with_pending++)
	{
	  INFO (what[mode] << (with_pending ? ", an error pending" : ", no error pending"));
	  join j = int_join_with (keys);
	  fake_list *fl = fake_list_of (j.first ()->list_id);
	  if (mode == 0)
	    {
	      fl->fail_open = true;
	    }
	  else if (mode == 1 || mode == 2)
	    {
	      fl->stop_at = 3;
	      fl->stop_sets_error = mode == 1;
	    }
	  attach_second (j);
	  owner_rows_before_fill (j);
	  QFILE_LIST_ID *sl = j.second ()->list_id;
	  std::vector<DB_VALUE> row = ints ({ 5 });
	  char *t = make_tuple (sl, row);
	  int saved = sl->type_list.type_cnt;
	  if (mode == 3)
	    {
	      sl->type_list.type_cnt = 0;
	    }
	  er_clear ();
	  if (with_pending)
	    {
	      er_set (ER_WARNING_SEVERITY, __FILE__, __LINE__, pending, 1, "INTEGER");
	    }
	  int pushes = fake_er_pushes;
	  if (mode == 4)
	    {
	      fail_nth_alloc (1);
	    }
	  bool s = hjoin_bloom_skip_tuple (owner_thread (), j.second (), t);	/* the threshold row */
	  fail_nth_alloc (0);
	  sl->type_list.type_cnt = saved;
	  free (t);
	  unitseam_bloom_view v = view (j.second ());
	  CHECK (fake_er_pushes == pushes + 1);	/* one frame */
	  CHECK (fake_er_stack.empty ());	/* popped */
	  CHECK (er_errid () == (with_pending ? pending : NO_ERROR));
	  if (mode == 5)
	    {
	      CHECK (v.state == C ().ready);
	      CHECK_FALSE (s);	/* key 5: a first-input key */
	    }
	  else
	    {
	      CHECK (v.state == C ().off);
	      CHECK (v.off_reason == C ().out_off_failed);
	      CHECK_FALSE (s);
	    }
	  er_clear ();
	  hjoin_bloom_release (owner_thread (), j.second ());
	}
    }
  /* the poll's fill */
  {
    join j = int_join_with (keys);
    fake_list_of (j.first ()->list_id)->fail_open = true;
    attach_second (j);
    HJOIN_BLOOM *b = hjoin_bloom_prepare (owner_thread (), j.second ());
    REQUIRE (b != NULL);
    unitseam_bloom_set_seen (j.second (), 1024);
    er_set (ER_WARNING_SEVERITY, __FILE__, __LINE__, pending, 1, "INTEGER");
    hjoin_bloom_poll (owner_thread (), b);
    CHECK (view (j.second ()).off_reason == C ().out_off_failed);
    CHECK (er_errid () == pending);
    er_clear ();
    hjoin_bloom_release (owner_thread (), j.second ());
  }
}
