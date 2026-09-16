# §15 Compiler (CC)

## CC-01 — Baseline flags

```bash
-O2                 # safe default. Usually little difference vs -O3
-O3                 # aggressive vectorization/unrolling. Code growth → possible I-cache pressure
-march=native       # only when deployment targets are the same architecture
-mtune=generic      # when deployment targets vary
-flto               # link-time optimization. Cross-TU inlining
-fno-omit-frame-pointer   # accurate profiling (~1% perf cost)
-g                  # symbols. Keep in release builds to enable profiling
```

**Caution:** `-O3` is not always faster. Code bloat sometimes makes it slower — measure against `-O2`.

## CC-02 — Apply PGO (best effort-to-effect ratio)

```bash
# Pass 1: instrumented build
g++ -O2 -fprofile-generate -o app.inst ...
./app.inst < representative_workload      # representative workload is mandatory

# Pass 2: profile-guided build
g++ -O2 -fprofile-use -fprofile-correction -o app ...
```

Branch hints, inlining decisions, and hot/cold code placement get optimized from real measurements.
**PGO with a non-representative workload backfires.**

## CC-03 — Release aliasing to permit vectorization

```c
void axpy(double * restrict y, const double * restrict x, double a, size_t n);
```

`restrict` is non-standard in C++ — use `__restrict__` (GCC/Clang). Misuse is UB, so real non-overlap must be guaranteed. Details in the ALIAS section.

## CC-04 — Verify vectorization actually happened

```bash
g++     -O3 -fopt-info-vec -fopt-info-vec-missed src.cpp
clang++ -O3 -Rpass=loop-vectorize -Rpass-missed=loop-vectorize src.cpp
```

**Common vectorization blockers:** pointer aliasing, function calls in the loop, conditional branches, non-sequential access (gather), loop-carried dependencies, possible signed-integer overflow, unknown trip count.

## CC-05 — Control inlining deliberately

```c
static inline           /* small hot functions in headers */
__attribute__((always_inline))   /* force. Abuse → I-cache pressure */
__attribute__((noinline))        /* push cold paths out */
__attribute__((hot)) / ((cold))  /* code-placement hints */
```

**Rule:** splitting cold paths (error handling) out with `noinline` raises hot-path I-cache efficiency. This beats `always_inline` abuse.

> **Unconditional-per-row-call check:** an out-of-line function called every iteration of a hot loop is a cost in itself (5–25 cycles + an optimization barrier). If the function body "does nothing in most cases," hoist the condition to the call site or specialize so it's called only when needed. Watch especially for resource-cleanup/init utilities called unconditionally even for fixed-width types.

## CC-06 — Never rely on signed integer overflow

It is UB, and the compiler may remove the overflow check itself. If wrapping is needed, use unsigned or `__builtin_add_overflow`.

```c
if (__builtin_mul_overflow(a, b, &result)) goto overflow;
```

Use signed integers or `size_t` consistently for loop indices. Sign mixing can insert a sign-extension instruction per iteration (CLOW-07).

> The old idiom of `volatile` + post-hoc checking for overflow forces memory round trips and is slow.
> `__builtin_*_overflow` does the same check via the flags register and keeps the value in a register.

## CC-07 — Inspect the assembly

For important hot functions, look at the generated assembly directly. Verify the expected instructions (cmov, vector instructions, multiply substitution) appeared.

```bash
g++ -O3 -S -masm=intel -o - src.cpp | less
objdump -d --no-show-raw-insn -M intel binary | less
perf annotate -s hot_function
```

## CC-08 — Code layout is part of the binary's behaviour: a cold-path edit can move the hot path

**The CPU executes the linked DSO, not the source call graph.** A source change that is *never executed* by a
workload can still change that workload's speed, because it changes the byte size of some function, the linker
re-packs every symbol after it, and the **address phase** (`addr % 32`, `addr % 64`) of unrelated hot functions
flips. Intel's DSB (decoded-µop cache) and fetch/predictor structures are indexed by address in 32-byte windows,
so the same instructions at a different phase are fed to the core differently.

Measured chain (CBRD-26382, CUBRID PR #6636, GCC 8.3.1 `-O2`, `libcubrid.so`):

| Step | Observation |
|---|---|
| Source | `scope_exit<std::function<void()>>` → `scope_exit{lambda}` in `scope_exit.hpp` / `log_recovery_redo.hpp` (+45/−36 lines, recovery-only code) |
| First binary delta | `log_recovery_redo(...).cold` fragment in `.text.unlikely` shrank **−7 B**; next symbol −8 B after alignment; next boundary −16 B |
| Propagation | **8,287 consecutive symbols** moved −16 B, including the whole query executor |
| Hot functions | `qexec_execute_scan`, `fetch_val_list`, `qdata_evaluate_aggregate_list`, `qexec_execute_mainblock`, `scan_next_scan` — zero source change, all `% 32` phase flipped (0↔16) |
| PMU (5 runs) | MITE µops/query +12.7 %, DSB→MITE penalty +71.7 %, IPC −1.5 % (5/5 separated); Top-down **core bound +2.9 pp**, front-end bound *decreased* |
| Wall time | stable PC +1.46 % (95 % CI +1.04…+1.90, 20 paired runs); QA's older CPU **+10.56 %** on the same `COUNT(*)` 5-way Cartesian product |
| Control D | B's objects unchanged + **7 NOP bytes** appended to `log_recovery.c.o`'s `.text.unlikely` → hot addresses back to A, D faster than B by 1.5–1.8 % in both shared-DB and fresh-DB protocols |
| Non-fix | forcing the destructor `noexcept` (variant C): hot addresses and bytes identical to B, timing unchanged |

Rules that follow:

1. **"The changed function is not on the query path" proves nothing about performance.** It only bounds the
   *direct* execution cost. Layout cost must be excluded separately (MEAS-08 gate).
2. **What changes function size changes layout.** Suspect on sight, even in cold code: template instantiation
   vs type erasure (`std::function` ↔ concrete lambda), exception-specification / EH-cleanup changes,
   `noexcept`, inlining decisions, `hot`/`cold` splitting into `.text.unlikely`, COMDAT folding, and any
   edit to a header included by many TUs of the same DSO.
3. **Small workloads amplify it.** A tight executor loop iterated 10⁸–10⁹ times turns a per-iteration
   delta of a fraction of a cycle into whole-percent wall time. Storage and plan are constant (`read_bytes=0`,
   plan hash equal) — that is exactly when layout dominates.
4. **A layout-induced slowdown does not have one PMU fingerprint.** Here DSB/MITE supply changed *and*
   the Top-down classification moved to core-bound, not front-end-bound. Report the full Top-down L1/L2
   (retiring / front-end / back-end / bad-spec, memory vs core), not a single miss counter. Events like
   `DSB2MITE_SWITCHES.PENALTY_CYCLES` are not defined on every core generation — auxiliary signal only.
5. **Magnitude is CPU-dependent.** +1.5 % on Core Ultra was +10.6 % on the QA host; do not dismiss a
   regression because your workstation reproduces only the direction.
6. **A compiler upgrade is not a fix.** A newer GCC/Clang produces a *different* ELF that may merely
   happen to avoid this phase; without a profile it still does not know which functions are hot.

## CC-09 — Layout mitigation ladder (narrowest intervention first; never blanket alignment)

Apply only after MEAS-08 has shown that the hot-symbol phase moved *and* a padding control restores timing.

| Rung | Means | Guarantees | Does **not** guarantee / cost |
|---|---|---|---|
| 0 | Padding control (NOP bytes in the cold contribution, same toolchain) | causality: layout ↔ timing | diagnostic only, never ship |
| 1 | `__attribute__((aligned(32)))` on the **few proven** hot functions | entry-address alignment of those functions | inner-loop alignment, DSB residency, speed; linker max-alignment limit |
| 2 | PGO `-fprofile-generate` → representative workload → `-fprofile-use` (CC-02) | real hotness drives function order + placement | layout-only change — inlining/unrolling also move; needs a separate product variant and a multi-workload profile (OLTP, DDL, recovery, utilities) |
| 3 | `-falign-functions=32` globally | every entry on 32 B | `.text` growth (≤31 B, ≈15.5 B avg per function), I-cache/iTLB/DSB footprint; diagnosis flag, not a product default |
| 4 | `-ffunction-sections` + tiny ld script (`INSERT BEFORE .text`, verify with `-M` map) | deterministic final order | ABI/relocation/unwind/strip/ASLR verification; highest maintenance |
| 5 | AutoFDO / BOLT | profile-driven post-link layout | BOLT conflicts with `-freorder-blocks-and-partition` (on in GCC 8 `-O2` x86); shared-object support immature; stale-profile regressions |

Things that **look** like fixes and are not:
- Adding `-falign-functions` / `-freorder-functions` by name — `-O2` already enables both; only an explicit
  value, a profile, or section control changes anything.
- `__attribute__((hot))` / `no_reorder` — hints for the compiler's subsection choice; final DSO order is
  the linker's, and `hot` is ignored under `-fprofile-use`.
- `ALIGN(0x20)` on an output section — aligns the section start, not each function in it.
- Padding NOPs *inside* a hot path (loop/label alignment) can execute on the fall-through edge; function-entry
  padding does not, but both cost footprint. "32 B is the DSB unit, so align everything to 32" is wrong:
  set/way conflicts, branch count, µop count and working-set size act together and differ per generation.

---

# §16 I/O & System (SYS)

## SYS-01 — Batch system calls
One syscall costs 500–2,000 cycles. Buffer instead of repeating small reads/writes.

## SYS-02 — Tell the kernel your access pattern

```c
posix_fadvise(fd, off, len, POSIX_FADV_SEQUENTIAL);  /* sequential scan */
posix_fadvise(fd, off, len, POSIX_FADV_WILLNEED);    /* will read soon */
posix_fadvise(fd, off, len, POSIX_FADV_DONTNEED);    /* prevent cache pollution */
madvise(ptr, len, MADV_SEQUENTIAL | MADV_WILLNEED);
```

On large sequential scans, cleaning the page cache with `DONTNEED` protects other workloads' cache.

## SYS-03 — Propagate page-fault errors
Ignoring page fix/pin failure reads bad data. Always check return values.

## SYS-04 — Use shell builtins where fork overhead bites

When process resources are exhausted (e.g. a crash loop), external commands fail to execute. Build diagnosis/recovery paths from shell builtins only.

```bash
# work without fork (builtins)
echo, printf, read, cd, kill, exec, [[ ]], while, for
# need fork (fail under exhaustion)
ls, cat, ps, grep, awk, sed
```

## SYS-05 — Disable Nagle on small request–response sockets (TCP_NODELAY)

Nagle's algorithm **waits for an ACK** to coalesce small packets — in a request–response (RPC) pattern that wait is added straight onto round-trip latency. One socket option removes it:

```c
int yes = 1;
setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof (yes));
```

- DB relevance: **client↔broker↔CAS↔server are all request–response sockets.** In a many-small-queries workload, a missing NODELAY anywhere on the connection path can add tens of ms per query.
- Bulk streaming (result-set bulk transfer) may benefit from Nagle instead — decide per pattern.
- Review point: in code that opens a new socket, confirm whether the presence/absence of this option is **intent or omission**.
