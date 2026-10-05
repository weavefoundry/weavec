# Architecture

WeaveC is a memory-safety checker for C and a drop-in C compiler, built on
Clang and LLVM. [RFC 0030](rfcs/0030-prove-or-trap.md), *Prove or trap*,
defines its design. Each safety *facet* (spatial, null, temporal) of every
memory operation, or *site*, gets exactly one outcome in the *ledger*:
proven, checked by a compiler-inserted runtime check, guarded by a check
against the runtime's object table, a definite violation (a build error), or
unresolved or trusted with a reason from a closed list. `weavec-cc` inserts
the checks into the AST through Sema before a deferred CodeGen, with no ABI
change. [RFC 0031](rfcs/0031-object-engine.md), *The object engine*,
replaced the engine behind RFC 0030's seam: its facts live on abstract
objects and symbolic values, so a fact made through one alias is seen
through every other.
[RFC 0032](rfcs/0032-runtime-enforcement.md), *Runtime enforcement*, added
the runtime every enforcing build links: an allocator that keeps each heap
object's extent and liveness, a table of the stack and global objects the
units register, and the *guards* that turn an unresolved spatial or temporal
facet with a pointer operand into the sixth outcome, `guarded`. A static
temporal check still does not exist: a temporal facet is proven, guarded, a
violation, or unresolved.

The code is three C++ libraries, two thin command-line tools and a C
runtime. The arrows point from a layer to what it may depend on.

```
        ┌──────────────────────┐  ┌──────────────────────────┐
        │  tools/weavec        │  │  tools/weavec-cc         │
        │  (libTooling)        │  │  (drop-in C compiler)    │
        └──────────┬───────────┘  └────────────┬─────────────┘
                   └───────────┬───────────────┘
                               ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  deferred CodeGen, check
                 │  lib/Frontend            │  emission, ledgers, unit
                 │                          │  records, link step, driver
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Analysis        │  kinds, sites, slots, the
                 │  lib/Analysis            │  engine seam, the object
                 │                          │  engine, check planning
                 └──────┬───────────┬───────┘
                        ▼           ▼
        ┌────────────────────┐   ┌────────────────────┐
        │  weavec::Core      │   │  Clang / LLVM      │
        │  lib/Core          │   │  (external)        │
        │  no Clang/LLVM     │   └────────────────────┘
        └────────────────────┘
```

`runtime/` is C code that `weavec-cc` links into user programs: into every
enforcing link unless the runtime is off (see *The runtime*). It depends on
nothing above and includes no WeaveC header but its own. The layering rule
is strict:

- Core includes nothing from `clang/` or `llvm/`. It is the one library
  built without their include paths.
- Analysis is the only layer that knows both Core and Clang.
- Inside Analysis, only the `Engine*.cpp` files include the engine's
  private header `lib/Analysis/Engine.h` (RFC 0031 §2, gate H2), so the
  components on the near side of the engine seam (see *The engine seam*),
  `SiteCollector`, `AttributeReader`, `KindInference`, `SlotCollector`,
  `BoundaryInvariants`, `CheckPlanner` and `LedgerAdapter`, never see the
  engine's internals. The rest of the code names the engine only as
  `ObjectEngine` behind `SafetyEngine`.

RFCs 0030, 0031 and 0032 are implemented in stages. This page describes
their design; the [roadmap](roadmap.md) says which stages have landed. Checked mode
(RFCs 0018–0029) was removed by RFC 0030 and remains in the repository
history at tag `v0.10.0`. The path-based engine that RFC 0030 kept, released
in v0.11.0, was deleted by RFC 0031 together with its Core trackers and its
summary format.

## `weavec::Core` — the model

`lib/Core` holds the model and depends only on the C++ standard library, so
it can be unit-tested without parsing code and reused by another frontend.
It never sees a `clang::VarDecl`, only an opaque `core::Handle` the
Analysis layer assigns, and never a `clang::SourceLocation`, only a
`core::SourceLocation` whose `opaque` field the frontend fills in. Check
operands that name program state are opaque place handles, which Analysis
resolves to Clang declarations.

### The ledger and its companions

| Header | Purpose |
| --- | --- |
| `Ledger.h` | Sites, facets and outcomes (RFC 0030 §2, RFC 0032 §1): `SiteKind`, `Facet`, `SiteOutcome` (six, with `Guarded`), the closed reason lists with their JSON spellings and phrases, merging by rank, the defaults for undecided facets, `CheckTemplate`, `RequireLevel`, `RuntimeUse` (`config.runtime`: on, off, or mixed for a program whose units differ), `UnitLedger` and `Ledger` with requirement records, diagnostics and the A1–A5 assumption counts, the `summary` rollup and the summary line. |
| `PointerKind.h` | The kind lattice (§7.1): `single`, `counted(e)`, `sized(e)`, `ended-by(q)`, `nul-terminated` or `unknown`, with nullability, a source (declared, inferred, default) and an `ExtentTerm` over a sibling parameter or field. Every kind is a lower bound. `ExtentClass` says whether an extent is exact, declared or a lower bound; only the first two may be check operands, and only an exact extent can make an access a violation. |
| `LibrarySpec.h`, [`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt) | The one declarative table of C library, POSIX, platform and builtin functions (§8), and its parser: per argument, the access, required length, nullability, ownership effect, release family, state slots and callback clause; per call, the result, disjointness, `exits`/`noreturn`, format arguments and fortified aliases. It also lists the platform headers of §5.2. CMake embeds the text, and it replaces the three library models of v0.10.0. |
| `CheckPlan.h` | Checks as pure data (§10.1): per checked requirement record, lowered violation or guarded facet, a template (`nonnull`, `index`, `span`, `len`, `disjoint`, `assert`, and RFC 0032's guards `object`, `live`, `release`), a form, a placement, an optional guard term and `CheckTerm` operands. `isGuard` tells the three guard templates from the static checks. |
| `FnSlots.h` | Function-pointer slots (§9.3): the constraints `f ∈ S`, `S ⊆ T` and `open(S)` over field, global, parameter, result and local slots, and the solver that computes each slot's targets and whether it is closed. |

A site is one operation in one emitted function, identified by
`{function, ordinal, kind, location}`. Its kind is `deref`, `index`,
`ptr-arith`, `cast`, `int-to-ptr`, `lib-call`, `release`, `call` (a call or
a function exit), `assume` or `raw` (§2.1), and a facet exists only where it
has meaning. `assume` sites have a fourth facet, *assertion*. Records of one
facet merge by rank within one pass:
`violation > unresolved > guarded > checked > trusted > proven`. A guarded
facet keeps the unresolved reason the engine gave it (`FacetDecision`'s
`unresolved` is set for both outcomes). The unresolved reasons
are `unknown-extent`, `unknown-index`, `inexpressible`, `may-released`,
`may-moved`, `may-alias-released`, `may-invalid-release`,
`may-mismatched-release`, `may-dangle`, `may-conflict`, `unknown-callee`,
`callback`, `setjmp`, `budget`, `unanalysed`, `raw-cast`, `dangling-escape`,
`second-owner` and `no-zero-init`. The trust reasons are `unsafe`,
`system-api`, `library-spec`, `extern-contract`, `caller-contract`,
`external-unit` and `concurrency`. Adding a reason requires an RFC.

### The abstract domain

The object engine's domain (RFC 0031 §4) is in Core, so it is unit-tested
without Clang. Questions only the frontend can answer (whether two types may
name one object, the value of a cell this activation never wrote) go
through the `HeapOracle` interface the engine implements.

| Header | Purpose |
| --- | --- |
| `Heap.h` | Symbols, objects, cells and states (§4.1–§4.9). A `Sym` names one runtime value; `SymInfo` is what is known of it: for an integer its C type, defining operation and linear form; for a pointer its points-to targets (at most 8 `(object, offset)` pairs, else "any object"), nullness with the `allocatorSource` bit, `ReleaseRecord`, share count, raw origin, owning-slot ancestors (D6) and pending result cases; for a function value a set of at most 32 functions. `ObjectTable` interns objects per function by their origin (`Local`, `Global`, `Literal`, `Function`, `HeapRecent`, `HeapOld`, `Entry`, `EntrySummary`, `Materialized`, `Focus`, `CallResult`, `Unknown`), so two states name an object the same way. `ObjectState` holds an object's cells (by `CellKey`: a byte offset, a selected element cell over an index symbol, or the summary cell of an element position), element segments, extent, life (`Live`, `Released`, `MayReleased`, `UnknownReleased`, `Ended`, `MayEnded`), release record, family and ownership, and string facts. `HeapState` is one program point: objects, symbols, the zone and the values of expressions carried between blocks. `Heap` is the operations: loads and stores (strong or weak), releases, the temporal and spatial verdicts, distinctness (D1–D6), join, widening and garbage collection. |
| `Zone.h` | The numeric domain (§4.4): closed difference bounds `x − y ≤ c` between integer symbols, with symbol 0 for the constant zero; relational bounds for at most 64 symbols per state, the rest keep bounds against constants only. Joins and widenings are paired: the heap decides which symbol of each side a result symbol stands for. |
| `Persistent.h` | `PMap`, the sorted vector shared by reference count and copied on the first write through a shared handle, so a state is copied along a CFG edge for one reference (§4.8). |
| `Path.h` | `SummaryPath`: a root (`param(i)`, `global(g)` or `result`) followed by dereference, field and index steps. Summaries, boundary facts and pointer kinds name places this way. An anonymous or positional member is spelled by its byte offset (`.#16`). |
| `Effects.h`, `EffectsIO.h` | The summary, `FunctionEffects` (see *Summaries* below), and its text form, summary format 30, with the join and global renumbering the program database needs. |
| `Ownership.h` | `OwnershipKind` lattice (`Unknown ⊑ {Owned, Shared, Mutable} ⊑ Raw`); `Raw` is usable only inside an unsafe region. |
| `Integer.h` | Target integers of 1 to 64 bits, modular ranges of at most two intervals, conversions, and the checked arithmetic of invalid operations (RFC 0017). |
| `Diagnostic.h` | `Diagnostic`, `Certainty`, the ids in `diag::`, `FixItHint` and `DiagnosticSink`. |
| `SourceLocation.h`, `Scc.h`, `AnalysisStats.h` | Frontend-neutral positions with an `opaque` slot; Tarjan's strongly connected components for call and unit graphs; work counters, never part of a proof. |

### Certainty

Every diagnostic is *definite* or *possible* (RFC 0030 §3). The
`ReleaseRecord` of RFC 0030 §3.1 lives on values and objects rather than on
places. It is definite when it has `allPaths` (it holds on every path merged
since), is not `conditional` (from an effect that holds only on some outcome
classes or paths, or from a `lossy` one), is not of unknown origin (the
unknown-callee default or an open slot, never diagnosed) and is not
`aliasOnly` (made by a weak merge of values that elements or aliases do not
tell apart, never diagnosed). A pointer's `allocatorSource` bit keeps a null
allocation result a checked facet rather than a `null-dereference` error.
A release that conflicts with a stored borrow is a `conflicting-borrow`
error only when the cell holds the borrow on every path and the release is
unconditional. A join keeps a record present on one side only with
`allPaths` cleared, and joins the lives of an object released on one side
and live on the other into `MayReleased`. A correlated bug is therefore a
warning, not an error.

### Summaries

`FunctionEffects` (`Effects.h`) is what a function does to its *entry
heap*, the objects its parameters and the globals reach, named by
`SummaryPath`s, and what it returns (RFC 0031 §6.1). It holds whether the
function returns, whether the summary is incomplete (and why), path effects
(release with family and interior offset, move, the unknown-callee default,
escape, share up and down), stores into entry cells, result alternatives
(null, a fresh object of a family with an extent term over the parameters,
an entry path, static storage, an integer range, a pointer to the callee's
dead frame), non-null facts per result class (RFC 0030 §9.2), and the paths
it reads and writes. Effects, stores and results carry RFC 0030 §9.1's case,
result classes optionally narrowed by a parameter's zero test, with `may`
and `lossy` bits; an effect on array elements carries its element range
(§4.9). `EffectsIO.h` defines the stable, Clang-free text form, summary
format 30: one item per line (`returns`, `incomplete`, `effect`, `store`,
`result`, `nonnull-on`, `reads`, `writes`), and every field round-trips.
The unit record carries each function's summary in this form.
Pointer kinds are not part of the summary: the record carries them beside
it.

## `weavec::Analysis` — the bridge

`lib/Analysis` is the only library allowed to include both `weavec/Core/*`
and `clang/*`. Besides the engine (next section), it holds shared services
and the components around the engine seam.

| Service | Role |
| --- | --- |
| `Annotations.h`, `ClangLocation.h` | Recognise WeaveC annotations on declarations, statements and function-pointer types, including a function's ownership signature; convert source locations both ways. |
| `ProgramDatabase.h` | What a unit exports (`UnitExports`: format-30 summaries with linkage and type keys, imports, indirect-call types, unknown callees, count fields, boundary rows) and the database of other units' exports, joined by name (`findEffects`) and by function type for indirect calls (`candidateEffects`), with globals renumbered by name (RFC 0031 §7). |
| `Concurrency.h`, `BypassedDeclarations.h` | The shared set G of RFC 0030 §5.3 (what threads and signal handlers reach); the locals a jump can bypass, so zero-initialisation does not reach them (§11). |

Before the engine, these components read the AST and the `LibrarySpec`, with
no engine fact:

- `AttributeReader` reads the declared kinds (§7.2): the `WEAVEC_*` extent,
  string and nullability macros, Clang's `counted_by`, `sized_by`,
  `alloc_size` and nullability attributes, and `[static N]` and VLA
  parameters. `WEAVEC_*` annotations win over ecosystem attributes, which win
  over the `LibrarySpec` entry and then system-header attributes; a finding
  that rests only on the last is `trusted(system-api)`.
- `KindInference` fills the rest of the `KindTable` (§7.3–7.6): parameter
  kinds with their `reliesOnSingle` flags, result kinds, slot kinds (a
  greatest fixpoint that demotes `single` to `unknown` at any store that is
  not Single-valid), the must-access requirements R1–R5, store groups, and
  the counted-field candidates of §7.6 with their disqualifications
  (`weavec --dump-kinds` prints them). The engine runs the Houdini rounds
  over the candidates of the records the unit defines in its main file
  (`EngineInvariants.cpp`, RFC 0031 *Implementation amendments*); a
  surviving invariant gives its pointer field an exact extent.
- `SiteCollector` enumerates the sites of every emitted function, with their
  ordinals and facets, into the `SiteIndex` and the unit's undecided rows
  (§2.6). It runs after the kinds, because PtrArith and Cast sites exist
  only in required positions.
- `SlotCollector` collects the unit's function-pointer constraints for
  `FnSlots` (§9.3).

After the engine, these components complete the ledger:

- `BoundaryInvariants` checks at every call boundary and function exit that
  no place reachable from a parameter or global may hold a released or
  dangling pointer (`unresolved(dangling-escape)`), and that no two owning
  places may hold the same object (`unresolved(second-owner)`). At a call
  the reachable places are the objects the arguments point to and the
  globals; at the exit that returns to the caller they are the places whose
  storage has died, the result included, because a release there is already
  in the summary. It then downgrades every temporal facet the unit *proved*
  for a place of a broken class, which is the facet that relied on the
  entry assumption the boundary broke (§9.4). At link the other units' rows
  come along, so the propagation is program-wide (§13.2 step 5).
- `CheckPlanner` turns checked requirement records into `CheckPlan` entries,
  and, for each violation lowered to a warning, a guard where the runtime
  can check it (a temporal violation, a violation of a callee's or library
  row's requirement) and otherwise its check or a trap (§10; RFC 0033). It decides
  expressibility first (§10.3): an extra term must be side-effect free, name
  C places unmodified since the extent was derived, and compare against an
  exact or declared extent; otherwise the record is
  `unresolved(inexpressible)`. Planning is pure and runs in every mode, so
  the ledger does not depend on whether checks are emitted.
  With `PlannerOptions::runtime`, a second pass over each site,
  `planGuards` (RFC 0032 §6), then turns what is still unresolved into
  `guarded`, with a plan entry, where a guard exists:

  | Site | Facet | Guard | Placement |
  | --- | --- | --- | --- |
  | Deref, Raw (not a library call) | spatial | `object`, with the offset and width of the bytes the access touches (`accessBytes`: the member chain above the dereference, a bit-field's bytes) | wraps the pointer operand |
  | Index (`p[i]`, `*(p + i)`) | spatial | `object`, with the element size, offset and width | replaces the access |
  | Deref, Index, Raw | temporal | `live`; the site's `object` guard instead when it has one | wraps the pointer operand |
  | LibCall, Call (the call boundary) | spatial, per unresolved requirement record with an `Object` witness | `object` in the form `Need` (`object_n`, the bytes the call needs) or `String` (`object_s`) | wraps the argument |
  | LibCall | temporal | `live` for each pointer argument the row reads or writes and borrows; none where the argument has an `object` guard | wraps the argument |
  | Release, and a Raw site that is a library call, of the heap family | spatial, temporal | `release` | wraps the released argument |

  A facet whose reason is `no-zero-init`, `second-owner`, `may-conflict` or
  `may-dangle` is not guarded, nor is a site in a constant expression, a
  shared operand or a non-default address space. PtrArith, Cast and
  IntToPtr sites, the temporal facet of a Call site, the temporal facet of
  a variadic library row, and a requirement that binds only under a guard
  term stay unresolved. The engine supplies the need of a call argument's
  guard as a witness of shape `Object` (`EngineLibrary.cpp`); a
  requirement that an argument points to a whole record of the program's
  own type gets none (RFC 0032 *Implementation amendments*, 5). An
  unresolved `disjoint` requirement whose length is a string's is guarded
  with the `disjoint` template, its length read inside the string's own
  object (`CheckTerm::ObjStrLen`). A guard never replaces a static check.
  In verify mode the same pass plans guards with `proven` set for the
  proven temporal facets of those sites, and for the proven spatial facets
  of accesses and releases that carry no static verify check and that the
  types alone do not prove, so `summary.verifyChecked.temporal` is no
  longer always 0.
- `LedgerAdapter`, `SafetyEngine` and `ObjectEngine` form the seam
  described in *The engine seam*.

`UnitPipeline` (`runUnitAnalysis`) runs steps 2 and 3 of the compile
pipeline for one unit. It builds the kinds and the unit's slot solution,
collects the sites, runs `ObjectEngine` through an authoritative
`LedgerAdapter` (a discarding one for a silent fixpoint round), calls
`finish`, and reports the diagnostics in publication order, followed by the
require-level errors. `UnitPipelineOptions::dropGuardedPossible` is how the
compiler asks `finish` to leave out the possible findings its guards
enforce. A discovery-only run returns the unit's exports
(`ObjectEngine::discover`) without analysing it. The §7.6 field candidates
it passes are empty. The Frontend calls it through
`analyzeTranslationUnit`.

## The engine: the object engine

`ObjectEngine` (`include/weavec/Analysis/ObjectEngine.h`) is the prover
behind the seam ([RFC 0031](rfcs/0031-object-engine.md)). Its facts live on
abstract objects and symbolic values: a pointer is a symbol with a
points-to set, memory maps object cells to symbols, and copying a value
copies its symbol, so every alias of a value sees every fact about it and
about the objects it points to. The engine's classes are declared in the
private header `lib/Analysis/Engine.h`:

| File in `lib/Analysis` | Responsibility |
| --- | --- |
| `EngineUnit.cpp` | `ObjectEngine` and `UnitRun`: the unit's call graph, summary rounds, the authoritative pass, owning slots, imported summaries and exports (§3, §7). |
| `EngineRun.cpp` | `FunctionRun`: the always-add CFG, the entry state, the fixpoint with widening at loop heads, the final (publishing) pass, object interning and materialisation of the entry heap (§3, §4). |
| `EngineExpr.cpp` | `Transfer`'s evaluation: rvalues to symbols, lvalues to addresses (`evaluate`, `addressOf`), loads, stores, casts, pointer arithmetic, integer operations and conversions (§5.1, RFC 0017). |
| `EngineCalls.cpp` | `CallApplier`: callee resolution and the effects of summaries, contracts, library rows, platform declarations and unknown callees; indirect calls; alias contexts (§5.4, §6.6). |
| `EngineSummary.cpp` | Summary derivation at the exits and instantiation at calls (§6.2, §6.3). |
| `EngineDecide.cpp` | `Decider`: the temporal, null and spatial facets of every site, releases, invalid releases and conflicting borrows, with witnesses (§5.2, §5.3, §5.5). |
| `EngineLibrary.cpp`, `EngineStrings.cpp` | The requirement records of `LibrarySpec` arguments, `disjoint` ranges and format calls, and the `Object` witness a guard of an unresolved requirement compares (RFC 0032 §6); RFC 0012's string facts over objects. |
| `EngineKinds.cpp` | What the kinds give the engine: parameter extents and nullness at entry, the accesses a must-access requirement covers, and the requirement records at calls (RFC 0030 §7.2–§7.5). |
| `EngineContexts.cpp` | Contexts across units: the portable keys of the contexts a call asks of another unit's function, and the runs that serve the contexts other units asked (§7 *Amendment (cross-unit contexts)*). |
| `EngineInvariants.cpp` | Counted-field invariants: Houdini over `KindInference`'s candidates, and the functions that read a standing invariant's field analysed again with it (RFC 0030 §7.6, restored by RFC 0031). |
| `EngineLifetimes.cpp` | Frame storage whose lifetime ended, the boundary facts of calls and exits with their place classes (the class of a cell is the field that holds it, `fieldHolding`, so an element of an array is of its array's class), `WEAVEC_ASSUME`, raw-pointer laundering (§5.6, §5.7). |
| `EngineAnnotations.cpp` | `annotation-mismatch` of a definition against its own declaration (RFC 0003 reconciliation, RFC 0012 sized-field stores). |
| `EngineIntegers.h` | Target integer types of Clang types and bit-fields, the overflow builtins and operators (RFC 0017). |

### The domain

Every value is a symbol (§4.1): an integer, whose numeric facts live in the
zone; a pointer, with its targets, nullness, release record, share count,
raw origin and the owning slots it was derived from; a function value; or
unknown. An object (§4.2) has an origin that names it deterministically: a
local, a global, a literal, a function, the most recent or the older
allocations of a site (the recency abstraction), an entry object named by a
`SummaryPath` from a parameter, a global or a callee's result, a k-limited
entry summary, a callee's result, or the unknown object behind a raw
pointer. A singular object stands for one runtime object and takes strong
updates and definite releases; the others take weak ones. Cells are keyed
by byte offset from Clang's record layout; a variable index names a
*selected* cell over its index symbol, and what stores through indices the
engine cannot name wrote goes to the element position's summary cell.
Segments `[from, to) ↦ v` describe ranges of elements, so a cleanup loop
releases a range and a summary can say so (§4.9). Bytes that code the engine
does not see rewrote are *forgotten*: the whole object (`havocked`), a byte
range (a member copied over, a union a callee wrote), or a range forgotten on
some paths only, where an unwritten cell reads as its value otherwise merged
with an unknown one. A value also records the entry cells it was computed
from (`entryOrigins`), which the boundary propagation follows (RFC 0031
*Implementation amendments*).

Entry objects are materialised on first load, with the kind of the slot
they were loaded from (§4.6). An entry path in which one field repeats more
than twice, or longer than 6 steps, folds into an entry summary object. At
a join where a variable points to a different object on each side, the join
makes a *focus* object with the two as candidates; an entry or focus object
that a path released and no root reaches any more becomes a *dead copy*,
which keeps the release for the summary without overlapping the live object
of the next iteration (RFC 0031 *Implementation amendments*). Garbage
collection at every block end drops what no root (the locals, the
parameters' cells, the globals, the carried expression values, the result)
reaches; dropping an owned, unreleased, unescaped allocation is a leak.

Two pointers may be equal unless one of the distinctness rules D1–D6 (§4.5)
separates their targets: incompatible types under C's effective-type rules,
two owning places, the owner forest (an object reached through an owning
step is distinct from the objects on its path), freshness, identity of two
singular objects the engine created, and derivation through owning slots.
The owner forest is the acyclicity clause RFC 0031 adds to assumption A3.

### Transfer

`FunctionRun` evaluates Clang's CFG built with `setAllAlwaysAdd()`, so
every subexpression is an element in evaluation order and `?:`, `&&`, `||`
and `,` need no special order; an expression's value is kept in the block's
memo, and values a later block reads travel in the state (§2). There is no
separate IR. Blocks are visited from a worklist in reverse post-order; a
loop head widens after two joins, jumping grown bounds to the program's
constants or the type's limits. A condition edge adds its constraint to the
successor's zone and prunes the edge when it is unsatisfiable (§4.4).
States are persistent maps, so a state is copied per edge for one
reference and a join costs the size of the difference (§4.8).

Loads and stores go through addresses (§5.1): `x` is its object's cell,
`*e`, `e->f` and `e[i]` add the field's offset or the scaled index to the
pointer's targets, pointer arithmetic moves the offset term, and pointer
casts keep the symbol. An integer-to-pointer conversion makes a pointer of
unknown provenance to the unknown object, whose accesses are
`unresolved(raw-cast)` (RFC 0033 §2: only `WEAVEC_RAW` declarations make
raw values, and a merged value is raw only when every value merged is;
one raw on some paths is `rawSome`, guarded and never an error); a pointer
read back from reinterpreted bits is `raw-cast`; a construct the engine does not evaluate yields unknown values
and `unresolved(unanalysed)` for its sites. `memcpy`, `memmove` and record
assignments copy leaf by leaf, every source cell read before any is
written. A body that transfers more blocks than `-fweavec-budget` (or visits
one block more than 64 times) stops: its facets take the defaults with
reason `budget`, and its summary is incomplete.

### Decisions

After the fixpoint, the final pass transfers each reachable block once more
from its entry state and decides every site `SiteCollector` enumerated
there (§5.2), publishing through `LedgerAdapter`. The tables of RFC 0030 §3
apply unchanged; their premises are read from the operand's value at the
site. A definite release record on the value is a violation and a possible
one a warning; a released target the value has no record of is
`unresolved(may-alias-released)`; a target an unknown callee reached, or a
pointer into the unknown object, is `unresolved(unknown-callee)`. Null
facets are proven, checked or, for a definite null that is not an
allocation result, a violation. A spatial access is compared, in the zone,
with the extent of every target: in bounds for all targets is proven, out
of bounds for every value against an exact extent is a violation, and an
undecided access against an exact or declared extent is checked when its
terms are expressible. A witness names the C places that hold the symbols
its terms are over at the site; because symbols are immutable, a place that
holds a symbol there holds the value the extent was derived from (§5.3).
Diagnostics are reported once per site and id, with RFC 0030's messages and
notes taken from the records. Nothing is suppressed in a `WEAVEC_UNSAFE`
region, and in a function that calls `setjmp` every temporal facet is
`unresolved(setjmp)`.

### Calls

A call's own sites and its boundary facts are decided from the state before
its effects. `CallApplier::applyDirect` then resolves a direct callee
(§5.4): the summary of a definition in the unit, or at link and in
`--whole-program` the program database's (`UnitRun::summaryOf`); a declared
ownership contract that covers every pointer argument; the declaration's
ownership annotations; the `LibrarySpec` row; a platform-header
declaration, which borrows its arguments under `trusted(system-api)`; else
the unknown-callee default of RFC 0030 §5.1. That default marks every object
reachable from a pointer argument through non-`const` pointees, every
escaped object and every object reachable from an externally visible global
as `UnknownReleased` and forgets their cells; a cell whose address is passed
(`&cmd`) holds a fresh value of unknown nullness afterwards. A library row's
`alloc` creates a recent heap object of the row's family with the extent the
row gives, and its argument requirements become requirement records;
lengths that may be zero make the null requirement `null-if-zero`.

Indirect calls resolve through the flow-sensitive function value, then the
slot solution (RFC 0030 §9.3). Several targets are each applied to a copy
of the state and the results joined. An open slot without targets gets the
unknown-callee default with reason `callback`. At a call whose pointer
arguments (or globals) point into the same object, the callee is re-analysed
in an *alias context* with those entry objects unified and the constant
integer arguments bound, at most 16 contexts per callee and 3 deep (§6.6).
The context run's adapter only collects; each use-after-free, double free or
use-after-move it finds is reported with the note "called here with related
pointer arguments" and linked to the call's temporal facet. Contexts are
unit-local.

### Summaries

A summary is read off the exit states against the entry heap (§6.2). An
entry object that is released, moved or reached by an unknown callee gives
the effect for its path; a cell of an entry object or global that was
written gives a `store`, told apart from an unchanged cell by the entry
value each materialised symbol remembers; a result pointing to a new object
gives a fresh result, with its extent re-expressed over the parameters
when the zone relates it to one, and the new object's contents as stores
below `result`. An exit whose result carries pending cases is derived once
per result class. Effects on some exits only are keyed by the result
classes that separate them, by a parameter's zero test, or both, and are
otherwise possible; a release records what the state knew of the releasing
function's unmodified integer parameters (RFC 0031 *Implementation
amendments*).

At a call the summary is instantiated (§6.3): each path is walked through
the caller's memory from the arguments and globals, materialising entry
objects as a load would, every store's place and value are read before any
is written, and each effect is applied to the objects the path reaches,
weakly when it reaches several. A fresh result becomes a heap object named
by the call site. A case keyed on the result becomes a *pending case* on the
result symbol, applied when a later test of the result selects its class;
cases the arguments already decide are applied at the call. An incomplete
summary adds the unknown-callee default to its known effects (RFC 0030
§5.5).

### The unit driver

`UnitRun` drives a unit (§3). It computes the unit's owning slots (the
fields and globals some function releases a value loaded from, D2), builds
the call graph over the unit's definitions (direct calls, plus the targets
the slot solution gives indirect calls), and takes its strongly connected
components bottom up (`core::Scc`). A recursive component iterates its
members' summaries from empty through a discarding adapter, at most 8
rounds; summaries that have not converged are marked incomplete. Then each
member gets one run: authoritative (`LedgerAdapter::beginFunction`) for a
function the unit reports, a summary run otherwise. Outside a cycle that run
also produces the function's summary, so a leaf function is analysed once.
`exports()` gives the summaries of the external and address-taken
definitions, with globals renumbered by portable name, and the unit's
imports and indirect-call types; `discover()` gives the same without
analysing anything.

At link and in `--whole-program`, the same driver runs with the program
database and the program-wide slot solution (RFC 0030 §13.2). A callee
another unit defines is known by its imported summary, whose globals are
renumbered into the unit's; an effect through a global the importing unit
does not declare makes the summary incomplete there. An indirect call that
neither its value nor the slots resolve reaches every address-taken
function of its type, the unit's own and the database's joined candidate
summary; in a unit alone it keeps the unknown-callee default.

`SafetyEngine::dump` is the hook of `--dump-analysis`: `ObjectEngine::dump`
re-runs a function and prints the entry state of every block, its objects
with their kind, name, extent and life, their cells with the symbols they
hold, and the zone. With `WEAVEC_ENGINE_DUMP` set, the engine prints every
summary it computes or imports to stderr; `WEAVEC_ENGINE_DUMP=2` adds each
run's exit states and `WEAVEC_ENGINE_DUMP=3` each run's block states.

RFCs [0001](rfcs/0001-ownership-model.md) (model),
[0002](rfcs/0002-intraprocedural-checking.md) (dataflow),
[0003](rfcs/0003-signature-inference.md) (summaries),
[0004](rfcs/0004-unsafe-boundaries.md) (unsafe boundaries),
[0005](rfcs/0005-whole-program-analysis.md) (whole program),
[0006](rfcs/0006-precision.md) (precision),
[0007](rfcs/0007-resource-lifecycle.md) (resources),
[0008](rfcs/0008-pointer-validity.md) (validity) and
[0009](rfcs/0009-value-conditional-behaviour.md) (guards) specify the model.
RFC 0030 replaces RFC 0001's guarantee statement and amends RFCs 0002–0008
where it changes them; its §19 lists each amendment. RFC 0031 restates over
objects how those RFCs' facts are represented, and its *Implementation
amendments* record the decisions made while it was built.

## `weavec::Frontend` — Clang integration

`lib/Frontend` adapts the analysis to Clang's frontend machinery, emits the
checks, writes ledgers and unit records, and runs the link step.

| Component | Role |
| --- | --- |
| `FrontendAction.h` | `WeaveCAction`, an `ASTFrontendAction` for libTooling, and `createWeaveCConsumer`, which `weavec-cc` runs at the end of each unit; both run `UnitPipeline`. Every emitted function is analysed, including `static inline` functions from user headers (§5.6). |
| `DeferredCodeGenConsumer` | Sits in front of CodeGen in every C code-generating action (§10.5). It forwards Sema set-up at once and records every other callback. At the end of the unit it runs the analysis and `CheckEmitter`, then replays the callbacks in order. Without deferral, CodeGen emits external functions before the analysis runs. It overrides every `ASTConsumer` and `SemaConsumer` virtual of LLVM 23, a list on the LLVM-upgrade checklist. |
| `CheckEmitter` | Applies the `CheckPlan` through Sema (§10.6): `BuildCallExpr` to the helper, `ImpCastExprToType` back to the operand's type so a dereference stays an lvalue, `BuildBinOp` with a comma for a check before a call. User expressions are never evaluated twice. A rewrite Sema rejects leaves the subtree unchanged and fails the compile with an internal error. It also applies the zero-initialisation lowering, gives the guards of a function their range cache (`planCache`, `declareCaches`; see *The runtime*), and registers the unit's stack and global objects (`registerObjects`). |
| `ObjectRegistration` | `planObjects` (RFC 0032 §4, §5): which locals and parameters of each emitted function escape and are entered into the runtime's stack list, which calls return twice and are wrapped, and which globals the unit defines and describes. The plan is pure; `CheckEmitter::registerObjects` applies it. |
| `Prelude` | The helpers the rewrites call, injected into the predefines buffer (§10.2): `static`, `always_inline`, `nodebug` functions for the six templates and their forms, term helpers that saturate toward failure, and the allocation wrappers. With the runtime it also declares the runtime's entry points and the arena descriptor, and defines the guard helpers (`object`, `object_c`, `object_n`, `object_s`, `object_l`, `live`, `live_c`, `release`) and the stack-object helpers. Trap mode calls `__builtin_verbose_trap("weavec", <template>)`, report mode `__weavec_rt_report`, and verify mode adds `__weavec_prv_*` with the category `weavec.proven`. PCH and module builds declare the helpers `extern` instead (§10.9). Each check and guard helper also has a copy that is not inlined, `<helper>_ool`, which a function with more than 4,096 plan entries calls ([RFC 0033](rfcs/0033-drop-in-by-default.md) amendment 12). |
| `ZeroInit` | Plans the zero-initialisation of the allocation family (§11): calls to `LibrarySpec` entries with the `zero-init` flag become wrappers that zero the usable region, and `alloca` gets a `memset`. The plan is pure, so the ledger's A5 counts precede any rewrite. A unit that defines an allocator lowers nothing. |
| `LedgerWriter` | JSON (`weavec-ledger`, version 2: `LedgerSchemaVersion`) and SARIF 2.1.0 renderings of a ledger (§12, RFC 0032 §10), the `weavec-fp/1` fingerprints (a truncated SHA-256 of key, root-relative path, function, normalised message and ordinal), the fingerprint root and atomic writes. |
| `LedgerOutput` | Completes a unit or program ledger with the producer, root, configuration and the unit's source, object and target; applies the `-W` flags so the ledger counts what was reported; writes it where `-fweavec-ledger` says (a file, or a directory receiving one ledger per unit and per link) through a temporary file renamed into place; and prints the summary line under `-fweavec-summary`, whenever a ledger is written, and always in `weavec`. |
| `UnitRecord` | The format-31 codec (§13.1, RFC 0031 §7, RFC 0032 §10, RFC 0033 §7): framing, a typed header, and a payload checked against the codec's field table, whose SHA-256 is the schema fingerprint. The encoder refuses values the table does not describe; the decoder rejects missing, unknown and mistyped keys. |
| `ProgramAnalysis` | The whole-program algorithm of RFC 0005 over an abstract `ProgramUnit`: discover every unit's exports, order the units by strongly connected component, analyse acyclic units once and cyclic groups to a fixpoint, and publish in the last round only. It hosts step 4 of the link step under `-fweavec-link=analyze`, stopping at the link budget (a unit not finished keeps its compile-time rows), and `weavec --whole-program`. |
| `RecordFacts` | Builds a unit's interface facts for the record from the components that run before the engine: the kinds, reliance flags and exported requirements of its definitions, the declared kinds and ownership annotations of its imports, the slot constraints with local slots eliminated, and the Call site of each import call with what the caller knows about each argument (§13.2 step 5). |
| `RecordPayload` | The payload codec's field table: the authoritative list of payload keys and their types, and the source of the schema fingerprint. |
| `LinkStep` | The parts of the link step that work on what the records say (§13.2), independent of how they were found: solving the program's function-pointer slots, verifying every import's declared annotations and kinds against the defining unit, deciding each exported requirement at the callers in other units (`verifyRequirements`: their Call rows, the A1 `verified` count, and the discharge of `trusted(caller-contract)` in a closed program), the reliance rows and the A1/A3 counts, the allocator warning of §11, and the program's boundary rows. |
| `Driver` | `weavec-cc`: Clang's driver plans the jobs, each `-cc1` job runs in-process behind `DeferredCodeGenConsumer`, compile jobs write the record, and link jobs run the link step before the linker. It decides whether a build uses the runtime (`runtimeObstacle`) and puts the runtime's archives on the link line (`addRuntimeLibraries`, `dropAllocatorIfDefined`). |
| `DispatchEdges` | `SplitDispatchEdges` (RFC 0031 §9.2): an LLVM function pass that splits every critical edge into a block ending in `indirectbr` with more than 8 predecessors, so a computed-goto interpreter keeps its dispatch replication when checks supply edges into it. `weavec-cc` registers it through `CodeGenOptions::PassBuilderCallbacks` at `OptimizerLastEP`, only for optimised units whose checks are emitted, so `-fweavec-checks=none` objects stay Clang's. |
| `DiagnosticControl` | Applies the `-W` flags by each diagnostic's id and certainty. An error can be lowered but never disabled, and a flag naming a removed id is refused. `FilteringSink` drops what an earlier step already reported. |
| `ClangDiagnosticSink` | Forwards `core::Diagnostic`s, with notes and fix-its, to Clang's `DiagnosticsEngine`, so they render exactly like Clang's own. |
| `ResourceDir`, `AnalysisStats` | Locate `weavec.h`, the runtime archives, Clang's resource directory and `clang`; write the work counters of `--analysis-stats`. |

The ledger writers and the record codec live in Frontend because Core may
not use LLVM. They are built on `llvm::json` and `llvm::SHA256`, and a Core
JSON and SHA-256 implementation would duplicate LLVM's.

## The runtime

`runtime/` holds the only code WeaveC links into user programs
([RFC 0032](rfcs/0032-runtime-enforcement.md)). It is C, built for the host
into three archives installed under `lib/weavec` next to `weavec.h`.
[`weavec_rt.h`](../runtime/weavec_rt.h) is its internal interface, shared by
its sources and its test and not installed; compiled code reaches it only
through the entry points the prelude declares and through the standard
allocation functions.

| Archive | Sources | Holds |
| --- | --- | --- |
| `libweavec_rt.a` | [`weavec_alloc.c`](../runtime/weavec_alloc.c), [`weavec_objects.c`](../runtime/weavec_objects.c), [`weavec_owner.c`](../runtime/weavec_owner.c), [`weavec_report.c`](../runtime/weavec_report.c) | The arena allocator (`__weavec_rt_alloc`, `__weavec_rt_free`, `__weavec_rt_realloc`, `__weavec_rt_size`), the object table over heap, stack and global objects (`__weavec_rt_find`), the guards' out-of-line entry points, `__weavec_rt_report`, `__weavec_rt_fatal` and `__weavec_rt_trapping`, and the table of entry points (`__weavec_rt_dispatch`) through which one copy forwards to another on Darwin. |
| `libweavec_alloc.a` | [`weavec_malloc.c`](../runtime/weavec_malloc.c) | The standard allocation functions over the arena, for the image the archive is linked into. `malloc`, `calloc`, `realloc` and `free` are strong definitions; the others (`reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized`, and `malloc_size`, `malloc_good_size`, `reallocf` on Darwin or `memalign`, `pvalloc`, `malloc_usable_size` elsewhere) are weak, so a program's own shim over `malloc` replaces them and keeps the arena. |
| `libweavec_chk.a` | [`weavec_chk.c`](../runtime/weavec_chk.c), [`weavec_chk_report.c`](../runtime/weavec_chk_report.c) | The prelude's helpers, guards included, as real functions for precompiled-header and module builds (§10.9), generated from the prelude by `weavec-cc -fweavec-print-prelude=out-of-line`. The report family carries a `_report` suffix, because one archive cannot define two signatures under one name. |

### The allocator and the object table

The allocator reserves one arena at first use: one region of
2^32 bytes (2^30 when that reservation fails) per size class, followed by
the metadata. The classes are 16 to 128 bytes in steps of 16, then four per
doubling up to 2^30 bytes. A request takes a slot of the smallest class
strictly larger than it, so at least one byte after every object belongs to
no object. Each slot has one 32-bit word of metadata, `size << 2 | state`,
with the states never allocated, live, free and dead; free lists and the
quarantine queues live in the metadata, never in freed memory. The object a
pointer points into, its requested size and whether it is live therefore
follow from the pointer by arithmetic and one load. Every block is
zero-filled. A released slot is *dead* and waits in a quarantine (64 MiB by
default, `WEAVEC_RT_QUARANTINE=<bytes>` in the environment) before it is
recycled; a reallocation that leaves its size class releases the old block
the same way. A request no class can hold is mapped on its own as a *huge
block* and kept in a table. The allocator validates every release itself:
a release of an interior pointer, of a dead or free slot, or of a stack or
global object ends the program through `__weavec_rt_fatal`
(`weavec: invalid release of <pointer>: <why>`). A pointer it does not own
goes to the next allocator (the pointer's own malloc zone on Darwin,
`__libc_free` and `__libc_realloc` or the next `free` and `realloc` in
lookup order elsewhere). On Darwin the arena is also registered as a malloc
zone and promoted to the process's default zone (the system's zones are
unregistered and registered again until the arena's is first), so `malloc`
from any image, the C library's own calls included, is served by the
arena; blocks the system's zones allocated before that go back to them.

On Darwin one runtime serves the process (RFC 0033 §6.2). Every image
linked by `weavec-cc` carries the archives, and the copy whose
`__weavec_rt_dispatch` `dlsym(RTLD_DEFAULT, …)` finds is the *owner*;
every other copy takes the owner's arena descriptor and forwards each
out-of-line entry point (allocation, release, lookup, registration, guards,
reports) to it, so one arena, object table and quarantine serve every image,
and inline guard paths read a descriptor that describes the one arena. A
table of another layout (another WeaveC version's runtime) is not used. On
ELF the executable's definitions already serve every shared library.

`__weavec_rt_find` answers what a pointer points into: an arena slot, a
huge block, a stack object of the calling thread, or a global object;
otherwise the pointer is *untracked*, and every guard passes on it
(assumption A6 of RFC 0032). A pointer into the arena that points to no
live block is into a dead object.

- **Stack objects** are kept per thread, ordered by frame. An entry is
  trusted only while its frame can still be live: entering, leaving,
  rewinding and looking up drop the entries of deeper frames, which is how
  a `longjmp` that skipped the cleanups is absorbed.
- **Global objects** come from descriptors `{address, size}` that each unit
  emits into one section (`__DATA,__weavec_glob` on Mach-O,
  `weavec_globals` on ELF). A constructor in each image hands its section
  to `__weavec_rt_globals_add`; the table is sorted before the next lookup.

### Guards

A guard is a prelude helper wrapped around a pointer operand or an argument,
or replacing a subscript, exactly as a static check is (§10.4). Three
templates exist:

| Template | Helper | Passes when |
| --- | --- | --- |
| `object` | `__weavec_chk_object(p, i, step, off, width)` | the `width` bytes at the accessed address `p + off + i * step` lie inside the live tracked object that holds the first of them; or none of them is tracked and the access did not leave a live object for them (RFC 0033 §4) |
| `object` (form `Need`) | `__weavec_chk_object_n(p, need)` | `need` bytes from `p` lie inside its object; a need of 0 passes without a lookup |
| `object` (form `Length`) | `__weavec_chk_object_l(p, need)` | as `Need`, wrapped around a call's length argument that no term can repeat (`strlen(s)`), and returning it |
| `object` (form `String`) | `__weavec_chk_object_s(p)` | the string's terminator lies inside `p`'s object |
| `live` | `__weavec_chk_live(p)` | `p` does not point into a dead tracked object |
| `release` | `__weavec_chk_release(p)` | `p` is null, the start of a live heap object, or untracked |

A guard reaches the runtime in three steps:

1. **Inline arena lookup.** The prelude declares the arena's descriptor
   (`__weavec_rt_heap`: base, size, metadata base, region shift, class
   table). For a pointer inside the arena, `object` and `live` compute the
   region and slot and load the slot word in the helper itself, with no
   call.
2. **Range caches** (RFC 0032 *Implementation amendments*, 2). A function
   that guards a pointer inside a loop gets a local array
   `__weavec_ranges` of four words per entry, `{lo, len, state, expect}`,
   cleared on entry, with at most 64 entries. An entry belongs to the local
   variable the guards read their pointer from, or to one site. The cached
   helpers `object_c` and `live_c` pass without a lookup when the access
   lies in `[lo, lo + len)` and the 32-bit word at `state` still reads
   `expect`: for an arena block `state` is the block's own slot word, for
   anything else the runtime's epoch `__weavec_rt_epoch`, which changes
   when a huge block is mapped or unmapped or a table of globals is added.
   A loop that calls nothing (`isQuietLoop`) checks the state of its
   entries once on the way in (`__weavec_range_check`), and its guards do
   not read it. A unit whose helpers are external has no range cache.
3. **Out-of-line entry points.** Any other pointer, and a cache miss
   outside the arena, call the runtime: `__weavec_rt_object`,
   `__weavec_rt_string`, `__weavec_rt_live`, `__weavec_rt_release_ok`, and
   for a cached guard `__weavec_rt_object_range` and
   `__weavec_rt_live_range`, which answer the range to remember by value.
   `__weavec_rt_strlen` reads a string's length inside its own object, for
   a need that is a string's length.

`__weavec_rt_object` looks up the accessed address, not `p` (RFC 0033 §4).
In a live heap object the access passes wherever `p` points, so a base
formed outside a buffer can index back into it. In a live stack or global
object it fails when reached forwards from inside another live stack or
global object, since those lie next to each other with no gap. In no
tracked object it fails when its last byte is in one, or when `p` is in a
live object (the access left it for untracked memory).

A failed guard traps with `__builtin_verbose_trap("weavec", "<template>")`
in trap mode, or with the category `weavec.proven` for a verify guard of a
proven facet. In a unit built with the runtime, every failed check or guard
first calls `__weavec_rt_trapping` (RFC 0033 §6.1), which unblocks
`SIGTRAP` and `SIGILL` in the thread and resets an ignored disposition, so
the trap terminates even when the program blocked them (a handler the
program installed still runs); `__weavec_rt_fatal` does the same. In report
mode it calls `__weavec_rt_report`, which prints
`weavec: runtime check failed: <template> at <file>:<line>:<column>` once
per site and goes on (or aborts under `WEAVEC_RT_ABORT=1`); a `release`
guard that only reported skips the release. The case runner and the corpus
gate attribute traps through this output.

### Registering stack and global objects

`planObjects` decides, from the AST alone, what a unit registers, and
`CheckEmitter::registerObjects` adds the code after the checks are emitted:

- An *escaping local* (a variable of automatic storage, parameters
  included, that is not `register`, and whose address is taken or which is
  or holds an array that decays to a pointer anywhere but as the base of a
  subscript) gets a synthesized variable `__weavec_frame_<n>` declared
  right after it, whose initialiser calls `__weavec_stack_enter` and whose
  `cleanup` attribute calls `__weavec_stack_leave`; a parameter is entered
  at the start of the body. An object declared in a nested scope is entered
  with the flag `WeavecRtScoped`, and the objects of a function with
  automatic storage the plan does not register (compound literals,
  `alloca`) with `WeavecRtLoose`.
- A function that calls a returns-twice function registers no local; each
  such call is wrapped in `__weavec_stack_rewind`, which drops the entries
  of the frames a `longjmp` abandoned.
- Every variable of static storage duration the unit defines (static locals
  and tentative definitions included; not thread-locals, weak definitions,
  variables in a named section or a non-default address space, or types
  that are incomplete or have a flexible array member) gets a descriptor
  `__weavec_global_<n>`, handed to CodeGen behind the unit's own
  declarations through `newTopLevelDecls`.

The stack helpers take the frame of the function they are inlined into, so
a unit built with a precompiled header or modules, whose helpers are
external, registers no stack object. `-fno-weavec-stack-objects` and
`-fno-weavec-global-objects` turn either registration off for a unit.

### Linking, and when the runtime is off

`addRuntimeLibraries` adds the archives to every link job of an enforcing
build (`-fweavec-checks` other than `none`) that targets the host, before
the first `-l` library of the line: `-u malloc` and `libweavec_alloc.a`
when the build enforces guards, then `libweavec_chk.a`, then
`libweavec_rt.a`, and `-lpthread` on Linux. An archive member is linked
only when referenced, except the allocator's, which `-u` forces in.

The runtime is off, as if `-fno-weavec-runtime` had been given, when
`runtimeObstacle` finds a reason on the command line: `-ffreestanding`;
`-nostdlib`, `-nodefaultlibs` or `-nolibc`; a sanitizer that replaces the
allocator (`address`, `hwaddress`, `kernel-address`, `memory`, `thread`,
`leak`); or a target other than 64-bit Darwin or Linux. A link then prints
one note saying why. Without the runtime no guard is planned, facets that
would be guarded stay `unresolved`, nothing is registered, and
`libweavec_alloc.a` is not linked; `config.runtime` is `false` in the
ledger and the record.

When an input of the link defines a symbol that `libweavec_alloc.a`
defines strongly (`allocatorDefinedBy` reads the set from the archive
itself), `dropAllocatorIfDefined` takes the archive and its `-u` off the
line and prints a note: the program keeps its allocator, its heap is
untracked, and releases are not validated. Guards and the stack and global
objects still work.

`WEAVEC_RT_STATS=1` in the environment of a program makes the runtime print
its counters on standard error at exit, one line per counter as
`weavec: runtime: <n> <what>` (allocations, releases, recycled slots, huge
blocks, lookups by kind, range requests and ranges kept, stack objects
entered). The counters are not synchronised. On Darwin only the owner
prints, and its counters count every image's work. With
`WEAVEC_RT_REPORT_LOG=<path>`, report mode appends its lines to that file
instead of standard error.

## `tools/weavec` and `tools/weavec-cc`

`weavec` is a libTooling application: `weavec file.c -- <compiler flags>`, or
`weavec -p build/ file.c` with a compilation database (with `-p` and no
source, every file of the database). It injects
`-isystem <resource-dir>/include` and `-D__WEAVEC__=1`, so user code can
`#include <weavec.h>`. `--whole-program` analyses the files as one program.
It always prints the summary line; `--ledger`, `--ledger-format`,
`--require`, `--budget`, `--no-zero-init` and `--no-runtime` model a
`weavec-cc` build with the default checks. A group of units that
`--whole-program` cannot make converge keeps its widened summaries, with a
note. Its ledger describes the
enforcing build with the runtime, so it plans guards unless `--no-runtime`
is given. `--dump-analysis` and `--dump-kinds` are debugging aids.

`weavec-cc` is the drop-in compiler: `CC=weavec-cc make`. Its own flags,
which `weavec-cc --help-weavec` lists, choose the checks mode
(`-fweavec-checks=trap|report|verify|none`), the runtime
(`-f[no-]weavec-runtime`, `-f[no-]weavec-stack-objects`,
`-f[no-]weavec-global-objects`), zero-initialisation, the require level
(`-fweavec-require=none|guarded|checked|proven`), the ledger, the summary
line, the budgets (`-fweavec-budget=`, `-fweavec-unit-budget=`) and the
link step (`-fweavec-link=records|analyze|none`, `-fweavec-link-budget=`);
everything else is Clang's. `--version` prints Clang's version block, then
`weavec-cc version …`; a Darwin link with `-flto` gets `-lto_library`
naming the libLTO of the LLVM WeaveC was built with. The design is [RFC 0005](rfcs/0005-whole-program-analysis.md),
with the command line of RFC 0030 §16. The checked-mode flags, the analysis
cache, `--strict-externs`, `--exclusive-borrows`, `--analyze-headers` and
`--report-unannotated` are gone, with no aliases.

## Compile pipeline

`weavec-cc` compiles one C translation unit in five steps (RFC 0030 §1):

1. Clang parses the unit. `DeferredCodeGenConsumer` forwards Sema set-up and
   records every CodeGen callback without running it.
2. At `HandleTranslationUnit`, `AttributeReader` and the syntactic part of
   `KindInference` compute kinds and must-access requirements,
   `SiteCollector` enumerates the sites, and `SlotCollector` and `FnSlots`
   solve the unit's slots. The engine runs through `LedgerAdapter`, and
   `BoundaryInvariants` consumes what it published (the field-invariant
   rounds are cut; see `KindInference` above).
   `LedgerAdapter::finish` fills the defaults, propagates
   boundary rows and plans the checks; the planner's guard pass then turns
   the guardable unresolved facets into `guarded` (only the planner makes
   that outcome: the engine publishes `unresolved` with a reason, as
   before). This step runs in every mode.
3. Definite violations are reported as errors and possible temporal findings
   as warnings, with the require-level errors under `-fweavec-require`. In a
   build that enforces its guards (checks and the runtime on),
   `LedgerAdapter::dropGuardedPossible` first removes the possible findings
   linked to a guarded facet from the diagnostics and from the ledger; the
   row keeps its reason. `-Wweavec-possible` keeps them.
4. After an error, the callbacks are replayed unchanged and CodeGen drops the
   module. Otherwise, when checks are on, `CheckEmitter` applies the plan and
   the zero-initialisation lowering and, with the runtime, registers the
   unit's stack and global objects; then the callbacks are replayed,
   followed by the descriptors the registration added.
5. The object is written, then the format-31 record to `<object>.weavec`, the
   unit ledger to `-fweavec-ledger` if given, and the summary line under
   `-fweavec-summary` or `-fweavec-ledger`.

`weavec` runs steps 1–3 and 5 without CodeGen, per source or as one program
with `--whole-program`, and writes no object and no record. Its summary line
says `checkable (not enforced)` where a `weavec-cc` build would check and
`guardable (not enforced)` where one would guard, as does a `weavec-cc`
build with `-fweavec-checks=none`; without the runtime the guardable count
is 0 and the facets are counted as unresolved.

Refinement is split by facet. Spatial, null and assertion outcomes are
decided once per unit, because they decide the emitted code, and the link
step copies them verbatim. Temporal outcomes are refined at a link with
`-fweavec-link=analyze`, where calls into other units stop being unknown;
a default link copies them too.

## The link step

When `weavec-cc` links, it runs the link step (RFC 0030 §13.2) before the
linker; `weavec --whole-program` uses the same `ProgramAnalysis`.
`-fweavec-link=` selects how much of it runs (RFC 0033 §7): `records`, the
default, runs steps 1–3, 5 and 6 over the records alone, parsing and
analysing no unit again; `analyze` adds step 4; `none` runs nothing.

1. **Collect inputs.** `collectLinkInputs` resolves objects, archives,
   shared libraries and `-l` arguments as the linker does. One
   `unanalyzed-input` warning per link names every non-system input without
   a valid record. Calls into functions no record defines are then
   `trusted(external-unit)`.
2. **Solve slots** over all records.
3. **Verify declarations** against the defining units' summaries and kinds.
   A contradiction is an `annotation-mismatch` error.
4. **Re-run the engine** (`analyze` only) over the units with records, with
   the program database and the solved slots, to refine temporal facets.
   Each unit's `ObjectEngine` run knows the callees of other units by their
   format-30 summaries, and an indirect call nothing else resolves by the
   candidates of its type. Only the last round publishes. The run stops at
   `-fweavec-link-budget=<seconds>` of wall-clock time (default 120): a unit
   it did not finish, one it could not run again, and a group that did not
   converge (which takes its widened summaries) keep their compile-time
   rows, and the link prints a note. Only a definite violation fails the
   link, through the diagnostic engine, so `-Wno-error=` lowers it.
5. **Verify interfaces.** Exported requirements and the reliance on Single
   defaults are decided at cross-unit callers, header-struct invariants are
   checked against every unit that stores to the fields, boundary rows
   propagate, and a unit that defines the allocator is recorded under A5.
6. **Compose the program ledger** from the units' spatial, null and
   assertion facets, the Call rows of step 5 and the temporal facets of
   step 4, or, for a unit step 4 did not run, the rows its record carries.

Each unit's rows follow the flags it was compiled with, which its record's
header carries; the program ledger's `config.runtime` is `true`, `false`,
or `"mixed"` when the units were compiled differently.

What no record covers stays listed under assumptions A1 and A3. Archives,
shared libraries and ccache do not carry records yet (planned as RFC 0035),
but every link names the gap.

A unit record (§13.1) is one self-delimiting file, so that it can later be
placed verbatim into an object section:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `89 57 56 43 0D 0A 1A 0A` |
| 8 | 8 | format `u32` = 31, then flags `u32` = 0, little-endian |
| 16 | 32 | schema fingerprint: SHA-256 of the codec's field table |
| 48 | 16 | header length `H` and payload length `P`, `u64` each |
| 64 | `H` + `P` | header and payload, UTF-8 JSON |
| 64+`H`+`P` | 32 | SHA-256 of the bytes before it |

The header names the producer, source, `-cc1` command, target,
configuration and object digest. The payload holds the unit's functions
(linkage, address taken, type key, the format-30 summary in the field
`effects`, kinds that spell their source, reliance flags, exported
requirements and location), its globals, imports with their declared
parameters and call sites, indirect-call types and unknown callees, slot
rules, solved slots and slot kinds, field invariants, count fields,
boundary place classes, one compact row per site, the diagnostics already
reported and the §11 `a5` counts, but no evidence. Format 29 carries the
summaries in format 30 and drops the fields only the old engine read
(`summary`, `contexts`, `unknownIndirect`, `sizedFields`, `sizedFieldLoads`
and `interfaces`; RFC 0031 §7). Format 30 adds the `guarded/<reason>`
facet cell of a site row and the header's `config.runtime` (RFC 0032 §10).
Format 31 makes the site rows the unit ledger's rows in full, so that a
default link composes the program ledger without analysing again: each
facet's check, its requirement records, the site's text, its boundary and
its callee (RFC 0033 §7).
`lib/Frontend/RecordPayload.cpp`'s field
table is the authoritative list, and the schema fingerprint is derived from
it. The
field-invariant verdicts stay empty while §7.6 is cut; the boundary rows
carry `BoundaryInvariants`'s findings to the link, where they propagate
program-wide.
Readers accept only format 31 with a matching schema fingerprint and a valid
digest. Anything else is a stale record, and the input counts as having
none.

## The engine seam

Everything an engine produces flows through one interface (RFC 0030 §14),
which is how RFC 0031 replaced the engine by implementing `SafetyEngine`
without touching the ledger, kinds, library table, planner or emitter. The
types are in `include/weavec/Analysis/SafetyEngine.h`, `LedgerAdapter.h`
and `CheckWitness.h`.

`EngineInput` is everything an engine gets for one unit: the `ASTContext`,
the `SiteIndex`, the `KindTable` with what `KindInference` found besides it,
the `LibrarySpec`, the unit's slot constraints and their solution (local,
or program-wide at link through the database's program facts), the
`ProgramDatabase` at link, the field candidates assumed at entry, and
`EngineOptions` (budget, zero-initialisation, require level, verify mode,
strict aliasing, the functions to report, dump stream and statistics). The
engine does not know whether the build has a runtime: `guarded` is made
after it has published.
`LedgerAdapter` is the only channel back:

| Method | Carries |
| --- | --- |
| `beginFunction` | the start of a function's authoritative pass; rows from earlier passes are discarded |
| `decide` | one outcome, with reason and detail, for one facet of a known site; records merge by rank |
| `decideAs` | the same, naming the site kind and boundary, for a statement that stands for several sites (a call that does not return, and its exit) |
| `suggest` | the annotation a row's fix-it offers; the first suggestion of a pass stands |
| `requirement` | one requirement record of a LibCall, Release or Call facet, kept with its own outcome and check |
| `report` | a diagnostic with its certainty, linked to its site and facet |
| `witness` | what a check needs: the extent and whether it is exact or declared, the base, the offset or index, library lengths; in the shape `Object`, what a guard of an unresolved requirement compares (the need, or that the argument is a string), which never serves a static check |
| `boundary` | the places reachable from parameters and globals that may hold released pointers or aliased owners, with place classes; an owning cycle is an aliased-owner fact with its `cycle` flag (RFC 0031 §8) |
| `overBudget` | a function that exceeded its budget |
| `storeVerdict` | a store group's verdict on a field-invariant candidate: holds, violated or unknown (kept for §7.6; the object engine publishes none) |
| `finish` | fills the defaults, applies the unsafe, `setjmp`, concurrency and boundary rules and the field-invariant upgrades, plans the checks and, with the runtime, the guards, drops the possible findings the build enforces instead (`dropGuardedPossible`), reports the require-level errors, and returns the `PlannedLedger` |

An adapter is authoritative, discarding (summary rounds and early link
rounds keep nothing) or collecting (alias-context runs decide no row and
keep their diagnostics for the caller). A decision about a statement
`SiteCollector` did not enumerate is an internal error, and an
`unresolved(unanalysed)` row in a release build.

`SafetyEngine` is what an engine implements: `analyzeUnit(input, out)`,
`exports()` for the unit record, and `dump(function, os)` for
`--dump-analysis`. `ObjectEngine` implements it. `PlannedLedger` is the
unit's `core::Ledger` and `core::CheckPlan`, with the tables that resolve
the plan's handles and site ids.

Two rules keep the seam honest, and gate H2 (`scripts/check-hygiene.py`,
RFC 0031 §2) checks both:

- The engine publishes nothing except through `LedgerAdapter`, diagnostics
  included. It receives no `DiagnosticSink`.
- Only the `Engine*.cpp` files include `lib/Analysis/Engine.h`.
  `SiteCollector`, `AttributeReader`, `KindInference`, `SlotCollector`,
  `BoundaryInvariants`, `CheckPlanner` and `LedgerAdapter` itself never
  reach it.

## Heap postconditions (RFC 0013)

[RFC 0013](rfcs/0013-interprocedural-heap-state.md)'s constructors and
returned fields are ordinary summary content in the object engine. A result
pointing to an object the function created is a fresh result, and the
object's cells, down to the objects they point to, are stores below
`result`. A fresh value
names which of the function's new objects it is, so two fields initialised
from one allocation stay aliases in the caller, and two call sites create
independent objects (§6.3). A store of a new object keyed on a result
class is weak, and the object is absent on the other classes: a test of
the result that selects them disowns it, so a constructor whose failure
path stored nothing leaks nothing (RFC 0031 *Implementation amendments*). At a call the
callee's stores are read before any is written, so extraction
(`p = *slot; *slot = NULL; return p`) stays precise. Symbols are immutable,
so an allocation's extent is the size's value at allocation time whatever
the size variable holds later.

## Pointer identity and call effects (RFC 0014)

A function value is a symbol holding a bounded set of at most 32 functions,
or unknown (§4.1), and an indirect call resolves against it before the slot
solution. Function pointers stored in fields and globals are resolved by
the slots of RFC 0030 §9.3, which replace RFC 0014's callback-global
fixpoint and its `callbackGlobals` export. A call through a closed slot with
one target is applied as a direct call, and with several targets as the
join of their summaries. An open slot with known targets gives their
temporal facts only, under `trusted(extern-contract)`; an open slot without
targets gets the unknown-callee default with reason `callback`. Every
indirect call has a null facet on its callee operand. The old engine's
specialisation of a callee per callback binding is gone: a call through a
callback parameter takes the parameter's slot solution (RFC 0031
*Unresolved questions*).

Pointer arithmetic and pointer casts keep the symbol's targets, so
`free(p); use(p + 1)` is a use after free. `memcpy`, `memmove` and record
assignments copy pointer and integer leaves cell by cell; a pointer loaded
from a cell whose last store was of another type, or from a partial copy,
is `unresolved(raw-cast)` where it is used (§4.2).

## Arrays and containers (RFC 0015)

Array elements are cells of their object (RFC 0031 §4.9): a constant index
is a concrete cell, an affine index a selected cell over its index symbol
(at most 32 per object), and stores the engine cannot name go to the
element position's summary cell. Segments `[from, to) ↦ v` (at most 4 per
element position) describe ranges of elements, each holding its own value;
at a loop head the element cells an iteration changed fold into a segment
that grows with the induction variable, so `for (i = 0; i < n; i++)
free(a[i]);` releases `[0, n)`. A join that cannot keep a selected cell or
match a segment evicts it as a weak store to the elements it described.
Summaries carry element effects as `elements=` ranges over constants or
integer parameters the body never assigns; a range that cannot be so
expressed is exported without one, as a possible effect on some elements.
`realloc`'s new block starts with the old block's cells and ranges below
its size.

## Compositional calls (RFC 0016)

A summary is derived assuming distinct parameter objects except where
D1–D6 cannot separate them, where the entry objects are joined already and
the summary holds for aliased calls. Where a caller passes arguments that
point into one object, the callee is re-analysed in an alias context (§6.6;
see *Calls* above): its entry objects unified, the call's constant integer
arguments bound, at most 16 contexts per callee and 3 deep. A context run
never decides the rows of the function it analyses (§2.6). It keeps its
diagnostics: each temporal one is reported at the use in the callee with a
note naming the call, and linked to that call's Call site. Contexts are
unit-local; the cross-unit context requests of RFC 0031 §7 are not
implemented, and the cases that need them are listed in
`test/cases/KNOWN-DIFFERENCES.md`.

## Target integers and compositional bounds (RFC 0017)

[RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) specifies
the target-integer model. Core represents integer types of 1 to 64 bits:
`IntegerValue` is an unsigned bit pattern, `IntegerRange` holds at most two
intervals, and transfers model unsigned wrap, signed validity and
conversions. An invalid operation supplies no invented value. The object
engine keeps each integer symbol's interval in its type's range and the
relations between symbols in the zone: `a = b + c` with a constant `c`
records `a − b = c`, other arithmetic computes intervals only, and a
comparison of an access against an extent with the same scale is a zone
query (§4.4). `EngineExpr.cpp` evaluates arithmetic, compound assignments
in their promoted type, conversions and the overflow builtins;
`invalid-integer-operation` is reported when the operands make the
operation invalid for every value (§5.10). A declaration or typedef of a
variable-length array captures its dimensions where it runs, and `sizeof`
and extents use the captured values; a subscript of a multi-dimensional
array is bounded by its own dimension (RFC 0031 *Implementation
amendments*).

C values and byte intervals are distinct: `malloc(n * sizeof(T))` receives
the actual C product, and a byte size that may wrap `size_t` gives no
extent, so a wrapped product can establish a violation but never that an
access fits. The spatial verdict needs a lower and an upper bound to prove
an access. It maps onto the spatial facet (RFC 0030 §3.3): a violation
against an exact extent is an `out-of-bounds` error; an undecided access
against an exact or declared extent is checked when its terms are
expressible; an access that only a lower-bound kind covers is
`unresolved(unknown-extent)`. Call-site checks and errors come from the
must-access requirements of §7.5.

## Diagnostics contract

Every diagnostic carries a stable id from `weavec::core::diag`, printed as
`[weavec::<id>]`. Scripts and editors filter on ids, so renaming one is a
breaking change. There are 20:

| Ids | Default severity |
| --- | --- |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`, `mismatched-release`, `invalid-release` | error when definite, warning when possible |
| `null-dereference`, `use-of-uninitialized`, `out-of-bounds` | error, reported only when definite |
| `unsafe-operation`, `annotation-mismatch`, `invalid-integer-operation`, `contradicted-assumption` | error |
| `unresolved-operation` | error, only under `-fweavec-require=guarded`, `checked` or `proven`, or in a `WEAVEC_REQUIRE_SAFE` function; `guarded` allows guarded facets, the others report them as "guarded at run time only" |
| `unchecked-operation` | error, only under `-fweavec-require=proven` |
| `leak`, `invalid-annotation`, `unanalyzed-input` | warning (`leak` off by default in `weavec-cc`: `-Wweavec-leak`) |
| `allocation-failure` | warning, off by default (`-Wweavec-allocation-failure`) |

`diag::defaultSeverity(id, certainty)` gives these severities. Possible null
and spatial findings are checked facets rather than diagnostics. A possible
temporal finding on a facet the build guards is not reported unless
`-Wweavec-possible` is given (RFC 0032 §9); the `weavec` tool, which
enforces nothing, reports it. `-Werror`
in project flags does not promote WeaveC warnings; `-Werror=weavec[-<id>]`
does. An error can be lowered with `-Wno-error=weavec-<id>` but not
disabled; a lowered violation is guarded where the runtime can check it,
and otherwise checked or trapped unconditionally. RFC 0030 removed
`analysis-incomplete` (now unresolved rows, with reasons such as
`unanalysed` and `budget`), `annotation-required` (now
`unresolved(unknown-callee)` rows with fix-its), `checking-incomplete` and
`checking-failed`. A `-W` flag naming one of them is an error.

## Tests and gates

- **Unit tests** (`unittests/`, GoogleTest) test each component alone;
  `LibrarySpecTest.cpp` checks every library entry against an independent,
  hand-written expectation table, `HeapTest.cpp` builds the state a
  violation of each domain invariant I1–I6 (RFC 0031 §4.7) would produce and
  checks that the decision is not proven, and `EffectsIOTest.cpp`
  round-trips summary format 30. The Analysis tests run `ObjectEngine` over
  snippets (`unittests/Analysis/TestUtils.h`).
- **The runtime test** ([`runtime/test/rt_test.c`](../runtime/test/rt_test.c),
  CTest `runtime`) is one C program linked with `libweavec_rt.a` and
  `libweavec_alloc.a`, so its `malloc` is the arena's. It tests the size
  classes, alignment, reallocation, the quarantine, invalid releases (in
  child processes that must die), huge blocks, foreign pointers, the stack
  list under `longjmp` and deep recursion, the global table, strings,
  threads and `fork`.
- **Lit tests** (`test/Analysis`, `test/Annotations`, `test/Driver`,
  `test/WholeProgram`, `test/Prelude`, `test/Emission`) pin exact messages
  and driver behaviour; `test/Emission` holds the rewrite-oracle pairs,
  those of the guards, the range cache and the object registrations among
  them (`runtime-oracle-*.c`), and `test/Driver/runtime-*.c` pins the
  runtime's flags, link line and fallbacks.
- **`test/cases`** is one tree of executable C cases by feature, with
  expectations as line-comment markers (`BUG`, `TRAP`, `GUARDED`,
  `UNRESOLVED`, `CLEAN`, …; see its [README](../test/cases/README.md)).
  [`scripts/run-cases.py`](../scripts/run-cases.py) builds each case with
  `weavec-cc`, checks its diagnostics and ledgers, and runs it in trap and
  report mode, optionally under an ASan oracle. CTest registers one
  `cases-<suite>` test per top-level directory, so `ctest -j` runs the
  suites in parallel.
- **`test/corpus`** pins 9 real projects in 11 configurations, and 11
  held-out projects (`"heldOut": true`, RFC 0031 §11.2) that are measured
  and gated but never motivate an engine rule.
  [`scripts/corpus-gate.py`](../scripts/corpus-gate.py) runs `--quick` on
  every pull request, and `--full` (builds, the projects' own test suites,
  injected bugs, benchmarks) weekly and for releases. `expected.json` is a
  ratchet, and `triage.json` holds a verdict for every definite error and
  possible temporal warning, and the guard failures of a test suite that
  were triaged as true bugs. See its [README](../test/corpus/README.md).

[`scripts/codegen-identity.py`](../scripts/codegen-identity.py) implements
gate G7: with `-fweavec-checks=none`, objects are byte-identical to Clang's.
`scripts/check-hygiene.py` implements gate H2: no checked-mode remnants
outside `docs/rfcs/`, no libc name comparisons outside the library table,
the seam rules and the line budgets, the runtime's own among them (RFC 0032
gate H1). `run-cases.py` measures G1–G6, `test/Emission` G8, and
`corpus-gate.py` G9–G15 and RFC 0032's `rfc0032.R4` (the unresolved share
of each facet); G14 now bounds the run-time cost with the runtime, without
it, and the peak memory.

## Build structure

- The top-level `CMakeLists.txt` builds `lib/` (Core, Analysis, Frontend),
  `tools/`, `runtime/` and, with `WEAVEC_BUILD_TESTS`, `unittests/` and
  `test/`.
- `cmake/WeaveCLLVM.cmake` finds LLVM and Clang and provides the
  `weavec::llvm` interface target and `weavec_link_llvm`/`weavec_link_clang`,
  which respect `LLVM_LINK_LLVM_DYLIB` and `CLANG_LINK_CLANG_DYLIB`.
- `cmake/WeaveCHelpers.cmake` provides `weavec_add_library` and
  `weavec_add_executable`. Only `USES_LLVM` targets get LLVM's include
  paths, and Core is not one of them.
- `lib/Core/CMakeLists.txt` embeds `LibrarySpec.txt` as a byte array at
  configure time; editing the table re-runs the configure step.
- `runtime/CMakeLists.txt` builds the three archives into the build tree's
  `lib/weavec`, generating the helper bodies of `libweavec_chk.a` with the
  freshly built `weavec-cc`. `libweavec_rt.a` and `libweavec_alloc.a` are
  always compiled with `-O2` and without the allocation builtins, whatever
  the build type. With `WEAVEC_BUILD_TESTS` it also builds `weavec_rt_test`,
  which `test/CMakeLists.txt` registers as the CTest `runtime`.
- A CMake package config (`find_package(WeaveC)`) exports `weavec::Core`,
  `weavec::Analysis` and `weavec::Frontend` to external tools.
