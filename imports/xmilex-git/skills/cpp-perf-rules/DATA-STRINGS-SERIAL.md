# §12 Data Structures (DS)

## DS-01 — Default choice is the contiguous array

| Need | Choose | Avoid |
|---|---|---|
| Sequential traversal | `std::vector` | `std::list`, `std::map` |
| Key lookup (no order needed) | open-addressing hash | `std::unordered_map` (chaining) |
| Range queries | sorted vector + binary search | `std::map` |
| Priority | `std::priority_queue` (vector-based) | sorted list |
| Few fixed elements | `std::array` / stack array | dynamic allocation |
| Insertion at both ends | `std::deque` | `std::list` |
| Frequent middle insertion | index free list on array | `std::list` |

## DS-02 — Know the limits of standard hash containers

`std::unordered_map` is specified as per-bucket node chaining — a pointer chase per lookup. On performance paths use an open-addressing (flat hash) implementation. Key points if implementing yourself:

```
- open addressing + linear probing (high rate of resolution within a cache line)
- keep load factor ≤ 0.75
- power-of-two size → modulo becomes a mask (& (cap-1))
- splitting keys/values SoA-style lets probing read only keys — better cache efficiency
- deletion via tombstones or backward shift
```

## DS-03 — Judge hash functions on distribution AND speed

Integers: multiply-shift family (splitmix64/mix64). Byte strings: xxHash/wyhash family. Cryptographic hashes (SHA etc.) are forbidden on performance paths.

**Important:** if the same data is processed by multiple paths (serial/parallel), **the hash function must be unified** or merging is lossy. Pin the function explicitly per type:

```c
/* unified per-type hashing — merge-safe even when paths mix */
static inline uint64_t h_int(int64_t v)     { return mix64((uint64_t)v); }
static inline uint64_t h_double(double v)   { uint64_t b; memcpy(&b,&v,8); return mix64(b); }
static inline uint64_t h_bytes(const void *p, size_t n) { return wyhash(p, n, SEED); }
```

## DS-04 — Trust standard sorts; verify the comparator

`std::sort` is generally a well-optimized introsort. Rather than reimplementing, focus on making the **comparator light and correct** (see FP-02). A non-inlined comparator hurts badly — use lambdas/function objects, avoid function pointers.

## DS-05 — Actively consider approximate data structures

Where exact answers are unnecessary, they save large amounts of memory and time.

| Goal | Structure | Properties |
|---|---|---|
| Distinct count | HyperLogLog | fixed memory (e.g. 16 KB), relative error ≈ 1.04/√m |
| Membership | Bloom / Cuckoo filter | false positives only, no false negatives |
| Top-frequency | Count-Min Sketch | overestimation bias |
| Quantiles | t-digest / KLL | streaming |
| Uniform sample | Reservoir sampling (Algorithm L) | one pass, fixed memory |

**Mergeability rule:** if parallel/partitioned results will be combined, pick a structure whose merge is mathematically identical to single-pass processing.
- HLL: per-register max → identical to a single sketch (lossless)
- Reservoir: needs population-size-proportional stratified merge (naive union is biased)
- Count-Min: per-counter sum → identical

---

# §13 Strings (STR)

## STR-01 — Use allocation-free views on hot paths

```cpp
std::string_view s = ...;      /* no copy, no allocation */
if (s == other) { ... }
```

APIs passing `std::string` by value are hot-path review flags. Beyond SSO range (15–22 chars depending on implementation) they heap-allocate.

## STR-02 — Apply normalization consistently to both sides before comparing

Trailing-space handling of fixed-width char types (CHAR), case, and collation must be applied **identically to both sides** to preserve self-consistency.

```c
static inline size_t rtrim_len(const char *s, size_t n) {
    while (n > 0 && s[n-1] == ' ') n--;
    return n;
}
/* rtrim both sides, then compare */
```

**Caution:** raw byte comparison is fast but ignores collation. If both sides are consistent, self-consistency holds, but it may differ from user expectations — document it.

## STR-03 — Store as string pool + offset

Inlining strings as arrays in a struct bloats it and hurts cache efficiency.

```c
struct rec { uint64_t key; uint32_t name_off; uint32_t name_len; };  /* 16 B */
char *string_pool;   /* separate contiguous buffer */
```

## STR-04 — Compare lengths first for short strings
Different lengths → immediate mismatch. Filter by length before calling `memcmp`.

---

# §14 Serialization & Layout (SER)

## SER-01 — Design offset-based formats

Never serialize pointers. Storing offsets from a base makes relocation and mmap possible.

```
[header: magic | version | count | ... ]
[fixed-size entry array: ..., offset_to_var, len, ... ]
[variable-length region: strings/blobs ]
```

## SER-02 — Validate magic, version, framing

Reading corrupted or older-version data must fail clearly, not silently misbehave.

```c
if (memcmp(hdr->magic, MAGIC, 4) != 0)        goto invalid;
if (hdr->version > CURRENT_VERSION)           goto invalid;
if (hdr->count > MAX_COUNT)                   goto invalid;
/* always check that variable-region slots stay inside the buffer */
if (slot.offset + slot.len > total_size)      goto invalid;
```

On validation failure, **fall back to default estimates** rather than crashing.

## SER-03 — Use endian-safe accessors

Never read by casting a struct over the bytes — that causes alignment violations (UB) and endianness problems at once.

```c
static inline uint64_t get_u64(const char *p) {
    uint64_t v; memcpy(&v, p, 8); return le64toh(v);
}
```

`memcpy` is optimized to a single load by the compiler — zero cost.

## SER-04 — Fix header size and keep reserved fields
Reserved bytes let you add fields later without bumping the format version.
