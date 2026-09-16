# §18 Review Checklist

## Mandatory (every performance-related change)
- [ ] Is bottleneck measurement data attached (MEAS-01)
- [ ] Is the improvement figure a median of repeated runs (MEAS-04)
- [ ] **Were absolute counts recorded alongside rates** (MEAS-06)
- [ ] **Is dispersion (MAD/stddev) reported next to the median** (MEAS-07)
- [ ] Did functional regression tests pass (MEAS-05)
- [ ] Are result-changing modifications (FP order, parallelism, SIMD) stated explicitly
- [ ] **If a hot-loop benchmark moved, was the hot-symbol address/phase diff (`nm -n -S`, Appendix E) run on A vs B before attributing the delta to the source change** (MEAS-08 / A80)

## Code layout (any edit to a header shared by many TUs, to template/EH/`noexcept`/`std::function`/lambda code, or to `.text.unlikely` cold paths in the same DSO as an executor loop)
- [ ] Did the top perf symbols of the gated workload keep address phase (`% 32`, `% 64`) and raw bytes (CC-08)
- [ ] If they shifted: was the first size-diverging symbol located and a padding control (same toolchain) built to separate layout from semantics (MEAS-08)
- [ ] Was the verdict read from the full Top-down L1/L2 of both variants, not from one DSB/MITE counter (MEAS-08 / A83)
- [ ] Was the "not executed by this query" argument used only for direct cost, never to skip the ELF gate (A81)
- [ ] Is any alignment fix narrowed to proven hot functions or PGO, not global `-falign-functions` (CC-09 / A82)
- [ ] Was the magnitude confirmed on the CPU generation that reported it, not only on the developer host (CC-08 §5)

## Memory
- [ ] Does no hot struct needlessly exceed 64B (MEM-02)
- [ ] No false sharing on parallel counters (MEM-03)
- [ ] Does layout (AoS/SoA) match the access pattern (MEM-04)
- [ ] Are hot/cold fields separated (MEM-05)
- [ ] No allocation inside loops (ALLOC-01)
- [ ] Are containers pre-reserved (ALLOC-02)
- [ ] Is memory that may be freed cross-thread on the global heap (ALLOC-08)
- [ ] No float/double mixed expressions in hot loops (FP-08)

## Compute & branches
- [ ] Any unpredictable branch in a hot loop (BR-03)
- [ ] Are loop-invariant conditions hoisted out (BR-04)
- [ ] **No re-testing of compile-time-fixed values per row** (BR-04 / A59)
- [ ] **Does the out-of-line function called unconditionally per row actually do work** (CC-05 / A60)
- [ ] **Is dispatch (switch/virtual call) not re-decided inside the loop** (BR-06 / A61)
- [ ] **In per-row conditions, is the cheap, most-rejecting test first** (BR-07)
- [ ] **No error-check chain strung along the hot path** (BR-08)
- [ ] **Is the error handler `noinline`** (BR-08)
- [ ] **No store writing the same value every iteration** (BR-04 / A62)
- [ ] Was vectorization actually confirmed (CC-04)
- [ ] No repeated division (FP-04, CLOW-06)
- [ ] Are overflow checks done with builtins (CC-06)

## Parallel & system
- [ ] Does the producer–consumer queue's synchronization match the producer count — lock/CAS on a single producer is overkill (PAR-15)
- [ ] Is the CPU cost of the wait strategy (spin/yield/sleep) stated (PAR-16)
- [ ] On a newly opened request–response socket, is TCP_NODELAY presence/absence intentional (SYS-05)

## Physical design (refactoring / module-split reviews only)
- [ ] Does every `#include` added to a header fall under the 5 cases (Is-A/Has-A/Inline/Enum/Typedef) (PHYS-01)
- [ ] Was a header included where a forward declaration suffices (PHYS-01)
- [ ] Any reliance on a transitive include (PHYS-01)
- [ ] No new cyclic dependency introduced (PHYS-02)
- [ ] Was the cycle removed by escalation/demotion rather than papered over (PHYS-03)
- [ ] Does a new type own or delete sibling objects instead of a manager (PHYS-03)
- [ ] No abstract interface inserted on a hot path that creates virtual calls (PHYS-05 ⚠ collides with CPP-09)
- [ ] Was CCD / cycle count measured before and after (PHYS-06)
- [ ] Does the implementation file include its own header first (PHYS-07)
- [ ] Do the test driver's dependencies stay within the component's (PHYS-07)
- [ ] If two public types share a component, which of the 4 colocation reasons applies (PHYS-08)
- [ ] Was a non-primitive operation added to the type instead of a higher utility (PHYS-09)
- [ ] Is the insulating wrapper/protocol placed high enough that its runtime price is acceptable (PHYS-10)
- [ ] No runtime-initialized file-scope static, no local `extern`, no `using` at header scope in new code (PHYS-11)

## Floating point
- [ ] No `==` comparison (FP-01)
- [ ] Was comparator symmetry verified with a probe (FP-02)
- [ ] If the comparator is used for sorting, was transitivity checked (FP-02 warning)
- [ ] No `-ffast-math` in correctness-sensitive code (FP-03)
- [ ] In fixed-point bulk summation, is the carry deferred and every flush point exhaustively checked (FP-07)

## Aliasing (C — first check on vectorization failure)
- [ ] No bit reinterpretation via type casts (ALIAS-01)
- [ ] No reliance on `-fno-strict-aliasing` (ALIAS-01)
- [ ] Byte loops replaced by `mem*` functions (ALIAS-02)
- [ ] `restrict` on non-overlapping pointer args (ALIAS-03)
- [ ] Loop accumulation done in locals (ALIAS-04)
- [ ] No alias mentions in vectorization-failure messages (ALIAS-07)

## Cache coherency (multithreaded)
- [ ] One writer per cache line (COH-01, COH-04) — **top-level protocol**
- [ ] Were top HITM symbols checked in `perf c2c` (COH-02)
- [ ] Is read-only data on different lines from written data (COH-04)
- [ ] No cross-thread shared bitfields (COH-07)
- [ ] Are atomics naturally aligned (COH-08)
- [ ] Are ring-buffer head/tail separated (COH-06)
- [ ] Validated on a weak memory model such as ARM (COH-10)
- [ ] Can sharing be replaced by partitioning/transfer (COH-12)

## C low-level
- [ ] `static` on internal symbols (CLOW-01)
- [ ] Error handling is `goto` single-exit (CLOW-04)
- [ ] No runtime division inside loops (CLOW-06)
- [ ] Index types unified (CLOW-07)
- [ ] No VLA/`alloca` (CLOW-08)
- [ ] Struct padding pinned with `_Static_assert` (CLOW-15)

## Global & static state
- [ ] Global accesses in loops moved into locals (GLOB-01)
- [ ] Hot globals not on the same cache line (GLOB-02) — verify addresses with `nm`
- [ ] Read-only globals are `const` (GLOB-03)
- [ ] No `volatile` for synchronization (GLOB-04)
- [ ] `thread_local` lookups moved out of loops (GLOB-05)
- [ ] Global counters sharded (GLOB-09)
- [ ] Can the hot loop be factored into a global-free pure function (GLOB-10)

## C++ features
- [ ] No `shared_ptr` copies on hot paths (CPP-01) — **multithread top priority**
- [ ] Range-for uses `const auto&` (CPP-02)
- [ ] No `return std::move(local)` (CPP-02)
- [ ] Move operations marked `noexcept` (CPP-03)
- [ ] No exceptions on the normal flow (CPP-03)
- [ ] No `dynamic_cast` on hot paths (CPP-04)

## Parallel
- [ ] Atomics replaced with thread-local accumulation (PAR-01)
- [ ] Memory orders not excessive (PAR-02)
- [ ] Cancellation checked in batches (PAR-03)
- [ ] Serial fallback condition present (PAR-06)
- [ ] Worker errors propagated to the coordinator (PAR-07)
- [ ] Thread-local state cleaned up (PAR-08)
- [ ] First-touch placement considered on multi-socket (PAR-09)
- [ ] No excessive synchronization on read-mostly data (PAR-10)
- [ ] Allocator contention absent from the top of the profile (PAR-11)
- [ ] `cpu_relax()` in spin loops (PAR-12)
- [ ] Was an existing verified lock-free queue reused rather than hand-written (PAR-15)
- [ ] **Serial-path improvements applied to parallel-only paths too** (PAR-14 / A63)

## Data & serialization
- [ ] Approximate-structure merge identical to single-pass processing (DS-05)
- [ ] Hash functions unified across serial/parallel paths (DS-03)
- [ ] External-format parsing validated (SER-02)
- [ ] Reads free of alignment violations (SER-03)
