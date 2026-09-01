# Ownership model — design notes

This document records the intended semantics of WeaveC's ownership, borrowing
and lifetime model and how the current implementation maps onto it. It is a
living design document; sections marked *planned* are not implemented yet.

## Goals

1. **Prove first, annotate second.** Most well-written C already follows an
   ownership discipline; WeaveC should infer it and only ask for annotations
   where inference is genuinely ambiguous (typically at ABI boundaries).
2. **Source compatibility.** Annotated code must compile unchanged with any C
   compiler. All annotations are `__attribute__((annotate(...)))` behind
   macros in `weavec.h` that expand to nothing elsewhere.
3. **Explicit unsafety.** Anything the model cannot justify must be inside a
   `WEAVEC_UNSAFE` function or block. The unsafe surface of a codebase should
   be small, greppable and reviewable.
4. **Incremental adoption.** A codebase can be made safe file by file. Unsafe
   is the default for unannotated external code, not an error.

## Core concepts

### Places

A *place* is a storage location the analysis reasons about: a local, a
parameter, a global, or (planned) a field path such as `node->next`. Places
are interned per analysis unit as `core::PlaceId`.

### Ownership kinds

Each pointer-typed place has an `OwnershipKind`:

| Kind      | Meaning                                                                             | Rust analogue |
| --------- | ----------------------------------------------------------------------------------- | ------------- |
| `Owned`   | Unique ownership; must be released exactly once; may be moved.                      | `Box<T>`      |
| `Shared`  | Read-only borrow; any number may coexist while no mutable borrow is live.           | `&T`          |
| `Mutable` | Exclusive borrow; no other access to the place while it is live.                    | `&mut T`      |
| `Raw`     | No guarantees. Legal only inside `WEAVEC_UNSAFE`.                                   | `*mut T`      |
| `Unknown` | Not yet inferred (lattice bottom).                                                  | —             |

Kinds form a lattice with `join` (`Ownership.h`). Inference is a fixpoint
over this lattice; contradictory facts about a place join to `Raw`, which the
checker reports unless the use is unsafe.

### Moves

`free(p)`, passing an owned pointer to a parameter annotated `WEAVEC_OWNED`,
or assigning it to another owned place *moves* ownership out of `p`. A moved
place is uninitialised until it is reassigned. Using it is
`use-after-free` / `use-after-move`; moving it again is `double-free`.
`MoveTracker` implements this with a conservative "may be moved" join at
control-flow merges.

### Borrows and loans

Taking a pointer to a place (or passing an owned pointer to a `WEAVEC_BORROWED`
/ `WEAVEC_MUT` parameter) creates a *loan* against the place for some
lifetime. `BorrowState` enforces the aliasing rules: shared loans coexist;
a mutable loan excludes all others; a place with any live loan cannot be
moved or directly mutated. Violations are `conflicting-borrow`.

### Lifetimes

Every loan has a `LifetimeId`. Inference generates `outlives` constraints
(`'a: 'b`) from scopes, assignments and calls; `LifetimeConstraints` answers
transitive queries. A loan whose lifetime cannot be shown to be outlived by
its referent's lifetime is `lifetime-too-short`. Lifetime `0` is `'static`.

## Inference (planned)

Inference runs per translation unit in three phases:

1. **Local inference.** Within a function, build a `clang::CFG`, map
   declarations to places, and run a forward dataflow analysis whose state is
   `(MoveTracker, BorrowState, LifetimeConstraints)`. Allocation functions
   produce `Owned`; `free`-like functions consume `Owned`; address-of and
   array decay produce loans. Merge with `join`.
2. **Signature inference.** Derive a *summary* for each function: the kind of
   each pointer parameter and return value, plus outlives relations between
   them. Summaries are computed bottom-up over the call graph and cached.
   Unannotated external functions get `Raw` parameters (safe default: their
   callers must be unsafe or annotate the declaration).
3. **Annotation reconciliation.** Where the user annotated a declaration, the
   annotation is authoritative and inference must agree; disagreement is a
   diagnostic pointing at both. Where inference is ambiguous and there is no
   annotation, `annotation-required` is reported (opt-in today via
   `--report-unannotated`, on by default later).

Whole-program refinement (summaries flowing across TUs via a summary database
next to the compilation database) is a later milestone.

## Allocation and release functions

Initially recognised by name: `malloc`, `calloc`, `realloc`, `strdup`,
`free`. Users will be able to teach WeaveC about their own allocators by
annotating declarations (`WEAVEC_OWNED` on the return type, `WEAVEC_OWNED` on
the consumed parameter) — the same mechanism as for any other function.

## Unsafe

`WEAVEC_UNSAFE` on a function skips analysis of its body but still applies
its *signature* to callers. `WEAVEC_UNSAFE { ... }` on a block skips the block
and treats all pointers escaping it as `Raw`. Unsafe never silences
diagnostics *about* the code around it; it only disables checks *inside* it.

## Current implementation status

| Component              | Status                                                                   |
| ---------------------- | ------------------------------------------------------------------------ |
| Ownership lattice      | Implemented (`Ownership.h`), unit-tested.                                |
| Places                 | Implemented for locals and parameters. Field paths: planned.             |
| Moves / free tracking  | Implemented (`MoveTracker`); used by the local checker.                  |
| Borrow state           | Implemented and unit-tested; not yet driven by the AST walk.             |
| Lifetime constraints   | Implemented and unit-tested; not yet driven by the AST walk.             |
| Local checker          | Path-insensitive AST walk with branch join; loops analysed once.         |
| CFG dataflow           | Planned; replaces the AST walk.                                          |
| Signature inference    | Planned.                                                                 |
| Annotations            | Parsed and honoured for `unsafe`; ownership annotations recorded only.   |
| Cross-TU summaries     | Planned.                                                                 |

## Open questions

- How to model `realloc` (a move that may or may not free).
- Interior pointers into arrays and structs: place granularity vs. precision.
- Function pointers and callbacks: require annotations on the pointer type?
- Interaction with `setjmp`/`longjmp`, signals, and threads (initially: unsafe).
