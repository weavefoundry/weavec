# RFC 0011: Spatial safety: derived pointers, extents and bounds

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-04
- **Accepted**: 2026-09-04
- **Tracking issue**: TBD
- **Supersedes / superseded by**: lifts the RFC 0001 exclusion of *bounds*
  from the model (*Deliberately not caught*); replaces the boolean
  *interior* attribute of RFC 0006's alias edges, RFC 0007's resource
  records, RFC 0003's `copy` sources and the analysis layer's value
  origins with a *pointer offset*; moves RFC 0002's `lifetime-too-short`
  check from the store that creates the borrow to the death of the
  borrowed object (RFC 0006's *Loans end at the last use of their holder*,
  applied to the other end of the loan); amends RFC 0004's *Pointer
  identity* so that taking the address of a field or element of an object
  reached through a pointer derives a pointer to the *same object*; adds
  a widening step to RFC 0005's whole-program fixpoint (as amended by RFC
  0010). Bumps the summary text format (RFC 0003) and the sidecar format
  (RFC 0005) from 6 to 7.

## Summary

Ten RFCs in, WeaveC checks *temporal* safety (use after free, double
free, leaks, null, uninitialised reads) and says nothing about *spatial*
safety: `p = malloc(n); p[n] = 0;` is accepted without comment, and so is
`memcpy(buf, src, 64)` into a 16-byte buffer. README promises "Rust-style
memory safety"; half of the CWE top 25 is out-of-bounds writes and reads.
This RFC adds the missing half, and it does so on top of a more precise
model of what a pointer *is*, which fixes a family of false positives and
one false negative the current model has around pointers that point
*into* an object rather than at its start:

1. **Derived pointers.** A pointer value is a *base object* and an
   *offset* into it. The offset is `zero`, a signed *field path*
   (`+struct outer .in`, `-struct outer .in`), a signed *element count*
   (`+4`), or `unknown`. `&p->f`, `&p[i]`, `p + k` and `(char *)p -
   offsetof(T, f)` derive a pointer to `p`'s object at a composed offset;
   the alias relation, the resource records, value origins and summary
   sources carry the offset in place of today's `interior` bit. A field
   offset that is later subtracted (`container_of`) yields the object's
   start again, so freeing through it is a valid release of the object
   and not a leak of its holder; a pointer to a field of a fresh object
   returned or stored (`return &object->json`, `push(list, &node->link)`)
   carries the object with it, so the holder does not leak; and a
   dereference through a field-derived pointer names the field's own
   subtree (`i->buf` *is* `o->in.buf`), so a release through it reaches
   the summary — today it does not, and a caller's later use is missed.
2. **Deferred lifetime checks.** Storing a borrow of a local into a place
   that outlives it is reported when the local *dies* while the place
   still holds the borrow, not at the store. The linked-stack-of-locals
   idiom (`fs.prev = ls->fs; ls->fs = &fs; ...; ls->fs = fs.prev;`) is
   accepted; every previously reported case still is, at the same
   location.
3. **Extents.** An allocation's *extent* is recorded when the checker can
   read it: a constant, `n`, `n * sizeof(T)`, `n + k`, `sizeof(T)`,
   `calloc(n, size)`, an array variable's declared size, a string
   literal's length, a `fresh` result whose summary says how big it is
   (`xmalloc(n)`), or a `WEAVEC_SIZED_BY(n)` parameter. A pointer's
   *spatial record* is its object's extent and its offset into it.
4. **Bounds checks.** An element access, a pointer's dereference after
   arithmetic, and the buffer/length argument pairs of the library table
   (`memcpy`, `memset`, `strncpy`, `fgets`, `snprintf`, `read`, `fread`,
   ...) are checked against the spatial record: a *definite*
   out-of-bounds access is `out-of-bounds`. Definite means the access is
   past the end (or before the start) on every value the facts allow:
   constant against constant, `p[n]` against `malloc(n)`, `p[i]` after
   the loop `for (i = 0; i < n; i++)` has exited (`i >= n` is known), and
   `p[i]` under `i <= n` where the extent is exactly `n` (the classic
   off-by-one, reported with *may* wording). Anything the facts leave
   open is not reported; a missed bound is a false negative this RFC
   accepts, not a false positive.
5. **Relational facts.** RFC 0009's facts are about one integer place;
   the loop reasoning above needs two. The state gains *relations*
   between integer places (`i < n`, `i <= n`, `i == n`, `i >= n`, `i >
   n`), learnt on condition edges and from copies, dropped when either
   side is written, joined by intersection.
6. **Extents in summaries.** A `fresh` return carries the extent of what
   it allocates in terms of the callee's parameters; a summary records
   what the callee *requires* of each pointer parameter's extent (from
   `WEAVEC_SIZED_BY` or inferred from unconditional accesses), so a
   too-small buffer is reported at the call.

Two engineering changes ride along. The whole-program fixpoint (RFC 0005,
RFC 0010) *widens* after a fixed number of rounds: a member's new exports
are joined with its previous ones, which makes the sequence monotone in
the finite summary lattice and so guarantees convergence, where today Lua
stops at `MaxRounds` with most units still dirty and the reported set
depends on the schedule. And the repository gains a *recall* check, a
small corpus of known-bad programs in the shape of the Juliet test suite
with the expected diagnostic pinned per case, run in CI beside the
precision corpus, so a precision change that quietly stops catching a bug
class fails the build.

## Motivation

### The missing half

The README's goal is Rust-style memory safety. Rust's guarantee has two
halves: no dangling pointers (temporal) and no out-of-bounds accesses
(spatial). RFC 0001 deferred bounds "deliberately"; ten RFCs later the
temporal half is in good shape on real code and the spatial half is
absent. Probing the current build:

```c
char *dup_upper(const char *s) {
  size_t n = strlen(s);
  char *p = malloc(n);           /* one short */
  for (size_t i = 0; i < n; i++) p[i] = toupper(s[i]);
  p[n] = '\0';                   /* heap overflow: nothing reported */
  return p;
}
void fill(void) {
  char buf[16];
  memcpy(buf, src, 64);          /* stack overflow: nothing reported */
  for (int i = 0; i <= 16; i++) buf[i] = 0;   /* off by one: nothing */
}
```

These are CWE-787, CWE-122, CWE-121 and CWE-193, the top of every
vulnerability ranking for a decade. A user who reads "memory safety" and
tries these first learns nothing.

### What a pointer is

The current model has one bit for "does not point at the start": an alias
edge is `exact` or `interior`, a resource record is `interior` or not, a
`copy` source is `interior` or not. That bit is enough to refuse `free(p +
4)` and to keep `p != q` from refuting an alias between `p` and `p + 1`,
and not enough for four shapes real code uses constantly. Probing the
current build (`/tmp` probes reproduced in the tests of this RFC):

```c
struct inner { char *buf; };
struct outer { struct inner in; };
static void release_inner(struct outer *o) {
  struct inner *i = &o->in;      /* today: a loan on o->in */
  free(i->buf);                  /* today: frees the local place i->buf */
}
void caller(struct outer *o) {
  release_inner(o);
  use(o->in.buf);                /* use-after-free: NOT reported */
}
```

`&o->in` is modelled as a borrow of `o->in` held by `i`. A borrow gives
`i` a loan, but `i->buf` is a place of its own with no relation to
`o->in.buf`, so the release never reaches the summary
(`param 0 *.in.buf: freed` is missing) and the caller's use is missed. A
false negative in the core guarantee.

```c
#define container_of(p, T, f) ((T *)((char *)(p) - offsetof(T, f)))
void free_container(struct inner *i) { free(container_of(i, struct outer, in)); }
void use(void) {
  struct outer *o = malloc(sizeof *o);
  free_container(&o->in);        /* today: 'o' is leaked */
}
struct inner *make_inner(void) {
  struct outer *o = malloc(sizeof *o);
  return &o->in;                 /* today: 'o' is leaked */
}
void link_node(struct list *head) {
  struct node *n = malloc(sizeof *n);
  push(head, &n->link);          /* today: 'n' is leaked */
}
```

Three false-positive leaks, all the same shape: a pointer to a field of an
owned object *is* the object, offset by the field, and handing it out
hands the object out. jansson does this for every `json_t` (`return
&object->json`, `json_to_object` casting back) and for every hash-table
node (`insert_to_bucket(…, &pair->list)`); the Linux kernel's whole
object model is `container_of`. The corpus README lists six jansson leaks
under exactly this cause.

```c
void open_func(LexState *ls, FuncState *fs) {
  fs->prev = ls->fs;
  ls->fs = fs;                   /* today: 'ls->fs' may outlive 'fs' */
  ...
}
void close_func(LexState *ls) { ls->fs = ls->fs->prev; }
```

Lua's parser, and every recursive-descent parser written the same way,
keeps a stack of frames as a linked list of locals threaded through a
pointer in caller memory, and unlinks each before returning. The RFC 0002
check reports the link at the store because `ls->fs` lives longer than
`fs`; that the store is undone before `fs` dies is invisible to a check
made at the store. jansson's `hashtable_init(&parents_set)` (eight
`lifetime-too-short` in the corpus) is the same shape one level down:
`buckets->last = &hashtable->list` stores the address of a field of the
stack table into memory the function allocated, and `hashtable_close`
frees that memory before the frame ends.

### What the corpus taught about Lua

RFC 0008 attributed Lua's 383 `use-after-free` in `luaV_execute` to
"interior pointers into `L->stack` that every protected call may
reallocate", and this RFC set out to fix them with derived pointers. Read
with the notes on, they are not that. 274 say `freed here (through
'L->l_G->twups->ci->top.p')` at one `luaF_newtbcupval` call: a callee
that runs the garbage collector, which frees the stacks of *dead threads*
reached from the global state's thread list; `g->twups` may alias the
running thread `L` (the collector links it there when it has open
upvalues); so the stack the collector frees may, as far as the alias
relation knows, be the one `base` and `ra` point into. The invariant that
makes the code correct — the running thread is never collected — is not
one a static analysis proves, and the remaining ~100 rest on a different
invariant (the callee sets `ci->u.l.trap` whenever it moves the stack,
and the interpreter re-derives `base` when `trap` is set) that would need
consumes guarded by facts the callee *establishes*, not facts it
requires. Neither is a derived-pointer problem. This RFC corrects the
record (in *Accepted false positives* and in the corpus README) and names
the second as future work; it does not claim Lua's VM loop.

### Why now, and why together

Derived pointers and extents are one feature seen from two sides: an
extent is useless without knowing where in the object a pointer points,
and an offset is only half a spatial record. Building the offset first,
as the replacement for the `interior` bit, fixes the false negative and
the false positives above on its own; adding the extent on the same
record gives bounds checking with no second representation of "where does
this pointer point". Doing them in two RFCs would mean designing the
offset twice.

## Soundness

### Bugs caught

New (spatial):

```c
char *p = malloc(n);
p[n] = 0;                              /* out-of-bounds: one past 'n' */

char buf[16];
memcpy(buf, src, 32);                  /* out-of-bounds: 32 > 16 */
strcpy(buf, "a string longer than 15"); /* out-of-bounds */
fgets(buf, 64, f);                     /* out-of-bounds */

int *a = calloc(n, sizeof *a);
for (i = 0; i < n; i++) a[i] = 0;
a[i] = 0;                              /* out-of-bounds: i >= n here */

for (i = 0; i <= n; i++) a[i] = 0;     /* out-of-bounds (may): i may equal n */

char *q = p + 8;                       /* offset +8 into an 8-byte object */
*q = 0;                                /* out-of-bounds */
q[-9] = 0;                             /* out-of-bounds: before the start */

void fill(char *WEAVEC_SIZED_BY(n) buf, size_t n);
char small[4];
fill(small, 8);                        /* out-of-bounds at the call */

static void put7(char *b) { b[7] = 0; } /* requires 8 bytes behind param 0 */
put7(small);                           /* out-of-bounds at the call */
```

New (temporal, through derived pointers):

```c
static void release_inner(struct outer *o) { struct inner *i = &o->in; free(i->buf); }
release_inner(o);
use(o->in.buf);                        /* use-after-free, now reported */
```

Unchanged: everything RFCs 0002–0010 catch. `free(p + 4)` stays
`invalid-release`, now with the offset known; `free(container_of(i, T,
f))` where `i` was derived at `+f` is *not* one.

### Bugs deliberately not caught

- **Accesses the facts leave open.** `p[i]` with nothing known about `i`,
  or `i < m` for an `m` unrelated to the extent, is not reported. This
  is the design decision of the RFC: an out-of-bounds report is made only
  when every value the facts allow is out of bounds (or, for `i <= n`
  against an extent of exactly `n`, when the boundary value is). The
  alternative — report whatever cannot be proven in bounds — is what
  Checked C and Rust do at run time and what no static checker does for
  C without drowning the user.
- **Extents the checker cannot read.** `malloc(strlen(s) + 1)` has no
  extent (`strlen(s)` is not a place); `malloc(a * b)` has none unless
  one factor is a constant; a `fresh` result of a callee whose size
  argument does not reach a parameter has none. Accesses through such
  pointers are unchecked.
- **Arithmetic the offset lattice cannot express.** Two field offsets
  (`&p->a` then `&q->b` on the result) compose to `unknown`; an element
  offset added to a field offset is `unknown`; `(char *)p + k` for a
  non-`char` `p` is `unknown` unless `k` is an `offsetof`. `unknown`
  keeps every guarantee the old `interior` bit gave and none more.
- **Reads of one past the end for comparison only.** `p + n` as a value
  (a bound, never dereferenced) is legal C and is not reported;
  only a dereference or a library access is.
- **Integer overflow in size computations.** `malloc(n * sizeof(T))`
  records the extent as `n * sizeof(T)` bytes as a mathematical quantity;
  wrap-around is out of scope (RFC 0009, *Assumptions*).
- **Flexible array members, unions, variable-length arrays,
  `alloca`.** No extent is recorded; nothing is reported.
- **Lua's VM loop.** See *Accepted false positives*: unchanged by this
  RFC, for the reasons given in *Motivation*.

### Accepted false positives

- **`i <= n` loops that exit early.** `for (i = 0; i <= n; i++) { if
  (i == n) break; a[i] = 0; }` is reported at `a[i]` if the `break` edge
  does not reach the access on the same path as the `i == n` fact; in
  practice `if (i == n) break;` refutes `i <= n` down to `i < n` on the
  fall-through edge (the relation is narrowed like a fact), so the shape
  is accepted. A loop that exits through a flag set elsewhere is
  reported. The user rewrites the bound.
- **A borrow that outlives its object through a path the checker cannot
  see undone.** `ls->fs = &fs; unlink_through_unknown(ls);` at the end
  of the frame is reported at the store: the unlink went through code
  with no summary. `WEAVEC_UNSAFE` or a summary for the callee.
- **A `WEAVEC_SIZED_BY` requirement the caller meets through a value the
  checker cannot relate.** `fill(buf, len)` where `buf` was `malloc(len
  * 2)` is fine (`2 * len >= len`); where `buf` was `malloc(cap)` and
  `len <= cap` holds only by an invariant, nothing is known and nothing
  is reported — this is a false *negative*. The false positive is the
  dual: `malloc(cap)` with a later `cap = 0;` drops the extent (the
  extent named `cap` at the allocation, and `cap` changed), so nothing
  is reported either. No false positive arises from unreadable extents;
  the RFC has been careful that every rule reports only on facts.
- **Lua's `luaV_execute` (~380 `use-after-free`, 16 `conflicting-borrow`
  → some of each become the other).** Same family, larger count; see
  *Motivation*. The 16 `conflicting-borrow` on `s2v(L->top.p)` (a borrow
  of `(L->top.p)->val`) become uses of a derived pointer; whichever of
  them are dead at their use are reported as `use-after-free` instead, the
  rest vanish. Measured on the corpus after implementation: Lua's
  `use-after-free` went from 408 to 632 (527 of them in `lvm.c`), because
  `correctstack`'s `ci->func.p = restorestack(L, ci->func.offset)` now
  names `ci->func.p` and `ci->top.p` as pointers into `L->stack.p` (121
  of the new reports are on those two), and a `base` re-derived from
  `ci->func.p` inherits the stack's fate where before it was an untracked
  borrow; three `leak`s on `L->ci->top.p` ("overwritten without being
  released", in `adjustresults`) are the same pointers seen from the other
  side, once the call graph's one big component says a callee stored a
  fresh block there. Every one of them rests on the two invariants
  *Motivation* names; none is a derived-pointer error. The corpus README
  records the counts.

### Assumptions

- Everything RFCs 0001–0010 assume.
- The size argument of an allocation is its extent in bytes, computed
  without wrap-around; `sizeof` is the byte size Clang reports for the
  target.
- An element access `p[i]` on `T *p` accesses `sizeof(T)` bytes at byte
  offset `i * sizeof(T)` from where `p` points; a `char *` view of an
  object (`(char *)p + k`) accesses bytes.
- `offsetof(T, f)` (Clang's `OffsetOfExpr`) names the field path it
  spells; the checker does not evaluate it to a number, it matches it to
  the field path an earlier `&p->f` derived, so the two cancel
  symbolically. This is what makes `container_of` type-safe in the model
  even though it is `char *` arithmetic in the language.
- A `WEAVEC_SIZED_BY(n)` annotation is trusted, like every annotation.

## Detailed design

### Pointer offsets (Core)

```cpp
// weavec/Core/Offset.h
struct PointerOffset {
  enum class Kind : uint8_t { Zero, Elements, Field, Unknown };
  Kind kind = Kind::Zero;
  int64_t elements = 0;   // Elements: signed element count (of the pointee type)
  std::string field;      // Field: canonical field path, "struct outer .in.buf"
  bool negative = false;  // Field: `-` for container_of
  static PointerOffset zero(), unknown(), ofElements(int64_t), ofField(std::string, bool negative);
  bool isZero(), isUnknown(), isField();
  PointerOffset plus(const PointerOffset &) const;  // composition
  PointerOffset negated() const;
  bool join(const PointerOffset &other);            // differ → Unknown
  std::string toString() const;                     // "0", "+4", "-2", "+struct outer .in", "-struct outer .in", "?"
  static std::optional<PointerOffset> parse(std::string_view);
};
```

Composition: `Zero + x = x`; `Elements(a) + Elements(b) = Elements(a +
b)`; `Field(f) + Field(f, negative)` (and the reverse) `= Zero`;
everything else is `Unknown`. The field key is the spelling RFC 0010 uses
for count fields (`countKeyFor`): the canonical record type followed by
the field path.

`AliasEdge::exact` becomes `AliasEdge::offset`: the offset of this end's
value from the other end's (`a = b + offset`, so the edge stored on `b`'s
side carries the negation). `exact` remains as `offset.isZero()`.
`unite(a, b, offset, ...)` relates `a` to `b` at `offset` and to each
alias `c` of `b` at `offset + edge(b, c)`; `separateExact` drops an edge
whose offset is zero (the values are equal, `!=` refutes that) and keeps
every other (the values differ, the object need not). Two edges joined
with different offsets are `Unknown`. The invariant RFC 0006 states —
edges are may-alias, not transitively closed at joins — is unchanged.

`ResourceRecord::interior` is removed. Where the pointer points is not an
ownership fact; it lives in the spatial record below. The release rule
that used it (RFC 0008, *Invalid releases*) reads the spatial record
instead: a release through a pointer whose offset is `Elements(k ≠ 0)` or
`Field(...)` is invalid (the message now says by how much: "points 4
elements past the start of its allocation", "points to field 'in' of its
allocation"); one whose offset is `Unknown` is invalid unless the
releasing expression is itself pointer arithmetic (`free(s - k)` reaching
a start the checker lost track of: RFC 0008's rule, kept).

### Spatial records (Core)

```cpp
// weavec/Core/Spatial.h
struct Affine {                 // value = scale * place + constant, or constant alone
  std::optional<PlaceId> place;
  int64_t scale = 1;
  int64_t constant = 0;
  bool isConstant() const;
  bool join(const Affine &);    // differ → the record is dropped by the caller
};
struct SpatialRecord {
  std::optional<Affine> extent; // bytes of the object the pointer points into
  PointerOffset offset;         // where in it the pointer points
};
class SpatialTracker {          // place → SpatialRecord; the state's `spatial`
  void set(PlaceId, SpatialRecord); std::optional<SpatialRecord> recordOf(PlaceId) const;
  void forget(PlaceId); void dropExtentsOn(PlaceId integer); // the count place was written
  bool join(const SpatialTracker &);  // per place: offsets joined, extents kept only if equal
};
```

The extent is in *bytes*. `malloc(n)` is `{n, 1, 0}`; `malloc(n *
sizeof(T))` and `calloc(n, sizeof(T))` are `{n, sizeof(T), 0}`; `malloc(n
+ 1)` is `{n, 1, 1}`; `malloc(sizeof(T))` and `char buf[16]` are `{—, —,
k}`; `"abc"` is `{—, —, 4}`. `n` must be a place `tracksScalar` accepts
(RFC 0009); the place is the one *at the allocation*, so a later write to
it drops every extent that names it (`dropExtentsOn`, called where
`assignScalar`/`forgetScalar` are). Sources are recognised in the
analysis layer by a small `extentOf(sizeExpr)` reader over
`PlaceBuilder::scalarOperand`'s shapes plus `+ k`.

Copies carry the record: `q = p` copies it; `q = p + k`, `&p[i]`, `&p->f`
copy it with the offset composed (`Elements(k)`, `Elements(i)` when `i`
is a constant else `Unknown`, `Field(f)`); `p += k` and `p++` compose in
place. A fresh allocation sets `{extent, Zero}`; an array variable's
decay, `&a[i]` and `&s.f` on a variable's storage set `{sizeof(a), Zero
/ Elements(i) / Field}` for a *borrow* (the record is keyed on the
holder; the borrow itself is unchanged); a string literal sets its
length. A `WEAVEC_SIZED_BY(n)` parameter enters with `{n * sizeof(*p),
Zero}`, a `void *` one with `{n, 1, 0}`. A copy of a place with no record
has none.

### Deriving a pointer (Analysis)

RFC 0004's *Pointer identity* says `p + k` refers to `p`'s object. This
RFC extends it: **`&E`, where `E`'s place path crosses a dereference of a
pointer place `p` and below that dereference has only field and index
steps, is a copy of `p` at the offset those steps spell**, not a borrow.
`&p->f` is `p + Field(f)`; `&p[i]` is `p + Elements(i)`; `&(*p)` is `p`;
`&p->a[3].b` is `p + Field(a) … ` — a path with an index step after a
field, or two fields, is `Unknown` (composition rules above); `&p->a.b`
is `Field(a.b)` (one field path); `&p->a[0]` is `Field(a)` (the first
element is the field). **An array member decaying is the address of its
first element**: `p->payload` passed or assigned where a pointer is
expected is `&p->payload[0]`, a copy of `p` at `Field(payload)`, so that
`release(w->payload)` with a `container_of` releaser reaches `w`'s
object and `wrapped_release(w->payload)` twice is one `double-free`.
`&E` where `E` is a variable's storage (`&x`, `&s.f`, `&a[i]`, a
variable's own array decaying) stays a borrow, exactly as today: the
object is the variable, which has a lifetime; a heap object has none.

Two consequences for the checks around a derived copy. A derived copy's
*nullness* is not its base's: `&p->f` is an address inside `p`'s object,
and the dereference that formed it is where `p`'s nullness is checked
(`nullnessOf` on a `Copy` with a non-zero offset knows nothing), so
`puts(e->d_name)` after a possibly-null `readdir` is one
`null-dereference` at the arrow, not that and "`e` may be null, passed
to `puts`". And walking through the base to form a *consumed* derived
argument is not a separate use of the base: `release(&o->in)` a second
time is one `double-free`, not that and a `use-after-free` of `o` (the
lvalue is noted in `consumedDerivations`; `doRead` on it skips the
moved check on the pointer it walks through and keeps the raw and null
checks).

`ValueOrigin::interior` becomes `ValueOrigin::offset`; `asInterior`
becomes `withOffset(origin, o)`, composing onto a `Copy` (and onto each
alternative of a `Conditional`). `(char *)p ± offsetof(T, f)` produces
`Field(T.f)` with the sign; any other `(char *)p + k` on a non-`char *`
`p` is `Unknown`; `p + k` for constant `k` is `Elements(k)`; `p + i` for
a non-constant `i` is `Unknown`. `p - q` (pointer difference) is an
integer and derives nothing.

What changes downstream of a derived copy, all in `applyPointerAssign`
and its callees:

- The alias edge carries the offset (was: `!exact`).
- The resource record is shared as today (the copy holds the same
  resource); the spatial record is copied with the offset composed.
- **Mirrors translate field offsets.** `mirrors(*q)` for an alias edge
  `q = p + Field(a.b)` is `(*p).a.b`, then the access's own steps: `q->g`
  mirrors `p->a.b.g`. An edge with a negative field offset contributes no
  mirror in that direction (the mirror would be an ancestor of the
  original); `Elements`/`Unknown` edges mirror `*p` as today (`*p` is the
  element summary). This is what carries `free(i->buf)` to `o->in.buf`
  and into the summary as `param 0 *.in.buf: freed`, closing the false
  negative.
- **Returning or storing a derived copy of an owned local hands out the
  resource.** `escapeValue` already escapes a copy's source; a derived
  copy is a copy, so `return &object->json` and `push(head, &n->link)`
  (a store through a callee whose summary says `param 1: escaped` or
  stores it) escape `object` / `n`. The summary records the return as
  `fresh +struct outer .in` (a fresh allocation the caller receives at an
  offset), the caller's record is `{extent, Field(in)}`, and
  `container_of` in the caller composes back to `Zero`.
- **The loan is not created.** A derived copy of a pointer holds no loan
  on `*p`; it holds copies of `p`'s own loans (`copyHolder`, as any copy
  does), so `q = &p->f` with `p = &local` keeps `q` tied to `local`'s
  lifetime. `free(p)` while `q = &p->f` is live is no longer a
  `conflicting-borrow` ("cannot free 'p' while it is borrowed"); `q`'s
  later use is a `use-after-free` through the alias, which names the
  actual bug. `--exclusive-borrows` conflicts between `q->g = …` and a
  borrow of `p->f.g` are found through the translated mirror.
- **`&p->f` stored where liveness cannot retire it still borrows `(*p).f`**
  (caller memory, a global, an address-taken local): `free(p)` while such
  a holder exists is the conflict RFC 0006 reports. Because these loans
  are now common, the conflict check on a free or move looks only at
  loans on the released *storage* (`storageOf`: the pointer, its object,
  and the fields and elements below it without crossing another
  dereference), not at loans on objects the pointers stored in it refer
  to; what those own is released, if at all, by a consume of its own, with
  its own check. jansson's `hashtable_do_rehash` frees `hashtable->buckets`
  while `bucket->last` points at a pair the intrusive list owns, and the
  pair's borrow is no conflict with freeing the array.
- **An owned field of a local that is exactly a caller's place is the
  caller's place.** `f = fs->f; f->upvalues = grow(...); return
  &f->upvalues[n]` (Lua's `allocupvalue`) names the value `copy
  fs->f->upvalues @?`, not `fresh @?`: the block is the caller's own through
  the store, and handing it out a second time as a fresh allocation made
  every caller's `up` a `leak`. (`sourceValueOf`, before the owned-local
  fallback: the local's base has an exact alias with a stable path, and
  the field path is translated onto it.)
- **Exact aliases through a shared base are may-aliases after a join.**
  Two derived copies of one base at one offset are exact aliases of each
  other (`unite` closes the relation through the base, offsets composed).
  Where RFC 0006 already made the alias relation a may-relation once
  paths join, this rule makes such edges common: Lua's `luaV_execute` has
  `ci` and `newci` both equal to `L` at `base_ci` on some iteration, and
  at the loop head `ci ~ newci` is an exact edge that holds on that
  iteration only. RFC 0008's "exact copies hold the same nullness"
  therefore travels a `Null` (or `MaybeNull`) record along an exact edge
  **only to a copy whose own null record still equals the tested place's**
  (`setNullness`): two places that were the same value on every path here
  were kept in step by the copy rule, and two whose records drifted apart
  were not the same value on some path. `NonNull` travels along every
  exact edge as before (a wrong `NonNull` costs a missed report, never a
  false one). Without this, `if ((newci = luaD_precall(L, ra, n)) == NULL)
  updatetrap(ci);` made `ci` definitely null on the null edge.
- **A local's summary name prefers its exact alias.** `sourceValueOf` on
  a `Copy` of a local names it by the first stable path it aliases; with
  derived pointers a local commonly aliases both a caller's place outright
  (`ci = L->ci`) and the base of a derived name (`L` at `+base_ci`, from
  what `L->ci` may have held on some path). The exact alias is the value's
  name (`copy L->ci`); a derived name is the fallback (`copy L @+f`). Lua's
  `luaD_precall` returns `copy L->ci`, as it did before this RFC, not
  `copy L @+base_ci`.

### Deferred lifetime checks (Analysis)

RFC 0002's rule — a loan must outlive its holder — is kept; *when* it is
checked moves. Today `applyBorrow` and the copy arm of
`applyPointerAssign` compare lifetimes at the store and report. Now the
store records, on the loan it gives the holder, the store's location
(`Loan::location` of the copied loan is the copy site, which is also
where a conflict note should point), and the check runs where the
borrowed object dies:

- at a local's scope end (`handleLifetimeEnd`, the CFG's lifetime-end
  element; only address-taken locals can be borrowed, and those are the
  locals that get one);
- at the function's exit for every local (`checkBlockEndResources` on the
  edge into the exit block, where every resource holder is already
  checked).

At that point, for every loan on the storage of the dying local whose
holder is not itself dying and whose lifetime (`lifetimesOfPlace(holder)`,
evaluated now) does not outlive the local's: report `lifetime-too-short`
at the loan's location with the same message as today and the same
notes. A holder that was overwritten (`ls->fs = fs.prev`) holds no loan
(`reinit` drops the holder's loans); a holder that is dead (RFC 0006
liveness) holds none; a holder below a dereference of a pointer whose
object was freed holds none (`doConsume` on a pointer drops the loans
held by places below its dereference, new in this RFC: the holder is
gone with the object).

`return p` and `return &x` keep the eager check: the local dies at the
return, and the report belongs on it. Stores through a callee's summary
(`keep(&x)` storing into a global) are stores, so they are deferred and
reported at the call when `x` dies.

Every case the current lit tests pin is reported at the same location
with the same text (the location is the store's, which is what they
pin). What changes is that stores undone before the death are accepted,
and a store into a holder that is dead before the local dies is not
reported (it is never read).

### Relations (Core)

```cpp
// weavec/Core/Relation.h
enum class Relation : uint8_t { Less, LessEqual, Equal, GreaterEqual, Greater };
class RelationTracker {         // the state's `relations`
  void learn(PlaceId lhs, Relation, PlaceId rhs);   // narrows an existing relation on the pair
  std::optional<Relation> between(PlaceId lhs, PlaceId rhs) const;  // normalised to lhs < rhs order
  void learnAtMost(PlaceId, int64_t bound);           // `x <= bound`; narrows an existing bound
  std::optional<int64_t> atMost(PlaceId) const;       // the bound, or one on a place known equal
  void noteBounded(PlaceId); bool isBounded(PlaceId) const;  // compared with a constant by an ordering
  void forget(PlaceId);        // either side written
  bool join(const RelationTracker &);   // pairs on both sides; Less ∨ Equal = LessEqual, etc.; else dropped
};
```

Learnt from a condition edge `x OP y` on two integer places (both
`tracksScalar`), through the same operator flips `applyCondition` uses for
constants; from `x = y` (a scalar copy: `Equal`); narrowed by a later
edge on the same pair (`i <= n` then `i != n` is `Less`; `i <= n` then
`i == n` is `Equal`). `learnFact` on either place with a constant, and
`assignScalar`/`forgetScalar`, forget the pair. RFC 0009's `x++`
adjustment forgets it too (`i++` after `i < n` says nothing about `i` and
`n`; the next iteration's condition edge re-learns it). The join keeps a
pair only when both sides have it, as the weakest relation implied by
either. `between` looks through one `Equal` hop (`j = i; if (j < n)`
relates `i` and `n`).

**Constant upper bounds.** RFC 0009's class facts keep the *sign* of an
integer against a constant; a bounds check needs how *large* it can be.
A condition edge `x < k` (or `x <= k`, or `x >= k` / `x > k` failing)
records `x <= k - 1` (`<= k`) with `learnAtMost`, narrowed by a tighter
later bound, cleared by any write to `x`, and joined as the *larger* of
the two sides' bounds (dropped when one side has none). An unsigned
comparison with `k` in the top half of the range records nothing (a
negative signed `x` in disguise). `atMost` looks through one `Equal` hop
like `between`. Any place with a bound, or compared with a constant by an
ordering at all, is *bounded* (`isBounded`), which is what keeps such a
path's accesses out of the exported requirements (below).

### Bounds checks (Analysis)

An access needs `need` bytes past the pointer: for `p[i]` / `*(p + i)` /
`(p + i)->f` on `T *p` the affine `{i.place, i.scale * sizeof(T), (off +
i.constant + 1) * sizeof(T)}` where `off` is the spatial record's element
offset (`Zero` is 0; a `Field` or `Unknown` offset skips the check) and
`i` is read by `affineOf(indexExpr)`: a constant, or `x`, `x + k`, `x -
k`, `x * k`, through casts (RFC 0009's `scalarOperand` shapes plus a
constant addend). The extent is `have = {n, unit, k}`. The access is
reported when `need > have` on every value the facts allow:

1. both constant: `need.constant > have.constant`;
2. same place and scale: `need.constant > have.constant` (`p[n]` against
   `malloc(n)`; `p[n - 1]` is not);
3. `need.place` related to `have.place` by `>=` (or `Equal`): as 2; by
   `>`: `need.constant + need.scale > have.constant`;
4. `need.place` related by `<=`, scales equal, `need.constant >
   have.constant`: reported with *may* wording ("'a[i]' may be out of
   bounds: 'i' may equal 'n'"), because the boundary value the relation
   allows is out of bounds and the loop shape that produces `i <= n` is
   the off-by-one;
5. a constant index below zero (`need` computed with `off + i.constant <
   0`): "before the start";
6. a constant on one side against a place bounded above by a constant on
   the other (`RelationTracker::atMost`): `need.place <= U` with `have`
   constant and `need.scale * U + need.constant > have.constant` is
   reported with *may* wording ("'buf[i]' may be out of bounds: 'i' may
   be 7 in an object of 4 bytes"): the loop `for (i = 0; i < 8; i++)
   buf[i]` on `char buf[4]` reaches `i = 7`, and `if (i < 9) buf[i]` on
   eight elements allows it; `have.place <= U` with `need` constant and
   `need.constant > have.scale * U + have.constant` is out of bounds
   outright (`if (n > 4) return; p = malloc(n); p[4]`: no object this
   path allocates holds the access). `BoundsVerdict::MayReachPastEnd`
   carries `U` for the message.

Nothing else is reported. `p[i]` under `i < n` with `have = {n, unit,
0}` is in bounds and produces nothing either way; the check has no
"proved safe" output.

Library calls: `BuiltinSpec` gains a `bounds` field, a list of
`(bufferParam, lengthParam, unit)` triples: `memcpy` is `{0, 2, 1}, {1,
2, 1}`; `memset {0, 2, 1}`; `memmove`, `memcmp` like `memcpy`; `strncpy
{0, 2, 1}`, `strncat` (destination only when `strlen` is unknown: skipped);
`fgets {0, 1, 1}`; `snprintf`, `vsnprintf {0, 1, 1}`; `read`, `recv`,
`pread {1, 2, 1}`; `write`, `send {1, 2, 1}`; `fread`, `fwrite {0, 1 ×
2}` (both constant only); `memchr {0, 2, 1}`; `strlcpy`, `strlcat {0, 2,
1}`; `bzero`, `explicit_bzero {0, 1, 1}`. `strcpy(dst, "literal")` and
`strcat` with a literal use the literal's length as a constant `need`.
The length is read by `affineOf` and compared as above. A `BuiltinSpec`
whose spec cannot express the pair is patched in `buildTable`, as today.

### Extents in summaries (Core, Analysis)

`ValueSource` gains `offset` (replacing `interior`, meaningful for `Copy`
and `Fresh`) and, for `Fresh`, `extent`: an `Affine` whose place is a
summary path (`param i` or an integer path in caller memory). At a
`return` of a place with a spatial record whose extent's place has a
stable summary path, the source carries it; `xmalloc(n)` returns `fresh
extent=param 0*1+0`. The caller's `originFromSource` translates the path
back to its argument (a constant argument folds into the constant part; a
place argument through `scalarOperand`, with scale) and the receiving
place gets the record.

`FunctionSummary` gains `requiresExtent`: per parameter, the
*requirements* on its extent: a set of `{PathAffine need, PathGuard
when}`. Sources: a `WEAVEC_SIZED_BY(n)` annotation (`need = {param n,
sizeof(*p), 0}`, trivial guard); an access `p[k]` or a library call with
a constant or parameter-affine length on `param i *` when `param i` has
no spatial record of its own (the callee did not allocate it), with the
path's guard (RFC 0009, `guardHere`), and only when `param i` was not
reassigned (RFC 0003). A constant need the parameter's pointee type
already promises (`p->f`, `*p`) is not recorded. An access under an
ordering against a constant (`if (n > 4) p[n]`) is not exported: a guard
spells classes and constants, not orderings, so a caller could not be
told when it happens (`isBounded` on the index or on any place in the
guard drops it).

**Boundary requirements.** An access indexed by a *local* the path bounds
above is exported as the need at the boundary, which is the most the body
needs (`boundaryRequirement`): under `i < n` on `param n`, `{param n,
scale, c - scale}` (`for (i = 0; i < n; i++) b[i]` on `int *b` requires
`n * 4`); under `i <= n`, `{param n, scale, c}`; under a constant bound
`i <= U` (`for (i = 0; i < 8; i++) b[i]`), the constant `scale * U + c`
(`8`). An index bounded both by a parameter and by a constant (`i < n &&
i < 16`) needs the smaller of the two, which no summary spells, and
either alone would blame a caller that satisfies the other: nothing is
required.

Joins by union. At a call, each requirement is translated
(`translateGuard` prunes the guard against the arguments and the facts;
the `need` through the same argument mapping as extents) and compared
against the argument's spatial record with the rules above; a violation
is `out-of-bounds` at the call ("'put7' requires 8 bytes behind 'small',
which has 4 bytes"). Requirements propagate through wrappers as consumes
do: a callee's requirement on `param i` becomes this function's on
whatever it passed.

### Summary and sidecar format (Core, Frontend)

Version 7. `copy` and `fresh` sources spell an offset after the path when
it is not zero, as one token (`copy param 0 @+4`, `copy param 0
@+struct~outer.in` — a space in a field key is spelled `~`); `fresh`
spells an extent as `extent <path> scale <unit> plus <k>` or `extent
<k>`; an `effect` line spells the offset a consume lands at as a flag
(`freed(free),at(-struct~outer.in)`); a `requires-extent <i> <extent>
[when ...]` line per requirement. The `interior` word of version 6 is
read as offset `?` (`Unknown`) so old text still parses; version 6
sidecars are rejected as today (RFC 0005: the version is the whole
contract). Round-trip tests cover every new form. The program dump
(`--whole-program --dump-analysis`) prints each of these in the same
spelling, plus a `requires-extent{param <i>: <extent> [when ...]}` group.

### Whole-program widening (Frontend)

`ProgramAnalysis`'s cyclic-group loop (RFC 0010, *Whole-program
fixpoint*) runs `MaxRounds = 16` rounds. From round `WidenAfter = 6`
onwards a member's new exports are *joined* with its previous exports
before comparison (`FunctionSummary::join` per function, as the
recursive fixpoint within a unit already does). The joined sequence is
monotone in a finite lattice, so it converges; because `join` treats
must-facts by conjunction, widening only ever makes a summary say
*less*, which is the sound direction. A group still marked
`nonConverging` after that is a bug to look at, not a schedule accident;
the corpus README's note that Lua's report set depends on the schedule
stops being true.

### Recall check (repository)

`test/recall/` holds small C programs, one bug each, in the shape of the
Juliet test suite's CWE cases (CWE-121/122/124/126/127 for bounds,
CWE-415/416/401/476/457 for the temporal classes), each with a
`// RECALL: <diag-id>` comment on the line that must be reported (or `//
RECALL: <diag-id> @<line>` for another line), and a `good` function of
the same shape without the bug. `scripts/recall.py --weavec <bin>` runs
them, prints per-CWE recall, and exits non-zero if any pinned diagnostic
is missing or if an *error* appears on a line no case pinned (the cases
are free of other bugs by construction, so that is a false positive;
warnings — a `leak` on the path a bug diverts, Clang's own
`-Warray-bounds` — are not counted). It is the `recall` ctest after the
unit tests and before `lit`; CI runs it again by hand so the table is in
the log. It is deliberately separate from `test/Analysis`: lit pins
messages and locations exactly and is the regression suite; the recall
set asks only "was this bug class caught", is organised by bug class,
and is what the README's status table can honestly cite.

### Performance

- The offset on an alias edge is a small struct with a `std::string`
  that is empty for `Zero`/`Elements`/`Unknown`; the edge map is copied
  with every state, so `AliasEdge` grows by one string and an
  `int64_t`. Measured on Lua (the largest state), analysis time is
  expected within 10% of the RFC 0010 baseline; the number is recorded
  in the corpus README.
- `SpatialTracker` and `RelationTracker` are `std::map`s copied per
  block, both empty in most functions. `RelationTracker` is bounded by
  the number of integer-place pairs a function compares, in practice a
  handful.
- Deferred lifetime checks add one pass over `state.loans` per local
  death, which is what `expireDeadLoans` already does.
- The bounds checks run in the final pass only, per access with a
  spatial record on its base.

## Annotation surface

One macro is added to `resources/include/weavec.h`, mirrored in
`include/weavec/Analysis/Annotations.h` (`spelling::SizedByPrefix =
"weavec.sized_by."`) and `docs/annotations.md`:

```c
/**
 * On a pointer parameter: the caller passes at least `n` elements
 * (bytes for `void *`) behind it, `n` being another parameter of the
 * same function by name. The body may access that many without a
 * report; a caller passing a smaller object is reported at the call.
 */
#define WEAVEC_SIZED_BY(n) WEAVEC_ANNOTATE_("weavec.sized_by." #n)
```

Authoritative: inside the body the parameter's extent is `n` elements;
at the call the argument must have at least that many. A `sized_by`
naming a parameter that does not exist or is not an integer, or written
on anything but a pointer parameter, is `invalid-annotation`. It does not
change ownership or nullness, and it composes with every other
annotation.

## Diagnostics

One new id.

- **`out-of-bounds`**, error. Messages:
  - `'<p>[<i>]' is out of bounds: index <k> of an object of <n> bytes`
    (constant against constant; the index is spelled as written);
  - `'<p>[<i>]' is out of bounds: '<i>' is at least '<n>', the number of
    elements of '<p>'` (relation `>=` / same place);
  - `'<p>[<i>]' may be out of bounds: '<i>' may equal '<n>', the number
    of elements of '<p>'` (relation `<=`);
  - `'<p>[<i>]' is out of bounds: index <k> is before the start of
    '<p>'`;
  - `'<p>[<i>]' may be out of bounds: '<i>' may be <U> in an object of
    <n> bytes` (a constant upper bound on the index, rule 6);
  - `'<p>[<i>]' is out of bounds: index <k> of an object of '<n>' bytes`
    (a constant index against an extent bounded above, rule 6);
  - `'<f>' accesses <k> bytes of '<p>', which has <n> bytes` (library
    call, constant against constant), `'<f>' may access past the end of
    '<p>': '<len>' may be <U>, and '<p>' has <n> bytes`, and the
    relational forms with the same tails as above;
  - `'<f>' requires <k> bytes behind '<p>', which has <n> bytes` (a
    callee's requirement at the call).
  - Note: `'<p>' is allocated here` / `'<p>' is declared here` / `the
    object behind '<p>' is declared here` (the extent's origin: an
    allocation, the pointer's own `WEAVEC_SIZED_BY` declaration, or the
    storage it was pointed at).

  ```c
  char *p = malloc(n);
  p[n] = 0;   // error: 'p[n]' is out of bounds: 'n' is the number of elements of 'p'
  ```

Changed wording, same id:

- `invalid-release`: "does not point to the start of its allocation"
  gains the offset when known: `points 4 elements past the start of its
  allocation`, `points to field 'in' of its allocation`. The lit test
  pinning the old text is updated.

Unchanged ids whose *when* changes: `lifetime-too-short` (deferred; same
location and text), `conflicting-borrow` (no longer produced for
`free(p)` while `&p->f` is held; the use is `use-after-free`).

Each form has a lit test in `test/Analysis/rfc0011-*.c` pinning the
message and a unit test of the core rule that produces it.

## Drawbacks

- **Core grows two trackers and a richer edge.** `AnalysisState` goes
  from twelve components to fourteen; every one is copied per block. The
  cost is bounded (see *Performance*) but it is the largest single
  addition to the state since RFC 0006.
- **A new false-positive surface.** Every rule in *Bounds checks* is
  "definite", and the RFC has been careful to report on facts only; but
  the relational reasoning is new and real code will find shapes the
  join loses (a relation dropped at a merge is a false *negative*, not a
  positive, so the risk is one-sided). The corpus run before merging is
  the check.
- **Deferring the lifetime check moves a report the user may expect at
  the store.** The location is kept at the store; only the *decision* is
  deferred. A user who reads the report while the store is still on
  screen sees no difference.
- **Dropping the loan for `&p->f`** changes which diagnostic a
  free-while-borrowed becomes (`use-after-free` at the use instead of
  `conflicting-borrow` at the free). The new one names the actual
  problem, and RFC 0006 already made the conflict report the exception
  rather than the rule; but a user who relied on the free-site report
  will see it move.
- **Format bump.** Sidecars and summary text from before this RFC are
  rejected; the whole program is re-analysed once. This has happened at
  every RFC since 0003.
- **`offsetof` matching is symbolic.** `(char *)p - 8` where 8 happens to
  be the field's offset is `Unknown`, and freeing through it is
  reported as before. The macro every real code base uses spells
  `offsetof`, and the alternative — computing layouts — would make the
  model target-dependent.

## Alternatives

- **Do nothing on bounds; keep improving temporal precision.** The
  README would have to stop saying "memory safety". The FP shapes in
  *Motivation* would still need the offset, and the false negative
  would stay.
- **Offsets as byte numbers.** Simpler lattice, no field keys; but
  `&p->f` would need Clang's record layout at analysis time (making
  summaries target-specific) and `container_of` would still need the
  same symbolic cancellation to be robust to padding. Rejected.
- **Bounds by "cannot prove in bounds".** The sound direction, and what
  a checked dialect enforces at run time. Statically, on C that indexes
  with values from parsing and I/O, it reports on nearly every loop.
  Rejected for the default; it could be an opt-in flag later (like
  `--exclusive-borrows`).
- **A full numeric domain (intervals, octagons).** Would prove more
  accesses in bounds and find more definite ones (`i < n - 1` then
  `a[i + 1]`). The relation tracker is the smallest domain that handles
  the idioms in *Bugs caught*; an interval domain is a natural follow-up
  RFC once the corpus shows which shapes it would add.
- **Keep the eager lifetime check and special-case the linked-stack
  idiom.** Pattern-matching `x->f = &local; ...; x->f = local.f` is
  fragile, and the deferred check is what RFC 0006 did for the holder's
  side already. Deferral is the principled version.
- **Keep the loan for `&p->f` alongside the derived edge.** Two
  representations of one fact, two reports for one bug. Rejected.

## Prior art

- **Rust.** Slices carry their length; indexing is bounds-checked at run
  time and the compiler removes the check when it can prove the index
  in range, with exactly the loop-guard reasoning in *Bounds checks*
  (LLVM's `IndVarSimplify` and range analysis). `ptr::offset` is UB out
  of bounds, which is the "derived pointer stays in its object"
  assumption here. Rust has no `container_of`; `Pin`/intrusive
  collections use raw pointers. We take: the extent travels with the
  pointer; the offset composes; definite violations are errors.
- **Checked C** (`_Array_ptr<T>`, `_Nt_array_ptr<T>`, `bounds(lo, hi)`
  annotations, `_Dynamic_bounds_cast`). Bounds are declared and checked
  dynamically, with static discharge where the compiler can. Its
  `count(n)` bound on parameters is `WEAVEC_SIZED_BY(n)`; its finding
  that most real bounds are affine in another variable is why `Affine`
  is the extent representation here.
- **Cyclone.** Fat pointers (`?`) carry bounds; `@numelts(n)` on
  parameters; arithmetic on thin pointers is restricted. We take the
  parameter annotation and the observation that `char *` views need
  byte extents.
- **Clang.** `-Warray-bounds` (constant index against constant array
  size, the case 1 rule here), `-fsanitize=bounds` (dynamic), the static
  analyzer's `ArrayBoundChecker`/`ArrayBoundCheckerV2` (symbolic offsets
  against symbolic extents with a constraint manager: the design of the
  V2 checker's "compare the affine offset to the extent" is what *Bounds
  checks* rule 2–4 specialise), and `__builtin_dynamic_object_size` /
  `__counted_by` (the `-fbounds-safety` extension), whose
  `__counted_by(n)` is `WEAVEC_SIZED_BY(n)` with the same "count of
  elements, or bytes for `void *`" rule. We take the checker's
  offset-vs-extent framing and `__counted_by`'s semantics.
- **CHERI / SoftBound.** Capability and shadow-bounds schemes prove that
  "base + bound + offset" per pointer is the right runtime model; the
  spatial record is its static shadow.
- **Linux kernel `container_of`.** The idiom motivating symbolic field
  offsets; the kernel's `container_of` also `static_assert`s the type,
  which is why matching on the `offsetof` field path rather than on a
  number is faithful.
- **Juliet / SARD.** The CWE-organised test suite whose shape
  `test/recall/` follows. We do not import Juliet (size, licence, and
  its C++ and Windows-specific halves); we write our own cases in its
  form.

## Unresolved questions

- **The `i <= n` "may" rule on real code.** Whether the off-by-one loop
  guard appears often enough in *correct* code (loops that break at the
  boundary through a flag) that the rule needs an opt-out. Decided by
  the corpus.
- **Extent of `realloc`.** `q = realloc(p, m)` gives `q` the extent `m`;
  `p`'s record is gone with the move. A `realloc` that shrinks and a
  stale copy of `p` are already `use-after-free` territory. No open
  question, noted for completeness.
- **Relations across a summary.** `int find(int *a, int n)` returning
  an index `< n` on its non-negative class would let `a[find(a, n)]`
  be proven in bounds, and `-1` on failure is exactly RFC 0006's
  negative class. Not needed for any rule here (the rules never prove
  in-bounds); a natural per-outcome fact for a later RFC.
- **Whether widening should start earlier.** `WidenAfter = 6` is a
  guess: enough rounds for the common two-unit cycle to settle exactly,
  few enough that Lua finishes. Measured on the corpus.
- **`Field` offsets through unions and anonymous structs.** The field
  key uses the canonical record; anonymous members spell as their index.
  Whether `container_of` through a union member composes correctly is
  to be checked on real code (the kernel's `list_head` idiom does not
  need it).

## Future work

- **Post-condition-guarded consumes.** Lua's `trap` protocol: a callee
  that frees an object *and* sets a flag the caller tests. A consume
  guarded by a fact the callee *establishes* on the paths that consume,
  with the caller's copy of the flag (`trap = ci->u.l.trap`) carrying
  the relation. This is RFC 0009's guard machinery in the other
  direction plus this RFC's `Equal` relation for the copy; it would
  address the ~100 non-`twups` reports in `luaV_execute`.
- **An interval domain** for indices and extents (`i < n - 1`, `a[i +
  1]`), and proving accesses in bounds so an opt-in "report what cannot
  be proven" mode is usable.
- **Null-terminated string extents** (`strlen`-based bounds, Checked C's
  `_Nt_array_ptr`): `malloc(strlen(s) + 1)` as an extent relative to
  `s`'s length, and `strcpy`/`strcat` checks against it.
- **Flexible array members and `struct hack` allocations**
  (`malloc(sizeof *s + n)`): an extent for the trailing array.
- **Mirroring through `Elements` offsets when the pointee is a record**
  (`q = p + 1; q->f` naming `p[1].f`): today `*p` is the element
  summary and `q->f` mirrors `p->f`, which is sound but coarse.
- **Requirements as per-outcome facts** (see *Unresolved questions*).
