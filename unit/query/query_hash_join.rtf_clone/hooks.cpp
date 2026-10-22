/* unitseam appendix to src/query/query_hash_join.c (same TU): a runtime filter built by hand the way
 * hjoin_rtf_build () fills one (query_hash_join.c:4906-4959 -- cap = next power of two >= 2 * n, calloc'd
 * HJOIN_RUNTIME_FILTER, slots / used, mask, is_date false, val_index, the open-addressing insert with
 * hjoin_rtf_hash ()), and attached as its success tail does (4962-4967: hj->rtf, rtf_keys, rtf_rows,
 * rtf_dropped, target->rtf, target->rtf_val, and the target node's own rtf_rows / rtf_dropped since the counters
 * moved onto the XASL node). The real build's list scan (qfile_open_list_scan /
 * qfile_slot_read_column_value, inline on a tuple layout) is not reproduced -- only the filter it leaves. */
HJOIN_RUNTIME_FILTER *
unitseam_rtf_make (const INT64 * keys, int n, int val_index)
{
  HJOIN_RUNTIME_FILTER *rtf;
  UINT64 cap, h;
  int i;

  for (cap = 16; cap < (UINT64) n * 2; cap <<= 1)
    {
      ;
    }
  rtf = (HJOIN_RUNTIME_FILTER *) calloc (1, sizeof (HJOIN_RUNTIME_FILTER));
  rtf->slots = (INT64 *) malloc (sizeof (INT64) * cap);
  rtf->used = (unsigned char *) calloc (cap, 1);
  rtf->mask = cap - 1;
  rtf->is_date = false;
  rtf->val_index = val_index;
  for (i = 0; i < n; i++)
    {
      for (h = hjoin_rtf_hash (keys[i]) & rtf->mask; rtf->used[h] && rtf->slots[h] != keys[i]; h = (h + 1) & rtf->mask)
	{
	  ;
	}
      if (!rtf->used[h])
	{
	  rtf->used[h] = 1;
	  rtf->slots[h] = keys[i];
	  rtf->n_keys++;
	}
    }
  return rtf;
}

void
unitseam_rtf_attach (XASL_NODE * hashjoin_xasl, XASL_NODE * target, HJOIN_RUNTIME_FILTER * rtf, DB_VALUE * cell)
{
  HASHJOIN_PROC_NODE *hj = &hashjoin_xasl->proc.hashjoin;

  hj->rtf = rtf;
  hj->rtf_keys = rtf->n_keys;
  hj->rtf_rows = 0;
  hj->rtf_dropped = 0;
  target->rtf = rtf;
  target->rtf_val = cell;
  target->rtf_rows = 0;
  target->rtf_dropped = 0;
}

INT64
unitseam_rtf_n_keys (const HJOIN_RUNTIME_FILTER * rtf)
{
  return rtf->n_keys;
}

INT64
unitseam_rtf_max_keys (void)
{
  return HJOIN_RTF_MAX_KEYS;
}

/* The TU's private-heap calls (db_private_alloc () and friends, CBRD-27361 review T10): no thread entry and no
 * LEA heap here, so they are plain malloc / free -- what the hand-built objects above are made with. */
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
