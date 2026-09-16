# §20 Physical Design & Build Cost (PHYS)

> This is the **build / dependency cost** axis, not runtime. Source: John Lakos, *Large-Scale C++ Vol. I: Process and Architecture* (2019), via the Korean digest `notes/대규모Cpp-물리설계-Lakos.md`; PHYS-01..07 mirror §20 of the upstream 성능규칙집 note, PHYS-08..11 are skill-local extensions drawn from the same digest.
> **Open this chapter for refactoring, module-split, header-cleanup, and "why does one header change rebuild everything" reviews.** Its purpose differs from the performance chapters (§0–§19).
> **Conflict rule:** when a PHYS rule and a performance rule collide on a **hot path, the performance rule wins** (PHYS-05, CPP-09). On cold/init/shutdown paths, PHYS wins.

Vocabulary (use it in PR descriptions — reviewers in the C++ community recognize it): *escalate*, *demote*, *levelize*, *insulate*, *primitive*, *transitive include*, *CCD*.

| Term | Meaning |
|---|---|
| **Component** | `.h` + `.cpp` with the same root name (+ its test driver). The atomic unit of physical design — **not the class.** |
| **Depends-On** | y needs x to compile or link. **Transitive.** |
| **Level number** | leaf = 1; level N depends on something at N−1 and nothing ≥ N. Only assignable when there are **no cycles** ("levelizable"). |
| **Encapsulated** | implementation change → clients need not **edit code** |
| **Insulated** | implementation change → clients need not even **recompile** (the physical counterpart; strictly stronger) |
| **Primitive** | an operation that needs private access to the type to be implemented efficiently |
| **Protocol** | pure abstract interface: only pure virtuals + a non-inline virtual destructor, no data members |
| **CCD** | Cumulative Component Dependency — Σ over components of (size of its link set incl. itself). Tracks the total link cost of testing every component independently. |

## PHYS-01 — A header's `#include` propagates build cost (insulation)

**Encapsulation** (clients need not change code) and **insulation** (clients need not even recompile) are different things. An `#include` in a header spreads that dependency **transitively** to every downstream client.

- A header includes **only what the header itself needs to compile.** Push everything else into the `.cpp`.
- A header legitimately needs another header in exactly 5 cases: **Is-A** (public inheritance) / **Has-A** (by-value data member) / **Inline** (used in an inline body) / **Enum** / **Typedef**. Otherwise a **forward declaration** suffices.
- **Never rely on transitive includes.** If another header happens to include what you use, that breaks silently when it is refactored. Use it directly → include it directly. (Sole exception: the public base class of a type you include directly — that dependency is intrinsic.)
- The payoff is more than "faster builds": **how fast you can ship a hotfix is proportional to how insulated the implementation details are.** If the interface is unchanged, only that `.o` needs to be swapped in.

## PHYS-02 — No cyclic dependency at any level

Cycles are forbidden between files, modules, and libraries alike. With a cycle you **cannot assign level numbers**, and at that moment "what can be tested first" disappears.

- When you find a cycle, do not paper over it (`#ifdef`, forward-declaration soup) — **remove** it with the PHYS-03 techniques.
- Cycles that only work because of link order are especially dangerous — they explode when the platform changes.
- Package-level acyclicity is **stronger** than component-level: an acyclic set of components can still form a package cycle if assigned to packages carelessly.

## PHYS-03 — Standard techniques for breaking cycles (nine, by frequency)

| Technique | Essence | CUBRID-shaped example |
|---|---|---|
| **Escalation** (move up) | lift the mutually-dependent functionality into a **new higher-level module**, turning mutual into downward dependency | storage↔query referencing each other → the contact point goes to a higher util. Factory methods on an abstract base → higher `*util` (otherwise base↔derived and all derived classes cycle) |
| **Demotion** (move down) | push the functionality both sides share **down** to a lower level | split the common info struct into a lower header (`Event` holding its whole parent → `CommonEventInfo` demoted) |
| **Opaque pointer** | hold a pointer to a type **by name only** (forward declaration, Uses-In-Name-Only) | child dereferencing its parent → cycle. Provide **both** const and non-const accessors or you create a const hole |
| **Dumb data** | **integer index** instead of a pointer; interpretation belongs to the higher manager | becomes serializable — fits page/slot structures. Removes even the forward declaration |
| **Redundancy** | deliberately duplicate a tiny piece of code to avoid a heavy dependency | **last resort.** Never when the copies must move in lock-step. A light module needing a heavy one → duplicate; heavy needing light → depend |
| **Callbacks** | client supplies a function/functor/protocol/concept so a lower layer acts in a higher context | data / function / functor / protocol / concept callbacks. Runtime (abstract base) or compile-time (member template) binding |
| **Manager class** | concentrate ownership/lifetime in a **separate manager type** | nodes must not `delete` other nodes |
| **Factoring** | pull independently-testable sub-behaviour out of a component whose *interface* cycle is fixed by an external spec | interface cycle immovable → shrink the implementation cycle to a minimum; extract non-template code out of templates (compile time + code size) |
| **Escalating encapsulation** | hide not the lower components but **their use** — the client-facing interface passes only value types | `Graph` exposes `NodeData`/`EdgeData`; `Node`/`Edge` become implementation-only components |

- **Escalation → demotion is by far the most frequent combination.** Examine those two first.
- A recursive destructor (`~Link() { delete next; }`) is slow **and overflows the stack on a long list.** Hierarchical ownership must be enforced by the **type system**, not by a runtime convention.
- Lakos's honest ordering: opaque pointers are for repairing an *existing* cyclic design; dumb data has proved more useful for removing dependencies *completely*.

## PHYS-04 — Granularity is a craft. Do not split mechanically

- Over-splitting **hurts discoverability.** "Someone looking for `DateUtil` — how do they know `DayOfWeekUtil` exists?" is the brake the source applies to itself.
- **Operations that are inherently primitive stay on the type** — the ones that need the internal representation to be efficient. `dayOfWeek()` is one `% 7` on a serial date; moving it out loses that performance.
- **Code that must change together stays together.** A template and its specializations, an encoder and its decoder, an enum and the utility that knows it — separate them and every change forces a simultaneous release.

## PHYS-05 — Lift heavy leaf dependencies upward (layered → lateral)

When a low module depends directly on heavy machinery (DB access, platform API, external library), **everything above it is transitively** bound to it. Result: no module can be unit-tested on its own.

- Replace the heavy dependency with an **abstract interface (protocol)** and inject the concrete implementation from above (adapter). Switching the implementation then changes one type name in `main` and recompiles nothing else.
- Non-portable (platform-specific) code: **minimize it and isolate it in one place.** Do not mix it into domain code.
- Global singletons artificially block reuse and testing. Prefer structures where the dependency is passed down from above.
- The skyscraper argument: software scales **laterally, not vertically.** With 15-component subsystems, light layering has CCD 85, corresponding layering 92, heavy layering 190 — build cost grows **super-linearly** with distance from the leaves.
- ⚠ **On a performance path an abstract interface creates virtual calls** (collides with CPP-09). Use template binding on hot paths and protocols on cold/init paths. Lakos himself: for small, well-tuned, single-algorithm high-performance code, **layered is ideal** — it promotes inlining and cross-function optimization. **When this chapter and §0–§19 collide, the performance chapter wins on the hot path.**
- "Being able to mock it" is almost never sufficient justification for introducing a protocol — forcing a lateral structure purely for tests "makes a mockery of your design".

## PHYS-06 — Build dependency is measurable

Replace the feeling ("the build is slow") with a number. Parsing only `#include` directives is enough — no C++ parsing needed — so a few dozen script lines suffice (see Appendix D).

- Extract the module dependency graph, **find the cycles**, and count per-component cumulative dependency (CCD).
- Use it as a **before/after metric** for refactoring. MEAS-01 ("no optimization without measurement") applies to physical design too.
- Intuition for CCD on 3 components: all independent = 3; tree = 5; chain = 6; **cycle = 9.** Leaves are cheap to test; the average link cost per component rises sharply with distance from the leaves.
- Measured reminder: moving the build directory forces **one full rebuild as a fixed cost** because `CMakeCache` bakes paths into compile command lines (2026-08-27 `.50`, 1358 targets; reproduced 08-28 on `.52`).

## PHYS-07 — A `.c`/`.cpp` includes its own header as the first substantive include

Including the component's own header on the **first line** of the implementation file (after license/comments) makes the compiler **verify on every build that the header compiles standalone.** Include-order defects — "A.h must come before B.h or it won't compile" — become impossible to create.

- Cost is zero: it's a reorder. New files unconditionally; existing files whenever touched.
- In a codebase where this isn't upheld, refactoring one header has **unpredictable cost because you cannot know which order dependencies are hiding.**
- A test driver's dependencies must **not exceed those of the component under test** — the moment they do, the test needs everything heavier than its subject built first, and eventually nobody runs it.

## PHYS-08 — Colocate for one of four reasons only; colocate what changes together *(skill extension)*

Default: one public class/struct per component. Putting two or more in one component is justified in exactly four cases:

1. **Friendship is needed** (the most common reason) — "long-distance" friendship across components is forbidden, so friends live together.
2. **An unavoidable cycle** (almost never truly necessary; but if so, the cycle must live inside one component).
3. **Single solution** — small peer entities that are only useful as a whole.
4. **Flea on an elephant** — a tiny class (`ScopedGuard`) or function (`operator==`) that (a) adds no dependency, (b) depends on the heavy class in the same component, and (c) is an essential part of its normal use.

- **Do not colocate functionality whose clients are largely disjoint** — every client pays the build cost of both halves.
- Keep tight collaboration **inside one release unit.**
- Physical design comes **before** logical design: decide where a thing lives in the repository the moment you decompose the problem, not after.

## PHYS-09 — A type carries only primitive operations; non-primitives escalate to a utility *(skill extension)*

- Reusable component functionality must be **complete, minimal, and (almost entirely) primitive.** "Every useful operation should be a method" does not scale: the component grows unboundedly and every addition edits its source (open-closed violation).
- Non-primitive operations go to a **higher-level utility struct**. Exposing an **iterator** dramatically shrinks the set that looks primitive.
- General-purpose functionality written for one application: **always factor it, and demote it pre-emptively when time allows.** Skipping this continuous demotion is how a codebase decays into a Big Ball of Mud.
- Anti-example from the source: segregating packages by *kind* (value types in `bdet`, utilities in `bdetu`) failed — it hurt discoverability and blocked value types from using the utilities. **Modularize by semantics and physical dependency, not by syntax.**
- Statistics collection / logging baked into a core type is the DB instance of this: the core keeps the primitives, the rest escalates (pairs with BR-08's slow-path split).

## PHYS-10 — Pick the insulation technique by its runtime price *(skill extension)*

Three techniques achieve *total* insulation (clients never recompile when the implementation changes). Each has a cost; place it where the cost is acceptable.

| Technique | When | Price / limits |
|---|---|---|
| **Protocol class** (pure abstract interface) | inheritance is already in play — then it is almost always right. Change a base's `short`→`int` coordinate and *every* derived class and client recompiles unless a protocol sits in between | one virtual call per operation; clients have **no link-time dependency** on the implementation. Best insulator: no data, no private members, no ctor visible; even interface types can be locally declared |
| **Fully insulating concrete wrapper** | the same client both creates and uses the objects (inheritance unsuitable); an encapsulating wrapper already exists | **significant runtime overhead**; multi-component wrappers are generally impossible (friendship rule) → only when the wrapped surface is small and fixed; clients still inherit the wrapped implementation's dependencies |
| **Procedural interface (PI)** | (1) large legacy subsystem, (2) actively growing horizontal library whose additions would otherwise recompile every client, (3) **C adapter for a C++ core** (CUBRID CCI-shaped) | overhead + fidelity loss (inheritance/templates especially). PI components are all level 1, hold no domain logic, map 1-1 to the wrapped components, treat underlying types opaquely, and are callable from C and C++. Never where it would block side-by-side reuse |

- Interface inheritance is the only kind that reliably adds value; **implementation inheritance in library code is suspect** — prefer separate concrete classes (Bridge) over a shared partial-implementation base.
- **An insulating wrapper's overhead is dramatic; it must sit at a level high enough that the slowdown is acceptable.** Never on the row loop.

## PHYS-11 — Component hygiene that costs nothing *(skill extension)*

Rules from the source that are free to follow in new code and pay back immediately in a legacy codebase:

- **No runtime-initialized file/namespace-scope statics.** Beyond ordering hazards, a library `.o` is only pulled in when it resolves a symbol — a "registry via static initializer" silently disappears depending on whether the `.o` came from an archive or the command line.
- **Never reach a component via a local `extern` declaration; only via its header.** A wrong local declaration is a link error or a runtime fault instead of a compile error — and it hides the dependency from any tool that parses `#include`s (PHYS-06 relies on that parse being complete).
- **Every header must compile standalone** (PHYS-07 is the mechanism that checks it).
- **No directory paths in `#include`**; component file names are unique so that deployment can regroup libraries freely. Splits made only for deployment must not become architecturally significant.
- **No `using` directive/declaration outside function scope in a header.**
- Design to prevent **accidental misuse, not fraud** — do not contort the design to stop deliberate circumvention.
- Legacy caveat: CUBRID predates these rules. Apply them **unconditionally to new files, and to existing files only in the lines you already touch** (surgical-change rule).

## Symptom → prescription (DB-engine mapping)

| Symptom seen in practice | Diagnosis / prescription |
|---|---|
| Touch one header, everything rebuilds | **Insulation missing** (PHYS-01). Push `#include`s into `.cpp`, forward-declare, protocol at major interface boundaries (PHYS-10) |
| Cannot unit-test one module in isolation | **High CCD, heavy layering** (PHYS-05/06). Leaf depends on a heavy subsystem → go lateral |
| Cyclic include breaks the build, patched with `#ifdef` | **Levelization techniques** (PHYS-03). Escalation/demotion first |
| storage↔query↔optimizer reference each other | **Escalate** the contact point to a new higher component, or make it name-only via opaque pointer / dumb data |
| Child holds a parent pointer → cycle | **Demote** the common info, or an opaque back pointer. Node/Edge/Graph maps onto B-tree page/node, transaction/lock structures |
| Statistics/logging welded into the core type | **Slow-path split + escalation** (PHYS-09, BR-08). Core keeps primitives |
| Platform-specific code mixed into domain code | **Minimize and isolate non-portable code** (PHYS-05). adapter + protocol |
| Storage manager etc. as global singletons | Singletons artificially block reuse (PHYS-05). Inject from above |
| Ownership exists only as a runtime convention | **Manager class** (PHYS-03). Recursive destructors overflow the stack |
| Header won't compile standalone; include order matters | **PHYS-07** removes the class of defect permanently |
| Finding where things live only by `grep` | Name cohesion: the use site should reveal the file (`bdlt::Date` → `bdlt_date.h`) |
| Third-party integration code leaking into the core | Isolate adapter packages into their own release unit; keep heavy dependencies out of the core group |
| Must ship a C API alongside (CCI) | **Procedural interface** (PHYS-10) — a C adapter is one of its three legitimate uses |
