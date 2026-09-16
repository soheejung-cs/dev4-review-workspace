# §4 Aliasing & Type Rules (ALIAS) — the biggest optimization lever in C

Whether the compiler can keep a value in a register hinges on **"can these two pointers overlap?"**
If it must assume they can, every store forces all loads to be redone — killing loop optimization and vectorization entirely.
**In C, the programmer must supply this information via types and `restrict`.**

## ALIAS-01 — Obey strict aliasing (correctness AND performance at stake)

The C standard makes accessing an object through an lvalue of an incompatible type UB. The compiler uses this to assume **"pointers of different types don't overlap" (TBAA)** and keeps loads in registers.

```c
/* BAD: UB. Results change with optimization level */
float f = 1.0f;
uint32_t bits = *(uint32_t *)&f;

/* GOOD 1: memcpy — compiler lowers it to a single load; zero runtime cost */
uint32_t bits;
memcpy(&bits, &f, sizeof bits);

/* GOOD 2: union — legal type punning in C (NOT in C++) */
union f32_bits { float f; uint32_t u; };
union f32_bits cvt = { .f = f };
uint32_t bits = cvt.u;
```

> **Do not paper over it with `-fno-strict-aliasing`.** That flag makes the whole compiler conservative,
> **degrading optimization quality of every loop.** The Linux kernel uses it for historical reasons;
> new code should express intent with `memcpy`/`union`.
> If the codebase already carries this flag, attempting its removal may be a free performance win.

## ALIAS-02 — `char*`-family aliases everything

By the standard, access through `char*`, `unsigned char*`, `void*` **may overlap any type.** So byte-wise loops prevent the compiler from ruling out aliasing — vectorization and register retention suffer.

```c
/* BAD: byte loop — dst and src assumed possibly overlapping */
for (size_t i = 0; i < n; i++) dst[i] = src[i];

/* GOOD: express intent via standard functions → compiler builtins, SIMD */
memcpy(dst, src, n);     /* promises no overlap */
memmove(dst, src, n);    /* may overlap */
```

**Rule:** never hand-write byte copy/compare/fill loops — use `memcpy`/`memmove`/`memcmp`/`memset`. They lower to compiler builtins faster than any hand-written loop.

## ALIAS-03 — State non-overlap with `restrict`

```c
/* Without this promise, writing out[i] forces reloads of a/b — no vectorization */
void merge_add(double * restrict out,
               const double * restrict a,
               const double * restrict b, size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = a[i] + b[i];
}
```

Also usable at block scope:

```c
{
    row_t * restrict p = base + off;   /* promise: sole access path within this block */
    for (...) p[i].flags |= F;
}
```

**Caution:** if they actually overlap, it is **UB and silently produces wrong results.** If any path may overlap, don't add it. The safe alternative is a runtime overlap check with two paths:

```c
if (dst + n <= src || src + n <= dst) fast_path_restrict(dst, src, n);
else                                  safe_path(dst, src, n);
```

## ALIAS-04 — Break same-type-pointer aliasing with locals

Two pointers of the same type may overlap, so a write through one invalidates loads through the other. This is the struct-field version of GLOB-01.

```c
/* BAD: each write to dst->sum may force reload of src->vals (dst could equal src) */
void accumulate(stats_t *dst, const stats_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst->sum += src->vals[i];
}

/* GOOD: accumulate in a local → register-resident + vectorizable */
void accumulate(stats_t *dst, const stats_t *src, size_t n) {
    uint64_t sum = dst->sum;
    for (size_t i = 0; i < n; i++) sum += src->vals[i];
    dst->sum = sum;
}
```

**General rule:** a value updated inside a loop must be **accumulated in a local variable and written back once after the loop** — whether the target is a global, a struct field, or a pointer dereference.

## ALIAS-05 — Don't round-trip pointers through integers (provenance)

Converting a pointer to an integer and back makes the compiler lose provenance tracking. Optimization becomes conservative — or, worse, makes wrong assumptions.

```c
/* BAD: manipulating tagged pointers with integer arithmetic */
uintptr_t tagged = (uintptr_t)p | TAG;
node_t *q = (node_t *)(tagged & ~TAG_MASK);   /* provenance lost */

/* GOOD: keep the original pointer separately, or design index-based */
struct tagged_ref { uint32_t index; uint32_t tag; };   /* array index + tag */
node_t *q = &pool[ref.index];
```

Index-based designs have no provenance problem and, being smaller than pointers, are also more cache-efficient.

## ALIAS-06 — Tell the compiler about alignment

```c
/* First guarantee the actual alignment */
double *buf = aligned_alloc(64, n * sizeof(double));
/* Then inform the compiler so it emits aligned SIMD instructions */
double *p = __builtin_assume_aligned(buf, 64);
for (size_t i = 0; i < n; i++) p[i] *= 2.0;
```

UB if false. Always pair with aligned allocation (CLOW-11).

## ALIAS-07 — Check aliasing first when vectorization fails

```bash
gcc   -O3 -fopt-info-vec-missed foo.c 2>&1 | grep -i alias
clang -O3 -Rpass-analysis=loop-vectorize foo.c 2>&1 | grep -i alias
```

Messages like "cannot prove pointers are not aliased" → fix with `restrict` or local-variable accumulation. This is the most common cause of vectorization failure.

## ALIAS-08 — `const` is not aliasing information

`const T *` only means "I won't write through this pointer" — **no guarantee that another pointer won't modify the object.** It does not help optimization. To promise non-overlap you need `restrict`.

```c
/* const alone is insufficient — the compiler assumes out may point into in */
void f(int *out, const int *in, size_t n);
/* this is what enables optimization */
void f(int * restrict out, const int * restrict in, size_t n);
```
