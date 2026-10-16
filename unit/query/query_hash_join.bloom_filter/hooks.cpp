/*
 * unitseam appendix for src/query/query_hash_join.c (compiled at the end of that TU), for
 * unit/query/query_hash_join.bloom_filter (CBRD-27659 prototype: Bloom filter on the second-materialized hash join
 * input). The Bloom struct and its build / decision functions are static in the TU; these hooks reach them, and
 * give the test the join's own key extraction (hjoin_fetch_key) and hash (qdata_hash_scan_key) as the reference.
 */

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

/* the filter's state, read through the second input it is attached to */
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
  v->built = bloom->built;
  v->disabled = bloom->disabled;
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
}

/* hjoin_bloom_build () on the filter attached to xasl (what the owner's first row triggers) */
void
unitseam_bloom_build (THREAD_ENTRY * thread_p, XASL_NODE * xasl)
{
  hjoin_bloom_build (thread_p, xasl->hj_bloom);
}

/*
 * The decision hjoin_bloom_skip_descr () / hjoin_bloom_skip_tuple () make, on a record the caller bound to the second
 * list's layout (qfile_slot_set_tuple_ptr_and_layout): hjoin_bloom_ready () then hjoin_bloom_skip_record ().
 *   return: -1 the filter is not active for this (thread, xasl), 0 keep the row, 1 skip it
 */
int
unitseam_bloom_skip_bound (THREAD_ENTRY * thread_p, XASL_NODE * xasl, QFILE_TUPLE_RECORD * record)
{
  HJOIN_BLOOM *bloom = hjoin_bloom_ready (thread_p, xasl);

  if (bloom == NULL)
    {
      return -1;
    }
  return hjoin_bloom_skip_record (thread_p, bloom, record) ? 1 : 0;
}

/* hjoin_bloom_may_contain () for a hash, on the filter attached to xasl */
bool
unitseam_bloom_may_contain (XASL_NODE * xasl, unsigned int hash)
{
  return hjoin_bloom_may_contain (xasl->hj_bloom, hash);
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
