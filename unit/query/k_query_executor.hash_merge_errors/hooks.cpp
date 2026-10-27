/* unitseam appendix to src/query/query_executor.c (same TU): the GROUP BY leftovers' hash merge driven by hand --
 * qexec_gby_merge_sample_groups (), qexec_gby_hash_merge () and qexec_gby_merge_open_parts () / free_parts () --
 * over the list-file, scan, value-fetch and key-hash fakes of test.cpp.  These hooks build what the functions read:
 * the GROUP BY state, its hash context, the XASL node (a BUILDLIST, one key, no aggregate) and a zeroed thread entry
 * (thread_is_on_trace () reads on_trace).  test.cpp counts the list ids its fake qfile_open_list () hands out and the
 * ones qfile_free_list_id () takes back. */

extern int k_scan_end;		/* what the fake scan returns once a list's tuples are read: S_END or S_ERROR */
extern int k_scan_limit;	/* >= 0: the fake scan stops after this many tuples whatever the list holds */
extern int k_add_fail_at;	/* the fake qfile_add_tuple_to_list () fails (ER_INTERRUPTED) at this call (0: never) */
extern int k_add_calls;
extern double k_groups;		/* the fake sketch's estimate */
extern long k_opened;
extern int k_open_fail_at;
long k_live_count (void);
void k_live_reset (void);
int k_is_live (const QFILE_LIST_ID * l);

static unsigned char k_thread_buf[sizeof (cubthread::entry)] __attribute__ ((aligned (64)));

static THREAD_ENTRY *
k_thread (void)
{
  memset (k_thread_buf, 0, sizeof (k_thread_buf));
  return (THREAD_ENTRY *) k_thread_buf;
}

static GROUPBY_STATE k_gb;
static AGGREGATE_HASH_CONTEXT k_ctx;
static XASL_STATE k_xs;
static XASL_NODE k_xasl;
static GROUPBY_DIMENSION k_dim;
static AGGREGATE_HASH_KEY k_key, k_pkey;
static AGGREGATE_HASH_VALUE k_pval;
static DB_VALUE *k_key_vals[4], *k_pkey_vals[4];
static QFILE_LIST_ID k_rows, k_partials;
static int k_dummy_table;

static void
k_setup (int rows, int pages)
{
  memset ((void *) &k_gb, 0, sizeof (k_gb));
  memset ((void *) &k_ctx, 0, sizeof (k_ctx));
  memset ((void *) &k_xs, 0, sizeof (k_xs));
  memset ((void *) &k_xasl, 0, sizeof (k_xasl));
  memset ((void *) &k_dim, 0, sizeof (k_dim));
  memset ((void *) &k_key, 0, sizeof (k_key));
  memset ((void *) &k_pkey, 0, sizeof (k_pkey));
  memset ((void *) &k_pval, 0, sizeof (k_pval));
  memset ((void *) &k_rows, 0, sizeof (k_rows));
  memset ((void *) &k_partials, 0, sizeof (k_partials));
  k_key.values = k_key_vals;
  k_pkey.values = k_pkey_vals;
  k_xs.query_id = 1;
  k_xasl.type = BUILDLIST_PROC;
  k_xasl.option = Q_ALL;	/* not DISTINCT: with agg_hash_respect_order = yes the order reaches the result */
  k_xasl.proc.buildlist.g_hkey_size = 1;
  k_xasl.proc.buildlist.g_func_count = 0;
  k_ctx.hash_table = (MHT_TABLE *) & k_dummy_table;	/* mht_count () is a fake: 0, the table was dumped */
  k_ctx.part_list_id = &k_partials;
  k_ctx.temp_key = &k_key;
  k_ctx.temp_part_key = &k_pkey;
  k_ctx.temp_part_value = &k_pval;
  k_ctx.state = HS_ACCEPT_ALL;
  k_ctx.key_sketch_partial = false;
  k_gb.xasl = &k_xasl;
  k_gb.xasl_state = &k_xs;
  k_gb.agg_hash_context = &k_ctx;
  k_gb.g_dim = &k_dim;
  k_gb.g_dim_levels = 1;
  k_gb.hash_eligible = 1;
  k_rows.tuple_cnt = rows;
  k_rows.page_cnt = pages;
  k_rows.type_list.type_cnt = 2;
  k_rows.query_id = 1;
  k_partials.tuple_cnt = 0;
  k_partials.type_list.type_cnt = 5;
  k_partials.query_id = 1;
}

/* the sample: rows tuples (all distinct keys), the scan then answers end_code */
int
k_sample_scenario (int rows, int end_code, int *rc, double *groups)
{
  THREAD_ENTRY *t = k_thread ();

  k_setup (1000000, 10000);
  k_scan_end = end_code;
  k_scan_limit = rows;
  *groups = -1;
  *rc = qexec_gby_merge_sample_groups (t, &k_gb, &k_rows, groups);
  k_scan_limit = -1;
  return 0;
}

/* the whole merge, order reaching the result (the collapse leaf): add_fail_at > 0 makes the k-th tuple write fail
 * the way an interrupted query's next page fails (qfile_allocate_new_page (): ER_INTERRUPTED) */
int
k_hash_merge_scenario (int rows, int pages, double groups, int add_fail_at, int *rc, int *result, long *opened,
		       long *live, int *part_list_live)
{
  THREAD_ENTRY *t = k_thread ();
  QEXEC_GBY_MERGE_RESULT r = QEXEC_GBY_MERGE_NONE;

  k_setup (rows, pages);
  k_live_reset ();
  k_groups = groups;
  k_add_fail_at = add_fail_at;
  k_add_calls = 0;
  k_scan_end = S_END;
  k_scan_limit = -1;
  *rc = qexec_gby_hash_merge (t, &k_gb, &k_xasl, &k_rows, &r);
  *result = (int) r;
  *opened = k_opened;
  *live = k_live_count ();
  *part_list_live = k_is_live (k_ctx.part_list_id);
  return 0;
}

/* n partitions opened, the open_fail_at-th list open failing (0: none), then freed as the callers free them */
int
k_open_parts_scenario (int n, int *rc, long *opened, long *live)
{
  THREAD_ENTRY *t = k_thread ();
  QFILE_LIST_ID **rows = NULL, **partials = NULL;

  int fail_at = k_open_fail_at;

  k_setup (100, 1);
  k_live_reset ();
  k_open_fail_at = fail_at;
  *rc = qexec_gby_merge_open_parts (t, &k_rows, &k_partials, 1, n, &rows, &partials);
  *opened = k_opened;
  qexec_gby_merge_free_parts (t, n, &rows, &partials);
  *live = k_live_count ();
  return 0;
}
