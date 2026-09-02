# RFC 0003: Signature inference

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-01
- **Accepted**: 2026-09-01
- **Implemented**: 2026-09-01
- **Tracking issue**: TBD
- **Supersedes / superseded by**: — (implements phase 2 and the first half of
  phase 3 of RFC 0001)

## Implementation notes

The design below landed as written except where this section says
otherwise. Each item clarifies *how* a decision was built, not the decision;
where the text below disagrees with an item here, the item wins. Behaviour is
pinned by `test/Analysis/rfc0003-*.c`, `test/Annotations/rfc0003-*.c`,
`unittests/Analysis/SignatureInferenceTest.cpp`,
`unittests/Analysis/SummariesTest.cpp` and `unittests/Core/SummaryTest.cpp`.

- **Call graph lives in the driver.** There is no separate `CallGraph.cpp`;
  `TranslationUnitAnalyzer` collects definitions, builds the callee edges
  and runs an iterative Tarjan SCC in one file. `FunctionAnalyzer::analyze`
  takes a `SummaryStore &` (not an optional provider) and returns whether
  the callee's summary changed, which is what the SCC fixpoint loop needs.
- **Effects are recorded where they happen; the exit rule is applied at
  finalisation.** `FunctionDataflow` records every read, write, free and
  move against the *stable* summary path of the place (the path as it was
  at function entry, via `stableSummaryPathOf`). At `finalizeSummary` the
  effects on parameter roots are kept as recorded; effects under `*param`
  and on globals are replaced by the exit state unless the parameter
  variable was reassigned anywhere in the body (`paramReassigned`), in
  which case the recorded events are kept for that parameter's sub-paths.
  This is the "where effects are read from" decision, built as one pass
  rather than as two collection modes.
- **A caller-visible slot that was re-nulled is `written`, not `freed`.**
  `free(b->data); b->data = NULL;` yields
  `b->data: written; stores{b->data = null}`: the write is an effect, the
  null is a store, and no `freed` remains because the exit state does not
  hold the field freed. The example under *Debug output* was corrected to
  match. Parameter kinds (`b: mut`) are not printed on the summary line;
  they are derivable (`FunctionSummary::inferredKind`) and already appear
  on the `places:` line when annotated.
- **Handing out a pointer into the pointee is a shared use.**
  `FunctionSummary::borrowKind` treats a returned or stored
  `Copy`/`Borrow` of a path under `*param(i)` as a read of the pointee, so
  `int *field_of(struct node *n) { return &n->v; }` infers `n` as
  `WEAVEC_BORROWED` rather than "no inferable ownership".
- **Copies of loans are lifetime-checked.** RFC 0002 checked
  `lifetime(dest) ⊑ lifetime(loan)` only when the loan was created (`p =
  &x`); a later plain copy `g = p` escaped the check. `applyPointerAssign`
  now performs the same check for every loan the source holds. This is a
  bug fix against RFC 0001 rather than a new rule; it is pinned by
  `test/Analysis/rfc0003-escapes.c` because callee stores (`keep(p)` with
  `g = p` inside) made it visible.
- **Argument roots that do not resolve to a place use the argument's own
  value.** For `Copy(param(i))` the caller classifies the argument
  expression directly (`PlaceBuilder::classifyValue`), so array decay and
  `&local` arguments carry their borrow into the store or return value and
  `keep(local)` reports `lifetime-too-short`.
- **`main` is not an exported API.** `--report-unannotated` skips `main`;
  its signature is fixed by the C standard and cannot carry annotations
  usefully.
- **Corpus.** `scripts/corpus.py` and `scripts/corpus/projects.json` landed
  with this RFC; the first baseline (`scripts/corpus/baseline.json`) has no
  diagnostics attributable to signature inference and no default
  `annotation-required` warnings on six real projects. The five diagnostics
  it does record are RFC 0002 false positives (`free(a[i])` in a loop
  against the single `a[*]` place; a `lc != &ctable` equality guard), see
  `scripts/corpus/README.md`. Both are candidates for the next model RFC.

## Summary

Every function definition in a translation unit gets a *summary*: what it
does to each pointer it receives (frees it, moves it, reads or writes
through it, stores it somewhere that outlives the call), what it stores
through its parameters and into globals, and where its pointer result comes
from (a fresh allocation, one of its arguments, a borrow of something an
argument points to). Summaries are inferred bottom-up over the
translation unit's call graph, iterated to a fixpoint inside recursive
cycles, and applied at every call site by the RFC 0002 dataflow, so
`node_free(n); n->v` is a `use-after-free` even though `node_free` is a plain
C function with no annotation. Declarations without a body get their
summary from their annotations, from a shipped table for the C standard
library, or, failing both, a documented default plus an
`annotation-required` warning at the first call. Annotations become
*checked*: a `WEAVEC_BORROWED` parameter that the body frees is an
`annotation-mismatch`. `--report-unannotated` grows a fix-it that inserts the
inferred annotation on exported functions. Nothing about the intra-procedural
model changes; this RFC only decides what a call *means*.

## Motivation

RFC 0002 checks every function in isolation and treats a call to an
unannotated function as having no effect on its pointer arguments. On real
code that is the dominant hole, because real code wraps everything:

```c
static void node_free(struct node *n) { free(n); }
static void buf_destroy(struct buf *b) { free(b->data); }

void f(void) {
  struct node *n = node_new(1);
  node_free(n);
  return n->v;            /* use-after-free, not reported before this RFC */
}
void g(void) {
  struct buf b; buf_init(&b, 8);
  buf_destroy(&b);
  b.data[0] = 1;          /* use-after-free, not reported before this RFC */
}
```

Every `xxx_destroy`, every `xxx_new`, every helper that takes ownership of a
list node defeats the checker, and `--dump-analysis` shows the information
already being computed and thrown away (`node_free: exit: moved{n freed}`).
The scaffold's `--report-unannotated` mitigation fires on *every* pointer
parameter, so it is noise rather than guidance.

This RFC is also what makes RFC 0001's first design goal ("prove first,
annotate second") true: today WeaveC can only *ask* for annotations; after
this RFC it infers them, checks the ones it is given, and asks only where
inference genuinely cannot answer, at the boundary with code it cannot see.

## Soundness

**Bugs caught after this RFC** (in addition to RFC 0002's), given the
assumptions below:

```c
static void node_free(struct node *n) { free(n); }
static struct node *node_new(void) { return malloc(sizeof(struct node)); }
static void buf_destroy(struct buf *b) { free(b->data); }
static int make(char **out) { *out = malloc(8); return *out != NULL; }
static char *g;
static void keep(char *p) { g = p; }

void wrapper_uaf(void) {
  struct node *n = node_new();
  node_free(n);
  n->v = 1;                    /* error: use of 'n' after it was freed     */
}
void wrapper_double_free(void) {
  struct node *n = node_new();
  node_free(n);
  free(n);                     /* error: 'n' is freed twice                */
}
void field_uaf(struct buf *b) {
  buf_destroy(b);
  b->data[0] = 1;              /* error: use of 'b->data' after freed      */
}
void out_param(void) {
  char *s;
  make(&s);
  free(s);
  s[0] = 1;                    /* error: use of 's' after it was freed     */
}
void escape(void) {
  char local[8];
  keep(local);                 /* error: 'g' may outlive 'local'           */
}
char *alias(char *WEAVEC_OWNED p) {
  char *q = strchr(p, 'x');    /* libc summary: result aliases p           */
  free(p);
  return q;                    /* error: use of 'q' after it was freed     */
}
void mismatch(struct node *WEAVEC_BORROWED n) {
  free(n);                     /* error: annotation-mismatch               */
}
```

Recursion is handled: a recursive `list_free` is summarised as freeing its
argument and every caller sees that.

**Deliberately not caught by this RFC:**

- **Calls through function pointers.** No summary is available; the call has
  no effect (the RFC 0002 behaviour). RFC 0001's *Function pointers and
  callbacks* question remains open. Arguments passed to a function pointer
  are not moved, not borrowed and not checked. *Superseded by
  [RFC 0004](0004-unsafe-boundaries.md), which gives indirect calls a
  signature from the function-pointer type's annotations or from the
  address-taken functions of that type, and otherwise treats them as the
  boundary described in the next item.*
- **Unannotated external functions** (no body in the TU, no annotation, not
  in the shipped table). By default their pointer arguments are treated as
  borrowed for the call and not retained, their result as unknown, and one
  `annotation-required` warning is emitted per callee (see *Diagnostics*).
  This is a deliberate hole with a warning attached; see *Alternatives* for
  why not RFC 0001's `Raw`-and-error stance yet.
- **Leaks**, including `WEAVEC_OWNED` parameters that are neither released
  nor stored. Still out of scope (RFC 0002, *Soundness*).
- **Cross-TU inference.** Summaries live and die with one translation unit.
  Functions defined in another TU are external (above). Milestone 4.
- **Effects on paths under a parameter that the callee re-points.** If a
  callee assigns to a parameter variable (`p = p->next`), effects on
  `p->field` afterwards are still attributed to the *argument's* `->field`.
  This is the conservative direction (see *Deriving a summary*).

**Accepted false positives**, beyond RFC 0001's and RFC 0002's:

- **May-effects are applied as must-effects at the call.** A callee that
  frees its argument on one path (`if (!ok) free(p)`) is summarised as
  freeing it, so the caller's later use is reported. This is the same
  merge-point conservatism RFC 0001 accepts intra-procedurally, now visible
  across a call. The fix is the same: restructure, or have the callee report
  the outcome and re-null.
- **Re-pointed parameters** (above): `void f(struct node *p) { p = p->next;
  free(p->data); }` is summarised as freeing `arg->data`.
- **Borrow-for-the-call conflicts.** A callee that reads through `p` is a
  shared borrow of `*p` for the call; passing `&x` while a mutable loan of
  `x` is live conflicts, exactly as an annotated `WEAVEC_BORROWED` parameter
  does today (RFC 0002).
- **Stores are unioned.** A callee that stores different things into `*out`
  on different paths gives the caller the join of them, which can be `Raw`
  if, say, one path stores a fresh allocation and another a borrow. Such a
  callee is contradictory and should be annotated or split.

**Assumptions.** RFC 0001's and RFC 0002's, plus: the shipped library table
is correct for the platform's libc (it describes ISO C and POSIX behaviour
and does not model extensions); a function's *definition* in the TU is the
one that will be called (no link-time interposition, no `-Wl,--wrap`);
annotations on a declaration are trusted over the body, per RFC 0001, and
disagreement is reported rather than resolved.

## Detailed design

### Summaries (`weavec::Core`)

A summary describes a function in terms of *roots*: its pointer parameters
(by index) and the globals it touches (by an id the Analysis layer interns
per TU), with structured *paths* below each root spelled with the RFC 0002
steps:

```
root ::= param(i) | global(g)
path ::= root ('*' | '.' field | '[*]')*
```

`param(0)*` is "the object the first argument points to"; `param(0)*.data`
is that object's `data` field; `global(g)` is the global itself.

```
FunctionSummary = { effects : Map<Path, PlaceEffect>
                  , stores  : Set<Store>
                  , returns : Set<ValueSource>
                  , flags   : { reallocLike }
                  }
PlaceEffect     = { read, written, freed, moved : bool }     -- may-flags
Store           = { dest : Path, value : ValueSource }
ValueSource     = Fresh | Copy(path) | Borrow(path) | Null | Unknown
```

- `effects[P]` records what the callee may do to the object at `P`: read it,
  write it, release it (`free` or a releasing callee), or move it (pass it
  to an owner). For `P = param(i)` the flags describe the argument *value*:
  `freed`/`moved` mean the caller's pointer is dead after the call. For
  paths with at least one `*`, they describe memory the caller owns.
- `stores` records pointer values the callee writes into caller-visible
  places: `param(0)*` for an out-parameter, `param(0)*.data` for a field,
  `global(g)` for a global. `Fresh` is a new allocation the caller now
  owns; `Copy(param(j))` means the callee stored its `j`-th argument there
  (the classic *escape*); `Borrow(param(j)*.buf)` means it stored the
  address of something under argument `j`; `Null` and `Unknown` are what
  they say.
- `returns` is the set of alternatives for a pointer result, using the same
  vocabulary. Empty means nothing is known (or the function does not return
  a pointer).

The **join** is component-wise set union (`PlaceEffect` joins by `or`).
Every component is a finite set over a finite alphabet (paths are bounded
by the RFC 0002 depth cap), so the summary lattice has finite height and the
recursive fixpoint below terminates.

Derived queries the checker uses:

- `consumes(i)`: `effects[param(i)].freed || .moved`, and which.
- `borrowKind(i)`: `Mutable` if any path strictly below `param(i)*` (or
  `param(i)*` itself) is written, freed, moved or is a store destination;
  `Shared` if any is read; none otherwise.
- `inferredKind(i)`: `Owned` if consumed; else `Mutable`/`Shared` from
  `borrowKind`; else `Unknown`. This is what `--report-unannotated` offers
  as a fix-it and what reconciliation compares against.

The summary is Clang-free: roots are integers, paths are step lists with
field names, locations are `core::SourceLocation`. It lives in
`include/weavec/Core/Summary.h`.

### The translation-unit driver (`weavec::Analysis`)

`TranslationUnitAnalyzer` replaces the per-function loop in the frontend:

1. Collect every `FunctionDecl` with a body in the TU (all files, not only
   the main file: a `static inline` helper in a header needs a summary even
   when its own diagnostics are filtered).
2. Build the direct call graph (`CallExpr` with a `getDirectCallee()` whose
   definition is in the TU). Indirect calls are not edges.
3. Compute strongly connected components (Tarjan) and visit them in reverse
   topological order, callees first.
4. A trivial SCC (one function, no self-call) is analysed once: the RFC 0002
   dataflow runs, reports, and produces the summary in the same pass.
5. A non-trivial SCC starts every member at the bottom summary (no effects)
   and re-analyses all members, with reporting disabled, until no member's
   summary changes (cap: 16 rounds; the lattice is finite so the cap is a
   guard, not a budget). Then each member is analysed once more with
   reporting enabled against the fixpoint summaries. Because the summary of a
   callee only ever grows and applying a larger summary only ever produces a
   larger exit state, the iteration is monotone.
6. Diagnostics for a function are emitted only if the frontend's filter
   accepts it (`mainFileOnly` semantics are unchanged).

The `SummaryProvider` consulted at every call resolves a callee in this
order, first match wins:

1. **Annotations on the declaration** for any parameter or return that has
   one: `WEAVEC_OWNED` parameter → `moved`; `WEAVEC_BORROWED` → `read` under
   `param(i)*`; `WEAVEC_MUT` → `written`; `WEAVEC_OWNED` return → `Fresh`;
   `WEAVEC_BORROWED`/`WEAVEC_MUT` return → `Unknown` (the borrow's source is
   unknown from the signature alone). For an annotated *root*, the annotation
   replaces the inferred effects for that root; unannotated roots of the same
   function keep their inferred effects. Annotations are authoritative
   (RFC 0001) and *checked* (see *Reconciliation*).
2. **The inferred summary**, if the callee is defined in this TU.
3. **The shipped library table** (`lib/Analysis/Builtins.cpp`), keyed by
   global name exactly as RFC 0002's allocator list is, and subsuming it.
   It covers the ISO C `<stdlib.h>`, `<string.h>` and `<stdio.h>` functions
   that take or return pointers, plus `strdup`/`strndup`/`aligned_alloc`.
   `fopen`/`fdopen`/`tmpfile` produce an owned `FILE *`; `fclose` releases
   it. `strchr`, `strrchr`, `strstr`, `strpbrk`, `memchr`, `strtok`,
   `strcpy`, `strcat`, `memcpy`, `memmove`, `memset`, `strncpy`, `strncat`
   return an alias of the argument they index into (`Copy(param(k))`).
   `strtol` and friends store `Copy(param(0))` through their `endptr`.
   `getenv`, `strerror`, `setlocale` return borrows of static storage
   (`Unknown` here; a `Static` source is future work). `qsort`, `bsearch`
   borrow. `realloc` keeps its RFC 0002 null-edge special case
   (`reallocLike`).
4. **Default for everything else**: no effects, `returns = {Unknown}`. The
   call is recorded so `annotation-required` fires once for the callee.

### Applying a summary at a call (`FunctionDataflow`)

Each `Path` is resolved against the call's arguments to a caller place:
`param(i)` is the argument's own place if the argument is a pointer place
(`p`, `s.p`, `q->next`), `param(i)*` is `*p` (or, when the argument is
`&x`, simply `x`; when it is an array decaying, `a[*]`), and further steps
apply as in RFC 0002. `global(g)` is the global's place in the caller.
Arguments that are not places (a call result passed directly, an opaque
expression, a null constant) make the paths under them unresolvable and the
corresponding effects are dropped; nothing is tracked, nothing is reported,
exactly the RFC 0002 hole for opaque values.

Effects are applied in this order, all through the existing RFC 0002 semantic
actions so their checks and diagnostics are reused:

1. **Consumption.** For each path with `freed` or `moved`, in path order and
   skipping paths below an already-consumed one: `doConsume` (this is where
   `double-free`, `use-after-free` and `cannot free ... while it is
   borrowed` come from). Reason `Freed` if the summary says freed, else
   `Moved`; the note is `freed here` / `moved here` at the call.
2. **Borrows for the call.** For each argument with a `borrowKind` and not
   consumed: the temporary-borrow check RFC 0002 already performs for
   annotated parameters (`conflicting-borrow`).
3. **Stores.** For each store destination: mutation check of the
   destination, then the RFC 0002 pointer-assignment rules with the store's
   `ValueSource` translated to a `ValueOrigin`: `Fresh` → allocation
   (`Owned`); `Copy(path)` → copy (alias, loans carried, lifetime check);
   `Borrow(path)` → borrow (loan created, lifetime and borrow rules);
   `Null`/`Unknown` → reinitialise. Several stores to one destination form
   one conditional assignment.
4. **Result.** The call expression's `ValueOrigin` is the summary's
   `returns` translated the same way (several alternatives form a
   conditional origin), so `q = node_new()` is an allocation, `q =
   strchr(p, c)` is a copy of `p` and `q = get_field_ptr(s)` is a borrow of
   `s->field`. `Copy`/`Borrow` are the mechanism by which lifetimes flow
   through calls without any lifetime annotation: the loan the argument
   carried is the loan the result carries.

The escape case (`keep(local)` above) falls out of step 3: the store
`global(g) = Copy(param(0))` becomes `g = local` in the caller, and the
RFC 0001 rule "assigning a loan into a place requires the loan to outlive
it" reports `lifetime-too-short`. That rule was not enforced for plain
pointer copies before this RFC (only for direct `&x`); enforcing it is a
bug fix against RFC 0001/0002, not a new decision, and lands here because
the escape check depends on it.

### Deriving a summary

The callee's own RFC 0002 analysis produces the summary; nothing is computed
twice.

- **Reads and writes** are recorded as they happen (flow-insensitive
  may-flags): a load through a dereference of a root-rooted pointer marks
  `read` on the dereferenced path; an assignment through one marks
  `written`. Aliases are honoured through RFC 0002's mirrors: reading `q->v`
  after `q = p` marks `param(p)*.v`.
- **Frees and moves** on a **parameter root itself** are recorded as they
  happen. The parameter variable is the callee's private copy of the
  argument, so `free(p); p = NULL;` still frees the caller's pointer.
- **Frees and moves on every other path** (below a dereference, or under a
  global) are read from the **exit state** of the dataflow. Those places
  are the caller's memory, and what the caller sees is their state at
  return: `free(b->data); b->data = NULL;` leaves `param(0)*.data` *not*
  freed, but stored-`Null`, which is right. If the parameter variable was
  reassigned anywhere in the body, paths under it fall back to
  as-they-happen recording (the exit state no longer describes the argument),
  which is the conservative direction noted under *Soundness*.
- **Stores** are recorded as they happen when the destination is under a
  dereference of a root or is a global. The stored value is classified
  from the RFC 0002 origin: an allocation is `Fresh`; a copy of a
  root-rooted pointer is `Copy(path)`; a borrow of a root-rooted place is
  `Borrow(path)`; a copy of a *local* is resolved through the local's
  aliases (a root-rooted alias → `Copy`), then its held loans (a loan on a
  root-rooted place → `Borrow`; a loan on a local is already
  `lifetime-too-short` in the callee), then its kind (`Owned` → `Fresh`);
  otherwise `Unknown`.
- **Returns** are classified the same way from every `return` statement.
- A summary for a function whose body is skipped (`WEAVEC_UNSAFE`) is its
  annotation-derived summary only (RFC 0001: the signature still applies).
  *Narrowed by [RFC 0004](0004-unsafe-boundaries.md): unsafe bodies are
  analysed and summarised like any other; only a `WEAVEC_UNSAFE`
  declaration without a body keeps the annotation-only summary.*

### Reconciliation

Where the body contradicts the signature, the body's analysis reports it
(during the RFC 0002 reporting pass, so it is once per program point):

- Releasing or moving a place whose kind is `Shared` or `Mutable` *and*
  which is, or aliases, a parameter annotated `WEAVEC_BORROWED`/`WEAVEC_MUT`.
- Writing through a pointer whose kind is `Shared` and which is, or aliases,
  a `WEAVEC_BORROWED` parameter (C already rejects writes through `const`
  pointers, so this only fires for non-`const` `WEAVEC_BORROWED`
  parameters, which is exactly the case where the annotation is the only
  thing saying "read-only").
- `return` of a borrow (`&x`, `&p->f`, array decay) from a function whose
  return type is `WEAVEC_OWNED`.
- `return` of a fresh allocation from a function whose return type is
  `WEAVEC_BORROWED` or `WEAVEC_MUT`.

Callers keep trusting the annotation (RFC 0001's model), so a single
mismatch is reported once, in the callee, and does not cascade.

### `annotation-required`, revised

The scaffold's rule ("every unannotated pointer parameter, behind a flag")
is replaced by two:

1. **External boundary, on by default.** The first call in analysed code to
   a callee with no definition in the TU, no annotations, and no library
   entry, where the callee has at least one pointer parameter or a pointer
   result, reports `annotation-required` at the call, once per callee per
   TU. Callees declared in system headers are exempt by default (the
   library table is expected to cover what matters there, and the rest is
   RFC 0001's "unsafe by default for external code"); `--report-unannotated`
   lifts the exemption. `--strict-externs` raises the severity to error.
   *Superseded by [RFC 0004](0004-unsafe-boundaries.md) for strict mode:
   the call is an `unsafe-operation` at every call site outside an unsafe
   region and its result is raw; no `annotation-required` is emitted.*
2. **Exported API, opt-in.** With `--report-unannotated`, every non-`static`
   function definition whose pointer parameter or pointer return has no
   annotation gets one warning per such position, carrying a **fix-it** that
   inserts the inferred annotation (`WEAVEC_OWNED`, `WEAVEC_BORROWED` or
   `WEAVEC_MUT`) when inference produced one, and the existing "no
   inferable ownership" wording when it did not. This is the "annotate at
   ABI boundaries" workflow from the roadmap: run once with the flag, apply
   the fix-its, and the TU is documented for its callers in other TUs.

### Debug output

`--dump-analysis` gains a fourth line per function:

```
function 'buf_destroy':
  places: b (param, unknown)
  lifetimes: caller fn
  exit: moved{} loans{} aliases{}
  summary: b->data: written; stores{b->data = null} returns{}
```

Format: each root or path with a non-empty effect (`read`, `written`,
`freed`, `moved`), then `stores{...}`, then `returns{...}` (`fresh`, `null`,
`copy <path>`, `borrow <path>`, `unknown`). As with the rest of the dump, the exact text is pinned by
`test/Driver/dump-analysis.c` and may change in a minor release.

### Layering

New in Core: `Summary.h`/`.cpp` (`SummaryPath`, `PlaceEffect`, `Store`,
`ValueSource`, `FunctionSummary` with `join` and `operator==`), and a
`fixits` field on `core::Diagnostic` (`FixItHint{location, insertion}`), all
Clang-free and unit-tested. New in Analysis: `Summary`-aware
`classifyCall` (`Allocators.h` becomes a thin façade over the
`SummaryStore`), `Summaries.h/.cpp` (annotation → summary, lookup order,
global interning), `Builtins.cpp`, `TranslationUnitAnalysis.h/.cpp` (call
graph, SCCs, driver), and the summary-recording and summary-applying paths
in `Dataflow.cpp`. The frontend's consumer hands the whole TU to
`TranslationUnitAnalyzer` and bridges fix-its to `clang::FixItHint`.
`FunctionAnalyzer` remains as the single-function entry point and takes the
`SummaryStore` to read callee summaries from and write its own into.

### Performance

Each function is analysed once, or `rounds + 1` times inside a recursive
SCC. Applying a summary costs one place resolution per effect. On the corpus
harness introduced alongside this RFC (`scripts/corpus.py`) analysis time is
recorded per TU so regressions are visible; the expectation remains "well
under Clang's parse time".

## Annotation surface

None. Existing annotations gain two properties: they are *checked* against
the body (`annotation-mismatch`) and they are *offered* as fix-its.

## Diagnostics

New:

| Id                    | Severity | Message                                                                                   | Notes                                                      |
| --------------------- | -------- | ----------------------------------------------------------------------------------------- | ---------------------------------------------------------- |
| `annotation-mismatch` | error    | `'<p>' is annotated WEAVEC_BORROWED but is freed here` (also `WEAVEC_MUT`; also `moved`)  | `'<p>' is annotated here`; when through an alias, `'<q>' is a copy of '<p>'` |
| `annotation-mismatch` | error    | `'<p>' is annotated WEAVEC_BORROWED but is written through here`                          | as above                                                   |
| `annotation-mismatch` | error    | `function returns a borrow but its return type is annotated WEAVEC_OWNED`                 | `annotated here`                                           |
| `annotation-mismatch` | error    | `function returns a fresh allocation but its return type is annotated WEAVEC_BORROWED` (or `WEAVEC_MUT`) | `annotated here`                            |

Changed:

| Id                    | Change                                                                                                                                                                                                                                  |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `annotation-required` | New primary form, on by default: `call to '<f>' is not checked: it has no definition or ownership annotations here` at the first call, with notes `'<f>' is declared here` and `annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT, or define it in this translation unit`. Under `--report-unannotated`, exported definitions get `pointer parameter '<p>' of '<f>' is inferred WEAVEC_OWNED; add the annotation to its declaration` (or `WEAVEC_BORROWED`/`WEAVEC_MUT`; `return value of '<f>' is inferred WEAVEC_OWNED; ...`) with a fix-it, and the unchanged `pointer parameter '<p>' has no inferable ownership; annotate it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT` when inference is empty. |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short` | Unchanged wording; now also emitted for effects that come from a callee's summary. The `freed here`/`moved here` note points at the call. |

Triggers are the snippets under *Soundness*. Each row gets a unit test and a
lit test under `test/Analysis/rfc0003-*.c`.

## Drawbacks

- **A second lattice.** Summaries are a new piece of Core state with their
  own join, and the fixpoint over SCCs is a second iteration on top of the
  per-function one. Both are small, but every later RFC must keep the
  summary vocabulary in step with the event table.
- **Analysis order becomes global.** Diagnostics for `f` depend on the
  summaries of everything `f` calls, so a change in a leaf helper can
  surface reports far away. This is inherent to inter-procedural checking;
  `--dump-analysis` shows the summary that was applied.
- **Trust in the library table.** A wrong entry silently weakens the
  guarantee for every caller (RFC 0001, *Drawbacks*). The table is small,
  reviewed, and unit-tested entry by entry.
- **The external default is a hole.** Unannotated externs are assumed
  harmless. The warning is the mitigation; making it an error is a flag
  away; closing it properly is Milestone 4 (cross-TU summaries) plus the
  unsafe-blocks RFC (`Raw`).
- **Fixture churn.** Test preludes that used an opaque `use(void *)` must
  now annotate it, since an unannotated extern warns by default.

## Alternatives

- **`Raw` and error for unannotated externs** (RFC 0001's stated stance).
  Sound, but on day one every call to a function from a header WeaveC has
  not seen is an error, which contradicts RFC 0001's goal 4 (incremental
  adoption; unsafe is the default for external code, not an error). We
  keep the *stance* for a strict mode (`--strict-externs`) and make the
  default a warning. Once `unsafe-operation` exists (unsafe-blocks RFC) the
  strict mode should switch to `Raw` results rather than an error at the
  call.
- **Annotation-only (no inference), as in GSL `owner<T>` or Checked C.**
  Simple and sound but requires annotating every helper before anything is
  caught, which is exactly the cost RFC 0001 set out to avoid.
- **Context-sensitive (call-string or cloning) analysis.** More precise
  (`free_if(p, flag)` would be handled per call), but exponential in
  general and unpredictable with budgets. One summary per function is what
  users can read in `--dump-analysis` and predict.
- **Summaries as Rust-style signatures with lifetime parameters.** Cleaner
  theory (`fn strchr<'a>(&'a str) -> &'a str`), but C code cannot write
  them, and inferring them needs the same `Copy(param)`/`Borrow(param)`
  facts this RFC records. The `ValueSource` vocabulary *is* the
  lifetime-parameter relation, expressed as "which argument does the result
  come from", which is enough for the checker and readable by C
  programmers.
- **Must-effects (intersection) instead of may-effects at joins.** Fewer
  false positives at calls that free conditionally, but a caller would then
  use a pointer the callee may have freed with no report. Rejected on
  RFC 0001's soundness-first rule; the same decision it made intra-
  procedurally.
- **Reading all effects from the exit state.** Simplest, but wrong for
  parameter variables that the callee reassigns (`free(p); p = NULL;`
  would summarise as "not freed"). Hence the split rule.
- **Recording all effects as they happen.** Simplest and sound, but reports
  `use-after-free` after `buf_destroy(&b)` when `buf_destroy` re-nulls the
  field, the most common destroy idiom in C. Hence the split rule.

## Prior art

- **Rust**: function signatures *are* summaries, with lifetime parameters
  tying results to arguments. We infer the same relation (`Copy(param)`,
  `Borrow(param)`) instead of asking the user to write it. Rust's insistence
  that the signature is the whole contract (bodies are never consulted by
  callers) is RFC 0001's "annotations are authoritative" and our
  reconciliation check is `rustc`'s borrow check of the body against its
  own signature.
- **Clang Static Analyzer `MallocChecker` and the `ownership_*` attributes**
  (`ownership_returns`, `ownership_takes`, `ownership_holds`): the same
  three-way classification of what a call does to a pointer; our library
  table is the annotated equivalent for libc. The analyzer's inlining-based
  inter-procedural mode is the context-sensitive alternative rejected above.
- **Clang's `[[clang::lifetimebound]]`** and the in-progress lifetime-safety
  analysis: `lifetimebound` is `Borrow(param(i))` on the return value,
  spelled by hand; the lifetime-safety work infers origins intra-
  procedurally and stops at calls without the attribute, which is precisely
  the gap this RFC closes for C.
- **Cyclone's region inference** and **Infer's biabduction** (Calcagno et
  al., 2009): bottom-up, compositional summaries over the call graph with a
  fixpoint for recursion. Infer shows that compositional summaries scale to
  millions of lines and that "may free" summaries are what users expect for
  `free`-like helpers.
- **Steensgaard / Andersen escape analysis**: the `stores` set is an
  escape analysis restricted to caller-visible destinations, which is all
  the borrow checker needs.

## Unresolved questions

**Resolved at acceptance** (decisions recorded inline above):

| Question                              | Decision                                                                                       |
| ------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Unannotated externs                   | Default: borrowed-for-the-call, no retention, unknown result; warning once per callee; `--strict-externs` for error (strict mode redefined by RFC 0004). |
| System-header externs                 | Exempt from the default warning; `--report-unannotated` includes them.                        |
| Where effects are read from           | Parameter roots: as they happen. Everything else: exit state, unless the parameter variable is reassigned. |
| Function pointers                     | Not modelled here; resolved by RFC 0004.                                                       |
| Partial annotations                   | Annotated roots use the annotation; unannotated roots of the same function use inference.     |
| Debug output                          | Extend `--dump-analysis` rather than add a flag.                                               |

**Deferred to corpus testing:**

- Whether the may-effect conservatism at calls (`free_on_error(p)` style
  helpers) is a significant false-positive source. If so, the remedy is
  path-sensitive summaries keyed on the return value (`returns NULL ⇒ freed
  p`), which is the general null-tracking RFC 0002 deferred.
- How often real code re-points parameters and then frees through them.
- Which libc functions outside the table are called often enough to add.

## Future work

- **Unsafe-blocks RFC** (done: [RFC 0004](0004-unsafe-boundaries.md)):
  `unsafe-operation`, analysed unsafe regions, strict-mode extern results
  becoming `Raw`.
- **Cross-TU summaries** (Milestone 4): serialise `FunctionSummary` next to
  `compile_commands.json`; the format here is designed to be the on-disk
  format, keyed by mangled name.
- **Return-value-conditional summaries** and general null tracking.
- **Function pointer summaries** (done: RFC 0004) via the type's
  annotations or by joining every address-taken function of the type.
- **Non-lexical loans**, unchanged from RFC 0002.
