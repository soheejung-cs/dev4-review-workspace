/* appended to xasl_cache.c (server): the TU's own compiled copies of the static inline helpers
 * in bind_variant.h -- the copies xcache_bind_variant runs. The test checks every case against
 * these and against its own copy (same header, same flags), so a header change that made the two
 * diverge (a macro only the server sees, a helper moved into the .c) shows up as a failing case. */
double
unitseam_bv_tu_distance (const BIND_FINGERPRINT *a, const BIND_FINGERPRINT *b, double floor)
{
  return bind_fingerprint_distance (a, b, floor);
}

bool
unitseam_bv_tu_known (const BIND_FINGERPRINT *fp)
{
  return bind_fingerprint_known (fp);
}

int
unitseam_bv_tu_nearest (const BIND_VARIANT_RECORD *records, int n_records, const BIND_FINGERPRINT *fp, double floor,
			double *out_dist)
{
  return bind_variant_nearest (records, n_records, fp, floor, out_dist);
}
