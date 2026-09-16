---
name: cpp-perf-rules
description: Self-contained C/C++ performance and physical-design rulebook for DB-engine, storage, query-processing, and systems-level code, with citable rule IDs (MEM-03, BR-04, PAR-15, PHYS-01, A59, ...). Use when writing, reviewing, or optimizing performance-sensitive C/C++ code in CUBRID (hot loops, executor/scan paths, parallel workers, cache/false-sharing issues, vectorization, allocation, atomics, wait strategies, tail latency), when a perf regression, "more threads don't help", or a performance PR review comes up, and for refactoring / module-split / header-cleanup reviews (include insulation, cyclic dependencies, levelization, build-time cost, CCD).
---

# C/C++ Performance & Physical-Design Rulebook (Self-Contained)

Rulebook applied directly by AI agents and humans when writing or reviewing C/C++ code.
Every rule is citable by ID (e.g. "MEM-03 violation"). Target: DB engines, storage,
query processing, systems-level code. §0–§19 cover **runtime** cost; §20 (PHYS) covers
**build/dependency** cost (Lakos, *Large-Scale C++ Vol. I*). Upstream sources: the Korean
`C-Cpp-성능규칙집.md` and `대규모Cpp-물리설계-Lakos.md` notes; IDs are kept identical so
they can be cited across both.

## Usage protocol (for the agent)

1. **Demand bottleneck evidence before proposing any optimization.** No measurement data → answer "measure first". Guess-based optimization proposals are forbidden.
2. **Separate hot path from cold path first.** Do not apply this rulebook to cold paths (error handling, init, shutdown) — readability wins there.
3. **On rule conflict, priority is:** correctness > memory safety > readability > performance. Never introduce UB for performance.
4. **Give quantitative rationale.** Not "faster" — "removes N cache misses" or "eliminates M branches".
5. **Numeric thresholds are baselines.** They vary by architecture/workload; state that they must be tuned by measurement.
6. **Apply C rules to C code.** Do not propose modern C++ idioms (`shared_ptr`, RAII, templates) in a C codebase. In C, the ALIAS/CLOW sections replace the CPP section.
7. **Flag non-portable techniques.** When proposing GCC/Clang extensions (computed goto, `__builtin_*`, `__attribute__`), present a fallback path too.
8. **Never forget weak memory models.** Lock-free code that works on x86 commonly breaks on ARM64. Mention COH-10 alongside any atomics proposal.
9. **Read rate and count together; report median and dispersion together.** A miss *rate* that moves says nothing without `cache-references`/`instructions` (MEAS-06); a mean that improves with a fat tail is not an improvement (MEAS-07). Wall-clock median decides; counters only explain.
10. **Choose the chapter by the complaint.** Runtime (slow query, no scaling) → §0–§19. Build time, "one header rebuilds everything", "can't test this module alone", cyclic includes → §20 PHYS. When the two collide on a **hot path, performance wins** (PHYS-05 / CPP-09); on cold/init paths PHYS wins.
11. **Reuse verified low-level code before writing it.** Lock-free queues, ring buffers, spin/backoff loops (PAR-15/16): find the engine's existing implementation first; the rules are review criteria, not a build order.
12. **Two builds are two ELFs.** Before attributing any hot-loop timing delta to a source change, run the hot-symbol layout gate (MEAS-08, Appendix E). A cold-path edit that shrinks a function by 7 bytes can shift 8,000 symbols by 16 bytes and cost 1.5–10 % on an executor loop (CC-08). Mitigate by the CC-09 ladder — never blanket `-falign-functions`.

## Recurring failure axes in this repository (CUBRID)

When writing or reviewing perf-path code, **skim [CHECKLIST.md](CHECKLIST.md) first**, and prioritize these:

| Axis | Rules | Real case |
|---|---|---|
| **Compile-time branch left in the row loop** | BR-04, A59 | `step->aux ? A : B`, `step->domain == NULL` tested per row in expr compile → removed via kernel specialization |
| **Unconditional out-of-line call per row** | CC-05, A60 | `pr_clear_value()` called per row even for fixed-width slots → replaced with inline flag set on the NULL path |
| **Multi-switch dispatch in predicate eval** | BR-06, A61 | `switch(kind)` → `switch(fast_type)` → `switch(rel_op)` triple → (type×operator) leaf function pointer fixed at compile time |
| **Invariant pointer re-published per row** | BR-04, A62 | Slot-owning step wrote the same address to the cell every row → once at program generation |
| **Serial path optimized, parallel-only loop missed** | PAR-14, A63 | px BUILDVALUE_OPT accumulation loop was outside the hook — only queries without GROUP BY were neutral |
| **Hot struct without hot/cold split** | MEM-02/05 | 96B step struct carrying fields unused during execution (regu, branch ranges) |
| **Cross-thread ownership free** | ALLOC-08, A64 | Private-heap allocation freed by another worker → mspace abort |
| **Per-row digit normalization in fixed-point** | FP-07 | NUMERIC SUM rounding/packing per row → deferred carry, finalize once |
| **False sharing on parallel worker counters** | MEM-03, GLOB-02 | px per-worker stats/partial sums |
| **Floating-point `==` in cost/selectivity** | FP-01/02 | Asymmetric plan comparator (CBRD-27139) |
| **Perf verdict from a single run** | MEAS-04 | One pass on a polluted baseline misjudged as "regression" → median-of-3 + cross-validation required |
| **Verdict from a secondary indicator alone** | MEAS-06 | Histogram judged "full-scan statistics" from `buckets:300` when it was sampled (2026-08-28) — rate/aux metric looked normal, primary was not |
| **Build-dir move treated as free** | PHYS-06 | `CMakeCache` bakes paths into compile lines → one full rebuild is a fixed cost (2026-08-27 `.50` 1358 targets, 08-28 `.52`) |
| **Cold-path edit moved the hot path (link-phase regression)** | CC-08/09, MEAS-08, A80–A83 | CBRD-26382: `std::function`→lambda in `scope_exit` (recovery only) shrank a GCC 8 cold fragment 7 B → 8,287 symbols −16 B → executor `%32` phase flipped → +1.5 % (stable PC) / +10.56 % (QA) on `COUNT(*)`; 7-byte NOP padding control restored addresses and timing |
| **Header change rebuilds the world / module untestable alone** | PHYS-01/05 | Insulation missing, heavy layering — measure CCD (Appendix D) before refactoring |

## Priority decision procedure (when handed a perf problem)

Condensed — full text in [PRIORITY.md](PRIORITY.md). Work top-down; stop at the level that solves it. Step 3 matters most: one DRAM access ≈ 200–400 integer ops.

0. **Is it a runtime problem at all?** Build time / "everything recompiles" / "can't test alone" → [PHYSICAL-DESIGN.md](PHYSICAL-DESIGN.md), measure with the include graph and CCD (PHYS-06, Appendix D), not `perf`.
0.5 **Is the delta caused by the source at all?** Direct cost can't explain it (cold path, header/template/EH churn) → hot-symbol layout gate + padding control (MEAS-08, Appendix E) before touching the algorithm.
1. **Lower algorithmic complexity?** O(n²)→O(n log n) beats everything else combined. Remove work entirely (early exit, caching, dedup).
2. **Reduce the data touched?** Column pruning, compression, approximate structures (DS-05), read only needed fields.
3. **Improve memory access patterns?** MEM-01..07 — most real-world gains live here.
4. **Remove allocation?** ALLOC-01..05.
4.5 **Remove what blocks the compiler?** ALIAS-03/04 (aliasing), GLOB-01/10 (localize globals), BR-04-ext/CC-05 (compile-time branches & unconditional calls left in the run loop). Low risk, high gain — always try first. Multithreaded: check GLOB-02 and CPP-01 first (classic "more cores don't help" causes).
5. **Reduce branches and dependency chains?** BR-*, ILP (split partial sums).
6. **Parallelize?** PAR-*. Doing this before 3–5 just runs bad code on more cores. If threads don't scale, check in order: (1) false sharing MEM-03/GLOB-02, (2) atomic refcounting CPP-01, (3) allocator contention PAR-11, (4) contended atomics PAR-01, (5) NUMA PAR-09. Root fix is COH-12: replace sharing with partitioned ownership. Separate parallel-only loops → PAR-14. Producer–consumer queues: match sync to producer count (PAR-15) and state the wait strategy's CPU cost (PAR-16).
7. **Vectorize?** CC-03/04 — needs data layout (MEM-04) fixed first.
8. **Compiler options / PGO?** CC-01/02.
9. **Hand assembly / intrinsics** — last resort, only on a measured bottleneck's tiniest part.

## Rule files (load what the task needs)

| File | Sections |
|---|---|
| [COSTS.md](COSTS.md) | §0 Cost baselines (cycles) + hardware constants — memorize the orders of magnitude |
| [MEASUREMENT.md](MEASUREMENT.md) | §1 MEAS — measurement procedure, verdict baselines, benchmark hygiene, rate-vs-count (MEAS-06), tail/dispersion (MEAS-07), code-layout confound gate + padding control (MEAS-08) |
| [MEMORY-COHERENCY.md](MEMORY-COHERENCY.md) | §2 MEM (memory access — top category, incl. cache warming MEM-10), §3 COH (cache coherency, MESI, false sharing) |
| [ALIASING.md](ALIASING.md) | §4 ALIAS — strict aliasing, restrict, the biggest C optimization lever |
| [BRANCHES-ALLOCATION.md](BRANCHES-ALLOCATION.md) | §5 BR (branches, dispatch, condition order BR-07, error-flag + cold handler BR-08), §6 ALLOC (allocation, arenas) |
| [FP-GLOBALS.md](FP-GLOBALS.md) | §7 FP (floating point, comparators, float/double mixing FP-08), §8 GLOB (global/static/TLS state) |
| [PARALLEL.md](PARALLEL.md) | §9 PAR — atomics, memory order, NUMA, sync selection, single-producer ring (PAR-15), wait strategy (PAR-16) |
| [CPP-CLOW.md](CPP-CLOW.md) | §10 CPP (C++ feature costs, compile-time dispatch CPP-09), §11 CLOW (C low-level techniques) |
| [DATA-STRINGS-SERIAL.md](DATA-STRINGS-SERIAL.md) | §12 DS (data structures), §13 STR (strings), §14 SER (serialization) |
| [COMPILER-SYSTEM.md](COMPILER-SYSTEM.md) | §15 CC (compiler flags, PGO, vectorization, code-layout sensitivity CC-08, alignment/ordering mitigation ladder CC-09), §16 SYS (I/O & system, TCP_NODELAY SYS-05) |
| [ANTIPATTERNS.md](ANTIPATTERNS.md) | §17 Anti-pattern catalog A01–A83 — flag on sight with rule ID + alternative (A67+ are skill-local) |
| [PRIORITY.md](PRIORITY.md) | §19 Priority decision procedure — full text |
| [CHECKLIST.md](CHECKLIST.md) | §18 Review checklist — run for every performance-related change; Code-layout section for header/template/EH/cold-path edits; PHYS section for refactoring reviews |
| [PHYSICAL-DESIGN.md](PHYSICAL-DESIGN.md) | §20 PHYS — insulation, acyclic dependencies, 9 levelization techniques, layered→lateral, CCD, colocation, primitiveness, insulation techniques & their runtime price, symptom→prescription map |
| [APPENDIX.md](APPENDIX.md) | A: common macros/utilities, B: multithread scalability diagnosis procedure, C: external references & installed tools, D: include-graph / CCD measurement script (PHYS-06), E: hot-symbol layout gate script (CC-08 / MEAS-08) |
