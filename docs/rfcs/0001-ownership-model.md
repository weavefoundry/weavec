# RFC 0001: Ownership, borrowing and lifetimes for C

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-01
- **Tracking issue**: TBD
- **Supersedes / superseded by**: —

> This RFC was retro-fitted from the original `docs/design/ownership-model.md`
> design notes when the RFC process was introduced. It records the model the
> scaffolding was built against; the parts marked *deferred* are owned by
> later RFCs rather than being unimplemented details of this one.

## Summary

WeaveC assigns every pointer-typed storage location in a C program an
*ownership kind* (`Owned`, `Shared`, `Mutable`, `Raw`, or the not-yet-inferred
`Unknown`), tracks *moves* out of owned places, tracks *loans* against places
with Rust-style aliasing rules, and orders loans by *lifetimes* with an
`outlives` relation. Kinds form a lattice so that inference can be a fixpoint
and contradictory facts degrade to `Raw` rather than to silence. Anything the
model cannot justify must sit inside an explicitly `WEAVEC_UNSAFE` region.
This RFC fixes the vocabulary and the guarantees; the algorithms that
populate the model are specified by later RFCs (0002 for intra-procedural
checking, a future RFC for signature inference).

## Motivation

C's memory-safety failures (use-after-free, double-free, dangling pointers
into dead stack frames, aliasing writes through what the author believed was
a unique pointer) are almost always violations of an ownership discipline the
author had in mind but could not state. Rust made that discipline a language
feature; C cannot change, but a large fraction of well-written C already
follows the same rules and could be *checked* against them rather than
rewritten.

WeaveC's design goals, in priority order:

1. **Prove first, annotate second.** Infer the discipline the code follows
   and only ask for an annotation where inference is genuinely ambiguous,
   typically at ABI boundaries.
2. **Source compatibility.** Annotated code must compile unchanged with any C
   compiler. All annotations are `__attribute__((annotate(...)))` behind
   macros that expand to nothing elsewhere.
3. **Explicit unsafety.** Anything the model cannot justify must be inside a
   `WEAVEC_UNSAFE` function or block. The unsafe surface of a codebase should
   be small, greppable and reviewable.
4. **Incremental adoption.** A codebase can be made safe file by file. Unsafe
   is the default for unannotated external code, not an error.

The cost of having no shared model is that every checker rule would define
its own notion of "freed", "borrowed" and "escapes", and they would disagree
at the edges. This RFC is the contract those rules build on.

## Soundness

This RFC defines what the model *means*; it does not by itself catch
anything. The guarantee it commits later RFCs to is:

> In a translation unit with no `WEAVEC_UNSAFE` regions and no
> `annotation-required` diagnostics, WeaveC reports every use of a place
> after its owned resource was released or moved, every double release, every
> violation of the aliasing rules below, and every borrow that may outlive its
> referent — subject to the assumptions listed.

**Bugs in scope of the guarantee** (owned by RFC 0002 and successors):

- Read or write through a pointer after it was freed or moved
  (`use-after-free`, `use-after-move`).
- Releasing the same owned resource twice (`double-free`).
- A shared or mutable loan coexisting with a mutable loan on the same place,
  or a move/mutation of a place with a live loan (`conflicting-borrow`).
- A loan whose lifetime is not provably outlived by its referent's, e.g. a
  pointer to a local escaping the local's scope (`lifetime-too-short`).
- Use of a `Raw` pointer outside an unsafe region (`unsafe-operation`).

**Deliberately out of scope**, now and possibly for good:

- Bounds. WeaveC reasons about *which* object a pointer refers to, not
  *where within it*. Buffer overruns are a different tool's job (or a later,
  separate RFC).
- Concurrency. Data races are out of scope; the model assumes
  single-threaded execution within a function. Threads, signals and
  `setjmp`/`longjmp` make the surrounding code `Raw` until an RFC says
  otherwise.
- Uninitialised reads of non-pointer data, integer overflow, type punning.

**Accepted false positives.** The model is deliberately conservative at
control-flow merges: a place freed on *either* incoming path is treated as
freed afterwards, so `if (c) free(p); ... if (c) use(p);` is rejected even
though it is correct. Users resolve this by restructuring, by re-nulling the
pointer (`p = NULL` reinitialises the place), or with `WEAVEC_UNSAFE`. We
accept this because path-sensitive reasoning about arbitrary predicates is
where analysers become unpredictable, and predictability matters more to
adoption than precision on this pattern.

**Assumptions.**

- Callee summaries (inferred or annotated) are trusted. A wrong
  `WEAVEC_BORROWED` on a function that actually frees its argument defeats
  the checker at every call site; this is the same trust Rust places in
  `unsafe` code behind a safe signature.
- Unannotated external functions receive `Raw` parameters, so calling them
  from safe code is an error rather than a hole.
- Memory is only released through recognised release functions (initially
  `free`; user allocators via annotation). Releasing through an unannotated
  function is invisible.

## Detailed design

### Places

A *place* is a storage location the analysis reasons about: a local, a
parameter, a global, or (deferred to RFC 0002) a field path such as
`node->next` or an array summary `a[*]`. Places are interned per analysis
unit as `core::PlaceId` by `core::PlaceTable`, which also carries the display
name used in diagnostics. The core model never sees a Clang declaration; the
Analysis layer owns the `ValueDecl → PlaceId` mapping.

### Ownership kinds

Each pointer-typed place has an `OwnershipKind`:

| Kind      | Meaning                                                                   | Rust analogue |
| --------- | ------------------------------------------------------------------------- | ------------- |
| `Owned`   | Unique ownership; must be released exactly once; may be moved.            | `Box<T>`      |
| `Shared`  | Read-only borrow; any number may coexist while no mutable borrow is live. | `&T`          |
| `Mutable` | Exclusive borrow; no other access to the place while it is live.          | `&mut T`      |
| `Raw`     | No guarantees. Legal only inside `WEAVEC_UNSAFE`.                         | `*mut T`      |
| `Unknown` | Not yet inferred (lattice bottom).                                        | —             |

Kinds form a lattice with `Unknown` at the bottom, `Raw` at the top and the
three safe kinds pairwise incomparable:

```
           Raw
        /   |   \
   Owned  Shared  Mutable
        \   |   /
         Unknown
```

`join` (`Ownership.h`) is the least upper bound: `join(x, x) = x`,
`join(Unknown, x) = x`, and any two distinct concrete kinds join to `Raw`.
Inference is a fixpoint over this lattice. A place that joins to `Raw`
because of contradictory facts is reported unless its uses are unsafe, so an
inference failure is always visible rather than silently permissive.

Design decision: the safe kinds are *incomparable*, not ordered. One could
argue `Owned` should sit above `Mutable` above `Shared` (an owner can do
anything a borrower can). We reject that because it would let inference
silently *upgrade* a borrow to ownership at a merge point, turning a missed
`free` into a false sense of safety. Contradiction should escalate to `Raw`
and be reported.

### Moves

`free(p)`, passing an owned pointer to a parameter annotated `WEAVEC_OWNED`,
returning it from a function whose return is `WEAVEC_OWNED`, or assigning it
to another owned place *moves* ownership out of `p`. A moved place is
uninitialised until it is reassigned. Reading it is `use-after-free` (if the
move was a release) or `use-after-move`; moving it again is `double-free`.

`core::MoveTracker` records `PlaceId → MoveRecord{reason, location}`. The
record carries the location of the move so the diagnostic can point at both
the use and the earlier release. `reinitialize(place)` clears the record;
plain assignment (`p = expr`) reinitialises. `join` is set union ("may be
moved"), giving the conservative merge described under *Soundness*.

Design decision: moves are tracked per *place*, not per *value*. If `q = p;
free(q);` then `p` is still a distinct place and a later `use(p)` is a
use-after-free that this RFC's vocabulary can express (`p` and `q` alias the
same owned resource) but that requires RFC 0002 to actually detect by
recording the copy as a transfer. Tracking values rather than places would
make the model closer to a points-to analysis and correspondingly less
predictable; we start from places and add alias facts explicitly.

### Borrows and loans

Taking the address of a place, array-to-pointer decay, or passing an owned
pointer to a `WEAVEC_BORROWED` / `WEAVEC_MUT` parameter creates a *loan*
(`core::Loan{place, kind, lifetime, location}`) against the place.
`core::BorrowState` holds the live loans and enforces:

- any number of `Shared` loans may coexist;
- a `Mutable` loan excludes every other loan of the same place;
- a place with any live loan may not be moved or mutated directly.

A refused `addLoan`, `checkMove` or `checkMutation` yields a
`BorrowConflict{existing, attempted}` so the diagnostic can show both sides.
Loans end either when their lifetime expires (`expire(lifetime)`) or when the
place is released (`release(place)`).

Design decision: these are Rust's *lexical* borrow rules, not NLL. A loan
lives until its lifetime expires, not until its last use. This is simpler,
matches how the scopes-as-lifetimes scheme below assigns lifetimes, and
produces false positives of the "borrow is still alive but never used again"
kind that Rust 2015 users know well. Moving to last-use liveness is an
explicit future RFC once the CFG dataflow exists to compute it.

### Lifetimes

Every loan has a `core::LifetimeId`. `LifetimeId{0}` is `'static` and
outlives everything. `core::LifetimeConstraints` allocates fresh lifetimes
and records `addOutlives(longer, shorter)` constraints (`'a: 'b`); `outlives`
answers the transitive query. A loan whose lifetime cannot be shown to be
outlived by its referent's lifetime is `lifetime-too-short`.

Lifetimes are *regions*, not points: in the first instance every lexical
scope gets a lifetime, a local's lifetime is its scope, a parameter's is the
function body, a heap allocation's is from allocation to release, and a
global's is `'static`. Constraints are generated from scope nesting
(`outer: inner`), from assignments (`assigning a loan into a place with
lifetime 'p requires loan lifetime: 'p`) and from calls (via summaries).

### Unsafe

`WEAVEC_UNSAFE` on a function definition skips analysis of its body but the
function's *signature* still applies to callers: an unsafe function with a
`WEAVEC_OWNED` parameter still consumes what is passed. `WEAVEC_UNSAFE {
... }` on a compound statement skips the block and treats every pointer that
escapes it (is assigned to, or read from, a place outside the block) as
`Raw`. Unsafe never silences diagnostics *about* the code around it; it only
disables checks *inside* it.

### Allocation and release functions

Recognised by name initially: `malloc`, `calloc`, `realloc`, `strdup`
produce `Owned`; `free` consumes `Owned`. Users teach WeaveC about their own
allocators by annotating declarations (`WEAVEC_OWNED` on the return type for
producers, `WEAVEC_OWNED` on the consumed parameter for releasers). There is
no separate "allocator" annotation; an allocator is just a function whose
summary says it hands out or takes ownership. The name-based list exists only
because libc headers cannot be annotated.

### Inference, in outline

Inference runs per translation unit in three phases. This RFC fixes the
phase boundaries and what flows between them; each phase gets its own RFC.

1. **Local inference** (RFC 0002). Within a function, build a `clang::CFG`,
   map declarations to places, and run a forward dataflow whose state is
   `(MoveTracker, BorrowState, LifetimeConstraints)`. Allocation functions
   produce `Owned`; release functions consume `Owned`; address-of and array
   decay produce loans. Merge with `join`.
2. **Signature inference** (future RFC). Derive a *summary* for each
   function: the kind of each pointer parameter and return value, plus
   outlives relations between them. Summaries are computed bottom-up over
   the call graph and cached. Unannotated external functions get `Raw`
   parameters.
3. **Annotation reconciliation** (future RFC). Where the user annotated a
   declaration, the annotation is authoritative and inference must agree;
   disagreement is a diagnostic pointing at both. Where inference is
   ambiguous and there is no annotation, `annotation-required` is reported
   (opt-in today via `--report-unannotated`, on by default later).

Whole-program refinement (summaries flowing across TUs via a summary database
next to the compilation database) is a later milestone.

## Annotation surface

This RFC introduces the header `resources/include/weavec.h` with five
macros. Each attribute macro expands to
`__attribute__((annotate("weavec.<name>")))` when `__has_attribute(annotate)`
and to nothing otherwise, so annotated code is portable C.

| Macro             | Attaches to                                    | Meaning                                       |
| ----------------- | ---------------------------------------------- | --------------------------------------------- |
| `WEAVEC_OWNED`    | pointer parameters, returns, variables, fields | Place has kind `Owned`.                       |
| `WEAVEC_BORROWED` | pointer parameters, returns, variables, fields | Place has kind `Shared`.                      |
| `WEAVEC_MUT`      | pointer parameters, returns, variables, fields | Place has kind `Mutable`.                     |
| `WEAVEC_UNSAFE`   | function definitions, compound statements      | Disable checking inside; see *Unsafe*.        |
| `WEAVEC_ENABLED`  | (object-like macro)                            | `1` under `weavec`, `0` under other compilers. |

Annotations are *authoritative*, not hints: inference must agree with them
or report a disagreement. Spellings live in exactly two places that must
match: `include/weavec/Analysis/Annotations.h` (`spelling::`) and the header.
`docs/annotations.md` is the user-facing reference.

## Diagnostics

This RFC reserves the following ids in `weavec::core::diag`. Ids are part of
the user-facing contract (they are printed as `[weavec::<id>]` and will be
filterable); renaming one is a breaking change. Which RFC makes each one
*emitted* is noted.

| Id                    | Severity | Meaning                                             | Emitted by                 |
| --------------------- | -------- | --------------------------------------------------- | -------------------------- |
| `use-after-free`      | error    | Read of a place after its resource was released.    | scaffold checker; RFC 0002 |
| `double-free`         | error    | Second release of a place without reinitialisation. | scaffold checker; RFC 0002 |
| `use-after-move`      | error    | Read of a place after ownership moved out.          | RFC 0002                   |
| `conflicting-borrow`  | error    | Loan or access violates the aliasing rules.         | RFC 0002                   |
| `lifetime-too-short`  | error    | Loan may outlive its referent.                      | RFC 0002                   |
| `unsafe-operation`    | error    | `Raw` pointer used outside `WEAVEC_UNSAFE`.         | future RFC                 |
| `annotation-required` | warning  | Ownership not inferable; annotate.                  | scaffold (opt-in)          |
| `invalid-annotation`  | warning  | Unrecognised `weavec.*` annotation.                 | scaffold                   |

Message shapes fixed by the scaffold and to be preserved:
`use of '<p>' after it was freed` with note `freed here`;
`'<p>' is freed twice` with note `previously freed here`.

## Drawbacks

- **False positives at merges.** The may-moved join and lexical loans will
  reject correct code. Adoption on existing codebases will be gated on how
  much restructuring or `WEAVEC_UNSAFE` that demands; this is a tracked
  metric (see roadmap, *Ongoing*).
- **Trusting summaries.** A single wrong annotation on a widely-called
  function silently weakens the guarantee across the codebase. Mitigation
  (reconciliation against inferred behaviour) is a later RFC.
- **Place granularity.** Reasoning per place rather than per value means
  aliasing through copies must be modelled explicitly, and interior pointers
  (into arrays and structs) need summary places that lose precision.
- **A Rust-shaped model on a non-Rust language.** C idioms with no Rust
  analogue (intrusive lists, arena allocation, `container_of`, pointer
  arithmetic across objects) do not fit and will land in `Raw`/unsafe. The
  bet is that these are a small, reviewable fraction of typical code.

## Alternatives

- **Points-to / alias analysis with escape tracking** (the classical
  approach; also what Clang's static analyzer `MallocChecker` does with
  symbolic values). More precise on aliasing and path conditions, but
  results depend on analysis budgets and heuristics, so the same code can
  pass or fail depending on context. WeaveC prioritises predictability: a
  user should be able to say *why* a program was accepted.
- **Type-based ownership only** (annotate every pointer type, no inference;
  roughly Checked C's or GSL `owner<T>`'s position). Sound and simple, but
  contradicts goal 1: it requires rewriting every signature before any
  benefit is seen.
- **Rust-style ordering of the safe kinds** in the lattice. Rejected above:
  silent upgrades hide errors.
- **Non-lexical lifetimes from day one.** Rejected as premature: NLL needs
  the CFG-based liveness that RFC 0002 introduces, and the lexical model is
  a strict subset of it, so nothing accepted now becomes rejected later.
- **Do nothing** (rely on ASan/Valgrind at run time). Dynamic tools only
  find bugs on paths that are executed; the point of WeaveC is the
  guarantee on the paths that are not.

## Prior art

- **Rust** ([The Rust Reference, *Ownership* and *References*]; RFC 2094
  *Non-lexical lifetimes*; the Polonius project). The kinds, the aliasing
  rules and the "moved-out place is uninitialised" rule are taken directly.
  What does not transfer: Rust has the type system enforce that every
  pointer *is* one of these kinds; WeaveC must infer it and needs `Raw` and
  `Unknown` to describe the gap.
- **Cyclone** (Jim, Morrisett et al., 2002). Region-based lifetimes for a
  C dialect, with region inference to reduce annotation burden. WeaveC's
  scope-as-region scheme and `'static` as a distinguished region come from
  here. Cyclone changed the language; WeaveC cannot.
- **Checked C** (Microsoft). Demonstrates that a C superset with explicit
  pointer kinds can be adopted incrementally, and that unannotated
  interop boundaries are where most of the friction lives, which is why
  `annotation-required` exists.
- **Clang** `-Wdangling`, `[[clang::lifetimebound]]`, and the in-progress
  lifetime-safety analysis. Show what a compiler can infer intra-procedurally
  with lexical scopes and no whole-program view; also show that without an
  ownership notion, "dangling" checks stop at function boundaries.
- **C++ Core Guidelines lifetime profile / GSL `owner<T>`.** A
  single-annotation ownership scheme for C++ raw pointers; the closest
  existing analogue to `WEAVEC_OWNED`. The profile's experience is that
  inference for *borrowed vs. owned* on return values is the hard case,
  which informs the signature-inference RFC.
- **Typestate** (Strom & Yemini, 1986) is the underlying formalism for
  `MoveTracker`: a place is in state *live* or *moved*, and operations are
  legal only in certain states.

## Unresolved questions

Recorded honestly so the next RFC does not rediscover them.

- **Aliasing through `void *`.** `void *` is how C expresses both generic
  ownership transfer (`malloc` returns one) and type erasure for callbacks
  and containers. Does a `void *` place carry a kind like any other pointer,
  or is every cast to/from `void *` a loan/move boundary? Current lean: it is
  an ordinary place; casts are transparent; the *callee* summary decides.
- **Copies as aliases.** `q = p` for two `Owned` places: is this a move (Rust
  semantics; `p` is dead afterwards) or does it create two places aliasing
  one resource, only one of which may free? Rust semantics are simpler and
  sound but reject the common `tmp = p; p = p->next; free(tmp);` only if
  written the other way round. *Resolved by RFC 0002: aliases, via a
  union-find partition in the dataflow state.*
- **`realloc`.** A move that may or may not free, and whose result may alias
  its argument. Model as "consume `Owned` argument, produce fresh `Owned`
  result"; on failure (`NULL` result) the argument is *still live*, which
  the model has no way to say without path sensitivity on the result.
  *Resolved by RFC 0002: consume and produce, with the argument reinstated
  on the null edge of a direct test of the result.*
- **Interior pointers.** `&s->field`, `&a[i]`, pointer arithmetic. The
  intended answer is a summary place per field path and one per array, with
  loans against the summary place, which loses the ability to have disjoint
  mutable borrows of two fields. Whether that is acceptable is an empirical
  question for corpus testing.
- **Function pointers and callbacks.** The kinds of a function pointer's
  parameters must come from the pointer *type*, which C cannot annotate
  portably (attributes on function types are fragile). Options: annotate
  the `typedef`; require callbacks to be `WEAVEC_UNSAFE`; infer from every
  function assigned to the pointer. Undecided.
- **Inference across translation units.** Summaries for functions defined in
  another TU are not available during a single-TU analysis. The plan is a
  summary database keyed by mangled name next to `compile_commands.json`,
  with unannotated externals defaulting to `Raw` until it is populated. Open:
  staleness detection, and what happens when two TUs infer different
  summaries for the same function (link-time reconciliation vs. error).
- **Globals and statics.** A global `Owned` pointer has no scope end at which
  to require release. Is a never-freed global a leak WeaveC reports, or
  legitimately `'static`? Current lean: `'static` and not reported; leak
  detection is not a goal.
- **`setjmp`/`longjmp`, signals, threads.** All `Raw` today. Whether any of
  them can be brought inside the model is not planned.
- **Is `Raw` really the top?** Should there be a distinct *error* element
  (contradiction) separate from *unsafe by choice* (`Raw` via
  `WEAVEC_UNSAFE`), so that diagnostics can distinguish "you opted out"
  from "inference failed"? The `MoveReason`-style tagging on the record
  might be enough without changing the lattice.

## Future work

- **RFC 0002** — sound intra-procedural checking: the CFG dataflow that
  drives `MoveTracker`, `BorrowState` and `LifetimeConstraints`, recognised
  allocators, field-sensitive places, and the first emitted
  `use-after-move`, `conflicting-borrow` and `lifetime-too-short`.
- **Signature inference RFC** — per-function summaries, bottom-up over the
  TU call graph; annotation reconciliation; `annotation-required` on by
  default.
- **Unsafe blocks RFC** — escape rules for `WEAVEC_UNSAFE { }` and
  `unsafe-operation`.
- **Non-lexical loans** once liveness is available from the CFG dataflow.
- **Cross-TU summaries** and shipped summaries for libc.

## Implementation status

Snapshot at the time this RFC was accepted; kept current by the PRs that
change it.

| Component            | Status                                                                 |
| -------------------- | ---------------------------------------------------------------------- |
| Ownership lattice    | Implemented (`Ownership.h`), unit-tested.                              |
| Places               | Implemented for locals and parameters. Field paths: RFC 0002.          |
| Moves / free tracking | Implemented (`MoveTracker`); used by the scaffold checker.            |
| Borrow state         | Implemented and unit-tested; not yet driven by the AST walk.           |
| Lifetime constraints | Implemented and unit-tested; not yet driven by the AST walk.           |
| Local checker        | Path-insensitive AST walk with branch join; loops analysed once.       |
| CFG dataflow         | RFC 0002.                                                              |
| Signature inference  | Future RFC.                                                            |
| Annotations          | Parsed and honoured for `unsafe`; ownership annotations recorded only. |
| Cross-TU summaries   | Future RFC.                                                            |
