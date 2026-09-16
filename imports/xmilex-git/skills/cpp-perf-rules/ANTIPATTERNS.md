# §17 Anti-pattern Catalog

Flag on sight. When found, cite the rule ID and present the alternative.
A01–A66 match the upstream 성능규칙집 note; **A67–A83 are skill-local extensions** for the rules added later (MEAS-06/07/08, BR-07/08, FP-08, PAR-15/16, SYS-05, PHYS, CC-08/09).

| # | Anti-pattern | Violates | Alternative |
|---|---|---|---|
| A01 | `new`/`malloc` inside a hot loop | ALLOC-01 | reserve outside the loop, arena |
| A02 | `push_back` loop without `reserve()` | ALLOC-02 | `reserve(n)` |
| A03 | atomic op in the loop body | PAR-01 | local accumulate, merge once |
| A04 | per-worker counters in an adjacent array | MEM-03 | `alignas(64)` + padding |
| A05 | floating-point `==` comparison | FP-01 | relative epsilon |
| A06 | component-wise compare then order by sum | FP-02 | single-scalar comparison |
| A07 | virtual call inside a hot loop | BR-06 | templates, loop splitting |
| A08 | sequential traversal of `std::list`/`std::map` | DS-01 | vector, sorted vector |
| A09 | division per loop iteration | FP-04 | reciprocal multiply (check reproducibility) |
| A10 | invariant condition tested inside a loop | BR-04 | loop unswitching |
| A11 | reading serialized data by struct cast | SER-03 | `memcpy` + endian conversion |
| A12 | treating allocation failure as success | ALLOC-06 | propagate the error |
| A13 | `std::string` by value (hot path) | STR-01 | `string_view` |
| A14 | cancel-flag check per row | PAR-03 | batch-level checks |
| A15 | allocation/I/O under a lock | PAR-05 | prepare outside the lock |
| A16 | parsing an external format without validation | SER-02 | magic/version/range checks |
| A17 | `-ffast-math` in correctness-sensitive code | FP-03 | remove, or per-function only |
| A18 | stack array with unchecked size (VLA) | CLOW-08 | check bound or heap/arena |
| A19 | concluding from an unrepresentative microbench | MEAS-04 | re-measure at real workload size |
| A20 | spawning threads without verifying gain | PAR-06 | serial fallback below thresholds |
| A21 | repeated global-variable access in a loop | GLOB-01 | copy to a local at entry |
| A22 | hot globals declared adjacent without padding | GLOB-02 | `CACHE_ALIGNED` on each |
| A23 | `volatile` for synchronization | GLOB-04 | `std::atomic` |
| A24 | repeated `thread_local` lookup in a loop | GLOB-05 | take the pointer once, outside |
| A25 | `errno` check inside a hot loop | GLOB-06 | judge by return value; `errno` only on failure path |
| A26 | hot-path function-local `static` access | GLOB-07 | init at boot + raw pointer |
| A27 | unsharded global counter | GLOB-09 | per-core shards + sum on read |
| A28 | master thread initializing the whole buffer (NUMA) | PAR-09 | per-worker first-touch |
| A29 | `mutex` on read-mostly data | PAR-10 | seqlock, COW pointer swap, RCU |
| A30 | multithreaded allocation storm | PAR-11 | arena or thread-caching allocator |
| A31 | raw spin without `pause` | PAR-12 | insert `cpu_relax()` + adaptive spin |
| A32 | hot-path `shared_ptr` by value/copy | CPP-01 | `const T&` or `const T*` |
| A33 | `for (auto x : container)` copying | CPP-02 | `for (const auto& x : ...)` |
| A34 | `return std::move(local)` | CPP-02 | plain `return local` (RVO) |
| A35 | exceptions for normal control flow | CPP-03 | return values, `optional`, error codes |
| A36 | hot-path `dynamic_cast` | CPP-04 | kind tag + `static_cast` |
| A37 | `std::endl` in bulk output | CPP-05 | `'\n'` + explicit flush |
| A38 | move operations missing `noexcept` | CPP-03 | mark `noexcept` (prevents copy fallback on vector realloc) |
| A39 | bit reinterpretation via type cast | ALIAS-01 | `memcpy` or `union` |
| A40 | hiding the problem with `-fno-strict-aliasing` | ALIAS-01 | fix the cause, then drop the flag |
| A41 | byte-wise copy/compare loops | ALIAS-02 | `memcpy`/`memcmp`/`memset` |
| A42 | missing `restrict` on non-overlapping args | ALIAS-03 | add `restrict` (after confirming non-overlap) |
| A43 | accumulating directly into a struct field in a loop | ALIAS-04 | local accumulation, write back once |
| A44 | pointer↔integer round trips (tagged pointers) | ALIAS-05 | index-based references |
| A45 | bitfields shared across threads | COH-07 | atomic bytes or zero-width separation |
| A46 | misaligned atomics / atomics in packed structs | COH-08 | `_Alignas` + natural alignment |
| A47 | ring-buffer head/tail on one line | COH-06 | line separation + copy caching |
| A48 | arrays all starting at 4KB multiples | COH-09 | stagger by cache-line offsets |
| A49 | hand-inserted `mfence` | COH-10 | standard atomics + memory orders |
| A50 | file-internal symbol missing `static` | CLOW-01 | add `static` |
| A51 | header+data split struct | CLOW-02 | flexible array member |
| A52 | nested-`if` error handling | CLOW-04 | `goto` single exit |
| A53 | ≤16B struct passed by pointer | CLOW-05 | pass by value (registers) |
| A54 | runtime-constant division inside a loop | CLOW-06 | precompute the reciprocal |
| A55 | `int`/`size_t` mixed as indices | CLOW-07 | unify on one |
| A56 | VLA / `alloca` use | CLOW-08 | fixed array + arena fallback |
| A57 | padding waste from struct field order | CLOW-15 | descending alignment + `_Static_assert` |
| A58 | `__attribute__((packed))` for performance | CLOW-16 | explicit serialization |
| A59 | **compile-time constant re-tested in the row loop** | BR-04 | fix a specialized kernel/function pointer in the prep phase |
| A60 | **unconditional out-of-line call per row (usually a no-op)** | CC-05 | hoist the condition to the call site; call only when needed |
| A61 | **nested multi-switch dispatch inside the row loop** | BR-06 | resolve the (kind×type×operator) leaf at compile time |
| A62 | **store writing the same value every iteration (invariant pointer re-publication)** | BR-04 | once in the prep phase |
| A63 | **serial path optimized, parallel-only loop missed** | PAR-14 | enumerate consumers, apply to both |
| A64 | **thread-local heap allocation freed by another thread** | ALLOC-08 | use the global heap |
| A65 | per-row rounding/packing in fixed-point bulk summation | FP-07 | deferred carry + one materialization at finish |
| A66 | overflow detection via `volatile` + post-hoc check | CC-06 | `__builtin_*_overflow` |
| A67 | float and double mixed in one hot-loop expression (`b * 1.23` on a float) | FP-08 | unify the type; `f` suffix on literals |
| A68 | `if/else if` error-check chain inlined on the hot path | BR-08 | single `error_flags` test + `noinline` cold handler |
| A69 | expensive condition ahead of a cheap, highly-selective one in a per-row predicate | BR-07 | reorder (no side effects; comment the selectivity assumption) |
| A70 | lock- or CAS-based queue with exactly one producer | PAR-15 | pre-allocated ring + sequence + barriers (reuse the verified implementation) |
| A71 | busy-spin wait with no stated CPU budget | PAR-16 | choose spin / spin-then-yield / block explicitly and document it |
| A72 | new request–response socket with no TCP_NODELAY decision | SYS-05 | set it, or comment why Nagle is wanted |
| A73 | perf verdict from a rate alone (miss rate up/down) without counts or wall-clock | MEAS-06 | record `cache-references`, `instructions`, and the wall-clock median |
| A74 | mean-only improvement report | MEAS-07 | median + MAD/stddev |
| A75 | header `#include` for a type used only by pointer/reference | PHYS-01 | forward declaration; include in the `.cpp` |
| A76 | relying on a transitive include | PHYS-01 | include directly |
| A77 | cyclic include patched with `#ifdef` / forward-declaration soup | PHYS-02, PHYS-03 | escalate or demote the contact point |
| A78 | recursive destructor freeing sibling nodes (`~Link() { delete next; }`) | PHYS-03 | manager class owns lifetime; stack-overflow hazard |
| A79 | abstract interface (virtual) inserted on the row loop for insulation/mocking | PHYS-05, CPP-09 | template binding on the hot path; protocol on the cold path |
| A80 | **perf delta attributed to the source change with no hot-symbol address/phase diff** (two DSOs compared as if one function changed) | MEAS-08, CC-08 | Appendix E gate; padding control before the verdict |
| A81 | **"not on the call path, so it cannot affect performance"** used to wave through a size-changing edit (template/EH/`noexcept`/`std::function` in cold or shared-header code) | CC-08 | direct cost ≠ layout cost; run the ELF gate on the perf-gated workload |
| A82 | **blanket `-falign-functions=32` / `aligned(32)` on every function** as the product fix | CC-09 | narrow `aligned(32)` on proven hot functions → PGO; global alignment is a diagnosis flag |
| A83 | **layout verdict from one PMU counter** ("DSB miss +50 % is the cause") or from a compiler-upgrade "it went away" | MEAS-08, CC-08 | full Top-down L1/L2 both variants; padding control; a new compiler is a different ELF, not a fix |
