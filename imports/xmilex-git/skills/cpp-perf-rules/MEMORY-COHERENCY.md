# §2 Memory Access (MEM) — top-priority category

Determines most of the performance in systems-level code. Review before all other categories.

## MEM-01 — Design for sequential access
The hardware prefetcher only recognizes **sequential or constant-stride** patterns. Pointer chasing defeats it.

```c
// BAD: node-based traversal — every node is a cache-miss candidate
for (node_t *n = head; n; n = n->next) process(n->value);

// GOOD: contiguous array traversal — prefetcher engages
for (size_t i = 0; i < n; i++) process(values[i]);
```

**Rule:** containers whose main operation is traversal must not be linked structures. If insert/delete is needed, implement an index-based free list on top of an array.

## MEM-02 — Fit hot structs in a cache line
A struct over 64B accessed every iteration burns two lines.

```c
// BAD: 72 B → two lines
struct entry {
    uint64_t key;      // 8
    char     name[56]; // 56
    uint64_t aux;      // 8
};

// GOOD: repack under 64 B, move big fields out
struct entry {
    uint64_t key;
    uint64_t aux;
    uint32_t name_offset;  // index into a separate string pool
    uint32_t name_len;
};  // 24 B
```

**Field ordering:** declare large → small types to minimize padding. Check `sizeof`; if it differs from expectation, investigate padding.

## MEM-03 — Eliminate false sharing (most common parallel-code mistake)
Different cores writing **different variables in the same cache line** make the line ping-pong. Logically independent, but performance collapses as if locked.

```c
// BAD: worker counters packed into one line
struct { uint64_t rows; uint64_t nulls; } stats[NUM_WORKERS];
// worker 0 and 1 share the same 64B line → line transfer on every increment

// GOOD: line-level isolation
struct alignas(64) worker_stats {
    uint64_t rows;
    uint64_t nulls;
    char _pad[64 - 2 * sizeof(uint64_t)];
};
static_assert(sizeof(worker_stats) == 64, "must be one cache line");
worker_stats stats[NUM_WORKERS];
```

**Diagnose:** `perf c2c record` / `perf c2c report` shows line contention directly. If IPC doesn't improve as thread count grows, suspect this first.

**Applies to:** per-worker counters, partial sums, progress, result-buffer pointers, variables adjacent to cancel flags.

## MEM-04 — Match layout to access pattern (AoS vs SoA)

```c
// AoS — good when multiple fields of one record are used together
struct row { int64_t a; double b; int32_t c; };
row rows[N];

// SoA — good when one field is scanned across all records (+ vectorizable)
struct columns { int64_t a[N]; double b[N]; int32_t c[N]; };
```

**Verdict:** loop reads a subset of fields → SoA. Loop reads all fields → AoS.
Column-wise aggregation/filter/statistics collection should default to SoA.

## MEM-05 — Split hot/cold fields
Rarely-accessed fields sharing a struct with per-row fields waste cache lines.

```c
// GOOD
struct scan_state_hot {     // touched every row
    uint64_t rows_seen;
    uint64_t reservoir_used;
    void    *sketch;
};
struct scan_state_cold {    // touched only at start/finish
    char      table_name[128];
    timestamp started_at;
    int       error_code;
};
```

## MEM-06 — Explicit prefetch only for sequential scans, distance by measurement

```c
#define PREFETCH_DIST 16   // determined by measurement; search 8–64
for (size_t i = 0; i < n; i++) {
    __builtin_prefetch(&rows[i + PREFETCH_DIST], 0 /*read*/, 1 /*low locality*/);
    process(rows[i]);
}
```

**Caution:** adding manual prefetch to patterns the hardware prefetcher already handles **slows things down** via cache pollution. Always measure before/after.
Biggest wins are on random accesses whose address is computable ahead of time (e.g. hash-table probes).

## MEM-07 — Block/tile the working set to fit cache
When data exceeds L2, process in chunks.

```c
// BAD: repeated full-array passes → each pass reloads from DRAM
for (pass = 0; pass < P; pass++)
    for (i = 0; i < N; i++) work(a[i], b[i]);

// GOOD: finish all passes per cache-sized block
const size_t BLOCK = L2_SIZE / (2 * sizeof(elem)) / 2;  // headroom
for (size_t base = 0; base < N; base += BLOCK)
    for (pass = 0; pass < P; pass++)
        for (i = base; i < min(base + BLOCK, N); i++) work(a[i], b[i]);
```

## MEM-08 — Consider huge pages for large sequential work
Scanning multi-GB data can bottleneck on TLB misses. 2 MB huge pages cut TLB entry demand 512×.

```c
madvise(ptr, len, MADV_HUGEPAGE);   // Linux, with THP enabled
```

## MEM-09 — Avoid read-for-ownership on write-only bulk buffers
When overwriting a whole buffer, the CPU reads it first (RFO) by default. Non-temporal stores bypass this.

```c
_mm256_stream_si256((__m256i*)dst, val);   // bypass cache, straight to memory
_mm_sfence();                               // if ordering is needed afterwards
```

**But if you will read that data again soon, this backfires.**

## MEM-10 — Keep rarely-run but latency-critical paths warm (cache warming)

A path that runs seldom but must be fast when it runs is always slow the first time — code (I-cache) and data (D-cache) are both cold. **Exercise the path during idle time so it stays cached.** Measured ~90% (cold 267.7ms → warm 25.6ms, paper note §2.1).

- HFT original: run the execution engine to completion on every tick, but only emit the order when a signal exists.
- DB equivalents: the first query after server restart, the recovery path after a long idle, rare slow-path transitions (spill, etc.). **Buffer-pool pre-warming and statistics preloading are the macro version of the same principle.**
- ⚠ The warm-up run must have **no side effects**. If the path mutates state, split out a dry-run first.
- ⚠ Relation to measurement: benchmark warm-up (MEAS-04) exists to **remove** this effect; this rule **exploits** it in production. Do not confuse the two.

---

# §3 Cache Coherency & Memory Sharing Protocol (COH)

The **hardware layer that underpins the MEM rules**. Most multithread perf problems are explained by this section.
"Sharing is slow" is imprecise: **read sharing is free; write sharing is catastrophic.** Understanding that difference is the core.

## COH-01 — Design sharing patterns around MESI line states

| State | Meaning | Copies on other cores | Write cost |
|---|---|---|---|
| **M** (Modified) | only this core, dirty | none | free (already owned) |
| **E** (Exclusive) | only this core, clean | none | free (transitions to M) |
| **S** (Shared) | several cores hold read copies | yes | **expensive (global invalidation)** |
| **I** (Invalid) | invalid | — | reload required |

**Three protocols follow:**

1. **Read-only from many cores → all coexist in S → zero coherency traffic. Completely free.**
2. **One core reads and writes → stays E/M → zero traffic.**
3. **One core writes while others hold the line → every write triggers an invalidation broadcast.**

> **Top-level protocol: exactly one writer per cache line.**
> This single rule eliminates MEM-03, GLOB-02, CPP-01, and PAR-01 problems.

## COH-02 — Know the RFO and HITM costs

To write, a core must first gain **exclusive ownership**:

```
Other core holds S  → invalidation broadcast + wait for acks
Other core holds M  → data forwarded directly from that core (HITM, cache-to-cache)
                       → 100–300 cycles; worst coherency cost
```

**HITM** is exactly what `perf c2c` measures. Top report symbols are the culprits.

```bash
perf c2c record -F 60000 -a -- ./bench
perf c2c report --stdio | head -60
# top of "Shared Data Cache Line Table" = most contended lines
# below: per-offset readers/writers and symbol names
```

**Key:** the write itself isn't expensive — **writing while another core holds the line** is. That's why the same code is fast with 1 thread and slow with many.

## COH-03 — 128B alignment may be needed due to the adjacent-line prefetcher

Intel's spatial prefetcher fetches **the sibling line of a 128B-aligned pair together**, so 64B padding can leave residual false sharing.

```c
/* 128B for extreme-contention spots */
#define COH_PAD 128
typedef struct { uint64_t v; char _pad[COH_PAD - sizeof(uint64_t)]; } hot_slot_t;
_Static_assert(sizeof(hot_slot_t) == COH_PAD, "pad mismatch");
```

**Rule:** default to 64B; if `perf c2c` still shows HITM, raise to 128B and re-measure. Blanket 128B doubles memory/cache waste — only with measured evidence.

## COH-04 — Keep single-writer-per-line as an invariant

Concrete design-time rules:

```c
/* 1. Per-worker state as line-aligned array */
typedef struct { uint64_t rows, nulls, bytes; char _pad[CACHE_LINE - 24]; } wstate_t;
static wstate_t g_wstate[MAX_WORKERS] CACHE_ALIGNED;

/* 2. Shared aggregation: accumulate locally, merge once (PAR-01) */

/* 3. Split result buffers on line boundaries — align chunk starts */
size_t chunk = ALIGN_UP(total / nworkers, CACHE_LINE / elem_size);

/* 4. Separate read-only data from written data onto different lines */
struct scan_ctx {
    /* --- read-only (all workers share in S; free) --- */
    const row_t *rows;
    size_t       nrows;
    int64_t      min_key;
    char _pad0[CACHE_LINE - 24];
    /* --- write region split into per-worker array (g_wstate above) --- */
};
```

## COH-05 — Share read-only data freely, but state it in the type

Read-only data is shared for free regardless of core count. Declaring it `const` places it in `.rodata`, optimized by both compiler and hardware (GLOB-03).

**The goal of scalable design is not "less sharing" but "no write sharing."** A read-only lookup table doesn't care how many cores hammer it.

## COH-06 — Split ring-buffer indices and cache local copies (SPSC queue)

Producer writes `tail`, reads `head`; consumer writes `head`, reads `tail`. If both indices share a line, **every push/pop causes a line round trip.**

```c
/* BAD: head and tail on the same line → HITM per operation */
struct ring { uint64_t head, tail; void *slots[N]; };

/* GOOD: separate lines + cached copies of the peer index to cut access frequency */
struct ring {
    CACHE_ALIGNED _Atomic uint64_t head;        /* written by consumer */
    CACHE_ALIGNED _Atomic uint64_t tail;        /* written by producer */
    CACHE_ALIGNED uint64_t p_cached_head;       /* producer-private copy */
    CACHE_ALIGNED uint64_t c_cached_tail;       /* consumer-private copy */
    CACHE_ALIGNED void *slots[N];
};

/* Producer: if the cached copy shows room, never touch the shared head */
static bool push(struct ring *r, void *v) {
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (t - r->p_cached_head >= N) {                       /* decide from the copy */
        r->p_cached_head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (t - r->p_cached_head >= N) return false;       /* only when truly full */
    }
    r->slots[t & (N - 1)] = v;
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return true;
}
```

This confines shared-line access to near-empty/near-full moments. Batching (multiple push/pop at once) reduces traffic further.

## COH-07 — Never share bitfields across threads (C's silent data race)

Bitfield writes are **word-granularity read-modify-write**. Two threads writing different bitfields of the same word is a data race — **one update is lost.** C11 treats adjacent bitfields as the same "memory location".

```c
/* BAD: logically independent, yet races and loses values */
struct page_flags { unsigned dirty:1; unsigned pinned:1; unsigned io:1; };
/* thread A sets dirty=1, thread B sets pinned=1 → one may vanish */

/* GOOD 1: separate memory locations with a zero-width bitfield */
struct page_flags {
    unsigned dirty:1;
    unsigned :0;              /* force next field into a new storage unit */
    unsigned pinned:1;
};

/* GOOD 2: separate atomic bytes (recommended) */
struct page_flags {
    _Atomic unsigned char dirty;
    _Atomic unsigned char pinned;
    _Atomic unsigned char io;
};

/* GOOD 3: one atomic word plus bit ops */
_Atomic uint32_t flags;
atomic_fetch_or_explicit(&flags, FLAG_DIRTY, memory_order_relaxed);
```

**Rule:** no bitfields in flag sets shared across threads. Only single-thread-private or lock-protected.

## COH-08 — Guarantee natural alignment of atomics

Misaligned atomic access can span cache lines, becoming a bus lock or losing atomicity. A **split lock** (line-boundary-crossing) degrades the whole system.

```c
_Alignas(8)  _Atomic uint64_t counter;      /* 8-byte natural alignment */
_Alignas(16) _Atomic struct { void *p; uint64_t tag; } tagged_ptr;  /* DWCAS */
_Static_assert(_Alignof(_Atomic uint64_t) >= 8, "unaligned atomic");
```

Never put atomic fields inside `__attribute__((packed))` structs.

## COH-09 — Know the store buffer and 4K aliasing

Stores go through the store buffer. Reading a just-written address is fast (store-to-load forwarding), but when **the low 12 address bits match while the actual addresses differ (4K aliasing)**, the CPU falsely infers a dependency and stalls.

```c
/* BAD: two big arrays start at multiples of 4KB → false dependencies during scan */
double *a = aligned_alloc(4096, N * 8);
double *b = aligned_alloc(4096, N * 8);
for (i = 0; i < N; i++) b[i] = a[i] * 2.0;   /* a[i] and b[i] 4K-alias */

/* GOOD: stagger the offsets by a cache line */
char *pool = aligned_alloc(4096, 2 * N * 8 + 4096);
double *a = (double*)pool;
double *b = (double*)(pool + N * 8 + CACHE_LINE);   /* deliberate offset */
```

Diagnose: `perf stat -e ld_blocks_partial.address_alias` (Intel).

## COH-10 — Distinguish compiler barriers from CPU barriers

```c
/* Compiler barrier: prevents reordering only. Zero instructions, zero runtime cost */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* CPU barriers: real instructions */
atomic_thread_fence(memory_order_acquire);   /* x86: usually no instruction */
atomic_thread_fence(memory_order_release);   /* x86: usually no instruction */
atomic_thread_fence(memory_order_seq_cst);   /* x86: mfence (~30–100 cycles) */
```

**x86-64 is a strong memory model (TSO)** — only store→load reordering occurs, so acquire/release are usually satisfied by a compiler barrier with no instruction. **ARM64 is weak** — `dmb ish` etc. are actually emitted.

**Conclusion:** express code with standard atomics and memory orders; let the compiler absorb per-platform cost. Never insert `mfence` directly. And since x86-working lock-free code commonly breaks on ARM, **always validate on a weak model.**

## COH-11 — Cross-process shared memory follows the same rules

Regions shared via `shm_open`/`mmap` are cache-coherent, so COH-01..09 all apply. Additionally:

- Different processes don't share a translation unit, so **compiler barriers cannot be relied on.** Atomic types and explicit memory orders are mandatory.
- Struct layout/alignment must match on both sides. Different compilers/flags can change padding — pin sizes/offsets with `_Static_assert`.

```c
_Static_assert(sizeof(shm_header_t) == 64, "shm layout changed");
_Static_assert(offsetof(shm_header_t, seq) == 8, "shm offset changed");
```

## COH-12 — Prefer transfer over sharing

Instead of concurrent access to the same data, **hand ownership over once** — this removes coherency traffic at the root.

| Model | Coherency profile |
|---|---|
| Share + lock | line round trip + serialization per access |
| Share + atomics | RFO per access |
| **Partitioned ownership** | **zero traffic; one writer per line** |
| **Transfer (queue/message)** | traffic only at handoff |

When partitioning is possible, it is always best. Work that can be split by region (statistics collection, scans) scales linearly with no locks.
