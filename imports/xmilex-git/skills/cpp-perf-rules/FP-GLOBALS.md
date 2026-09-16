# §7 Floating Point (FP)

## FP-01 — Never use `==` for floating-point equality
After non-trivial arithmetic, exact equality virtually never occurs. Comparison logic never reaches its "equal" branch, and sub-epsilon noise decides the result.

```c
/* BAD */
if (cost_a == cost_b) return EQ;

/* GOOD: relative-error criterion */
#define COST_EPS 1e-6
static inline bool fp_eq(double a, double b) {
    double diff = fabs(a - b);
    double mag  = fmax(fabs(a), fabs(b));
    return diff <= COST_EPS * fmax(mag, 1.0);   /* absolute error near 0 */
}
```

## FP-02 — Comparators must satisfy both symmetry and transitivity
**Comparing component-wise and then ordering by sum yields an asymmetric comparator.**

```c
/* BAD: equal sums with different components return LT in BOTH directions */
if (a_fixed == b_fixed && a_var == b_var) return EQ;
return (a_fixed + a_var <= b_fixed + b_var) ? LT : GT;
    /* forward: LT, reverse: LT — result depends on visit order */

/* GOOD: compare a single scalar (the total) with epsilon */
int cost_cmp(double a, double b) {
    if (fp_eq(a, b)) return EQ;
    return (a < b) ? LT : GT;
}
```

**Mandatory verification probes:**

```c
/* Symmetry: for random pairs, cmp(a,b) and cmp(b,a) must be opposite */
for (int i = 0; i < 400; i++) {
    double a = rnd_cost(), b = rnd_cost();
    int f = cost_cmp(a, b), r = cost_cmp(b, a);
    assert((f == LT && r == GT) || (f == GT && r == LT) || (f == EQ && r == EQ));
}
/* Transitivity: a≈b and b≈c must imply a≈c — epsilon equality easily breaks this */
for (...) {
    if (fp_eq(a,b) && fp_eq(b,c)) assert(fp_eq(a,c));   /* can fail! */
}
```

> **Warning:** epsilon-based equality can violate transitivity (a≈b, b≈c but a≉c).
> Passing a comparator that violates strict weak ordering to `std::sort` is **undefined behavior**
> and can lead to crashes or memory corruption.
> As a sort key: (1) quantize values before comparing, or (2) drop epsilon equality and use
> strict comparison + a deterministic tie-breaker (an ID, etc.).

```c
/* Safe sorting pattern: quantize, strict compare, deterministic tie-break */
static inline int64_t quantize(double c) { return (int64_t)(c / COST_EPS); }
int plan_cmp(const plan_t *x, const plan_t *y) {
    int64_t qx = quantize(x->total_cost), qy = quantize(y->total_cost);
    if (qx != qy) return (qx < qy) ? -1 : 1;
    return (x->id < y->id) ? -1 : (x->id > y->id) ? 1 : 0;   /* stable */
}
```

## FP-03 — No `-ffast-math` in correctness-sensitive code
It re-associates, assumes no NaN/Inf, and substitutes division with reciprocal multiplication. Cost models, selectivity estimation, and statistics aggregation need reproducibility — forbidden there. If needed, apply per-function only.

## FP-04 — Replace repeated division with reciprocal multiplication

```c
/* BAD: a division per iteration (13–20 cycles) */
for (i = 0; i < n; i++) out[i] = v[i] / total;

/* GOOD: one division + n multiplications */
const double inv = 1.0 / total;
for (i = 0; i < n; i++) out[i] = v[i] * inv;
```

**Caution:** results may differ bit-for-bit. Skip when reproducibility is required.

## FP-05 — Account for error accumulation in bulk summation
Naive sequential summation accumulates O(n) error. When accuracy matters, use Kahan compensated summation or pairwise summation. Pairwise gets accuracy and vectorization at once.

```c
/* Splitting into partial sums: less error + more ILP + vectorizable */
double s0=0, s1=0, s2=0, s3=0;
for (i = 0; i + 4 <= n; i += 4) { s0+=v[i]; s1+=v[i+1]; s2+=v[i+2]; s3+=v[i+3]; }
double sum = (s0 + s1) + (s2 + s3);
for (; i < n; i++) sum += v[i];
```

## FP-06 — Keep integer-representable values as integers
Counts, offsets, sizes, cardinalities are never stored as floating point. This eliminates comparison problems and precision loss at the source.

## FP-07 — Defer carries in fixed-point (DECIMAL/NUMERIC) accumulation
Decimal fixed-point addition drags along precision/scale lookup, digit scans, rounding, and packing every time.
For bulk summation, **accumulate into integer word buckets with ample headroom** and perform digit normalization/rounding/packing **once at finalization**. Separate sign-specific buckets even remove the per-row overflow check.
Introducing an accumulator state means **every consumption point (reading intermediate results, merging partial sums, spill store/load, free, re-init) must be preceded by materialization (flush)** — pin the list of those points in a code comment and verify it exhaustively in review.

## FP-08 — Never mix float and double in one expression

Mixing inserts implicit promotion (float→double) and demotion (double→float) instructions. In a per-row loop that is not negligible. Measured 21.6ns → 14.2ns (~52%, paper note §2.11).

```c
float a, b;
a = b * 1.23;    /* 1.23 is a double literal → promote + demote */
a = b * 1.23f;   /* float literal → no conversion */
```

- Decide the required precision first, then unify the type. double→float demotion loses precision.
- DB check: are statistics/selectivity computations uniformly double, with no stray `f` literals mixed in?

---

# §8 Global / Static / TLS State (GLOB)

Global state **blocks optimization in single-threaded code** and **causes cache-coherency storms in multithreaded code.**
Frequently overlooked as a perf cause, hence its own category.

## GLOB-01 — Copy globals into locals in loops

The compiler **cannot keep a global in a register across a function call** — it cannot rule out that the callee modifies it. Every access becomes a memory load.

```c
extern int g_threshold;

/* BAD: g_threshold reloaded from memory each iteration.
        The call to process() is assumed able to change it, so no caching */
for (i = 0; i < n; i++) {
    if (v[i] > g_threshold) process(v[i]);
}

/* GOOD: register-resident; also enables invariant hoisting and vectorization */
const int threshold = g_threshold;
for (i = 0; i < n; i++) {
    if (v[i] > threshold) process(v[i]);
}
```

**Applies to:** config values, thresholds, flags, size constants — every global that doesn't change during the loop.

## GLOB-02 — Remove false sharing of adjacently-declared globals (very common)

**Globals declared consecutively in the same translation unit are placed adjacently in memory by the linker.** Two logically unrelated globals land in the same 64B cache line; different threads updating each make the line ping-pong between cores.

```c
/* BAD: the two counters can land on the same line */
uint64_t g_scan_count;      /* updated by scan thread */
uint64_t g_flush_count;     /* updated by flush thread */
/* → completely independent logically, but performance collapses as if they shared a lock */

/* GOOD: isolate each on its own line */
CACHE_ALIGNED uint64_t g_scan_count;
CACHE_ALIGNED uint64_t g_flush_count;
```

**Diagnose:**

```bash
nm -S --size-sort binary | grep -E 'g_scan_count|g_flush_count'
# address delta under 64 → same cache line
objdump -t binary | sort -k1 | less    # check .bss/.data placement
perf c2c record ./binary && perf c2c report   # confirm actual line contention
```

Same cause as MEM-03 (false sharing in struct arrays), but **globals get it from declaration position alone, making it much harder to spot.**

## GLOB-03 — Declare read-mostly globals `const`

Read-only data goes in `.rodata`, shared cleanly by all cores with zero coherency traffic. But **a single write mixed in invalidates the line in every core's cache.**

```c
/* BAD: never changes after init, but the compiler doesn't know */
int g_page_size = 4096;

/* GOOD */
const int g_page_size = 4096;          /* .rodata */
static constexpr int PAGE_SIZE = 4096; /* C++: compile-time constant outright */
```

## GLOB-04 — Never use `volatile` for thread synchronization

`volatile` **does not prevent reordering, does not guarantee atomicity, and does not provide cache coherency.** It only forces a memory reload per access — pure slowdown.

```c
/* BAD: doesn't synchronize; only slow */
volatile bool g_stop;

/* GOOD */
std::atomic<bool> g_stop;
g_stop.load(std::memory_order_relaxed);
```

The only legitimate uses of `volatile`: **MMIO registers, signal-handler variables (`sig_atomic_t`), variables crossing a setjmp boundary.**

## GLOB-05 — Know `thread_local` access cost; cache the pointer

TLS access is more expensive than a local. Cost varies greatly by TLS model:

| Model | Situation | Cost |
|---|---|---|
| initial-exec | statically linked into executable | one segment-relative offset → very cheap |
| local-exec | same module | cheap |
| **global-dynamic** | `dlopen`-able `.so` | **a `__tls_get_addr()` function call** |

```c
/* BAD: TLS lookup per iteration */
for (size_t i = 0; i < n; i++) tls_stats.rows++;

/* GOOD: take the pointer once */
stats_t *st = &tls_stats;
for (size_t i = 0; i < n; i++) st->rows++;
```

**Build option:** `-ftls-model=initial-exec` — safe only when that shared library is never `dlopen`ed. Never for plugins/extension modules.

## GLOB-06 — Don't read `errno` in hot loops

`errno` is a TLS macro (`*__errno_location()`) — every access is a TLS lookup.

```c
/* BAD: TLS access every iteration though failure is rare */
for (...) { r = op(); if (errno) handle(); }

/* GOOD: decide from the return value; touch errno only on the failure path */
for (...) { r = op(); if (UNLIKELY(r < 0)) handle(errno); }
```

## GLOB-07 — Know the thread-safety guard of function-local `static` (C++)

Since C++11, function-local `static` initialization is **guaranteed** thread-safe, which makes the compiler **check a guard variable on every entry.** Even after initialization, an atomic load of the guard byte remains.

```cpp
/* BAD: guard check per call (accumulates on hot paths) */
const lookup_table& get_table() {
    static lookup_table t = build_table();
    return t;
}

/* GOOD 1: explicit one-time init at startup, unguarded access afterwards */
static lookup_table *g_table = nullptr;   /* initialized at server boot */
inline const lookup_table& get_table() { return *g_table; }

/* GOOD 2: constexpr if it's a compile-time constant */
static constexpr auto TABLE = make_table();   /* C++20 */
```

`-fno-threadsafe-statics` removes the guard, but use it **only when init races are provably impossible.**

## GLOB-08 — Shared-library global access goes through the GOT

In PIC code, access to externally-visible globals is indirected through the GOT (Global Offset Table) — one extra load.

```bash
-fvisibility=hidden           # default to hidden; internal symbols accessed directly
-Wl,-Bsymbolic-functions      # bind intra-module references internally
```

```c
__attribute__((visibility("default"))) void public_api(void);   /* expose only public API */
```

Fewer symbols also shorten link and load time.

## GLOB-09 — Shard global counters and statistics

A server-wide statistics counter is a single point every thread hits. Spread across core-count shards; sum on read.

```c
#define STAT_SHARDS 64
/* each shard on its own cache line */
typedef struct { uint64_t v; char _pad[CACHE_LINE - sizeof(uint64_t)]; } shard_t;
static shard_t g_stat[STAT_SHARDS] CACHE_ALIGNED;

static inline void stat_inc(int shard_hint) {
    g_stat[shard_hint & (STAT_SHARDS - 1)].v++;    /* no atomics needed */
}
static uint64_t stat_read(void) {
    uint64_t s = 0;
    for (int i = 0; i < STAT_SHARDS; i++) s += g_stat[i].v;
    return s;   /* approximate snapshot; handle separately if point-in-time consistency is needed */
}
```

`shard_hint` comes from a thread ID or `sched_getcpu()`. The trade flips to **O(1) uncontended writes, O(#shards) reads** — ideal for rarely-read statistics.

## GLOB-10 — Global state blocks inlining, vectorization, and reordering

The compiler assumes any function call may modify globals, so loop-invariant hoisting, vectorization, and load/store reordering are all blocked.

```c
/* BAD: g_config access blocks most loop optimizations */
void scan(row_t *rows, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (rows[i].key > g_config.min_key) g_stats.hits++;
}

/* GOOD: strip globals at entry; make the loop pure */
void scan(row_t *rows, size_t n) {
    const int64_t min_key = g_config.min_key;   /* localize */
    uint64_t hits = 0;                          /* local accumulation */
    for (size_t i = 0; i < n; i++)
        hits += (rows[i].key > min_key);         /* vectorizable */
    g_stats.hits += hits;                        /* write back once */
}
```

**General principle:** factor the hot loop into a **pure function that takes globals as arguments and returns results.** Gather global reads at entry, global writes at exit.
