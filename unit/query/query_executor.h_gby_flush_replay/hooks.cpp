/* unitseam appendix to src/query/query_executor.c (same TU): the page batch hash GROUP BY node's flush and cache
 * (qexec_batch_gby_flush (), qexec_gby_cache_add (), qexec_gby_cache_find ()), driven by hand.
 *
 * Fakes for the other translation units the two functions reach (each mimics only what these functions read back):
 *   qdata_evaluate_aggregate_batch ()  records the selection it is handed (batch->sel / n_sel) and the accumulators
 *   qdata_get_agg_hvalue_size ()        0: the accumulators the batch takes never change size (fixed-width types)
 *   mht_get ()                          records the key it is asked (the LRU touch order) and returns h_get_result
 *   qdata_save_agg_htable_to_list ()    counts the dump of an abandon
 *   qdata_copy_agg_hkey () / qdata_free_agg_hkey ()  count the cache's key copies and their releases (calloc, never
 *                                       freed: a test-sized leak)
 *   db_private_*_release ()             malloc / realloc / free (no thread entry, no private heap here) */

#define H_MAX_ROWS 256
#define H_MAX_CALLS 64

struct h_flush_result
{
  int done;
  int n_calls;			/* qdata_evaluate_aggregate_batch () calls */
  int call_group[H_MAX_CALLS];	/* the group whose accumulators the call took */
  int call_n[H_MAX_CALLS];
  int call_sel[H_MAX_CALLS][H_MAX_ROWS];
  int n_touch;			/* mht_get () calls */
  int touch_group[H_MAX_CALLS];
  int off;
  int dumps;
  int syncs;
  int tuple_count_after;
  int state_after;
  int evictions_after;
  int n_cached_after;
  int value_tuple_count[H_MAX_CALLS];
  int counts_reset;		/* every cache entry's count is 0 again */
  int sel_restored;		/* batch->sel / n_sel are the caller's again */
};

static const void *h_get_keys[H_MAX_CALLS * 4];
static int h_n_get;
static void *h_get_result;
static cubxasl::aggregate_accumulator *h_eval_acc[H_MAX_CALLS];
static int h_eval_n[H_MAX_CALLS];
static int h_eval_sel[H_MAX_CALLS][H_MAX_ROWS];
static int h_n_eval;
static int h_n_dump;
static int h_n_copy, h_n_free_key;

int
qdata_evaluate_aggregate_batch (cubthread::entry * thread_p, cubxasl::aggregate_list_node * agg_list_p,
				val_descr * val_desc_p, expr_batch_plan * plan, const expr_batch * batch,
				bool skip_optimized, cubxasl::aggregate_accumulator * alt_acc_list)
{
  if (h_n_eval < H_MAX_CALLS)
    {
      h_eval_acc[h_n_eval] = alt_acc_list;
      h_eval_n[h_n_eval] = batch->n_sel;
      for (int j = 0; j < batch->n_sel && j < H_MAX_ROWS; j++)
	{
	  h_eval_sel[h_n_eval][j] = batch->sel[j];
	}
    }
  h_n_eval++;
  return NO_ERROR;
}

int
qdata_get_agg_hvalue_size (cubquery::aggregate_hash_value * value, bool ret_delta)
{
  return 0;
}

void *
mht_get (MHT_TABLE * ht, const void *key)
{
  if (h_n_get < H_MAX_CALLS * 4)
    {
      h_get_keys[h_n_get] = key;
    }
  h_n_get++;
  return h_get_result;
}

int
qdata_save_agg_htable_to_list (cubthread::entry * thread_p, mht_table * hash_table, qfile_list_id * tuple_list_id,
			       qfile_list_id * partial_list_id, db_value * temp_dbval_array)
{
  h_n_dump++;
  return NO_ERROR;
}

cubquery::aggregate_hash_key *
qdata_copy_agg_hkey (cubthread::entry * thread_p, cubquery::aggregate_hash_key * key)
{
  h_n_copy++;
  return (cubquery::aggregate_hash_key *) calloc (1, sizeof (cubquery::aggregate_hash_key));
}

void
qdata_free_agg_hkey (cubthread::entry * thread_p, cubquery::aggregate_hash_key * key)
{
  /* counted, not freed: the flush scenario's entries name static keys (the clear of an abandon reaches them) */
  h_n_free_key++;
}

extern "C" void *
db_private_alloc_release (THREAD_ENTRY * thrd, size_t size, bool rc_track)
{
  return malloc (size);
}

extern "C" void
db_private_free_release (THREAD_ENTRY * thrd, void *ptr, bool rc_track)
{
  free (ptr);
}

extern "C" void *
db_private_realloc_release (THREAD_ENTRY * thrd, void *ptr, size_t size, bool rc_track)
{
  return realloc (ptr, size);
}

static QFILE_LIST_ID h_list;
static int h_n_sync;
static QFILE_LIST_ID *
h_list_of (void *arg)
{
  return &h_list;
}

static void
h_sync (void *arg)
{
  h_n_sync++;
}

/* A flush over n pending rows: row t (its chunk row index rows[t]) belongs to group grp[t] (0..ng-1), each group one
 * cache entry (its own home slot, bytes = the group number).  The table had tuple_count0 tuples and group_count0
 * groups before the rows (HS_ACCEPT_ALL).  What the flush hands the aggregates and the table comes back in *res. */
int
h_flush_scenario (int n, const int *rows, const int *grp, int ng, int tuple_count0, int group_count0,
		  struct h_flush_result *res)
{
  static AGGREGATE_HASH_VALUE vals[H_MAX_CALLS];
  static cubxasl::aggregate_accumulator accs[H_MAX_CALLS];
  static cubquery::aggregate_hash_key keys[H_MAX_CALLS];
  QEXEC_BATCH_GBY st;
  AGGREGATE_HASH_CONTEXT ctx;
  XASL_NODE *xasl = (XASL_NODE *) calloc (1, sizeof (XASL_NODE));
  EXPR_BATCH batch;
  int caller_sel[H_MAX_ROWS], slot_of[H_MAX_CALLS];
  int g, t, k, rc;

  memset (res, 0, sizeof (*res));
  memset (&st, 0, sizeof (st));
  memset (&ctx, 0, sizeof (ctx));
  memset (&batch, 0, sizeof (batch));
  h_n_get = h_n_eval = h_n_dump = h_n_sync = 0;

  ctx.state = HS_ACCEPT_ALL;
  ctx.tuple_count = tuple_count0;
  ctx.group_count = group_count0;
  ctx.hash_table = (MHT_TABLE *) & h_list;	/* never dereferenced by the fakes */
  xasl->proc.buildlist.agg_hash_context = &ctx;
  st.xasl = xasl;
  st.list = h_list_of;
  st.sync = h_sync;
  st.arg = NULL;
  st.cache = (QEXEC_GBY_ENTRY *) malloc (sizeof (QEXEC_GBY_ENTRY) * QEXEC_GBY_CACHE_SIZE);
  for (k = 0; k < QEXEC_GBY_CACHE_SIZE; k++)
    {
      st.cache[k].len = -1;
      st.cache[k].bytes = NULL;
      st.cache[k].key = NULL;
    }
  for (g = 0; g < ng; g++)
    {
      unsigned int slot = (unsigned int) (37 * g + 5) & (QEXEC_GBY_CACHE_SIZE - 1);
      QEXEC_GBY_ENTRY *e = &st.cache[slot];

      memset (&vals[g], 0, sizeof (vals[g]));
      vals[g].accumulators = &accs[g];
      vals[g].batch_slot = (int) slot;
      e->bytes = (unsigned char *) malloc (1);
      e->bytes[0] = (unsigned char) g;
      e->hash = slot;
      e->len = 1;
      e->value = &vals[g];
      e->key = NULL;		/* the fake mht_get () reads only the pointer: keys[g] is passed below */
      e->count = 0;
      e->placed = 0;
      slot_of[g] = (int) slot;
      st.n_cached++;
    }
  st.cap = H_MAX_ROWS;
  st.pend_row = (int *) malloc (sizeof (int) * H_MAX_ROWS);
  st.pend_ent = (int *) malloc (sizeof (int) * H_MAX_ROWS);
  st.grp_rows = (int *) malloc (sizeof (int) * H_MAX_ROWS);
  st.order = (int *) malloc (sizeof (int) * H_MAX_ROWS);
  for (t = 0; t < n; t++)
    {
      st.pend_row[t] = rows[t];
      st.pend_ent[t] = slot_of[grp[t]];
    }
  /* the key pointer mht_get () is asked for identifies the group (st.cache[].key is what the flush passes) */
  for (g = 0; g < ng; g++)
    {
      st.cache[slot_of[g]].key = &keys[g];
    }
  for (t = 0; t < H_MAX_ROWS; t++)
    {
      caller_sel[t] = 1000 + t;
    }
  batch.sel = caller_sel;
  batch.n_sel = 77;

  rc = qexec_batch_gby_flush (NULL, &st, NULL, &batch, n, &res->done);

  res->n_calls = h_n_eval;
  for (k = 0; k < h_n_eval && k < H_MAX_CALLS; k++)
    {
      res->call_group[k] = -1;
      for (g = 0; g < ng; g++)
	{
	  if (h_eval_acc[k] == &accs[g])
	    {
	      res->call_group[k] = g;
	    }
	}
      res->call_n[k] = h_eval_n[k];
      for (t = 0; t < h_eval_n[k] && t < H_MAX_ROWS; t++)
	{
	  res->call_sel[k][t] = h_eval_sel[k][t];
	}
    }
  res->n_touch = h_n_get;
  for (k = 0; k < h_n_get && k < H_MAX_CALLS; k++)
    {
      res->touch_group[k] = -1;
      for (g = 0; g < ng; g++)
	{
	  if (h_get_keys[k] == &keys[g])
	    {
	      res->touch_group[k] = g;
	    }
	}
    }
  res->off = st.off ? 1 : 0;
  res->dumps = h_n_dump;
  res->syncs = h_n_sync;
  res->tuple_count_after = ctx.tuple_count;
  res->state_after = (int) ctx.state;
  res->evictions_after = (int) ctx.evictions;
  res->n_cached_after = st.n_cached;
  for (g = 0; g < ng; g++)
    {
      res->value_tuple_count[g] = vals[g].tuple_count;
    }
  res->counts_reset = 1;
  for (k = 0; k < QEXEC_GBY_CACHE_SIZE; k++)
    {
      if (st.cache[k].len >= 0 && st.cache[k].count != 0)
	{
	  res->counts_reset = 0;
	}
    }
  res->sel_restored = (batch.sel == caller_sel && batch.n_sel == 77) ? 1 : 0;

  /* keys[] are static: the clear must not free them */
  for (k = 0; k < QEXEC_GBY_CACHE_SIZE; k++)
    {
      if (st.cache[k].len >= 0)
	{
	  free (st.cache[k].bytes);
	}
    }
  free (st.cache);
  free (st.pend_row);
  free (st.pend_ent);
  free (st.grp_rows);
  free (st.order);
  free (xasl);
  return rc;
}

/* qexec_gby_cache_add () for n distinct one-byte...four-byte keys (key i = i), each naming value vals[i % 64] through
 * the fake mht_get (); then which of them qexec_gby_cache_find () still finds.  Returns n_cached; *found_first: key 0
 * found; *found_last: key n-1 found and its value's batch_slot is the slot it was found in; *copies / *frees: key
 * copies made / freed so far */
int
h_cache_add_scenario (int n, int *found_first, int *found_last, int *copies, int *frees_key)
{
  static AGGREGATE_HASH_VALUE vals[64];
  QEXEC_BATCH_GBY st;
  AGGREGATE_HASH_CONTEXT ctx;
  cubquery::aggregate_hash_key temp;
  int i, n_cached;

  memset (&st, 0, sizeof (st));
  memset (&ctx, 0, sizeof (ctx));
  memset (vals, 0, sizeof (vals));
  ctx.temp_key = &temp;
  h_n_copy = h_n_free_key = 0;
  for (i = 0; i < n; i++)
    {
      unsigned char key[4];
      int v = i;

      memcpy (key, &v, 4);
      h_get_result = &vals[i % 64];
      qexec_gby_cache_add (NULL, &st, &ctx, key, 4, qexec_gby_hash_bytes (key, 4));
    }
  n_cached = st.n_cached;
  {
    unsigned char key[4];
    int v = 0;
    QEXEC_GBY_ENTRY *e;

    memcpy (key, &v, 4);
    *found_first = (qexec_gby_cache_find (&st, key, 4, qexec_gby_hash_bytes (key, 4)) != NULL) ? 1 : 0;
    v = n - 1;
    memcpy (key, &v, 4);
    e = qexec_gby_cache_find (&st, key, 4, qexec_gby_hash_bytes (key, 4));
    *found_last = (e != NULL && e->value == &vals[(n - 1) % 64] && vals[(n - 1) % 64].batch_slot == (int) (e - st.cache))
      ? 1 : 0;
  }
  *copies = h_n_copy;
  *frees_key = h_n_free_key;	/* the releases the cache made by itself (its clear when half full) */
  qexec_gby_cache_clear (NULL, &st);
  free (st.cache);
  return n_cached;
}
