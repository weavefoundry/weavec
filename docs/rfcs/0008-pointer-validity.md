# RFC 0008: Pointer validity: null dereferences, uninitialised pointers, invalid releases and replaced values

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-03
- **Accepted**: 2026-09-03
- **Tracking issue**: TBD
- **Supersedes / superseded by**: amends the RFC 0003 rule on *where
  effects are read from* (*Deriving a summary*, third bullet: frees and
  moves below a dereference are read from the exit state alone); extends
  the RFC 0003 summary vocabulary (`replaced`, `requires`, `notnull`, the
  `result` root) and the RFC 0006/0007 outcome facts (`nonNullOn`);
  resolves the RFC 0007 *Future work* item "Nullability (a later RFC)".

## Summary

WeaveC answers "who owns this pointer and for how long". It does not yet
answer "is this pointer *valid right now*". Four things fall through today:

1. A callee that releases a value reachable from its arguments and then
   overwrites the place it came from (`v->items = realloc(v->items, n)`,
   `free(b->data); b->data = NULL;`) is summarised as if nothing was
   released, because RFC 0003 reads such effects from the callee's exit
   state. A caller's own copy of the old value is then used without a
   report. This is a soundness hole in the guarantee RFC 0002 gives.
2. Nothing tracks nullness. `p = malloc(n); p[0] = 1;`, `q = strchr(s,
   ':'); *q`, `if (!n) log(); return n->v;` and passing a possibly-null
   pointer to `strlen` are all silent.
3. Reading a pointer variable or a pointer field of a local struct that was
   never initialised is silent (Clang catches the variable case; nothing
   catches `struct s x; free(x.data);`).
4. Releasing something that was never allocated is silent: `free(buf)` on a
   stack array, `free("literal")`, `free(p + 1)`, `free(&s)`, `free(g)` on
   a static array (which today crashes the checker).

This RFC closes all four with three new diagnostics (`null-dereference`,
`use-of-uninitialized`, `invalid-release`), one new state component
(nullness), two annotations (`WEAVEC_NULLABLE`, `WEAVEC_NONNULL`), and a
summary vocabulary extension (`replaced`, `requires`, `notnull`, a `result`
root for struct-by-value returns). Summary text format 3 → 4, sidecar
format 3 → 4.

## Motivation

The README promises Rust-style memory safety for C. Rust's `Option<Box<T>>`
and its refusal to read uninitialised memory or to free anything but a
`Box` are as much a part of that promise as the borrow checker. Of the
memory-safety CWEs that appear in real C CVE data, null dereference
(CWE-476) is the most frequent after out-of-bounds access, and "invalid
free" (CWE-590/761) is a steady presence; WeaveC reports neither.

Item 1 is worse than a missing feature. RFC 0003 chose to read consumption
of non-root paths from the exit state so that `buf_destroy(&b)`, which
frees `b.data` and nulls the field, does not make the *caller's* later
`if (b.data)` a use-after-free. The choice is right for the caller's own
place. It is wrong for every other name the caller has for the old value:

```c
struct vec { int *items; size_t len, cap; };
static void vec_grow(struct vec *v) { v->items = realloc(v->items, 64); }

int f(struct vec *v) {
  int *first = &v->items[0];   /* borrows the old buffer            */
  int *p = v->items;           /* a copy of the old pointer         */
  vec_grow(v);                 /* the old buffer is gone            */
  return *first + p[0];        /* silent today: two dangling reads  */
}
```

`vec_grow`'s summary is `v->items: written; stores{v->items = fresh(free)}`.
The `moved` that `realloc` performed on `v->items` was recorded and then
erased by the reinitialisation, and the caller never hears of it. The same
happens with `free(b->data); b->data = NULL;` for a caller that copied
`b->data` first, and with `free(b->data); if (c) b->data = NULL;` for the
caller's `b->data` itself (the store re-initialises the place at the call
site even though on the `!c` path it still holds the freed pointer). A
user who finds this on their own `grow()` helper has reason to distrust
every other report.

## Soundness

### Bugs caught

**Replaced values (item 1).** For a callee that consumes the value at a
caller-visible path and later stores to the path:

```c
static void grow(struct vec *v) { v->items = realloc(v->items, 64); }
static void reset(struct box *b) { free(b->data); b->data = NULL; }
static void maybe(struct box *b) { free(b->data); if (cond()) b->data = NULL; }

int *p = v->items; grow(v); p[0];          // use-after-move of 'p'
char *q = b->data; reset(b); q[0];         // use-after-free of 'q'
maybe(b); b->data[0];                      // use-after-free of 'b->data'
reset(b); if (b->data) ...;                // clean: the place was replaced
```

**Null dereferences (item 2).** A dereference (`*p`, `p->f`, `p[i]`, a
write through `p`) of a place that *may be null*, or passing such a place to
a callee that dereferences its parameter unconditionally:

```c
char *p = malloc(8); p[0] = 1;             // 'malloc' may return NULL
char *c = strchr(s, ':'); *c = 0;          // 'strchr' may return NULL
if (!n) log("no node"); return n->v;       // tested, then dereferenced
struct node *n = NULL; return n->v;        // is null
char *p = malloc(n); memset(p, 0, n);      // passed to a dereferencing callee
int *x; if (!get(&x)) return; ... x[0]     // clean: the outcome says x is non-null
```

A place *may be null* when it was assigned a null constant, received the
result (or a callee's store) of a function whose summary lists `null` among
its returns, was copied from such a place, or is on the far side of a null
test that did not end the path (`if (!n) log();`). Parameters, loads from
fields, and results of unknown callees are trusted non-null unless tested
or annotated `WEAVEC_NULLABLE`; the guarantee for those is the caller's or
the annotation's.

**Uninitialised pointers (item 3).**

```c
char *p; if (c) p = malloc(8); free(p);    // 'p' may be uninitialised
struct s x; free(x.data);                  // a pointer field of a local
```

Reads of a pointer variable declared without an initialiser, or of a pointer
field of a local record declared without one, before an assignment reaches
them on every path. Taking the address (`&p`), passing `&p` or `&x` to any
callee, or a callee's store through the address counts as initialisation.

**Invalid releases (item 4).** Releasing (`free`, any `f` entry, any
inferred or annotated consuming parameter) or moving into an owning
parameter (`realloc`, `WEAVEC_OWNED`) a value that is known not to be the
start of a heap allocation:

```c
char buf[8]; free(buf);                    // storage of a local
static char g[8]; free(g);                 // storage of a global
struct s x; free(&x); free(&x.d);          // address of a local or its field
char *s = "abc"; free(s);                  // a string literal
char *p = malloc(8); free(p + 1);          // into the middle of an allocation
char *q = strchr(p, 'x'); free(q);         // an interior copy of an owned place
```

### Bugs deliberately not caught

- **Null through unknown code.** A pointer returned by an unannotated
  external function, or loaded from a field nobody here assigned, is
  trusted non-null. The RFC 0003 boundary warning and `WEAVEC_NULLABLE`
  are the remedies; treating every unknown pointer as nullable would report
  on nearly every line of real code and was rejected (RFC 0002, RFC 0006).
- **Null-tolerant callees are not required non-null.** A callee that tests
  its parameter before every dereference (`if (!b) return;`) has no
  `requires` fact; one that dereferences it on *some* path before any test
  does. The latter is a may-fact like every other summary effect, so `void
  f(struct s *s, int c) { if (c) use(s->x); }` called with a possibly-null
  `s` and `c == 0` is reported. This is the RFC 0001 conservatism at joins,
  accepted as elsewhere.
- **Interior releases with an unknown base.** `free(&p->f)` and
  `free(strchr(s, c))` where `s` is a parameter are not reported: the model
  cannot tell `&p->first_field` (valid: same address) from
  `&p->second_field`, nor an interior pointer into a parameter's object
  from the object's start (`sds` frees `s - header`). Only a base that this
  function *knows* to be an allocation (a resource record) makes an offset
  from it invalid.
- **Bounds.** Whether `p + 1` is in bounds remains out of scope (RFC 0001).
- **Uninitialised arrays and non-pointer scalars.** `char *a[4]; use(a[1])`
  after `a[0] = ...` is not reported (element writes reinitialise the
  summary place); integer uninitialised reads are Clang's `-Wuninitialized`.
- **Address-taken locals.** `char *p; char **pp = &p; *pp = malloc(8);
  use(p)` is not tracked: a local whose address is taken anywhere in the
  body is never marked uninitialised.
- **`fclose(NULL)` and friends** are reported through the `requires` fact
  on the table entry, but `free(NULL)` and `realloc(NULL, n)` are not:
  ISO C defines them.

### Accepted false positives

- **Tested-then-merged.** `if (p == NULL) warn(); p->x` is reported even
  when `warn` never returns in practice (an unannotated `noreturn` or a
  `longjmp`). Declare the function `noreturn` or restructure. This is the
  same heuristic Clang's static analyzer and `-Wnull-dereference` use.
  *Superseded by [RFC 0009](0009-value-conditional-behaviour.md), which
  infers `never-returns` from `warn`'s body, in the same unit or across the
  program; the false positive remains only for a callee the checker cannot
  see (a library function without the attribute).*
- **Unchecked `malloc`.** `p = malloc(n); memset(p, 0, n)` is reported.
  Code that considers allocation failure unrecoverable should wrap the
  allocator in an `xmalloc` that aborts; its inferred summary then returns
  `fresh` without `null` and nothing downstream is reported. The corpus will
  show how much of real code this is; the id is an error, and
  `-Wno-error=weavec-null-dereference` exists.
- **May-consume of a may-borrow.** `const char *name = argc > 1 ?
  strdup(argv[1]) : "default"; ... if (argc > 1) free(name);` is an
  `invalid-release`: the checker cannot correlate the two tests (RFC 0006,
  *Deliberately not caught*: integer facts). `WEAVEC_UNSAFE` around the
  release, or `strdup` on both arms.
- **Replaced values are may-consumed.** After `grow(v)` every copy of the
  old `v->items` is dead even if `realloc` failed (RFC 0006 already accepts
  this for `p = realloc(p, n)`).

### Assumptions

Those of RFCs 0001–0007. In addition: the nullness of a place is a property
of its *value*, so it copies with the pointer and is dropped when the place
is reassigned; the `null` alternative of a shipped table entry describes
the ISO C / POSIX contract (a platform that never returns `NULL` from
`malloc` is not modelled); and a `WEAVEC_NONNULL` annotation is trusted
like every other annotation (RFC 0001: annotations are authoritative).

## Detailed design

### Replaced values (amending RFC 0003)

RFC 0003 records consumption of parameter roots (and of paths under a
reassigned parameter) *as it happens* and reads consumption of every other
path from the *exit state*. This RFC keeps both sources and combines them
per path:

- **Event consumption**: the value the caller's memory held at path `P` on
  entry was released or moved on some path through the callee *that
  returns*. Recorded as it happens for every caller-visible path in the
  flow-sensitive `state.consumed` (which replaces the RFC 0003
  `eventEffects` side table), so that outcome classes see it (RFC 0006) and
  so that the exit state's `consumed` is the union over the returning
  paths. A block that ends in a `noreturn` call never hands control back
  (RFC 0003, *What a summary describes*): Lua's `os_exit` does
  `lua_close(L); exit(status);` and must not tell every caller of a
  `lua_CFunction` that `L->l_G` is gone. Likewise a null edge retracts the
  consumption below the pointer (RFC 0007).
- **Exit consumption**: at some return the place `P` still holds the
  consumed value (the exit state's move records, as RFC 0003).

The effect written to the summary is the *event* consumption. A new flag
**`replaced`** is set when the path has event consumption but no exit
consumption: on every path that released the value, the place was
reinitialised before returning. `replaced` is a must-fact and joins by
conjunction (a summary joined from several bodies is `replaced` only if
each is).

| Callee                                     | Effect on `param 0 *.data`             |
| ------------------------------------------ | -------------------------------------- |
| `free(b->data);`                           | `freed(free)`                          |
| `free(b->data); b->data = NULL;`           | `written,freed(free),replaced`         |
| `free(b->data); if (c) b->data = NULL;`    | `written,freed(free)`                  |
| `v->items = realloc(v->items, n);`         | `written,moved(free),replaced`         |
| `free(b->data); memcpy(b, &t, sizeof t);`  | `freed(free),replaced` (+ `*b: written`) |

Parameter roots never get `replaced`: `free(p); p = NULL;` reassigns the
callee's private copy, not the caller's argument. Paths under a reassigned
parameter never get it either (they do not describe the argument, RFC
0003).

**Applying a summary at a call** (RFC 0003 step 1, RFC 0007 deepest-first)
changes as follows. For a consumed path `P` resolving to caller place `c`:

- with `replaced`: the consume targets are the *aliases* of `c` and of its
  mirrors (every other name for the old value), not `c` itself nor its
  mirrors. The place `c` is then reinitialised (its facts and the facts
  below it are dropped) so a following store to `P` lands on a clean cell;
  if the summary has no store to `P`, `c` holds an unknown value.
- without `replaced`: as today, `c`, its mirrors and its aliases are
  marked. If the summary also stores to `P`, the store is applied and then
  the move record on `c` is **restored**: the callee may have left the
  freed value there (`free(b->data); if (c) b->data = NULL;`).

Nothing else about summary application changes. `PendingOutcome` records
the consume targets as before, so an outcome test retracts the aliases'
records in the `replaced` case exactly as it retracts the place's today.

**Recording** (`recordConsume`, `consumptionAt`, `finalizeSummary`): every
consume of a caller-visible place records into `state.consumed`;
`consumptionAt` is the union of `state.consumed` and the move records with
a summary path; `finalizeSummary` reads the unconditional effects from the
exit state's `consumed`, adds the exit state's moves (as today) and sets
`replaced` on each event-consumed path that is neither a parameter root,
nor under a reassigned parameter, nor moved in the exit state.
`forgetBelowNull` continues to erase `state.consumed` below a pointer on
its null edge.

**Element consumes (amending RFC 0006).** RFC 0006 applies a consume from a
summary with a *whole* witness, which matches every element: a callee that
does `free(history[len])` on one path makes the second call in a caller's
loop a `double-free` of `*history`. RFC 0007's reading of a `written` effect
as a reinitialisation used to mask this; *Replaced values* (which keeps the
record unless the consume is `replaced`) exposes it. So the effect carries a
second must-fact, **`element`**: every consume of the path went through an
element access (its witness was not *whole*). A caller applies an `element`
consume with an *unknown* witness, which matches only a *whole* record:
`callee(a); use(*a)` and `free(*a); callee(a)` still report, `callee(a);
callee(a)` does not, and neither does `callee(a); use(a[j])` (RFC 0006,
*Different-witness elements*). A whole consume on either side of a join
clears the flag.

**Struct-by-value results.** A function returning a record hands the
caller its fields, but RFC 0003's `returns` describe a pointer result only,
so `struct buf make(void) { struct buf b = { malloc(8), 8 }; return b; }`
leaves the caller's `b.data` unknown and its leak unreported. A third
summary root, **`result`**, names the returned object: `stores{result.data
= fresh(free)}`. It is recorded at each `return` of a record-typed place
(from the returned place's storage, one store per pointer field path with
a known source, RFC 0003 `sourceOf`), and applied by `copyRecord` when the
value assigned to a record place is a call: each `result` store becomes a
pointer assignment to the corresponding field of the destination. No
effects or returns are ever rooted at `result`; `remapGlobals` keeps it;
the text form is `store result .data fresh(free)`.

### Nullness

**Core.** A new `NullTracker` (`weavec/Core/Nullness.h`) in
`AnalysisState::nulls` maps places to a `NullRecord{ Nullness state;
SourceLocation location; NullReason reason; std::string detail; }` where

```
Nullness ::= Null | MaybeNull | NonNull
NullReason ::= AssignedNull | CalleeResult | CalleeStore | Tested | Declared
```

A place with no record has *unknown* nullness. The join per place:

| ⊔          | absent    | NonNull   | MaybeNull | Null      |
| ---------- | --------- | --------- | --------- | --------- |
| absent     | absent    | absent    | MaybeNull | MaybeNull |
| NonNull    | absent    | NonNull   | MaybeNull | MaybeNull |
| MaybeNull  | MaybeNull | MaybeNull | MaybeNull | MaybeNull |
| Null       | MaybeNull | MaybeNull | MaybeNull | Null      |

with `Null ⊑ MaybeNull`, `NonNull ⊑ absent ⊑ MaybeNull`: finite, monotone.
When the result is `MaybeNull` the record kept is the one that said `Null`
or `MaybeNull` (this side first). `NonNull` is a positive fact used for
outcome classes (below) and to silence declared-nullable places; it never
reports. The RFC 0007 must-null set in `ResourceTracker` stays for the
leak logic; a place marked null there is also `Null` here.

**Sources of facts** (`weavec::Analysis`):

- `p = NULL`, `p = 0`, an initialiser list zeroing a field: `Null`
  (`AssignedNull`).
- `p = f(...)` where `f`'s summary `returns` include `null` (the origin has
  a `Null` arm): `MaybeNull` (`CalleeResult`, detail `'f'`). The `null`
  alternative is what every shipped allocator (`F`), every searching
  function (`0`–`9` interior results) and the `A`–`J` functions that can
  fail (`fgets`, `gets`, `realpath`, `getcwd`, ...) now carry. An inferred
  summary lists `null` when some `return` is a null constant *or a copy of
  a place that is `Null`/`MaybeNull` at the return* (`return p` after `p =
  malloc(n)` without a test).
- A callee's store whose sources include `null` (`*out = malloc(n)`): the
  destination is `MaybeNull` (`CalleeStore`).
- `q = p` copies `p`'s record to `q` (the fact is the value's).
- A null test (`p == NULL`, `!p`, `p`, through `&&`/`||`/`!`/
  `__builtin_expect` as RFC 0006): on the null edge `Null` (`Tested`), on
  the non-null edge `NonNull` (and the exact copies of `p` likewise). After
  the merge of the two edges the place is `MaybeNull` with the `Tested`
  record.
- Tests of a call result select outcome classes (RFC 0006); a class the
  callee marks in `nonNullOn` (below) sets the corresponding caller places
  `NonNull` on that edge, the way `nullOn` sets them null.
- A parameter, variable or field declared `WEAVEC_NULLABLE`: `MaybeNull`
  (`Declared`) when read with no record; `WEAVEC_NONNULL`: `NonNull`.
- Any reassignment drops the record (`reinit`), as does `written` below a
  pointer (`forgetBelow`).

Facts are recorded only for whole-place accesses (`ref.element.isWhole()`);
`a[i] = NULL` says nothing about `a[*]` (RFC 0006, *Element witnesses*).

**The check.** In `doRead`, for every pointer place dereferenced on the way
to the accessed place (`ref.derefs`): a `Null` record reports `dereference
of '<p>', which is null`; `MaybeNull` (or a declared-nullable place with no
record) reports `dereference of '<p>', which may be null`; with the
record's note. The record is then cleared on the path so one bad pointer
reports once (`p->a; p->b;`), in both dataflow phases so the fixpoint is
unaffected. Dereferences inside an unsafe region are not reported (RFC
0004). Use-after-free on the same dereference wins (it is checked first);
a raw pointer is reported as raw.

**Requirements.** `FunctionSummary::requiresNonNull` is the set of
parameter indices the callee dereferences while their nullness is unknown
on some path: recorded when a dereferenced place, or an exact alias of it,
is a parameter root with no record; and when such a place is passed to a
callee that itself requires the corresponding parameter (so `size_t len(
const char *s) { return strlen(s); }` requires `s`). `WEAVEC_NONNULL` on a
parameter adds it; `WEAVEC_NULLABLE` removes it (the body is checked
instead). In the shipped table every `r`/`w` parameter and every `f`/`m`
parameter of a family other than `free` requires non-null, except a short
list of functions documented to accept `NULL` (`strtok`'s first argument,
`time`, `fflush`, `setlocale`'s second argument, `mmap`'s first,
`getcwd`'s first, `realpath`'s second, `select`'s pointer arguments,
`pthread_create`'s attributes, `sigaction`'s pointers, `nanosleep`'s
second, `waitpid`'s status, `accept`'s and `recvfrom`'s address
arguments, and the like). At a call, an argument whose place is `Null` /
`MaybeNull` passed to a required parameter reports `'<p>', which may be
null, is passed to '<f>', which dereferences it` (or `which is null`).
Requirements join by union (a may-fact).

**Per-outcome non-null facts.** `FunctionSummary::nonNullOn` mirrors RFC
0007's `nullOn`: per outcome class, the caller-visible paths that are
`NonNull` at every return of that class, plus the tested place of `return
*out != NULL` on the class that means non-null. `PendingOutcome::
nonNullOn` carries them to the caller's edge; `nonNullInAll()` is applied
like `nullInAll()`. This is what makes `if (!make(&p)) return -1; p[0]`
clean when `make` stores `{fresh, null}` and returns `*out != NULL`.
Joins by intersection (a must-fact).

**Performance.** One map lookup per dereference and per assignment; the
tracker joins like `ResourceTracker`. No new places are interned. The
corpus timings in `scripts/corpus/README.md` are the measurement; see
*Performance* under *Implementation notes* for what they showed.

### Uninitialised pointers

`MoveReason` gains `Uninitialized`. `handleDecl` marks a pointer variable
declared without an initialiser (not `static`, not `extern`, and not
address-taken anywhere in the body, see *Deliberately not caught*)
`Uninitialized` at the declaration; for a record-typed local without an
initialiser it interns every pointer-typed field path (through nested
records, not through arrays or pointers, bounded by `MaxPlaceDepth`) and
marks each. The existing machinery does the rest: a read of the place, a
dereference through it, a consume of it, or a copy from it finds the record
(`findMoved`) and reports; an assignment (`reinit`), a callee's store, a
`written` effect on the place's object, a mutable borrow of the place for a
call (`init(&p)`, `memset(&x, 0, n)`, a call into unknown code) clears it.
`reportUseOfMoved` reports the new reason as `use-of-uninitialized`;
`doConsume` does not treat it as a double free. The record never reaches a
summary (`consumptionAt`, `finalizeSummary` and `recordConsume` skip it):
only locals can be uninitialised.

### Invalid releases

In `doConsume`, before the consume is recorded and unless the place is
annotated `WEAVEC_BORROWED`/`WEAVEC_MUT` (RFC 0003's `annotation-mismatch`
already covers it) or the region is unsafe, the place `c` being released
(`Freed`) or moved into an owning parameter (`Moved`) is checked:

1. **Borrowed storage.** `c` holds a loan whose place is the storage of a
   variable: its root is a variable and the path from the root crosses no
   dereference (`buf[*]`, `x`, `x.d`, `g`, but not `*p`, `p->f`, `p[*]`).
   Message: `'<c>' is released but points to '<x>', which is not a heap
   object`, note `'<x>' is declared here`.
2. **String literals.** A string literal is a borrow of a per-function
   synthetic place with static lifetime (so returning or storing a literal
   is never `lifetime-too-short`). A loan on it reports `'<c>' is released
   but points to a string literal`.
3. **Interior pointers.** `ResourceRecord` gains `interior`: set when a
   record is copied to a destination through an interior copy (`q = p +
   1`, `q = strchr(p, c)`, RFC 0006 *Alias exactness*), cleared on a fresh
   allocation. A consume of a place whose record is `interior`, or whose
   argument expression is pointer arithmetic on a place with a
   non-interior record, reports `'<c>' is released but does not point to
   the start of its allocation`, note `allocated here`.

The consume proceeds after the report (the place is dead either way), as
for `mismatched-release`. `checkContainerFree` no longer assumes the freed
place's declaration has pointer type (the crash on `free(static_array)`).

### `--dump-analysis`

The exit state gains `nulls{p@3:14 maybe-null, q nonnull}`; move records
print `uninitialized`; resource records print `interior`; summaries print
`replaced` and `element` among the flags, `requires{s}` after `returns{...}`, `notnull{*out}`
after a class's `null{...}`, and `result.data` as a store destination.

### Summary text format

`SummaryFormatVersion` and `SidecarFormatVersion` become 4. New: the flags
`replaced` and `element` in `effect` lines; `requires N` lines (parameter index);
`notnull <class> <path>` lines; the path root `result`. Everything else is
unchanged; a version-3 sidecar is rejected by its header and the object
re-analysed.

## Annotation surface

Two new macros in `resources/include/weavec.h` (header version 0.3 → 0.4),
spelled `weavec.nullable` and `weavec.nonnull` in `Annotations.h`:

| Macro             | Attaches to                        | Meaning                                                                                         |
| ----------------- | ---------------------------------- | ----------------------------------------------------------------------------------------------- |
| `WEAVEC_NULLABLE` | parameter, return type, variable, field | The pointer may be null. A parameter is checked in the body (dereferences without a test report) and imposes no requirement on callers; a return adds `null` to the summary's returns; a variable or field is `MaybeNull` whenever loaded without a fact. |
| `WEAVEC_NONNULL`  | parameter, return type, variable, field | The pointer is never null. A parameter is a `requires` fact for callers; a return has no `null` alternative; a variable or field is never reported. |

Both expand to nothing under compilers without `annotate`. Both are
authoritative (RFC 0001); neither changes ownership. `WEAVEC_NULLABLE` and
`WEAVEC_NONNULL` on the same declaration is `invalid-annotation`.

## Diagnostics

New ids, all errors by default:

| Id                    | Message                                                                                                 | Notes                                                                                  |
| --------------------- | ------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `null-dereference`    | `dereference of '<p>', which may be null` / `dereference of '<p>', which is null`                        | `'<p>' may be null: it is the result of '<f>' here` / `'<p>' may be null: it is set by '<f>' here` / `'<p>' is assigned NULL here` / `'<p>' may be null: it is compared with NULL here` / `'<p>' is declared WEAVEC_NULLABLE here` / `'<p>' may be null: the result of '<f>' is declared WEAVEC_NULLABLE here` |
| `null-dereference`    | `'<p>', which may be null, is passed to '<f>', which dereferences it` (or `which is null`; `a null pointer is passed to '<f>', which dereferences it` for a null constant argument) | the same note for `<p>`, plus `'<f>' is declared here` (omitted for an implicitly declared builtin) |
| `use-of-uninitialized`| `use of '<p>' before it was initialized`                                                                | `'<p>' is declared here` (the variable, or the record variable for a field)            |
| `invalid-release`     | `'<p>' is released but points to '<x>', which is not a heap object`                                     | `'<x>' is declared here`                                                               |
| `invalid-release`     | `'<p>' is released but points to a string literal`                                                      | —                                                                                      |
| `invalid-release`     | `'<p>' is released but does not point to the start of its allocation`                                   | `allocated here`                                                                       |

Triggers are the snippets under *Soundness*. Each gets a unit test and a lit
test under `test/Analysis/rfc0008-*.c`.

Changed: `use-after-free` / `use-after-move` are now also reported for a
caller's copy of a value a callee released and replaced (*Replaced values*);
their wording is unchanged and the note points at the call.

## Drawbacks

- **A fourth state component.** Nullness is joined per block like the
  others; the cost is one more map per state. It is the smallest component
  by construction (only places with a fact have an entry).
- **Two new sources of reports on unchecked code.** Unchecked allocations
  and searches are common in quick C; every one is now an error at its
  first dereference. This is deliberate (it is the bug class), but it will
  dominate the corpus delta until triaged.
- **Summary vocabulary growth.** `replaced`, `requires`, `notnull` and the
  `result` root are four more things every reader of the format handles.
  Each is orthogonal; a reader that ignores `requires`/`notnull` is exactly
  as sound as before, one that ignores `replaced` is *more* conservative
  (it marks the place too), and `result` stores are unknown to an old
  reader only in the direction of losing a leak report.
- **Event recording widens `state.consumed`.** Every caller-visible consume
  is now in the flow-sensitive map; on Lua-sized summaries this is a few
  dozen entries per state.

## Alternatives

- **Do nothing on item 1.** Rejected: it is a silent soundness hole in the
  headline guarantee, and RFC 0007 already had to special-case its shadow
  (the "consumption below a store is the callee's business" comment).
- **Record all consumption as it happens, no `replaced`.** RFC 0003
  rejected this because `buf_destroy(&b); if (b.data)` becomes a
  use-after-free. Applying the store after the consume fixes the
  unconditional re-null but not the conditional one, and the caller's
  place must stay marked in that case. Hence the flag.
- **Two flags (`freed-value` vs `freed`).** Same information as
  `freed,replaced`, but every plain `free(b->data)` would spell two flags
  and every existing summary text would change. `replaced` is additive.
- **Nullness as a fourth ownership kind.** Rejected by RFC 0001 and again
  here: nullness is a property of the value, orthogonal to who owns it
  (`Option<Box<T>>` vs `Box<T>`, not a third thing).
- **Treat every unknown pointer as nullable (Clang analyzer's
  `NullDereference` with `nullability` extensions).** Sound, unusable: it
  reports on every parameter dereference in every function. The
  tested-or-declared rule is what Clang's own `-Wnull-dereference` and the
  analyzer's default checker do, and what the corpus can absorb.
- **A separate `--nullability` mode.** The RFC 0005 `-W` machinery already
  lets a project disable or demote each id; a mode flag would be a second
  way to say the same thing.
- **Invalid release by ownership kind alone** (`Shared`/`Mutable` kind ⇒
  invalid). Kinds join to `Raw` across arms (RFC 0001) and lose the
  information; loans, literals and interior records are exact.

## Prior art

- **Rust**: `Option<T>` makes nullability a type; `Box::from_raw` on a
  non-`Box` pointer is UB the type system prevents; reading an
  uninitialised local is a compile error. We recover the first as a
  flow-sensitive fact, the second as `invalid-release`, the third as
  `use-of-uninitialized`.
- **Clang**: `-Wnull-dereference`, `-Wuninitialized`/
  `-Wsometimes-uninitialized`, the `_Nullable`/`_Nonnull` type qualifiers
  and `-Wnullable-to-nonnull-conversion`; the static analyzer's
  `core.NullDereference`, `core.uninitialized.*`, `unix.Malloc`'s
  "Argument to free() is the address of a stack/global variable / offset
  of N bytes from the start of memory allocated by malloc". Our table
  `requires` set is the analyzer's `nonnull` argument knowledge; our
  annotations are `_Nonnull`/`_Nullable` without changing the type.
- **Cyclone** made nullable and non-null pointers different types (`*`
  vs `@`), with inserted checks; Checked C's `_Nt_array_ptr` likewise.
  Both show that a per-value nullness fact is enough for the dereference
  check and that inference removes most annotations.
- **Infer** (`NULL_DEREFERENCE`, `Pulse`) reports exactly the
  "tested-then-dereferenced" and "unchecked allocation" shapes and found
  them to be the highest-signal null reports on real code.
- **RFC 0007** built the must-null set and per-outcome null stores; this RFC
  is the may-null side it left as future work.

## Unresolved questions

**Resolved at acceptance**:

| Question                                   | Decision                                                                                            |
| ------------------------------------------ | --------------------------------------------------------------------------------------------------- |
| Parameters: nullable by default?           | No: trusted unless tested or `WEAVEC_NULLABLE` (see *Alternatives*).                                |
| `malloc` may return null in the table?     | Yes: every `F`, every interior result, and the fallible `A`–`J` entries carry `null`.               |
| Severity of the three ids                  | Error, like every other guarantee-class diagnostic; `-Wno-error=` per id.                           |
| `replaced` as a flag vs new flags          | A flag; additive, most summaries unchanged.                                                         |
| Report the release of a may-borrow?        | Yes, as for every other may-fact.                                                                   |

**Deferred to corpus testing**:

- How much of the corpus delta is unchecked `malloc`, and whether an
  `xmalloc`-style summary heuristic (an allocator wrapper that aborts on
  null) needs anything beyond the inferred `returns{fresh}` it already gets.
- Whether `requires` on `w`/`r` table entries should be narrowed for
  functions with a size argument that may be zero (`memcpy(NULL, NULL,
  0)` is UB in ISO C but tolerated by every libc).
- Whether per-element nullness (`a[i] == NULL` witnesses) is worth the
  state.

## Implementation notes

Where the code refines the design above:

- **`replaced` and may-consumes.** `replaced` is decided over the paths that
  consumed: `v->items = realloc(v->items, n); if (!bigger) return 0;` is
  `written,moved(free),replaced` even though the failure path consumed
  nothing, because on that path the place still holds a valid (old) value.
  The per-outcome classes carry the same flag
  (`outcome positive{v->items: moved(free) replaced}`).
- **`overwritten`.** `AnalysisState::overwritten` records the
  caller-visible paths whose entry value has been replaced on every path
  reaching here (a whole-element write, not `p = p + 1`); a later consume of
  such a path is the function's own value, not the caller's, and is not
  recorded into the summary. It joins by intersection and a proper prefix
  covers the paths below it only when no dereference intervenes
  (overwriting a struct does not overwrite what its pointers point to).
- **Nullness of copies.** The non-null edge of a test clears the fact on
  the tested place and on its exact aliases (same value), as *Sources of
  facts* says; the `null` edge marks only the tested place.
- **A dereference establishes non-null.** When a place with no fact is
  dereferenced, or passed to a callee that requires it, the requirement is
  recorded (*Requirements*) and the place becomes `NonNull`
  (`Dereferenced`) on the continuing path: it could not have continued
  otherwise. This is what Clang's analyzer and Infer assume, and with the
  next rule it keeps cJSON's `buffer_at_offset(b)` at the top of a function
  from turning a `cannot_access_at_index(b, 0)` retest further down into a
  maybe-null `b`.
- **Redundant tests.** A test never weakens a `NonNull` fact: on the null
  edge of `p != NULL` for a `p` already known non-null the edge is
  infeasible and the fact is kept (the RFC 0007 must-null marking and the
  RFC 0006 retraction below the pointer still happen there; on a dead edge
  they only suppress reports). Without this, cJSON's
  `can_access_at_index(b, i)` macro, which retests `b != NULL` on every
  use, made `b` maybe-null after every loop. `(T *)0` counts as a null
  constant in tests and assignments alike, although ISO C reserves the term
  for `0` and `(void *)0` (zlib's `buf != (charf *)0`).
- **Unchecked callees may write what they reach.** A call into unchecked
  code (RFC 0003 boundary, including a callee with nullness annotations
  only) drops the nullness facts, and the RFC 0007 must-null flag, at the
  storage of every argument passed by address (`fill(&lc)` after
  `linenoiseCompletions lc = { 0, NULL }`) and below every pointer argument
  (`f(p)` may write `*p`, not `p`). Ownership facts are untouched: the
  boundary warning is what covers an unknown callee's frees and moves.
- **A store that did not happen is not a null fact.** RFC 0007's `nullOn`
  holds, besides the places null at every return of a class, every `fresh`
  store destination that holds nothing at any of them (*Per-outcome null
  stores*: "or holds nothing this function stored there"), so that `if
  (init(s) != 0) return -1;` is not a leak. For `int grow(struct buf *b,
  unsigned n) { char *p = realloc(b->data, n); if (!p) return -1; b->data
  = p; return 0; }` that puts `b->data` into `null{...}` on `negative`,
  where the caller's memory holds what it held before the call. Reading
  every `nullOn` place as `Null` (as *Nullness* says of the must-null set)
  made `if (grow(b, n) == -1 && n > b->len) n = b->len; b->data[n]` a
  `null-dereference` (linenoise's `linenoiseEditGrow`, cJSON's `ensure`,
  zlib's `gz_init`). At a call, a `nullOn` path that is the destination of
  a `fresh` store and of no `null` store can be there only through the
  relaxation (`PendingOutcome::unheldOnly`); selecting its class retracts
  the caller's record (`ResourceTracker::forget`, what RFC 0007 says the
  caller may act on) and leaves nullness alone. Every other `nullOn` place
  is marked must-null and `Null` as before (`*out = malloc(n); if (!*out)
  return -1;` stores `{fresh, null}`). The approximation is per path, not
  per class: a callee that stores both null and fresh into a path and has
  a third return where neither happened still says `Null` there.
- **Dereferenced call results.** `f()->x` with no place for the result is
  checked by `checkResultDereference` from the call's summary (`returns`
  with `null`) or its `WEAVEC_NULLABLE` declaration; the message names the
  callee's result.
- **Nullness annotations on unchecked callees.** A declaration with
  `WEAVEC_NULLABLE`/`WEAVEC_NONNULL` but no ownership annotations is still
  `annotation-required` (RFC 0003 is unchanged), but its nullness
  annotations are honoured: the result is `MaybeNull` (`Declared`) and a
  `WEAVEC_NONNULL` parameter is a requirement. Nullness annotations alone
  do not make a declaration "annotated" for `annotation-required`.
- **`_FORTIFY_SOURCE`.** `__builtin___memcpy_chk` and friends resolve to
  the table entry of the function they wrap (`memcpy`), so `requires`
  applies to them and their declaration note is omitted (they are implicit).
- **Interior releases need a record.** Rule 3 (*Invalid releases*) fires
  for a place whose `ResourceRecord` is `interior`, or for arithmetic on a
  place that has a non-interior record. `strchr(s, c)` on a parameter with
  no record is not reported: nothing is known about `s`. `p + 0` is `p`.
- **Uninitialised records and the dump.** The record is dropped with the
  local at scope exit, so it does not appear in `--dump-analysis`'s exit
  state; the diagnostic is the evidence. Locals of a function whose summary
  is dumped never carry `uninitialized` into `summary:`.
- **Diagnostic columns.** A `use-of-uninitialized` for a consume
  (`free(p)`) is reported at the call, as `use-after-free` is; a copy
  (`q = p`) at the read.
- **Clang's own `-Wfree-nonheap-object`.** Clang reports `free(buf)` on a
  named array too; WeaveC's `invalid-release` covers the cases Clang
  cannot see (through a copy, a call, a struct field) and reports the
  direct case once as well.

### Performance

The nullness tracker itself is within noise everywhere. Lua (34 units as
one program) nevertheless went from about 12 minutes (RFC 0007) to 52 on
the same machine and build (29 after the two fixes below), and the cause is
*Replaced values*, not nullness: `luaV_execute` calls a `luaM_realloc`-family helper at some 130
sites, RFC 0007 read each callee's `written` effect on `L->stack` and its
relatives as a reinitialisation and dropped the record, and this RFC keeps
it, so every one of those calls now performs a consume of the path, of its
mirrors through `L->twups ~ L`, and of their aliases, and the function's
states carry a hundred move records where they carried none. Two costs
around that were removed in the same change:

- The whole-program fixpoint (RFC 0005) rebuilt its database by
  renumbering every member's summaries whenever one member's exports
  changed; members are now kept numbered by the database's own table
  (`ProgramDatabase::renumbered`, `GlobalNames::extendTo`) and a rebuild
  copies them.
- `BorrowState` held one loan per *site* of a borrow: a callee's `L->ci =
  borrow(L->base_ci)` store applied at 128 call sites, times nine
  holder/place pairs through the mirrors, was 1,173 loans per state in
  `luaV_execute`, each copied at every block visit and merged at every
  join. It now holds one loan per borrow (place, holder, kind, lifetime) at
  the earliest site, which is the loan a conflict would have cited anyway
  (`checkMove` returns the first in order). The join fell from 39% of the
  run to under 3%.

What remains is the consume fan-out: `doConsume` and `mirrors` are
two-thirds of the run, the same place enumerated once per mirror and once
more below each target. A consume-target cache per call, or the summary
carrying "may free below" as a mark on the object the way RFC 0007's
*Performance* proposes for scalar writes, would remove it; both are left
to a later change with the RFC 0007 item.

## Future work

- **Dereferenced fresh results leak.** `make()->v` where `make` returns
  `fresh` drops the allocation; the discarded-call rule (RFC 0007) covers
  only statement-level discards. A `leak` at the dereference is the
  natural extension.
- **Per-outcome stores** (RFC 0007 *Future work*), which would let the
  `replaced` rule keep the place's record only on the classes where the
  store did not happen.
- **Nullability of fields inferred from constructors** (`s->opt` is
  nullable because `make_s` stores `NULL` there): a `nullable` store
  fact in the summary rather than an annotation.
- **Arrays of pointers**: element-witnessed uninitialised and null facts.
- **`WEAVEC_NULLABLE` on function-pointer types** for callbacks that may be
  absent.
