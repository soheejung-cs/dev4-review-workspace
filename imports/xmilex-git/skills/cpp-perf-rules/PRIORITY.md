# §19 Priority Decision Procedure (full text)

When handed a performance problem, proceed in this order. If a level solves it, do not descend further.

**Step 0 — is it actually a runtime problem?** If the complaint is build time, "one header change rebuilds everything", or "can't test this module alone", it is a **physical-design** problem: go to [PHYSICAL-DESIGN.md](PHYSICAL-DESIGN.md) and measure with the include graph / CCD (PHYS-06) instead of `perf`. The two axes share MEAS-01 (measure first) and nothing else.

**Step 0.5 — is the delta even caused by the source semantics?** Two builds are two ELFs. If a benchmark moved after an edit whose direct cost cannot explain it (cold path, header-only, `noexcept`/template/`std::function` churn), run the hot-symbol phase gate (MEAS-08, Appendix E) and a padding control **before** descending into steps 1–9. Optimizing the algorithm to compensate for a 16-byte link-phase shift is wasted work; the mitigation ladder is CC-09.

```
1. Can algorithmic complexity be lowered?
   → Turning O(n²) into O(n log n) beats the sum of all other optimizations.
   → Can unnecessary work be removed entirely? (early exit, caching, dedup)

2. Can the amount of data accessed be reduced?
   → column pruning, compression, approximate structures (DS-05), read only needed fields

3. Can memory access patterns be improved?
   → MEM-01 ~ MEM-07. Most real-world gains come from here.

4. Can allocation be removed?
   → ALLOC-01 ~ ALLOC-05

4.5 Can whatever prevents the compiler from optimizing be stripped away?
   → ALIAS-03/04: release aliasing assumptions. The #1 cause of vectorization failure.
   → GLOB-01, GLOB-10: localize globals. This alone often unlocks loop optimization.
   → BR-04 extension, CC-05: remove compile-time branches and unconditional calls left in the run loop.
   → This step doesn't change the algorithm, so risk is low and gain is high. Always try it first.
   → If multithreaded, check GLOB-02 (global false sharing) and CPP-01 (shared_ptr refcount) first.
     These two are the classic reasons more cores don't raise performance.

5. Can branches and dependency chains be reduced?
   → BR-*, improve ILP (split partial sums, etc.)

6. Can it be parallelized?
   → PAR-*. But without doing 3–5 first, you're running bad code on more cores.
   → If more threads don't raise performance, check in order:
     (1) false sharing — MEM-03, GLOB-02
     (2) atomic reference counting — CPP-01
     (3) allocator contention — PAR-11
     (4) contended atomic ops — PAR-01
     (5) NUMA remote access — PAR-09
   → The root remedy is COH-12: remove sharing, switch to partitioned ownership.
     A design that removes sharing itself always beats optimizing locks/atomics.
   → If a separate parallel-only loop exists, check for missed improvements via PAR-14.

7. Can it be vectorized?
   → CC-03, CC-04. Data layout (MEM-04) must be sorted out first.

8. Compiler options / PGO
   → CC-01, CC-02. Gains without code changes.

9. Hand assembly / intrinsics
   → Last resort. High maintenance cost — only on the tiniest part of a measured bottleneck.
```

**Step 3 is the most important.** On modern CPUs one DRAM access equals 200–400 integer ops, so improving memory access patterns almost always beats compute optimization.
