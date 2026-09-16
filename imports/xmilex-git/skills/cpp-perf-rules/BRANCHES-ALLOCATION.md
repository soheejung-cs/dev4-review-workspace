# §5 Branches (BR)

## BR-01 — Leave predictable branches alone
Branch predictors exceed 99% accuracy on regular patterns — effectively free. **Apply branchless transformations only when measured misprediction rate is high.**

## BR-02 — Mark cold paths with hints

```c
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

if (UNLIKELY(ptr == NULL)) goto error;      // errors almost never happen
if (LIKELY(cache_hit)) return cached_value;
```

C++20 and later: `if (x) [[likely]] { ... }`

The effect is not branch prediction but **code placement**: hot paths become contiguous, improving I-cache efficiency.

## BR-03 — Replace data-dependent branches with arithmetic

```c
// BAD: ~50% misprediction on random data
for (i = 0; i < n; i++) if (v[i] > t) count++;

// GOOD: branch removed
for (i = 0; i < n; i++) count += (v[i] > t);

// Conditional select via arithmetic too
// BAD
int m = (a > b) ? a : b;
// GOOD (the compiler usually emits cmov, but it can be made explicit)
int m = a ^ ((a ^ b) & -(a < b));   // hurts readability — only with measured gain
```

**Priority:** first check whether the compiler emits `cmov` (inspect the assembly). If it does, manual bit tricks are unnecessary.

## BR-04 — Hoist invariant branches out of loops

```c
// BAD: condition tested every iteration
for (i = 0; i < n; i++) {
    if (mode == MODE_A) do_a(v[i]);
    else                do_b(v[i]);
}

// GOOD: loop unswitching
if (mode == MODE_A) for (i = 0; i < n; i++) do_a(v[i]);
else                for (i = 0; i < n; i++) do_b(v[i]);
```

This duplicates code — manage with templates or macros. Compilers sometimes do it automatically, but it is not guaranteed.

> **Extended application (execution engines / interpreters):** re-testing a **value already fixed at compile time** inside the execution loop is the same violation. Operator kind, operand types, result domain, flags, and field indices are fixed when the program is built, so **select a specialized function (kernel) up front and only call it in the loop.** Likewise, a **store that writes the same value every iteration** (invariant pointer re-publication, etc.) belongs in the preparation phase.

## BR-05 — Make switches dense integer cases
Contiguous, dense case values let the compiler emit a jump table; sparse values become a compare chain. Assign enum values consecutively from 0.

## BR-06 — Remove virtual calls from hot loops
Indirect calls mean misprediction + no inlining + blocked optimization.

```cpp
// BAD: virtual dispatch per row
for (auto& row : rows) processor->handle(row);

// GOOD 1: split the loop per type (dispatch moves outside the loop)
switch (processor->kind()) {
  case KIND_A: for (auto& r : rows) static_cast<A*>(processor)->handle(r); break;
  case KIND_B: for (auto& r : rows) static_cast<B*>(processor)->handle(r); break;
}

// GOOD 2: fix at compile time via template
template<typename Proc> void run(Proc& p, span<row> rows) {
    for (auto& r : rows) p.handle(r);   // inlinable
}
```

> **Trade-off note:** function-pointer dispatch is not always bad. Calling a once-resolved function pointer is far cheaper than walking a several-hundred-case switch tree recursively per row. What's bad is **re-deciding the dispatch inside the loop every time.** Especially when nested like `switch(kind)` → `switch(type)` → `switch(operator)`, pre-resolve the (kind×type×operator) leaf.

## BR-07 — Condition order is cost (exploit short-circuit evaluation)

`&&`/`||` short-circuit. Put the **cheap, frequently-filtering condition first** and the expensive one is often never executed. Measured ~50% across the board (paper note §2.6).

```c
/* BAD — expensive check first */
if (expensive_type_check (v) && flag_is_set (v)) ...
/* GOOD — cheap flag rejects most rows, then the expensive check */
if (flag_is_set (v) && expensive_type_check (v)) ...
```

- Biggest effect on **per-row conditions** such as predicate evaluation and scan filters. Move the low-selectivity (= most rejecting) condition to the front.
- **Never reorder conditions with side effects.** If short-circuiting removes a call, semantics change.
- When you reorder, **leave the selectivity assumption in a comment.** Data changes can invert it.

## BR-08 — Fold error checks into one flag; move handling to a cold function

An `if/else if` chain of error checks on the hot path (1) burns branch-predictor slots and (2) inlines handling code that **pollutes the I-cache**. Remove both at once.

```c
/* BAD — a branch per check + handling code inlined on the hot path */
if (check_a (p)) { handle_a (p); return; }
else if (check_b (p)) { handle_b (p); return; }
else do_work (p);

/* GOOD — one branch, handling out of line */
if (LIKELY (!p->error_flags)) do_work (p);
else handle_error (p);          /* __attribute__((noinline)), cold */
```

Measured: if-else chain 7.35ns → flag 4.68ns (fewer branches, ~36%); slow-path split ~12% (paper note §2.7, §2.8). Use the two together — one is the branch-prediction view, the other the I-cache view.

- Mark the error handler `__attribute__((noinline))` explicitly; otherwise the compiler inlines it and the point is lost.
- **Do not apply to cold paths.** Rewriting init/shutdown/error paths this way is pointless (usage protocol: hot/cold split).
- MEAS-03's `stalled-cycles-frontend > 20%` is the signal for this problem.

---

# §6 Allocation (ALLOC)

## ALLOC-01 — Never allocate in a hot loop
`malloc`/`new` costs 50–200 cycles + lock contention + cache pollution. Hoist out of the loop or reuse.

## ALLOC-02 — Always pre-reserve containers

```cpp
std::vector<sample> v;
v.reserve(expected_n);     // removes reallocation, copies, and moves entirely
```

A `push_back` loop without `reserve` is an automatic review flag.

## ALLOC-03 — Use an arena for bulk-freeable work

```c
typedef struct { char *base; size_t used, cap; } arena_t;

static inline void *arena_alloc(arena_t *a, size_t sz, size_t align) {
    size_t off = (a->used + (align - 1)) & ~(align - 1);
    if (UNLIKELY(off + sz > a->cap)) return NULL;   /* or add a chunk */
    a->used = off + sz;
    return a->base + off;
}
static inline void arena_reset(arena_t *a) { a->used = 0; }   /* O(1) free-all */
```

**Good fit:** per-request temporaries, parse intermediates, scan-time collection buffers, plan nodes.
**Bad fit:** objects with individually differing lifetimes.

## ALLOC-04 — Use a pool allocator for fixed-size objects
An index-based free list on top of an array gives O(1) alloc/free without pointer chasing.

```c
typedef struct { uint32_t next_free; /* payload */ } slot_t;
/* manage only a free_head index; nodes live in contiguous memory — cache-friendly */
```

## ALLOC-05 — Handle small-and-many with SBO
When most objects are small and rare ones are big, handle the small case with a stack/inline buffer.

```cpp
template<typename T, size_t N>
class small_vector {
    alignas(T) unsigned char buf_[N * sizeof(T)];
    T*     data_ = reinterpret_cast<T*>(buf_);
    size_t size_ = 0, cap_ = N;
    bool   heap_ = false;
public:
    void push_back(const T& v) {
        if (UNLIKELY(size_ == cap_)) grow();   // heap transition only here
        new (data_ + size_++) T(v);
    }
    ~small_vector() { /* run destructors, then free if heap_ */ }
};
```

## ALLOC-06 — Never swallow allocation failure silently
A correctness issue independent of performance: treating `nothrow new` or `realloc` failure as success silently drops data.

```c
void *p = db_private_realloc(old, new_sz);
if (UNLIKELY(p == NULL)) {
    er_set(ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, new_sz);
    return ER_FAILED;      /* never ignore */
}
```

## ALLOC-07 — Know container destruction cost
A container holding millions of destructor-bearing objects is expensive just to destroy. Designing with arenas + trivially destructible types makes destruction free.

## ALLOC-08 — Memory whose ownership crosses threads goes on the global heap
Freeing a thread-local (private) heap allocation from another thread can abort the allocator.
For worker pools / parallel scans, where the creating and freeing threads may differ, allocate on the global heap from the start.
Document **"which thread frees this"** in the data structure's comments.
