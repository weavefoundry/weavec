# RFC 0012: Spatial safety II: strings, sized fields, offset relations and assumptions

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-05
- **Accepted**: 2026-09-05
- **Tracking issue**: TBD
- **Supersedes / superseded by**: extends RFC 0011's *Spatial records* with
  string facts, its *Relations* with constant offsets and lower bounds, its
  *Bounds checks* with the string functions of the library table, and its
  *Annotation surface* with `WEAVEC_SIZED_BY` on fields and
  `WEAVEC_ASSUME`; strikes *null-terminated string extents*, *flexible
  array members* and the `i < n - 1` case of *an interval domain* from
  RFC 0011's *Future work*, and *flexible array members* from its *Bugs
  deliberately not caught*. Bumps the sidecar format (RFC 0005) from 7 to
  8; the summary text format (RFC 0003) stays at 7.

## Summary

RFC 0011 gave WeaveC extents and bounds checks, and left the checker blind
to the two ways C code most often says how big a buffer is: the NUL
terminator of a string, and a count stored next to the pointer in a
struct. On the probes of RFC 0011's own motivation the current build
reports `p[n] = 0` after `malloc(n)` and says nothing about the line that
usually follows it, `strcpy(p, s)` after `malloc(strlen(s))`; it says
nothing about `strcpy(buf, "a long literal")` into `char buf[4]` unless
the platform's `_FORTIFY_SOURCE` macros happen to; nothing about
`strcat`; nothing about `strlen(buf)` after `strncpy(buf, src, sizeof
buf)` left it without a terminator; and nothing about `b->data[b->cap]`
in any project that keeps `{data, cap}` pairs, which is every project in
the corpus. This RFC adds those, on top of the model RFC 0011 built:

1. **String facts.** A spatial record gains the *length* of the string
   the object holds, an `Affine` like the extent (`3` for `"abc"`,
   `strlen(s)` as a *length place* for a string of unknown length,
   `n` after `n = strlen(s)`), and whether the object is known to hold
   *no* terminator (`unterminated`). Sources: literals and array
   initialisers, `strlen`, `strcpy`/`strcat`/`sprintf`/`strdup` and the
   other terminating writers of the table, `strncpy`/`memcpy`/`memset`
   that provably overwrite every byte without a NUL, and a NUL store
   `buf[i] = 0`. `malloc(strlen(s) + 1)` records the extent `strlen(s) +
   1` against the same length place, so `strcpy(p, s)` needs `strlen(s)
   + 1` bytes and has them, and `malloc(strlen(s))` followed by
   `strcpy(p, s)` is `out-of-bounds`. `strcpy`, `stpcpy`, `strcat` and
   `sprintf` are checked against the destination's extent with the need
   computed from the lengths; every function that walks a string to its
   terminator (`strlen`, `strcpy`'s source, `puts`, `strchr`, `strcmp`,
   ...) is checked against `unterminated`. Any write the model does not
   follow drops the facts; the facts are updated on every name of the
   object (aliases and the borrowed storage), never on one.
2. **Sized fields.** `WEAVEC_SIZED_BY(n)` is allowed on a pointer field
   naming a sibling integer field: a load of `b->data` has the extent `n
   * sizeof *b->data` in `b->n`, so `b->data[b->n]` is `out-of-bounds`
   and `b->data[i]` under `i <= b->n` is the off-by-one; a store into the
   field that contradicts the annotation (`b->data = malloc(4); b->n =
   8;`) is `annotation-mismatch`. The same relation is *inferred* for
   unannotated fields, program-wide: a function that stores a value into
   `o->f` and leaves, at its exit, that value's extent equal to `scale *
   o->g` for a sibling `g` *witnesses* `f sized by g`; a function that
   stores a non-null value the exit state cannot relate to any sibling,
   or that writes `o->g` without `o->f` ending up null or related,
   *refutes* it. A field with a witness and no refutation anywhere in
   the program is sized. Witnesses and refutations travel in the
   sidecar (`sized-field`, `unsized-field`), and inside one unit are
   applied by a second, `out-of-bounds`-only pass over the functions
   that load a confirmed field, so an inferred extent is never used
   before every store in the unit has been seen.
3. **Offset relations and lower bounds.** `RelationTracker` learns `i
   REL n + k` (`i < n - 1`, `i <= n - 1`, `j = i + 1`) and `x >= k`
   (from `if (i >= 8)`, `if (i > 7)` and its failing edges). Bounds
   checks substitute the boundary through the offset: `a[i + 1]` under
   `i <= n - 1` on `n` elements is the off-by-one; `if (i >= 8) buf[i]`
   on `char buf[8]` is `out-of-bounds` outright; `a[i - 1]` under `i >=
   1` is fine. The rules stay "definite"; the domain is still the
   relation tracker, not intervals with widening, for the reasons RFC
   0011 gave.
4. **`WEAVEC_ASSUME(expr)`.** A statement that tells the checker `expr`
   holds here, applied exactly as the true edge of `if (expr)` is:
   pointer equalities, scalar facts, relations and their guard
   refinements. `WEAVEC_ASSUME(len <= cap)` is how a user states an
   invariant inference cannot see. It compiles to a call of an empty
   inline function under WeaveC and to `((void)0)` elsewhere; it never
   reaches the optimiser as an assumption.
5. **Flexible array members** are already checked by RFC 0011's access
   model (`s->fam[i]` is an access on `s`'s object at `offsetof(fam) +
   i * sizeof(elem)`, against `malloc(sizeof *s + n)`); the probe
   reports `h->data[n]`. This RFC pins it with tests and strikes it from
   RFC 0011's exclusions.

The recall set gains the string shapes of Juliet's CWE-121/122/126 and a
CWE-170 (improper null termination) directory. The corpus decides RFC
0011's open `i <= n` question together with this RFC's rules; the numbers
go in the corpus README.

## Motivation

### What real code says about sizes

RFC 0011's extent sources are the size argument of an allocation, a
declared array size, a literal and `WEAVEC_SIZED_BY` on a parameter. The
checker therefore knows the extent at the allocation site and along
copies of that pointer within one function, and nowhere else. Two shapes
account for nearly every buffer in the corpus projects and neither is
covered:

```c
/* sds, cJSON, jansson's strbuffer, zlib's z_stream, linenoise: a pointer
   and a count in a struct, filled in one function and used in another. */
struct strbuffer { char *value; size_t length; size_t size; };
int strbuffer_append_bytes(strbuffer_t *strbuff, const char *data, size_t size) {
  if (size >= strbuff->size - strbuff->length) { ... realloc ... }
  memcpy(strbuff->value + strbuff->length, data, size);   /* nothing known */
}

/* Every C program: a string's size is where its NUL is. */
char *dup_upper(const char *s) {
  size_t n = strlen(s);
  char *p = malloc(n);            /* one short */
  strcpy(p, s);                   /* heap overflow: nothing reported */
  return p;
}
```

The first is Juliet's CWE-122 "struct with buffer and size" family, the
second its `strcpy`/`strcat`/`sprintf` family, which together make up the
bulk of its bounds cases. `test/recall/` today holds homegrown cases
because these shapes are not caught, and its README says so. A user who
reads "memory safety" in the README and tries `strcpy` first learns
nothing, and Clang's own `-Wfortify-source` only fires when the SDK's
`_chk` macros are in play and the source length is a literal.

### Strings as extents

The information is there: `strlen(s)` names a quantity; `malloc(strlen(s)
+ 1)` is an extent in that quantity; `strcpy(p, s)` needs exactly that
quantity plus one. RFC 0011 could not record it because `strlen(s)` is
not a place. Giving it one — a *length place*, created when `strlen(s)`
is evaluated and attached to `s`'s spatial record as its length — makes
every existing rule apply: `malloc(strlen(s) + 1)` is `{strlen(s), 1,
1}`, `strcpy(p, s)` needs `{strlen(s), 1, 1}`, rule 2 of RFC 0011
compares them. `n = strlen(s)` relates `n` to the length place by
`Equal`, which `between` already follows one hop. Nothing new is needed
in the comparison; only the sources and the needs.

Termination is the other half. `strncpy(buf, src, sizeof buf)` with a
source at least as long as the buffer leaves no terminator; `strlen(buf)`
then reads until it finds one somewhere past the end (CWE-170, and the
usual root of CWE-126). The checker knows both facts on the path: the
source's length (a literal, or a length place with a relation), and the
destination's extent. "Definitely no NUL in the object" is a fact like
"definitely past the end", and reporting a terminator-seeking read of
such an object is the same kind of report.

### Counts next to pointers

`b->data[b->cap]` is exactly `p[n]` after `malloc(n)`, with the size
stored a field away instead of in a local. RFC 0011's `Affine` can name
`b->cap` as its place already (`tracksScalar` accepts memory behind a
pointer); what is missing is the *source*: where does a load of `b->data`
get its extent? Two answers, in order of trust:

- The user says so. Checked C's `_Array_ptr<T> data : count(cap)`,
  Clang's `__counted_by(cap)` and this RFC's `WEAVEC_SIZED_BY(cap)` on
  the field. Authoritative; checked at stores.
- The program says so. In every corpus project the count is written in
  the same function as the pointer, from the same value the size
  argument came from: `b->data = malloc(cap); b->cap = cap;`. At that
  function's exit the checker holds `b->data`'s extent as `cap` and the
  relation `b->cap == cap`; that is a witness. A function that stores
  into `b->data` something the exit state cannot relate to a sibling, or
  that changes `b->cap` alone (`b->len++` in an append: the *used*
  length, not the capacity), refutes the pair. Confirming a field takes
  every store in the program, which is what the sidecar and the program
  database are for (RFC 0005, RFC 0010's `count-field` lines are the
  same mechanism).

### Relations one step further

RFC 0011's relation tracker records `i < n` and `i <= n` but not `i < n
- 1`, so `if (i < n - 1) a[i + 1]` is neither proven nor reported, and
`for (i = 0; i <= n - 1; i++) a[i + 1]` — the same off-by-one as `i <=
n`, one rewrite away — is missed. It records `x <= k` from `if (x < 8)`
but not `x >= k` from `if (x >= 8)`, so `if (i >= 8) buf[i] = 0` on
`char buf[8]`, which is out of bounds on *every* value the facts allow,
is not reported. Both are the same tracker with one more integer per
entry. RFC 0011 named an interval domain as the natural follow-up; this
RFC takes the two pieces of it that the "definite" rules can use and
leaves widening out, because the rules never reason inductively about a
loop (the condition edge re-learns the relation on every iteration) and
so have nothing to widen.

### Saying what inference cannot see

Every check in this RFC reports on facts. When the fact is an invariant —
`len <= cap` maintained by every writer, `p` and `q` pointing at disjoint
halves of one buffer — the checker has no way to learn it and the user
has no way short of `WEAVEC_UNSAFE` to state it. `WEAVEC_ASSUME(expr)`
is the smallest possible way: the same machinery a condition edge runs,
on demand, trusted like every other annotation.

## Soundness

### Bugs caught

New (strings):

```c
char *p = malloc(strlen(s));
strcpy(p, s);                          /* out-of-bounds: needs strlen(s) + 1 */

size_t n = strlen(s);
char *q = malloc(n);
q[n] = 0;                              /* out-of-bounds (RFC 0011, unchanged) */
memcpy(q, s, n + 1);                   /* out-of-bounds: n + 1 > n */

char buf[4];
strcpy(buf, "hello");                  /* out-of-bounds: 6 > 4 */
strcpy(buf, "abc");
strcat(buf, "d");                      /* out-of-bounds: 3 + 1 + 1 > 4 */
sprintf(buf, "%s!", "abc");            /* out-of-bounds: at least 5 > 4 */

char name[8];
strncpy(name, "0123456789", sizeof name);
size_t len = strlen(name);             /* out-of-bounds: 'name' is not NUL-terminated */
puts(name);                            /* out-of-bounds: same */

char *d = strdup(s);                   /* extent strlen(s) + 1, length strlen(s) */
d[strlen(s) + 1] = 0;                  /* out-of-bounds */
```

New (sized fields):

```c
struct buf { char *WEAVEC_SIZED_BY(cap) data; size_t cap; };
void put(struct buf *b) { b->data[b->cap] = 0; }         /* out-of-bounds */
void fill(struct buf *b) {
  for (size_t i = 0; i <= b->cap; i++) b->data[i] = 0;   /* out-of-bounds (may) */
}
void shrink(struct buf *b) { b->data = malloc(4); b->cap = 8; }  /* annotation-mismatch */

/* Unannotated, inferred from `init` alone when nothing in the program
   refutes it: */
struct vec { int *items; size_t n; };
void init(struct vec *v, size_t n) { v->items = malloc(n * sizeof *v->items); v->n = n; }
int last(struct vec *v) { return v->items[v->n]; }        /* out-of-bounds */
```

New (relations):

```c
int *a = malloc(n * sizeof *a);
if (i <= n - 1) a[i + 1] = 0;          /* out-of-bounds (may): i may be n - 1 */
char buf[8];
if (i >= 8) buf[i] = 0;                /* out-of-bounds: i is at least 8 */
if (i > 7) buf[i] = 0;                 /* out-of-bounds */
size_t j = i + 1;
if (i < n) a[j] = 0;                   /* out-of-bounds (may): j may be n */
```

New (assumptions): none caught by the annotation itself; it removes
reports, see *Accepted false positives*.

Pinned (flexible array members, RFC 0011's model):

```c
struct hack { size_t len; char data[]; };
struct hack *h = malloc(sizeof *h + n);
h->data[n] = 0;                        /* out-of-bounds: n + 9 > n + 8 */
```

Unchanged: everything RFCs 0002–0011 catch.

### Bugs deliberately not caught

- **Accesses the facts leave open** (RFC 0011's rule, restated). `strcpy(p,
  s)` with nothing known about `s`'s length or `p`'s extent is not
  reported. `strncpy(buf, src, n)` with `n` unrelated to `buf`'s extent
  and `src`'s length leaves `buf`'s termination *unknown*, and
  `strlen(buf)` is not reported. Only `unterminated` — no NUL in the
  object on every value the facts allow — is reported.
- **Lengths through unknown writes.** After `read(fd, buf, n)`,
  `fgets`, `snprintf`, a callee that writes the object, or a store the
  string tracker does not read (`buf[i] = c` with `c` not a constant),
  the length is unknown and the object's termination is unknown. `fgets`
  and `snprintf` *do* terminate on success and not on failure; since
  only `unterminated` is reported and only a known length feeds a need,
  "unknown" is both the safe and the useful answer.
- **`strcat` with two unknown lengths.** `strcat(d, s)` needs `len(d) +
  len(s) + 1`, two places; `Affine` holds one. When either length is a
  constant the need is computed; when both are places nothing is
  checked, and `d`'s length afterwards is unknown. A two-place affine is
  future work.
- **Formats with numeric conversions.** `sprintf(buf, "%d", x)` needs at
  least one character per conversion; the need is a lower bound, so a
  buffer that cannot hold the literal part plus one byte per conversion
  is reported, and one that can is not. `%s` arguments contribute their
  known length (a literal, a length place) or nothing.
- **Sized fields the program does not witness.** A field written only
  through `memcpy` of a whole struct, only in another library, or only
  in code with no summary, is not sized. Annotate it.
- **Count fields written through aliases or callees.** A refutation is
  recorded for writes the function makes itself (`o->g = …`, `o->g++`);
  a callee's `written` effect on the object drops the extent in the
  caller (RFC 0006, *`written` forgets what lies below*) and is not a
  refutation. A count changed only ever through a callee with no summary
  is trusted. See *Assumptions*.
- **Integer overflow in `n + 1` and `n * sizeof(T)`.** As RFC 0011:
  extents and needs are mathematical quantities.
- **Wide strings, `strnlen`-bounded reads, `%ls`.** No length facts;
  `wcslen` and friends are not in the string table.
- **Lua's VM loop.** Unchanged; see RFC 0011.

### Accepted false positives

- **A string whose length the checker learnt before a write it cannot
  see.** Every write the model follows (`strcpy`, `strcat`, `sprintf`,
  `strncpy`, `memcpy`, `memset`, `buf[i] = c`, a callee's `written`) is
  applied to every name of the object: the pointer's exact aliases, the
  storage it borrows, and every holder of a borrow of that storage. A
  write through a name the alias relation does not connect (a pointer
  laundered through an integer, a callee whose summary is silent because
  it is the library table's) leaves the facts stale, and a later
  `strcat` may be reported on the old length. The table entries with a
  `w` parameter drop the facts; an annotated declaration that writes
  through a `WEAVEC_BORROWED` parameter is the remaining gap, and
  `WEAVEC_MUT` on it closes it.
- **`sprintf` needs from a format with `%s` of an unknown string.** The
  unknown contributes nothing to the lower bound; the report is
  therefore never wrong, only weak.
- **An inferred sized field whose invariant a *later* unit breaks.**
  Within one unit the two-pass rule (below) sees every store before any
  inferred extent is used. Across units, `weavec-cc`'s compile step
  analyses each unit against the database of the units compiled before
  it, and the link step re-analyses against the whole program; a unit
  that both loads a field and refutes it is consistent with itself, and
  the link step's report set is the authoritative one (RFC 0005,
  *Compile-time reports not repeated at link time*). Within one
  whole-program run the units are analysed in the call graph's order,
  which does not follow types: a reader analysed before the unit that
  refutes the pair keeps its report for that run (see *Across units*
  below; the next build's sidecars carry the refutation from the start).
  A report from a compile step that the link step does not make is the
  known cost of per-unit compilation, as for every cross-unit fact since
  RFC 0005.
- **`WEAVEC_ASSUME` of something false.** Trusted, like every
  annotation; a false assumption hides reports (an assumed `i < n`
  removes the bounds report on `a[i]`) and can make others (`assume(i
  >= n); a[i]` is reported). The user asked for it.
- **Everything RFC 0011 accepts.**

### Assumptions

- Everything RFCs 0001–0011 assume.
- A string literal's length is the number of bytes before its first
  NUL; `sizeof` of a `char` array initialised from one is its extent.
- `strlen(s)` evaluated twice without an intervening write the model
  follows yields the same value: the length place is stable until the
  string facts are dropped, and a fresh one is created after.
- `strcpy(d, s)` writes exactly `strlen(s) + 1` bytes and leaves `d`
  with length `strlen(s)`; `strcat(d, s)` writes from `d + strlen(d)`
  and leaves `strlen(d) + strlen(s)`; `sprintf(d, fmt, ...)` writes at
  least the literal bytes of `fmt` plus the known lengths of its `%s`
  arguments plus one per other conversion, plus the terminator, and
  leaves `d` terminated; `strncpy(d, s, n)` with `strlen(s) >= n` writes
  `n` bytes and no terminator; `memset(d, c, n)` with `c != 0` and
  `memcpy(d, s, n)` from an `s` with `strlen(s) >= n` likewise; `strdup(s)`
  returns `strlen(s) + 1` bytes holding `s`. These are the C standard's
  specifications, not observations about implementations.
- `WEAVEC_SIZED_BY(g)` on a field is trusted: whoever stores into the
  field keeps `g` in step. Stores this function makes are checked
  against it where the facts decide.
- Inference of a sized field assumes the count is written only by code
  the program analyses (the store into the pointer and the write to the
  count both appear in a function body somewhere in the program), and
  that a write to the count through a pointer the alias relation does
  not connect to the object does not happen. The corpus projects satisfy
  both; a program that does not can annotate the field or accept the
  report.
- `WEAVEC_ASSUME(e)` is trusted, and `e` has no side effects the checker
  should model (it is evaluated at run time under WeaveC, as
  `assert(e)`'s operand is under `NDEBUG` off).

## Detailed design

### String facts (Core)

```cpp
// weavec/Core/Spatial.h
struct StringFact {
  std::optional<Affine> length;   // bytes before the terminator, from the object's start
  bool unterminated = false;      // no NUL anywhere in the object
  SourceLocation location;        // where the fact was established (for the note)
};
struct SpatialRecord {
  std::optional<Affine> extent;
  PointerOffset offset;
  SourceLocation location;
  bool declared = false;
  std::optional<StringFact> string;   // new
};
class SpatialTracker {
  // as before, plus:
  void dropStringFacts(PlaceId place);          // the object behind `place` was written
  void dropExtentsOn(PlaceId counter);          // now also drops lengths in `counter`
};
```

`length` set means the object holds a terminator at that offset (and so
is terminated); `unterminated` means it holds none within its extent;
both unset means unknown. Like the extent, the length is affine in one
place at most and is stated from the object's *start*; a pointer at
offset `Elements(k)` into the object sees a string `k` shorter, which the
analysis layer applies when it computes a need (an offset of `Field` or
`Unknown` skips string checks, as it skips bounds checks). The join keeps
`length` only when both sides agree and `unterminated` only when both
sides say so; a record on one side only is dropped as today.

### Length places (Analysis)

`PlaceBuilder::lengthPlace(PlaceId string)` creates a base place named
`strlen(<name of string>)`; it is a *length place* (`isLengthPlace`),
never written, with no variable behind it. `tracksScalar` accepts it, so
relations and constant bounds attach to it; `stableSummaryPathOf` has
nothing for it, so it never appears in a summary (a need or extent in a
length place is not exported).

When `strlen(E)` is evaluated on a pointer value `E` whose place `s` has
a record with a known length `L`: the call's value is `L` (a constant
fact on the receiving place when `L` is a constant; the relation `Equal`
to `L.place` otherwise). When `s` has no known length: a length place
`ℓ` is created, `s`'s record gets `length = {ℓ, 1, 0}`, and the call's
value is `ℓ` (`n = strlen(s)` records `n Equal ℓ`; `malloc(strlen(s) +
1)` records the extent `{ℓ, 1, 1}` through `affineOf`, which learns to
read `strlen(E)` as the length of `E`'s string, creating the place if
needed). When `s` is `unterminated`, the call is reported (below) and no
length is recorded. A record whose string facts are dropped forgets `ℓ`;
a later `strlen(s)` creates a new place, because the old one names the
old length. Extents and relations that name the old place stay valid:
they were about that value.

### Sources of string facts (Analysis)

Established where RFC 0011 establishes extents, on every name of the
object (`stringTargets(place)`: the place, its exact aliases, the storage
it borrows when it holds a loan on a variable, and every zero-offset
holder of a loan on that storage when the place is such storage):

- A string literal: `length = its byte length`, on the literal borrow.
- `char a[N] = "…"` and `char a[] = "…"`: the array's storage place gets
  `length = the literal's length`; `char a[N] = {…}` with a NUL among
  the constant initialisers: `length` = the index of the first NUL. An
  array declared without an initialiser gets nothing (its bytes are
  indeterminate, which is RFC 0008's business, not this RFC's).
- `strlen(s)`: as above.
- `strcpy(d, s)`, `stpcpy`: `d.length = s.length` when known, else `d`'s
  facts dropped. `strcat(d, s)`: `d.length = d.length + s.length` when
  `sumOf` can add them (at most one place between them), else dropped.
  `sprintf(d, fmt, …)`: `d.length` = the format's exact length when it
  has no conversions other than `%%`, `%c` and `%s` with known lengths
  (one place at most), else dropped. `strncat(d, s, n)`, `snprintf`,
  `vsnprintf`, `strlcpy`, `strlcat`, `fgets`, `getcwd`, `realpath`,
  `strerror_r`: dropped (terminated on success, but the checker reports
  only `unterminated`, so "unknown" is exact enough).
- `strdup(s)`: the fresh result has `extent = s.length + 1`, `length =
  s.length`, when `s.length` is known; `strndup(s, n)`: the result has
  `extent <= n + 1`, which `Affine` cannot say; nothing.
- `strncpy(d, s, n)`: when `s.length >= n` is decided (constants, the
  same place, a relation, `atLeast`/`atMost`) *and* `n >= extent(d)` is
  decided: `d.unterminated = true`; when `s.length < n` is decided:
  `d.length = s.length`; else dropped. `memcpy(d, s, n)` from an `s`
  with a known length: the same rule. `memset(d, c, n)` with a non-zero
  constant `c` and `n >= extent(d)` decided: `unterminated`; with `c ==
  0` and `n >= 1` decided: `length = 0`; else dropped. `memmove` as
  `memcpy`.
- `d[i] = 0` (a constant NUL): `unterminated = false`; `length = 0` when
  `i` is the constant 0, else unknown (there may be an earlier NUL).
  `d[i] = c` for a non-zero constant `c`: `length` dropped,
  `unterminated` kept (still no NUL). `d[i] = e` otherwise: dropped.
  `*d = 0`, `*(d + i) = 0`, `d->name[i]` on a field: the same through
  `accessOf`.
- Every other write the model follows to the object drops its string
  facts: a table entry's `w` parameter not listed above, a callee's
  `written` effect on the object or below it (`forgetBelow`), an
  assignment to a field of it, a whole-record copy, `reinit`.

### String checks (Analysis)

Run in the final pass, where RFC 0011's bounds checks run, through
`reportBounds` with the library-call wording:

- `strcpy(d, s)`, `stpcpy`: `need = s.length + 1` against `d`'s extent
  (`knownExtentOf` on the argument, as for `memcpy`), when `s.length` is
  known. `strcat(d, s)`: `need = d.length + s.length + 1` when `sumOf`
  adds them. `sprintf(d, fmt, …)`: `need = lower bound + 1` as above.
  The `_chk` forms (`__builtin___strcpy_chk(d, s, size)`,
  `__builtin___sprintf_chk(d, flag, size, fmt, …)`, `__builtin___snprintf_chk`)
  are read with their arguments shifted.
- A *terminator-seeking read* of an `unterminated` object is
  `out-of-bounds`: the source of `strcpy`, `stpcpy`, `strcat`, `strdup`,
  `strchr`, `strrchr`, `strstr`, `strpbrk`, `strspn`, `strcspn`,
  `strcmp`, `strcoll`, `strcasecmp`, `strlen`, `puts`, `fputs`, `perror`,
  `system`, `getenv`, `atoi`/`atol`/`atoll`/`atof`, `strtol` and family
  (first argument), `fopen`/`open`/`access`/`stat`/`unlink`/`remove`/
  `rename`/`mkdir`/`chdir`/`opendir` (path arguments), and a `%s`
  argument of `printf`/`fprintf`/`sprintf`/`snprintf` whose format is a
  literal. The table gains a `readsString` set per entry; `strnlen`,
  `strncmp`, `strncpy`'s source and `memchr` are bounded reads and are
  not in it.
- The needs of `strcpy`/`strcat`/`sprintf` on a *parameter* of unknown
  extent are recorded as `requires-extent` when they are constant (a
  literal source); a need in a length place is not exported (no summary
  path), as *Length places* says.

### Sized fields (Analysis, Frontend)

**Annotation.** `WEAVEC_SIZED_BY(g)` on a pointer field `f` of record `R`
names a sibling field `g` of `R` of integer type; anything else
(`WEAVEC_SIZED_BY` on a non-pointer field, `g` not a field of `R`, `g`
not an integer) is `invalid-annotation`, reported once per field at the
declaration when a function of the unit reads or writes it. `sizedFieldOf(FieldDecl)`
returns `{count FieldDecl, unit}` with the unit `sizeof(*f)` (1 for
`void *` and `char *`).

**Loads.** Wherever the checker asks for the spatial record of a place
`P = o->f` (or `o.f`, or deeper: the record of the object that holds `f`
is found by `countedObjectOf`'s walk: fields only, no dereference between
`o` and `f`) and finds none, and `f` is sized by `g` (annotated, or
confirmed inferred, below): the record is synthesised as `{extent =
{place(o->g), unit, 0}, offset = Zero, location = f's declaration,
declared = true}` (`spatialRecordAt(place, state)`, used by
`knownExtentOf` and by the copy arm of `applyPointerAssign`). A place
that *has* a record (this function stored it) keeps it: what the function
did is more precise than the invariant. Writes to `o->g` drop extents
counted in it as today (`assignScalar` → `dropExtentsOn`); a synthesised
record is recomputed at the next load from the new `o->g`, which is the
invariant's meaning.

**Stores.** `o->f = v` where `f` is *annotated* sized by `g`: with `v`'s
extent `E` known and `o->g`'s value decided against it
(`boundsVerdict(need = {place(o->g), unit, 0}, have = E, between,
atMost, atLeast)` says `OutOfBounds`), report `annotation-mismatch`
("'b->data' is declared WEAVEC_SIZED_BY(cap) but is given 4 bytes where
'b->cap' says 8"). A store of null is not checked; a store whose extent
or count is unknown is not checked. The count written *after* the
pointer (`b->data = malloc(n); b->cap = m;`) is checked at the function's
exit with the same rule (the store's extent is still on `b->data`'s
record), reported at the count's store. Inferred fields are not checked
at stores: a store that contradicts the pair refutes it instead.

**Inference.** In the final pass, `finalizeSummary` runs over the exit
state (the state on the edge into the exit block, which RFC 0007 already
computes for leaks):

- For every pointer-field place `P = o->f` this function *stored to*
  (recorded in `applyPointerAssign` when `dest` is a field place below a
  dereference or a record variable, with `f`'s key `fieldKey(R, f)`
  non-empty): if `P` is null or moved at exit, nothing; else if `P`'s
  record has `extent = {X, s, 0}` and `X` is the place `o->g` for a
  sibling integer field `g` of `R`, or `relations.between(X, o->g) ==
  Equal` (with offset 0) for such a `g`: **witness** `(key(f), key(g),
  s)`; else **refute** `key(f)`.
- For every integer-field place `Q = o->g` this function wrote
  (`assignScalar`/`forgetScalar`/adjustments on a field place below a
  dereference or a record variable), for every pointer-field sibling `f`
  of `R`: if `o->f` was stored to in this function (so the rule above
  decided it), nothing; else **refute** `(key(f), key(g))`.
- A field key is RFC 0010's count-field key (`struct buf.data`); a record
  with no stable spelling (anonymous) contributes nothing.

`SummaryStore` keeps the unit's witnesses and refutations in one
`SizedFieldFacts` (`witnesses: set<{fKey, gKey, scale}>`,
`unsizedFields: set<fKey>`, `unsizedPairs: set<{fKey, gKey}>`);
`UnitExports` carries them; the sidecar spells them as `sized-field
<fKey> <gKey> <scale>` and `unsized-field <fKey> [<gKey>]` with spaces in
keys as `~` (RFC 0011's convention for field keys in offsets);
`ProgramDatabase` unions them across units. `f` is **confirmed sized by `g` at scale `s`** when the
witnesses for `f` (unit and database together) are exactly `{(g, s)}`,
`f` is in no refutation set, and `(f, g)` is in none. Version 8 of the
sidecar; version 7 files are rejected as today.

**Two passes in a unit.** `TranslationUnitAnalyzer::run` analyses and
reports as today, with inferred sized fields taken from the *database
only* (annotated fields apply immediately). At the end, if the unit's
own witnesses confirm any field the database alone did not, every
reported function whose body contains a `MemberExpr` on such a field
(a syntactic walk) is analysed once more with the unit's confirmations
in force, and the `out-of-bounds` diagnostics of that run that the first
run did not produce (same id, location and message) are emitted. Only
`out-of-bounds` can change: a synthesised record adds an extent where
there was none, which enables bounds reports and nothing else (a
requirement is recorded only for a *parameter root* of unknown extent,
which a field load is not; nullness, moves and resources do not read
extents). The second pass costs one analysis per function that loads a
confirmed field; the common unit has none.

**Across units.** The exports' witnesses and refutations are part of
`sameSummariesAs`, so a cyclic group iterates on them; widening (RFC
0011) unions them like `countFields`. But the pair is a fact about a
*type*, not about a callee: the unit that fills `struct vec` and the unit
that reads it need not call each other, so RFC 0005's callee-first unit
order does not put the witnesses before the reader. Two additions carry
the fact to where it is used:

- Every bounds check that looks up the extent of an *unannotated*
  pointer field records the field's key (`SummaryStore::noteSizedFieldLoad`);
  `UnitExports::sizedFieldLoads` exports the set, sidecar line
  `loads-field <fKey>`. It is what the unit *could* learn.
- `ProgramAnalysis::run`, after the sweep over the unit graph, runs one
  more reporting pass (`reportConfirmedSizedFields`) over every unit
  whose reporting run saw fewer confirmed pairs than the settled
  database now has, restricted to units whose `sizedFieldLoads` name a
  newly confirmed field. The run's `alreadyReported` set is what the unit
  has shown so far, so only new reports appear. The pass is one: a
  witness it adds (a copy `dst->items = src->items; dst->cap = src->cap`
  seen with the count's extent) widens the program's view for the next
  build, not this one.
- `weavec-cc`'s link step adds to RFC 0005's "needs analysis" test
  (imports something another object defines, or has a callback type
  another object provides candidates for): *loads a field some other
  object witnesses*. A unit whose sidecar has no `loads-field` line for
  any witnessed field keeps its compile-time view, as today.

A refutation in a unit analysed *after* a reader is not retracted: the
reader's report on a pair the program later refuted stands for that run.
The window is one link (the next build's sidecars carry the refutation
from the start), and the shape needs two other units to witness the pair
first; it is accepted under *Accepted false positives*.

### Offset relations and lower bounds (Core)

```cpp
// weavec/Core/Relation.h
struct RelationEdge { Relation relation; std::int64_t offset; };  // lhs REL rhs + offset
class RelationTracker {
  void learn(PlaceId lhs, Relation, PlaceId rhs, std::int64_t offset = 0);
  std::optional<Relation> between(PlaceId lhs, PlaceId rhs) const;     // offset 0 only (unchanged callers)
  std::optional<RelationEdge> edgeBetween(PlaceId lhs, PlaceId rhs) const;  // with the offset
  void learnAtMost(PlaceId, std::int64_t);   std::optional<std::int64_t> atMost(PlaceId) const;
  void learnAtLeast(PlaceId, std::int64_t);  std::optional<std::int64_t> atLeast(PlaceId) const;  // new
  ...
};
```

A pair's entry is `(relation, offset)` stated as `min REL max + offset`
(flipping negates the offset). `learn` on a pair already known with the
*same* offset narrows the relation as today; with a *different* offset,
the stronger of the two in the direction of the new relation is kept
when they are comparable (`i <= n` then `i < n - 1`: the second; `i <
n - 1` then `i <= n`: the first), and the pair is forgotten when they
are not (`i < n + 3` then `i > n - 3`). `Equal` with an offset (`j = i +
1`) is a genuine equality shifted; `between`/`edgeBetween` follow one
`Equal` hop as today, composing offsets (`j = i + 1; if (j < n)` gives
`i < n - 1`). The join keeps a pair only when both sides have it with
the same offset (the weakest relation), and drops it otherwise.

`atLeast`, the mirror of RFC 0011's `atMost`: learnt from `x > k` (`x
>= k + 1`), `x >= k`, and the failing edges of `x < k` / `x <= k`;
narrowed to the *larger* bound; joined as the *smaller*; cleared by any
write; looked up through one `Equal` hop with the offset applied. An
unsigned comparison with `k` in the top half records nothing, as for
`atMost`. RFC 0009's constant fact (`x == 5`) already implies both
bounds and is read first.

### Bounds checks through offsets (Core, Analysis)

`boundsVerdict` gains `needAtLeast` and `haveAtLeast`:

- rule 3′: `need` in a place bounded *below* by `L`, `have` constant:
  `need.scale * L + need.constant > have.constant` is `OutOfBounds`
  ("'i' is at least 8"); `have` in a place bounded below, `need`
  constant: nothing (a large object holds a small access).
- rule 5′: `need` in a place bounded *above* by `U` with `need.scale *
  U + need.constant <= 0`: `BeforeStart` ("'i' is at most -1").

The analysis layer substitutes an offset relation before calling it: for
`need = {i, a, c}`, `have = {n, b, d}` and `edgeBetween(i, n) = (REL,
k)`, the need at the boundary is `{n, a, c + a * k}` under `REL`, and
the message spells the boundary ("'i' may be 'n' - 1"). `boundaryRequirement`
applies the same substitution when it exports a requirement (`for (i =
0; i < n - 1; i++) a[i + 1]` requires `n` elements).

### Learning offset relations (Analysis)

`learnRelation` reads both sides with `affineOf`: `a*x + c OP b*y + d`
with `a == b == 1` is `x OP y + (d - c)`; a scaled side is not related
(as today). `assignScalar(x, y + k)` records `x Equal y + k`; RFC
0009/0010's `x++` adjustment forgets `x`'s pairs as today (the next
condition edge re-learns them). `testInteger` learns `atLeast` beside
`atMost`.

### `WEAVEC_ASSUME` (Analysis, Frontend)

`weavec.h`:

```c
#if WEAVEC_ENABLED
WEAVEC_ANNOTATE_("weavec.assume") static inline void weavec_assume_(int condition) { (void)condition; }
#define WEAVEC_ASSUME(expr) weavec_assume_((expr) != 0)
#else
/* Unevaluated, so `expr` is neither run nor an unused-variable warning. */
#define WEAVEC_ASSUME(expr) ((void)sizeof((expr) != 0))
#endif
```

A call whose callee carries `weavec.assume` is handled in `handleCall`
before summary lookup: its first argument is applied with
`applyCondition(arg, /*holds=*/true, /*wrapped=*/true, state)` — the
same function the true edge of `if (arg)` runs, so pointer equalities
unite aliases, `x OP k` narrows scalar facts and bounds, `x OP y` learns
a relation, a test of a call result selects its outcome classes, and
every fact learnt refines the guards it contradicts (RFC 0009). An
infeasible assumption (`WEAVEC_ASSUME(0)`, or one contradicting a
must-fact) ends the path, as an infeasible edge does. The argument is
not otherwise a use of anything (no read is recorded; it is evaluated at
run time, but a read of a moved pointer in an assumption is the user's
statement, not a use the checker second-guesses). `weavec_assume_` has
an empty body and is never analysed or summarised; under other compilers
the macro is `((void)sizeof((expr) != 0))`, an unevaluated operand, so
`expr` is neither run nor a source of unused-variable warnings.
`weavec.assume` on anything but that function is `invalid-annotation`.

### Summary and sidecar format (Core, Frontend)

The summary text format is unchanged at version 7: no new line kinds
(a need or extent in a length place is not exported; sized fields are
per record, not per function). The sidecar goes to version 8 for the
`sized-field`, `unsized-field` and `loads-field` lines; the program dump
(`--whole-program --dump-analysis`) prints the witnesses and refutations
after `count-field`, and the per-function dump prints string facts in
the exit state's `spatial{...}` (`s string=len(strlen(s))`,
`name string=unterminated`).

### Performance

- One `std::optional<StringFact>` per spatial record (an `Affine` and a
  bool); records exist only for pointers with a known extent or offset.
- `RelationTracker` entries grow by one `int64_t`; `atLeast` is a second
  small map. Both are empty in most functions.
- Length places are one place per `strlen` evaluation of a string of
  unknown length; a function that calls `strlen` in a loop on a string it
  does not write reuses the one place.
- The second pass in a unit runs only over functions that load a field
  the unit's own witnesses confirmed; the syntactic walk that finds them
  is one traversal per reported function.
- The cross-unit pass re-analyses only units that both looked up an
  unannotated field's extent and were reported on before the program
  confirmed its pair; a program with no inferred pair, or whose readers
  call the unit that fills the pair, has none.

## Annotation surface

`WEAVEC_SIZED_BY(n)` is extended to pointer fields; `WEAVEC_ASSUME(expr)`
is added. `weavec.h` goes to 0.7; `include/weavec/Analysis/Annotations.h`
gains `spelling::Assume = "weavec.assume"`; `docs/annotations.md` gains
both rows.

```c
/**
 * On a pointer parameter: the caller passes at least `n` elements (bytes
 * for `void *`) behind it, `n` being another parameter of the same
 * function by name. On a pointer field: the object holds at least `n`
 * elements behind it, `n` being another integer field of the same struct
 * by name (`struct buf { char *WEAVEC_SIZED_BY(cap) data; size_t cap; }`).
 * ...
 */
#define WEAVEC_SIZED_BY(n) WEAVEC_ANNOTATE_("weavec.sized_by." #n)

/**
 * States that `expr` holds here, as if the code below were inside
 * `if (expr)`: `WEAVEC_ASSUME(len <= cap)` lets the checker prove an
 * access in bounds when the invariant that makes it so is not visible
 * in the function. Trusted like every annotation. `expr` is evaluated
 * (and must be side-effect free) under WeaveC and is not compiled at all
 * elsewhere.
 */
#define WEAVEC_ASSUME(expr) ...
```

Authoritative in both cases. A `WEAVEC_SIZED_BY(g)` field with no such
sibling integer field, or on a non-pointer field, is `invalid-annotation`.
A `WEAVEC_ASSUME` outside a function body does not parse (it is a
statement).

## Diagnostics

No new id. New message forms, all under existing ids, each with a lit
test in `test/Analysis/rfc0012-*.c` pinning the text and a unit test of
the rule:

- **`out-of-bounds`**, error:
  - `'strcpy' accesses 'strlen(s)' + 1 bytes of 'p', which has
    'strlen(s)' bytes` (a string need against an extent in the same
    length place; the library wording of RFC 0011 with a length place's
    name);
  - `'strcpy' accesses 6 bytes of 'buf', which has 4 bytes` (a literal);
  - `'strcat' accesses 5 bytes of 'buf', which has 4 bytes`;
  - `'sprintf' accesses at least 5 bytes of 'buf', which has 4 bytes` (a
    format's lower bound);
  - `'strlen' reads past the end of 'name', which is not NUL-terminated`
    (a terminator-seeking read of an `unterminated` object), with the
    note `'name' is left without a terminator here`;
  - `'buf[i]' is out of bounds: 'i' is at least 8 in an object of 8
    bytes` (a lower bound, rule 3′);
  - `'a[i + 1]' may be out of bounds: 'i' may reach one below 'n', and
    'a' has 'n' * 4 bytes` (an offset relation at the boundary);
  - `'b->data[b->cap]' is out of bounds: 'b->cap' is the number of
    elements of 'b->data'` (a sized field's count; RFC 0011's wording
    with the count's name), with the note `'b->data' is declared here`.
- **`annotation-mismatch`**, error: `'b->data' is declared
  WEAVEC_SIZED_BY(cap) but is given 4 bytes where 'b->cap' says 8`, with
  the note `'b->data' is declared here`.
- **`invalid-annotation`**, warning: `field 'data' is declared
  WEAVEC_SIZED_BY(cap) but 'cap' is not an integer field of 'struct
  buf'`; `field 'n' is declared WEAVEC_SIZED_BY(cap) but is not a
  pointer`; both once per unit, at the field, when the field is first
  touched; `'weavec.assume' is not an annotation for 'f'` (on anything
  but the header's function).

Changed wording, same id: none. Unchanged ids whose *when* changes:
`out-of-bounds` is now also produced by `strcpy`, `stpcpy`, `strcat`,
`sprintf`, the terminator-seeking reads, sized-field loads, lower bounds
and offset relations.

## Drawbacks

- **The string tracker is a mutable fact on an aliased object.** Extents
  never change after the allocation; lengths change with every write.
  The design updates every name the alias relation and the loans
  connect and drops the facts on every write it cannot follow; the
  remaining exposure is a write through a name the model does not
  connect, listed under *Accepted false positives*. This is the first
  fact in the state with that property since RFC 0008's nullness, which
  made the same bet and has held up on the corpus.
- **Length places are places without storage.** Every query that walks
  the place table (`tracksScalar`, `nameOf`, the dump) must know them.
  They are base places with a recognisable name and a flag; the
  alternative, a second kind of `Affine` operand, would touch every
  `Affine` consumer.
- **Inference needs a second pass.** One analysis per function that loads
  a confirmed field, after the first. The alternative — using a witness
  as soon as it appears — would report on an invariant a later function
  breaks; the RFC series has been consistent that a report rests on
  facts. The cost is bounded and measured on the corpus.
- **Sidecar bump.** Version 7 sidecars are rejected; every RFC since
  0003 has done this.
- **Two more integers per relation entry** and a second bound map.
  Bounded like the tracker itself.
- **`WEAVEC_ASSUME` is a runtime call under WeaveC.** An empty inline
  function the optimiser removes; the alternative (`__builtin_assume`)
  would hand the optimiser a claim the user made to the *checker*, and a
  wrong one is undefined behaviour at run time. Under other compilers
  the macro is nothing.

## Alternatives

- **Model strings as a pointer-plus-length type (fat strings).** Rust's
  `&str`, Checked C's `_Nt_array_ptr`. Requires a type the program does
  not have; the facts here are the static shadow of what the program
  already does at run time.
- **Only literals for `strcpy`/`strcat`.** Catches the Juliet cases and
  none of the `malloc(strlen(s) + 1)` idiom that real code has. The
  length place costs one flag on a base place and is what makes RFC
  0011's rules apply unchanged.
- **Report `strlen` on anything not known terminated.** The sound
  direction and unusable: every string parameter is "not known
  terminated". Rejected, as RFC 0011 rejected "cannot prove in bounds".
- **Sized fields by annotation only.** Simpler and enough for a user who
  annotates. The corpus projects are not annotated and the RFC series'
  premise is that inference carries unannotated code. Inference is kept
  conservative (a witness plus no refutation) and its cost is in a second
  pass over the functions it affects.
- **Sized-field inference at the store site rather than the exit.**
  `b->cap = n; b->data = malloc(n);` (count first) would refute at the
  count's store, since the old pointer's extent is not `n`. Deciding at
  the exit handles either order and the null-then-count destructor
  shape.
- **Use unit-local witnesses as they appear (no second pass).** Cheaper
  and wrong when a later function refutes; see *Drawbacks*.
- **A full interval domain with widening.** Would prove more in bounds
  and change nothing the "definite" rules report beyond what offsets and
  lower bounds add here, at the cost of loop reasoning that has to
  guess. RFC 0011 rejected it for the default and this RFC does again;
  an opt-in "report what cannot be proven" mode remains the place it
  would pay for itself.
- **`WEAVEC_ASSUME` as `__builtin_assume`.** See *Drawbacks*.
- **A separate `unterminated-string` id.** A terminator-seeking read of
  an unterminated object *is* an out-of-bounds read (CWE-126 by way of
  CWE-170); one id keeps `-W` control and the recall table simple, and
  the message names the cause.

## Prior art

- **Checked C** (`_Nt_array_ptr<T>`, `count(n)` on struct members,
  `_Dynamic_check`). Null-terminated pointer types whose bounds widen as
  the program tests bytes, and member counts exactly like
  `WEAVEC_SIZED_BY` on fields. We take the member count and the
  observation that most string bounds are `strlen + 1`.
- **Clang `-fbounds-safety`** (`__counted_by`, `__sized_by`,
  `__null_terminated`, `__ended_by`). `__counted_by(n)` on a struct
  member is this RFC's field annotation with the same "count of
  elements, bytes for `void *`" rule and the same requirement that `n`
  be a sibling; its `__null_terminated` is the `length`/terminated fact.
  Its inference (`-fbounds-safety` infers nothing; every bound is
  written) is what this RFC's inference avoids requiring.
- **Clang `-Wfortify-source` / `-Wformat-overflow` and GCC's
  `_FORTIFY_SOURCE`, `-Wstringop-overflow`.** Constant-source `strcpy`,
  `strcat` and `sprintf` against `__builtin_object_size`; GCC's
  `-Wstringop-overflow` also tracks `strlen` results through locals
  (its "string length range" pass). We take the format lower-bound rule
  and the `strlen` tracking, on the checker's places instead of the
  optimiser's SSA.
- **Clang static analyzer `CStringChecker`.** Tracks `strlen` as a
  symbolic value per string region, checks `strcpy`/`strcat`/`strncpy`
  destinations against it, and flags `strlen` on a non-terminated
  region. The length place is its `strlen` symbol; its "not
  null-terminated" state is `unterminated`.
- **Juliet / SARD CWE-121, 122, 126, 170.** The `strcpy`/`strcat`/
  `sprintf`/`memcpy`-into-`char buf[N]`, `struct {buffer, size}` and
  `strncpy`-without-terminator families whose shapes the recall set
  adds.
- **Splint** (`/*@maxSet@*/`, `/*@nullterminated@*/`). Annotation-driven
  buffer and termination constraints on C; the original of every
  `count(n)`-style annotation.
- **Astrée / Frama-C EVA.** Interval and octagon domains for exactly
  `i < n - 1` shapes; this RFC takes the octagon's "difference with a
  constant" for pairs the tracker already relates and nothing more.

## Unresolved questions

- **Whether `strcat` needs a two-place `Affine`.** `strcat(d, s)` with
  both lengths in places is common in path-building code; the corpus
  shows how often the check is skipped for that reason.
- **Whether inferred sized fields should also be checked at stores.**
  Today a contradicting store refutes the pair and the program is
  silently unchecked on that field; a warning ("field 'data' is sized by
  'cap' in `init` but not in `shrink`") would tell the user where to
  look. Decided by the corpus: how often a refutation is a bug versus a
  shape.
- **Length places in the dump.** `spatial{}` names them `strlen(s)`;
  two evaluations after an intervening write produce two places of the
  same name. Whether to number them.
- **`WEAVEC_ASSUME` and liveness.** The argument's reads are not uses;
  whether the variables it mentions should keep their loans alive
  (RFC 0006) is open. Today they do not.

## Future work

- **Lengths in summaries.** `char *my_strdup(const char *s)` returns
  `fresh extent = strlen(param 0) + 1`; a `PathAffine` over a *length of
  a path* would carry it, and `requires-terminated` on a parameter the
  callee walks would carry the read check across calls.
- **A two-place `Affine`** for `strcat`, `memcpy(d + off, s, n)` against
  `cap - off`, and RFC 0011's `min(n, cap)` requirements.
- **Post-condition-guarded consumes** (RFC 0011, *Future work*),
  unchanged.
- **Termination as a per-outcome fact** (`fgets` returns non-null ⇒
  terminated), so the failure path is the only unknown one.
- **`WEAVEC_ASSUME` in declarations** (`__attribute__((assume))`-style
  preconditions on functions), so an invariant is stated once at the
  interface rather than at each use.
