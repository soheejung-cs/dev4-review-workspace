# Appendix A: Frequently Used Macros & Utilities

```c
/* branch hints */
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

/* cache line */
#define CACHE_LINE 64
#define CACHE_ALIGNED __attribute__((aligned(CACHE_LINE)))

/* array length (compile error if used on a pointer) */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* power-of-two alignment */
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define IS_POW2(x)       ((x) && !((x) & ((x) - 1)))

/* safe unaligned loads */
static inline uint32_t load_u32(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
static inline uint64_t load_u64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }

/* integer hash (splitmix64 family) */
static inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* relative-error floating-point equality */
static inline bool fp_eq_rel(double a, double b, double eps) {
    double d = fabs(a - b);
    double m = fmax(fabs(a), fabs(b));
    return d <= eps * fmax(m, 1.0);
}

/* prevent dead-code elimination in benchmarks */
#define DO_NOT_OPTIMIZE(x) __asm__ volatile("" : : "r,m"(x) : "memory")

/* yield the core while spin-waiting (PAR-12) */
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

/* cache-line-isolated global declaration (GLOB-02) */
#define HOT_GLOBAL(type, name) CACHE_ALIGNED type name

/* sharded counter (GLOB-09) */
#define STAT_SHARDS 64
typedef struct { uint64_t v; char _pad[CACHE_LINE - sizeof(uint64_t)]; } stat_shard_t;
_Static_assert(sizeof(stat_shard_t) == CACHE_LINE, "shard must be one cache line");

/* compile-time struct size verification (MEM-02, CLOW-15) */
#define ASSERT_SIZE(T, n)   _Static_assert(sizeof(T) == (n), #T " size changed")
#define ASSERT_FITS_LINE(T) _Static_assert(sizeof(T) <= CACHE_LINE, #T " exceeds cache line")
#define ASSERT_OFFSET(T, f, n) _Static_assert(offsetof(T, f) == (n), #T "." #f " moved")

/* compiler barrier — no instruction, only blocks reordering (COH-10) */
#define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* safe type reinterpretation (ALIAS-01) — memcpy optimizes to a single load */
#define BIT_CAST(dst_type, src) \
    __builtin_choose_expr(sizeof(dst_type) == sizeof(src), \
        ({ dst_type _d; memcpy(&_d, &(src), sizeof _d); _d; }), (void)0)

/* overlap check — for restrict path branching (ALIAS-03) */
#define NO_OVERLAP(a, b, n) ((const char*)(a) + (n) <= (const char*)(b) || \
                             (const char*)(b) + (n) <= (const char*)(a))

/* zero-safe bit ops (CLOW-13) */
static inline int clz64_safe(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
static inline int ctz64_safe(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }

/* HyperLogLog rank — strip top p bits, then leading zeros + 1 */
static inline int hll_rank(uint64_t w, int p) {
    uint64_t rest = w << p;
    return rest ? __builtin_clzll(rest) + 1 : (64 - p + 1);
}
```

---

# Appendix B: Multithread Scalability Diagnosis Procedure

Narrow down the cause of "more cores don't make it faster", in order.

```bash
# 0) First plot the throughput curve by thread count
for t in 1 2 4 8 16 32; do THREADS=$t ./bench; done
#   - linear rise then plateau     → resource saturation (memory bandwidth, I/O)
#   - decline past a point         → contention (false sharing, locks, atomics)
#   - no improvement from the start → serial-section dominance (Amdahl) or everyone waiting on one lock

# 1) Check cache-line contention (false sharing / atomic counters)
perf c2c record -- ./bench
perf c2c report --stdio
#   Top HITM (fetched from another core's cache) symbols are the culprits.
#   Global variable names → GLOB-02, struct arrays → MEM-03, refcounts → CPP-01

# 2) Check lock waiting
perf record -e sched:sched_switch -g -- ./bench     # where context switches are triggered
perf lock record ./bench && perf lock report        # kernel locks (where supported)
#   futex-related symbols at the top → mutex contention

# 3) Check the allocator
perf report | grep -iE 'malloc|free|arena|tcache'
#   near the top → PAR-11

# 4) Check NUMA
numastat -p $(pidof bench)          # per-node memory placement
perf stat -e node-load-misses,node-store-misses ./bench
#   high remote-access ratio → PAR-09

# 5) Check memory-bandwidth saturation
perf stat -e uncore_imc/data_reads/,uncore_imc/data_writes/ ./bench
#   at the bandwidth limit → not a contention problem but algorithm/data-volume (return to priority steps 1–2)
```

## Symptom → cause mapping

| Symptom | Check first | Rules |
|---|---|---|
| More threads make it slower | false sharing | MEM-03, GLOB-02 |
| Read-only yet doesn't scale | atomic reference counting | CPP-01 |
| Read-only yet doesn't scale (2) | rwlock reader counter | PAR-10 |
| Low IPC, high backend stalls | memory stalls | MEM-01, MEM-04, PAR-09 |
| `malloc` tops the profile | allocator contention | ALLOC-01, PAR-11 |
| 100% CPU but no progress | spin loops | PAR-12 |
| Degrades only as sockets grow | NUMA remote access | PAR-09 |
| Fast only with 1 thread | serial-section dominance | rethink the algorithm |
| Improvement vanishes only in parallel | parallel-only loop missed | PAR-14 |
| Mean is fine, p99/timeouts are not | allocation or wait-strategy jitter | MEAS-07, ALLOC-01, PAR-16 |
| Producer–consumer queue tops the lock profile | single producer paying for locks/CAS | PAR-15 |
| Counters "improved" but wall-clock did not | rate read without count | MEAS-06 |
| First query after restart / after idle is slow | cold I-/D-cache on a rare path | MEM-10 |
| Many tiny queries each cost tens of ms over the wire | Nagle on a request–response socket | SYS-05 |

---

# Appendix C: External Sources & Tools (optional reference)

The body is written to be sufficient on its own; consult these only for deeper dives.

| Source | Link | Notes |
|---|---|---|
| **Optimizing software in C++** (Agner Fog) | `agner.org/optimize/optimizing_cpp.pdf` | free; the de-facto standard document |
| **Instruction tables** (Agner Fog) | `agner.org/optimize/instruction_tables.pdf` | instruction latency/throughput |
| **Microarchitecture** (Agner Fog) | `agner.org/optimize/microarchitecture.pdf` | pipelines & branch-predictor internals |
| **What Every Programmer Should Know About Memory** (Drepper) | `lwn.net/Articles/250967/` | the classic on cache hierarchies |
| **Intel Optimization Reference Manual** | Intel developer site | official docs |
| **Modern Microprocessors: A 90-Minute Guide** | `lighterra.com/papers/modernmicroprocessors/` | condensed CPU internals |
| *Performance Analysis and Tuning on Modern CPUs* (Bakhvalov) | free PDF | practical perf/VTune |
| *Data-Oriented Design* (Richard Fabian) | free online | background for MEM-04 |
| Blogs | `easyperf.net`, `godbolt.org`, `quick-bench.com` | assembly & microbenchmarks |
| *Large-Scale C++ Vol. I: Process and Architecture* (Lakos, 2019) | Addison-Wesley | source of §20 PHYS — levelization, insulation, CCD. Digest: `notes/대규모Cpp-물리설계-Lakos.md` |
| Low-latency pattern measurements (HFT paper digest) | `notes/저지연패턴-HFT논문.md` | the ns/ms figures cited in MEAS-06/07, MEM-10, BR-07/08, FP-08, PAR-15, CPP-09 |

**Tools:** `perf` (stat/record/annotate/c2c/lock), Intel VTune (Top-Down), Valgrind (cachegrind/callgrind),
`pahole` (struct layout), `numastat`/`numactl` (NUMA), Compiler Explorer (assembly).

> Installed in this container: `~/tools` has perf 4.18 + FlameGraph, heaptrack 1.2, bpftrace,
> VTune **2025.0** (the 2026 version doesn't support Cascade Lake). Usage: `dev/profiling-guide.md`.

---

# Appendix D: Physical-Design Measurement (PHYS-06)

Parses only `#include` directives — no C++ parsing — and reports component count, edges, **CCD**, and the strongly-connected components (cycles). Component = files sharing a root name (`foo.c`/`foo.h` → `foo`), the Lakos convention CUBRID mostly follows. Run it before and after a refactoring and put both numbers in the PR.

```python
#!/usr/bin/env python3
"""include-graph metrics: cycles (PHYS-02), CCD (PHYS-06).  usage: incgraph.py <src-root>"""
import os, re, sys
from collections import defaultdict

ROOT = sys.argv[1]
INC = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
EXT = ('.c', '.cpp', '.cc', '.h', '.hpp')
comp = lambda path: os.path.splitext(os.path.basename(path))[0]

deps = defaultdict(set)
for dp, _, fs in os.walk(ROOT):
    for f in fs:
        if not f.endswith(EXT): continue
        c = comp(f); deps.setdefault(c, set())
        with open(os.path.join(dp, f), errors='replace') as fh:
            for line in fh:
                m = INC.match(line)
                if m and comp(m.group(1)) != c: deps[c].add(comp(m.group(1)))
local = set(deps)                         # system headers = level 0 → dropped
for c in deps: deps[c] &= local

# Tarjan SCC — any SCC larger than 1 is a cycle
sys.setrecursionlimit(1 << 20)
idx, low, on, st, sccs, n = {}, {}, set(), [], [], [0]
def strong(v):
    idx[v] = low[v] = n[0]; n[0] += 1; st.append(v); on.add(v)
    for w in deps[v]:
        if w not in idx: strong(w); low[v] = min(low[v], low[w])
        elif w in on:    low[v] = min(low[v], idx[w])
    if low[v] == idx[v]:
        s = []
        while True:
            w = st.pop(); on.discard(w); s.append(w)
            if w == v: break
        sccs.append(s)
for v in list(deps):
    if v not in idx: strong(v)
cycles = sorted((s for s in sccs if len(s) > 1), key=len, reverse=True)

# CCD = Σ |transitive link set incl. self|
def reach(v):
    seen, todo = {v}, [v]
    while todo:
        for w in deps[todo.pop()]:
            if w not in seen: seen.add(w); todo.append(w)
    return seen
link = {v: len(reach(v)) for v in deps}
ccd = sum(link.values())

print(f"components={len(deps)} edges={sum(map(len, deps.values()))} "
      f"CCD={ccd} avg_link_set={ccd / max(len(deps), 1):.1f} cycles={len(cycles)}")
for s in cycles[:5]:
    print(f"CYCLE size={len(s)}: {' '.join(sorted(s)[:12])}{' ...' if len(s) > 12 else ''}")
print("heaviest link sets:", sorted(link.items(), key=lambda kv: -kv[1])[:10])
```

Reading the output in CUBRID:
- Expect **one very large SCC** at first — a legacy engine is usually a single knot. The metric that matters is that knot **shrinking** and CCD **dropping** between two commits, not the absolute value.
- Level numbers (leaf = 1) can only be assigned once a component is outside every cycle; components inside the big SCC have no level.
- Components with the heaviest link sets are the insulation targets (PHYS-01, PHYS-10): every client of them recompiles on every change beneath.

---

# Appendix E: Hot-Symbol Layout Gate (CC-08 / MEAS-08)

Compares two builds of the same DSO **before** a perf verdict is attributed to a source change. Reports, for
each hot symbol, address / size / `% 32` / `% 64` / raw-byte hash in A and B, then the **first symbol whose
size diverges** (the origin of the shift) and how many symbols moved. Needs only `nm`, `objdump`, `python3`.
Pass the perf top-N symbol names of the gated workload; without a list it uses the CUBRID executor set from
CBRD-26382.

```python
#!/usr/bin/env python3
"""layout-gate.py A.so B.so [sym ...]  — hot-symbol address-phase diff (CC-08, MEAS-08)"""
import hashlib, subprocess, sys

DEFAULT_HOT = ["qexec_execute_scan", "fetch_val_list", "qdata_evaluate_aggregate_list",
               "qexec_execute_mainblock", "scan_next_scan"]

def symtab(path):
    """name -> (addr, size), sorted by addr; C++ names demangled, only defined text symbols."""
    out = subprocess.run(["nm", "-n", "-S", "-C", "--defined-only", path],
                         capture_output=True, text=True, check=True).stdout
    tab = {}
    for line in out.splitlines():
        f = line.split(None, 3)
        if len(f) == 4 and f[2] in "tTwW":
            name = f[3].split("(")[0]          # strip C++ signature
            if "[clone " in f[3]:               # GCC hot/cold split: keep ".cold.N" fragments distinct
                name += f[3][f[3].index("[clone ") + 7:].rstrip("]")
            tab.setdefault(name, (int(f[0], 16), int(f[1], 16)))
    return tab

def body_hash(path, addr, size):
    raw = subprocess.run(["objdump", "-s", f"--start-address={addr:#x}", f"--stop-address={addr+size:#x}", path],
                         capture_output=True, text=True).stdout
    data = "".join("".join(l.split()[1:5]) for l in raw.splitlines() if l.startswith(" "))
    return hashlib.sha1(data.encode()).hexdigest()[:10]

a_path, b_path = sys.argv[1], sys.argv[2]
hot = sys.argv[3:] or DEFAULT_HOT
A, B = symtab(a_path), symtab(b_path)

print(f"{'symbol':40} {'A addr':>10} {'B addr':>10} {'Δ':>5} {'A%32':>4} {'B%32':>4} {'A%64':>4} {'B%64':>4} {'sizeA':>6} {'sizeB':>6} bytes")
moved = 0
for s in hot:
    if s not in A or s not in B:
        print(f"{s:40} MISSING in {'A' if s not in A else 'B'}"); continue
    (aa, asz), (ba, bsz) = A[s], B[s]
    same = "same" if body_hash(a_path, aa, asz) == body_hash(b_path, ba, bsz) else "DIFF"
    moved += aa != ba
    print(f"{s:40} {aa:#10x} {ba:#10x} {ba-aa:>+5} {aa%32:>4} {ba%32:>4} {aa%64:>4} {ba%64:>4} {asz:>6} {bsz:>6} {same}")

# origin of the shift: first common symbol (in A address order) whose size differs, and shift statistics
common = sorted(set(A) & set(B), key=lambda n: A[n][0])
shifted = sum(1 for n in common if A[n][0] != B[n][0])
first = next(((n, A[n][1], B[n][1]) for n in common if A[n][1] != B[n][1]), None)
print(f"\ncommon symbols={len(common)} shifted={shifted} hot moved={moved}/{len(hot)}")
print("first size divergence:", f"{first[0]}  size {first[1]:#x} -> {first[2]:#x} ({first[2]-first[1]:+d} B)" if first else "none")
print("VERDICT:", "LAYOUT CONFOUND — run the padding control (MEAS-08 step 3) before attributing timing to the source change"
      if moved else "hot symbols stationary — layout excluded for this hot set")
```

Reading it:
- `hot moved > 0` with `bytes=DIFF` but no source change to that function is the CBRD-26382 signature: PC-relative
  displacements re-encoded because the function moved. `same` + stationary means the two DSOs are equivalent for
  that function and the delta is somewhere else.
- The "first size divergence" is where to append the padding control (`.text.unlikely` of that object) — the
  number of bytes to restore is `−Δsize`, rounded to whatever brings the hot symbols' `% 32` back to A.
- A shift that is uniform (e.g. every hot symbol `−16`) is one cascade; mixed shifts mean several independent
  size changes, each of which needs its own control.
- Run this as a PR gate on the release toolchain (the CentOS 6 / devtoolset-8 build), not on the developer
  compiler: a different compiler produces a different layout and a different verdict (CC-08 §6).
