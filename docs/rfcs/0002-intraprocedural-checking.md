# RFC 0002: Sound intra-procedural checking

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-01
- **Accepted**: 2026-09-01
- **Implemented**: 2026-09-01
- **Tracking issue**: TBD
- **Supersedes / superseded by**: — (implements phase 1 of RFC 0001)

## Implementation notes

The design below landed as written except where this section says
otherwise. Each item is a clarification of *how* a decision was built, not a
change of decision; where the text below disagrees with an item here, the
item wins. Behaviour is pinned by `test/Analysis/rfc0002-*.c` and
`unittests/Analysis/DataflowTest.cpp`.

- **Alias relation instead of a partition.** `core::AliasRelation` replaced
  the proposed `core::Partition` (union-find). The join of two *equivalence*
  relations is not the union of the relations but its transitive closure,
  and that closure invented aliases between pointers that never named the
  same object on any single path. Concretely, walking a list with
  `prev`/`cur`/`cur->next` in a loop merged every node into one class after
  two iterations and the canonical unlink-and-free idiom reported
  `use-after-free`. `AliasRelation` is a symmetric may-alias graph: `unite`
  relates two places *and* each with the other's current aliases (so it is
  closed under copies within a path), `separate` removes a place from every
  edge, and `join` is a plain union of edges. Everything the RFC says about
  "alias classes" reads as "the aliases of" in the implementation
  (`AliasRelation::members`). Same operations, same soundness argument (a
  fact about a place is applied to every place that may alias it on some
  path), strictly fewer false positives.
- **Dereference is a path step.** `p->next` is the place `*p` `.next`, with
  `*` an explicit `Deref` step (`PathStep::Deref`) rather than "the alias
  class of `p`". Facts about the objects under `*p` are *mirrored* onto
  `*q` when `q = p` copies the pointer, and queries expand a place to the
  same path under every alias of each dereferenced pointer on its path
  (`FunctionDataflow::mirrors`). Synthesised mirror paths are capped at
  depth 8 so cyclic structures cannot grow the place table without bound;
  paths written in the source are never truncated. The rule runs both
  ways: a whole write to `p->x` (`reinit`) also drops the move records of
  its mirrors `q->x`, since it replaces what that cell held under every
  name (`reinitMirrors`, added with RFC 0007). Before that, `free(L->stack);
  L->stack = fresh` with `L->twups ~ L` left `L->twups->stack` freed
  forever, and every second call was a `double-free`.
- **`reallocs` maps the result to the consumed places** (the argument and
  its aliases at the call), not to a class id, so the entry stays meaningful
  after the relation changes. Join semantics are as specified.
- **Allocator list.** `posix_memalign` is not modelled; it lands with the
  signature-inference work. Recognition requires global linkage, so a
  `static` helper named `free` is *not* libc's (the "shadow is treated as
  libc" limitation only applies to global redefinitions).
- **Iteration cap.** 64 visits per block, always on: a block past the cap is
  dropped from the worklist and analysis continues from the states already
  computed. Every state component is a finite lattice so the cap is a guard
  against non-monotone bugs, not a budget.
- **Direct `return &x`** is reported as `returned pointer may outlive '<x>',
  which it points to` since there is no holder to name; the `'<x>' goes out
  of scope here` note is omitted for returns.
- **`--dump-analysis` format** (unstable, see *Debug output*):

  ```
  function 'f':
    places: p (param, unknown) c (param) x (local) a (local, mutable)
    lifetimes: caller fn
    exit: moved{p->buf@8:10 freed} loans{} aliases{}
  ```

  Loans and aliases held by locals are already dropped at function exit, so
  `exit:` shows what escaped: moved places, and loans/aliases through
  parameters and globals.

## Summary

Replace the scaffold's path-insensitive AST walk with a forward dataflow
analysis over `clang::CFG` whose state is the triple
`(MoveTracker, BorrowState, LifetimeConstraints)` from RFC 0001, plus an
alias partition over places and a narrow `realloc` fact. Recognise the libc
allocation and release functions, treat copies of owned pointers as aliases
of one resource, accept the canonical `realloc` failure idiom, drive
loans from address-of and array decay, and assign lifetimes to lexical scopes
so that pointers escaping a scope are caught. After this RFC lands WeaveC
emits `use-after-move`, `conflicting-borrow` and `lifetime-too-short` for the
first time, handles loops, `switch`, `goto` and short-circuit evaluation
soundly, and gains `--dump-analysis` for inspecting inferred facts. Function
signatures are still not inferred; every function is analysed in isolation
with parameters treated per RFC 0001's defaults.

## Motivation

The scaffold checker (`LocalOwnershipChecker`) exists to prove the pipeline
runs end to end. It is not sound: loop bodies are analysed once, so a `free`
on the second iteration of a loop that reads the pointer at the top is
missed; `switch`, `goto`, `?:` and `&&`/`||` are walked as flat child lists;
`q = p; free(q); use(p)` is accepted because moves are per place with no
aliasing; nothing ever creates a loan or a lifetime, so `BorrowState` and
`LifetimeConstraints` are dead code outside their unit tests. Every rule
added on top of the AST walk would have to be rewritten once the dataflow
exists, so the dataflow has to come first. This RFC is the last piece of
infrastructure before WeaveC can be pointed at real code.

## Soundness

**Bugs caught after this RFC**, all within a single function body, given the
assumptions below:

```c
void loop(void) {                 /* use-after-free across an iteration */
  char *p = malloc(8);
  for (int i = 0; i < 2; i++) {
    p[0] = 0;                     /* error on iteration 2                */
    free(p);
  }
}

void alias(char *WEAVEC_OWNED p) {/* use-after-free through an alias    */
  char *q = p;
  free(q);
  p[0] = 0;                       /* error: resource freed via 'q'       */
}

void sw(int k, char *WEAVEC_OWNED p) {
  switch (k) {
  case 0: free(p);                /* falls through                        */
  case 1: free(p);                /* error: double-free on the k==0 path  */
  }
}

int *escape(void) {               /* lifetime-too-short                  */
  int x = 0;
  int *p = &x;
  return p;                       /* error: 'x' does not live long enough */
}

void borrow(void) {               /* conflicting-borrow                  */
  int x = 0;
  int *a = &x;                    /* mutable loan of x                   */
  int *b = &x;                    /* error: second mutable loan          */
  *a = *b;
}
```

Also caught: `use-after-move` when an owned pointer is passed to a
`WEAVEC_OWNED` parameter and then used; `double-free` via any alias; a
`free` of a place with a live loan (`conflicting-borrow`); use of a pointer
after a `goto` back past its `free`.

**Deliberately not caught by this RFC:**

- Anything requiring a callee's behaviour. Calls to functions without
  annotations are treated per RFC 0001: pointer arguments to unannotated
  callees are *not* moved, *not* loaned, and the callee is assumed not to
  free or retain them. This is a known hole that the signature-inference RFC
  closes; until then the scaffold's `--report-unannotated` is the mitigation.
- Leaks. An `Owned` local that goes out of scope without being released is
  not reported. RFC 0001 says "exactly once"; enforcing the "at least once"
  half needs a decision about error paths and is a separate RFC.
- Bounds, arithmetic across objects, concurrency (RFC 0001, *Soundness*).
- Interior pointers with precision: two mutable borrows of `s->a` and `s->b`
  conflict (see *Field-sensitive places*).

**Accepted false positives**, beyond RFC 0001's merge-point conservatism:

- `realloc` failure paths where the result is not tested *directly*. The
  canonical `q = realloc(p, n); if (!q) { free(p); return; }` is accepted
  (see *`realloc` and the null edge*), but if `q` is first stored somewhere
  else, passed to a helper that does the check, or tested through a
  computed condition, `p`'s resource stays consumed and the `free(p)` on the
  failure path reports `double-free`.
- Pointer arithmetic that leaves an object and casts between unrelated
  pointer types produce *no* place (see *Places*), so nothing done through
  such a pointer is checked. This is a hole rather than a false positive,
  but it is the flip side of the same decision: not making these `Raw`
  errors yet.
- Lexical loans: a loan lives until its scope ends even if the borrowing
  pointer is never used again, so `int *a = &x; use(a); int *b = &x;` in the
  same scope conflicts. Non-lexical loans are future work (RFC 0001).
- Loops whose body conditionally frees and unconditionally re-nulls on a
  path the analysis cannot correlate.

**Assumptions.** Single-threaded execution within the function; no
`setjmp`/`longjmp`; `clang::CFG` is complete for the function (no
`asm goto`, no computed `goto`); the recognised allocator list is correct.

## Detailed design

### Analysis state

The dataflow state at each CFG block entry/exit is:

```
State = { moves    : MoveTracker           -- per RFC 0001
        , loans    : BorrowState
        , aliases  : Partition<PlaceId>    -- new, see below
        , reallocs : Map<PlaceId, Class>   -- new, see "realloc and the null edge"
        }
```

`LifetimeConstraints` is *not* part of the per-block state. Lifetimes are
allocated once per lexical scope and per heap allocation site before the
dataflow runs (see *Lifetimes*), and the constraint set is global to the
function; the dataflow only *queries* it. Keeping the constraint set out of
the join is what keeps the lattice finite.

`join(a, b)` is component-wise: `MoveTracker::join` (set union, "may be
moved"), `BorrowState` loan-set union, the coarsest partition finer than both
alias partitions (union of the equivalence relations, "may alias"), and for
`reallocs` the *intersection* of entries that agree (a pending `realloc` that
is only pending on one incoming path cannot be safely undone, so it is
dropped, which is the conservative direction). All four are monotone over
finite sets (places, program points), so the fixpoint terminates without
widening.

### Alias partition

Copying a pointer creates an alias of the same resource rather than moving
ownership: after `q = p`, `p` and `q` are in one alias class. Releasing or
moving *any* member marks *every* member moved (with the location of the
release), so `free(q); use(p)` is `use-after-free` and the note points at
`free(q)`. Reinitialising a place (`p = malloc(...)`, `p = NULL`, `p = r`)
removes it from its class and puts it in a fresh singleton (or into `r`'s
class).

`core::Partition` is a small union-find over `PlaceId` with the operations
`unite`, `separate`, `members`, `join`, added to `weavec::Core`. It is
Clang-free. *(Built as `core::AliasRelation` with the same operations but
without transitive closure at joins; see Implementation notes.)*

This is the decision RFC 0001 left open under *Copies as aliases*. See
*Alternatives* for why not Rust's move-on-copy.

### Places

`core::PlaceTable` gains structured places. A place is a *base* (local,
parameter, global, or the result of a heap allocation at a given site) plus
a *path* of field selections and a single array summary element:

```
place ::= base ('.' field)* ('[*]')?
```

`p->next` is the place `(*p).next`; the deref step is represented as "the
resource `p` currently refers to", i.e. the alias class of `p`, so field
paths through different aliases of one resource resolve to the same place.
`a[i]`, `a[0]`, `*(a + k)` all map to `a[*]`. Nested arrays collapse to one
summary. Paths are interned, so `PlaceId` stays a small integer and the state
maps stay dense.

> **Amended by [RFC 0006](0006-precision.md), *Element witnesses*.** `a[*]`
> is still one place, but a move record on it carries the element that was
> named (a constant, a variable, or unknown), and only an access with a
> matching witness is a use of it: `free(a[0]); use(a[1]);` is no longer
> reported.

> **Superseded by [RFC 0004](0004-unsafe-boundaries.md), *Pointer
> identity*.** Pointer arithmetic and every pointer-to-pointer cast now
> preserve the place of their operand; only integer-to-pointer casts yield a
> (raw) value with no place. The paragraph below records the RFC 0002
> decision for history.

Anything the mapping cannot express (pointer arithmetic that leaves an
object, casts between unrelated pointer types, `container_of`-style
subtraction) yields *no* place: the expression is treated as an opaque read
that neither moves nor loans. It is *not* treated as `Raw`. Decision: making
these `Raw` would be more honest, but `Raw` outside `WEAVEC_UNSAFE` is an
error, and until the unsafe-blocks RFC defines the escape rule users have no
*scoped* way to say "this arithmetic is fine" short of marking the whole
function unsafe. Turning the hole into an error is that RFC's first item,
and the place builder must return a distinguishable "opaque" result (not
just `nullopt`) so the switch is a one-line change.

Field-path granularity is likewise decided rather than deferred: one summary
place per field path, one `[*]` per array, nested arrays collapsed. Two
mutable loans of `s->a` and `s->b` therefore conflict. Corpus testing will
tell us whether that matters; if it does, per-field places under a deref are
a compatible extension that changes no accepted program into a rejected one.

### Events

The Analysis layer translates CFG elements into core events. The complete
list, in the order they are applied within one statement:

| Expression / statement                             | Event                                                        |
| -------------------------------------------------- | ------------------------------------------------------------ |
| `DeclStmt` for a pointer `p` with initialiser `e`  | reinit `p`; then apply `e`'s assignment rules                |
| `p = e`, `e` an allocation call                    | reinit `p` into a fresh singleton class; kind `Owned`        |
| `q = realloc(p, n)`                                | `checkMove(p)`; `markMoved(class(p), Moved)`; reinit `q` fresh, `Owned`; record `reallocs[q] = class(p)` |
| `p = q` (plain copy of a pointer place)            | reinit `p`; unite `p` with `q`'s class                       |
| `p = &x`                                           | reinit `p`; `addLoan{x, Mutable, lifetime(scope of p)}`      |
| `p = a` (array decay), `p = &a[i]`                 | as `&x` against `a[*]`                                       |
| `p = e`, `e` opaque (call result, cast, field load)| reinit `p` into a fresh singleton; kind `Unknown`            |
| `p = NULL` / `p = 0`                               | reinit `p`; fresh singleton                                  |
| `free(p)` and recognised releasers                 | `checkMove(p)` → `conflicting-borrow`; `markMoved(class, Freed)` |
| call with `WEAVEC_OWNED` parameter receiving `p`   | as `free`, reason `Moved`                                    |
| call with `WEAVEC_BORROWED` / `WEAVEC_MUT` param   | `addLoan{p, Shared/Mutable, lifetime(call)}`; expire after   |
| `return p`                                         | for each loan `p` carries: `outlives(loan.lifetime, fn)` else `lifetime-too-short` |
| `*p`, `p->f`, `p[i]`, `p` as rvalue                | `movedAt(class)` → `use-after-free` / `use-after-move`       |
| `*p = e`, `p->f = e` (store through `p`)           | read of `p` as above; `checkMutation(target)` → `conflicting-borrow` |
| scope exit                                         | `expire(lifetime(scope))`; drop loans against dying locals; `lifetime-too-short` for any live loan of a dying place |

Address-of produces a `Mutable` loan unless the *destination* pointer type
(the declared type of the place being assigned, or the parameter type for a
call argument) has a `const`-qualified pointee, in which case `Shared`. So
`const int *a = &x; const int *b = &x;` is fine and `const int *a = &x;
int *b = &x;` conflicts, which is correct. Decision: this is the rule, not
"infer `Shared` when no store through the pointer is visible". Inferring
read-only-ness needs a backward liveness/use pass that non-lexical loans
will introduce anyway; until then, `const` is the idiomatic C fix and
improves the code regardless.

### `realloc` and the null edge

> **Superseded by [RFC 0006](0006-precision.md), *Outcome-conditional
> summaries*.** `realloc` is now an ordinary table entry whose summary says
> it moves its argument only in the `nonnull` class; the null-edge rule
> below is the general mechanism applied to it, and every function whose
> body consumes conditionally gets the same treatment. `reallocs` is
> replaced by `AnalysisState::pending`.

`realloc` is the one place this RFC is path-sensitive, and deliberately only
in the narrowest useful form. `q = realloc(p, n)` consumes `p`'s alias class
(reason `Moved`, so a later use is `use-after-move` rather than
`use-after-free`; the memory may well still be live), produces a fresh
`Owned` class for `q`, and records `reallocs[q] = class(p)`. When the CFG
branches on a condition that is *directly* `q`, `!q`, `q == NULL`,
`q != NULL` or the `NULL == q` spellings, and `q` has not been reassigned
since, the transfer function on the **null edge** reinstates every place in
`class(p)` (clears their move records) and puts `q` in a fresh singleton;
on the non-null edge it leaves the state as is. In both cases
`reallocs[q]` is then dropped. Any other use of `q`, or any reassignment,
also drops the entry, so the special case never applies to a stale result.

This accepts the canonical failure idiom in all its common shapes:

```c
char *q = realloc(p, n);
if (q == NULL) { free(p); return -1; }   /* ok: p reinstated on this edge */
p = q;                                    /* p joins q's class            */
```

and `p = realloc(p, n)` needs no special handling at all: the consumed class
is `p`'s own, `p` is immediately reinitialised, and on failure the old block
is leaked, which is a real bug this RFC does not report.

Decision: do this rather than accept the false positive, because rejecting
the *correct* way to call `realloc` would teach users on day one that WeaveC
fights idiomatic C; and do it this narrowly rather than as general
null-tracking, because `malloc` failure paths need no help (nothing is
consumed) and a general mechanism would want to be designed alongside
signature inference, where `NULL`-returning callees appear.

Recognised allocation functions: `malloc`, `calloc`, `realloc`, `strdup`,
`strndup`, `aligned_alloc`, `posix_memalign` (via its out-parameter), and
any function whose declaration carries `WEAVEC_OWNED` on the return type.
Recognised release functions: `free`, and any function with a `WEAVEC_OWNED`
parameter. Recognition is by `IdentifierInfo` and global linkage, as today,
so a user's own `malloc` shadow is treated as the libc one; that is a known
limitation.

### Dataflow

A standard worklist over `clang::CFG` built with `AddInitializers`,
`AddImplicitDtors` off (C), `AddTemporaryDtors` off, `AddScopes` on (needed
for scope-exit events), `AddStaticInitBranches` off. Reverse post-order
seeding; a block is re-queued when its input state changes. Because the join
is a finite-height lattice, iteration count is bounded by
`|blocks| × (|places| + |loans|)` in the worst case; we expect low
single-digit passes on real code and will assert a hard cap of 64 iterations
per block behind `-DWEAVEC_ANALYSIS_ASSERTIONS` to catch non-monotone bugs.

Short-circuit operators, `?:`, `switch` fall-through, `goto`, `break`,
`continue` and early `return` all become ordinary CFG edges and need no
special handling. `CFGScopeBegin`/`CFGScopeEnd` elements drive lifetime
expiry. Unreachable blocks (per the CFG's reachability) are skipped so dead
code produces no diagnostics.

Diagnostics are **not** emitted during the fixpoint iteration. Once the
worklist is empty, a single *reporting pass* re-runs the transfer function
over every reachable block exactly once from its fixed entry state, with
reporting enabled. Each problem is therefore reported once per program point
with no deduplication set and no risk of suppressing a distinct problem that
happens to share an id and location. Where the join has merged several
origins into one record (two paths freeing the same place at different
lines), the note points at whichever record the join kept; `MoveTracker::join`
must be deterministic about that (keep the existing record) so output is
stable across runs.

### Lifetimes

Before the dataflow runs, a pass over the CFG scopes allocates:

- one `LifetimeId` per lexical scope, with `outer: inner` constraints from
  nesting; the function body scope is `fn`; `'static` outlives `fn`;
- one per heap allocation *site* (not per iteration), unconstrained except
  `'static: alloc_site` so it is never trivially valid to keep;
- parameters live for `fn`; globals are `'static`.

> **Amended by [RFC 0006](0006-precision.md), *Loans end at the last use
> of their holder*.** A loan held by a local ends at the holder's last use
> (backward liveness over the CFG), not at the end of its scope; loans held
> through a pointer, by a global or by an address-taken local still end
> when the holder is reassigned. The lifetime constraints below are
> unchanged.

A loan created by `p = &x` in scope `S` has lifetime `S`. Storing it into a
place whose own lifetime is `T` requires `S: T`; storing a loan on a local
into a parameter-pointed or global place therefore fails unless the local is
`static`. Returning `p` requires `lifetime(p's loan): fn`, which a loan of a
non-static local never satisfies. This is the whole of the lifetime
reasoning this RFC does; there is no inference of relationships between
parameters (that is the signature RFC).

### Unsafe interaction

> **Superseded by [RFC 0004](0004-unsafe-boundaries.md), *Unsafe
> regions*.** Unsafe blocks and unsafe function bodies are now analysed with
> their diagnostics suppressed, so a free inside one is reported at a use
> outside it.

`WEAVEC_UNSAFE` on the function still skips it entirely. `WEAVEC_UNSAFE { }`
blocks are *skipped* by the transfer function (their CFG blocks are treated
as identity) and their escaping-pointer rule is left to the unsafe-blocks
RFC; until then a pointer freed inside an unsafe block and used outside it
is *not* reported. This preserves the scaffold's behaviour and avoids
half-implementing RFC 0001's escape rule.

### Debug output

`weavec --dump-analysis` prints, per analysed function, the place table with
paths and kinds, the lifetime constraints, and the exit state (moved places,
live loans, alias classes) in a stable textual form intended for FileCheck:

```
function 'alias':
  places: p (param, owned) q (local, owned)
  lifetimes: caller fn
  exit: moved{p@5:3 freed} loans{} aliases{}
```

The exact format is fixed by the lit tests that use it
(`test/Driver/dump-analysis.c`) and may change in a minor release with a
CHANGELOG entry; it is a debugging aid, not an API.

### Layering

New core code: `core::Partition`, structured paths in `PlaceTable`, and a
`core::AnalysisState` struct bundling the four components with its `join`. All
Clang-free and unit-tested in isolation with hand-built place tables. The
Analysis layer contributes `CFGDataflow` (the worklist), `PlaceBuilder`
(expression → place), `EventTranslator` (CFG element → core events) and
`AllocatorRegistry`. `LocalOwnershipChecker` is deleted once the lit tests
for `use-after-free` and `double-free` pass against the new engine with
identical messages.

*(As built: `core::AliasRelation`, `core::AnalysisState`
(`include/weavec/Core/`); `FunctionDataflow` in `lib/Analysis/Dataflow.cpp`
combines the worklist and the event translation; `PlaceBuilder` in
`lib/Analysis/PlaceBuilder.cpp`; `classifyCall` in
`include/weavec/Analysis/Allocators.h`.)*

## Annotation surface

None. Existing annotations gain behaviour (`WEAVEC_OWNED` parameters now
move their argument; `WEAVEC_BORROWED`/`WEAVEC_MUT` parameters now create
loans for the call), which is the meaning RFC 0001 already assigned them.

## Diagnostics

Newly emitted (ids already reserved by RFC 0001):

| Id                   | Severity | Message                                                                 | Notes                                                  |
| -------------------- | -------- | ----------------------------------------------------------------------- | ------------------------------------------------------ |
| `use-after-move`     | error    | `use of '<p>' after it was moved`                                       | `moved here`                                           |
| `conflicting-borrow` | error    | `cannot borrow '<x>' as mutable because it is already borrowed`         | `previous borrow of '<x>' by '<a>' here`               |
| `conflicting-borrow` | error    | `cannot borrow '<x>' as shared because it is already mutably borrowed`  | `previous borrow of '<x>' by '<a>' here`               |
| `conflicting-borrow` | error    | `cannot free '<p>' while it is borrowed`                                | `borrowed by '<a>' here`                               |
| `conflicting-borrow` | error    | `cannot move '<p>' while it is borrowed`                                | `borrowed by '<a>' here`                               |
| `conflicting-borrow` | error    | `cannot assign to '<x>' while it is borrowed`                           | `borrowed by '<a>' here`                               |
| `lifetime-too-short` | error    | `'<p>' may outlive '<x>', which it points to`                           | `'<x>' is declared here`; `'<x>' goes out of scope here` (omitted for returns) |
| `lifetime-too-short` | error    | `returned pointer may outlive '<x>', which it points to` (for `return &x`) | `'<x>' is declared here`                             |

Wording decision: the messages keep the *borrow* vocabulary. The annotation
names (`WEAVEC_BORROWED`), the id (`conflicting-borrow`) and RFC 0001 all
already commit to it, and a user who meets the word in the header should
meet the same word in the error. Each note names the other pointer (`by
'<a>'`) so the message reads as a statement about two pointers, which is
how C programmers think about it, without inventing a second vocabulary.

Changed:

| Id               | Change                                                                                                   |
| ---------------- | -------------------------------------------------------------------------------------------------------- |
| `use-after-free` | Unchanged message. When the release was through an alias the note becomes `freed here (through '<q>')`. |
| `double-free`    | Unchanged message. Same alias note as above.                                                             |

Triggers for each row are the snippets under *Soundness*. Each row gets a
unit test against the core state machine and a lit test under
`test/Analysis/rfc0002-<id>.c`.

## Drawbacks

- **Core grows a partition and structured places.** Both are simple, but
  they are the first pieces of *state* in Core beyond the three RFC 0001
  structures, and every later RFC has to keep them consistent.
- **Field summaries lose precision.** `&s->a` and `&s->b` conflict. On
  struct-heavy code this may be the dominant false positive; if so the fix
  (per-field places under a deref) is a compatible extension.
- **Alias-set semantics are not Rust's.** Someone porting intuition from
  Rust will expect `q = p` to kill `p`. The RFC 0001 *Copies as aliases*
  question is resolved here in favour of C idiom; the drawback is that
  "which alias is the owner" is never stated, so a leak (neither freed)
  cannot be distinguished from a transfer, which is part of why leaks are
  out of scope.
- **Lexical loans** produce the well-known "borrow is still live" false
  positives until non-lexical loans land.
- **Analysis time.** A worklist over the CFG with copyable state is more
  expensive than one AST walk. Expected to be well under Clang's parse time
  per function; measured by `--dump-analysis --time-report` on the corpus.

## Alternatives

- **Move-on-copy (Rust semantics).** `q = p` makes `p` dead. Simpler state
  (no partition), matches RFC 0001's `use-after-move` wording. Rejected for
  this RFC because it rejects `tmp = p; p = p->next; free(tmp);`, the single
  most common linked-list idiom in C, for no soundness gain. Alias sets
  catch the same bugs.
- **Full points-to (symbolic values, as in Clang's `MallocChecker`).** More
  precise on `realloc` and on null checks, at the cost of path explosion,
  budgets and unpredictable results. RFC 0001 chose predictability.
- **Keep the AST walk and bolt on loop iteration.** Iterating an AST walk to
  a fixpoint reinvents the CFG badly and still mishandles `goto` and
  `switch`. Clang already builds the CFG we need.
- **Store `LifetimeConstraints` in the dataflow state.** Would let loops
  discover new constraints per iteration, but constraints from a fresh
  lifetime per iteration do not converge. Allocating lifetimes per scope and
  per site before the dataflow is the standard fix (it is what region
  inference does).
- **No path sensitivity for `realloc`** (accept the `double-free` false
  positive on the failure idiom). Simplest state, but rejects the *correct*
  use of `realloc`. Rejected; see *`realloc` and the null edge*.
- **General null-tracking** (every pointer carries a may-be-null fact
  refined by conditions). Subsumes the `realloc` case and would later help
  with `NULL`-returning callees, but it is a second lattice component with
  its own join and its own false positives, and nothing in this RFC needs it
  except `realloc`. Deferred to the signature-inference RFC, where the need
  is real.

## Prior art

- **Rust's MIR borrow checker** (`rustc_borrowck`): dataflow over MIR with
  `MaybeUninitializedPlaces` (our `MoveTracker`) and `Borrows` (our
  `BorrowState`) as separate analyses joined at the same points. We take the
  factoring; MIR's move paths are our structured places.
- **Clang's `CFG`** and its consumers: `-Wuninitialized`
  (`UninitializedValues.cpp`) is a small forward dataflow over `clang::CFG`
  with exactly the worklist shape proposed here, and shows that `AddScopes`
  is sufficient for scope-exit events in C. The lifetime-safety analysis
  (`clang/Analysis/Analyses/LifetimeSafety/`: `Facts`, `Loans`,
  `LoanPropagation`, `LiveOrigins`) is the closest existing code: loans and
  origins propagated over CFG facts, with expiry at scope end. Worth
  studying for whether WeaveC can reuse its fact generator rather than
  writing `EventTranslator` from scratch.
- **Clang Static Analyzer `MallocChecker`.** Its allocator list and its
  `realloc` modelling (with the failure path handled via symbolic
  constraints) are the reference for what users expect; we adopt the list
  and a deliberately narrow, non-symbolic version of its `realloc` failure
  handling.
- **Steensgaard-style unification** for the alias partition: union-find,
  may-alias, linear-time; imprecise across the whole program but exactly
  right for "which locals currently name the same allocation" within one
  function.
- **Cyclone's region inference** for allocating regions per lexical scope
  before checking rather than as part of it.

## Unresolved questions

**Resolved at acceptance.** The Draft posed six questions; each was decided
and the decision is recorded inline in *Detailed design* and *Diagnostics*.
Summary, so the reasoning is findable from here:

| Question                          | Decision                                                                                     |
| --------------------------------- | -------------------------------------------------------------------------------------------- |
| `realloc` failure path            | Narrow null-edge reinstatement for the directly-tested result; no general null tracking.     |
| Opaque pointer expressions        | No place (hole) in this RFC; resolved by RFC 0004 the other way: arithmetic and pointer casts preserve the place, only integer casts are `Raw`. |
| Loan kind of `&x`                 | `Mutable` unless the destination pointee is `const`; no read-only inference yet.             |
| Field summary granularity         | One place per field path, `[*]` per array; measure on the corpus, extend compatibly if needed. |
| Diagnostic deduplication          | None needed: diagnostics are emitted in a post-fixpoint reporting pass, once per block.      |
| `conflicting-borrow` wording      | Keep the *borrow* vocabulary; notes name the other pointer.                                  |

**Settled during implementation** (see *Implementation notes*):

- Exact `--dump-analysis` format: pinned by `test/Driver/dump-analysis.c`,
  documented only there.
- The iteration cap (64 per block): silent, always on; the block is dropped
  from the worklist and analysis continues.
- `posix_memalign`'s out-parameter form: not modelled yet.
- `goto` *into* a scope: lifetimes are attached to `CFGLifetimeEnds`
  elements per variable rather than to scope-end markers, so a jump into a
  scope simply never sees the variable's end on that path. No lifetime rule
  relies on it.

**Deferred to corpus testing** (empirical; may motivate a follow-up RFC):

- Whether field-summary conflicts (`&s->a` vs `&s->b`) are a significant
  source of false positives on struct-heavy code.
- Whether the `Mutable`-by-default loan kind produces enough friction on
  read-only helper pointers to justify pulling read-only inference forward
  of non-lexical loans.
- Whether the narrow `realloc` rule covers enough real code, or whether
  helper-wrapped checks (`if (!checked(q))`) are common.

## Future work

- **Signature inference RFC** (done: [RFC 0003](0003-signature-inference.md)):
  summaries make unannotated callees stop being a hole and make
  `annotation-required` default-on.
- **Unsafe blocks RFC** (done: [RFC 0004](0004-unsafe-boundaries.md)):
  `unsafe-operation`, analysed unsafe regions, and pointer identity for
  arithmetic and casts.
- **Non-lexical loans**: liveness from the CFG dataflow; removes the
  lexical-loan false positives and enables read-only loan inference.
- **General null tracking** alongside signature inference, subsuming the
  `realloc` special case.
- **Leak reporting** as an opt-in diagnostic once ownership among aliases is
  decidable.
