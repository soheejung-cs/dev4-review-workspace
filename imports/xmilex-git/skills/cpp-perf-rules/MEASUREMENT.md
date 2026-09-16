# §1 Measurement (MEAS)

## MEAS-01 — Never propose optimization without measurement
80% of bottlenecks are where you did not predict. "This looks slow" from reading code is not accepted as evidence.

## MEAS-02 — Follow the 3-stage measurement procedure

```
Stage 1: Characterize — what is the bottleneck (memory? branches? compute?)
  perf stat -r 10 -e cycles,instructions,cache-misses,cache-references,\
    branch-misses,branch-instructions,stalled-cycles-frontend,\
    stalled-cycles-backend ./target

Stage 2: Locate — which function
  perf record -g --call-graph dwarf ./target
  perf report --sort=overhead,symbol

Stage 3: Explain — which instruction in that function
  perf annotate -s target_function
```

## MEAS-03 — Verdict baselines

| Metric | Formula | Normal | If bad, cause |
|---|---|---|---|
| IPC | instructions / cycles | > 1.5 | < 1.0 → memory stalls or dependency chains |
| Cache miss rate | cache-misses / cache-references | < 3% | > 5% → data layout |
| Branch miss rate | branch-misses / branch-instructions | < 1% | > 2% → unpredictable branches |
| Frontend stall | stalled-cycles-frontend / cycles | < 10% | > 20% → I-cache pressure, over-inlining |
| Backend stall | stalled-cycles-backend / cycles | < 20% | > 40% → memory stalls |

## MEAS-04 — Benchmark hygiene
- Compare the **median** of repeated runs; the mean is polluted by outliers. `perf stat -r 10`.
- Consume the result so the measured code isn't dead-code-eliminated (`DoNotOptimize` pattern).
- Pin CPU frequency scaling/turbo, or run long enough to average it out.
- Measure after warm-up; the first run is polluted by cache and page faults.
- **Measure with production-like data sizes.** Small data fits entirely in L1 and hides cache problems.

## MEAS-05 — Verify correctness after the improvement
Performance changes must pass functional regression tests. FP reordering, parallelization, and SIMD substitution can change results — verify explicitly.

## MEAS-06 — Never judge from a single metric (usage protocol for the MEAS-03 table)

The MEAS-03 thresholds are **suspicion signals, not verdicts**. Read one metric alone and you will read it backwards. Two measured cases (low-latency paper note §2.1, §3.5):

| Case | Miss rate | Instructions | Wall time |
|---|---|---|---|
| Cache cold → warm | 73.96% → 71.56% (≈ unchanged) | 4.93B → 12.01B (**up**) | 267.7ms → **25.6ms** |
| Combined optimizations | 16.0% → 33.9% (**2× worse**) | 6.01B → **3.27B** | **fastest** |

- The first case got faster because the **number of cache references** dropped, not the miss *rate*. Rate alone reads as "no improvement".
- The second has double the miss rate and is the fastest — total instruction count halved.
- So **record rate and absolute count together**: both `cache-misses` and `cache-references` from `perf stat`, plus `instructions`.
- The final verdict is always the **wall-clock median**. Hardware counters explain *why*; they never decide.

> Same failure shape outside the engine: a histogram was judged "full-scan statistics" from `buckets:300` alone when it was actually sampled (2026-08-28). **A secondary indicator looking normal is not evidence that the primary thing is normal.**

## MEAS-07 — Variance (the tail) is a metric, not just the mean

What a DB user feels is not the mean but the **slow tail**. Same mean with high variance means timeouts and SLA violations.

- When reporting an optimization, **write the dispersion (MAD or stddev) next to the median.**
  Measured: pair-trading stddev 4,233ns → 400ns — not just faster but **predictable** (paper note §3.4).
- Pre-allocation and fixed-size buffers contribute more to **reducing variance** than to the mean. Runtime allocation is not slow; it is **occasionally very slow** (same reason as ALLOC-01).
- If the mean improved while variance stayed high, do not call it an improvement.

## MEAS-08 — Exclude the code-layout confound before attributing a delta to source semantics

Any A/B on hot-loop code that lands in the same DSO compares **two different binaries**, not two versions of
one function (CC-08). Before saying "change X made the executor N % slower/faster", run the ELF gate:

```
1. Hot-symbol phase diff (Appendix E):  nm -n -S on A and B for the workload's top perf symbols
     → addr, size, addr % 32, addr % 64, raw-byte hash for each; count of symbols shifted
   No shift and identical bytes  → layout excluded, attribute to semantics.
   Shift (typically a uniform ±16 / ±32 across a long run of symbols) → continue.
2. Locate the first divergence: walk the sorted symbol list, find the first symbol whose size differs
   (in CBRD-26382 a 7-byte cold fragment 3 MB upstream of the executor).
3. Padding control: rebuild B with N non-executable bytes appended to that object's section so that the
   hot addresses return to A's phase, keeping B's logic. Same toolchain, same day, same flags.
   If timing follows the phase, the delta is layout, not the source change — say so in the PR/report.
4. Extend to phases 0/8/16/24/32 with independent rebuilds if the finding will drive a product decision
   (removes periodicity and build-date confounds).
```

Reporting rules:
- Give **paired** B/A ratios with a bootstrap CI from ≥ 20 runs per variant; publish the direction *and*
  whether the magnitude reproduced (stable host +1.46 % vs QA +10.56 % is a real, CPU-dependent spread).
- Read the **full Top-down L1/L2** for both variants. "DSB miss +52 %" alone is not a cause; in the
  measured case front-end bound went *down* while core bound went up. Say which slot moved.
- Restart protocol matters for reproduction (server/master restart per sample vs fresh `createdb`); state it.
  Confirm `read_bytes`/major-faults are 0 before ruling out I/O — don't argue disk from hardware specs.
- A control that changes hot bytes for a *different* reason (e.g. forced `noexcept`) is a **separate**
  variant, not evidence about layout: compare its hot-symbol hashes to B before interpreting its timing.
