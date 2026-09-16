# §9 Parallelism (PAR)

## PAR-01 — Replace atomics with thread-local accumulation

```cpp
/* BAD: atomic increment per row — 100–1000 cycles under contention */
std::atomic<uint64_t> total{0};
for (auto& r : rows) { process(r); total.fetch_add(1); }

/* GOOD: accumulate locally, merge once */
uint64_t local = 0;
for (auto& r : rows) { process(r); local++; }
total.fetch_add(local, std::memory_order_relaxed);
```

**Rule:** any atomic operation in a loop body is automatically a review target.

## PAR-02 — Specify the minimum sufficient memory order

| Purpose | Order | Cost |
|---|---|---|
| Independent counters / stat accumulation | `relaxed` | minimal |
| Cancel-flag polling | `relaxed` | minimal |
| Publishing data (producer) | `release` | medium |
| Acquiring data (consumer) | `acquire` | medium |
| Sequential consistency needed | `seq_cst` | maximal (the default) |

```cpp
counter.fetch_add(1, std::memory_order_relaxed);
ready.store(true, std::memory_order_release);        /* publishes prior writes */
if (ready.load(std::memory_order_acquire)) use();    /* guarantees later reads */
```

**Caution:** the default `seq_cst` is the safest and the most expensive. Relax only with a clear correctness argument.

## PAR-03 — Check cancellation in batches

```cpp
std::atomic<bool> abort{false};
/* BAD: check per row */
for (auto& r : rows) { if (abort.load()) return; process(r); }

/* GOOD: check per page/block */
for (size_t p = 0; p < npages; p++) {
    if (UNLIKELY(abort.load(std::memory_order_relaxed))) break;
    scan_page(p);     /* no checks inside */
}
```

## PAR-04 — Align work partitioning to cache-line boundaries
If per-thread ranges share a cache line, false sharing occurs. Align chunk starts to 64B (or page) boundaries.

## PAR-05 — Minimize lock scope; no allocation or I/O under a lock

```cpp
/* BAD */
{ std::lock_guard g(mtx); auto v = build_large_result(); results.push(v); }

/* GOOD */
auto v = build_large_result();                       /* prepare outside the lock */
{ std::lock_guard g(mtx); results.push(std::move(v)); }   /* minimal critical section */
```

## PAR-06 — First check that parallelization pays
Thread creation, synchronization, and merging cost cycles; small workloads run faster serially. Set lower bounds on workers/pages and fall back to serial below them.

```c
if (npages < MIN_PAGES_FOR_PARALLEL || avail_workers < 2)
    return scan_serial(...);
```

## PAR-07 — Propagate worker failure and early-terminate siblings
When one worker fails, the rest keep doing wasted work. Propagate to the coordinator via a shared abort flag + error-code collection.

## PAR-08 — Always clean up worker-thread thread-local state
Lingering connection entries, transaction indices, or error contexts misbehave on the next task. Guarantee with RAII.

## PAR-09 — Consider NUMA placement (multi-socket)

Linux uses a **first-touch policy**: a page is allocated on the node of **the thread that first touches it**, not at `malloc` time. Remote-node access is 1.5–2× local latency.

```c
/* BAD: master thread initializes everything → all pages on node 0 */
buf = malloc(size);
memset(buf, 0, size);
/* workers on node 1 then do all-remote access */

/* GOOD: each worker first-touches its own region */
buf = malloc(size);
parallel_for(0, nworkers, [&](int w) {
    auto [lo, hi] = worker_range(w, size);
    memset(buf + lo, 0, hi - lo);       /* allocated on its own node */
});
```

**Supporting measures:**

```bash
numactl --hardware                                  # inspect node topology
numactl --cpunodebind=0 --membind=0 ./server        # single-node experiment
```

```c
numa_alloc_onnode(size, node);                      /* explicit node placement */
pthread_setaffinity_np(...);                        /* pin threads to prevent migration */
```

If threads migrate between nodes, careful first-touch placement is wasted — **effective only combined with pinning.**

## PAR-10 — Pick synchronization suited to read-mostly data

`mutex` is overkill for read-mostly data. But `rwlock` readers also update a shared counter, which itself bottlenecks as cores grow.

| Mechanism | Fits | Read cost | Caution |
|---|---|---|---|
| `mutex` | balanced read:write | high | default choice |
| `rwlock` | reads ≥ 10:1 | medium (shared counter update) | counter bottlenecks with many cores |
| **seqlock** | reads dominant, short critical section | **low (no writes)** | readers must retry; no pointer reads |
| **RCU / epoch** | reads must be near-free | **minimal** | deferred free / grace-period management |
| per-CPU + sum | counters/statistics | minimal writes | reads are O(#cores) |
| **COW pointer swap** | rarely-updated config/metadata | **minimal (one atomic load)** | old-version free timing |

The COW pattern is simple and effective — especially useful for config and catalog caches.

```cpp
std::atomic<const config_t*> g_config;

/* Read: one atomic load. No lock */
const config_t *c = g_config.load(std::memory_order_acquire);
use(c->min_key);

/* Write: build a new object, swap only the pointer */
config_t *nc = new config_t(*old);  nc->min_key = v;
g_config.store(nc, std::memory_order_release);
/* Free the old object after all readers have left (epoch/RCU or deferred-free queue) */
```

## PAR-11 — Check allocator contention

`malloc` uses internal locks or arenas. A multithreaded allocation storm serializes in the allocator — a classic reason more cores don't help.

**Diagnose:** `perf record`; check whether `malloc`/`free`/`_int_malloc` or lock-related symbols top the profile.

**Response order:**
1. Remove the allocation itself (ALLOC-01, ALLOC-03) — the root fix
2. Per-thread arenas
3. Switch to a thread-caching allocator: `tcmalloc`, `jemalloc`, `mimalloc` (often a big win from just relinking)

## PAR-12 — Use spinning vs kernel waiting appropriately

Critical sections within a few hundred cycles favor spinning; longer favors kernel waiting. When spinning, use the `pause` instruction to yield resources to the hyperthread sibling and reduce power/contention.

```c
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

for (int i = 0; i < SPIN_LIMIT; i++) {          /* adaptive spin */
    if (try_lock()) return;
    cpu_relax();
}
lock_slow_path();                                /* then kernel wait */
```

**Caution:** raw spinning without `pause` significantly degrades the hyperthread sibling's performance.

## PAR-13 — Match thread count to workload and core count

More runnable threads than cores only adds context switches and cache pollution. Clamp the thread cap for stats/maintenance work by logical cores and target page count.

```c
int workers = min3(requested, num_online_cpus(), (int)(npages / MIN_PAGES_PER_WORKER));
if (workers < 2) return scan_serial(...);        /* PAR-06 */
```

## PAR-14 — Verify serial-path optimizations also landed in the parallel path

If parallel execution uses a **separate dedicated loop**, it's easy to improve only the serial path and miss the parallel one.
Symptoms: "the same query loses the improvement when run in parallel", "only certain query shapes are neutral."
For each improvement, **enumerate both serial and parallel consumption sites**, verify both, and record the list in the commit message or comments.

## PAR-15 — A single-producer queue needs neither locks nor CAS (pre-allocated ring buffer)

In a producer–consumer structure (WAL append, log buffer, statistics pipeline) with **one producer**, a pre-allocated ring buffer + sequence number + **memory barriers alone** guarantee visibility (the LMAX Disruptor principle). Measured 38–55% over a lock-based queue, widening as event count grows (paper note ch. 4).

- The real cost of a lock is not the lock instruction (~25ns) but that **contention is arbitrated via context switches, invalidating and refilling caches.**
- **Batching comes for free**: when the consumer observes the cursor several slots ahead, it drains up to that point without further synchronization — bursts are absorbed naturally.
- With multiple producers, use CAS only to claim a slot. A bounded queue that shares head/tail/size variables concentrates write contention on them (worst when combined with MEM-03 false sharing).
- Latency variance also shrinks (MEAS-07): measured σ 453,766ns → 53,600ns.
- ⚠ This falls under the "reuse verified low-level code" rule — **do not hand-write one**; look first for an existing verified implementation (e.g. the engine's lock-free queue). This rule is a review criterion, not a build order.

## PAR-16 — A wait strategy is a latency↔CPU trade-off. Choose it explicitly

How the consumer waits when there is no event is a design decision:

| Strategy | Latency | CPU | Fits |
|---|---|---|---|
| busy-spin | lowest | burns a core | extremely hot path with a dedicated core |
| spin N times → yield | low | medium | ordinary hot path (hybrid) |
| condvar/sleep | high (wake-up latency) | minimal | cold / background |

- A design that demands "fast" without stating the CPU cost is incomplete. busy-spin **buys latency with CPU** — stealing another worker's core can be a net loss for the system.
- DB equivalents: log-flush wait, worker-pool job wait, latch spin limits. CUBRID's spin-then-block parameters live on this axis.
