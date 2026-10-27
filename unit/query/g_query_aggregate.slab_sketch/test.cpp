/*
 * PR#8196 (CBRD-27361) -- the hash GROUP BY's group slab and distinct-key sketch, src/query/query_aggregate.cpp (server).
 *
 * qdata_slab_new_group () puts a new group -- its key, key values, value, accumulators and their values, and a first
 * tuple up to inline_cap bytes -- in one block of the context's fixed-size heap; qdata_free_agg_hentry () gives the
 * key's values back first and then the block (the free-list link overwrites the key header).  The sketch
 * (qdata_key_sketch_*) is a HyperLogLog of 1,024 one-byte registers over the key's full-range hash
 * (qdata_hash_agg_hkey (key, 0xffffffffu)), re-mixed, top 10 bits the register, the run of zeros in the low 22 bits
 * the rank.
 *
 * Contracts held here (review G group, 2026-10-27):
 *   S1-S5  the slab: a group's key and value share one block, the key sits at value - off_value, the accumulators are
 *          set up as qdata_alloc_agg_hvalue () sets them, a first tuple within inline_cap stays in the block and a
 *          longer one is a private allocation the release frees, a released block is handed to the next group (LIFO);
 *          what a group costs in private allocations on each path (current numbers, the review's evidence).
 *   K1-K5  the sketch: the rank is clz ((mix << 10) | 0x200) + 1 at register mix >> 22 (a branchless form of the
 *          loop), adding a key again or in another order leaves the registers as they are (so feeding it once per
 *          group is the same as once per row), merge is the union, and the estimate of n distinct INTEGER keys hashed
 *          the way the row path hashes them is within 4 standard errors (4 x 1.04 / 32).
 *
 * Real code: query_aggregate.cpp (unit under test), object_primitive.c + db_macro.c (DB_VALUE clone / clear),
 * memory_hash.c (mht_get_hash_number), fixed_alloc.c + heaplayers/customheaps.cpp (the slab's fixed heap).
 * Faked: db_private_alloc/free/realloc_release (malloc with counters), db_alignment / db_align_to (memory_alloc.c's), er_set / er_errid / er_clear (records the
 * code), prm_Def (booted defaults the DB_VALUE code reads).
 */
#include "catch.hpp"

#include "dbtype.h"
#include "error_manager.h"
#include "system_parameter.h"
#include "query_aggregate.hpp"
#include "xasl_aggregate.hpp"
#include "query_list.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <vector>

/* ---------------------------------------------------------------- fakes */

static int g_errid = NO_ERROR;
void
er_set (int severity, const char *file_name, const int line_no, int err_id, int num_args, ...)
{
  (void) severity;
  (void) file_name;
  (void) line_no;
  (void) num_args;
  g_errid = err_id;
}

int
er_errid (void)
{
  return g_errid;
}

void
er_clear (void)
{
  g_errid = NO_ERROR;
}

SYSPRM_PARAM prm_Def[PRM_LAST_ID + 1] = { };
static bool
boot_params ()
{
  prm_Def[PRM_ID_ORACLE_STYLE_EMPTY_STRING].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_STYLE_EMPTY_STRING].value.v.b = false;
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].datatype = PRM_BOOLEAN;
  prm_Def[PRM_ID_ORACLE_COMPAT_NUMBER_BEHAVIOR].value.v.b = false;
  return true;
}
static const bool params_booted = boot_params ();

/* the thread's private heap: plain malloc, counted */
static long g_private_allocs = 0;
static long g_private_frees = 0;

void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  g_private_allocs++;
  return malloc (size > 0 ? size : 1);
}

void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  if (ptr != NULL)
    {
      g_private_frees++;
      free (ptr);
    }
}

void *
db_private_realloc_release (THREAD_ENTRY * thrd, void *ptr, size_t size, bool rc_track)
{
  (void) thrd;
  (void) rc_track;
  if (ptr == NULL)
    {
      g_private_allocs++;
    }
  return realloc (ptr, size > 0 ? size : 1);
}

/* memory_alloc.c (not linked): the alignment helpers fixed_alloc.c's record size takes, as memory_alloc.c has them */
int
db_alignment (int n)
{
  if (n >= (int) sizeof (double))
    {
      return (int) sizeof (double);
    }
  else if (n >= (int) sizeof (void *))
    {
      return (int) sizeof (void *);
    }
  else if (n >= (int) sizeof (int))
    {
      return (int) sizeof (int);
    }
  else if (n >= (int) sizeof (short))
    {
      return (int) sizeof (short);
    }
  return 1;
}

int
db_align_to (int n, int alignment)
{
  return (n + alignment - 1) & ~(alignment - 1);
}

/* the appendix (hooks.cpp): agg_group_slab is the TU's own struct */
void unitseam_slab_layout (const agg_group_slab * slab, size_t * block_size, size_t * off_keyptrs,
			   size_t * off_keyvals, size_t * off_value, size_t * off_accs, size_t * off_accvals,
			   size_t * off_tuple, int *inline_cap);

/* ---------------------------------------------------------------- helpers */

struct agg_nodes
{
  std::vector < cubxasl::aggregate_list_node > n;

  explicit agg_nodes (const std::vector < FUNC_CODE > &funcs, int shared_from_of_second = 0)
  {
    n.resize (funcs.size ());
    for (size_t i = 0; i < funcs.size (); i++)
      {
	memset ((void *) &n[i], 0, sizeof (n[i]));
	n[i].function = funcs[i];
	n[i].next = (i + 1 < funcs.size ())? &n[i + 1] : NULL;
      }
    if (funcs.size () > 1)
      {
	n[1].accumulator.shared_from = shared_from_of_second;
      }
  }
  cubxasl::aggregate_list_node *head ()
  {
    return n.empty ()? NULL : &n[0];
  }
};

/* a scan-side key of one INTEGER, as qexec_alloc_agg_hash_context () makes temp_key (values not owned) */
struct int_key
{
  DB_VALUE v;
  DB_VALUE *vp;
  cubquery::aggregate_hash_key k;

  explicit int_key (int x)
  {
    db_make_int (&v, x);
    vp = &v;
    k.val_count = 1;
    k.free_values = false;
    k.in_slab = false;
    k.values = &vp;
  }
};

/* lowbias32, the re-mix qdata_key_sketch_add_hashed () applies to the key's hash (a copy: the test predicts the
 * register and rank the function must set) */
static unsigned int
mix (unsigned int h)
{
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return h;
}

static unsigned int
inv_mul (unsigned int a)
{
  unsigned int x = a;		/* Newton: x = x * (2 - a * x), 5 steps reach 32 bits for odd a */

  for (int i = 0; i < 5; i++)
    {
      x *= 2u - a * x;
    }
  return x;
}

static unsigned int
unmix (unsigned int m)
{
  m ^= m >> 16;
  m *= inv_mul (0x846ca68bu);
  m ^= (m >> 15) ^ (m >> 30);
  m *= inv_mul (0x7feb352du);
  m ^= m >> 16;
  return m;
}

/* the branchless rank the review proposes: 1 + the zeros above the first one in the low 22 bits, 23 for none */
static unsigned char
rank_clz (unsigned int m)
{
  return (unsigned char) (__builtin_clz ((m << 10) | 0x200u) + 1);
}

static double
sketch_of_int_keys (unsigned char *regs, long n, long stride, long offset)
{
  memset (regs, 0, QDATA_KEY_SKETCH_REGS);
  for (long i = 0; i < n; i++)
    {
      int_key key ((int) (offset + i * stride));
      /* the row path: one full-range hash, the sketch takes it (query_executor.c qexec_hash_gby_agg_tuple ()) */
      qdata_key_sketch_add_hashed (regs, qdata_hash_agg_hkey (&key.k, 0xffffffffu));
    }
  return qdata_key_sketch_estimate (regs);
}

/* ---------------------------------------------------------------- the slab */

TEST_CASE ("S1 a group's key and value live in one block: key in_slab, value->slab set, the key at value - off_value; the "
	   "key value is a copy", "[contract]")
{
  agg_nodes aggs ({PT_SUM});
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 1, 112);
  REQUIRE (slab != NULL);
  size_t bs, okp, okv, ov, oa, oav, ot;
  int cap;
  unitseam_slab_layout (slab, &bs, &okp, &okv, &ov, &oa, &oav, &ot, &cap);

  int_key key (42);
  cubquery::aggregate_hash_key * k = NULL;
  cubquery::aggregate_hash_value * v = NULL;
  REQUIRE (qdata_slab_new_group (NULL, slab, &key.k, aggs.head (), 56, &k, &v) == NO_ERROR);
  CHECK (k->in_slab);
  CHECK (!k->free_values);
  CHECK (v->slab == slab);
  CHECK ((char *) k == (char *) v - ov);	/* what an accessor qdata_slab_group_key (value) would return */
  CHECK (k->values[0] != &key.v);
  CHECK (db_get_int (k->values[0]) == 42);
  CHECK (v->batch_slot == -1);
  CHECK (v->first_tuple.tpl == (char *) k + ot);	/* 56 <= inline_cap: in the block */
  CHECK (v->first_tuple.size == 56);
  qdata_free_agg_hentry (k, v, NULL);
  qdata_slab_destroy (NULL, slab);
}

TEST_CASE ("S2 the accumulators start as qdata_alloc_agg_hvalue () starts them: COUNT and COUNT(*) a BIGINT 0, SUM NULL, "
	   "curr_cnt 0, sum_acc inactive, sharing inherited from the list", "[contract]")
{
  agg_nodes aggs ({PT_COUNT_STAR, PT_AVG, PT_COUNT, PT_SUM}, 4);	/* AVG shares SUM's accumulator (1 + index 3) */
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 4, 0);
  REQUIRE (slab != NULL);
  int_key key (7);
  cubquery::aggregate_hash_key * k = NULL;
  cubquery::aggregate_hash_value * v = NULL;
  REQUIRE (qdata_slab_new_group (NULL, slab, &key.k, aggs.head (), 0, &k, &v) == NO_ERROR);
  REQUIRE (v->func_count == 4);
  CHECK (DB_VALUE_TYPE (v->accumulators[0].value) == DB_TYPE_BIGINT);
  CHECK (db_get_bigint (v->accumulators[0].value) == 0);
  CHECK (DB_IS_NULL (v->accumulators[1].value));
  CHECK (v->accumulators[1].shared_from == 4);
  CHECK (DB_VALUE_TYPE (v->accumulators[2].value) == DB_TYPE_BIGINT);
  CHECK (DB_IS_NULL (v->accumulators[3].value));
  for (int i = 0; i < 4; i++)
    {
      CHECK (v->accumulators[i].curr_cnt == 0);
      CHECK (!v->accumulators[i].sum_acc.is_active);
      CHECK (DB_IS_NULL (v->accumulators[i].value2));
    }
  CHECK (v->first_tuple.tpl == NULL);
  qdata_free_agg_hentry (k, v, NULL);
  qdata_slab_destroy (NULL, slab);
}

TEST_CASE ("S3 a first tuple longer than inline_cap is a private allocation of its own, and the release frees it",
	   "[contract]")
{
  agg_nodes aggs ({PT_SUM});
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 1, 64);
  REQUIRE (slab != NULL);
  int_key key (1);
  cubquery::aggregate_hash_key * k = NULL;
  cubquery::aggregate_hash_value * v = NULL;
  long a0 = g_private_allocs, f0 = g_private_frees;
  REQUIRE (qdata_slab_new_group (NULL, slab, &key.k, aggs.head (), 200, &k, &v) == NO_ERROR);
  CHECK (g_private_allocs - a0 == 1);
  CHECK (v->first_tuple.size == 200);
  qdata_free_agg_hentry (k, v, NULL);
  CHECK (g_private_frees - f0 == 1);
  qdata_slab_destroy (NULL, slab);
}

TEST_CASE ("S4 a released block is the next group's (LIFO): in eviction mode the next new group gets the block the LRU "
	   "head just gave back", "[contract]")
{
  agg_nodes aggs ({PT_SUM});
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 1, 112);
  REQUIRE (slab != NULL);
  int_key a (1), b (2), c (3);
  cubquery::aggregate_hash_key * ka, *kb, *kc;
  cubquery::aggregate_hash_value * va, *vb, *vc;
  REQUIRE (qdata_slab_new_group (NULL, slab, &a.k, aggs.head (), 56, &ka, &va) == NO_ERROR);
  REQUIRE (qdata_slab_new_group (NULL, slab, &b.k, aggs.head (), 56, &kb, &vb) == NO_ERROR);
  CHECK (ka != kb);
  qdata_free_agg_hentry (ka, va, NULL);	/* the oldest group leaves the table (qexec_hash_gby_agg_tuple ()'s LRU loop) */
  REQUIRE (qdata_slab_new_group (NULL, slab, &c.k, aggs.head (), 56, &kc, &vc) == NO_ERROR);
  CHECK (kc == ka);
  CHECK (db_get_int (kc->values[0]) == 3);
  qdata_free_agg_hentry (kb, vb, NULL);
  qdata_free_agg_hentry (kc, vc, NULL);
  qdata_slab_destroy (NULL, slab);
}

TEST_CASE ("S5 current: a group costs no private allocation in the slab; the generic path takes 8 for one INTEGER key and "
	   "one aggregate, and the batch key cache's copy of the key takes 3 of them back", "[contract]")
{
  agg_nodes aggs ({PT_SUM});
  int_key key (5);
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 1, 112);
  REQUIRE (slab != NULL);
  cubquery::aggregate_hash_key * k = NULL;
  cubquery::aggregate_hash_value * v = NULL;
  long a0 = g_private_allocs;
  REQUIRE (qdata_slab_new_group (NULL, slab, &key.k, aggs.head (), 56, &k, &v) == NO_ERROR);
  CHECK (g_private_allocs - a0 == 0);

  /* the generic path of qexec_hash_gby_agg_tuple (): the key copy, the value, the first tuple */
  long a1 = g_private_allocs;
  cubquery::aggregate_hash_key * gk = qdata_copy_agg_hkey (NULL, &key.k);
  cubquery::aggregate_hash_value * gv = qdata_alloc_agg_hvalue (NULL, 1, aggs.head ());
  void *tpl = db_private_alloc (NULL, 56);
  long generic = g_private_allocs - a1;
  CHECK (generic == 8);
  db_private_free (NULL, tpl);
  long f1 = g_private_frees;
  qdata_free_agg_hkey (NULL, gk);
  qdata_free_agg_hvalue (NULL, gv);
  CHECK (g_private_frees - f1 == 7);

  /* the page batch's key cache (query_executor.c qexec_gby_cache_add ()) copies the group's key per new group */
  long a2 = g_private_allocs, f2 = g_private_frees;
  cubquery::aggregate_hash_key * ck = qdata_copy_agg_hkey (NULL, &key.k);
  CHECK (g_private_allocs - a2 == 3);
  qdata_free_agg_hkey (NULL, ck);
  CHECK (g_private_frees - f2 == 3);

  qdata_free_agg_hentry (k, v, NULL);
  qdata_slab_destroy (NULL, slab);
}

TEST_CASE ("S6 current layout: one INTEGER key and one aggregate -- the part a row touches (key, value, accumulator) is "
	   "248 bytes, the block 488 with a 112-byte inline tuple area, not a multiple of 64", "[contract]")
{
  agg_group_slab *slab = qdata_slab_create (NULL, 1, 1, 112);
  REQUIRE (slab != NULL);
  size_t bs, okp, okv, ov, oa, oav, ot;
  int cap;
  unitseam_slab_layout (slab, &bs, &okp, &okv, &ov, &oa, &oav, &ot, &cap);
  CHECK (okp == 16);
  CHECK (okv == 24);
  CHECK (ov == 88);
  CHECK (oa == 168);
  CHECK (oav == 248);
  CHECK (ot == 376);
  CHECK (cap == 112);
  CHECK (bs == 488);
  CHECK (oa + sizeof (cubxasl::aggregate_accumulator) == 248);
  CHECK (bs % 64 != 0);
  qdata_slab_destroy (NULL, slab);

  /* Q1's shape: two keys, eight aggregates */
  slab = qdata_slab_create (NULL, 2, 8, 368);
  REQUIRE (slab != NULL);
  unitseam_slab_layout (slab, &bs, &okp, &okv, &ov, &oa, &oav, &ot, &cap);
  CHECK (oa == 240);
  CHECK (oav == 880);
  CHECK (ot == 1904);
  CHECK (bs == 2272);
  qdata_slab_destroy (NULL, slab);
}

/* ---------------------------------------------------------------- the sketch */

TEST_CASE ("K1 the register is mix (h) >> 22 and the rank is clz ((mix (h) << 10) | 0x200) + 1 -- the loop's result for "
	   "every rank, 23 included", "[contract]")
{
  static unsigned char regs[QDATA_KEY_SKETCH_REGS];
  std::mt19937 rng (8196);
  long checked = 0;

  memset (regs, 0, sizeof (regs));
  /* each rank 1..23 at a few registers, from a mixed value built for it */
  for (unsigned int r = 1; r <= 23; r++)
    {
      for (unsigned int idx = 0; idx < QDATA_KEY_SKETCH_REGS; idx += 341)
	{
	  unsigned int low = (r <= 22) ? (1u << (22 - r)) : 0u;	/* the first one after r - 1 zeros */
	  unsigned int m = (idx << 22) | low | ((r <= 21) ? (rng () & ((1u << (22 - r)) - 1)) : 0u);
	  unsigned int h = unmix (m);

	  REQUIRE (mix (h) == m);
	  qdata_key_sketch_add_hashed (regs, h);
	  CHECK ((int) regs[idx] == (int) r);
	  CHECK ((int) rank_clz (m) == (int) r);
	  regs[idx] = 0;
	  checked++;
	}
    }
  /* and random hashes */
  for (int t = 0; t < 2000000; t++)
    {
      unsigned int h = rng ();
      unsigned int m = mix (h);
      unsigned int idx = m >> 22;

      qdata_key_sketch_add_hashed (regs, h);
      if (regs[idx] != rank_clz (m))
	{
	  FAIL ("rank differs at h=" << h);
	}
      regs[idx] = 0;
      checked++;
    }
  for (int i = 0; i < QDATA_KEY_SKETCH_REGS; i++)
    {
      REQUIRE (regs[i] == 0);	/* no other register was touched */
    }
  CHECK (checked > 2000000);
}

TEST_CASE ("K2 adding a key again, or the keys in another order, leaves the registers as they are -- feeding the sketch "
	   "once per group gives the registers feeding it once per row gives", "[contract]")
{
  static unsigned char per_row[QDATA_KEY_SKETCH_REGS], per_group[QDATA_KEY_SKETCH_REGS];
  std::mt19937 rng (27758);
  std::vector < unsigned int >rows;
  std::set < unsigned int >seen;
  std::vector < unsigned int >first_seen;

  for (int i = 0; i < 200000; i++)
    {
      unsigned int key = rng () % 30000;	/* 30,000 groups, rows arriving in random order with repeats */
      rows.push_back (key);
      if (seen.insert (key).second)
	{
	  first_seen.push_back (key);
	}
    }
  memset (per_row, 0, sizeof (per_row));
  memset (per_group, 0, sizeof (per_group));
  for (unsigned int key:rows)
    {
      int_key k ((int) key);
      qdata_key_sketch_add_hashed (per_row, qdata_hash_agg_hkey (&k.k, 0xffffffffu));
    }
  for (size_t i = first_seen.size (); i-- > 0;)	/* the groups' creations, in reverse */
    {
      int_key k ((int) first_seen[i]);
      qdata_key_sketch_add_hashed (per_group, qdata_hash_agg_hkey (&k.k, 0xffffffffu));
    }
  CHECK (memcmp (per_row, per_group, QDATA_KEY_SKETCH_REGS) == 0);
}

TEST_CASE ("K3 merging two sketches gives the sketch of the union (register max): a parallel scan's workers merged into "
	   "the leader equal one serial scan", "[contract]")
{
  static unsigned char a[QDATA_KEY_SKETCH_REGS], b[QDATA_KEY_SKETCH_REGS], u[QDATA_KEY_SKETCH_REGS];

  memset (a, 0, sizeof (a));
  memset (b, 0, sizeof (b));
  memset (u, 0, sizeof (u));
  for (int i = 0; i < 150000; i++)
    {
      int_key k (i);
      unsigned int h = qdata_hash_agg_hkey (&k.k, 0xffffffffu);
      qdata_key_sketch_add_hashed ((i % 3 == 0) ? a : b, h);	/* overlapping split is not needed: disjoint parts */
      qdata_key_sketch_add_hashed (u, h);
    }
  for (int i = 100000; i < 120000; i++)
    {
      int_key k (i);
      qdata_key_sketch_add_hashed (a, qdata_hash_agg_hkey (&k.k, 0xffffffffu));	/* and some keys both saw */
    }
  qdata_key_sketch_merge (a, b);
  CHECK (memcmp (a, u, QDATA_KEY_SKETCH_REGS) == 0);
}

TEST_CASE ("K4 the estimate of n distinct INTEGER keys hashed as the row path hashes them is within 4 standard errors "
	   "(13%): 1,000 / 30,000 / 1,000,000 / 15,000,000 (TPC-H SF10 l_orderkey)", "[contract]")
{
  static unsigned char regs[QDATA_KEY_SKETCH_REGS];
  const double tol = 4 * 1.04 / sqrt ((double) QDATA_KEY_SKETCH_REGS);
  struct
  {
    long n, stride, offset;
  } runs[] =
  {
    {1000, 1, 0}, {30000, 7, 3}, {1000000, 1, 1}, {15000000, 4, 1}
  };

  for (auto & r:runs)
    {
      double est = sketch_of_int_keys (regs, r.n, r.stride, r.offset);
      double rel = fabs (est - (double) r.n) / (double) r.n;

      INFO ("n=" << r.n << " estimate=" << est << " relative error=" << rel);
      CHECK (rel < tol);
    }
}

TEST_CASE ("K5 a key added once more after the registers settled changes nothing: the sketch is idempotent per key",
	   "[contract]")
{
  static unsigned char regs[QDATA_KEY_SKETCH_REGS], again[QDATA_KEY_SKETCH_REGS];

  sketch_of_int_keys (regs, 50000, 1, 0);
  memcpy (again, regs, sizeof (regs));
  for (int i = 0; i < 50000; i += 3)
    {
      int_key k (i);
      qdata_key_sketch_add_hashed (again, qdata_hash_agg_hkey (&k.k, 0xffffffffu));
    }
  CHECK (memcmp (regs, again, QDATA_KEY_SKETCH_REGS) == 0);
}
