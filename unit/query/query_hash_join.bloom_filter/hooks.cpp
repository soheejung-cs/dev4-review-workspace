/*
 * unitseam appendix for src/query/query_hash_join.c (compiled at the end of that TU), for
 * unit/query/query_hash_join.bloom_filter (CBRD-27659 prototype: Bloom filter on the second-materialized hash join
 * input). The Bloom struct and its build / decision functions are static in the TU; these hooks reach them, and
 * give the test the join's own key extraction (hjoin_fetch_key) and hash (qdata_hash_scan_key) as the reference.
 *
 * Two layouts of struct hjoin_bloom are served by one appendix:
 *   staged  (e8f629fcc on, detected by HJOIN_BLOOM_SEEN_BATCH): set_up, state WAITING -> READY / OFF, off_reason,
 *           first_rows, threshold, seen; static hjoin_bloom_setup / _fill / _owner_row / _check / _turn_off
 *   earlier (e55956083, fdc3fcd32, 1a7bccdcc): built, disabled; static hjoin_bloom_build / _ready
 * The views below map the earlier layout onto the staged one (built = set up, !built = WAITING, built && !disabled =
 * READY, built && disabled = OFF for a failure, threshold 1: the build ran at the second input's first row), so one
 * test.cpp reads both.
 *
 * v5: two later layouts of the staged struct are told apart by their members (SFINAE, no macro marks them):
 *   concurrent (8ad5ee2df on, member first_done): first_xasl, first_done; the set-up moved into the fill; owner_thread
 *           bound at its first use; hjoin_bloom_input_done / _finish
 *   early      (3726fd95d on, member key_regus): key_regus, key_cast; static hjoin_bloom_fetch_key_values /
 *           _key_absent; the fill in an error stack frame (hjoin_bloom_fill_bits)
 * What exists on one revision only is reached in templates over the struct type, inside if constexpr, so a revision
 * never instantiates the branch it lacks; names the earlier layouts do not have at all stay behind
 * UNITSEAM_BLOOM_STAGED.
 */

#if defined (HJOIN_BLOOM_SEEN_BATCH)
#define UNITSEAM_BLOOM_STAGED 1
#else
#define UNITSEAM_BLOOM_STAGED 0
#endif

/* v5: which members the struct has (8ad5ee2df: first_done; 3726fd95d: key_regus) */
template < typename B, typename = void > struct unitseam_has_first_done
{
  static const bool value = false;
};
template < typename B > struct unitseam_has_first_done <B, decltype ((void) &B::first_done) >
{
  static const bool value = true;
};
template < typename B, typename = void > struct unitseam_has_key_regus
{
  static const bool value = false;
};
template < typename B > struct unitseam_has_key_regus <B, decltype ((void) &B::key_regus) >
{
  static const bool value = true;
};

#define UNITSEAM_BLOOM_CONCURRENT (unitseam_has_first_done < struct hjoin_bloom >::value)
#define UNITSEAM_BLOOM_EARLY (unitseam_has_key_regus < struct hjoin_bloom >::value)

/* stand-ins for the outcomes a revision does not have (never equal to a real one) */
#define UNITSEAM_ABSENT_FIRST_LATE	(-20)
#define UNITSEAM_ABSENT_OFF_FIRST_ROWS	(-21)
#define UNITSEAM_ABSENT_ENDED_FIRST	(-22)

#if UNITSEAM_BLOOM_STAGED
/* the outcomes 8ad5ee2df / 3726fd95d added to enum hashjoin_bloom_outcome, read through the enum's name */
template < typename E, typename = void > struct unitseam_out_first_late
{
  static const int value = UNITSEAM_ABSENT_FIRST_LATE;
};
template < typename E > struct unitseam_out_first_late <E, decltype ((void) E::HASHJOIN_BLOOM_FIRST_LATE) >
{
  static const int value = (int) E::HASHJOIN_BLOOM_FIRST_LATE;
};
template < typename E, typename = void > struct unitseam_out_off_first_rows
{
  static const int value = UNITSEAM_ABSENT_OFF_FIRST_ROWS;
};
template < typename E > struct unitseam_out_off_first_rows <E, decltype ((void) E::HASHJOIN_BLOOM_OFF_FIRST_ROWS) >
{
  static const int value = (int) E::HASHJOIN_BLOOM_OFF_FIRST_ROWS;
};
template < typename E, typename = void > struct unitseam_out_ended_first
{
  static const int value = UNITSEAM_ABSENT_ENDED_FIRST;
};
template < typename E > struct unitseam_out_ended_first <E, decltype ((void) E::HASHJOIN_BLOOM_ENDED_FIRST) >
{
  static const int value = (int) E::HASHJOIN_BLOOM_ENDED_FIRST;
};
#endif

/* the earlier layout's stand-ins for enum hashjoin_bloom_outcome (it has none) */
#define UNITSEAM_EARLIER_NOT_BUILT	(-10)
#define UNITSEAM_EARLIER_CHECKED	(-11)
#define UNITSEAM_EARLIER_OFF_LOW_SKIP	(-12)
#define UNITSEAM_EARLIER_OFF_FAILED	(-13)

/* hjoin_init_domain_info () exactly as hjoin_init_manager () calls it: the common domains of the two key lists */
int
unitseam_hj_init_domain_info (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl)
{
  HASHJOIN_MANAGER manager;

  memset (&manager, 0, sizeof (HASHJOIN_MANAGER));
  manager.merge_info = &hj_xasl->proc.hashjoin.merge_info;
  manager.outer = &hj_xasl->proc.hashjoin.outer;
  manager.inner = &hj_xasl->proc.hashjoin.inner;
  return hjoin_init_domain_info (thread_p, &manager, &hj_xasl->proc.hashjoin.domain_info);
}

/* the TU's constants: the state values, the outcome enum and the staging parameters (earlier: stand-ins) */
struct unitseam_bloom_consts
{
  bool staged;
  int waiting;
  int ready;
  int off;
  int out_not_built;
  int out_checked;
  int out_off_low_skip;
  int out_off_failed;
  long long min_second_rows;
  long long first_per_second;
  long long sample_rows;
  long long min_skip_percent;
  long long seen_batch;
  /* v5 */
  bool concurrent;
  bool early;
  int out_first_late;
  int out_off_first_rows;
  int out_ended_first;
  long long max_first_rows;
};

void
unitseam_bloom_consts_of (unitseam_bloom_consts * c)
{
  memset (c, 0, sizeof (*c));
#if UNITSEAM_BLOOM_STAGED
  c->staged = true;
  c->waiting = HJOIN_BLOOM_WAITING;
  c->ready = HJOIN_BLOOM_READY;
  c->off = HJOIN_BLOOM_OFF;
  c->out_not_built = HASHJOIN_BLOOM_NOT_BUILT;
  c->out_checked = HASHJOIN_BLOOM_CHECKED;
  c->out_off_low_skip = HASHJOIN_BLOOM_OFF_LOW_SKIP;
  c->out_off_failed = HASHJOIN_BLOOM_OFF_FAILED;
  c->min_second_rows = HJOIN_BLOOM_MIN_SECOND_ROWS;
  c->first_per_second = HJOIN_BLOOM_FIRST_PER_SECOND;
  c->sample_rows = HJOIN_BLOOM_SAMPLE_ROWS;
  c->min_skip_percent = HJOIN_BLOOM_MIN_SKIP_PERCENT;
  c->seen_batch = HJOIN_BLOOM_SEEN_BATCH;
  c->out_first_late = unitseam_out_first_late < enum hashjoin_bloom_outcome >::value;
  c->out_off_first_rows = unitseam_out_off_first_rows < enum hashjoin_bloom_outcome >::value;
  c->out_ended_first = unitseam_out_ended_first < enum hashjoin_bloom_outcome >::value;
#else
  c->staged = false;
  c->waiting = 0;
  c->ready = 1;
  c->off = 2;
  c->out_not_built = UNITSEAM_EARLIER_NOT_BUILT;
  c->out_checked = UNITSEAM_EARLIER_CHECKED;
  c->out_off_low_skip = UNITSEAM_EARLIER_OFF_LOW_SKIP;
  c->out_off_failed = UNITSEAM_EARLIER_OFF_FAILED;
  c->min_second_rows = 1;
  c->first_per_second = 0;
  c->sample_rows = 0;
  c->min_skip_percent = 0;
  c->seen_batch = 0;
  c->out_first_late = UNITSEAM_ABSENT_FIRST_LATE;
  c->out_off_first_rows = UNITSEAM_ABSENT_OFF_FIRST_ROWS;
  c->out_ended_first = UNITSEAM_ABSENT_ENDED_FIRST;
#endif
  c->concurrent = UNITSEAM_BLOOM_CONCURRENT;
  c->early = UNITSEAM_BLOOM_EARLY;
#if defined (HJOIN_BLOOM_MAX_FIRST_ROWS)
  c->max_first_rows = HJOIN_BLOOM_MAX_FIRST_ROWS;
#else
  c->max_first_rows = 10000000;	/* the earlier attach's literal bound */
#endif
}

/* the filter's state, read through the second input it is attached to */
struct unitseam_bloom_view
{
  bool present;
  bool staged;
  bool set_up;
  int state;
  long long off_reason;
  long long first_rows;
  long long threshold;
  long long seen;
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
  const void *key_regus;	/* early; NULL earlier */
  const void *key_cast;		/* early; NULL earlier */
};

/* the v5 members, where the struct has them */
template < typename B > static void
unitseam_view_v5 (B * bloom, unitseam_bloom_view * v)
{
  if constexpr (unitseam_has_first_done < B >::value)
    {
      v->first_xasl = bloom->first_xasl;
      v->first_done = bloom->first_done;
    }
  else
    {
      v->first_xasl = NULL;
      v->first_done = -1;
    }
  if constexpr (unitseam_has_key_regus < B >::value)
    {
      v->key_regus = bloom->key_regus;
      v->key_cast = bloom->key_cast;
    }
  else
    {
      v->key_regus = NULL;
      v->key_cast = NULL;
    }
}

void
unitseam_bloom_view_of (XASL_NODE * xasl, unitseam_bloom_view * v)
{
  HJOIN_BLOOM *bloom = xasl->hj_bloom;

  memset (v, 0, sizeof (*v));
  if (bloom == NULL)
    {
      return;
    }
  v->present = true;
#if UNITSEAM_BLOOM_STAGED
  v->staged = true;
  v->set_up = bloom->set_up;
  v->state = (int) bloom->state;
  v->off_reason = bloom->state == HJOIN_BLOOM_OFF ? bloom->off_reason : -1;
  v->first_rows = bloom->first_rows;
  v->threshold = bloom->threshold;
  v->seen = bloom->seen;
#else
  v->staged = false;
  v->set_up = bloom->built;
  v->state = !bloom->built ? 0 : bloom->disabled ? 2 : 1;
  v->off_reason = bloom->built && bloom->disabled ? UNITSEAM_EARLIER_OFF_FAILED : -1;
  v->first_rows = -1;
  v->threshold = 1;
  v->seen = -1;
#endif
  v->first_is_outer = bloom->first_is_outer;
  v->hj_xasl = bloom->hj_xasl;
  v->owner_xasl = bloom->owner_xasl;
  v->owner_thread = bloom->owner_thread;
  v->bits = bloom->bits;
  v->row_buf = bloom->row_buf;
  v->key = bloom->key;
  v->mask = bloom->mask;
  v->keys = bloom->keys;
  v->tested = bloom->tested;
  v->skipped = bloom->skipped;
  unitseam_view_v5 (bloom, v);
}

/*
 * The filter of xasl as the owner has it once the second input has given threshold rows: staged - hjoin_bloom_setup
 * () if not yet, then hjoin_bloom_fill () if still waiting (what hjoin_bloom_owner_row / hjoin_bloom_poll do at the
 * threshold); concurrent (8ad5ee2df on) - hjoin_bloom_fill () alone, which sets up inside; earlier -
 * hjoin_bloom_build () if not yet built (what the first row triggered). Counts no row.
 */
template < typename B > static void
unitseam_fill_now_impl (THREAD_ENTRY * thread_p, B * bloom)
{
#if UNITSEAM_BLOOM_STAGED
  if constexpr (unitseam_has_first_done < B >::value)
    {
      /* concurrent: the fill sets up itself (hjoin_bloom_fill_bits / the fill of 8ad5ee2df); it runs once the first
       * input is done - the caller's attach made it so */
      if (bloom->state == HJOIN_BLOOM_WAITING)
	{
	  hjoin_bloom_fill (thread_p, bloom);
	}
    }
  else
    {
      if (!bloom->set_up)
	{
	  hjoin_bloom_setup (thread_p, bloom);
	}
      if (bloom->state == HJOIN_BLOOM_WAITING)
	{
	  hjoin_bloom_fill (thread_p, bloom);
	}
    }
#else
  if (!bloom->built)
    {
      hjoin_bloom_build (thread_p, bloom);
    }
#endif
}

void
unitseam_bloom_fill_now (THREAD_ENTRY * thread_p, XASL_NODE * xasl)
{
  unitseam_fill_now_impl (thread_p, (struct hjoin_bloom *) xasl->hj_bloom);
}

/* staged only: the shared row count, written as a state injection (earlier: nothing) */
void
unitseam_bloom_set_seen (XASL_NODE * xasl, long long seen)
{
#if UNITSEAM_BLOOM_STAGED
  xasl->hj_bloom->seen = seen;
#else
  (void) xasl;
  (void) seen;
#endif
}

/*
 * The owner's per-row path hjoin_bloom_skip_descr () / hjoin_bloom_skip_tuple () take, on a record the caller bound to
 * the second list's layout (qfile_slot_set_tuple_ptr_and_layout): staged hjoin_bloom_owner_row () (counts the row,
 * sets up at the first, fills at the threshold), earlier hjoin_bloom_ready () (builds at the first row); then
 * hjoin_bloom_skip_record () (counts, and staged: judges the sample).
 *   return: -1 the row is not checked (not the owner, waiting, off), 0 keep it, 1 skip it
 */
int
unitseam_bloom_skip_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record)
{
#if UNITSEAM_BLOOM_STAGED
  HJOIN_BLOOM *bloom = hjoin_bloom_owner_row (thread_p, xasl);
#else
  HJOIN_BLOOM *bloom = hjoin_bloom_ready (thread_p, xasl);
#endif

  if (bloom == NULL)
    {
      return -1;
    }
  return hjoin_bloom_skip_record (thread_p, bloom, record) ? 1 : 0;
}

/*
 * The decision alone - the bits and the key, no row counted, no sample judged - with the owner's key, on a filled
 * filter: staged hjoin_bloom_check (); earlier hjoin_bloom_skip_record () with its two counters put back.
 *   return: -1 the filter is not ready, -2 the key fetch failed, 0 keep, 1 skip
 */
int
unitseam_bloom_check_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record)
{
  HJOIN_BLOOM *bloom = xasl->hj_bloom;

  if (bloom == NULL)
    {
      return -1;
    }
#if UNITSEAM_BLOOM_STAGED
  bool failed = false;
  bool skip;

  if (bloom->state != HJOIN_BLOOM_READY)
    {
      return -1;
    }
  skip = hjoin_bloom_check (thread_p, bloom, bloom->key, record, &failed);
  return failed ? -2 : skip ? 1 : 0;
#else
  INT64 tested, skipped;
  bool skip;

  if (!bloom->built || bloom->disabled)
    {
      return -1;
    }
  tested = bloom->tested;
  skipped = bloom->skipped;
  skip = hjoin_bloom_skip_record (thread_p, bloom, record);
  bloom->tested = tested;
  bloom->skipped = skipped;
  return bloom->disabled ? -2 : skip ? 1 : 0;
#endif
}

/* hjoin_bloom_may_contain () for a hash, on the filter attached to xasl */
bool
unitseam_bloom_may_contain (XASL_NODE * xasl, unsigned int hash)
{
  return hjoin_bloom_may_contain (xasl->hj_bloom, hash);
}

/* what the hash join's bloom_stats says (qdump_print_hashjoin_stats_text prints a BLOOM line when shown) */
struct unitseam_stats_view
{
  bool staged;
  bool shown;			/* staged: attached; earlier: used */
  int outcome;			/* staged: state (enum hashjoin_bloom_outcome); earlier: CHECKED when used, else NOT_BUILT */
  long long keys;
  long long threshold;		/* staged; -1 earlier */
  long long seen;		/* staged; -1 earlier */
  long long tested;
  long long skipped;
};

void
unitseam_bloom_stats_of (XASL_NODE * hj_xasl, unitseam_stats_view * s)
{
  const HASHJOIN_BLOOM_STATS *g = &hj_xasl->proc.hashjoin.bloom_stats;

  memset (s, 0, sizeof (*s));
#if UNITSEAM_BLOOM_STAGED
  s->staged = true;
  s->shown = g->attached;
  s->outcome = g->state;
  s->threshold = g->threshold;
  s->seen = g->seen;
#else
  s->staged = false;
  s->shown = g->used;
  s->outcome = g->used ? UNITSEAM_EARLIER_CHECKED : UNITSEAM_EARLIER_NOT_BUILT;
  s->threshold = -1;
  s->seen = -1;
#endif
  s->keys = g->keys;
  s->tested = g->tested;
  s->skipped = g->skipped;
}

/* a previous execution's counters left in bloom_stats: every field non-zero */
void
unitseam_bloom_stats_stale (XASL_NODE * hj_xasl)
{
  HASHJOIN_BLOOM_STATS *g = &hj_xasl->proc.hashjoin.bloom_stats;

#if UNITSEAM_BLOOM_STAGED
  g->attached = true;
  g->state = HASHJOIN_BLOOM_OFF_LOW_SKIP;
  g->threshold = 8;
  g->seen = 9;
#else
  g->used = true;
#endif
  g->keys = 5;
  g->tested = 6;
  g->skipped = 7;
}

/*
 * What the hash join decides for a first-input row and a second-input row, with the fetch infos hjoin_init_manager ()
 * builds from proc->domain_info (call unitseam_hj_init_domain_info first):
 *   first_null / second_null  hjoin_fetch_key () skipped the row (a NULL key)
 *   first_hash / second_hash  qdata_hash_scan_key (key, UINT_MAX, HASH_METH_IN_MEM) - the in-memory table is keyed by it
 *   key_eq                    qdata_hscan_key_eq (first key, second key)
 *   compare_eq                hjoin_fetch_key (second, ..., compare_key = first key) did not skip: the probe's match test
 *   return: NO_ERROR or the error of a fetch
 */
int
unitseam_hj_join_pair (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, bool first_is_outer, QFILE_TUPLE_RECORD * first_rec,
		       QFILE_TUPLE_RECORD * second_rec, bool *first_null, bool *second_null, unsigned int *first_hash,
		       unsigned int *second_hash, bool *key_eq, bool *compare_eq)
{
  HASHJOIN_PROC_NODE *proc = &hj_xasl->proc.hashjoin;
  HASHJOIN_DOMAIN_INFO *domain_info = &proc->domain_info;
  HASHJOIN_FETCH_INFO first, second;
  HASH_SCAN_KEY *k1, *k2, *k3;
  int key_cnt = proc->merge_info.ls_column_cnt;
  bool skip;
  int error = NO_ERROR;

  memset (&first, 0, sizeof (first));
  memset (&second, 0, sizeof (second));
  first.input = first_is_outer ? &domain_info->outer : &domain_info->inner;
  first.coerce_domains = domain_info->coerce_domains;
  first.need_coerce_domains = domain_info->need_coerce_domains;
  second.input = first_is_outer ? &domain_info->inner : &domain_info->outer;
  second.coerce_domains = domain_info->coerce_domains;
  second.need_coerce_domains = domain_info->need_coerce_domains;

  *first_null = *second_null = *key_eq = *compare_eq = false;
  *first_hash = *second_hash = 0;

  k1 = qdata_alloc_hscan_key (thread_p, key_cnt, true);
  k2 = qdata_alloc_hscan_key (thread_p, key_cnt, true);
  k3 = qdata_alloc_hscan_key (thread_p, key_cnt, true);

  skip = false;
  error = hjoin_fetch_key (thread_p, &first, first_rec, k1, NULL, &skip);
  if (error != NO_ERROR)
    {
      goto end;
    }
  *first_null = skip;

  skip = false;
  error = hjoin_fetch_key (thread_p, &second, second_rec, k2, NULL, &skip);
  if (error != NO_ERROR)
    {
      goto end;
    }
  *second_null = skip;

  if (!*first_null)
    {
      *first_hash = qdata_hash_scan_key (k1, UINT_MAX, HASH_METH_IN_MEM);
    }
  if (!*second_null)
    {
      *second_hash = qdata_hash_scan_key (k2, UINT_MAX, HASH_METH_IN_MEM);
    }
  if (!*first_null && !*second_null)
    {
      *key_eq = qdata_hscan_key_eq (k1, k2) != 0;
      skip = false;
      error = hjoin_fetch_key (thread_p, &second, second_rec, k3, k1, &skip);
      if (error != NO_ERROR)
	{
	  goto end;
	}
      *compare_eq = !skip;
    }

end:
  qdata_free_hscan_key (thread_p, k1, key_cnt);
  qdata_free_hscan_key (thread_p, k2, key_cnt);
  qdata_free_hscan_key (thread_p, k3, key_cnt);
  return error;
}

/*
 * hjoin_init_manager () as qexec_hash_join () calls it - after the aptr loop, so after hjoin_bloom_detach () - then
 * only the manager's own type list is freed (hjoin_clear_manager () would also close and destroy the input lists).
 */
int
unitseam_hj_init_manager (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl)
{
  HASHJOIN_MANAGER manager;
  int error;

  error = hjoin_init_manager (thread_p, &manager, hj_xasl, (QUERY_ID) 1, NULL);
  if (manager.type_list.domp != NULL)
    {
      db_private_free_and_init (thread_p, manager.type_list.domp);
    }
  return error;
}

/*
 * v3 (1a7bccdcc): a parallel scan worker's view of the shared filter - struct hjoin_bloom_worker, which exists from
 * 1a7bccdcc on. On an earlier revision the declaration below only introduces an incomplete type, the partial
 * specialization drops out (sizeof of an incomplete type) and the view is empty: the suite still compiles there, and
 * test.cpp registers its worker cases only when the worker API links (weak references). Staged (e8f629fcc on): the
 * worker also caches the state as of its last batch (ready) and counts the rows it has not handed over yet
 * (seen_unflushed); earlier a worker always checked (ready = true, seen_unflushed = -1).
 */
struct hjoin_bloom_worker;

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
  /* v5 (3726fd95d): the key columns' regu variables the worker found in its clone's output list; NULL earlier */
  const void *key_regu0;
  const void *key_cast;
};

template < typename W, typename = void >struct unitseam_worker_access
{
  static void view (const void *, unitseam_worker_view * v)
  {
    memset (v, 0, sizeof (*v));
  }
};

template < typename W > struct unitseam_worker_access <W, decltype ((void) sizeof (W)) >
{
  static void view (const void *p, unitseam_worker_view * v)
  {
    const W *w = (const W *) p;

    memset (v, 0, sizeof (*v));
    if (w == NULL)
      {
	return;
      }
    v->present = true;
    v->bloom = w->bloom;
    v->key = w->key;
    v->row_buf = w->row_buf;
    v->row_buf_size = w->row_buf_size;
    v->disabled = w->disabled;
#if UNITSEAM_BLOOM_STAGED
    v->ready = w->ready;
    v->seen_unflushed = w->seen_unflushed;
#else
    v->ready = true;
    v->seen_unflushed = -1;
#endif
    v->tested = w->tested;
    v->skipped = w->skipped;
    if constexpr (unitseam_has_key_regus < W >::value)
      {
	v->key_regu0 = w->key_regus != NULL ? w->key_regus[0] : NULL;
	v->key_cast = w->key_cast;
      }
  }
};

void
unitseam_worker_view_of (const void *worker, unitseam_worker_view * v)
{
  unitseam_worker_access < struct hjoin_bloom_worker >::view (worker, v);
}

/* ================================================================ v5 (8ad5ee2df, 3726fd95d) */

/* 3726fd95d: the regu variable attach found for key k in the checked input's output list; NULL when the filter has none
 * (a computed key, or a revision without the check before the projection) */
template < typename B > static const void *
unitseam_key_regu_impl (B * bloom, int k)
{
  if constexpr (unitseam_has_key_regus < B >::value)
    {
      return (bloom != NULL && bloom->key_regus != NULL) ? (const void *) bloom->key_regus[k] : NULL;
    }
  else
    {
      return NULL;
    }
}

const void *
unitseam_bloom_key_regu (XASL_NODE * xasl, int k)
{
  return unitseam_key_regu_impl ((struct hjoin_bloom *) xasl->hj_bloom, k);
}

#if UNITSEAM_BLOOM_STAGED
/*
 * 3726fd95d: the owner's check of a row before its projection, taken apart - hjoin_bloom_fetch_key_values () into the
 * owner's key (each key_cast slot set to NULL first, so cast_mask says which keys went through it: a value not of its
 * list column's domain), the key's hash qdata_hash_scan_key (key, UINT_MAX, HASH_METH_IN_MEM), and on a filled filter
 * the decision of the bits (hjoin_bloom_key_absent). Counts no row and judges no sample (what hjoin_bloom_check_values
 * does, without the error clear).
 *   return: -3 no key_regus (a computed key, or a revision without it), -4 not set up, -2 the fetch failed (its error id
 *	     left as set), 1 a NULL key, 0 a key; decision -1 not ready, 0 keep, 1 skip
 */
template < typename B > static int
unitseam_early_key_impl (THREAD_ENTRY * thread_p, B * bloom, VAL_DESCR * vd, unsigned int *hash, int *cast_mask,
			 int *decision)
{
  *hash = 0;
  *cast_mask = 0;
  *decision = -1;
  if constexpr (unitseam_has_key_regus < B >::value)
    {
      bool skip = false;
      int k, key_cnt;

      if (bloom == NULL || bloom->key_regus == NULL)
	{
	  return -3;
	}
      if (!bloom->set_up || bloom->key == NULL || bloom->key_cast == NULL)
	{
	  return -4;
	}
      key_cnt = bloom->hj_xasl->proc.hashjoin.merge_info.ls_column_cnt;
      for (k = 0; k < key_cnt; k++)
	{
	  pr_clear_value (&bloom->key_cast[k]);
	  db_make_null (&bloom->key_cast[k]);
	}
      if (hjoin_bloom_fetch_key_values (thread_p, &bloom->second, bloom->key_regus, bloom->key_cast, vd, bloom->key,
					&skip) != NO_ERROR)
	{
	  return -2;
	}
      for (k = 0; k < key_cnt; k++)
	{
	  if (!DB_IS_NULL (&bloom->key_cast[k]))
	    {
	      *cast_mask |= 1 << k;
	    }
	}
      if (skip)
	{
	  *decision = bloom->state == HJOIN_BLOOM_READY ? 1 : -1;
	  return 1;
	}
      *hash = qdata_hash_scan_key (bloom->key, UINT_MAX, HASH_METH_IN_MEM);
      if (bloom->state == HJOIN_BLOOM_READY)
	{
	  *decision = hjoin_bloom_key_absent (bloom, bloom->key) ? 1 : 0;
	}
      return 0;
    }
  else
    {
      (void) thread_p;
      (void) bloom;
      (void) vd;
      return -3;
    }
}

int
unitseam_bloom_early_key (THREAD_ENTRY * thread_p, XASL_NODE * xasl, VAL_DESCR * vd, unsigned int *hash,
			  int *cast_mask, int *decision)
{
  return unitseam_early_key_impl (thread_p, (struct hjoin_bloom *) xasl->hj_bloom, vd, hash, cast_mask, decision);
}
#else
int
unitseam_bloom_early_key (THREAD_ENTRY *, XASL_NODE *, VAL_DESCR *, unsigned int *hash, int *cast_mask, int *decision)
{
  *hash = 0;
  *cast_mask = 0;
  *decision = -1;
  return -3;
}
#endif

/*
 * The join's own key of a second-input row read from its list: hjoin_fetch_key () with the fetch info
 * hjoin_init_manager () builds from proc->domain_info (call unitseam_hj_init_domain_info first), then
 * qdata_hash_scan_key (key, UINT_MAX, HASH_METH_IN_MEM) - what the in-memory table and the filled bits are keyed by.
 *   return: -1 the fetch failed, 1 a NULL key (the join skips the row), 0 a key (hash set)
 */
int
unitseam_hj_second_key (THREAD_ENTRY * thread_p, XASL_NODE * hj_xasl, bool first_is_outer, QFILE_TUPLE_RECORD * record,
			unsigned int *hash)
{
  HASHJOIN_PROC_NODE *proc = &hj_xasl->proc.hashjoin;
  HASHJOIN_DOMAIN_INFO *domain_info = &proc->domain_info;
  HASHJOIN_FETCH_INFO second;
  HASH_SCAN_KEY *key;
  int key_cnt = proc->merge_info.ls_column_cnt;
  bool skip = false;
  int rc;

  memset (&second, 0, sizeof (second));
  second.input = first_is_outer ? &domain_info->inner : &domain_info->outer;
  second.coerce_domains = domain_info->coerce_domains;
  second.need_coerce_domains = domain_info->need_coerce_domains;
  *hash = 0;

  key = qdata_alloc_hscan_key (thread_p, key_cnt, true);
  if (key == NULL)
    {
      return -1;
    }
  rc = hjoin_fetch_key (thread_p, &second, record, key, NULL, &skip) != NO_ERROR ? -1 : skip ? 1 : 0;
  if (rc == 0)
    {
      *hash = qdata_hash_scan_key (key, UINT_MAX, HASH_METH_IN_MEM);
    }
  qdata_free_hscan_key (thread_p, key, key_cnt);
  return rc;
}
