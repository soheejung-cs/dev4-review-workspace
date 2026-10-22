/* unitseam appendix to src/query/query_executor.c (same TU): a hand-built page-batch GROUP BY cache,
 * n groups of one linear-probe cluster (same home slot), then qexec_gby_cache_evicted () over the note
 * the consumer would have taken (slot = each group's batch_slot before the call). Returns what is left.
 * keep_mask: bit i set = group i of the cluster is NOT evicted (not noted) and must still be found after. */
int
unitseam_gby_evict_scenario_keep (int n, int home, unsigned keep_mask, int *n_left, int *stale_found, int *stale_slot,
				  int *kept_found)
{
  QEXEC_BATCH_GBY st;
  static AGGREGATE_HASH_VALUE vals[QEXEC_GBY_EVICT_NOTE];
  AGGREGATE_HASH_VALUE *note_val[QEXEC_GBY_EVICT_NOTE];
  int note_slot[QEXEC_GBY_EVICT_NOTE];
  unsigned int mask = QEXEC_GBY_CACHE_SIZE - 1, s;
  int i, k, n_note = 0;

  memset (&st, 0, sizeof (st));
  st.cache = (QEXEC_GBY_ENTRY *) malloc (sizeof (QEXEC_GBY_ENTRY) * QEXEC_GBY_CACHE_SIZE);
  for (k = 0; k < QEXEC_GBY_CACHE_SIZE; k++)
    {
      st.cache[k].len = -1;
    }
  for (i = 0; i < n; i++)
    {
      QEXEC_GBY_ENTRY *e;

      for (s = (unsigned) home & mask; st.cache[s].len >= 0; s = (s + 1) & mask)
	;
      e = &st.cache[s];
      e->bytes = (unsigned char *) malloc (1);
      e->bytes[0] = (unsigned char) i;
      e->hash = (unsigned) home;
      e->len = 1;
      e->value = &vals[i];
      e->key = NULL;
      e->count = e->placed = 0;
      vals[i].batch_slot = (int) s;
      st.n_cached++;
      if (!(keep_mask & (1u << i)))
	{
	  note_val[n_note] = &vals[i];
	  note_slot[n_note] = (int) s;
	  n_note++;
	}
    }
  qexec_gby_cache_evicted (NULL, &st, note_val, note_slot, n_note);
  *n_left = st.n_cached;
  *stale_found = 0;
  *stale_slot = -1;
  *kept_found = 0;
  for (i = 0; i < n; i++)
    {
      unsigned char kb = (unsigned char) i;
      QEXEC_GBY_ENTRY *e = qexec_gby_cache_find (&st, &kb, 1, (unsigned) home);

      if (keep_mask & (1u << i))
	{
	  if (e != NULL && e->value == &vals[i])
	    {
	      (*kept_found)++;
	    }
	}
      else if (e != NULL)
	{
	  (*stale_found)++;
	  *stale_slot = (int) (e - st.cache);
	}
    }
  for (k = 0; k < QEXEC_GBY_CACHE_SIZE; k++)
    {
      if (st.cache[k].len >= 0)
	{
	  free (st.cache[k].bytes);
	}
    }
  free (st.cache);
  return 0;
}

int
unitseam_gby_evict_scenario (int n_evict, int home, int *n_left, int *stale_found, int *stale_slot)
{
  int kept = 0;

  return unitseam_gby_evict_scenario_keep (n_evict, home, 0, n_left, stale_found, stale_slot, &kept);
}
