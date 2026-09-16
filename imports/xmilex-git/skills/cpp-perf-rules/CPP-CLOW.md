# §10 C++ Language Feature Costs (CPP)

Most C++ abstraction is free, but **reference counting, type lookup, and exceptions** are the exceptions — costs spike especially under multithreading.

## CPP-01 — Never copy `shared_ptr` on hot paths (the biggest multithread trap)

Every copy/destroy performs an **atomic refcount inc/dec**. When multiple threads copy `shared_ptr`s to **the same object**, the cache line holding the counter ping-pongs between cores — **effectively the cost of a lock.** The problem: logically it's read-only sharing, but performance-wise it's write contention.

```cpp
/* BAD: two atomic ops per call + line contention */
void process(std::shared_ptr<Node> n);
for (auto& n : nodes) process(n);

/* BAD: copy in range-for */
for (std::shared_ptr<Node> n : nodes) { ... }

/* GOOD: if not transferring ownership, use a reference or raw pointer */
void process(const Node& n);
void observe(const Node* n);
for (const auto& n : nodes) process(*n);
```

**Rules:**
- Function **shares** ownership → `shared_ptr` (by value)
- Function **transfers** ownership → `unique_ptr` (by value) or `shared_ptr&&`
- Function **only observes** → `const T&` or `const T*` ← the majority case
- Container iteration is always `const auto&`

`weak_ptr::lock()` also involves atomics — don't call it repeatedly inside a loop.

## CPP-02 — Eliminate unnecessary copies

```cpp
/* Arguments: large objects by const& */
void f(const std::vector<row>& v);          /* not std::vector<row> v */

/* Returns: return by value and trust RVO. Wrapping in std::move defeats RVO */
std::vector<row> build() {
    std::vector<row> v;  ...;
    return v;                                /* not return std::move(v); */
}

/* Loops: auto copies */
for (const auto& r : rows) { ... }           /* not for (auto r : rows) */

/* Insertion: emplace avoids the temporary */
v.emplace_back(a, b, c);                     /* not v.push_back(T(a,b,c)); */

/* Map lookup: remove the duplicate search */
if (auto it = m.find(k); it != m.end()) use(it->second);   /* not count()+[] */
```

## CPP-03 — Exceptions only for exceptional situations

Nearly free when not thrown (zero-cost model), but **throwing costs microseconds.** Never use for normal control flow (e.g. "not found").

```cpp
void scan_page(page_t*) noexcept;   /* noexcept elides cleanup-code generation */
```

`noexcept` matters especially on move operations: during `std::vector` reallocation, non-`noexcept` moves fall back to copies.

## CPP-04 — Avoid `dynamic_cast` and RTTI on hot paths

Requires a type-hierarchy walk: 50–500 cycles. Replace with a tag field + `static_cast`.

```cpp
/* BAD */
if (auto *p = dynamic_cast<HeapScan*>(node)) { ... }

/* GOOD: own kind tag */
enum node_kind { NK_HEAP_SCAN, NK_INDEX_SCAN, ... };
if (node->kind == NK_HEAP_SCAN) { auto *p = static_cast<HeapScan*>(node); ... }
```

## CPP-05 — No iostream for bulk I/O

Locale/format handling overhead is large. Bulk processing uses `read`/`write` or `fread`/`fwrite`.

```cpp
std::ios::sync_with_stdio(false);   /* detach from C stdio */
out << data << '\n';                /* std::endl flushes every time → never */
```

## CPP-06 — Avoid virtual/multiple inheritance on performance paths
They introduce pointer adjustment (thunks) and extra indirection. Keep inheritance hierarchies shallow.

## CPP-07 — Fix at compile time whatever can be fixed

```cpp
constexpr size_t BUCKETS = 1024;                 /* removes runtime loads */

template<bool Parallel>                          /* removes runtime branches */
void collect(...) {
    if constexpr (Parallel) { ... } else { ... }
}
```

`if constexpr` excludes the untaken branch from compilation entirely — shrinking code size and branches at once.

## CPP-08 — Know container destruction cost

Millions of destructor-bearing objects cost O(n) just to destroy. Trivially destructible types + arena make destruction effectively free.

```cpp
static_assert(std::is_trivially_destructible_v<sample_t>);   /* state the guarantee */
```

## CPP-09 — A repeatedly-called virtual function can become compile-time dispatch

The vtable lookup + indirect call costs less than the fact that it **blocks inlining and constant propagation**. If the call target is known at compile time, move to templates/CRTP/overloading. Measured 2.60ns → 1.92ns (~26%, paper note §2.2).

- **Precondition**: only when runtime polymorphism is genuinely not needed. If the type is decided at runtime, this does not apply.
- ⚠ **Never propose this in `.c` files** (usage protocol). Much of CUBRID's hot path is C — there, BR-06 (hoist the function-pointer table out of the loop) is the counterpart.
- Templating causes **code bloat → I-cache pressure**. It pays only with a small number of instantiations (same trade-off as CPP-03 and CC-02).
- ⚠ Conflicts with PHYS-05 (insulation via abstract interfaces). On a hot path the performance rule wins; keep protocols for cold/init paths.

---

# §11 C Low-Level Techniques (CLOW)

Techniques used in C itself, without modern C++ abstraction. Non-portable items are flagged.

## CLOW-01 — Mark file-internal symbols `static`

The compiler then knows all call sites — enabling inlining, constant propagation, dead-code removal, register-passing optimization. Also bypasses GOT/PLT indirection (GLOB-08).

```c
static int  compute_ndv(const col_t *c);   /* always static if no external exposure needed */
static uint64_t g_local_counter;
```

**Rule:** every function/variable that needn't be declared in a header is `static`. The best gain-per-effort mechanical improvement there is.

## CLOW-02 — Remove double indirection with flexible array members

```c
/* BAD: header and data separated → one pointer chase + two allocations */
struct blob { size_t len; char *data; };

/* GOOD: single allocation, contiguous, cache-friendly */
struct blob { size_t len; char data[]; };            /* C99 */
struct blob *b = malloc(sizeof *b + len);
b->len = len;
```

Fits histogram bucket arrays, variable-length records, serialization buffers.

## CLOW-03 — Use computed goto in interpreter dispatch loops (GCC/Clang extension)

`switch` dispatch is **one indirect branch**, so the predictor learns only one history. Computed goto spreads branch sites per instruction, greatly raising prediction accuracy. 10–30% improvements are reported for expression evaluators / VM loops.

```c
#if defined(__GNUC__)
  #define DISPATCH_TABLE  static void *tbl[] = { &&OP_ADD, &&OP_SUB, &&OP_LOAD, &&OP_HALT }
  #define NEXT()          goto *tbl[(++pc)->op]
  #define OP(name)        OP_##name:
#else
  /* portable fallback: switch loop */
#endif

int eval(insn_t *pc) {
    DISPATCH_TABLE;
    NEXT();
    OP(ADD)  { st[sp-1] += st[sp]; sp--; NEXT(); }
    OP(SUB)  { st[sp-1] -= st[sp]; sp--; NEXT(); }
    OP(LOAD) { st[++sp] = mem[pc->arg];  NEXT(); }
    OP(HALT) return st[sp];
}
```

**Caution:** non-portable — always keep the `switch` fallback. Larger code can pressure the I-cache, so measurement is mandatory.

## CLOW-04 — Single-exit error handling with `goto`

Removing nested `if`s and gathering cleanup in one place makes **the hot path a branch-free straight line**, improving I-cache efficiency and branch prediction.

```c
int collect(ctx_t *ctx) {
    int r = ER_FAILED;
    void *res = NULL, *sk = NULL;

    if (!(res = reservoir_create(cap)))     goto exit;
    if (!(sk  = hll_create(P)))             goto exit;
    if (scan_heap(ctx, res, sk) != NO_ERROR) goto exit;
    /* hot path: straight-line, branch-free up to here */
    r = NO_ERROR;
exit:
    hll_destroy(sk);
    reservoir_destroy(res);
    return r;
}
```

**Rule:** clean up in reverse order of creation. `free(NULL)` is safe, so initializing to `NULL` yields branch-free cleanup.

## CLOW-05 — Pass small structs by value

In the x86-64 SysV ABI, **structs ≤ 16 bytes are passed in registers.** Passing by pointer instead causes memory round trips and aliasing assumptions.

```c
typedef struct { int64_t lo, hi; } range_t;   /* 16 B */
bool in_range(range_t r, int64_t v);          /* GOOD: two registers */
bool in_range(const range_t *r, int64_t v);   /* BAD: via memory + aliasing */
```

At 17+ bytes they go through memory anyway — from there a pointer is better.

## CLOW-06 — Replace runtime-constant division with multiply-shift

Compile-time-constant division is substituted automatically. A **divisor fixed at runtime and reused repeatedly** deserves a hand-prepared reciprocal.

```c
/* power of two: substitute immediately */
if (IS_POW2(d)) { q = x >> __builtin_ctzll(d); r = x & (d - 1); }

/* general divisor: compute the magic number once, reuse (libdivide technique) */
typedef struct { uint64_t magic; uint8_t more; } divisor_t;
divisor_t dv = divisor_init(d);        /* once, outside the loop */
for (i = 0; i < n; i++) out[i] = divide_by(in[i], &dv);   /* mulhi + shift */
```

Division 20–100 cycles → multiply+shift 5–8. Big wins where the same divisor is used millions of times, e.g. histogram bucket computation.

## CLOW-07 — Use one index type consistently

Mixing `int` and `size_t` inserts a sign-extension (`movsxd`) per iteration, and the differing signed-overflow-UB assumptions change vectorizability.

```c
/* BAD: i is int, array indexing is size_t → sign extension per iteration */
for (int i = 0; i < (int)n; i++) sum += buf[i];

/* GOOD: pick one */
for (size_t i = 0; i < n; i++) sum += buf[i];
```

## CLOW-08 — No VLAs or `alloca`

When size isn't compile-time known, frame-pointer adjustment code appears, stack overflow becomes a risk (attack surface), and optimization worsens. Small upper bound → fixed array; large → arena.

```c
/* BAD */
void f(size_t n) { char buf[n]; ... }

/* GOOD */
void f(size_t n) {
    char stack_buf[256];
    char *buf = (n <= sizeof stack_buf) ? stack_buf : arena_alloc(a, n, 1);
    ...
}
```

## CLOW-09 — Keep `setjmp`/`longjmp` off performance paths

A function containing `setjmp` must keep locals in memory — **register allocation degrades for the whole function.** Handle errors with `goto` (CLOW-04) and return codes.

## CLOW-10 — Separate hot/cold code into sections

```c
__attribute__((hot))  void scan_page(page_t *p);
__attribute__((cold)) __attribute__((noinline)) void report_corruption(...);
__attribute__((section(".text.unlikely"))) static void slow_path(void);
```

Contiguous hot functions reduce I-cache and I-TLB misses. PGO (CC-02) does this automatically; where PGO isn't available, explicit annotation works.

## CLOW-11 — Use aligned-allocation APIs

```c
void *p = aligned_alloc(64, ALIGN_UP(size, 64));   /* C11: size must be a multiple of align */
if (posix_memalign(&p, 64, size) != 0) goto oom;   /* POSIX */
```

Needed for cache-line isolation (COH-03), aligned SIMD loads (ALIAS-06), and DMA. `malloc` usually guarantees only 16-byte alignment.

## CLOW-12 — Make fixed-size copies use constant sizes

```c
memcpy(dst, src, 16);   /* constant → inlined as two 8-byte moves; no call */
memcpy(dst, src, n);    /* variable → libc call */
```

If record sizes are limited to a few values, constant-izing via a size switch pays:

```c
switch (rec_size) {
    case 8:  memcpy(d, s, 8);  break;
    case 16: memcpy(d, s, 16); break;
    default: memcpy(d, s, rec_size);
}
```

## CLOW-13 — Exploit bit-operation builtins

```c
__builtin_popcountll(x)    /* count of 1-bits. POPCNT */
__builtin_clzll(x)         /* leading zeros. LZCNT — used directly in HLL rank */
__builtin_ctzll(x)         /* trailing zeros. TZCNT */
__builtin_bswap64(x)       /* endian swap */
```

**Caution:** `clz`/`ctz` are undefined at `x == 0`. Always pre-check or add a correction like `x | 1`.

```c
/* HyperLogLog rank: leading zeros + 1 */
static inline int hll_rank(uint64_t w, int p) {
    uint64_t rest = w << p;                  /* strip the top p bits */
    return rest ? __builtin_clzll(rest) + 1 : (64 - p + 1);
}
```

## CLOW-14 — Prefer switch over function-pointer tables

A function-pointer call causes an indirect branch + no inlining + worse aliasing assumptions, all at once. With a small fixed set of kinds, dispatch statically via `switch`; use tables only when kinds are many and extensibility is needed (BR-06).

> **But if the dispatch is re-decided inside the execution loop every time, it's the opposite.** An interpreter that walks a several-hundred-case switch tree recursively per row is far better served by resolving a function pointer during preparation (BR-04 extended application). The criterion: "is the dispatch decision inside the loop?"

## CLOW-15 — Remove padding via struct field order

```c
/* BAD: 24 B (7 + 3 padding) */
struct s { char  a; uint64_t b; int c; };

/* GOOD: 16 B */
struct s { uint64_t b; int c; char a; };

/* leave the verification in the code */
_Static_assert(sizeof(struct s) == 16, "unexpected padding");
```

Declare in descending alignment order. The `pahole` tool finds padding holes in existing structs:

```bash
pahole -C entry ./binary      # show struct layout and holes
```

## CLOW-16 — Use `__attribute__((packed))` with caution

It removes padding and saves memory, but **causes unaligned accesses — slower, and trapping on some architectures.** Putting atomics inside is a COH-08 violation. For disk/network formats, use **explicit serialization** (SER-03) instead of packed structs.
