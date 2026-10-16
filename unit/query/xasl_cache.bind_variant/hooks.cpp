/* appended to xasl_cache.c: reach the TU's statics from the test */
void unitseam_xcache_enable (bool on) { xcache_Enabled = on; }
int unitseam_dir_pending (XASL_CACHE_ENTRY *e)
{
  int n = 0;
  if (e->bind_dir == NULL) return -1;
  for (int i = 0; i < e->bind_dir->next_variant; i++) n += e->bind_dir->resolved[i] ? 0 : 1;
  return n;
}
