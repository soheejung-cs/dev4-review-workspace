# §0 Cost Baselines (memorize)

Order-of-magnitude comparison matters more than absolute values.

| Operation | ~Cycles | Notes |
|---|---|---|
| L1 cache hit | 4–5 | effectively free |
| L2 cache hit | 12–15 | |
| L3 cache hit | 40–75 | shared across cores |
| **DRAM access** | **200–400** | ~100× L1 |
| Branch mispredict | 15–20 | pipeline flush |
| Integer add/sub/logic | 1 | |
| Integer multiply | 3–5 | |
| Integer divide (64-bit) | 20–100 | **very expensive** |
| FP add/mul | 4–5 | |
| FP divide | 13–20 | |
| `sqrt` | 15–20 | |
| Function call (not inlined) | 5–25 | |
| TLS access (initial-exec) | 1–3 | static link |
| TLS access (global-dynamic) | 20–50 | `thread_local` in a `.so`, calls `__tls_get_addr` |
| Atomic op (uncontended) | 20–50 | |
| **Atomic op (contended)** | **100–1000+** | cache-line migration |
| Cache-line invalidation (S→I broadcast) | 50–200 | proportional to cores holding copies |
| **HITM (transfer from another core's cache)** | **100–300** | worst coherency cost; see `perf c2c` |
| Compiler barrier | 0 | no instruction emitted; only blocks reordering |
| `mfence` (seq_cst fence) | 30–100 | x86; acquire/release usually 0 |
| Integer divide after mul-shift substitution | 5–8 | CLOW-06 |
| Mutex lock/unlock (uncontended) | 20–50 | |
| Mutex (contended, kernel entry) | 1,000–10,000+ | |
| `malloc`/`free` (small) | 50–200 | |
| `shared_ptr` copy (uncontended) | 20–40 | two atomic inc/dec |
| **`shared_ptr` copy (contended)** | **100–1,000+** | refcount line ping-pongs between cores |
| `dynamic_cast` | 50–500 | type-hierarchy walk |
| Throwing an exception | 1,000–10,000+ | never on the normal path |
| NUMA remote-node access | 1.5–2× local | multi-socket |
| System call | 500–2,000 | |
| Context switch | 1,000–10,000 | |
| SSD random read | ~50,000–500,000 | microseconds |
| HDD seek | ~10,000,000 | milliseconds |

**Three key conclusions:**
- **One DRAM access ≈ 200–400 integer ops.** Trading more computation for fewer memory accesses is almost always a win.
- **Division is 5–20× multiplication.** Replace repeated division with reciprocal multiplication.
- **A contended atomic is 10–50× a division.** It can swallow the entire parallelization gain.

## Hardware constants

| Item | Value | Notes |
|---|---|---|
| Cache line size | 64 B | most x86-64, ARM64 |
| L1d size | 32–48 KB / core | |
| L2 size | 512 KB – 2 MB / core | |
| L3 size | 8–256 MB / socket | shared |
| Page size | 4 KB (default), 2 MB (huge) | |
| TLB entries | hundreds–thousands | beyond → page walk |
| SIMD width | 128/256/512 bit | SSE/AVX2/AVX-512 |
