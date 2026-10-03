# RFC 0031: The object engine — a sound heap abstraction behind the engine seam

- **Status**: Implemented (as amended by [RFC 0032](0032-runtime-enforcement.md): unit record format 29 becomes 30, the place class of array-element cells at a boundary (§4.9) is the one RFC 0032's first amendment states, and the part of gate G6 that runtime extents and liveness close is closed there)
- **Authors**: WeaveC authors
- **Created**: 2026-09-28
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Replaces the engine of RFC 0030
  (`FunctionDataflow`, RFC 0030 §15) and the summary format of RFC 0030 §9.1
  and §13.1 (summary format 29 becomes 30, unit record format 28 becomes 29).
  Amends RFC 0030's assumption A3 (*Soundness*, below), its §3 certainty
  rules (restated over objects in §5), its §9.4 boundary facts (§5.6), its
  §14 seam (§8), and takes over its open gates G10, G14 and G15. Amends RFCs
  0002–0017 wherever they describe how `FunctionDataflow` represents a fact;
  what those RFCs decide about outcomes and diagnostics stays in force as
  RFC 0030 restated it.

This RFC was drafted before implementation. On 2026-09-28 the project owner
accepted the recommendation of the milestone analysis (replace the engine
behind the seam, together with the engine-independent fixes, and schedule a
runtime backstop after it) and authorized drafting this RFC and implementing
it end to end in one large change, with breaking changes and deletion of the
old engine rather than compatibility layers. Accepted status records that
authorization. The status becomes Implemented only when every gate in
*Acceptance gates* passes on the final tree.

Where this RFC departs from the recommendation as it was put to the owner,
the text says so with the word **Departure** and the reason.

## Summary

RFC 0030 made WeaveC decide every facet of every memory operation and made
`weavec-cc` insert checks for what it cannot prove. What it proves is only
as good as its engine, and the engine it kept, `FunctionDataflow`, stores
facts on *access paths* (`bx.buf`, `q->buf`) and copies them to the aliases
it knows about when the fact is made. An alias it does not know about at that
moment never receives the fact, so the engine proves things that are false:
a heap overflow through a buffer replaced via an alias is *proven in
bounds*, `weavec-cc` emits no check, and the program runs past the overflow.

This RFC replaces that engine with the **object engine**. Facts live on
*abstract objects* and *symbolic values*; a pointer is a symbolic value
with a points-to set; memory is a map from object cells to values; copies
share values, so every alias sees every fact by construction. The engine
keeps entry heaps finite by k-limited, materialising entry objects, keeps
loops finite by recency and widening, proves bounds with a zone domain over
symbolic values, and states distinctness of objects explicitly, including a
new *owner-forest* assumption that makes ownership-based temporal proofs
sound. It publishes through the RFC 0030 seam unchanged in meaning, with a
new, smaller summary format. `FunctionDataflow`, its 23.7K lines, the
path-based Core trackers and the format-29 summaries are deleted.

The same change fixes the false definite errors and false traps measured on
eleven projects WeaveC was never tuned on, and adds those projects to the
corpus as a held-out set, so that precision is no longer measured only on
code the engine was shaped around.

## Motivation

Measured on 2026-09-28 on v0.11.0 (`581670a`), Release build, Apple M3.

**False proofs.** Each was confirmed with AddressSanitizer and with a control
that removes the alias (`build/rfc31/probes/`, reproduced in
`test/cases/soundness/alias-*.c` by S0):

```c
struct box { int *buf; };
void s3(int k) {
  struct box bx;
  struct box *q = &bx;
  bx.buf = malloc(16 * sizeof(int));
  if (!bx.buf) abort();
  q->buf = malloc(2 * sizeof(int));   /* replaces bx.buf through an alias */
  if (!q->buf) abort();
  bx.buf[10] = k;                     /* spatial: proven; no check; overflow */
}
```

```c
struct box *bp = malloc(sizeof *bp);   struct n *o = malloc(sizeof *o);
bp->a = o;  struct box *q = bp;  free(o);
return q->a->v;                        /* temporal: proven; use after free */
```

The same programs without the alias are definite errors. The cause is
structural, not a missing case: facts are keyed by `PlaceId` (an access
path, at most 8 steps), and `computeMirrors` copies a fact to the aliases
recorded *when the fact is made* (`lib/Analysis/Dataflow.cpp`, 161
occurrences of "mirror"). RFC 0030 §5.1 found one instance of this class
(unknown callees) and fixed it with lazy records; the general form remained.

**False definite errors on untuned code.** Eleven projects built with
`CC=weavec-cc` (bzip2, hiredis, http-parser, inih, libyaml, lz4, miniz,
mujs, sqlite, tinyexpr, utf8proc): five fail to build as shipped because of
false errors, among them

- `memcpy(&v[4], v, 4)` reported as copying between overlapping ranges
  (`memcpy(v + 4, v, 4)` is accepted);
- `fmt(&cmd, …); free(cmd); fmt(&cmd, …); free(cmd);` with `fmt` external:
  definite `double-free` and `use-after-free`, because the write through
  `&cmd` by an unknown callee is not modelled;
- pointer arithmetic on the stale value after `realloc` reported as
  `use-after-move` (libyaml `api.c:81–154`);
- a back pointer the object's own release makes unreachable reported as
  `lifetime-too-short` at link (bzip2 `bzlib.c:1321`);

and three projects trap at run time on correct code, two of them on the
zero-length null arguments of `fwrite(NULL, 1, 0, f)` and
`strncmp(s, NULL, 0)`, which the library table treats as requiring non-null.

**Coverage.** Across the corpus (program ledgers), 42% of spatial facets are
unresolved and only about 7% of unproven spatial facets are checked; 49% of
temporal facets are unresolved. The largest reasons are `unknown-extent`
(5,978), `unknown-callee` (4,872), `dangling-escape` (2,570) and
`may-alias-released` (1,685). The last two come from rules RFC 0030 had to
make coarse because the engine has no object identity: a boundary breaks the
entry assumption for a *type* (`struct cJSON`) rather than for the objects
the callee can reach, and a release makes every type-compatible pointer
loaded from a parameter unprovable.

**Cost.** mujs's single-file build ran 59 CPU-minutes and 3.7 GB before it
was killed (clang: 5.6 s); sqlite3.c did not finish in 15 minutes; Lua's
whole-program analysis takes about 300 s (RFC 0030 gate G15: 214 s). The
engine copies its whole state, 43 ordered maps keyed by strings, per block
visit and per extra successor, and walks the AST through a place builder
that formats declaration names on every field access.

**Maintainability.** `Dataflow.cpp` is 11,776 lines; `FunctionDataflow` has
about 280 methods, 130 members and 35 side tables keyed by AST nodes, and
cites superseded RFCs 633 times. An object representation cannot be added
to it in place: every tracker (`Moves`, `Nullness`, `Spatial`, `Resource`,
`Raw`, `Borrow`) is keyed by path.

The seam RFC 0030 built (§14) exists so that this replacement can happen
without touching the ledger, the kinds, the library table, the planner, the
emitter or the tests. This RFC uses it.

## Soundness

### The guarantee

RFC 0030's guarantee (S), (N), (T), (V) and the blame property are
unchanged. Its assumptions A1, A2, A4 and A5 are unchanged. A3 gains one
clause:

> **A3 — other code maintains the heap invariants.** Code outside *U*
> leaves every pointer *U* can reach through parameters, results and globals
> either null or pointing to a live object with at least one element of its
> type. Pointers in owning slots are unique **and acyclic: no object is
> reachable from itself by following owning slots only (the owner forest)**.
> Exact counted-field invariants on header structs that *U* relies on hold.

An *owning slot* is RFC 0030's (§9.4): a field or global some function of
the unit (at link, of the program) releases a value loaded from, or one
declared `WEAVEC_OWNED`. The new clause is what makes a pointer loaded from
an owning slot of an object distinct from that object and from its owners,
which is what lets a list or tree destructor be proven (§4.5). It is the
invariant Rust's `Box` enforces by construction. Code that keeps an owning
cycle (a circular list freed by a counter) breaks it; such code gets
possible findings or unresolved facets where the cycle is visible to *U*,
and where it is not, the assumption is listed like the rest of A3. The
link step reports what it can verify (§8.3).

### What changes about soundness

- **Proofs no longer depend on alias bookkeeping.** Every fact a decision
  reads is looked up through the operand's value and points-to set at the
  site (§4.1). A fact made through any alias is on the object, so it is seen
  through every other alias. The domain's invariants I1–I6 (§4.7) are what a
  proof rests on, and each has a unit test that breaks it on purpose.
- **Distinctness is explicit.** Two pointers are treated as possibly equal
  unless one of the rules D1–D6 (§4.5) proves them distinct. RFC 0030's
  §3.1 list of distinctness rules becomes D1, D2 and D5; D3 (the owner
  forest), D4 (freshness) and D6 (derivation) are new, and each is as strong
  as the assumption named with it.
- **Boundary facts are per object, not per type.** A call breaks the entry
  assumption only for objects the callee can reach (§5.6). RFC 0030's type
  class propagation is kept for what a boundary hands to code the unit
  cannot see (the other units, at link), where the class is all that is
  known.

### Where this is less sound than v0.11.0

Nowhere intentionally. Every decision rule of RFC 0030 §3 keeps its
meaning; where the object engine cannot establish the premise of a proven
outcome, the facet takes the RFC 0030 unresolved reason for the missing
premise. A construct the object engine does not model is `unanalysed`
(§5.1), never proven.

Two rules become more permissive, each justified by the new A3 clause or by
facts the old engine could not see:

- a temporal facet through a pointer loaded from an owning slot of an
  object is no longer `may-alias-released` merely because some
  type-compatible object was released (D3, D6);
- a call no longer breaks the entry assumption for every object of a type,
  only for the objects the callee can reach (§5.6).

### Where this is more sound

- The false proofs above, and the whole class they belong to (§4.7, I1).
- Stores and releases through pointers the old engine represented only as
  "unknown place" (a pointer loaded from an array summary cell, a pointer
  returned by a callee and stored through twice) now update or taint the
  objects they may reach, instead of a path nothing else reads.

### Bugs caught

Everything RFC 0030 catches (its *Bugs caught*), with the same severities.
Additionally, as definite errors or checks where the values are exact, and as
possible findings otherwise:

- accesses through an alias created before the fact (the probes above);
- releases and replacements through pointers stored in heap objects
  (`*slot = o; alias = slot; free(o); (*alias)->v`);
- a use after `decref` of a reference-counted value's own share, through
  any copy of the value.

### Bugs deliberately not caught

As RFC 0030. In addition, where the owner forest is broken by code outside
*U*, a destructor that relies on it may be proven although it frees a node
twice. This is A3, not a new blind spot of the engine.

### Accepted false positives

The engine may still give possible findings (warnings) on correct code, in
the same classes RFC 0030 lists. It must not give *definite* findings on
correct code in any of the held-out projects (gate G5).

## Detailed design

### 1. Scope

| Kept (unchanged or edited) | Replaced | Deleted |
| --- | --- | --- |
| `Ledger`, `PointerKind`, `LibrarySpec` (+ table), `CheckPlan`, `FnSlots`, `Diagnostic`, `SourceLocation`, `Scc`, `AnalysisStats`, `Integer` (target integer semantics), `Ownership`; `AttributeReader`, `KindInference`, `KindTable`, `SiteCollector`, `SlotCollector`, `BoundaryInvariants`, `CheckPlanner`, `LedgerAdapter`, `Annotations`, `Concurrency`, `BypassedDeclarations`, `UnitPipeline`; all of Frontend except the parts named in §8 | `SafetyEngine` implementation (`DataflowEngine` → `ObjectEngine`); `FunctionSummary` and `SummaryIO` (format 29 → 30); `SummaryStore`; `ProgramDatabase` import/export of summaries; the unit record's summary section (format 28 → 29) | `lib/Analysis/Dataflow*.{h,cpp}`, `PlaceBuilder`, `FunctionAnalysis`, `TranslationUnitAnalysis`, `CallbackSummaries`, `CallContextSummaries`, `KindSeeding`, `FunctionPreparation`, `AffineSupport`, `IntegerSupport`, `Allocators`, `LibrarySummaries`, `SummaryDependencies`; Core `AliasRelation`, `AnalysisState`, `Array`, `Borrow`, `CallContext`, `CallTargets`, `Moves`, `Nullness`, `Offset`, `Place`, `Raw`, `Relation`, `Resource`, `Scalar`, `Spatial`, `Lifetime`, `Traversal`, `CheckedInteger`, `IntegerExpression`, `SummarySteps`, `Interface`; their unit tests |

`SummaryPath` (a root and field/deref/index steps) survives: the seam's
`BoundaryFacts`, `PointerKind`'s `ExtentTerm` and the kinds use it. It moves
to `include/weavec/Core/Path.h`. What `IntegerExpression`, `Interface` or
`CallTargets` provide that a kept component still needs moves with it; a
kept component may not keep a deleted header alive (hygiene gate H2).

The line budget (H2) is re-derived at the end of S7 from the measured tree
and recorded as an amendment, as RFC 0030 did. It is a ratchet: the new
engine may not grow past it without an amendment.

### 2. Architecture

```
            Clang AST + CFG (per function)
                     │
   ┌─────────────────▼──────────────────┐
   │ lib/Analysis/Engine*.cpp            │  transfer: expressions, lvalues,
   │   (private header Engine.h)         │  calls, library rows, summaries,
   │                                     │  decisions, witnesses, boundaries
   └───────┬───────────────────┬─────────┘
           │ domain ops        │ publishes only through
   ┌───────▼─────────┐   ┌─────▼─────────┐
   │ Core: Heap.h,    │   │ LedgerAdapter │ (unchanged seam, RFC 0030 §14)
   │ Zone.h, Values.h,│   └───────────────┘
   │ Persistent.h,    │
   │ Summary.h (v30)  │   no Clang, unit-tested alone
   └──────────────────┘
```

- **Core** holds the abstract domain: symbols, values, objects, cells, the
  zone domain, join, widening, garbage collection, materialisation,
  distinctness, summaries and their text form. It never sees Clang. Objects
  refer to program entities through opaque 32-bit handles the Analysis layer
  assigns (the `core::SourceLocation::opaque` pattern).
- **Analysis** holds the transfer functions over the Clang CFG, the call
  dispatcher, the decision rules and the TU driver. It is the only place
  that includes `Engine.h`. Hygiene gate H2's include rule moves from
  `Dataflow.h` to `Engine.h`, with the allowed includers `ObjectEngine.cpp`
  and `Engine*.cpp`.
- There is no separate IR. The engine evaluates Clang's CFG built with
  `setAllAlwaysAdd()`, so every subexpression is an element in evaluation
  order, and the value of each evaluated expression is kept in a per-block
  expression environment, as Clang's own flow-sensitive framework does.
  `?:`, `&&`, `||` and `,` then need no special evaluation order.
  **Departure:** the recommendation said "a small IR lowered from Clang's
  CFG". Evaluating the always-add CFG gives the same linear order without a
  second representation to keep in sync with the AST that sites, witnesses
  and the emitter all name.

### 3. The engine's run

For each unit:

1. **Order.** Build the call graph over the unit's definitions (direct
   calls, plus indirect calls resolved by the slot solution), take its
   strongly connected components in bottom-up order (`core::Scc`).
2. **Summaries.** Analyse each component bottom up. A non-trivial component
   iterates its members' summaries to a fixpoint, with summary widening
   (§6.4) after 3 rounds and a hard limit of 8 (the limit marks the
   summaries incomplete, §6.5). These runs publish into a discarding
   adapter.
3. **Authoritative pass.** Each emitted function (RFC 0030 §2.6) is analysed
   once more with the final summaries of everything it calls, publishing
   through the unit's adapter after `beginFunction`. A function whose
   summary run already had the final callee summaries (every function
   outside a cycle whose callees did not change) reuses that run: its
   decisions were recorded during the run and are replayed into the
   authoritative adapter, so a leaf function is analysed once.
4. **Contexts.** Alias contexts (§6.6) run after the authoritative pass,
   publish only diagnostics (RFC 0030 §2.6 *Departure*), and share the
   callee's second budget (RFC 0030 §5.5).
5. **Exports.** The summaries, the kinds and the facts of §7.

`weavec --whole-program` and the link step run the same algorithm over the
program's units with the program database (§7), as RFC 0005 and RFC 0030
§13 describe.

### 4. The abstract domain (Core)

#### 4.1 Symbols and values

A **symbol** (`core::Sym`, 32 bits) names one runtime value that the
function computed or received. Symbols are immutable: a program variable
that is assigned gets a new symbol. Every value the engine handles is one
of:

- an **integer symbol**, whose numeric facts live in the zone (§4.4);
- a **pointer symbol**, with attributes (§4.3);
- a **function value**: a bounded set of function names (at most 32), or
  unknown;
- **unknown** of a type: no facts.

Copying a value copies its symbol. That is the whole of the alias
mechanism: two variables, cells or expressions that hold the same symbol
hold the same runtime value, so a fact attached to the symbol, or to the
objects it points to, is seen through all of them.

A **value set** is a set of at most 4 symbols of one type, for weak cells
(§4.2); a loaded value set of more than one symbol becomes one fresh
*join symbol* whose attributes are the join of the members' and which is
recorded as *may equal* each member (§4.5, D6).

#### 4.2 Objects and cells

An **abstract object** (`core::ObjectId`) stands for one or more runtime
objects. Its **origin** is one of:

| Origin | Singular | Name | Created |
| --- | --- | --- | --- |
| `local` | yes (per activation) | the declaration's handle | at function entry for every local whose storage the function uses; its lifetime ends at scope exit (CFG lifetime-end elements) |
| `global` | yes | the declaration's handle | on first use |
| `literal` | yes | the string literal's handle | on evaluation; read-only |
| `function` | yes | the function's handle | on `&f` |
| `heap recent` | yes | the allocation site's handle | by an allocating call (§5.4) |
| `heap old` | no | the allocation site's handle | when a `heap recent` of the same site is allocated again: the old recent object folds into it (*recency abstraction*) |
| `entry` | yes | a `SummaryPath` from a parameter, a global or a callee's result | materialised on first load from the entry heap (§4.6) |
| `entry summary` | no | a `SummaryPath` prefix and a repeated step | when an entry path exceeds the k-limit (§4.6) |
| `unknown` | no | none | the target of a pointer the engine has no facts about (a raw value, an integer conversion) |

A *singular* object stands for at most one runtime object at a time, so a
store through a pointer that must point to it is a *strong update* and a
release of it is a definite release. A non-singular object gets weak
updates and may-releases only.

A **cell** is `(object, key)`. The key is a byte offset within the object,
computed from Clang's record layout, for fields and constant indices up to
the object's first 64 elements; a variable index, or a constant one beyond
that, uses the object's **summary cell** `[*]` for its element type (weak).
*Amended by §4.9:* an affine variable index names a **selected cell**, and
the summary cell holds only what stores through indices the engine cannot
name wrote.
A union's members share offsets; a load of a pointer member from a cell
whose last store was of a non-pointer type yields an unknown value with the
`raw-cast` reason (RFC 0030 §2.3). Bit-fields are integer cells of their
storage unit. A byte-wise write (`memset`, `memcpy`, a character-typed
store, a `LibrarySpec` `w` argument) over a range of cells replaces them:
by the copied symbols when the copy is a whole-cell copy between objects of
compatible layout, by zero symbols for a zero fill, and otherwise by
unknown values marked `raw-cast` for pointer cells.

The **memory** is a persistent map from cells to value sets. A cell that
was never written in this activation holds its *entry value*: for
`local`, uninitialised (§5.9); for `heap recent`, zero or uninitialised by
the allocating row (§5.4); for `entry`, `global` and `heap old`, an entry
symbol materialised on first load (§4.6). *Amended (S7):* a `global` with
internal linkage that the unit only reads by value (every reference to it
is a load through subscripts of it and `.` members, or unevaluated; its
address is never taken or passed, and no store names it) holds its
initializer throughout, so a function-pointer cell of it reads the function
its initializer names there (a dispatch table `static void (*t[2])(void *)
= {drop, keep}` calls `keep` through `t[1]`).

#### 4.3 Attributes

A pointer symbol carries:

- **points-to**: a set of at most 8 `(object, offset)` targets, or `⊤`
  ("any object"). The offset is an affine term `k·s + c` over one integer
  symbol `s`, or unknown. A points-to set of more than 8 targets becomes
  `⊤`;
- **nullness**: `null`, `nonnull` or `maybe`, with RFC 0030 §3.2's
  `allocatorSource` bit;
- **release record**: none, or `released {location, family, allPaths,
  conditional, unknownOrigin, lossy, via}`, the RFC 0030 §3.1 record moved
  from places to values; `moved` is the same record with reason `moved`;
- **share count** (RFC 0010): the number of references this value holds
  on a reference-counted object, when it is known;
- **raw origin** (RFC 0004) with its creation location;
- **derivation** (§4.5 D6): the symbol and owning slot it was loaded from,
  when it was loaded from an owning slot;
- **names**: the C spellings it had where it was created (for messages
  only, never for identity).

An object carries:

- **extent**: `bytes = k·s + c` with its class (`exact`, `declared`,
  `lower-bound`, RFC 0030 §7.1), or unknown; for a `local` or `global` of
  complete type, the constant `sizeof`; for a VLA, the size symbol;
- **state**: `live`, `released` (with the record of the release that made
  it so), `ended` (storage lifetime over), `may-released` (a weak release
  or a join of `live` and `released`), or `unknown-released` (the
  unknown-callee default, RFC 0030 §5.1, which keeps its `unknownOrigin`
  meaning);
- **family** of its allocation (RFC 0007) and whether this activation owns
  it (for leaks, §5.8);
- **string fact**: an affine term bounding the offset of the first NUL, when
  known (RFC 0012);
- **origin facts**: read-only (literals), escaped (its address was stored
  where a callee or another unit can reach it, or passed to one).

#### 4.4 Numbers: the zone domain

Integer symbols and the offset and extent terms are related by a **zone**
(difference-bound constraints `x − y ≤ c` and bounds `x ≤ c`, `−x ≤ c`),
kept closed incrementally (each added constraint tightens the matrix in
O(n²)). Every integer symbol also has an interval in the target type's
range (RFC 0017 semantics: `Integer.h`'s modular ranges, kept), and the
zone and the intervals are kept consistent.

- A condition edge (`i < n`, `p != NULL`, `x == 3`, `!flag`) adds its
  constraint to the successor's state and prunes the edge when the
  constraint is unsatisfiable.
- `a = b + c` with a constant `c` records `a − b = c`; other arithmetic
  computes intervals only, with wrap-around per RFC 0017.
- A comparison of an access `k·i + c₁ + w ≤ k·n + c₂` against an extent with
  the same scale is a zone query `i − n ≤ (c₂ − c₁ − w)/k`; different scales
  fall back to intervals.
- The zone is limited to 64 symbols per state. Beyond that the symbols held
  by the fewest cells lose their relations first (intervals stay), which is
  sound: it forgets.

#### 4.5 Distinctness

Two pointers *may be equal* when their points-to sets share a target that
could be the same runtime object. Objects `o₁ ≠ o₂` may still be the same
runtime object when both are `entry` or `entry summary` objects (or one is
`unknown`), because the entry heap can alias. They are **distinct** when one
of these rules applies:

- **D1 — types.** Their types could not designate the same object under C's
  effective-type rules: neither is a character type and they are not
  compatible (all pairs are compatible with `-fno-strict-aliasing`).
  Unchanged from RFC 0030 §3.1.
- **D2 — owners.** Both were loaded from owning places (RFC 0030 §9.4),
  which A3's uniqueness keeps distinct. Unchanged.
- **D3 — owner forest.** One is reached from the other through a path that
  contains an owning step: `E(p.next)` is distinct from `E(p)`, and from any
  prefix of `p`, when `next` is an owning slot. New, from A3's acyclicity.
- **D4 — freshness.** An object this activation created (a `local`, a
  `literal`, a `heap recent` or `heap old` of a site it executed, a callee's
  fresh result) is distinct from every `entry` object, and two fresh objects
  of different origins are distinct.
- **D5 — identity.** Different singular objects of the same origin kind
  that the engine created are distinct (two locals, two allocation sites).
- **D6 — derivation.** A symbol loaded from an owning slot of the object a
  symbol `s` points to is distinct from `s`'s target and from every object
  `s` was itself derived from (transitively, up to depth 8). This is D3 for
  values that no longer have an entry path, such as the cursor of a loop
  after a join.

A join symbol (§4.1) may equal each of its members and nothing else they
are distinct from.

#### 4.6 Entry objects, k-limiting and materialisation

At entry, each pointer parameter `pᵢ` holds an entry symbol pointing to
`E(param i)`, with the nullness and extent of its kind (RFC 0030 §7.3;
the engine reads `EngineInput::kinds`, which replaces the old
`KindSeeding`). A load of a pointer from a cell of an `entry` object that
this activation has not written materialises `E(path.f)` (or
`E(path[*])` for a summary cell), with the kind of the slot it was loaded
from (RFC 0030 §7.3).

**k-limit.** An entry path in which the same `(record type, field)` step
occurs more than twice, or which is longer than 6 steps, is folded into an
`entry summary` object `E(prefix.f+)` covering every object reachable from
`prefix` by one or more `f` steps. Its cells point back to itself.

**Materialisation.** When a symbol `s` whose points-to set contains a
non-singular object Σ is released, or when a pointer is loaded through `s`
from Σ, the engine *focuses*: it creates a singular object `N` with Σ's
attributes and cell values, retargets `s` (and every cell and expression
holding `s`) from Σ to `N`, and adds `N` to the points-to set of every other
symbol that points to Σ and is not distinct from `s`. `N`'s cells that held
pointers into Σ keep pointing into Σ, which now stands for "the other
objects". By D3 and D6, a pointer loaded from an owning slot of `N` is
distinct from `N`.

**Garbage collection.** At every join and widening point, objects that are
unreachable from the roots (the function's locals, parameters' cells,
globals, the expression environment and the result) are dropped. Dropping a
`heap` object this activation owns that is neither released nor escaped is
a leak (§5.8). Dropping a released object is how a released list node that
nothing points to any more stops tainting its summary.

**Folding.** At a loop head's widening (§4.8), a singular object that was
materialised from Σ inside the loop and is still reachable is folded back
into Σ (its state and cells join into Σ's), so the number of objects is
bounded by the program's allocation sites, locals, and the k-limited entry
paths.

With these three rules a destructor

```c
void list_free(struct n *p) { while (p) { struct n *next = p->next; free(p); p = next; } }
```

is proven: each iteration materialises the current node, releases it, and
moves to a node D6 keeps distinct; the released node is unreachable at the
loop head and is collected, so no released object is left for `p->next` to
alias.

#### 4.7 Invariants a proof rests on

- **I1.** Every symbol a cell, variable or expression holds points only to
  objects in its points-to set, or into `⊤`. A store or release through a
  pointer updates every object in its set (strong when the set is one
  singular object and the pointer is not null on the path, weak otherwise).
- **I2.** Two symbols that may denote the same runtime object are equal or
  not distinct by D1–D6.
- **I3.** An object's state is the join of every release that may have
  reached it on some path, and a symbol's release record is the join of the
  releases of that value.
- **I4.** A cell's value set contains every value the cell may hold on
  some path reaching the point; the entry value of a cell never written in
  this activation is its entry symbol.
- **I5.** Every zone constraint holds of the runtime values of the symbols
  on every path reaching the point.
- **I6.** Garbage collection drops only objects no root can reach, so no
  later access can name them.

A proven facet (§5) is derived from I1–I6 and the entry assumptions only.
Each invariant has a unit test in `unittests/Core/HeapTest.cpp` that
constructs the state a violation would produce and checks that the
corresponding decision is not proven.

#### 4.8 Joins, widening and iteration

- **Join.** Objects join by identity (their origin names them
  deterministically). A cell present on one side only keeps its value if
  the object is absent on the other side (an absent object cannot be
  referenced from that side), and joins with the entry value otherwise.
  Two different symbols in one variable or cell join into a *join symbol*
  named deterministically by `(block, cell)`, so iterations reach the same
  names, with the join of their attributes and the projected zone.
- **Iteration order.** Blocks are visited in a weak topological order
  (Bourdoncle). Loop heads widen after 2 visits: intervals and zone bounds
  that grew jump to the type's limits or to the nearest program constant;
  points-to sets that grew keep growing up to their bound; materialised
  objects fold (§4.6). One narrowing pass follows.
- **Budget.** The block transfers count against `-fweavec-budget`
  unchanged (RFC 0030 §5.5, default 50,000). A function over budget takes
  the §2.6 defaults with reason `budget`, and its summary is incomplete.
- **Persistence.** Memory, attributes and the zone are persistent maps
  (`core::PMap`, sorted vectors shared by reference count and copied on
  write), so a state is copied per edge in O(1) and a join costs the size
  of the difference, not of the state.

#### 4.9 Elements and ranges (*Amendment (arrays)*)

*Added during S2.* One weak summary cell per element position made every
read of `a[i]` the same symbol, so `free(a[i]); *a[j] = 1;` was a definite
use after free inside `zap` (evaluation `rfc0016-array-good`), and it could
not express RFC 0015's release history: every cleanup loop
`for (i = 0; i < n; i++) free(a[i]);` warned "may be freed twice", and no
summary could say that a helper released elements `[0, n)`. The array
domain is therefore RFC 0015 §1–§5 over objects, as a segmentation of each
element position:

- **Selected cells.** A load or store at byte offset `k·s + c` for an
  integer symbol `s` uses the cell `(o, k·s + c)`. Symbols are immutable,
  so two accesses with the same key are the same runtime cell, and a saved
  index (`old = i; i = 7; a[old]`) still names the element it named. Two
  keys whose offsets the zone makes equal are one cell; offsets it proves
  different, or with different residues modulo the element size (other
  fields), are different cells; otherwise they *may* be the same cell. At
  most 32 selected cells per object.
- **Summary cell.** Holds only the values of stores through positions the
  engine cannot name (an unknown offset, a callee's `[*]` store, an
  evicted cell or range). A missing summary cell is "nothing written that
  way", so a join keeps a summary cell one side has.
- **Segments.** An object keeps, per element position (offset within the
  element, element size), a list of ranges `[from, to) ↦ v`, newest first
  (at most 4 per position). `from` and `to` are element-index terms over
  symbols; `v` describes *each* element of the range: a read copies `v`'s
  attributes into a fresh symbol (the elements are different runtime
  values), and a definite release record on `v` says every element in the
  range was released. `v` joins two such values keeping a record definite
  only when it is definite on both.
- **Reads.** An element's value is its own cell; else a cell the zone makes
  equal; else a copy of the newest range that must contain it; else the
  unwritten value (§4.6) joined with the summary cell. Cells and ranges that
  *may* be the element contribute their values as *possible* ones: a release
  record keeps its evidence as a possible release (RFC 0015 *Accepted false
  positives*: an index that may select a released cell is reported as a
  warning), while the summary cell's contribution is `aliasOnly` (no
  diagnostic, never proven). A read of "some element" (a callee's `[*]`
  path) joins everything at the position `aliasOnly`. The objects that
  elements of an entry array point to, `E(p[*])`, are not singular.
- **Writes.** A store to an element cell is strong for that cell when the
  object is singular and single; cells and ranges that may be it take the
  value weakly; a range that must contain it is shadowed by the cell. A
  store through a summary key reaches every cell and range at its
  position weakly.
- **Eviction.** A selected cell whose index symbol a join cannot keep, or a
  range that matches nothing on the other side of a join, is removed as a
  weak store of its value to the elements it described (the summary cell
  and the ranges that may hold them). Nothing an element holds is dropped
  without such a store, so the segments and cells only refine the summary
  cell and the unwritten values, and the decisions of §5.2 rest on I1–I6 as
  before.
- **Joins.** At a loop head, the element cells the iteration changed (on
  the back edge, a new cell, another value, or the same value with other
  facts such as a release) fold into ranges: a cell at the index a range
  ends at extends it (`[lo, i) + a[i] → [lo, i + 1)`), otherwise it becomes
  a one-element range; the head's own cell for that element is evicted. At
  every join, an element cell one side has only is matched with an element
  cell of the other side whose index the join pairs (the first iteration's
  `a[0]` with a later iteration's `a[i]`: one cell `a[i']` of the join);
  otherwise it is materialised on the other side, or evicted when that
  side's ranges cannot tell it apart. Ranges are aligned per position from
  their oldest ends; a bound of the joined range is a result symbol of a
  pair the join makes anyway (the variables' values, with a constant
  offset), or the pair of the two bounds' own symbols. A range only one side
  has is kept when the pairing makes it empty on the other side (the loop's
  first entry: `[0, i)` with `i = 0`), else it is evicted. Every choice is
  sound; they differ in precision only.
- **Entry objects across a join.** An entry object (or its dead copy) that
  one side released and the other never materialised is `may-released` in
  the join: the other path left it live. (Before this amendment the join
  kept `released`, and a loop's first iteration made `release *a always`
  part of the summary of a cleanup that may run zero times.)

**Copies** (RFC 0015 §4). `memcpy`/`memmove` of elements copy element-wise
by the element type the arguments point to (pointer and integer leaves of a
record), reading every source cell before writing any (so overlapping moves
are simultaneous). A symbolic length copies exactly the elements its lower
bound covers and writes the rest weakly. *Amended (S7):* when the source is
an `entry` object no store has reached in the activation (not havocked, not
the destination), the rest is instead a *copied range* of the elements the
length counts (`n` for `n * sizeof *s` without wrap, else a count symbol no
smaller than the lower bound's): each element holds the entry value of its
own source element, which is the symbol a load of that element reads (or
read, found by the cell it was materialised for), so `free(s[0])` after the
copy is seen through `d[0]`. An element the range may hold gets that value
as a possible one. Any later change to the range (a weak store, a fold, a
join) makes it the plain range its value describes (some element of the
source), so the refinement never outlives what it rests on. `realloc`'s new block starts with
the old block's cells, selected cells and ranges below its size; pointers
into the old block do not follow it.

**Summaries** (§6.1). An entry object's ranges and selected cells at an
exit become `release <p>[*]* elements [<from>, <to>) <family>` when their
value is the elements' own entry values released definitely, and `store
<p>[*] elements [<from>, <to>) := <value>` otherwise; bounds are constants or
terms over integer parameters the body never assigns. A range whose bounds
cannot be so expressed is exported without a range, as a possible effect on
some elements; a caller applies a `[*]` path without a range weakly. The
same holds for a global's elements (`global(g)[*]`). A cell of an entry
object or a global is a store (§6.2) unless it still holds the value
materialised for it (each such value remembers its object and cell, a join
keeps that only when both sides agree, arithmetic and copies drop it), so
an integer written with a non-constant value (`g_n++`) is exported as
`int [lo, hi]` from the zone; an object no store reached in the activation
exports no stores at all, and one that was stored into exports its
elements' entry values as `p[*] := path p[*] may` (they may have been
permuted). At a
call, `release … elements` releases the caller's cells in the range and adds
a range holding the released values; `store … elements` writes a range. A
`fresh` value of several new objects of one family (a fill loop's) is
`fresh … many`, a non-singular object at the call.

**Known limits.** A permutation of an array's own elements through
variable indices is not exported. The zone cannot keep `i ≤ n` across a
loop whose signed counter starts above a possibly negative `n`, so such a
range is exported without bounds. `n * sizeof(T)` of an unbounded `n` may
wrap, so it gives no count of copied elements.

### 5. Transfer and decisions (Analysis)

#### 5.1 Expressions and lvalues

An rvalue evaluates to a value (§4.1); an lvalue evaluates to an **address**,
a points-to set with an offset. Loads and stores go through the address:

- `x` (a local, parameter or global): the cell `(object(x), 0)`;
- `*e`, `e->f`, `e[i]`, `e.f`: the pointer value of `e` plus the field's
  byte offset or the index times the element size;
- `&lv`: the address as a pointer value (a fresh symbol pointing to it);
- `p + i`, `p - i`, `&p[i]`, `++p`: a fresh symbol with the same targets
  and the offset term moved; `p - q` over one object is the difference of
  the offsets;
- casts between pointer types keep the symbol (the object and offset do not
  change); an integer-to-pointer conversion other than a null constant makes
  a raw pointer to `unknown`; a pointer-to-integer conversion keeps the
  pointer symbol behind the integer, so the round trip is recognised;
- compound literals are `local` objects of their scope; string literals are
  `literal` objects; `sizeof` of a VLA is the VLA's size symbol.

A construct the engine does not evaluate (atomic builtins beyond load and
store, vector types, `__builtin_*` without a table row, statement
expressions containing jumps) yields unknown values and, for every site it
contains, the unresolved reason `unanalysed` with the construct named in
the detail.

#### 5.2 Decisions at sites

The engine decides each facet of each site `SiteCollector` enumerated
(RFC 0030 §2.1) from the operand's value at the site. The tables of RFC
0030 §3 apply unchanged; their premises are read from the object domain:

**Temporal** (Deref, Index, Call arguments, Release, Raw):

| Facts about the operand's value `v` at the site | Outcome | Diagnostic |
| --- | --- | --- |
| `v` has a definite release record (`allPaths ∧ ¬conditional ∧ ¬unknownOrigin`) | violation | `use-after-free` / `use-after-move` / `double-free`, error |
| `v`'s record is not definite and not `unknownOrigin` | `unresolved(may-released)` / `(may-moved)` | same id, warning |
| every target of `v` is released and singular, with definite records | violation | as above |
| some target is `released`/`may-released` and `v` has no record of its own | `unresolved(may-alias-released)` | none |
| some target is `unknown-released` | `unresolved(unknown-callee)` or `(callback)` | none |
| some target is `ended` | violation if every target is ended and singular; else `unresolved(may-dangle)` | `lifetime-too-short` as RFC 0030 §3.4 |
| otherwise | proven | none |

**Null** (RFC 0030 §3.2): `null` without `allocatorSource` → violation
(`null-dereference`); `nonnull` → proven; anything else → checked (with
`allocation-failure` when `allocatorSource`). Refinement after a
dereference follows §3.2: the symbol becomes `nonnull` downstream only when
the facet was proven or checked.

**Spatial** (RFC 0030 §3.3, §7.4): for each target `(o, off)`, the access
`[off, off + width)` is compared with `o`'s extent in the zone:

| Result over all targets | Outcome |
| --- | --- |
| in bounds for every target | proven |
| out of bounds for every value on every target, extent exact | violation (`out-of-bounds`) |
| undecided, and one target whose extent is exact or declared and expressible (§5.3) | checked |
| extent unknown or lower-bound only | `unresolved(unknown-extent)` |
| offset unknown against a known extent | `unresolved(unknown-index)` |
| `⊤` points-to | `unresolved(unknown-extent)` |

A target set with several objects is checked only when every target's
check is the same C expression (same extent term); otherwise it is
`unresolved(unknown-index)`. RFC 0030 §7.4's object rules (flexible
trailing arrays, complete-object extents for interior pointers, byte
arithmetic without wrap) are how the extent and offset terms are formed.
The overlap rule of `disjoint` requirements compares the two argument
ranges in the zone: `&v[4]` and `v` with length 4 are disjoint, which fixes
the lz4 false error.

Unsafe regions, `setjmp`, concurrency, assumptions and require levels
(RFC 0030 §5.3, §5.4, §6) are applied exactly as there.

#### 5.3 Witnesses and C names

A checked facet needs a witness (RFC 0030 §14, `CheckWitness`) whose terms
name C entities at the site. The engine keeps, per state, a reverse map from
integer and pointer symbols to the **C places** that currently hold them: a
local, a parameter, or a field path below one reached only through
non-escaping locals. A term over symbol `s` is expressible at the site when
some C place holds `s` there; the witness names that place. Because symbols
are immutable, a C place that holds `s` at the site holds the value the
extent was derived from, which is RFC 0030 §10.3 rule 4 without a separate
"unmodified" analysis. When no C place holds `s`, the witness carries the
symbol's defining expression if it is side-effect free and its operands are
themselves nameable, and is otherwise inexpressible.

In verify mode the engine publishes witnesses for proven spatial and null
facets too (RFC 0030 G6).

#### 5.4 Calls

A call's callee is resolved in RFC 0030's order (§5.1–§5.3, §9.3):

1. a declared ownership contract (annotations, ecosystem attributes);
2. the callee's summary from this unit (after its component was analysed);
3. the program database's summary (link, `--whole-program`);
4. its `LibrarySpec` row;
5. a platform-header declaration: borrow-only, `trusted(system-api)`;
6. otherwise unknown: the §5.1 default (below).

Indirect calls resolve through the flow-sensitive function value, then the
slot solution (RFC 0030 §9.3, unchanged). Several targets join their
summaries (a consume is definite only if every target consumes).

**Library rows.** A row's `alloc` creates a `heap recent` object of the
row's family with the extent the row's size expression gives, zeroed when
the row or zero-initialisation says so; its result symbol is
`maybe`-null with `allocatorSource` unless the row says `nonnull`. A
`release` releases the argument's value and its targets (§5.5). Argument
requirements (`r:bytes(n)`, `nonnull`, `string`, `disjoint`) become
requirement records on the LibCall or Release site with the decision rules
of §5.2. `realloc`-like rows keep RFC 0030 §8.2's outcome classes: the
argument is released on the `nonnull` class and on the `null` class when
the size is zero.

**Zero-length arguments.** A buffer argument whose required length is a
term that may be zero is `null-if-zero`: its null requirement applies only
when the length is non-zero, and its check is the zero-length `nonnull`
form. The table's rows for `fwrite`, `fread`, `memcpy`, `memmove`,
`memcmp`, `memset`, `strncmp`, `strncpy`, `strncat`, `strnlen`, `memchr`,
`snprintf`, `vsnprintf`, `write` and `read` are corrected accordingly
(§9.1). This removes the miniz and mujs false traps. **Departure:** C
leaves `memcpy(NULL, p, 0)` undefined; RFC 0030 §8.3 already chose the
zero-length form for `memcpy`'s fold case, and this RFC extends it to every
row whose length can be zero, because the trap protects no memory.

**Unknown callees** (RFC 0030 §5.1, unchanged in meaning, now per object):
for each pointer argument without an ownership contract, the call marks
every object reachable from the argument's targets through non-`const`
pointees, every escaped object and every object reachable from a global
external code can reach as `unknown-released` and forgets their cells
(they read as fresh entry symbols afterwards); the argument's own value
keeps its nullness and extent. A pointer argument that is the address of a
cell (`&cmd`) makes that cell *written by the callee*: after the call it
holds a fresh symbol of unknown nullness and no release record, which is
what the hiredis `fmt(&cmd, …)` pattern needs. The result is a fresh
`maybe` symbol pointing to `unknown` with the Single default (A3).

**Callee summaries** are applied by instantiation (§6.3).

**Callbacks, threads and signals** keep RFC 0030 §5.3: a `sync` callback
applies its targets' summaries as may-effects; `entry` targets define the
set G of shared globals and objects whose facets get the concurrency rules.

**Non-returning calls** end the path; the call's exit boundary (§5.6) is
checked first.

#### 5.5 Releases

A release of value `v` (a `LibrarySpec` release row, a summary's release
effect, `WEAVEC_RELEASES`):

- **Temporal facet.** `v` or its targets already released → `double-free`
  per §5.2's table; an `unknown-released` target → `unresolved(unknown-
  callee)`, and the record is replaced (RFC 0030 §3.1, *a known release
  after an unknown one*).
- **Spatial facet (`invalid-release`).** A target that is not `heap`,
  `entry` or `unknown` (a local, global or literal), or whose offset is
  provably non-zero, is a violation when it holds for every target, and
  `unresolved(may-invalid-release)` with a warning otherwise. The messages
  are RFC 0030's (`'<p>' is released but points to field '<f>' of its
  allocation`, …).
- **Family (`mismatched-release`).** Compared as RFC 0030 §3.4.
- **Effect.** `v` gets the release record (`allPaths = true` on this path).
  Each target becomes `released` when `v` must point to that one singular
  target, and `may-released` otherwise. Every *other* live symbol whose
  targets may equal a released target (§4.5) is left as it is: the object's
  state is what makes its later uses `may-alias-released`, and the symbol's
  own record stays empty, so no diagnostic is made from aliasing the engine
  cannot confirm. RFC 0030 §9.4 item 4 (interior aliases are not owners) is
  kept: releasing an object through an interior pointer does not release the
  enclosing object's other names.
- **Conflicting borrows.** When a pointer into a target (not the released
  value itself) is stored in a cell reachable from a parameter, a global or
  an address-taken local, the release is a `conflicting-borrow` (RFC 0002):
  an error when that cell holds the pointer on every path and the
  release is unconditional, a warning (`unresolved(may-conflict)`)
  otherwise.

Shared ownership (RFC 0010): a `decref` row or summary effect decrements the
value's share count and releases *the share*: the value gets a release
record with the message wording `after its reference was released`; the
object becomes `may-released` unless the engine knows the count reached
zero. An `incref` increments it. The count-field keys of RFC 0010 are
exported as before.

#### 5.6 Boundaries

At every boundary RFC 0030 §9.4 names, the engine publishes `BoundaryFacts`
over the objects **the callee can reach**: the objects reachable from the
arguments' targets and from every global external code can reach, through
the memory. A reachable cell that may hold a released or ended pointer and
is not overwritten before the boundary is a `Dangling` fact; two reachable
owning cells that may hold the same object are `SharedOwners`. Places are
spelled as `SummaryPath`s from the boundary's arguments and globals, and
each fact carries the RFC 0030 place class of the cell.

`BoundaryInvariants` is unchanged. What changes is what reaches it:

- a cell of an object the callee cannot reach is not a fact at that call
  (bzip2's `strm.state->strm` back pointer, freed together with `state`,
  is not reachable by the next call's arguments);
- the propagation of RFC 0030 §9.4 downgrades a proven temporal facet only
  when its operand was loaded from a cell of the broken place class *and*
  the cell's object may be one the breaking boundary exposed (the object
  was reachable there, or is an `entry` object not distinct from it). The
  type-class-only rule remains for rows that come from other units at link,
  where objects are not shared.

Exits: at an exit that returns to the caller, only storage whose lifetime
ended is reported (RFC 0030 §9.4 amendment 1); the result counts (amendment
2).

#### 5.7 Lifetimes

`local` objects end at their scope's CFG lifetime-end element and at
function exit. A pointer to an ended object:

- returned, or left in a cell reachable from a parameter or global at the
  exit: `lifetime-too-short` (definite when every target is ended and
  singular and the cell must hold it), else `unresolved(may-dangle)`;
- stored in a global during the function and overwritten before any read or
  boundary: nothing (the http-parser `test.c:2687` false error);
- dereferenced: the temporal facet per §5.2.

#### 5.8 Leaks

A leak (RFC 0007, never an error, RFC 0030 §3.4) is reported when garbage
collection (§4.6) drops an owned `heap` object that is neither released nor
escaped, at the statement that made it unreachable: an overwrite of the last
cell holding it (`'<p>' is leaked: it is overwritten without being
released`), a scope exit, or a return (`'<p>' is leaked`). Not at a return
from `main` or after an `exits` row (RFC 0030 §3.4). An object passed to a
callee that may retain it, or stored in a cell another unit can reach, is
escaped.

#### 5.9 Initialisation

A `local` pointer cell that was never written holds `uninit`. A use where
every value is `uninit`:

- with zero-initialisation on (the enforcing modes) is a null value: the
  null facet is a violation with `use-of-uninitialized` when every path is
  uninitialised, checked otherwise (RFC 0030 §3.1 table);
- with `-fno-weavec-zero-init`, `unresolved(no-zero-init)`.

Scalar `uninit` uses keep RFC 0030 §3.4's rule (error when definite, no
facet).

#### 5.10 Integer operations

`invalid-integer-operation` (RFC 0017) is reported when the operands'
intervals make the operation invalid for every value (division by a zero
interval, a shift count interval outside the width, a signed overflow for
every value). Possible invalid operations are not reported (unchanged).

#### 5.11 Diagnostics and messages

Every diagnostic id, message template and note of RFC 0030 is kept word for
word; the lit tests that pin them stay. Names in messages come from the
symbol's `names` (§4.3), preferring the operand as written at the site
(`'q->a'`), then the name the value was created under. The notes (`freed
here`, `freed here (through '<p>')`, `allocated here`, …) come from the
records.

### 6. Summaries (format 30)

#### 6.1 Content

A summary describes a function's effect on its *entry heap* (parameters and
globals, as `SummaryPath`s) and its result, per outcome case:

```
summary <name> v30
  always-returns | may-not-return | never-returns
  incomplete <reason>                       (optional)
  param <i> kind <kind> [relies-single]     (from KindInference, unchanged)
  result kind <kind>
  result fresh <family> [extent <term>] [zeroed] when <case>
  result path <path> [offset <term>]        when <case>
  result null when <case> | result nonnull when <case>
  result int [<lo>, <hi>] [rel <path> <op> <c>] when <case>
  release <path> <family> [lossy] [conditional] when <case>
  release <path>[*]* elements [<term>, <term>) <family> when <case>   (§4.9)
  move <path> when <case>
  unknown <path>                            (the §5.1 default reached it)
  store <path> := fresh <family> | null | path <path> | unknown when <case>
  store <path>[*] elements [<term>, <term>) := <value> when <case>   (§4.9)
  escape <path>                             (retained where others can reach it)
  share <path> +1 | -1 when <case>
  nonnull-on <case> <path>                  (RFC 0030 §9.2)
  string <path> nul-within <term>
  reads <path> | writes <path>              (for alias contexts, §6.6)
  context-request <alias-partition>         (§6.6)
```

`<case>` is RFC 0030 §9.1's grammar (`always`, `result <classes>`,
`result <classes> and param <i> =0|!=0`), extended by the pointer
comparison and entry test amendments below; the at-most-two-cases rule, the
`lossy` bit and the pruning amendment of RFC 0030 §9.1 are kept. Paths use
the `SummaryPath` spelling of format 29. Summary format 30 is not readable
as 29 or the reverse; a record of another version is stale (RFC 0030 §13.1).

The fields format 29 carried for the old engine alone (heap descriptions,
value snapshots, array copy/fill/release forms, numeric output expressions,
memory and callback context requests, object views, sized-field facts,
count lists) are not carried. What each fed is either derived at the call
from the fields above (array forms from `store`/`release` over `[*]` steps;
sized fields from `result fresh … extent`), or dropped with its cases
listed in `test/cases/KNOWN-DIFFERENCES.md`.

#### 6.2 Derivation

At every exit of the authoritative or summary run, the engine reads the
state against the entry heap: an entry object that is `released`, `moved`
or `unknown-released` gives the corresponding effect for its path; a cell of
an entry object or global that was written gives a `store`; a result
pointing to a fresh object gives `result fresh`, with the extent term
re-expressed over parameters when the zone relates it to one. The cases
come from the result's value on each exit path (RFC 0030 §9.1 derivation,
with the guard over symbols instead of places). Exits join into at most two
cases per effect.

#### 6.3 Instantiation

At a call, the callee's parameter paths are matched against the caller's
state: `param i` is the argument's value, each step loads through the
caller's memory (materialising entry objects as a load would), and each
effect is applied to the objects the path reaches in the caller, weakly
when the path reaches several. A `result fresh` becomes a `heap recent`
object named by the call site. A case keyed on the result becomes a pending
case on the result symbol, resolved by a later test of it (RFC 0030 §9.1's
`PendingOutcome` behaviour, now on the symbol).

*Amendment (heap outputs).* Added during S6 for RFC 0013's heap outputs,
which the first format-30 summaries lost:

- **Contents of stored objects.** A `store` of a new object into an entry
  cell is followed by the object's contents as `store … contents=<n>`
  (spelled `(new)` in dumps): the path lies below the new object that the
  store to its first `n` steps puts there. At a call, the dereferences of a
  contents path from step `n` on read the values the summary stores (the
  new objects, created before any store is resolved), never the entry
  heap's; one with no such value is not applied. Paths of stores into entry
  objects keep reading the caller's state before the call, so the two never
  name the same cell by accident. A cell that holds its entry value on some
  paths and a new object on others is that object, stored `may`; a cell null
  on some exits and a new object on the others holds the object
  `maybe-null`, and a null test of it in the caller tells the failure, which
  made no object.
- **Record results.** A record returned by value is described by
  `store result.<f> …` (fields without a dereference), when every exit
  returns storage of the callee's frame whose cells it knows; the caller's
  temporary for the call then holds exactly those cells (an unnamed field
  was never written). Otherwise the temporary holds unknown values.
- **Values.** `fresh … offset <c>` is a pointer `c` bytes into the new
  object (a cursor stored beside its base); `unknown raw` is a raw pointer
  (RFC 0004), which stays raw in the caller. An unknown value in a new
  object's cell is stored, never omitted (the caller would read the object's
  unwritten, zero value). Member paths spell nested records
  (`p->box.data`), so a caller resolves them.
- **Strings.** `string <path> nul-within=<t> [nul-from=<t>]
  [contents=<n>]` gives an object's RFC 0012 string fact relative to where
  its pointer points, on every exit where the object exists.
- **Class-keyed stores.** A store on some result classes leaves the join of
  the old and new values in the cell until a test of the result selects a
  class, which puts back the one value (a `Stored` pending case). The join
  stays when the call may release the old or the new value under a
  condition no case names: that release is then an alias's (§5.5), not a
  possible finding on the path where the store did not happen.
- **Absent objects** (§4.3, §5.8). On a path where an allocation failed (a
  null test of its result) or a class-keyed store did not happen, the new
  object is *absent*: it owns nothing there, and a join takes its ownership
  from the paths on which it exists, so a leak on those paths is still
  reported.

#### 6.4 Recursion and widening

A component's summaries are iterated from "no effect"; after 3 rounds, a
summary that still grows widens (paths that keep lengthening fold at the
k-limit; integer terms lose their bounds). After 8 rounds the remaining
summaries are marked incomplete.

#### 6.5 Incomplete summaries

RFC 0030 §5.5: a caller applies an incomplete summary's known effects plus
the unknown-callee default on every pointer argument.

#### 6.6 Alias contexts

A summary is derived assuming distinct parameter objects except where D1–D6
cannot separate them; where they cannot, the entry objects are joined
already and the summary is sound for aliased calls. Where a caller passes
arguments that *must* be the same object to parameters the callee treated
as possibly distinct, and the callee `reads` one after releasing the other,
the caller requests an alias context: the callee is re-analysed with those
entry objects unified, its diagnostics are reported at the use in the
callee with a note naming the call (RFC 0030 §2.6 *Departure*, probe
`two(p, p)`), and the call's temporal facet takes the result. At most 16
contexts per callee, sharing the callee's second budget.

*Amendment (numeric contexts).* A summary cannot say what a callee does
with values it does not know: `unsigned char narrow(unsigned n) { return
n; }` returns `int [0, 255]`, and `make(&n)` allocates `*n * 2` bytes of an
`*n` the summary cannot name. RFC 0017 §5 requires a narrowed result, a
checked size through an out-parameter, and a size computed from an input to
be preserved in the caller; format 30 carries no numeric expressions
(§6.1). So at a direct call to a function of the unit outside the caller's
component, whose general summary returns or stores an integer that is not
one constant, allocates a size it does not know, or has a `may`, `lossy` or
parameter-keyed effect, and whose call knows integers the callee reads (an
integer argument that is a constant, or an integer cell of the single object
a pointer argument, or a pointer global the callee names, points to), the
engine analyses the callee again in that *numeric context*: its alias
context (above) with those parameters bound to their constants and those
entry cells to theirs. A call whose alias context
is not trivial is analysed in it whatever its summary says. The run is a
summary run (it reports nothing) and the call instantiates the summary it
derives instead of the general one; the summary is sound for the call
because its entry state is the call's. Contexts are cached per callee (at
most 8), nest at most two deep, and are run only for callees whose own run
took at most 64 block transfers; a context run that is over budget or
incomplete leaves the general summary. Two pointer arguments are the same
object only when they point to one singular object.

### 7. Records, the program database and the link step

- `UnitExports` loses `memoryRequests`, `callbackRequests`, `sizedFields`,
  `sizedFieldLoads` and `unknownIndirectTypes`; it keeps `functions`
  (with format-30 summaries), `globals`, `imports`, `indirectTypes`,
  `unknownCallees`, `countFields` and `boundaries`, and gains
  `contextRequests` (§6.6).
- The unit record becomes format 29: format 28 with the summary section in
  format 30 and the removed fields gone. The schema fingerprint follows the
  codec's field table as before.
- `ProgramDatabase` imports format-30 summaries by linkage name and type key
  as before.
- **Anonymous members.** Paths through anonymous struct and union members
  spell the member by its index (`.#2`) instead of an empty name, so a
  function that reads one produces a record that decodes (the tinyexpr
  `te_eval` stale-record bug).
- The link step's algorithm (RFC 0030 §13.2: declaration verification,
  reliance checks, slot solving, the program-wide fixpoint over summaries,
  boundary propagation, the program ledger) is unchanged; its engine runs
  are `ObjectEngine` runs.

#### 7.1 The seam

`EngineOptions::analysis` (`AnalysisOptions`, `FunctionDataflow`'s own
tunables) is removed; the options it held that are not engine-private
(`dumpStream`, `stats`) become fields of `EngineOptions`. `SafetyEngine.h`
no longer includes `FunctionAnalysis.h` or `Summaries.h`. `facetOfDiagnostic`
moves into `LedgerAdapter` as a table keyed by the `diag::` constants.
`storeVerdict` and `EngineInput::fieldAssumptions` stay (RFC 0030 §7.6 may
return), unused.

### 8. The owner forest at link

The link step already checks owner uniqueness at boundaries it can see
(`second-owner`, RFC 0030 §9.4). The engine adds to each boundary's facts
an `OwningCycle` fact when a reachable owning cell may hold a pointer to an
object from which the cell's own object is reachable through owning cells
(a store `n->next = n`, or `a->next = b; b->next = a` visible in one
function). `BoundaryInvariants` treats it like `SharedOwners`: the boundary
is `unresolved(second-owner)`, with detail "owning cycle", and its class
propagates. **Departure:** a new unresolved reason `owning-cycle` would be
clearer, but the closed list is RFC 0030's and `second-owner` already means
"the ownership invariant of A3 is broken here".

### 9. Engine-independent fixes

#### 9.1 The library table

The rows of §5.4 get the zero-length form. Each changed row gets a unit
test in `unittests/Core/LibrarySpecTest.cpp` (one per row, as RFC 0030 §8
requires).

#### 9.2 The computed-goto layout cliff (RFC 0030 G14)

RFC 0030 G14 measured that Lua's interpreter loses its dispatch
replication when checks supply the edges into a block ending in
`indirectbr`, and that splitting those critical edges restores it
(1.4783 → 1.10). `weavec-cc` runs Clang in process, so it registers the
split through `CodeGenOptions::PassBuilderCallbacks` (no pass plugin, no
`PassPlugin.h`): at `OptimizerLastEP`, a function pass splits every critical
edge into a block that ends in `indirectbr` and has more than 8
predecessors. The callback is registered only when checks are emitted, so
`-fweavec-checks=none` objects stay identical to Clang's (G7).

#### 9.3 Lowered violations

RFC 0030 §3.4 is kept: a definite violation lowered with
`-Wno-error=weavec-<id>` still traps at the site. **Departure:** the
recommendation put to the owner listed "no trap left behind a lowered
error". That would break guarantee (V), which every enforcing build states.
The cause of the libyaml and lz4 traps was the false errors themselves,
which this RFC removes (§5.2, §5.4, §5.7); a baseline and waiver mechanism
for errors a user decides to accept belongs with RFC 0033's baselines
(*Future work*), where a waived site can be recorded as trusted with a
reason rather than silently unguarded.

### 10. Deletions

Listed in §1. The Core and Analysis unit tests of deleted components go
with them; their scenarios that describe behaviour (not representation)
are converted to `test/cases/semantics/` or to `unittests/Analysis/
ObjectEngineTest.cpp` first. The superseded RFCs keep their text.
`docs/architecture.md`, `docs/development.md`, the roadmap and the
docs-site pages that describe the engine are rewritten.

### 11. Tests

#### 11.1 New cases

- `test/cases/soundness/alias-*.c`: the probes of *Motivation* (p3–p7 of
  the analysis and their controls), each with the ASan-reported line
  marked, and correct twins.
- `test/cases/repros/ooc-*.c`: one reduced case per false error and false
  trap of the held-out projects (overlap by `&v[k]`, unknown callee writing
  through `&cmd`, realloc rebasing, back pointer freed with its owner, global
  dangling then overwritten, `fwrite(NULL, 1, 0, f)`, `strncmp(s, NULL,
  0)`, anonymous-member record round trip).
- `test/cases/semantics/objects/`: the domain's cases: strong and weak
  updates, recency in loops, list and tree destructors (proven),
  materialisation, owner-forest cycles (not proven), array summary cells,
  unions, byte-wise copies of pointer structs.

#### 11.2 The held-out corpus

`test/corpus/manifest.json` gains the eleven projects of *Motivation* as
configs marked `"heldOut": true`, pinned by SHA, with their builds and test
suites. Held-out configs are measured and gated (G5, G12, G13) but their
triage entries may only record verdicts, never motivate an engine rule that
names them (H2 already forbids naming a corpus project in `lib/`). sqlite
and mujs are compile-and-time configs (G13); their test suites are not run
by the gate.

#### 11.3 Existing tests

Every case in `test/cases` keeps its markers, except where the object
engine's result is at least as strong and the marker is updated with the
reason in the commit (a `possible` that became `definite` on an exact
alias, an `UNRESOLVED` that became proven with a proof the case's comment
supports). A marker the object engine no longer meets is listed in
`test/cases/KNOWN-DIFFERENCES.md` with its reason; the list is bounded by
gate G2. The lit tests that pin messages stay; those that pin the old
engine's dump format or summary text are rewritten for format 30 and the new
dump.

### 12. Command line

No flag changes except: `--dump-analysis` prints the object engine's states
(objects, cells, symbols and zone at each block exit and site; unstable
format), and `--analysis-stats` reports block transfers, joins,
materialisations, objects and zone sizes.

### 13. Performance

Per function, cost is the number of block transfers times the size of what
changes. The expected costs, which gate G13 bounds:

- the state is shared across edges (§4.8), so a switch with 80 arms costs
  80 references, not 80 copies;
- objects are bounded by allocation sites, locals and k-limited entry
  paths, and the zone by 64 symbols per state;
- the always-add CFG makes a function's transfer linear in its
  subexpressions.

## Annotation surface

None. `resources/include/weavec.h` is untouched.

## Diagnostics

None added or removed. Every id keeps its severity rules (RFC 0030 §3) and
message templates. The unresolved and trust reasons are RFC 0030's closed
lists; none is added (§8 *Departure*).

## Implementation plan

The stages land on branch `rfc0031-object-engine` as checkpoint commits and
ship as one change. The old engine stays buildable beside the new one until
S6, selected by an internal environment variable only the test scripts set,
so every stage can compare both on the same inputs; S6 deletes it.

| Stage | Work | Gate to leave the stage |
| --- | --- | --- |
| **S0 Tests first** | The alias probes, the held-out repros and `semantics/objects/` cases (§11.1), expected to fail on the old engine where it is wrong; the held-out corpus configs (§11.2); a baseline run of every gate on v0.11.0 recorded in the PR. | The new cases fail on the old engine exactly where the analysis says; the held-out configs build with the reference compiler. |
| **S1 Domain** | Core: `PMap`, symbols and values, objects and cells, attributes, the zone, distinctness, join, widening, GC, materialisation, folding (§4); unit tests for each, including I1–I6. | `WeaveCCoreTests` pass under ASan; the domain has no Clang include. |
| **S2 Intraprocedural engine** | `ObjectEngine` over the always-add CFG: expressions, lvalues, loads, stores, casts, locals, globals, literals, conditions, loops, scopes; decisions for Deref, Index, PtrArith, Cast, IntToPtr, Raw and Assume sites with witnesses (§5.1–§5.3); budgets. Library calls only for allocation and release. | The alias probes are caught; `run-cases.py --filter 'pairs/**' --filter 'proofs/**'` passes under the new engine; no proven facet at an ASan-reported line in any case. |
| **S3 Calls** | The full call dispatcher (§5.4): library rows, unknown callees, system APIs, slots, callbacks; format-30 summaries: derivation, instantiation, cases, recursion, incompleteness (§6.1–§6.5). | `evaluation/**` and `recall/**` pass under the new engine. |
| **S4 Temporal completeness** | Releases, families, invalid releases, conflicting borrows, shares (§5.5), boundaries and the owner forest (§5.6, §8), lifetimes (§5.7), leaks (§5.8), initialisation (§5.9), integer operations (§5.10), alias contexts (§6.6), concurrency and `setjmp`. | Every `test/cases` suite passes under the new engine, with the marker changes of §11.3 and within G2. |
| **S5 Records and link** | `UnitExports`, format-29 records, `ProgramDatabase`, the link step and `--whole-program` over the new engine (§7). | `test/WholeProgram` and the multi-unit cases pass; G11 measured. |
| **S6 Delete** | Remove the old engine and everything §1 lists; the hygiene gate's include rule and line budgets; lit and unit test migration (§10, §11.3). | Build with warnings as errors; every unit, lit and case suite passes; H1, H2. |
| **S7 Fixes and cost** | §9.1, §9.2; profiling against G13; docs. | G1–G13 and H1–H3 on the final tree. |

**Fallback.** If S4 cannot reach G1–G4 with the object engine, the change
does not ship with the old engine deleted: the branch is re-planned with the
owner, and the engine-independent fixes (§9) and the new tests (S0) are
offered as a separate change. There is no configuration in which both
engines ship.

## Acceptance gates

Binaries are the `release` preset's `weavec` and `weavec-cc` on the final
tree; the reference compiler is `$WEAVEC_LLVM_PREFIX/bin/clang` (LLVM 23).
Timing gates are measured on an idle machine (load average below 2 at the
start of each run), and the figures in the PR say which machine.

**Soundness**

- **G1.** `scripts/run-cases.py --asan` over every suite: 0 cases in which
  an ASan-reported bug line has its matching facet *proven*; the alias
  probes (§11.1) are each reported (error, warning, trap or non-proven
  row); `--checks verify` over every executable case gives 0
  `weavec.proven` traps. The same `--checks verify` holds for the test
  suites of the eleven original corpus configs and the held-out configs
  that have test suites (`corpus-gate.py --full --checks verify`).

**Parity and recall**

- **G2.** Every `test/cases` suite passes under `run-cases.py` (trap mode,
  `--asan`). Markers changed per §11.3; at most 20 entries in
  `KNOWN-DIFFERENCES.md` beyond the ones it lists today, none of them a
  `BUG` marker in `evaluation/`, `pairs/` or `soundness/`.
- **G3.** RFC 0030 G1–G5, G8 and G12 hold as RFC 0030 states them (its
  evaluation, recall, engine-pin, probe, twin, rewrite-oracle and injection
  gates), with G12's Lua allocator injections included.

**Precision**

- **G4.** Corpus (the eleven original configs): at most 10 definite errors,
  all triaged true (RFC 0030 G9, including zlib's repeated `fclose(stdout)`);
  possible temporal warnings at most 60 in total (RFC 0030 G10, which is
  failing today with 652); 0 traps in the project test suites (RFC 0030
  G11).
- **G5.** Held-out configs: every one builds as shipped with `CC=weavec-cc`
  and runs its test suite with 0 traps, except where a definite error is
  triaged true with source evidence; 0 definite errors triaged false.
- **G6.** Unresolved shares (program ledgers where a whole-program analysis
  exists, unit ledgers otherwise), over the original configs together:
  spatial at most 0.35 (0.42 today) and temporal at most 0.30 (0.49 today);
  over the held-out configs together: temporal at most 0.35 (0.50 today at
  unit level). The per-config values are recorded in `expected.json` as a
  ratchet. RFC 0030 G13's per-file limits (linenoise 0.25, cJSON 0.25, sds
  0.60) hold.

**Codegen and cost**

- **G7.** RFC 0030 G7 (byte-identical objects with `-fweavec-checks=none`,
  at least 100 TUs × 3 configurations).
- **G8.** RFC 0030 G14: Lua bench ≤ 1.10, zlib ≤ 1.10, cJSON ≤ 1.15.
- **G9.** Lua whole-program analysis at most 214 s CPU (RFC 0030 G15), or at
  most the v0.11.0 binary's time on the same machine divided by 1.4,
  whichever is larger.
- **G10.** zlib `make -j8` with `CC=weavec-cc` at most 5.4 s wall on the
  reference machine (RFC 0030 G15).
- **G11.** Over-budget functions at most 1% of analysed functions over the
  original and held-out configs, each listed.
- **G12.** Every held-out config's `weavec-cc` build takes at most 8× the
  reference compiler's user CPU; mujs's single-file `one.c` and sqlite's
  `sqlite3.c` each compile within 15 CPU-minutes and 4 GB.
- **G13.** Per-unit analysis time on the original configs is recorded in
  `expected.json` (the ratchet's `cpuSeconds`) and is not above v0.11.0's
  by more than 10% for any config.

**Hygiene**

- **H1.** CTest in CI as RFC 0030 H1 (Linux Release ≤ 120 s; ASan CTest
  step ≤ 8 minutes).
- **H2.** `scripts/check-hygiene.py`: no includer of `Engine.h` outside
  `ObjectEngine.cpp` and `Engine*.cpp`; no deleted header included; no
  `FunctionDataflow`, `PlaceBuilder`, `AnalysisState`, `MoveTracker` or
  `computeMirrors` in `lib/`, `include/`, `tools/`, `unittests/` or
  `docs/` outside `docs/rfcs/`; the corpus-name and `LibrarySpec`-name rules
  of RFC 0030 H2; `Engine*.{h,cpp}` and the Core domain headers at most the
  budget §1 records; the code under `lib/`, `include/` and `tools/` at most
  80,000 lines (`LibrarySpec.txt` excluded; 86,774 today).
- **H3.** `npm test && npm run build` in `docs/` pass; the architecture,
  development and roadmap pages describe the object engine; RFC 0030 is
  marked Implemented as amended by this RFC once G1–G13 pass, and this RFC
  is marked Implemented.

## Implementation amendments

The stages recorded the decisions below as they were implemented. Each
amends the section it names; where the text above and an amendment
disagree, the amendment holds.

- **Focus objects and dead copies (§4.6).** When a variable points to one
  object on each side of a join and the two differ, the join makes a
  *focus* object for the variable's cell, with the two as candidates, and
  its state is the join of theirs. An entry or focus object that a path
  released and that no root reaches any more is replaced by a *dead copy*
  (same key, `dead` bit), so the released object stops overlapping the
  live one of the next iteration while the summary still sees the release
  (the dead copy keeps the path). Loops that walk owning links (the list
  and tree destructors) stay `unresolved` where a focus object must stand
  for a node its candidates already released: see *Unresolved questions*.
- **Zone cost (§4.4, §13).** A join's closure is computed once
  (Floyd-Warshall over the zone's symbols) with the size limit kept at its
  end; the join itself pairs only the result symbols a side's zone bounds,
  or that several results share on one side. Each symbol's count of
  relational bounds is kept as bounds come and go, so the size limit costs
  nothing while under it; over it, the symbols in the fewest relational
  bounds are demoted until three quarters of the limit remain, so the next
  few do not demote again at once. A bound between two symbols of 2^31 or
  more is not kept: it is what the C types' ranges give (an unknown `int`
  against an unknown `unsigned`), it proves nothing, and keeping it related
  every symbol to every other (a third of Lua's whole-program time).
  Demotion is one pass over the rows, and a join pairs only the result
  symbols some side's stored bounds relate (or that share a symbol on a
  side), not every pair of result symbols. The
  values a block read from earlier blocks
  (§2's cross-block expressions) leave the state on its out-edges, except
  the operands of a conditional or logical operator, whose untaken side's
  last value the operator's join still reads.
- **Sparse zone (§4.4, §13).** The zone stays closed in what it answers,
  but stores a bound between two symbols only where it is tighter than what
  their bounds against zero imply (`x - y <= upper(x) - lower(y)`, below
  2^31), and a query adds that back. Closing over zero had related every
  bounded symbol to every other: each new constant cost a pass over every
  pair of symbols (an unrolled checksum loop took 3 s for 229 block
  transfers). Adding `x - y <= c` now visits only the symbols with a stored
  bound into `x` and out of `y`, and zero, since a path through zero on
  either side is an implied bound; a join takes the relations a side
  stores, those between results that share a symbol, and those between a
  value whose upper bound and one whose lower bound move the same way
  between the sides (two counters stepped together), which are exactly the
  ones the result's own bounds do not imply; its closure runs over zero and
  the symbols in stored relations. Equality compares what the zones answer,
  not what they store. zlib's units took 2.9 s instead of 12 s, with the
  same ledgers.
- **Widening a recursive component (§6.4).** From the fourth round a
  member's summary is its last round's joined with the new one
  (`widenEffects`): the integer results of one case become one interval
  whose bounds that moved are dropped; what an unknown effect on every case
  covers (every object reachable from its path, which the caller forgets)
  is folded into it on both sides before the join (unknown effects, and
  possible releases and moves, below its path; stores into the objects it
  covers); and new objects are numbered by first appearance, so rounds that
  differ only in the join's numbering compare equal. Rounds had replaced
  each summary instead, and mujs's parser, compiler and runtime, one
  component through `js_throw`, never settled: every summary in it was
  incomplete, so every call into it applied the unknown-callee default.
  A component that still does not settle after eight rounds is incomplete
  as before, and a member its last round said never returns may return.
- **Whole-program fixpoints (§7, RFC 0005).** A cyclic component iterates
  on its members' function summaries (the contexts they ask and serve are
  settled afterwards, *Amendment (cross-unit contexts)*). A join of two
  summaries keeps a result alternative once when the two differ only by
  which new object they return and no store names it (the right side's
  objects are renumbered after the left's, so a widening join would
  otherwise add the same allocation every round).
- **Values one side of a join never read (§4.8).** A cell that one side
  of a join wrote and the other never read (a global or an entry object
  the other side did not materialise, or a cell it left unwritten) holds,
  on that other side, its initial or entry value: a release recorded on the
  joined value happened on some paths only (`do { fclose(stdout); } while
  (…)` is a possible double free at the loop head).
- **`va_list` (§5.1).** `va_start` and `va_copy` initialise the `va_list`
  they are given by reference.
- **One-sided zone relations (§4.4).** At a join, a relation only one side
  knows is kept only when it bounds a symbol against a constant, or when
  both of its symbols are one-sided; a relation between a paired symbol and
  a one-sided one would otherwise tighten the paired symbol on the other
  side (a loop proved `data[i]` with `i <= 10`).
- **Carried expression values (§3).** A value an earlier block computed is
  reused only when the expression is not evaluated again in the current
  block, so a loop body re-evaluates its expressions every iteration.
- **Pending cases and exit splitting (§6.2).** An exit whose result carries
  pending cases (RFC 0030 §9.1) is derived once per result class, with the
  cases that class selects applied. Effects join per path *and kind*: an
  effect on some exits is keyed by the result classes of those exits when
  they separate it from the others, by a parameter's zero test where they do
  not (both together when needed: `release *p when result null and param 1
  =0`), and is otherwise possible. A result alternative carries the
  parameter test every exit returning it passes, so a call whose argument
  fails the test never gets it (`xrealloc(p, 0)` returns null). Pending cases
  that the arguments already decide are applied at the call. Releases record
  what the state knew of the releasing function's unmodified integer
  parameters (`paramGuard`), which keys a possible release that no exit
  class separates (lossy), and which pointer locals of the body's outermost
  block, assigned only where they are declared, held a non-null value
  (`nonNullLocals`): an exit that returns such a local is derived per
  result class too, and on its null class the release did not happen
  (`m = malloc(n); if (m && p) free(p); return m;` releases `*p` only
  when the result is non-null).
- **Comparisons as results (§6.2).** An exit whose result is a comparison
  (`return *out != NULL`, `return --*r == 0`) is derived per result class
  too, the class refining the comparison's operands. A store that holds a
  new object on some classes and null on the others (its allocation failed
  there) is one store whose object is *absent* on those classes (format 30
  `absent-on=<classes>`): a caller's test that selects them disowns it, and
  the others make it non-null. Integer values of a store that differ per
  class are joined into their hull. A callee with several result
  alternatives, or a stored value it cannot describe, depends on its inputs
  for the numeric contexts of §6.6.
- **Stores (§6.1, §6.3).** Stores are keyed by result classes like
  effects. At a call every store's place and value are read before any is
  written (the callee's values name its entry state). A keyed store of a new
  object is weak, and the object is *absent* on the other classes: a test of
  the result that selects them disowns it, so a constructor whose failure
  path stored nothing leaks nothing. Integer cells are exported when the
  function wrote them (`SymInfo::entryOf` tells an entry value from a store).
- **Interior releases (§5.5).** A release records the constant offset of
  the released pointer in its object; the summary carries it (`release
  *param0 offset 16`), and the call reports an interior release of a
  caller's allocation there.
- **Memory the analysis knows nothing about (§5.1).** A pointer into the
  `unknown` object (what an unknown callee left in a cell, an integer made a
  pointer) is `unresolved(unknown-callee)`, never proven; cells of memory an
  unknown callee reached hold unknown values whatever the object's kind.
- **Failed allocations (§5.4).** A test that finds an allocation's result
  null disowns the allocation and every object the same call created inside
  it (a constructor's `result->a`).
- **Leaks (§5.8).** Not on a path that ends the program (a block with a
  noreturn call), and not for `main`'s locals, whose frame lasts until exit
  (RFC 0030 §3.4).
- **Parameters (§5.1, RFC 0030 §7.3).** A parameter whose kind §7.3 infers
  unknown (a static function some caller passes a cursor) gets no Single
  extent. `main`'s `argc` is non-negative (C11 5.1.2.2.1).
- **Alias contexts (§6.6).** A context also binds the constant integer
  arguments of the call, so a context run follows the branch the call
  takes.
- **Cross-unit contexts (§6.6, §7).** A call into a function another unit
  defines asks for its summary in the call's context: the arguments it makes
  one object, the constant integers it passes and the integers their objects
  hold, the globals that point into an argument's object, and the callbacks
  it passes, spelled portably (`n=… a=… c=… m=… f=… g=…`, functions and
  globals by portable name: their own for external linkage, `<source>#<name>`
  otherwise). `UnitExports` carries the `contextRequests` a unit makes and
  the `contextEffects` it serves; the program database collects both. The
  defining unit runs each requested context (at most 16 per callee) and
  exports its summary; a context that makes arguments one object also runs
  as an alias context there, reporting what it finds inside the callee. The
  whole-program loop reruns the definers of unserved requests (once per
  request), then the dependents of every unit whose exports that changes,
  until the requests are served (at most 8 rounds). A request stands for as
  long as a call makes it. A unit whose run asks a context a unit of the
  program has yet to serve reports nothing from that run and publishes no
  ledger: it reports once the context is served, or after the last round
  with what is served, so its diagnostics are those of its last run. For
  the same reason `weavec --whole-program` prints each unit's summary line
  from its last run, after all runs. At link, a unit the program view does
  not otherwise re-analyse stays a serving unit: it runs again only if
  another unit asks a context of it. A constant unsigned 64-bit argument
  above `INT64_MAX` is carried by its bits and binds the parameter's
  interval.
  A call through a function value names this unit's functions by
  declaration and another unit's by portable name (`SymInfo`'s
  `foreignFunctions`), applying the latter by their summaries; a slot
  solution's targets this unit does not declare are applied the same way.
  Summaries describe a function value as `function <names>` (format 30
  `fn=`), and a unit that only refers to another unit's function imports it.
  This replaces the old engine's callback and memory context requests (RFCs
  0014 and 0016).
- **Stores past the caller's object (§6.1, §6.3, RFC 0030 §7.5).** A
  summary store the callee makes on every return (not `may`, keyed by no
  case, the callee always returns) whose value is known, to one cell or to
  a range of elements whose bounds are constants at the call, that lies
  outside the one object the argument points into (an exact, constant
  extent) is a spatial violation of the call and an `out-of-bounds` error
  at the argument, in RFC 0030 §7.5's wording ("'f' requires N bytes behind
  'a', which has M bytes", "'f' requires 'a' before its start", measured
  from the object's start); a call §7.5 already found gets no second
  finding. A known value says the callee wrote every element of the range:
  an element it skipped would hold its entry value, which a range of
  unknown values allows. With contexts (§6.6, §7) this recovers what format
  29's extent requirements found at link: the context binds the call's
  constants, so its summary's stores name the exact bytes written. An
  unsigned 64-bit integer above `INT64_MAX` in a summary is carried as its
  interval (format 30 `range=<type>:<lo>-<hi>`).
- **Products that may wrap (§4.4, RFC 0017).** An unsigned 64-bit product
  of a value and a positive constant that may wrap (`n * sizeof *p`)
  records the mathematical product it is the reduction of (`SymInfo`'s
  `unwrapped`); an allocation of it records the same on its extent
  (`Extent::unwrapped`). The bytes are never more than that product, so an
  access past it is a violation of an exact extent (`p[n]` after `p =
  malloc(n * sizeof *p)`), while an access below it is not proven by it.
- **Counted-field invariants (RFC 0030 §7.6, restored).** The cut of RFC
  0030 §7.6 is undone in the object engine, in this form. The candidates
  are `KindInference`'s, after its disqualifications, for records defined in
  the unit's main file (a header's record is other units' to make too, and
  their stores would need §7.6's link verification, A3, which is not
  built). After the unit's summaries and authoritative runs, Houdini runs
  over them: each round assumes, per pointer field, the first standing
  candidate at entry (its field's kind becomes `counted`/`sized` over the
  count field, exact); a checking run of every function that writes a
  candidate's `d` or `f` (or initialises its record) examines, at each call
  and at each exit, the objects of the record it hands out (arguments, and
  every object but its locals at an exit). A candidate holds of an object
  when `d` is null, or points to the start of one object whose exact extent
  is `(f + c) * unit` (unit the element size for `count`, 1 for `bytes`),
  or is the size type's reduction of it (above: what `f * sizeof *d`
  computes in C). Where `d` holds its entry value only the assumed
  candidate is decided, and only when `f` changed; another over a changed
  `f` is refuted. A candidate a stored `d` meets is witnessed. Refuted
  candidates are dropped and the round repeats, at most four times (still
  refuting after that: none stands); a candidate no function witnessed is
  dropped too (it would only replace the kinds' default). The functions
  that read a standing invariant's `d` are analysed once more with it
  (their rows replace the first run's; a diagnostic the first run made
  links to the new rows); summaries stay what callers used. Where the
  count's range cannot rule out the wrap, a load of `d` gets an extent whose
  bytes are a symbol computed as `f * unit` in C, with the product as its
  `unwrapped` bound: `v->items[v->cap]` is a violation, `v->items[i]` under
  `i < v->cap` is checked, not proven.
- **Owner forest (§8).** The `OwningCycle` fact is a `SharedOwners` row
  with a `cycle` flag, whose detail reads "closes an owning cycle".
- **Indirect calls at link (§7, RFC 0005).** An indirect call that neither
  the value nor the program's slot solution resolves reaches, at link and in
  `--whole-program`, every address-taken function of its type: the unit's
  own and the database's joined candidate summary. In a unit alone it keeps
  the unknown-callee default. The unit's call graph orders slot-resolved
  targets before their callers.
- **Variable-length arrays (§5.3, RFC 0017).** A declaration or typedef
  captures its dimensions where it runs (for a pointer to a variable-length
  array, before its initializer, as CodeGen evaluates them), and `sizeof`
  and extents use the captured values. A dimension that may be zero or
  negative gives no storage to prove an access in; one that is for every
  value is the declaration's `invalid-integer-operation`. A byte size that
  may wrap `size_t` is no extent (and no `sizeof` check). A subscript of a
  multi-dimensional array is bounded by its own dimension; an index past it
  is a violation whatever the storage.
- **Summary paths (§7).** An anonymous or positional member is spelled by
  its byte offset (`.#16`), not its index: the engine names cells by
  offset, and an offset decodes against any view of the object.
- **Summary format 30 (§6.1).** `EffectsIO` spells the summary as one item
  per line: `returns`, `incomplete`, `effect <kind> <path> when=<case>`
  with `family=`, `may`, `lossy`, `offset=`, `elements=`; `store <path>
  when=<case> [may] [elements=...] :: <value>`; `result classes=<c,...>
  [param=<i>=0|<i>!=0] :: <value>`; `nonnull-on`, `reads`, `writes`. Paths
  are `p<i>`, `g<i>` or `r` followed by `*`, `.<name>` and `[<name>]`.
  Every field round-trips. The database joins several definitions of a name
  (and the candidates of a type) with `joinEffects`, and renumbers globals
  by name; an effect through a global the importing unit does not declare
  makes the summary incomplete there (RFC 0030 §5.5).
- **The unit record (§7).** Format 29 carries each function's summary in
  the field `effects`; `summary`, `contexts`, `unknownIndirect`,
  `sizedFields`, `sizedFieldLoads` and `interfaces` are gone. The link
  step's declaration verification reads the format-30 summary.
- **Deletions (§1).** `IntegerSupport.h` is kept as the engine's
  `EngineIntegers.h` (target integer types for the engine), and the checked
  arithmetic of `CheckedInteger` moved into `Integer.cpp`; the signature
  annotations of `Summaries.h` moved to `Annotations.h`. Everything else §1
  lists is deleted, with `SummarySteps` and `Place` replaced by `Path.h`.
- **The dispatch split (§9.2).** Implemented as `SplitDispatchEdges`
  (`lib/Frontend/DispatchEdges.cpp`), registered for optimised builds whose
  checks are emitted.
- **Pointer comparisons (§6.1, RFC 0014).** A state remembers the
  comparisons of two pointer values its path decided (`p == q`, `p != q`),
  and a join keeps those both sides decided alike. A release records the
  comparisons of the function's unmodified pointer parameters on its path,
  and the summary keys the effect by one of them: `<case>` gains `and param
  <i> ==|!= param <j>` (spelled `<classes>:<param test>:<i>==<j>` in format
  30). A call skips the effect when its arguments decide the comparison the
  other way (a remembered comparison, a null argument against a non-null
  one, or targets that cannot overlap by §4.5), and takes it as possible
  when they do not decide it. This is RFC 0014's `release_same(p, q)` guard
  (`evaluation/rfc0014-pointer-guard-good.c`).
- **Call results (§4.5 D4).** The result of an unknown callee may be what
  that callee could reach, which excludes an allocation of this activation
  that has not escaped: every call into unknown code marks what it can reach
  escaped, and an escape is never undone. Its temporal default names the
  reason `callback` for an indirect call through a slot with no known
  target, for what the arguments reach as for the call (RFC 0030 §9.3).
- **Entry tests (§6.1, §6.2, lazy initialisation).** A state records the
  zero tests its path made of values cells held at entry (`if (!g)`, `if
  (b->buf == NULL)`), by cell, and a join keeps those both sides made alike.
  A join where one side stored a cell under a test and the other took the
  opposite test and left the entry value marks the cell as stored exactly
  where the test holds; an allocation one side made under a test the other
  side refuted exists exactly where it holds. A summary keys an effect or a
  store by such a test, `<case>` gaining `and entry <path> =0|!=0`
  (spelled `:E<path>=0` in format 30), and a call applies it by the value
  it holds at `<path>`: skipped when that value decides the test the other
  way, possible when it does not decide it, where the new object of such a
  store exists exactly where the caller's own entry value tests as the
  callee's did. An entry object exists where its holder's entry value is
  not null; a store through a pointer to two objects of which each path has
  exactly one (the entry object and the object made where that value was
  null) is strong, and a load through it reads one value while both cells
  are unchanged, so a test of it holds for the next load. This is `if (!g)
  g = malloc(…)` (`HeapState.LazyPublicationKeepsItsEntryGuardAcrossTheCall`).
- **Leaks on some paths (§5.8).** A join keeps an owned allocation that only
  one side made; when a local's value tests zero on one side and not on the
  other, the object exists where it tests as on the side that made it, and a
  leak check where the path decided otherwise skips it (`if (c) p =
  malloc(n); … if (!c) return;`). An object one path leaves live that the
  join ahead no longer shows live (it was released on the other paths) is
  leaked on that path, reported at the branch that takes it, when the edge
  reaches the exit through blocks with no statements (`switch` without a
  `default`, `if (c) free(p);` at the end of a body); lifetimes that end in
  a block with no statements are checked there, and a leak found there is
  reported at the end of the scope.
- **Stores that keep the entry value possible (§6.2).** When a cell at an
  exit holds either its entry value or a new one (the paths that stored
  joined those that did not before the `return`), the summary stores the new
  value as a possible store, which the caller applies weakly, instead of
  describing the join as `unknown`. A `vec_push` that may `realloc` its
  buffer thus leaves the caller's buffer pointer either the old buffer
  (possibly moved) or the new one, and an element pointer kept across the
  call is reported (`soundness/44_vector_element_ptr_bug.c`).
- **Calls that do not return (§5, RFC 0030 §2.2).** A call to a function
  declared `noreturn` (`_Noreturn`, `__attribute__((noreturn))`) ends the
  path whatever its summary says, as a call whose summary never returns
  does: a parser's error routine that `longjmp`s out of a half-built frame
  joins nothing at the exit.
- **Unknown effects on entry objects (§6.3, §4.6).** The paths of a
  summary's `unknown` effects name the callee's entry state, so the caller
  resolves every one before applying any (applying `unknown *p` first
  would forget the cell `*p->next` is read through). An entry object an
  unknown callee may have released and that no root reaches at the exit is
  a dead copy; while no live object has its key it still names the entry
  object, and the bytes the callee rewrote are an `unknown` store at its
  path. Without the store, a caller kept the old contents of a record whose
  fields the callee reset before passing it on (Lua's `close_func` after
  `leaveblock`).
  An `unknown` effect on `*p` reaches, in the caller, every object the
  caller's memory reaches from `*p`, as a direct call to unknown code does:
  the callee's summary names only the objects it materialised, and the code
  it could not see had the rest (`semantics/temporal/unknown-effect-reaches-frame.c`).
- **Loop heads and widening (§4.8).** Widening starts after a loop head's
  first two joins and uses as thresholds the constants the loop's own
  conditions test (the conditions of the blocks of its natural loop, each
  constant with its neighbours, and -1, 0 and 1) until the head has changed
  eight times, then drops a growing bound. (The function's every constant
  had been the thresholds: a counter no condition bounds by a constant,
  `while (l++ < width)` in printf's `_vsnprintf`, climbed one small
  integer a round, and again each time an enclosing loop entered it anew.) A function is
  over budget when a loop head changes more than 64 times (a join that adds
  nothing, such as another back edge of the same round, is no change), or
  is joined more than 256 times; a head many edges reach (an interpreter's
  dispatch, an edge per opcode) may change twice and be joined 64 times per
  edge, and a loop entered again from outside it (with what a round of an
  enclosing loop changed) counts afresh. It is also over budget when a
  join leaves a block's state with more than 2,048 symbols, each of which
  every later join pairs: sqlite's `sqlite3VdbeExec` converged once its
  dispatch settled (below) with states of 4,677 symbols, in 90 to 180 s a
  run, where the next largest state in the corpus holds under 1,000. A local whose scope does not hold
  a block, that nothing after it names and whose address is not taken,
  leaves that block's state (liveness now ends at a declaration and sees
  the references an element holds): the opcode bodies' locals had piled up
  at Lua's dispatch head, which never settled; `luaV_execute` is no longer
  over budget. The worklist takes blocks in reverse post-order, except that
  a loop head a back edge changed waits until no block of its natural loop
  is pending, so it takes in every back edge of a round before the next
  round starts (Bourdoncle's order for reducible loops): a computed-goto
  dispatch, which every opcode's end reaches, had restarted the round at
  each of its 80 back edges, re-running `luaV_execute` 244 times through
  the dispatch; it now runs about as many rounds as the head changes. A
  bound against zero stops at the next program constant; a bound between
  two symbols stops only at -1, 0 or 1 (a relation climbing one constant a
  round kept zlib's `crc32_z` from converging). Result symbols are numbered
  in the same order on every join: the objects' symbols first, then the
  values of expressions one side still holds, so two states that differ
  only in such a value still compare equal. A loop head's new state that
  differs from the old only in how its symbols are numbered (a join numbers
  its results in pairing order) is no change: the two are compared under a
  bijection of their symbols, conservatively (a field the comparison does
  not follow must be equal as it is).
- **Bytes rewritten in part (§4.2, §6.3).** Forgetting a bounded range of
  an object's bytes (a member copied over, a union written by a callee, a
  `read` into part of a record) forgets that range only: the object keeps
  `forgotten` ranges beside the whole-object `havocked`, and an unwritten
  cell reads as unknown only inside one. A range forgotten on some paths
  only (`mayForgotten`, and a join of a path that forgot it with one that
  did not) reads as the cell's value otherwise merged with an unknown
  one. Summaries say which bytes of an entry object were rewritten
  (`store *p bytes 24..32 := unknown`, format 30's `bytes=`), possibly
  (`may`) unless every exit rewrote them on every path; the caller forgets
  exactly those bytes, before the summary's other stores, and on a
  possible rewrite keeps each cell's value beside an unknown one, so a
  pointer it left there still reaches its object
  (`semantics/temporal/possible-rewrite-stays-possible.c`).
- **`realloc` of zero bytes (RFC 0030 §8.2, §11).** RFC 0030 releases the
  argument of `realloc` on the null class when the size may be zero,
  because glibc's `realloc(p, 0)` frees `p` and returns null. A build with
  zero-initialisation (the default, and what the analysis models unless
  `-fno-weavec-zero-init` or `--no-zero-init` says otherwise) calls
  `realloc` through `__weavec_realloc_zero`, which asks for one byte
  instead of none, so there the null class always keeps the argument and
  the release does not arise. Without zero-initialisation RFC 0030's rule
  stands. A buffer wrapper's failure path (`if (!new) return;` after
  `realloc(ab->b, ab->len + len)`) thus keeps a live buffer, and no
  boundary sees a pointer that may be gone (linenoise's `abAppend`).
- **Which facets a broken boundary reaches (RFC 0030 §9.4 point 3).** A
  proven temporal facet rests on the entry assumption of the places its
  pointer's value was loaded from at entry: the engine records, for each
  value, the cells whose entry values it was computed from (a load, a
  copy, pointer arithmetic, a cast, a merge of paths; `SymInfo::
  entryOrigins`), and publishes their classes with the proof
  (`LedgerAdapter::reliesOn`). The propagation downgrades a proven facet
  whose operand is spelled from a broken class, as before, or whose value
  came from one. A broken field class `struct s.buf` no longer breaks the
  record class `struct s`: that rule reached `b[0]` after `char *b =
  o->buf` only by also downgrading every `o->len` in the program, a third of
  zlib's temporal facets for one `ZFREE(strm, strm->state)` whose own fields
  held the buffers just freed. Probe 02d's bug site is reached through the
  field class directly; `soundness/02g_uaf_heap_field_copied_helper_bug.c`
  covers the copies.
- **The C library's own globals (RFC 0030 §5.1).** A call to code the
  analysis does not see may do anything to what the program's globals
  reach, and clears the globals' cells: their values are unknown after it.
  Globals declared in system headers (`stdout`, `stdin`, `stderr`) are the
  exception: the objects they point to are still exposed (the stream may be
  closed), but the name keeps its value, so the stream a later round reads
  from `stdout` is the one an earlier round closed. Without it, the reload
  materialised a fresh copy of the entry object and minigzip's repeated
  `fclose(stdout)` (RFC 0030 G9) went unreported
  (`semantics/library/stdout-closed-twice.c`).
- **Entry objects a function zero-fills (§6.2).** `memset(p, 0, sizeof *p)`
  on a parameter's record leaves an object whose unwritten cells read as
  zero, which a summary did not describe, so the caller kept the record's
  old contents (a pointer the function had just freed before clearing it,
  cJSON's test `reset`). The summary now stores zero (null for a pointer)
  into each scalar member of the record's type that the function wrote no
  other value into.
- **Stores into part of a scalar.** A summary names a store at a byte
  offset inside a scalar member (`d.#4`, the high word of a `double` a
  union also holds as integers) by that offset; the caller no longer takes
  the member's type for the store's, which made the store run past the
  object (jansson's `dtoa`; `semantics/objects/union-high-word-store.c`).
  Past the end of a scalar the offset still names another element of its
  type (`b[7]` through a `char *`).
- **A second release at a call (RFC 0030 §3.1).** A call whose callee may
  release a pointer argument the caller freed already is a double free when
  the callee reads and writes nothing through it (its summary's `reads`
  and `writes`, which the engine now fills: the entry objects whose cells
  it loaded or stored); a callee that reads it first makes that read the
  first invalid operation, a use after free, as before. The callee is every
  function the call may reach (a hook slot's `internal_free` and `free`):
  some that may release it make the release possible, all that do make it
  certain (`semantics/temporal/free-then-guarded-wrapper.c`,
  `semantics/slots/hook-freed-twice.c`).
- **Releases at an unknown offset (§6.1, RFC 0008).** A release whose
  pointer is at an offset the callee cannot name into the object its
  parameter points into (`free(s - hdr_size(s[-1]))`, hiredis's `sdsfree`),
  or at different offsets on different exits, was summarised at offset 0,
  so the caller released its argument itself and reported a definite (and
  false) `invalid-release` when that pointed past the object's start. Such a
  release is written `offset=?` in format 30 (`PathEffect::anyOffset`); the
  caller releases the same objects at an offset it does not know either,
  which leaves the release's validity possible, not definite
  (`semantics/library/release-at-unknown-offset.c`).
- **A new result the callee points into.** The contents of a fresh result
  are described only when the result is the new object's start; one that
  points into it (an `sds` string after its header) was still summarised
  as zero-filled, so the caller read the header the callee wrote as zeros.
  Its cells now read as unknown in the caller, as those of a new object
  below another one's already did.
- **Fields of a member array's records (§4.9, *Summaries*).** A position
  is kept modulo its stride, so the second field of `m->sub[i]` may be
  counted from the start of `m->sub[i + 1]`; a summary names its store as
  `param1->sub[*].ep` with the element range shifted back by one, where it
  used to drop it (mujs's `Resub`). A store's extent past the caller's
  object is measured to the end of the last element's cell, not the whole
  stride (`semantics/objects/member-array-record-fields.c`).
- **A path value and null (§6.1).** A summary describes a pointer the
  function joined with a null it stored (`sub->sub[i].sp = NULL` through a
  pointer to the caller's record or a local one) as the entry path's value
  `maybe-null`; a value only maybe-null at entry, the path's value alone. A
  join that takes in a null pointer beside another value marks the result
  (`SymInfo::nullJoined`), and only such a value is described `maybe-null`.
  The call now applies both what a path value's description says: its
  `offset`, which a store used to drop (`s + 1` stored as `s`), and its
  `maybe-null`, a join of the value with null, which it used to ignore (the
  caller's unassigned elements stayed unassigned, a definite and false
  `use-of-uninitialized`). A path result keeps the caller's nullness of the
  value, where the callee's view of an entry value's nullness used to make
  it maybe-null (`semantics/objects/optional-out-elements.c`). A cell
  that holds its own entry value or a null the function stored (`if
  (errorp) *errorp = NULL`) is a store, where "still holding its entry
  value" made it none (`semantics/objects/guarded-null-out.c`).
- **Some elements, weakly, at their position (§4.9).** A store or release
  a call applies to "some elements" without a range now does so at the
  elements' position in the object (`sub[*].sp`, 8 of each 16 bytes), where
  it used to use position 0 (the field `ep`'s), reading what the elements
  held first; at an offset the call does not know, the object's cells are
  forgotten.
- **Locals after `setjmp` (RFC 0030 §5.4).** In a function that calls a
  returns-twice function, a `longjmp` may return to it after stores the
  path through the first return does not see, so a local no path assigned
  reads as possibly unassigned there, not certainly: no definite
  `use-of-uninitialized` (mujs's `js_try` handlers, which free what the
  protected code allocated; `semantics/ledger/setjmp-assigned-after.c`).
- **Raw through some of a call's functions (RFC 0004, RFC 0030 §9.3).** A
  call through a hook whose functions return raw pointers from some and
  tracked ones from others (a test's allocator returning an integer as a
  pointer, installed in the slot the library allocates through) has a
  raw result, but raw only through some of them (`SymInfo::rawSome`, the
  summary's `raw-some`). Using, releasing or passing it outside an unsafe
  region is no definite `unsafe-operation`, which made every allocation in
  the library an error; its facets are `unresolved(raw-cast)`. Loads
  through it and joins keep the mark; only such a call makes it, so a raw
  value a join in the function makes is still an error (RFC 0004's
  "rawness joins as may be raw";
  `semantics/slots/hook-raw-on-some-targets.c`).
- **Units that do not parse.** A unit with an error that stops its
  compilation is not analysed (Clang's own analyzer does the same): its
  records may have no layout, and the analysis crashed on one.
- **Locals an expression makes.** Their objects (a compound literal, a
  call's record result) are keyed by the expression, not by a
  declaration (`ObjectKey::expression`); code that asked every local for
  its variable read an expression as a declaration.
- **Joins inside an expression (§5.4, §4.8).** A call through a hook
  applies each function it may reach to a copy of the state and joins
  them, in the middle of an expression whose other operands the caller
  has already evaluated (`0 != settings->on_begin(p)`). A join numbers its
  result's symbols afresh, so such an operand could name another value
  afterwards, even the comparison's own result, whose condition then named
  itself (http-parser's whole program recursed until the stack ran out).
  Such a join keeps the number of every symbol both copies still hold
  from the state before the call, and numbers the rest past it
  (`Heap::join`'s `keepBelow`); refining a condition also stops at one it
  is already refining.
- **Globals that unknown code may write (RFC 0030 §5.1, §5.5).** A call of
  code the analysis does not see forgets what the globals in the state
  hold, but a summary of a function that made such a call said nothing of
  the globals it never named, so its callers kept theirs (http-parser's
  test counts messages in callbacks the parser runs, and kept the count at
  zero: a definite, false `out-of-bounds`). A run that applies an unknown
  callee, or a summary that is incomplete or says this, now marks its
  summary `unknown-globals` (format 30, `FunctionEffects::unknownGlobals`),
  and a call of such a summary, or of an incomplete one, forgets what the
  caller's globals hold as an unknown call does
  (`semantics/boundary/global-written-by-unknown-code.c`). So does a
  summary imported into a unit that cannot name a global it stores into
  (another unit's `static` counter a callback increments), where the store
  used to be dropped without a trace. The flag names no global: a caller
  forgets all of its own, which costs http-parser's test a third of its
  proven facets (*Unresolved questions*).
- **Contexts served at link (§7 *Amendment (cross-unit contexts)*).** A
  unit serves another's contexts only of a callee whose own run was small,
  as it builds its own (`Transfer::contextSummary`): a context run of a
  large function costs what its own run did, for each context
  (http-parser's `http_parser_execute`, whose sixteen contexts took more
  than twenty minutes).
- **Gate status (S7, measured 2026-09-30, final tree).** Measured with
  `corpus-gate.py --full --held-out`, the case runner and the scripts the
  gates name, on the reference machine.
  - *Pass.* G1 (481 of 481 cases under `--asan` and under `--checks
    verify`; no proven facet trapped in any corpus or held-out test suite
    built in verify mode); G2 (481 of 481); G3 (evaluation 76 of 76, pairs
    24 of 24, recall 67 of 67 pins, engine 143 of 143 pins, soundness 137
    of 137 with 75 probes reported, injections 30 of 31); G4's errors (0
    definite errors on the original configs, zlib's `fclose(stdout)`
    reported) and traps (0 in every test suite); G5 (no definite error
    triaged false; every held-out project builds and passes its tests with
    no trap, except sqlite, whose build stops at the three definite
    `unsafe-operation` errors on pointers made from integers, triaged true
    under RFC 0004); G7 (153 units, identical in all three
    configurations); G8 (Lua 1.08, zlib 1.01, cJSON 1.15); G9 (Lua's whole
    program, 114 s); G10 (zlib `make -j8`, 1.7 s); G11 (4 of 9,588
    functions over budget); G12's single units (`sqlite3.c` 486 s and
    2.3 GB, `mujs/one.c` 12 s); G13 (in retired instructions against
    v0.11.0's binary, Lua 1.02, zlib 0.67, jansson 0.97, and the smaller
    configs within the gate's second of slack: cJSON 1.27, printf 1.49);
    H2; H3's build and pages.
  - *Open.* **G4's possible temporal warnings**: 65, limit 60, all
    triaged; 35 are cJSON's test files, which reuse one global item across
    parses, so the child a reset released and the one parsed next are both
    the unknown object (the values a summary cannot describe share it). A
    per-call object for such values was tried and added leak reports.
    **G6**: over the original configs spatial 0.42 and temporal 0.35
    (limits 0.35 and 0.30); over the held-out configs temporal 0.55 on the
    unit ledgers and 0.50 on the program ledgers where they exist (limit
    0.35). 81% of the unresolved temporal facets of the original configs are
    `unknown-callee`, 5,897 of 7,249 of them Lua's: `luaD_precall` calls a
    `lua_CFunction` through a slot of about two hundred targets, past
    `MaxCallTargets`, and any of them may run the collector, which may
    release whatever the state reaches (without Lua the share is 0.23).
    95% of the unresolved spatial facets are `unknown-extent`: pointers
    loaded from fields whose kind inference proves no element count, and
    strings walked by pointer. sqlite holds 48,370 of the held-out
    configs' 86,601 temporal facets, at 0.60: its allocator, mutexes and
    VFS are hooks in `sqlite3GlobalConfig`, open slots `sqlite3_config`
    sets. **G12's build ratios**: bzip2 11.0, http-parser 31.6, lz4 13.0,
    mujs 46.7 and utf8proc 9.2 against the reference compiler, limit 8
    (hiredis 1.9, inih 4.8, libyaml 2.9, miniz 2.9, tinyexpr 7.6): the
    link step analyses the program again for every executable it links,
    and the analysis of one unit alone exceeds the bound for http-parser
    (`http_parser.c`, 3.5 s against a whole build of 0.6 s) and mujs.
    **H1** is measured by CI. **G5's traps** depend on the fuzzer's seed in
    one place: lz4's `frametest` makes corrupt frames, and on one of them
    `LZ4_memcpy_using_offset_base` copies with an offset of 0,
    `memcpy(dst, dst, 2)`, whose operands overlap, which C leaves undefined
    (lz4's own comment says an offset of 0 happens in testing). The
    disjointness requirement there is checked, not proven, and its check
    trapped in the verify-mode run; the trap-mode run's seed did not reach
    it. It is a true report: a trap of the kind G5's exception names for
    definite errors, which this RFC records as true here.
  - None of the open gates is a matter of tuning: G6 needs the modelling
    of hooks and of extents that RFC 0032 (runtime enforcement) plans for
    what stays unresolved, G12 a link step that reuses what the compile
    step decided (RFC 0033), and G4's count values a summary cannot
    describe that are not one object.
- **Gates carried forward (G4's count, G6, G12's build ratios; decided by
  the owner on 2026-09-30).** The three targets above were set before the
  engine existed and are not met by it; what each needs is design this RFC
  does not contain, so they leave this RFC's acceptance and become *Future
  work*, and what was measured becomes a ratchet that no later change may
  worsen:
  - **G4's possible temporal warnings**: at most 65 (was 60), all triaged
    (`gates.G10` of the corpus manifest; RFC 0030 G10 is amended alike).
  - **G6**: the unresolved shares recorded per config in `expected.json`
    are the ratchet; over the held-out configs together the temporal share
    is at most 0.50 (was 0.35), on the program ledgers where a
    whole-program analysis exists. The targets of 0.35 and 0.30 over the
    original configs and 0.35 over the held-out ones go to RFC 0032.
  - **G12's build ratios**: measured and reported by the gate, not
    limited; the single-unit limits (15 CPU-minutes and 4 GB for
    `sqlite3.c` and `mujs/one.c`) stay. The bound of 8 goes to RFC 0033.
  G5 counts a trap of a checked facet that reports a defect the source
  shows as it counts a definite error triaged true. With these, G1–G13 and
  H2–H3 hold on the final tree (H1 is CI's), and this RFC and RFC 0030 are
  Implemented.
- **Ranges a join keeps (§4.2 *Amendment (arrays)*).** A join keeps both
  sides' ranges that match nothing on the other side, and the limit of four
  per position was kept only where an element was loaded, so a loop head
  compounded them (sqlite's `isDupColumn` context: ten thousand ranges of
  one object, eight gigabytes). A join's result now keeps each position's
  newest four, and evictions go in one pass (`Heap::trimSegments`,
  `evictSegments`); `sqlite3.c` takes 850 s and 1.4 GB.
- **Owning slots through a function (RFC 0030 §9.4).** A slot is owning when
  some function releases a value loaded from it; the unit also counts a
  value it hands to one of its functions that releases that parameter, to a
  fixpoint (`free_tree(t->left)` makes `left` owning), where only a library
  release or an ownership contract counted.
- **Owners below the k-limit (§4.5 D3, §4.6).** A summary object folded
  from an owning slot of an entry object stands for objects owned below it,
  so it is one object per such owner and owned by it, and a load through it
  by an owning slot stays below that owner; reached any other way it is
  owned by nothing, as before. A recursive tree destructor is proven
  (`semantics/objects/tree-destructor.c`).
- **Derivation at loads (§4.5 D6).** A pointer loaded through `p->f`, where
  `f` is an owning slot, records `p` among its derivation, which D6 reads:
  it was never set before.
- **Nullness through arithmetic.** Arithmetic on null is undefined, so a
  pointer made from another by arithmetic is null exactly when that one is
  (`HeapState::nullFollows`, from each result to the pointer its chain
  started from): a dereference or a test that decides one non-null decides
  the others, even when the value the chain started from has no pointer
  type (an untyped union cell, Lua's `ci->u.l.savedpc`). `*(q++)` decides
  the incremented `q` too, so each fetch of an interpreter's `pc` after the
  first is proven non-null; Lua's `luaV_execute` had a null check at every
  opcode's fetch. The links are not kept by joins, where each side's
  values are paired anew. A pointer made by adding an amount that is not
  zero (a constant, or a value whose bounds exclude zero) is non-null: were
  the pointer null the arithmetic would be undefined, and its result is not
  the null pointer, so a check of it could not fail (Lua's `base =
  ci->func.p + 1`, from which every register pointer is made).
- **Rounds of a recursive component (§6.4).** A member runs again only
  when a summary it calls within the component changed since its last run
  (its summary is a function of theirs); the component has settled when no
  member waits. The rounds that widen and the limit of eight count as
  before. sqlite's code generator is one component of several hundred
  functions. A member's summary that says unknown code may write any
  global (`unknown-globals`) names no effect on one global inside its
  component, beyond releases and moves: its callers forget every global
  anyway (the C library's own excepted, whose effects stay), and those
  effects climbed one call edge a round, so the component of 1,392
  functions never settled in eight rounds (1,388 changed in the first,
  265 still in the eighth).
- **Expression values a block carries (§2).** A block's state carries the
  values of expressions a later block reads (`HeapState::exprs`) only while
  some path from it reads them before evaluating them again: a backward
  liveness over the CFG, whose reads are the expressions in a block's
  elements, its expression terminator and its condition, and whose kills are
  the expressions it evaluates and, for a conditional's value, its branch
  and the blocks of its arms. A conditional reads neither its condition nor
  its arms (its value is the one its arm recorded under it), so they are no
  longer carried for it. Its arm is found with its parentheses stripped, as
  the CFG evaluates it: an arm in parentheses (`c ? (x = i, 5) : 0`, Lua's
  `tonumberns`) had recorded nothing, and the operator's value on that path
  was the other arm's from an earlier iteration, a false proof
  (`soundness/conditional-arm-in-parens_bug.c`); the branch now forgets the
  operator's value, so an arm that records none reads as unknown. Lua's
  dispatch head carried about 150 such values, each a symbol every join
  paired.
- **Line budget (§1, H2).** Measured at the end of S7: 19,654 lines in
  `lib/Analysis/Engine*.{h,cpp}` (13,795 after S6: the S7 fixes above) and
  64,367 under `lib/`, `include/` and `tools/` (`LibrarySpec.txt` excluded;
  56,610 after S6), within this RFC's ceiling of 80,000. The hygiene gate's
  budgets become 20,000 and 64,500, the measurement rounded up to a multiple
  of 500; they are ratchets, raised only by an amendment that records a new
  measurement. Measured again once the branch was formatted with
  clang-format 23 (the CI's; the S0–S7 checkpoints were not) and the last
  S7 fixes above: 19,793 and 64,609 lines, so the second budget becomes
  65,000; with the sparse zone and recursive widening, 19,944 and 65,120,
  so 65,500; with the ranges a join trims, owning slots through functions
  and owners below the k-limit, 20,040 and 65,276, so the engine's budget
  becomes 20,500; with the iteration order, the carried values' liveness
  and the cost bounds of this round, 20,373 and 65,872, so the second
  becomes 66,000; made clean under CI's clang-tidy (braces, split
  declarations, suppressions with their reasons), 20,535 and 66,102, so
  the budgets become 21,000 and 66,500.

## Drawbacks

- **A rewrite loses what nobody wrote down.** `FunctionDataflow` encodes
  hundreds of decisions made against cases, many of them described only in
  commit history. The test tree (410 cases, the lit pins, the corpus
  ratchet) is the specification this RFC relies on, and whatever it does not
  pin can regress silently in precision (never in soundness, by §4.7).
- **Materialisation and folding are the subtle part.** A bug there is a
  false proof. I1–I6 have unit tests, verify mode monitors proven facets,
  and the ASan oracle runs over every case; the risk is still the largest in
  the change.
- **The owner forest is a new assumption.** Code with owning cycles gets
  unresolved facets or possible findings where they are visible, and proofs
  that rest on A3 where they are not.
- **Precision can move in both directions.** Array elements, which the old
  engine told apart by selectors (RFC 0015), are summary cells here beyond
  64 constant indices; some old proofs about element-wise release loops may
  become unresolved. They are listed if they do.
- **The change is very large.** About 25–35K authored lines and 40K deleted,
  most of it the old engine and its tests.

## Alternatives

- **Patch `FunctionDataflow`.** A lazy fallback at lookup (check the
  mirrors of ancestors) fixes the five probes. It does not fix the class:
  every tracker keyed by path has its own copy of the mirroring rules, and
  the next false proof is in whichever one was not patched. It also leaves
  the cost and the false errors of *Motivation*.
- **Runtime enforcement first** (a bounds-and-liveness runtime, RFC 0032).
  It turns unresolved facets into checks, but it leaves proven facets
  unchecked, so it builds enforcement on proofs that can be wrong. It is
  planned after this RFC, on an engine whose proofs it can trust.
- **An explicit IR.** Cleaner semantics, but a second representation beside
  the AST that sites, witnesses and rewrites name (§2 *Departure*).
- **Clang's FlowSensitive framework.** It has an object model and a SAT
  solver, but it has no heap abstraction (a join of two pointees makes a
  fresh location, which loses may-alias facts), no summaries and no
  interprocedural story; it is built for C++ value types.
- **Full separation-logic shape analysis** (list segments, abduction). The
  most precise option for recursive structures, and the most expensive and
  least predictable; the owner forest plus materialisation covers the
  destructor and traversal shapes the corpus has, at a fraction of the
  cost.

## Prior art

- **IKOS** (NASA): cells per memory location, offset and size variables per
  allocation site, relational numeric domains over them. The cell/offset/
  size split of §4.2–§4.4 follows it.
- **Infer Pulse** (Le et al., OOPSLA 2022): abstract addresses with
  attributes (allocated, invalid), memory as edges between addresses, and
  summaries over formals' access paths, instantiated at call sites. The
  symbol-as-value design and the summary instantiation of §6.3 follow it;
  Pulse is under-approximate, and §4.5's distinctness rules are what make
  the same representation over-approximate.
- **Recency abstraction** (Balakrishnan and Reps, SAS 2006): the `heap
  recent`/`heap old` split that allows strong updates on the most recent
  allocation.
- **k-limiting** (Jones and Muchnick, 1979) and **materialisation** (Sagiv,
  Reps and Wilhelm, TVLA): the entry-path limit and focus of §4.6.
- **Zones** (Miné, 2001): the difference-bound domain of §4.4.
- **Rust ownership**: `Box` trees are the owner forest of A3; the forest is
  what makes a destructor loop provable without shape invariants.
- **CCured, Checked C, `-fbounds-safety`**: unchanged from RFC 0030; the
  kinds and checks this engine feeds are theirs.

## Unresolved questions

- **The zone's size limit.** 64 symbols per state is a guess; S7 measures
  it against G9 and G13 and records the value.
- **The k-limit.** "A step repeated more than twice, or longer than 6" is
  chosen to fold list and tree recursion quickly; S4 measures the temporal
  share (G6) under 1, 2 and 3 repetitions and records the choice.
- **Array cells.** 64 constant-index cells per object may be too many for
  large tables (Lua's opcode tables); S7 may lower it.
- **What the old engine proved that this one does not.** Only running the
  cases and the corpus will say; §11.3 and G2 bound it.
- **Alias contexts** may prove unnecessary once distinctness is explicit;
  if the cases that motivated RFC 0016 pass without them, §6.6 is cut and
  this RFC amended.
- **Loops over owning links.** A loop that frees the node it stands on
  and moves to the node loaded from its owning slot (`while (p) { next =
  p->next; free(p); p = next; }`) needs, at the loop head, one object that
  stands for "the rest of the list" and is disjoint from every node already
  released. The focus objects of §4.6 give one per join, but a release of the
  focus weakens its candidates, so the released nodes stay possible
  overlaps and the list destructors of `semantics/objects/` stay
  `unresolved(may-alias-released)` (the tree destructor is proven, D3 below
  the k-limit). A value-level validity fact (a pointer that points to no
  object released since it was obtained, invalidated at each release) was
  tried and does not suffice: the focus object's cells join the released
  node's stale values with the live one's. A list-segment abstraction (the
  candidates of a focus that the other side released and no root reaches are
  absorbed into it) is the candidate fix.
- **Which globals unknown code writes.** `unknown-globals` makes a caller
  forget every global it holds. A summary that named the globals another
  unit's code writes (by their portable names, as contexts do) would keep
  the rest, but needs a unit to name globals it does not declare.

## Future work

- **RFC 0032: runtime enforcement.** An ABI-compatible allocator with O(1)
  object lookup, so that `unresolved(unknown-extent)` facets become checks
  against the runtime extent and unresolved temporal facets become liveness
  checks (with a quarantine), on top of proofs this engine makes sound.
- **RFC 0033: adoption.** Records in object sections (archives, shared
  libraries, ccache, LTO), fingerprinted baselines and waivers (the place
  for accepting a definite error without trapping, §9.3), `weavec.toml`,
  `weavec suggest --apply`, relocatable installs.
- **The precision targets of G6** (carried from this RFC: unresolved shares
  of 0.35 spatial and 0.30 temporal over the original configs, 0.35 temporal
  over the held-out ones; 0.42, 0.35 and 0.50 at its close). Three things
  hold them: slots with more targets than a call can name (Lua's
  `lua_CFunction`, any of which may run the collector), hooks in
  configuration objects that code outside the program may set (sqlite's
  allocator, mutexes and VFS; cJSON's), and extents nothing in the code
  states. RFC 0032's runtime extents and liveness checks turn the last
  into checks; the first two need a model of hooks (a joined summary per
  slot, or declared hook contracts).
- **The build-cost bound of G12** (carried from this RFC: a `weavec-cc`
  build within 8 times the reference compiler's CPU; 2 to 47 times at its
  close). The link step analyses the program again for every executable it
  links: it should reuse what the compile step decided for the units whose
  program facts did not change, and keep results across links of the same
  objects (with RFC 0033's records in object sections).
- **Values a summary cannot describe** share the unknown object, so a
  release of one is a possible release of all (the possible temporal
  warnings in cJSON's tests, G4's count of 65 against 60). A per-call
  object, as an unknown callee's result has, needs its ownership settled
  first: tried at this RFC's close, it reported leaks.
- **Array element precision** beyond summary cells (RFC 0015's selectors
  over objects).
- **Counted-field invariants across units** (RFC 0030 §7.6, A3): the
  records a header defines, verified at link from every unit's stores (in
  a unit's own records they are inferred, *Implementation amendments*).
