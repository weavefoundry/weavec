# RFC 0004: Unsafe boundaries: raw pointers, unsafe regions and indirect calls

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-01
- **Accepted**: 2026-09-01
- **Implemented**: 2026-09-01
- **Tracking issue**: TBD
- **Supersedes / superseded by**: supersedes RFC 0001 *Unsafe* and RFC 0002
  *Unsafe interaction* / *Places* (opaque expressions) / *Events* (pointer
  arithmetic); supersedes the `--strict-externs` row of RFC 0003
  *Diagnostics*; implements the remaining item of RFC 0001 *Soundness*
  (`unsafe-operation`) and resolves RFC 0001's open question *Function
  pointers and callbacks*.

## Implementation notes

The design below landed as written except where this section says
otherwise. Each item clarifies *how* a decision was built, not the decision;
where the text below disagrees with an item here, the item wins. Behaviour is
pinned by `test/Analysis/rfc0004-*.c`, `test/Annotations/rfc0003-unknown-extern.c`
(strict mode), `test/Driver/dump-analysis.c`, `unittests/Core/RawTest.cpp`,
`unittests/Analysis/DataflowTest.cpp`, `unittests/Analysis/SummariesTest.cpp`
and `unittests/Analysis/SignatureInferenceTest.cpp`.

- **`RawRecord` carries a `detail` string.** Besides `reason`, `location` and
  `via`, the record keeps the text the note needs and the core cannot
  reconstruct: the name of the raw pointer a value was loaded through
  (`LoadedThroughRaw`) or the callee's name (`Callee`, `UnknownCallee`).
  `RawTracker::join` keeps the existing record, so the note names the first
  reason seen on the path that reaches the report.
- **Assertion by assignment applies to fields as well as locals.** Rule 4
  (*Raw pointers*) consults the declared annotation of the destination
  *place*, which may be a variable, a parameter or a field
  (`b->owned = (struct node *)x` with `struct box { struct node
  *WEAVEC_OWNED owned; }`). This is the only thing consumed from a
  `WEAVEC_OWNED`/`WEAVEC_BORROWED`/`WEAVEC_MUT` annotation on a field; the
  *Unresolved questions* row saying such annotations "remain unconsumed"
  should be read as "for every other purpose".
- **Self-assignment is a no-op on the state.** `p = p + k`, `p = (T *)p` and
  `p = p` copy a place's own value back into it; `applyPointerAssign` returns
  before resetting the destination, so aliases, loans, move and raw records
  survive. This is what makes the *Pointer identity* sentence "in-place
  forms no longer reset the place" hold for the spelled-out forms too
  (`cur = cur + 1; free(cur); head->v` is a `use-after-free`).
- **A `WEAVEC_RAW` destination is raw whatever is stored into it.** Storing a
  tracked value into a place declared `WEAVEC_RAW` (`int *WEAVEC_RAW r = q;`,
  or a reassignment of a raw parameter) leaves the source tracked and makes
  the destination raw with reason `Declared`; it is the cast-free form of the
  "out of the model" launder and never an assertion.
- **The callee-name in raw notes is the same text the call diagnostics
  use.** `'<f>'` for a direct callee; for an indirect call, the place holding
  the pointer (`'ops->drop'`) or `a function pointer`, so the note reads
  `handed out by 'lookup' here` or `handed out by a function pointer here`.
- **Annotated function-pointer types still join their candidates.** When a
  function-pointer type carries ownership annotations, `lookupIndirect`
  first joins the summaries of the address-taken candidates of that type
  and then applies the annotations root by root, exactly as
  `SummaryStore::lookup` does for a direct callee whose declaration is
  partly annotated (RFC 0003). Roots with an annotation are therefore
  authoritative and unannotated roots keep the inferred join. The result is
  cached per (type, annotated declaration) and invalidated whenever any
  inferred summary changes or a new address-taken function is registered,
  which is what the *Performance* section's "until any candidate's summary
  changes" means in practice.
- **Address-taken means "used as a value".** `AddressTakenCollector` records
  a function for every `DeclRefExpr` to it that is not the callee of a
  direct call: initialisers (including file-scope aggregate initialisers),
  assignments, arguments, casts and discarded expression statements. A
  function referenced only as a direct callee is never a candidate.
- **Strict mode and system headers.** Default mode keeps RFC 0003's
  exemption of callees declared in system headers from `annotation-required`.
  Strict mode has no such exemption: an unchecked call is an
  `unsafe-operation` at every call site, whatever header declared the
  callee, because the point of strict mode is that nothing unknown is
  silent. Compiler intrinsics (`__builtin_*`) and calls with no pointer
  parameters and no pointer result are never boundaries in either mode.
- **Dump format.** The raw component prints as `raw{<place>@<line>:<col>
  <reason>, ...}`, mirroring `moved{}`; the location is that of the raw
  source (the cast, the `WEAVEC_RAW` declarator, the load or the call). The
  `places:` line shows `raw` as the kind of a place that is declared
  `WEAVEC_RAW` or raw at exit.
- **The library table.** Landed at about 490 entries, well past the "roughly
  200" estimated below, because the headers listed there were covered
  completely rather than by their most common functions. Where this RFC
  says a function "returns unknown" because it hands out static storage
  (`getpwnam`, `localtime`, `strsignal`, ...), the entry does exactly that;
  the `_r` variants (`localtime_r`, `getpwnam_r`, `strtok_r`, ...) are
  patched to return a copy of, or store through, their buffer parameter.
  `pthread_create`'s start routine and its argument have *no* effect in the
  entry (rather than the borrow the text below suggests): the argument is
  handed to another thread, which RFC 0001 puts out of scope, so neither a
  borrow for the duration of the call nor a move describes it; recording
  nothing keeps `pthread_create(&t, 0, worker, ctx); free(ctx)` from being a
  false `conflicting-borrow` and leaves the (real) race for a future
  threads RFC. The corpus baseline was regenerated with this RFC; see
  `scripts/corpus/README.md`.

## Summary

Give the `Raw` ownership kind the semantics RFC 0001 reserved for it and
emit `unsafe-operation` for the first time. A pointer becomes *raw* when the
model can no longer say which object it refers to or who owns that object:
it was cast from an integer, it was declared `WEAVEC_RAW`, it was loaded
through another raw pointer, a callee handed it out as raw, or (under
`--strict-externs`) it came back from code WeaveC cannot see. Dereferencing,
releasing or otherwise trusting a raw pointer is legal only inside an
*unsafe region*, which is a `WEAVEC_UNSAFE` block or the body of a
`WEAVEC_UNSAFE` function. Unsafe regions are no longer skipped: the dataflow
runs through them with diagnostics suppressed and raw operations permitted,
so `WEAVEC_UNSAFE { free(p); } p[0]` is reported where the bug is, outside
the region, and unsafe functions get inferred summaries. Pointer arithmetic
and pointer-to-pointer casts stop being opaque: they preserve the identity
of the object the pointer refers to, so `q = p + 1; free(p); q[0]` is a
`use-after-free`. Calls through function pointers get a signature from
annotations on the function-pointer type or from the join of every function
of that type whose address is taken in the translation unit; with neither
they become a checking boundary exactly like an unannotated external
function (RFC 0003), including under `--strict-externs`, where the whole
boundary now yields raw results instead of an error at the call. A fifth
annotation, `WEAVEC_RAW`, lets declarations say "no ownership guarantee" at
the exact position where that is true. The shipped library table grows from
ISO C to the POSIX and GNU functions real programs call, so that strict mode
is usable.

## Motivation

RFC 0001 promises that in a translation unit with no `WEAVEC_UNSAFE` regions
and no `annotation-required` diagnostics WeaveC reports every use after
free. Three things make that sentence hollow today:

```c
int f1(void) {
  char *p = malloc(8);
  WEAVEC_UNSAFE { free(p); }
  return p[0];                    /* nothing reported                      */
}

int f2(void (*cb)(char *)) {
  char *p = malloc(8);
  cb(p);                          /* cb may free p; not even a warning     */
  return p[0];                    /* nothing reported                      */
}

int f3(void) {
  char *p = malloc(8);
  char *q = p + 1;
  free(p);
  return q[0];                    /* q is "opaque": nothing reported       */
}
```

- **Unsafe is skip-only.** RFC 0002 treats the CFG blocks of an unsafe block
  as identity and RFC 0003 skips unsafe functions entirely, so a release
  inside a region is invisible outside it, and a `WEAVEC_UNSAFE` definition
  with no ownership annotations has an *empty* summary: `poke(p); p[0]` is
  silent. RFC 0001's escape rule (pointers escaping an unsafe block become
  `Raw`) was never built because nothing consumed `Raw`. `OwnershipKind::Raw`
  is produced only by contradictory joins, checked nowhere, and
  `diag::UnsafeOperation` has never been emitted.
- **Function pointers are a hole with no warning.** `handleCall` returns
  before `noteUnknownCallee` when there is no direct callee, so an indirect
  call has no effects, no borrow, and does not trip the RFC 0003 boundary
  warning, even under `--strict-externs`. cJSON's allocator hooks, log.c's
  callbacks, `qsort` comparators and every vtable-style struct of function
  pointers are unchecked.
- **Opaque values are a hole.** RFC 0002 made pointer arithmetic and
  pointer casts produce *no place*, expecting this RFC to turn them into
  `Raw`. Meanwhile `q = p + 1` creates a fresh untracked pointer, and `p++`
  *resets* `p`'s state, so a pointer that walks a buffer and is then freed
  through is not checked at all.

The corpus (`scripts/corpus/README.md`) is clean only because none of the
six projects happens to exercise these paths in a way that produces a
report; that is the definition of a hole rather than of soundness. This RFC
is also the prerequisite for the cross-TU work (Milestone 4) and the
compiler driver (Milestone 3): a strict mode, a summary database that marks
unknown code, and a `-fweavec-strict` flag all need a definition of what an
untrusted pointer *is* and where it may be used.

## Soundness

**Bugs caught after this RFC**, in addition to RFC 0002's and RFC 0003's:

```c
int escape(void) {
  char *p = malloc(8);
  WEAVEC_UNSAFE { free(p); }
  return p[0];                    /* error: use of 'p' after it was freed  */
}

int interior(void) {
  char *p = malloc(8);
  char *q = p + 1;
  free(p);
  return q[0];                    /* error: use of 'q' after it was freed  */
}

int walk(char *WEAVEC_OWNED p) {
  char *cur = p;
  cur++;                          /* cur still refers to p's object        */
  free(cur);
  return p[0];                    /* error: use of 'p' after it was freed  */
}

typedef void (*dtor_t)(void *WEAVEC_OWNED);
int hook(dtor_t dtor) {
  char *p = malloc(8);
  dtor(p);                        /* the type says: consumes its argument  */
  return p[0];                    /* error: use of 'p' after it was moved  */
}

static void my_free(void *p) { free(p); }
struct obj { void (*drop)(void *); char *buf; };
int vtable(void) {
  struct obj o = { my_free, malloc(8) };
  o.drop(o.buf);                  /* inferred: joins every `void (void *)` whose address is taken */
  return o.buf[0];                /* error: use of 'o.buf' after it was freed */
}

int launder(char *WEAVEC_OWNED p) {
  char *r = (char *)(unsigned long)p;   /* r is raw                        */
  return r[0];                    /* error: dereference of raw pointer 'r' outside an unsafe region */
}

char *WEAVEC_RAW mmap_like(void);
int raw_result(void) {
  char *m = mmap_like();
  free(m);                        /* error: 'free' releases raw pointer 'm' outside an unsafe region */
  return 0;
}

WEAVEC_UNSAFE static void poke(char *p) { free(p); }
int unsafe_fn(void) {
  char *p = malloc(8);
  poke(p);                        /* inferred: frees its argument          */
  return p[0];                    /* error: use of 'p' after it was freed  */
}
```

Under `--strict-externs`, additionally:

```c
void *mystery(void *);
int strict(char *p) {
  char *q = mystery(p);           /* error: unchecked call to 'mystery' outside an unsafe region */
  return q[0];                    /* q is raw: second error                */
}
```

**Deliberately not caught:**

- **Anything inside an unsafe region.** Diagnostics whose program point is
  inside a `WEAVEC_UNSAFE` block or function are suppressed. The state
  still flows, so a bug that *starts* inside the region (a free) and
  *manifests* outside it (a use) is reported at the use. That is RFC 0001's
  "unsafe never silences diagnostics about the code around it", now made
  precise: the region hides what happens *in* it, not what it does *to* the
  rest of the function.
- **Type confusion.** A cast between unrelated pointer types preserves the
  place, so reading a `struct b` through a pointer that really addresses a
  `struct a` is not reported. RFC 0001 puts type punning out of scope; this
  RFC merely stops pretending the cast is a mystery. See *Pointer identity*
  for why this is the sound choice for the properties WeaveC does check.
- **Bounds.** `p + 1` is the same object as `p`. `free(p + 1)` is undefined
  behaviour that this model reports as a release of `p`'s object, which is
  the conservative reading for everything WeaveC checks and the only one it
  can make without bounds reasoning (RFC 0001, out of scope).
- **Callbacks registered from another translation unit.** The inferred
  signature of a function-pointer type is the join over functions whose
  address is taken *in this TU*. A function stored into the pointer
  elsewhere is invisible; this is the same single-TU assumption RFC 0003
  already makes for direct calls, and the cross-TU summary database is the
  fix. A function-pointer type nothing in the TU points at is a boundary and
  warns (or is raw under strict mode), so the gap is at least visible.
- **Indirect calls in default mode.** Like unannotated externs, an indirect
  call with no signature borrows its pointer arguments for the call, retains
  nothing, and returns an unknown value. A warning is emitted once per
  function-pointer type. `--strict-externs` closes this.
- **Integer round-trips that the model would accept.** `(char *)(uintptr_t)p`
  is raw even though the value is unchanged. This is deliberate: it is the
  idiom for taking a pointer *out* of the model (see *Laundering*).

**Accepted false positives**, beyond earlier RFCs':

- **Whole-region suppression is coarse.** A user wrapping one dubious line
  in `WEAVEC_UNSAFE { }` also silences a genuine bug on the same line. The
  region should be as small as possible; the annotations reference says so.
- **A raw pointer passed to a callee that only *might* dereference it.**
  The check is against the callee's summary, which is may-effects: a callee
  that reads through its parameter on one path dereferences it for the
  purpose of this rule.
- **Struct copies drop raw records** exactly as they drop move records
  (RFC 0002): fields of a struct assigned as a whole are forgotten, so a
  raw field of the copy is not raw. Consistent, and the same future fix.

**Assumptions.** RFC 0001's, 0002's and 0003's, plus: code inside an unsafe
region upholds the model's invariants for every place it touches (this is
the contract the user signs by writing the annotation); a `WEAVEC_RAW`
annotation is truthful (it can only make callers *more* careful, so an
unnecessary one costs friction, not soundness); the POSIX/GNU library
entries describe the platform's behaviour.

## Detailed design

### Pointer identity

RFC 0002 made three kinds of expression *opaque* (no place, no facts):
pointer arithmetic that may leave an object, casts between unrelated pointer
types, and `container_of`-style subtraction. It deferred the decision of
whether to make them `Raw` to this RFC. The decision is: **none of them is
raw.** A pointer value's identity (which object it refers to, who owns that
object, which loans it carries) is preserved by every pointer-to-pointer
conversion and by pointer arithmetic, and is lost only when the value passes
through an integer.

- `p + k`, `p - k`, `k + p`, `p++`, `p--`, `p += k`, `p -= k` as values are
  *copies* of `p` (an interior pointer into the same object): the result
  aliases `p`, carries `p`'s loans and `p`'s move record. In-place forms no
  longer reset the place. `*(p + k)`, `p[k]` remain `*p` as in RFC 0002.
- Every cast between pointer types is transparent for place identity, not
  only the `void *`/`char *` ones RFC 0002 allowed. `(struct sockaddr *)&sin`
  is a borrow of `sin`; `(struct outer *)((char *)m - offsetof(...))` is a
  copy of `m`.
- An integer-to-pointer conversion (`(T *)(uintptr_t)p`, `(T *)0x1000`,
  `(T *)hash`) produces a **raw** value; see *Raw pointers*.
- A null pointer constant stays `Null`.

Rationale. The properties WeaveC checks are about *objects*: was this one
freed, is it borrowed, does it outlive that one. A cast changes the type
through which an object is viewed, never the object, so treating it as
identity-preserving is *sound* for those properties, and it is the reading
that makes the socket API (`bind(fd, (struct sockaddr *)&addr, len)`),
`container_of`, tagged-pointer-free intrusive lists and every `void *`
callback context checkable. Making these `Raw`, as RFC 0002 anticipated,
would have made every one of them an error outside `WEAVEC_UNSAFE` and
buried the small set of genuinely provenance-destroying operations under
noise. The integer boundary is where C itself loses provenance (C2y TS 6010,
*provenance-not-via-integers*), so it is the natural and explainable line.
The corpus confirms the intuition: `sds`, `printf.c` and `cJSON` do the
arithmetic-and-cast dance on every line and none of it is unsafe in the
ownership sense.

`PlaceBuilder::classifyValue` gains a `Raw` origin for integer-to-pointer
casts and treats arithmetic and pointer casts as copies; `isTransparentCast`
becomes true for every pointer-to-pointer pair. The `Opaque` origin remains
for expressions that are genuinely not pointer values the model tracks
(a struct-returning call's field, a value read from an untracked place) and
keeps its RFC 0002 meaning: no facts.

### Raw pointers

A **raw** pointer is one about which the model has no ownership knowledge:
it may point anywhere, the object may be freed, borrowed or live, and no
loan or move record describes it. Rawness is tracked as a fact about a
*place*, in a new state component:

```
State = { moves, loans, aliases, reallocs, kinds, raw }
raw   : Map<PlaceId, RawRecord{reason, location, via}>
```

`core::RawTracker` mirrors `MoveTracker`: `markRaw`, `clear`, `rawAt`,
`join` (set union, "may be raw", existing record kept), `rawPlaces`. It is
Clang-free. `AnalysisState::forget` clears a place's raw record; joins are
monotone over the finite set of places, so the lattice height argument of
RFC 0002 is unchanged. A raw place's kind is `OwnershipKind::Raw`, which is
how `--dump-analysis` and `--report-unannotated` see it.

A place becomes raw when a raw *value* is stored in it. A value is raw when
it is:

| Source (`RawReason`)  | Produced by                                                                                   |
| --------------------- | --------------------------------------------------------------------------------------------- |
| `IntegerCast`         | an integer-to-pointer conversion                                                              |
| `Declared`            | a read of a place declared `WEAVEC_RAW` (parameter, local, global, field); see *Annotation surface* |
| `LoadedThroughRaw`    | a pointer loaded from memory reached through a raw pointer (`raw->next`, `*raw`)             |
| `Callee`              | the result of, or a store by, a callee whose summary says `raw` (`ValueSource::Kind::Raw`)   |
| `UnknownCallee`       | under `--strict-externs`, the result of a call into code with no summary (see *Boundaries*)  |
| copy                  | a copy of a raw place; the record is copied with `via` set so notes can say `(through 'q')`  |

Rawness is *not* produced by conflicting kinds at a join any more. RFC 0001
let `join(Owned, Shared) = Raw` stand for "contradiction"; that reading
made `Raw` mean two different things. From this RFC, `kinds` still joins to
`Raw` as a lattice value (nothing else fits), but the checker acts only on
the `raw` component, so a contradictory join is a precision loss recorded
in `--dump-analysis`, not an error. The *Unresolved questions* of RFC 0001
asked whether a separate contradiction element was needed; the answer is
that the two concepts live in different components.

A **raw operation** is any of the following on a raw place `p` (or a raw
value with no place):

1. **Dereference**: `*p`, `p->f`, `p[i]`, `*(p + k)`, as a load or as a
   store target.
2. **Release or move**: `free(p)`, passing `p` to a parameter whose summary
   frees or moves it.
3. **Passing to a dereferencing callee**: passing `p` to a parameter that
   the callee's summary reads, writes, frees or stores through
   (`borrowKind(i)` is not none or `consumes(i)`). The call *is* the
   dereference. Passing `p` to a parameter with no effects (the callee only
   stores or compares it), or to a `WEAVEC_RAW` parameter, is not a raw
   operation.
4. **Asserting a safe kind**: storing `p` into a place declared
   `WEAVEC_OWNED`, `WEAVEC_BORROWED` or `WEAVEC_MUT`, or returning `p` from
   a function whose return type carries one of those annotations. Inside an
   unsafe region this is the way back into the model (see *Laundering*);
   outside it is an error.

Everything else is safe: copying a raw pointer (`q = p`, storing it in a
field, passing it to a parameter with no effects or to a `WEAVEC_RAW` one),
comparing it, converting it to an integer, returning it from a function
whose return is unannotated (the summary then says `raw` and the caller
inherits the obligation), storing it through an out-parameter or into a
global (the summary store says `raw`). These are Rust's rules for `*mut T`:
raw pointers are ordinary values; only dereferencing them, and calling
`unsafe fn`s with them, requires `unsafe`.

Outside an unsafe region every raw operation is an `unsafe-operation`
error (see *Diagnostics*), reported once per program point in the RFC 0002
final pass. The operation is then carried out as if the pointer were not
raw (a dereference reads, a free frees) so that the rest of the analysis
continues with the most information available and one mistake produces one
report.

Raw pointers interact with the other components as follows. A raw place has
no loans and no move record of its own (storing a raw value `forget`s the
destination first, as any assignment does); if it aliases another place
(`q = p` where `p` is raw makes `q` raw *and* an alias of `p`), facts about
the alias still propagate, which is harmless because the raw check fires
first. Loading a pointer *through* a raw pointer yields a raw value (its
provenance is unknown); inside an unsafe region that is the normal way raw
data structures are walked. Passing a pointer to a `WEAVEC_RAW` parameter
does nothing to the argument: the callee has promised, by writing unsafe
code, to uphold the caller's invariants, which is exactly the contract of a
Rust function taking `*mut T`.

### Laundering

Two idioms follow from the rules above and are the intended way to move a
pointer across the boundary in each direction.

**Out of the model.** `char *r = (char *)(uintptr_t)p;` makes `r` a raw
pointer with no alias relation to `p`. `free(p)` does not touch `r`; `r`
can be used only inside unsafe regions. This is `p as *mut T`. It exists so
that a user who must keep a dangling or otherwise unmodellable pointer has a
way to say so that is visible in the code and greppable, instead of marking
the whole function unsafe. `WEAVEC_RAW` on the declaration of `r` says the
same thing without the cast.

**Back into the model.** Inside an unsafe region, assigning a raw value to a
place declared with a safe kind asserts that kind:

```c
struct node *WEAVEC_OWNED n;
WEAVEC_UNSAFE { n = (struct node *)registry_lookup(id); }   /* n is Owned */
free(n);                                                     /* checked    */
```

After the assertion the destination is not raw, has the declared kind, and
is a fresh singleton in the alias relation (for `Owned`: a new resource to
release exactly once; for `Shared`/`Mutable`: a pointer whose loan is not
known, so nothing is checked against its referent). Returning a raw value
from a function annotated `WEAVEC_OWNED`/`WEAVEC_BORROWED`/`WEAVEC_MUT`
inside an unsafe region is the same assertion for callers. This is
`&*raw`/`Box::from_raw(raw)`: the one place where the user, not the
checker, is the source of truth, and it must be inside `unsafe`.

To make the assertion expressible, ownership annotations on *local
variables* are now consumed: a local declared `WEAVEC_OWNED`,
`WEAVEC_BORROWED` or `WEAVEC_MUT` has that declared kind for the purposes
of rule 4 and of `--dump-analysis`. Annotations on locals still do not
create loans or change what is checked otherwise (that is future work,
noted below); annotations on *fields* are consumed only for `WEAVEC_RAW`.

### Unsafe regions

An **unsafe region** is the body of a function annotated `WEAVEC_UNSAFE` or
a compound statement annotated `WEAVEC_UNSAFE`. Inside a region:

1. the dataflow runs exactly as outside: every statement is translated to
   events, state flows through, summaries record effects;
2. raw operations are permitted and un-raw their destination when they are
   assertions (rule 4 above);
3. no diagnostic whose program point is inside the region is emitted.

Point 1 is the change from RFC 0002 (which treated the region's CFG blocks
as identity) and RFC 0003 (which did not analyse unsafe functions at all).
The consequences:

- `WEAVEC_UNSAFE { free(p); } p[0]` reports `use-after-free` at `p[0]`.
- A `WEAVEC_UNSAFE` function's summary is inferred from its body like any
  other function's; ownership annotations on it remain authoritative per
  root (RFC 0003). `poke(p); p[0]` above is caught. The RFC 0001/0003 rule
  "the signature is the contract" is thereby narrowed to what it was always
  meant for: a `WEAVEC_UNSAFE` *declaration without a body* is still an
  explicit opt-out with an empty summary and no `annotation-required`
  warning.
- RFC 0001's escape rule ("every pointer that escapes the block is `Raw`")
  is not needed and not implemented. Values produced inside the region by
  operations the model understands are tracked; values produced by
  operations it does not understand are raw by the rules above and stay raw
  when they escape, which is the escape rule with a precise definition of
  "escapes". A pointer merely *read* inside the region is unaffected, which
  is what makes `WEAVEC_UNSAFE { hash = mix(buf); } buf[0]` work.

Point 3 keeps RFC 0001's promise that unsafe is the user's scoped override
for the model's accepted false positives (merge-point conservatism, lexical
loans, may-effects at calls). Rust does not need this because its checker
accepts no false positives it knows of; WeaveC's RFCs list theirs, and a
tool that lists its false positives must offer a scoped way past them.
Suppression is by program point: a diagnostic *about* an object freed inside
the region but *at* a use outside it is not suppressed.

Implementation: `FunctionDataflow` keeps the RFC 0002 pre-pass that marks
statements under an unsafe block, but the transfer function consults it to
set an `inUnsafe` flag per CFG element instead of skipping the element;
`report` drops diagnostics while the flag is set, and the raw checks consult
it. A `WEAVEC_UNSAFE` function sets the flag for the whole body.
`FunctionAnalyzer::analyze` no longer returns early for unsafe functions.

### Boundaries: indirect calls and unknown code

RFC 0003 defines a *boundary*: a callee with no summary. It made a call
across the boundary borrow its pointer arguments for the call, retain
nothing, return an unknown value, and warn once per callee; `--strict-externs`
turned the warning into an error. This RFC extends the boundary to indirect
calls and redefines strict mode.

**Signatures for function pointers.** `SummaryStore::lookupIndirect(call)`
resolves the callee expression's function type in this order:

1. **Annotations on the function-pointer type.** The callee expression is
   stripped to the declaration it names (a variable, parameter or field of
   function-pointer type, possibly through a `typedef`) and that
   declaration's type location is walked to its `FunctionProtoTypeLoc`.
   Annotations on the prototype's parameters are the parameters'
   annotations; an ownership annotation on the declaration itself (the
   `typedef`, field, variable or parameter of function-pointer type) or on
   a `typedef` passed through on the way describes the function's *result*
   (an owned function pointer is not a meaningful concept, so the position
   is free). Clang keeps `annotate` on the `ParmVarDecl`s of a prototype in
   a `typedef`, field or parameter declarator, so `typedef void
   (*dtor_t)(void *WEAVEC_OWNED);` works as written. Annotated roots are
   authoritative exactly as in RFC 0003 (they replace inferred effects for
   that root); a type with any ownership annotation is a signature.
2. **The join of address-taken functions.** `TranslationUnitAnalyzer`
   collects, over the whole TU including file-scope initialisers, every
   function whose name is used as a value rather than as the callee of a
   direct call. For an indirect call with callee type `T` (after stripping
   the pointer and any typedefs), the summaries of all collected functions
   whose canonical function type is `T`, resolved through `SummaryStore::
   lookup` (so annotations, inferred bodies and the library table all
   apply), are joined. This is the RFC 0003 summary lattice join, so it is
   the may-summary of "whichever of these is called". `static internal_hooks
   hooks = { internal_malloc, internal_free }` and `qsort(a, n, sz, cmp)`
   both register their functions; a cast such as `(void (*)(void *))free`
   registers `free` under its own type, which is the type the call site
   sees. Unprototyped (`K&R`) function types get no signature.
3. **Otherwise the call is a boundary.**

The call graph gains an edge from a function containing an indirect call to
every collected function of the compatible type, so callbacks are analysed
before their callers and recursion through a callback is one SCC.

**Boundary semantics.** For a direct call to a callee with no summary and for
an indirect call with no signature:

- **Default.** As RFC 0003: pointer arguments are borrowed for the call and
  not retained, the result is unknown, and `annotation-required` is emitted
  once per callee (direct) or once per function type (indirect). The
  indirect message says what to do: annotate the type, or take the address
  of a function of that type here.
- **`--strict-externs`.** The call is a raw operation and its pointer result
  is raw (`UnknownCallee`). Outside an unsafe region it is an
  `unsafe-operation` error at every call site; inside one it is silent, the
  arguments are untouched (the region's author vouches for the callee), and
  the raw result must be handled by the rules above. No
  `annotation-required` is emitted in strict mode; the error replaces it.
  This is what RFC 0003 asked for ("once `unsafe-operation` exists the
  strict mode should switch to `Raw` results rather than an error at the
  call"), and it is Rust's FFI rule: `extern` functions are `unsafe fn`.

### The library table

Strict mode is only usable if the functions real programs call have
summaries, and the default-mode boundary warning is only informative if it
does not fire on `getline`. `Builtins.cpp` grows from the 85 ISO C entries
to roughly 200, adding the POSIX and common GNU/BSD functions that take or
return pointers: `<unistd.h>` (`read`, `write`, `close`, `pipe`, `dup2`,
`getcwd`, `readlink`, ...), `<fcntl.h>`, `<sys/stat.h>`, `<dirent.h>`
(`opendir`/`fdopendir` produce an owned `DIR *`, `closedir` releases it,
`readdir` returns a borrow of the directory stream), `<stdio.h>` extensions
(`getline`, `getdelim`, `asprintf`, `vasprintf`, `popen`/`pclose`,
`fmemopen`, `open_memstream`, `dprintf`, `freopen`), `<stdlib.h>` extensions
(`posix_memalign`, `reallocarray`, `realpath`, `mkstemp`, `mkdtemp`,
`setenv`/`unsetenv`, `qsort_r`), `<string.h>`/`<strings.h>` extensions
(`strtok_r`, `strsep`, `stpcpy`, `stpncpy`, `strcasestr`, `strchrnul`,
`memmem`, `mempcpy`, `memrchr`, `strlcpy`, `strlcat`, `strcasecmp`,
`strncasecmp`, `explicit_bzero`), `<time.h>` (`localtime_r`, `gmtime_r`,
`strftime`, `strptime`, `clock_gettime`, `nanosleep`; the static-storage
variants return unknown), `<sys/mman.h>` (`mmap` produces an owned mapping,
`munmap` releases it), `<pthread.h>` (create/join/mutex/cond/rwlock/key
functions; the start routine and its argument are borrowed for the call and
not modelled as escaping, because threads are out of scope per RFC 0001),
`<sys/socket.h>`/`<netdb.h>`/`<arpa/inet.h>` (`getaddrinfo` stores a fresh
list through its out-parameter, `freeaddrinfo` releases it, `inet_ntop`
returns its buffer argument), `<dlfcn.h>` (`dlopen` produces an owned
handle, `dlclose` releases it), `<regex.h>`, `<signal.h>`, `<sys/wait.h>`,
`<sys/time.h>`, `<sys/uio.h>`, `<poll.h>`/`<sys/select.h>`, `<err.h>`,
`<syslog.h>`, `<libgen.h>`, `<pwd.h>`/`<grp.h>` (static storage: unknown),
`<iconv.h>`, `<glob.h>` and `<fnmatch.h>`. Entries whose behaviour the spec
string cannot express (`getline`, `asprintf`, `posix_memalign`,
`getaddrinfo`, `strtok_r`: stores through out-parameters) are patched by
hand as `strtol` already is. Functions returning pointers to static storage
(`getpwnam`, `localtime`, `strsignal`, `ttyname`, ...) return unknown, as
`getenv` does; a `Static` value source remains future work.

Each entry describes ISO C / POSIX behaviour, not a platform extension. The
table is unit-tested entry by entry as before (`SummariesTest.cpp`); the
RFC 0003 drawback about trusting the table grows with it and is mitigated
the same way (small specs, reviewed, tested).

### Debug output

`--dump-analysis` prints the raw component after the alias relation, a
`raw` kind for raw places, and `raw` as a value source:

```
function 'launder':
  places: p (param, owned) r (local, raw)
  lifetimes: caller fn
  exit: moved{} loans{} aliases{} raw{r@3:13 integer-cast}
  summary: stores{} returns{raw}
```

Reasons are spelled `integer-cast`, `declared`, `loaded-through-raw`,
`callee`, `unknown-callee`. The format remains a debugging aid pinned only
by `test/Driver/dump-analysis.c`.

### Layering

New in Core: `Raw.h`/`Raw.cpp` (`RawReason`, `RawRecord`, `RawTracker`),
`AnalysisState::raw`, `ValueSource::Kind::Raw`, `toString(RawReason)`;
`FunctionSummary::inferredReturnKind` reports `Raw` when any alternative is
raw. All Clang-free and unit-tested.

New in Analysis: `Annotation::Raw` and `spelling::Raw`; a
`FunctionTypeAnnotations` collector that walks a declarator's type location
to its prototype; `SummaryStore::addAddressTaken`, `lookupIndirect` and the
per-type boundary set; `PlaceBuilder`'s `Raw` origin, arithmetic-as-copy
and all-pointer-casts-transparent rules, plus `isDeclaredRaw` for places
whose variable or field carries `WEAVEC_RAW`; in `FunctionDataflow`, the
unsafe-region flag, the raw checks in `doRead`/`doConsume`/borrow-for-call/
`applyPointerAssign`/`handleReturn`, raw propagation on copies and loads,
declared kinds for locals, and the strict-mode boundary. The TU driver
collects address-taken functions and indirect edges.

Frontend and driver: no new flags. `--strict-externs` changes meaning as
described; its help text is updated.

### Performance

The raw component is one more map joined per edge, bounded by the number of
pointer places. Indirect-call resolution is one type-keyed lookup per call
with the join cached per type until any candidate's summary changes. The
address-taken pre-pass is one traversal of the TU. Expected cost on the
corpus: within noise of the current 1.1 s; measured by `scripts/corpus.py`
as before.

## Annotation surface

One new macro in `resources/include/weavec.h`, mirrored in
`include/weavec/Analysis/Annotations.h` (`spelling::Raw = "weavec.raw"`) and
`docs/annotations.md`:

| Macro        | Attaches to                                                                                            | Meaning                                                                 |
| ------------ | ------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------- |
| `WEAVEC_RAW` | pointer parameters, returns, variables, fields; the same positions inside function-pointer types | No ownership guarantee. Dereferencing or releasing the pointer requires an unsafe region. |

Semantics by position:

- **Parameter.** In the body the parameter place is raw at entry. Callers
  are unaffected: passing any pointer to a `WEAVEC_RAW` parameter is not a
  raw operation and has no effect on the argument (the summary records
  nothing for that root).
- **Return.** The result is raw for callers (`returns{raw}`). Returning a
  raw value from such a function is not an assertion and needs no unsafe
  region.
- **Local or global variable.** Every value read from the place is raw.
  This is the spelling of the "out of the model" launder without a cast.
- **Field.** Every value read from the field place is raw (`s->h` for
  `struct s { void *WEAVEC_RAW h; }`). Useful for opaque handles kept in
  structs.
- **Inside a function-pointer type** (`typedef`, field, parameter): as for
  a function declaration's parameters and return.

Annotations remain authoritative and, where a body exists, checked. A
`WEAVEC_RAW` parameter that the body dereferences outside an unsafe region
is an `unsafe-operation`, which is the reconciliation for this annotation;
no `annotation-mismatch` form is added because there is nothing a body can
do to contradict "no guarantee".

Existing macros change as follows. `WEAVEC_UNSAFE` on a definition now means
"the body is an unsafe region" (analysed, suppressed, raw permitted) rather
than "skip the body"; on a declaration without a body it is unchanged.
`WEAVEC_OWNED`, `WEAVEC_BORROWED` and `WEAVEC_MUT` on local variables are
consumed for the assertion rule and the dump; nothing else about them
changes. Ownership annotations on a declaration of function-pointer type,
or on a `typedef` of one, describe the function's result; on the
parameters inside such a type they describe the parameters.

## Diagnostics

New: `unsafe-operation` (error), reserved by RFC 0001, emitted for the first
time. One message per kind of raw operation; each carries a note saying why
the pointer is raw.

| Message                                                                                             | Trigger                                                                                     |
| --------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `dereference of raw pointer '<p>' outside an unsafe region`                                         | `*p`, `p->f`, `p[i]` with `p` raw (as load or store target); also a dereference of a raw value with no place, named `raw pointer` |
| `'<f>' dereferences raw pointer '<p>' outside an unsafe region`                                     | `p` passed to a parameter `f` reads or writes through                                      |
| `'<f>' releases raw pointer '<p>' outside an unsafe region`                                         | `free(p)`, or `p` passed to a parameter `f` frees                                           |
| `'<f>' takes ownership of raw pointer '<p>' outside an unsafe region`                               | `p` passed to a parameter `f` moves                                                         |
| `raw pointer '<p>' is assigned to '<q>', which is declared <MACRO>, outside an unsafe region`       | assertion by assignment (rule 4)                                                            |
| `raw pointer is returned from a function whose return type is annotated <MACRO> outside an unsafe region` | assertion by return (rule 4)                                                          |
| `unchecked call to '<f>' outside an unsafe region`                                                  | `--strict-externs`: direct call to a callee with no summary                                 |
| `unchecked call through '<fp>' outside an unsafe region`                                            | `--strict-externs`: indirect call with no signature; `<fp>` is the place holding the pointer, or `a function pointer` |

Notes. For the first six rows: `'<p>' is raw: <why> here` (or `the pointer
is raw: <why> here` for a value with no place), where `<why>` is `cast from
an integer`, `declared WEAVEC_RAW`, `loaded through raw pointer '<r>'`,
`handed out by '<f>'` (a callee summary's `raw` return or store), or
`returned by a call into unchecked code ('<f>')`; when the record was
copied, the note ends with ` (through '<q>')` as `freed here` does. Every
row also carries `move this operation into a WEAVEC_UNSAFE block or
function, or assert the pointer's ownership first`. For the strict-mode
rows: `'<f>' is declared here` and `annotate its pointer parameters with
WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, define it in this
translation unit, or move the call into a WEAVEC_UNSAFE region` (direct);
`annotate the parameters of its function type, take the address of a
function of that type in this translation unit, or move the call into a
WEAVEC_UNSAFE region` (indirect).

Triggers are the snippets under *Soundness*.

Changed:

| Id                    | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `annotation-required` | New form for indirect calls, on by default, once per function-pointer type: `call through '<fp>' is not checked: its function type has no ownership annotations and no function of that type has its address taken in this translation unit`, with note `annotate the parameters of its function type with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or take the address of a function of that type in this translation unit`. The direct-call form's second note gains `or WEAVEC_RAW`. Under `--strict-externs` neither form is emitted; `unsafe-operation` replaces them. The `--report-unannotated` fix-it offers `WEAVEC_RAW` for a return value inferred raw. |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`, `annotation-mismatch` | Unchanged wording. Now also emitted for effects that originate inside an unsafe region and manifest outside it, and for pointers produced by arithmetic or casts. Never emitted at a program point inside an unsafe region. |

## Drawbacks

- **Two meanings of `Raw`.** The lattice element (a contradictory join) and
  the state fact (an untrusted pointer) share a name. This RFC keeps the
  lattice as RFC 0001 defined it and gives the checker's meaning to the
  state component, which is the smaller change; renaming the lattice element
  would touch every RFC.
- **Suppression by region is coarse** (see *Soundness*). Finer control
  (per-diagnostic-id suppression, `WEAVEC_UNSAFE` on a single expression)
  is possible later without changing this design.
- **Analysing unsafe bodies costs time and may report more.** An unsafe
  function that was previously skipped now contributes a summary; callers
  that were silently fine may now see reports. That is the point, but it is
  a behaviour change for anyone who used `WEAVEC_UNSAFE` as "ignore this".
- **Pointer casts as identity lose the RFC 0002 hedge.** If type punning
  ever comes into scope, the cast will need a fact of its own. Nothing
  accepted now becomes rejected then; it becomes rejected for a new reason.
- **Per-type join for function pointers is coarse.** All `int (*)(const
  void *, const void *)` comparators share one summary. Precise per-place
  tracking of which function a pointer holds is a points-to analysis, which
  RFC 0001 declined. The join is a may-summary, so coarseness costs
  precision, not soundness.
- **The boundary set grows.** With indirect calls counted, more real code
  warns by default. The warning names the fix.

## Alternatives

- **RFC 0001's escape rule as written** (skip the region; every outer place
  touched inside becomes raw). Simple and sound, but it makes a pointer
  merely *read* inside a region unusable afterwards, so `WEAVEC_UNSAFE {
  log(p); } p[0]` would be an error, and it throws away the effects the
  model *does* understand inside the region, so `WEAVEC_UNSAFE { free(p); }
  p[0]` would stay silent. Analysing the region and letting rawness attach
  to genuinely unmodellable values is strictly more useful and no less
  sound.
- **Rust's rule: no suppression inside unsafe.** Principled, but WeaveC's
  RFCs commit to accepted false positives and promise unsafe as the scoped
  override for them. Dropping suppression would leave users with no
  alternative to restructuring their code. Revisit when non-lexical loans
  and null tracking have shrunk the false-positive list.
- **Make opaque expressions `Raw`**, as RFC 0002 anticipated. Rejected
  under *Pointer identity*: it errors on the socket API, `container_of`,
  and every buffer walk, for no gain in the properties WeaveC checks.
- **Keep arithmetic opaque** (RFC 0002 status quo). Rejected: it is a
  soundness hole (`q = p + 1; free(p); q[0]`), and making the result an
  alias costs nothing.
- **Indirect calls as errors unless annotated.** Sound and simple, but it
  contradicts RFC 0001 goal 4 and RFC 0003's boundary decision for direct
  calls. Treating the two boundaries identically is easier to explain and
  strict mode gives the sound variant.
- **Infer function-pointer signatures per place** (track which functions
  flow into `o.drop`). More precise for structs of hooks, but it is a
  points-to analysis with all the predictability problems RFC 0001 declined,
  and per-type inference already handles the common cases (hook tables
  initialised with function names, callbacks passed as arguments).
- **A `WEAVEC_UNSAFE_FN` annotation** meaning "callers must be in unsafe"
  (Rust `unsafe fn`). Not needed: `WEAVEC_RAW` on the parameters and return
  expresses the caller's obligation at the exact position it applies, and
  strict mode makes every unknown function behave that way. Adding a second
  meaning to `WEAVEC_UNSAFE` would have been confusing.
- **Warning severity for `unsafe-operation`.** Rejected: rawness only ever
  comes from an explicit source (a cast the user wrote, an annotation the
  user wrote, or strict mode the user asked for), so an error is never a
  surprise, and RFC 0001 fixed the severity.
- **Do nothing.** Leaves the soundness statement false and blocks
  Milestones 3 and 4 on the definition of untrusted code.

## Prior art

- **Rust**: `*const T`/`*mut T` are ordinary values; only dereferencing
  them and calling `unsafe fn` (including every `extern "C"` function)
  requires an `unsafe` block; `&*raw` and `Box::from_raw` are the
  assertions back into the type system; the borrow checker still runs
  inside `unsafe`. We take the value/operation split, the assertion idiom
  and the FFI rule, and depart on suppression inside regions for the reason
  given under *Alternatives*. The Rustonomicon's chapter on unsafe is the
  source of "the region's author upholds the invariants" as the contract.
- **C provenance (C2y TS 6010, PNVI-ae-udi)**: pointer arithmetic and
  pointer casts preserve provenance; integer round-trips are where it is
  lost and must be re-established. Our identity rule is that model applied
  to ownership, and our `IntegerCast` raw source is its "exposed" boundary.
- **Clang Static Analyzer `MallocChecker`**: treats a pointer that escapes
  through an integer cast or an unknown call as *escaped* and stops tracking
  it, precisely to avoid false positives. We instead keep tracking it as raw
  and require the user to be explicit about using it; the analyzer's list of
  escape events (unknown call, cast to integer, stored through unknown
  pointer) is the checklist our raw sources were compared against.
- **Checked C**: `_Ptr<T>` vs unchecked `T *` and `_Unchecked { }` scopes.
  Their experience that the unchecked/checked boundary needs an
  *annotation on the pointer* (not only a region) is why `WEAVEC_RAW`
  exists in parameter, return and field positions.
- **Cyclone**: `@notnull`/`@fat` pointer qualifiers and the `unsafe`
  escape. Confirms that a small number of pointer *qualifiers* plus one
  region construct covers systems code.
- **Clang `[[clang::lifetimebound]]`, `__attribute__((callback))`**: the
  `callback` attribute is the existing way to tell Clang which parameter a
  function will call with which arguments; our annotations on
  function-pointer types are the ownership counterpart, and the
  address-taken join is what the attribute's absence forces us to infer.
- **CIL / `SAFECode` / SoftBound** on function pointers: joining the set of
  address-taken functions of a compatible type is the standard sound
  approximation for indirect calls without points-to analysis (the "class
  hierarchy analysis" of C); we use it for the same reason they did.

## Unresolved questions

**Resolved at acceptance** (decisions recorded inline above):

| Question                                     | Decision                                                                                            |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| What makes a pointer raw                     | Integer casts, `WEAVEC_RAW`, loads through raw, callee `raw` sources, strict-mode unknown results. Not pointer casts, not arithmetic, not contradictory joins. |
| Unsafe regions: skip or analyse              | Analyse; suppress diagnostics inside; permit and un-raw assertions inside.                          |
| `WEAVEC_UNSAFE` on definitions               | Body is an unsafe region; summary inferred; annotations authoritative. Declarations without body unchanged. |
| Escape rule for unsafe blocks                | Subsumed: raw values stay raw when they escape; tracked values stay tracked.                        |
| Function pointers                            | Type annotations, then join of address-taken functions of the type, then boundary.                  |
| Indirect calls with no signature             | Same as unannotated externs: warning once per type by default; raw operation under strict mode.     |
| `--strict-externs`                           | Raw operation at the call, raw result; `unsafe-operation` outside unsafe regions; no error inside.  |
| `unsafe-operation` severity                  | Error.                                                                                              |
| `WEAVEC_UNSAFE_FN`                           | Not added; `WEAVEC_RAW` on parameters/returns carries the caller's obligation.                      |
| Annotations on locals                        | Consumed for the assertion rule and the dump; loans/kinds from them remain future work.             |
| Which kinds are asserted by `WEAVEC_RAW` on fields | Only rawness; `WEAVEC_OWNED` on fields remains unconsumed (future work).                    |

**Deferred to corpus testing:**

- How many indirect calls in real code fall to the boundary (no annotation,
  no address-taken function of the type in the TU). The initial corpus
  answer is recorded in `scripts/corpus/README.md`.
- Whether per-type joining of comparators and hooks is ever too coarse in
  practice (a freeing hook and a non-freeing one sharing a type).
- Which POSIX functions outside the expanded table are still called often
  enough to add.

## Future work

- **Cross-TU summaries (Milestone 4)** carry `raw` as a value source and
  make the address-taken join whole-program.
- **Compiler driver (Milestone 3)**: `-fweavec-strict` is `--strict-externs`;
  `-Wno-weavec-unsafe-operation` should not exist (it is an error), but the
  boundary warning gets a `-W` group.
- **Ownership annotations on fields and locals as loans/kinds** (a local
  declared `WEAVEC_BORROWED` creating a loan on assignment).
- **Static storage as a value source** (`getenv`, `localtime`), replacing
  `unknown` in the library table.
- **Allocator families** (`mmap`/`munmap`, `opendir`/`closedir`, custom
  pools): releasing with the wrong function is a bug the summary vocabulary
  can express (`freed` by family) but does not yet.
- **Finer suppression** (`WEAVEC_UNSAFE` on an expression, per-id
  suppression) if regions prove too coarse.
- **Reconciliation for function-pointer types**: an annotated `typedef`
  whose address-taken functions contradict it is an `annotation-mismatch`
  waiting to be specified.
