# RFC 0010: Shared ownership: reference counts, ownership by outcome and per-outcome facts

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-04
- **Accepted**: 2026-09-04
- **Tracking issue**: TBD
- **Supersedes / superseded by**: amends the RFC 0001 rule that every
  owned resource has exactly one owner (*Ownership kinds*) by adding
  *shares*; amends RFC 0002's alias relation (an edge now records whether
  its ends hold the same share); amends RFC 0007's leak rule for a holder
  that took a reference; resolves the RFC 0007 *Unresolved questions* items
  *Reference counting* and *Family annotations* and its *Future work* item
  *Per-outcome stores*; resolves the RFC 0009 *Future work* item *Facts
  about call results kept as scalars* for integer memory the callee writes.
  Bumps the summary text format (RFC 0003) and the sidecar format (RFC
  0005) from 5 to 6.

## Summary

Nine RFCs in, WeaveC's model has one owner per resource. C does not: the
two most common ways real programs share an object, a *reference count* in
the object and a *container that takes ownership only when it succeeds*,
are both read by the checker as ordinary frees and ordinary stores, and
both produce hard errors (`use-after-free`, `double-free`) on correct
code. This RFC adds the two idioms to the model and the summary vocabulary
so that they are inferred from bodies, as everything else is, and can be
declared for code the checker cannot see:

1. **Shares.** An owned resource may be held through several *shares*, one
   per reference the program has counted. A function that increments an
   integer field of the object its parameter points to (`o->rc++`,
   `__atomic_fetch_add(&o->rc, 1, ...)`) *retains* that argument: the
   caller gains a share. A function that decrements such a field and
   releases the object on the paths where the count reached zero (`if
   (--o->rc == 0) free(o)`) *releases a share* of its argument: the
   caller's name for the object is dead, but every other share, and the
   object, live on. Shares are tracked per place, propagate through
   wrappers and across units, leak like any other resource, and the
   *inline* forms (`Py_INCREF`/`Py_DECREF`-style macros expanded into the
   caller) are recognised by the same rules.
2. **Per-outcome stores.** A summary records, per outcome class of the
   result, which stores into caller-visible memory happened on every path
   returning it. A caller that tests the result retracts the stores that
   did not happen on the selected classes: after `if (bag_put(&g, s) != 0)
   free(s);` the bag does not alias `s`, and the later `free(g.slots[i])`
   is not a second free.
3. **Per-outcome integer facts.** A summary records, per outcome class,
   what the callee left in the integer memory it wrote: `int
   dec_and_test(int *r) { return --*r == 0; }` has `*r` equal to zero
   exactly on its positive class. This is what lets the release-a-share
   rule see through the helper every reference-counting library wraps its
   decrement in.
4. **Annotations.** `WEAVEC_RETAINS` and `WEAVEC_RELEASES` on a pointer
   parameter declare the two effects for code with no body (a library's
   `g_object_ref`/`g_object_unref`); `WEAVEC_REFCOUNT` on an integer field
   declares it a reference count so a leaked share is reported even when
   the release function is out of view; and `WEAVEC_OWNED_BY(f)` names the
   release family of a `WEAVEC_OWNED` parameter or result, closing RFC
   0007's open item.
5. **Stores out of sight.** A callee that keeps a copy of its argument in
   memory its summary cannot name (a node it allocated and linked into the
   caller's container) says so with an `escaped` effect, so the caller
   neither reports the argument leaked nor the share it just took lost.
   Without it, the share rule turns every container insert into a leak.

A bounded engineering change rides along: the whole-program fixpoint (RFC
0005) re-analyses a member of a cyclic group only when the exports it
imports changed since its last run, instead of every member every round.

## Motivation

Probing the current build with the two idioms:

```c
struct obj { int rc; char *name; };
static struct obj *obj_ref(struct obj *o) { o->rc++; return o; }
static void obj_unref(struct obj *o) {
  if (--o->rc == 0) { free(o->name); free(o); }
}
int use_refcount(void) {
  struct obj *a = obj_new("x");
  if (!a) return -1;
  struct obj *b = obj_ref(a);   /* two holders */
  obj_unref(a);
  int len = (int)strlen(b->name); /* use-after-free reported: wrong */
  obj_unref(b);                   /* double-free reported: wrong */
  return len;
}

struct bag { char *slots[8]; int n; };
static int bag_put(struct bag *g, char *s) {
  if (g->n == 8) return -1;
  g->slots[g->n++] = s;           /* takes ownership on success only */
  return 0;
}
void bag_use(void) {
  struct bag g = { {0}, 0 };
  for (int i = 0; i < 10; i++) {
    char *s = malloc(4);
    if (!s) break;
    if (bag_put(&g, s) != 0) { free(s); break; }
  }
  bag_free(&g);                   /* double-free reported: wrong */
}
```

Both reports are *errors*, not warnings, and both are wrong. Reference
counting is how GLib, CPython, libuv, jansson, systemd, the Linux kernel
and most C object systems share objects; `bag_put` is the shape of every
`*_append` and `*_insert` that can fail. A user who meets either on the
first file they try cannot proceed except by `WEAVEC_UNSAFE`, which gives
up the checking of exactly the code where ownership is subtle.

The corpus baseline (`scripts/corpus/README.md`) shows the same gap
through a different lens: Lua's remaining false positives are about
objects reached through several names whose relationship the checker
cannot express. Shares and per-outcome stores are the two relationships
the probes found missing.

Everything the design needs is already in the state or the summary:
integer facts on caller memory and guards on consumes (RFC 0009), the alias
relation with per-edge attributes (RFC 0006), resource records with an
origin and a family (RFC 0007), and outcome classes with per-class facts
(RFC 0006, 0007, 0008). What is missing is the vocabulary: a *share* as a
thing a place can hold more than one of, an alias edge that says "same
object, different share", and per-class stores and integer facts in the
summary.

## Soundness

### Bugs caught

- **Over-release.** A share released twice, or a name used after its share
  was released:

  ```c
  b = obj_ref(a); obj_unref(a); obj_unref(a);      /* double-free: 'a' is released twice */
  obj_unref(a); use(a->name);                      /* use-after-free through 'a' */
  q = a; obj_unref(a); use(q);                     /* q holds the same share as a */
  ```

- **Under-release (leaks of shares).** A share taken and never released,
  when the count field is known (an `unref` is in view, or the field is
  `WEAVEC_REFCOUNT`):

  ```c
  void f(struct obj *o) { obj_ref(o); }            /* leak: the reference is never released */
  a = obj_new(); obj_ref(a); obj_unref(a);         /* leak: a's own share */
  ```

- **Stores that did not happen.** The container's element is not the
  caller's pointer on the failing class, so the failing arm's `free(s)` is
  the only release and the bag's later release is its own:

  ```c
  if (bag_put(&g, s) != 0) free(s);   ... bag_free(&g);   /* clean */
  if (bag_put(&g, s) == 0) free(s);   ... bag_free(&g);   /* double-free: the bag holds s */
  ```

- **Direct frees of shared objects stay frees.** `free(a)` on an object
  another name holds a share of kills every name: shares are a discipline
  of *counted* releases, and a release that bypasses the count is what it
  always was.

### Bugs deliberately not caught

- **Counts manipulated outside recognised shapes.** `o->rc = o->rc + 1`,
  `o->rc += n` for `n != 1`, a count kept behind a pointer (`o->hdr->rc`),
  a count in a global or a local, and a decrement whose zero test does not
  guard the release (`if (o->rc > 1) { o->rc--; return; } free(o);`) are
  not recognised. Such a function is an ordinary free of its argument, as
  before this RFC; annotate it `WEAVEC_RELEASES`.
- **A release that is the last one.** `obj_unref(a)` when `a` holds the
  only share frees the object, and the checker cannot know it did: every
  *other* name for the object that holds its own share is trusted to keep
  it alive. A program whose counts are wrong at run time (a share taken
  without an increment) is outside the guarantee, as a program that frees
  through a raw pointer is.
- **Leaked shares of unknown counts.** A retained share of an object whose
  count field no analysed function releases, and which is not
  `WEAVEC_REFCOUNT`, is not reported when it dies: the checker cannot tell
  a reference count from a length field a callee incremented. The fact is
  kept (the share is real for the purpose of releases), only the leak
  report is withheld. Whole-program analysis (RFC 0005) makes the release
  visible when it is in another unit.
- **Surplus shares lost at a merge.** `if (c) obj_ref(p); obj_unref(p);`
  joins to the *smaller* share count, so the release is treated as
  consuming the caller's share (a use after it is reported) and the leaked
  share on the `c` path is not. The sound direction for use-after-release
  is chosen over the leak.
- **Stores retracted on an outcome class lose the old value.** When a
  caller retracts a store the callee did not perform on the selected
  class, the destination is *forgotten*, not restored: what it held before
  the call is unknown again. A resource it held may then leak silently.

### Accepted false positives

- **Discipline over physics.** After `obj_unref(p)` the name `p` is dead
  even when another share provably keeps the object alive (`q =
  obj_ref(p); obj_unref(p); use(p)`). This is Rust's rule for `Rc` and it
  is what makes the over-release bugs above reportable at all; code that
  relies on a sibling's share after releasing its own should use the
  sibling.
- **Copies split shares eagerly.** `obj_ref(p); q = p;` hands the new
  share to `q`; a program that meant `q` to be a plain copy and later
  releases through `p` twice is reported at the second release rather than
  accepted. The eager split is what makes `obj_ref(o); l->head = o;
  obj_unref(o);` (GLib's setter shape) clean; the alternative, deciding at
  the release which name held the share, has no basis in the source.
- **A retained parameter released through a copy.** `obj_ref(o); q = o;
  obj_unref(q); use(q)` is clean (the caller's share underlies every copy
  of a value that came from outside), but `a = obj_new(); obj_ref(a); q =
  a; obj_unref(q); use(q)` is reported: `q`'s share was the object's own,
  and `a`'s share is a sibling's. Both follow the discipline rule.
- **Per-outcome stores need an outcome test.** A container that reports
  failure through an out-parameter or a global rather than its result is
  summarised as before: the store may have happened, the caller's
  pointer is an alias of the element on every path.

### Assumptions

Unchanged from RFC 0001 through 0009, plus:

- A reference count is an integer field of the object (possibly of an
  embedded struct: `o->base.refs`) reached through a single dereference of
  the pointer whose share is being counted, adjusted by exactly one at a
  time.
- Every increment of a count field is a share taken *by the argument's
  holder*: `obj_ref(p)` gives `p` a share until some copy of `p` takes it.
- A value that came from outside the function (a parameter, a global, a
  load through one) carries a share the function does not own, which
  survives the call; releasing an owned share of it leaves the name valid.
  A value the function allocated (or received `fresh`) carries only the
  shares the function owns.
- The decrement helpers a program defines (`dec_and_test`) are analysed
  like any other function; a compiler builtin (`__atomic_*`, `__sync_*`,
  `__c11_atomic_*`) adjusts its target by its constant operand and yields
  the old or new value as its name says.
- An outcome class the callee never returns cannot be selected by the
  caller (RFC 0006); a store recorded on no class was performed only on
  paths that never return.

## Detailed design

### Shares (Core)

**Resource records.** `ResourceRecord` gains `shares` (the number of shares
the function owns through this place; `1` for every record made by RFC
0007's rules) and `countField` (an opaque key naming the count field the
shares were taken through, empty when the record is not share-counted).
`ResourceOrigin` gains `Retained`: a share taken by a count increment on a
value the function does not own. A `Retained` record whose last owned
share is released leaves its holder valid; an `Allocated` or `Declared`
record whose last share is released leaves its holder dead, as today.
`ResourceTracker::join` takes the smaller `shares` and clears `countField`
when the sides disagree.

**Alias edges.** `AliasEdge` gains `sameShare` (default `true`): the two
ends hold the same share of the object. `unite` takes the attribute for the
new edge and derives it for the edges to the other end's aliases by
conjunction (`q` shares with `r` only if it shares with `p` and `p` shares
with `r`). At a join an edge is `sameShare` if either side says so: a
release through one end is propagated to the other whenever it may be the
same share, which is the direction that reports rather than misses a use
after release. `mayAlias`, `members`, `mirrors` are unchanged: the two
ends name the same object, so facts about what lies below them are still
mirrored.

**Move records.** `MoveReason` gains `Released`: the place's share was
released (an `unref`). It reports as `use-after-free` / `double-free` with
its own messages (*Diagnostics*) and feeds the summary as a `freed` effect
with the `share` qualifier.

### Shares (summaries)

`PlaceEffect` gains `share`: the consume releases one share of the object
at the path rather than the object. Meaningful only with `freed`/`moved`;
joins as a must-fact (a plain free on one side makes the join a plain
free). Text: `effect param 0 freed(free),share`.

`FunctionSummary` gains four sets of summary paths, all joining by union:

- `increments`: integer paths the callee adds one to (`param 0 *.rc`);
- `decrements`: integer paths it subtracts one from;
- `counts`: the count paths of its share releases (`param 0 *.rc` for an
  `unref` of `param 0`), for the count-field registry below;
- and, per outcome class, `factOn`: integer paths the callee wrote and the
  fact each satisfies on every path returning the class (*Per-outcome
  integer facts*).

Text: `increment <path>`, `decrement <path>`, `count <path>`, `fact <class>
<path> <fact>` where `<fact>` is `ValueFact::toString` (`=0`, `zero`,
`positive|negative`). A retain annotation with no field is spelled with
the object path itself: `increment param 0 *`.

### Recognising increments and decrements (Analysis)

An *adjustment* of an integer place `x` is one of `++x`, `x++`, `--x`,
`x--`, `x += 1` and `x -= 1`; anything else that writes an integer forgets
its fact as before. A call to one of the adjusting builtins (`__atomic_fetch_add`, `__atomic_add_fetch`,
`__atomic_fetch_sub`, `__atomic_sub_fetch`, `__sync_fetch_and_add`,
`__sync_add_and_fetch`, `__sync_fetch_and_sub`, `__sync_sub_and_fetch`,
`__c11_atomic_fetch_add`, `__c11_atomic_fetch_sub`, and C11's
`atomic_fetch_add`/`atomic_fetch_sub` which Clang lowers to the last two)
whose first argument is `&x` and whose second is the constant `1` adjusts
`x` the same way. An adjustment of an integer place with a caller-visible
summary path is recorded in `increments`/`decrements`, and the paths a
callee's summary lists are translated to the caller's places and recorded
in turn, so wrappers compose.

`PlaceBuilder::scalarOperand` learns to read the value of an adjusting
expression as *the place plus an offset*: `--x` and `++x` are `x`; `x--` is
`x + 1` and `x++` is `x - 1` (the post-forms yield the old value); the
`fetch_*` builtins yield the old value (`x + c` for a subtraction of `c`),
the `*_fetch` and `*_and_fetch` forms the new one. A comparison `e OP k`
whose left side has offset `d` is the comparison `x OP (k - d)` of the
place, so `if (o->rc-- == 1)` learns `o->rc =0` on its true edge exactly
as `if (--o->rc == 0)` does.

A `return` of an integer comparison or truth value of an integer place
(`return --*r == 0;`, `return !--*r;`, `return *r;`) records the fact per
outcome class as `nullTestReturn` records a null test: the positive class
satisfies the comparison, the zero class its negation (*Per-outcome
integer facts*).

### Retaining (Analysis)

An increment of an integer place `c` whose nearest dereference is of the
pointer place `o` (`o->rc`, `o->base.refs`) *retains* `o`: its resource
record gains a share, or, if it has none, `o` gets a `Retained` record
with one share at the increment, `countField` set to the key of the field
(the canonical spelling of the record type and the field path: `struct obj
.rc`). The same happens at a call whose summary lists `increment param i
*...` for the argument's place, and for `increment global g *...` for a
global. Only the argument's own place is retained; its aliases are not.

**Copies split surplus shares.** In `applyPointerAssign`, a copy of a
place whose record has more shares than the place needs to stay valid (two
or more, or one when the origin is `Retained`) hands one share to the
destination: the destination gets a record with one share and the source's
origin, family, location and `countField`; the source loses one (its record
is cleared when that leaves a `Retained` record with none); the alias edge
between them is *not* `sameShare`. Every other copy is a same-share copy,
exactly as before this RFC. This one rule handles `q = obj_ref(p)`,
`obj_ref(p); l->head = p;`, `return obj_ref(p)` through a wrapper, and a
callee's `store param 0 *.head = copy param 1` after its `increment param 1
*.rc`, because a call's increments are applied before its result or its
stores are assigned.

### Releasing a share (Analysis)

`doConsume` gains the `share` qualifier. A consume is a share release when
the callee's effect says so, or when it is a `free` (directly or through a
callee) of a place `o` on a path whose facts, together with the callee's
guard, say an integer field of `*o` that this function decremented is zero
(`=0`, or a class set without `positive`): the inline `Py_DECREF` shape.
It proceeds as follows, for the place `o` and, where "moved" is said, its
`sameShare` exact aliases and mirrors only:

1. `o` has a record with more than one share: one share is released;
   nothing is moved and nothing reaches the summary.
2. `o` has a record with one share: the record is cleared (on `o` and its
   `sameShare` aliases). If its origin is `Retained`, `o` stays valid and
   nothing reaches the summary. Otherwise `o` is moved (`Released`) and the
   release is recorded for the summary (a consume of a local reaches no
   summary; of a parameter root, `freed,share`).
3. `o` has no record: it holds a share the function does not own (the
   caller's). `o` is moved (`Released`), and the consume is recorded as
   `freed,share` with the release family.

In every case the storage below `*o` is the object's business
(`releaseStorageBelow`, never the RFC 0007 container check), and the
mismatched-release check applies to the family as for any free. The
annotation mismatch check (`WEAVEC_BORROWED` parameter consumed) runs only
in case 3: releasing a share this function took itself does not touch the
caller's.

**Recognising an `unref` body.** At `finalizeSummary`, before
`dropUnstableGuards`, every consume of a parameter root `param i` (in
`effects` and in every outcome class) whose guard has a conjunct on a path
`param i *.F...` that this function decremented, with a fact whose classes
exclude `positive`, is marked `share`, the count path is added to
`counts`, and every consume of a path strictly below `param i *` carrying
the same conjunct is dropped: the object's contents go with the object,
and the caller must not learn that `o->name` is freed by a call that may
not have freed anything. The conjunct itself is then dropped by
`dropUnstableGuards` as before (the count was written).

### Leaks of shares (Analysis)

A record with `shares >= 1` whose holder dies is a leak by the RFC 0007
rule, with two amendments. A `sameShare = false` alias does not keep the
resource: `resourceLost` ignores such edges, since the alias's share is its
own. And a `Retained` record is reported only when its `countField` is a
*known count*: some function whose summary is available (inferred in this
unit, or exported by another unit of the program) lists it in `counts`, or
the field is annotated `WEAVEC_REFCOUNT`. `SummaryStore` keeps the registry
of count keys, fed by `setInferred` and by the program database;
`UnitExports` gains `countFields` (sidecar line `count-field <key>`) so the
registry crosses units. The note on such a leak is `reference taken here`
rather than `allocated here`.

### Per-outcome stores (Core and Analysis)

`FunctionSummary` gains `storesOn: map<Outcome, set<SummaryPath>>`: per
class, the store destinations written on *some* path returning it (a
may-fact per class; the map is empty when every class stores to the same
destinations, so summaries do not grow for functions whose stores are
unconditional). Text: `stored <class> <path>`. `AnalysisState` gains
`stored`, the destinations stored on the current path (joins by union),
which `recordOutcomes` reads at each `return`.

At a call, `notePendingOutcome` records per class the caller places of the
callee's stores (`PendingOutcome::storedBy`) and, for each `copy` store,
the source place and whether it was already escaped
(`PendingOutcome::sources`). When an outcome test narrows the classes, a
destination stored in none of the remaining classes is *retracted*: it is
reinitialised (its record, aliases, loans, nullness and move record go),
and the source of a retracted copy store has its `escaped` flag restored to
what it was before the call. The existing RFC 0007 `nullOn` relaxation for
`fresh` stores is unchanged and now redundant with this rule for the
`fresh` case.

### Per-outcome integer facts (Core and Analysis)

`FunctionSummary::factOn: map<Outcome, map<SummaryPath, ValueFact>>`: per
class, for integer paths in caller memory this function writes (on some
path of its body; a fact about memory it only ever reads is the caller's to
know), the fact that holds at every return of the class, whether the path
wrote it or tested it (`if (b->n == 8) return -1;` leaves `b->n =8` on the
negative class). A must-fact: the facts are joined across the returns of a
class, a path with a fact at only some returns is dropped. Text: `fact
<class> <path> <fact>`. `PendingOutcome` gains
`factOn` (per class, place and fact); when the classes are narrowed, each
place with a fact in every remaining class learns the join of those facts
(`ScalarTracker::set`, then `learnFact` so guards are refuted).

This is what makes the wrapped decrement work end to end:

```c
static int dec_and_test(int *r) { return --*r == 0; }
/* summary: decrement param 0 *; fact positive param 0 * =0; fact zero param 0 * positive|negative */
void obj_unref(struct obj *o) { if (dec_and_test(&o->rc)) obj_free(o); }
/* summary: param 0 freed(free),share; count param 0 *.rc */
```

### Stores out of sight (Analysis)

A container keeps what it is given in a node of its own:

```c
int table_set(struct table *t, struct obj *value) {
  struct pair *p = malloc(sizeof *p);
  if (!p) return -1;
  p->value = value;          /* below a local: no caller-visible path */
  p->next = t->first;
  t->first = p;              /* store t->first = fresh */
  return 0;
}
```

RFC 0007 records stores by their caller-visible destination only, so
`p->value = value` reaches no summary and `table_set`'s silence about
`value` is trusted: a caller's owned `value` is reported leaked, and the
share `table_set_shared` takes with `obj_ref(value)` before calling
`table_set` is a leak at every caller of a wrapper (the shape of every
jansson container). `PlaceEffect` therefore gains `escaped` (text
`escaped`; a may-fact, joins by disjunction): a store whose destination has
no summary path but lies below a dereference of a pointer that does not
borrow a local's storage, and whose value is a `copy` of a caller-visible
path, records `escaped` on that path (`table_set`: `value: escaped`,
`t->first: escaped`, the old head now living in the new node). A caller
applies it as it applies a `copy` store's escape (RFC 0007, *Escape*), after
the call's adjustments so that a share the call gave the argument is what
escapes, and records `escaped` on its own path for the argument when it has
one, so wrappers compose (`table_set_new`, `table_set_shared`: `value:
escaped`). An escaped `Retained` record is not a leak; nothing else about
the value changes.

A `param i` root at a call is resolved through the argument's value: the
place a `copy` names, or the one place every non-null alternative of a
conditional value copies (`table_set_new(t, obj_ref(value))` where `obj_ref`
returns `{copy param 0, null when[param 0 null]}`), so the callee's
effects, adjustments and escapes reach `value`.

The paths recorded in `increments` and `decrements` are bounded by the
place depth the summary keeps (`MaxPlaceDepth`): a count reached through a
recursive structure (`json_delete` releasing `a->table[i]`, which releases
its own `table[i]`) would otherwise grow the summary by a path per fixpoint
round.

### Annotations (Analysis)

`WEAVEC_RETAINS` on a pointer parameter contributes `increment param i *`
to the annotation summary and, when the declaration has no body of its own
to say otherwise, returns a pointer of that parameter's type and carries no
ownership annotation on the result, `return copy param i` as well (the
returning-ref shape of `g_object_ref`: the caller's `b = ref(a)` then
carries the new share, not an unknown pointer; a second `WEAVEC_RETAINS`
parameter of the result's type makes the result unknown again);
`WEAVEC_RELEASES` contributes `effect param i
freed,share` and `count param i *`; `WEAVEC_REFCOUNT` on a field makes its
key a known count for the leak rule (it says nothing else: the field is
still recognised by its adjustments); `WEAVEC_OWNED_BY(f)` sets the
release family of the `WEAVEC_OWNED` parameter or result it accompanies
(`fresh(f)`, `moved(f)`). A `WEAVEC_OWNED_BY` without `WEAVEC_OWNED`, or
`WEAVEC_RETAINS` and `WEAVEC_RELEASES` on one declaration, is
`invalid-annotation`. Like every ownership annotation, `WEAVEC_RETAINS`
and `WEAVEC_RELEASES` make a declaration checked (RFC 0003) and are
authoritative per root.

### Whole-program fixpoint (Frontend)

`ProgramAnalysis::analyzeCyclic` keeps, per member of a cyclic group, the
members it depends on (through `imports` and `indirectTypes`, as
`unitGraph` computes them) and re-runs a member in a round only when it
has never run or one of its dependencies' exports changed since its last
run. Round 0 runs every member; the reporting pass is unchanged. The
result is the same fixpoint (a member whose inputs did not change produces
the same exports) in fewer runs.

### Format changes

`SummaryFormatVersion` and `SidecarFormatVersion` go from 5 to 6. New
summary lines: `increment`, `decrement`, `count`, `stored`, `fact`; new
flags `share` and `escaped` on `effect` and `outcome` lines. New sidecar line:
`count-field <key>`. A version-5 sidecar is rejected as before (the
compile step regenerates it).

### Performance

Shares add a counter and a string to `ResourceRecord` and a bit to
`AliasEdge`; `stored` adds a set of paths to the state, populated only by
functions that store into caller memory. The registry lookup runs at
death points of `Retained` records only. Measured on the corpus with a
release build: single units within noise; the Lua program 3 min 16 s before
this RFC, 2 min 16 s with shares, per-outcome stores and the dirty-tracking
fixpoint (the same eight rounds; the last re-runs 30 of 31 units, so the
saving is in cheaper rounds more than fewer runs), and 3 min 22 s once
*Stores out of sight* is applied as well. The `escaped` effects reach many
callers on Lua (`L` is stored below almost everything), and each is
resolved at every call of the function that carries it; the profile is
`PlaceTable` lookups and `SummaryPath` comparisons under `applySummary`.
Bounding that cost, for example by resolving an `escaped` effect only when
the argument holds a tracked resource, is a follow-up (*Unresolved
questions*). The 3 min 22 s is the quietest of several runs on a machine
that was otherwise busy; treat it as an upper bound.

## Annotation surface

| Macro | Attaches to | Meaning |
| --- | --- | --- |
| `WEAVEC_RETAINS` | pointer parameter | The callee takes a reference on the argument's object: the caller's place gains a share, which the next copy of it carries away. Spelled `weavec.retains`. |
| `WEAVEC_RELEASES` | pointer parameter | The callee releases one reference: the argument's name is dead afterwards, other shares are untouched. Spelled `weavec.releases`. |
| `WEAVEC_REFCOUNT` | integer field | The field is a reference count: a share taken through it and never released is a `leak` even when no release function is in view. Spelled `weavec.refcount`. |
| `WEAVEC_OWNED_BY(f)` | pointer parameter or return type, next to `WEAVEC_OWNED` | The release family is `f` (`WEAVEC_OWNED WEAVEC_OWNED_BY(fclose)`). Spelled `weavec.family.<f>`. |

All expand to nothing on compilers without `annotate`. Inference from a
body is unchanged by their absence; on a declaration they are
authoritative for the root they name.

## Diagnostics

No new ids. Two existing ids gain messages:

- `use-after-free`: `use of '<p>' after its reference was released`, note
  `reference released here` / `reference released here (through '<q>')`.
  ```c
  obj_unref(o); use(o->name);
  ```
- `double-free`: `'<p>' is released twice`, note `previously released
  here` / `... (through '<q>')`.
  ```c
  obj_unref(o); obj_unref(o);
  ```
- `leak`: unchanged message `'<p>' is leaked`, with the note `reference
  taken here` when the record is `Retained`.
  ```c
  void f(struct obj *o) { obj_ref(o); }
  ```
- `invalid-annotation`: `WEAVEC_OWNED_BY without WEAVEC_OWNED` and
  `WEAVEC_RETAINS and WEAVEC_RELEASES on the same declaration`.

## Drawbacks

- The alias relation now carries a second attribute per edge and the
  consume rules must ask which one they propagate over; the `mirrors` /
  `targets` / `consumeTargets` distinction that RFC 0006 introduced grows
  by one case.
- The "copies split shares" rule is a heuristic about *which* name holds
  a share. It is the right answer for every shape found in GLib, jansson
  and CPython, but a program with an unusual convention will be reported
  at its second release rather than accepted.
- Recognising `unref` bodies depends on RFC 0009's facts surviving to the
  release: a body that copies the count into a local first (`int n =
  --o->rc; if (n == 0) free(o)`) works (the local's fact is a copy of the
  place's), but one that goes through a function the checker cannot see
  does not, and falls back to today's behaviour.
- Five new summary lines and a format bump; sidecars must be regenerated.

## Alternatives

- **Model the count as a value.** Track `o->rc`'s integer fact and treat
  `free(o)` as safe when it is known non-zero elsewhere. Rejected: the
  count's value at a call is rarely known (it is whatever the rest of the
  program did), and the guarantee would rest on an unknowable number.
  Shares are Rust's answer (`Rc` is a type, not a counter you reason
  about) and need no arithmetic.
- **Annotations only.** Require `WEAVEC_RETAINS`/`WEAVEC_RELEASES` on
  every ref/unref. Rejected: the point of WeaveC is inference on existing
  code; the bodies are short and follow two shapes.
- **Treat every increment of a field as a retain, every decrement as a
  release.** Rejected: length and cursor fields would create shares, and
  their deaths would be false leaks. The design retains eagerly (harmless
  without a matching release) but reports leaks only for known counts.
- **Per-outcome stores as must-facts (stored on every path of a class).**
  Rejected: the caller needs "did not happen on any path of the selected
  classes" to retract, which is the complement of a may-fact per class.
- **Restore the destination's old value on retraction.** Would need the
  state to remember pre-call values per pending outcome; the forgetting
  rule loses only leak information, never a use-after-free.

## Prior art

- **Rust `Rc<T>`/`Arc<T>`.** `Rc::clone` is a share, `drop` releases one,
  the value is freed at the last drop, and the *binding* is moved by
  `drop` whatever the count. The discipline rule and the message wording
  come from here. Rust needs no inference because the type says it; we
  infer from the two shapes C uses.
- **Clang Static Analyzer, `RetainCountChecker`.** Tracks Core Foundation
  and Objective-C retain counts symbolically per object, with annotations
  (`CF_RETURNS_RETAINED`, `ns_consumed`) for the boundaries. It models the
  count as a symbolic integer and reports leaks and over-releases per
  path; we model shares per *place* so the fact joins over paths without
  a constraint solver. Its `cf_consumed` is our `WEAVEC_RELEASES`;
  `cf_returns_retained` on a function that returns its argument is our
  `WEAVEC_RETAINS` plus `return copy param 0`.
- **GLib's `g_object_ref` / `g_object_unref`, CPython's `Py_INCREF` /
  `Py_DECREF`, jansson's `json_incref` / `json_decref`, the Linux kernel's
  `refcount_inc` / `refcount_dec_and_test`.** The four shapes the
  recognition rules were checked against: a returning ref, a void ref, a
  macro expanded inline, and a decrement wrapped in a helper that returns
  the zero test.
- **Typestate and linear types (Cyclone, Vault).** Shares are the
  "counted" relaxation of a linear resource; Vault's adoption/focus is the
  same trade (a sibling keeps the object alive, the focused name is dead
  when it is released).

## Unresolved questions

- Should a decrement not followed by a zero-guarded release (`o->rc--;` in
  a function that never frees) be recorded as a share release anyway? It
  is one, semantically, but the release family would be unknown and the
  false-leak risk on length fields is the same as for increments. Deferred
  until the corpus shows a program that needs it.
- The count-field key uses the record type's canonical spelling; two
  structurally identical anonymous records, or a count reached through a
  `void *` cast, have no key and never become known counts. Acceptable for
  now; a type-independent key (the field's declaration location) would
  need the whole-program database to carry locations.
- Whether the copy-split rule should also apply when the source has
  exactly one share and an `Allocated` origin (`a = obj_new(); q = a;`):
  today this is a same-share copy (both names die at the release), which
  is right for `tmp = a; obj_unref(tmp);` and wrong for a program that
  meant `q` to hold the initial reference and `a` to be a scratch name.
  The corpus will tell.
- The cost of `escaped` effects on Lua (*Performance*). An effect is
  applied at every call whether or not the argument holds a record that
  could be reported leaked; skipping the lookup when nothing below the
  argument is tracked, or recording `escaped` only for values that were
  owned or shared in the callee, would bound it. Needs a quiet machine to
  measure.

## Future work

- **Weak references** (`g_object_add_weak_pointer`, `Py_NewRef` vs
  borrowed references): a name that holds no share and is valid only while
  some share exists. Needs a third edge attribute.
- **Thread-safety of counts.** A non-atomic increment is a data race under
  concurrency, which RFC 0001 leaves out of scope.
- **Per-outcome facts about globals and about the callee's own result**
  beyond the class (`return n` with `n` known): the remaining half of RFC
  0009's *Facts about call results kept as scalars*.
