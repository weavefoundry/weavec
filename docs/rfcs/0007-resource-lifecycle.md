# RFC 0007: Resource lifecycle: leaks, release families and owned fields

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-02
- **Accepted**: 2026-09-02
- **Tracking issue**: TBD
- **Supersedes / superseded by**: supersedes the RFC 0003 rule that a
  callee's consumption *below* a consumed argument is "the callee's
  business" at the call site (*Applying a summary at a call*, step 1); the
  RFC 0004 decision that `WEAVEC_OWNED` on a field is recorded but not
  consumed (*Unresolved questions*, last row); and the RFC 0003 summary text
  format version 2 (bumped to 3). Resolves the RFC 0002 and RFC 0006
  *Future work* items *Leak reporting*, *Leak detection and release
  families* and *Owned fields*, and the RFC 0004 item *Allocator families*.

## Summary

RFC 0001 through 0006 check the *acquire* side of ownership: an owned
resource is used after it was released, released twice, or released while
borrowed. They say nothing about the *release* side: an owned resource that
is never released, released by the wrong function, or lost when the object
holding it is freed passes silently. This RFC completes the `Owned` kind's
contract, "must be released exactly once, by the matching releaser":

1. **Leaks.** Every place that holds a resource this function is
   responsible for is tracked from the point it acquires it (an allocating
   call, a `WEAVEC_OWNED` parameter) to the point it can no longer reach it
   (its last use, its scope end, the function's return, an overwrite). If no
   alias still reaches the resource and it was neither released, moved,
   stored into memory the caller can see, returned, nor handed to code the
   checker cannot follow, the new `leak` warning is reported.
2. **Release families.** Every allocator and releaser in the shipped table
   belongs to a *family* named after its releaser (`free`, `fclose`,
   `closedir`, `munmap`, `pclose`, `freeaddrinfo`, `dlclose`, `iconv_close`,
   `freelocale`). Summaries carry the family of what a function hands out
   (`fresh(fclose)`) and of what it consumes (`freed(free)`), inferred
   bottom-up through wrappers and across units, and releasing a resource
   with a function of another family is the new `mismatched-release` error.
3. **Owned fields.** `WEAVEC_OWNED` on a struct field now means what it says:
   freeing the containing object while the field still owns its resource
   leaks it. The same holds without an annotation for a field this function
   itself made owned (`b->data = malloc(n); free(b);`).
4. **A soundness fix.** A callee that frees both an argument and a path
   below it (`free(b->p); free(b);`) is applied deepest path first, so a
   caller's copy of the field is marked freed. Before, the field's free was
   skipped because the argument was consumed, and `q = b->p; box_free(b);
   q[0]` was not reported.

## Motivation

The model's central promise is that `Owned` means "released exactly once".
Six RFCs in, the checker rejects the "more than once" half and is silent on
the "zero times" half and on "by the wrong function". Probing the current
build with the idioms below shows every one passes without a diagnostic:

```c
int leak(void)          { char *p = malloc(8); p[0] = 0; return 0; }
int leak_path(int c)    { char *p = malloc(8); if (c) return -1; free(p); return 0; }
void family(void)       { FILE *f = fopen("x", "r"); if (!f) return; free(f); }
void field_leak(void)   { struct buf *b = malloc(sizeof *b); if (!b) return;
                          b->data = malloc(8); free(b); }
void overwrite(struct buf *b) { b->data = malloc(8); b->data = malloc(16); }
void owned_param(char *WEAVEC_OWNED p) { p[0] = 0; }
```

`leak_path` is the most common real bug in the kind of C WeaveC targets: an
error return in the middle of a function that has already acquired
something. `family` and `free(fopen())` are undefined behaviour. The owned
field case is how every `*_destroy` that forgets a member leaks, and the
`WEAVEC_OWNED` annotation on fields, accepted since RFC 0001, has never been
checked.

Every fact this needs is already in the state or the summary: where a value
came from (`ValueOrigin::Alloc`, `ValueSource::Fresh`), which places hold
the same value (`AliasRelation`), when a local is last used (RFC 0006
liveness), what a callee stores or returns (`stores`, `returns`) and what it
consumes (`effects`). What is missing is the release-side bookkeeping: which
places hold a resource this function must account for, which family it
belongs to, and whether it has escaped.

The same probe found the soundness bug fixed here: `char *q = b->p;
box_free(b); q[0] = 1;` where `box_free` frees `b->p` then `b` is not
reported, because `applySummary` skips every consumed path under a consumed
argument. Every destructor in C has this shape.

## Soundness

### Bugs caught

- **Leaks** (warning). An owned resource whose every holder goes out of
  reach without the resource having been released, moved, escaped or
  reached from another live place:

  ```c
  int f(int c) { char *p = malloc(8); if (c) return -1; free(p); return 0; }  /* leak at return -1 */
  void g(void) { char *p = malloc(8); p = malloc(16); free(p); }             /* leak at the overwrite */
  void h(char *WEAVEC_OWNED p) { use(p); }                                    /* leak: p is never released */
  void k(void) { strdup("x"); }                                               /* leak: result discarded */
  ```

- **Mismatched releases** (error). Releasing (or moving into a consuming
  parameter) a resource of one family with a function of another:

  ```c
  FILE *f = fopen(path, "r"); if (!f) return; free(f);        /* free vs fclose */
  char *p = malloc(8); fclose((FILE *)p);                      /* fclose vs free */
  FILE *f = fopen(...); realloc(f, 16);                        /* realloc is family free */
  static void xfree(void *p) { free(p); }  ... xfree(fopen(...)); /* through a wrapper, across units too */
  ```

- **Owned fields lost with their container** (warning):

  ```c
  struct box { char *WEAVEC_OWNED p; };
  void box_free(struct box *b) { free(b); }                    /* leaks b->p */
  void f(struct buf *b) { b->data = malloc(8); free(b); }     /* leaks b->data (inferred) */
  void g(char **a, int n) { a[0] = strdup("x"); free(a); }    /* leaks *a */
  ```

- **Newly caught after the ordering fix**: a caller's alias of a field the
  callee frees together with the object:

  ```c
  static void both(struct box *b) { free(b->p); free(b); }
  void f(struct box *b) { char *q = b->p; both(b); q[0] = 1; }  /* use-after-free */
  ```

### Bugs deliberately not caught

- **Leaks through unknown code.** A pointer passed to a call the checker
  cannot follow (a callee with no summary, a parameter the shipped table or
  an annotation says nothing about, a variadic position of a non-library
  callee, a `WEAVEC_RAW` parameter) or cast to an integer is *escaped*: the
  callee may have retained it, so its holder's death is not a leak. This is
  a false negative by design: a boundary already reports
  `annotation-required`, and a `WEAVEC_RAW` parameter is the callee's
  promise to account for it.
- **Leaks of globals.** A resource held only by a global or a `static`
  local when the function returns is not a leak; RFC 0001 makes globals
  `'static` and leak detection at program exit is not a goal. Overwriting an
  owned global *within one function* is reported (the old value is lost by
  that function).
- **Leaks in a function that does not return.** A block that ends in a
  `noreturn` call (`exit`, `abort`, `err`) is not a death point: the process
  ends, and reporting there would flag every `exit(1)` after a `malloc`.
- **The old block after a failed in-place `realloc`.** `p = realloc(p, n);
  if (!p) return -1;` loses the old block on the null edge. RFC 0006's
  pending outcome deliberately forgets what `p` held, so this is not
  reported.
- **Fields of a fresh object.** A `WEAVEC_OWNED` field of an object this
  function allocated (`b = malloc(sizeof *b)`) or received fresh from a
  callee (`b = box_new()`) owns nothing until this function stores something
  in it; `free(b)` on an error path right after allocation is not a leak of
  `b->p`. Only fields of objects that came from *outside* (a parameter, a
  global, a load) are assumed to uphold the annotation.
- **Overwriting a `WEAVEC_OWNED` field with no state fact.** `b->p = x`
  where the checker knows nothing about the current value of `b->p` is not
  reported even though the annotation says it owned something: the shape is
  every constructor and initialiser. Only a field the function itself made
  owned is checked on overwrite.
- **A callee overwriting caller memory the caller made owned.** `b->data =
  malloc(8); reset(&b);` where `reset` stores into `b->data` without
  releasing the old value is not reported: RFC 0003 reads a sub-path's
  consumption from the callee's exit state, so the far more common
  `free(b->data); b->data = NULL;` shows the caller only the store, and the
  two cannot be told apart. A summary store into a place that holds a record
  therefore escapes the old value. Reporting inside the callee would need
  `WEAVEC_OWNED` on the field and is the previous item. *Partly superseded
  by [RFC 0008](0008-pointer-validity.md), *Replaced values*: the two are
  now told apart (`written,freed,replaced` versus a plain `written`), so the
  caller's copies of the old value are checked; the overwrite itself is
  still not reported.*
- **Leaks of resources reached only through a struct passed by pointer to
  library code.** `memcpy(dst, &p, sizeof p)` copies the owned pointer `p`
  out through a read borrow of `p`'s storage; the shipped table does not say
  that `memcpy` retains what it reads, so `p`'s death is a false leak. Rare;
  a `WEAVEC_UNSAFE` region or an explicit alias (`dst->p = p`) resolves it.

### Accepted false positives

- **Merge-point conservatism** (RFC 0001) in its leak form: `char *p =
  NULL; if (c) p = malloc(8); if (!c) return; free(p);` reports a leak at
  the return, because `p` may own at the merge and is released on no path
  reaching it. Condition facts are about pointers and call results, not
  arbitrary booleans.
- **Out-parameter constructors whose error class may hold.** `if (mk(&x)
  != 0) return -1;` is clean when every error return of `mk` leaves `*out`
  null or untouched (*Per-outcome null stores*). It is a leak when some
  error return may hold: zlib's `deflateInit2_` ends in `return
  deflateReset(strm)` after storing `strm->state`, and `deflateReset`'s
  negative class cannot be told apart from the earlier guard returns, so
  `compress2`'s `if (err != Z_OK) return err;` is reported. Per-outcome
  *stores* would not help; only knowing that `deflateReset` fails solely on
  an invalid stream would.
- **Two death points for one resource.** A resource held into both arms of
  a branch that each end the function is reported once per arm (each is a
  path on which it is leaked). The alternative, one report at the join,
  would name a program point where nothing happened.
- **A declared-owned field that is conditionally null** is not a false
  positive: `if (b->p) free(b->p); free(b);` leaves a may-freed record on
  `b->p`, which counts as released. Only a field the function neither tests,
  nulls nor frees before freeing the container is reported.

### Assumptions

Unchanged from RFC 0001 through 0006, plus:

- A callee summarised from its body (in this unit or in the program) that
  records no effect on a pointer parameter does not retain it. This is what
  the summary means; it is the reason a summary from a *body* is trusted not
  to escape an argument while one from an *annotation* or the *library
  table* is not.
- The shipped table is right about which library functions retain their
  arguments. Two entries are corrected here (`putenv` keeps its string,
  `setvbuf` keeps its buffer); the rest borrow for the call only.
- A resource is null on the edge where a null test of its holder holds.
  This is the RFC 0006 outcome-test machinery read for the holder itself
  rather than for a pending callee outcome.

## Detailed design

### Release families (Core)

A *family* is a short identifier naming a releaser; the empty string means
"unknown". `PlaceEffect` gains `family` (the family of the release or of the
move target when `freed`/`moved` is set) and `ValueSource` gains `family`
(the family of a `Fresh` value). `MoveRecord` gains `family` so the exit
state can feed it into the summary. Families are plain strings in Core; the
Analysis layer never sees them as anything else.

`PlaceEffect::join` keeps the family when both sides consume with the same
one, takes the other side's when only it consumes, and clears it when they
disagree: a mismatch is reported only when both families are known and
differ, so the join can only make the checker report less.
`ValueSource::fresh(family)` values with different families are distinct set
members, so a summary that returns `malloc` or `fopen` results carries both.

`FunctionSummary` gains `returnsFresh()` (any `Fresh` alternative,
whatever its family) and `freshReturnFamily()` (the family shared by every
`Fresh` alternative, or empty). Callers that used
`returns.contains(fresh())` use these.

### Resources (Core)

`Core/Resource.h` adds `ResourceTracker`, the fourth per-place fact
alongside moves, loans and raw records:

```
struct ResourceRecord {
  ResourceOrigin origin;      // Allocated (a call), Declared (a WEAVEC_OWNED parameter)
  SourceLocation location;    // the allocating call or the declaration
  std::string family;         // "" when unknown
  bool escaped;               // handed to code the checker cannot follow
};
```

`ResourceTracker` maps `PlaceId → ResourceRecord` for the places that hold
a resource *this function is responsible for*, and keeps a set of places
known to be null. Operations: `hold(place, record)`, `recordOf(place)`,
`escape(place)` (sets the flag on an existing record), `clear(place)`,
`markNull(place)` / `isNull(place)` / `forgetNull(place)`. `join` is union
of records (both sides: this side's record, `escaped` if either side
escaped, the family cleared if the sides disagree) and *intersection* of
the null set. `AnalysisState` gains `resources`; `forget(place)` clears its
record and its null fact.

A record is not ownership *kind*: `kinds` still says a place is `Owned`
(RFC 0001), the record says *which* resource it holds, where it came from,
and whether this function still has to account for it. Kinds are joined by
the RFC 0001 lattice; records by union. A place with a record and no move
record *may* hold a live resource.

### Acquiring and losing a resource (Analysis)

**Acquire.** `applyPointerAssign` gives the destination a record when an arm
of the assigned value is an allocation (`ValueOrigin::Alloc`, now carrying
the family: `Allocated`, at the call) or a copy of a place that has one (the
record is copied, `escaped` included). `initialState` gives every parameter
annotated `WEAVEC_OWNED` a `Declared` record at its declaration. Records are
mirrored with the other facts by `mirrorSubtree` and `copyRecord`. A summary
store of a `fresh(F)` value into caller memory (`getline(&buf, ...)`,
`asprintf(&s, ...)`) acquires like a local allocation.

**Null.** On the edge of a conditional where a pointer place is tested null
(`!p`, `p == NULL`, the false edge of `p`/`p != NULL`, the assigned operand
of `(p = f()) == NULL`, any of these as the right operand of `||` or `&&`:
`if (fd == -1 || (p = malloc(n)) == NULL) return -1;`, wrapped in
`__builtin_expect(c, k)` (Lua's `l_unlikely`, glibc's `__glibc_unlikely`),
or compared with zero as a truth value: `(p == NULL) != 0` is `p == NULL`
and `(p != NULL) == 0` is `p == NULL`), the place and its exact aliases lose
their records and are marked null, and every fact below the pointer (moves,
records, aliases, loans, kinds) is dropped: there is nothing there.
Assigning a null constant marks the destination null; any other assignment
forgets the mark. This is what keeps `p = malloc(n); if (!p) return -1;`
clean.

**Escape.** `state.resources.escape(place)` is called for:

- the operand of a `return` (every arm of a conditional), and for a struct
  returned by value, every place of its storage (below: *storage*);
- a pointer value cast to an integer (`(uintptr_t)p`, RFC 0004's way out of
  the model);
- the right side of an assignment whose left side is not a place the
  builder can resolve;
- a value assigned to a `WEAVEC_RAW` destination;
- an argument of a call the checker cannot resolve at all (RFC 0003's
  boundary), and every place of the storage under an argument passed as
  `&s` to such a call;
- an argument in a position the callee's summary has *no effect for* when
  the summary came from an annotation or the shipped table (a `WEAVEC_RAW`
  parameter, `pthread_create`'s `arg`, `qsort`'s comparator's world), and an
  argument in a variadic position of any callee that is not in the table;
- the caller place of every summary `store` whose value is a `copy` or
  `interior` copy of a path, whether or not the store's destination resolves
  in the caller (`registry[key] = p` in the callee escapes the caller's `p`
  even when the registry is not visible here);
- every resource referenced from below a pointer that is being forgotten
  (reassigned, or dead at its last use) while an unrelated alias of the
  pointer still reaches its object. The alias edges below `n` (`n->prev ~
  p`) are found through `n` and go with it; the object, and the reference it
  holds, do not. Without this, a list-building loop (`item->prev = prev;
  ...; prev = item;` then `item = create()`) leaves the previous node with
  one visible reference that the next iteration's path-insensitive
  `a->child = item` appears to overwrite. Mirroring the edges onto the alias
  instead was tried and rejected: a loop's may-aliases make the alias its
  own descendant (`a->child ~ a->child->next`), and the relation grows to the
  depth limit.
- a value stored below a dereference of an *opaque* pointer, and the
  destination itself: a local that is not caller memory (no summary path),
  holds no record, no loan and no alias, and is not `Owned`; that is, one
  whose value came from code the checker could not follow (`box =
  lua_touserdata(L, 1); box->buf = malloc(n);`). The object belongs to
  whoever handed the pointer out, and a value stored into it is reachable
  from there; without this the store looks like a write into memory only
  `box` reaches, and   `box`'s death a leak. A pointer that is a parameter, a
  global, an allocation, a borrow or a copy of any of these is *not* opaque:
  the store is reported to the caller, mirrored onto the aliases or held
  by an object this function tracks;
- a value stored below a dereference of a local that *borrows caller
  memory or a global* when the destination has no summary path (`tb =
  &G(L)->strt; tb->hash = newvect` in Lua's `luaS_resize`), and the
  destination itself. The store landed in an object that outlives this
  function, but under a name the summary cannot report (RFC 0003 paths do
  not go through loans), so neither the caller nor this function's death
  points would ever see the value again: `tb`'s death cleared `tb->hash`'s
  record, and `newvect`'s death then looked like the last handle. A borrow
  of a *local* object is not this case: the value is still this function's
  to release, and its death is reported through whichever name dies last.

A summary from a *body* (this unit or the program) that records no effect on
an argument is trusted not to retain it (*Assumptions*).

**Death points.** A resource is *lost* at a program point if its holder can
no longer be reached. The holders that die at a point are:

- the locals (parameters included) that are live before the previous
  element and not live before this one (RFC 0006 liveness), checked before
  the element; a local the previous element wrote outright (`char *p =
  malloc(8);`, `p = malloc(8);`) that is not live afterwards was never live
  and dies here too, unless the same block writes it again later (then the
  overwrite check reports);
- on each edge out of a block, those live before the block's last element
  and not live *into the successor* (the successor's live-in set, not the
  block's live-out: `p` dies on the edge into `return -1` while the other
  arm still frees it). The report lands on the outermost statement of the
  successor block (the `return -1`), else on the block's terminator or last
  statement. An edge into a block that ends in a `noreturn` call is not a
  death point either (`if (!q) fatal("...")` after `p = malloc(8)`: `p` is
  dead on that edge, and the process is about to end);
- the variable of a `CFGLifetimeEnds` element, checked before its facts are
  forgotten (this is where address-taken locals die, which liveness never
  declares dead). The report lands on the statement the scope ends after
  (the `return`, or the last statement of the block), not on the
  declaration;
- on the edge into the exit block from a block that does not end in a
  `noreturn` call: every local and parameter (what liveness and lifetime
  ends missed: address-taken locals in a function with computed `goto`,
  owned parameters);
- the destination of a pointer assignment, before `reinit`, when the write
  names the whole place (an element write `a[i] = ...` does not overwrite
  the summary place `a[*]`);
- the storage below a place a callee `written` (RFC 0006, *`written`
  forgets what lies below*), before `forgetBelow`, and the storage of a
  record overwritten by `copyRecord`/`initRecord`;
- the storage below the object being freed, in `doConsume` for a `Freed`
  consume, before the move is recorded (*Owned fields*, below).

The *storage* of a place is the place itself and every descendant reachable
without crossing a `Deref` step: `s`, `s.f`, `s.f.g`, `s.a[*]` for a local
`s`; `*b`, `b->p`, `b->arr[*]` for the object `*b`. A holder below a `Deref`
(`b->p` for a dying local `b`) is not itself dying: it lives in the heap
or in the caller, and if `b` was the last handle to it, `b` is the leak.

**The check.** For each dying place `p` with a record and no move record
(any witness): the resource is lost unless `p` or some alias of it is
escaped, or some alias of `p` is not itself dying (another name still
reaches it), or some alias has a move record (released through a name the
mirroring missed). Aliases are the direct alias-relation members, which the
RFC 0002 relation keeps closed under copies, so a stale may-alias from a join
can only suppress a report. When lost, `leak` is reported once and the
records of `p` and of every dying alias are cleared, so a later death point
(the scope end after the last use, the second arm of a join) stays silent.
Clearing happens in both phases; reporting only in the final pass, so the
fixpoint is unaffected. A holder that is not itself dying but whose name
just died (`b->p` when `b` dies) has its record cleared too, except when
it is *caller memory*, below a dereference of a parameter: that record
outlives the parameter name's last use so that a `return` after it can
still say what the caller's memory holds (*Per-outcome null stores*).

**Discarded results.** A call whose value is discarded (a statement
expression, possibly cast to `void`) and whose summary returns only `Fresh`
alternatives (null aside) leaks its result: `strdup(s);`. The pre-pass that
classifies roles records the statement-level calls.

### Mismatched releases

`doConsume` takes the family of the consume. For a `Freed` or `Moved`
consume with a known family `F`, if the consumed place or any of its consume
targets has a record with a known family `G ≠ F`, `mismatched-release` is
reported at the call, and the consume proceeds (the place is dead either
way). The family of a consume is the summary effect's; the shipped table
sets it for every allocator and releaser (*The library table*); an inferred
summary carries the family of whatever its body ultimately called (`xfree`
calling `free` is `freed(free)`; a body that frees with two families on
different paths is unknown); an annotation-only `WEAVEC_OWNED` parameter is
unknown.

### Owned fields

`WEAVEC_OWNED` on a field (`getAnnotations(FieldDecl).owned`, reachable
through `declaredAnnotations(place)`) means the field owns its referent
whenever the containing object is *live and came from outside this
function*. When `free`-family code releases an object (`doConsume`, reason
`Freed`, from a shipped-table summary), every place of the storage below the
freed object that

- has a record (this function stored an owned value there), or
- is declared `WEAVEC_OWNED`, the freed pointer has no `Allocated` record
  (the object was not allocated here or handed out fresh to us; a
  `WEAVEC_OWNED` parameter's object came from outside and *is* checked),
  the field is not marked null and has no move record. The field's place is
  created for the check if the function never named it,

and that is lost by the check above (its aliases are limited to the storage
being freed) is reported as `leak` in the container form. The check runs
for releases through the library table only: a *defined* destructor that
forgets a field is reported inside the destructor, where `free(b)` is, and
an annotated opaque destructor is trusted.

An owned field this function made owned (record) is also checked on
overwrite like any place; a declared field with no record is not
(*Deliberately not caught*).

### Applying a summary: deepest paths first

`applySummary` step 1 becomes: resolve every consumed summary path (roots
and sub-paths), sort by decreasing depth, and consume in that order. The
RFC 0003 skip of sub-paths under a consumed argument is narrowed: a path
below another path the same call consumes (`b->p` under `b`) is applied
only when the caller *knows* the place — it, a mirror of it, or an alias of
it is named by the caller's source, or holds a resource, move or null
record — and is skipped when the container is already gone, so a second
`both(b)` reports `'b' is freed twice` once, not once per field. Consuming
`b->p` before `b` marks the caller's aliases of the field (`q = b->p`)
before `b`'s facts are dropped, so `q[0]` after `both(b)` is a
use-after-free as it already was when the two frees were written inline. A
place the caller never mentions has nothing to mark and would only make
its own summary repeat the callee's: without the narrowing, a recursive
destructor (`del(o->child); free(o)`) grows one `o->child->child...` level
per fixpoint iteration up to the depth limit. `written` effects under a
consumed argument are still skipped. Two paths of one summary can also name
one cell through the caller's aliases (`g->allgc` and
`g->twups->l_G->allgc` with `g->twups ~ L`, in Lua's
`luaC_freeallobjects`); a path whose place was already marked by an earlier
path of the same call is the same release, not a `double-free`.

A summary store of a `copy` of a path is translated as RFC 0003 says, with
RFC 0006's exception (a copy of a path the callee *moved* is the resource
itself, `t->array = resizearray(...)`) and one more: a copy of a path the
callee *frees on some outcome class only* is not tracked at all (an opaque
value). zlib's `gz_look` stores `state->x.next = state->out` and frees
`state->out` on its error path; the copy is not a resource of its own (as
`fresh` it would be, and overwriting it later would be a false leak) and
not a dangling pointer either (as an alias it would be marked freed with
`state->out`, and the caller's `== -1` test cannot retract the negative
class). Rejected: applying such copy stores before the consumption so the
alias is marked and reinstated with the path; it is only reinstated when
the test narrows the classes exactly, which `== -1` does not.

### Per-outcome null stores

RFC 0006's outcome classes carry consumption only; the out-parameter
constructor tested by status code (`if (make(&x) != 0) return -1;` with
`*out = malloc(n)` inside `make`) was listed as an accepted false positive
in the draft of this RFC. The corpus (zlib's `deflateInit`, `inflateInit`
and `gz_look`, each tested by status code in every caller) showed the shape
often enough to resolve it here, in the narrow form the leak check needs.

`FunctionSummary` gains `nullOn`: per outcome class, the set of
caller-visible paths that on **every** `return` of that class hold null or
hold **nothing this function stored there**. At each `return`,
`handleReturn` records for the class(es) it may fall in the caller-visible
places marked null (`state.resources.nullPlaces()`, plus the tested place
of a `return *out != NULL`), and the caller-visible places that *hold a
resource record*; per class the null set is intersected and the held set
united across returns. At `finalizeSummary`, a class's `nullOn` is its null
set plus every destination of a `fresh` store that is held at none of the
class's returns: `int init(struct strm *s) { if (s == NULL) return -2; ...;
s->state = malloc(n); if (!s->state) return -4; ...; return 0; }` has
`null{s->state}` on `negative` (on the guard return the store has not
happened; on the allocation-failure return the place is null) and nothing
on `zero`. A return reached with the parameter itself null needs no special
case: nothing is held below it there. A class present in `nullOn` is also a
key of `outcomes`, so a summary with null facts and no conditional
consumption still keeps its classes.

At a call, `notePendingOutcome` copies the resolved places into
`PendingOutcome::nullOn`; the pending entry is kept while any class has
something to retract *or* a null fact to apply, so `err = init(&s); if (err
!= 0) return err;` works through the variable as well as on the call. On
an edge that selects classes, `PendingOutcome::nullInAll` returns the places
null in every selected class and `markNullWithCopies` marks them (and their
exact copies) null, clearing their records: on the failure edge the caller
holds nothing, so the `return` is not a leak; on the success edge the store
stands.

The text format gains `null <class> <path>` lines (version 3, together with
the families above); `SummaryIO` round-trips them and the program dump
prints `null{...}` after each class. Joining two summaries (`join`) keeps,
for a class both may return, the paths both agree on; for a class only one
returns, that side's.

What this is not: it is not per-outcome *stores*. The store itself is still
unconditional in the summary (`stores{s->state = fresh(free)}`); the class
fact only says where the store did not take effect. A wrong claim in
`nullOn` would hide a leak, so the fact is a must-fact (every return of the
class) and the relaxation to "or holds nothing this function stored" is
exactly what a caller can act on: it retracts the record the store gave,
and nothing else. The result `Negative` class remains coarse (RFC 0006):
`if (init(&s) == -1)` on a callee that also returns `-2` leaves the
negative class selected on both edges, and `deflateInit2_`'s `return
deflateReset(strm)`, whose negative class holds `strm->state`, keeps
`compress2`'s `if (err != Z_OK) return err;` an accepted false positive.

### Summary text format

`SummaryFormatVersion` becomes 3, `SidecarFormatVersion` becomes 3. Flags
gain an optional family in parentheses, `freed(free)`, `moved(free)`, and
sources gain `fresh(fclose)`; the bare spellings remain valid and mean
"unknown family". Nothing else changes. A version-2 sidecar is rejected by
its header as before and the object re-analysed.

### The library table

Each `'F'`, `'f'` and `'m'` entry gets a family, keyed by its releaser:
`free` (`malloc`, `calloc`, `realloc`, `reallocarray`, `aligned_alloc`,
`strdup`, `strndup`, `tempnam`, `backtrace_symbols`, and the stores of
`getline`, `getdelim`, `asprintf`, `vasprintf`, `posix_memalign`,
`scandir`), `fclose` (`fopen`, `fdopen`, `tmpfile`, `fmemopen`,
`open_memstream`), `pclose` (`popen`), `closedir` (`opendir`,
`fdopendir`), `munmap` (`mmap`), `freeaddrinfo` (`getaddrinfo`'s store),
`dlclose` (`dlopen`), `iconv_close` (`iconv_open`), `freelocale`
(`newlocale`, whose base-locale argument it moves). `getline` and `getdelim`
additionally *move* `*lineptr` (`param 0 * moved(free)`) before storing the
fresh buffer, since they reallocate the buffer they are given: `while
(getline(&line, &n, f) != -1)` is not a leak per iteration. `putenv`'s
string and `setvbuf`'s buffer are retained by the library and become `'.'`
(no effect, hence escaped).

### Inference

`sourceOf` returns `fresh(F)` for an allocation and for a copy of an owned
local whose record has family `F`; an owned local that has *escaped* is
returned as `unknown` rather than `fresh`: a caller must not be told it
owns something the callee also handed to code nobody can see.
`recordConsume`, `consumptionAt` and `finalizeSummary` carry the family of
each move record into the effects and outcome classes. Nothing else in the
summary changes; a reader that ignores families is exactly as sound as
before.

### `--dump-analysis`

The exit state gains `owned{p@3:14 allocated free, q@5:3 declared fclose
escaped}` (origin, family, `escaped` when set); summary effects and sources
print their family (`n: freed(free)`, `returns{fresh(free)}`), in the
per-function dump, the program dump and the sidecar alike, and the null
facts of a class follow it (`outcome zero{} null{*out}`).

### Interaction with existing RFCs

- RFC 0001: `Owned` gains its release-side obligations; the lattice and the
  guarantee are unchanged. Globals stay `'static`.
- RFC 0002: no change to moves, loans or the alias relation; the leak
  check reads them.
- RFC 0003: the summary vocabulary gains families; step 1 of *Applying a
  summary at a call* changes order (above); a summary's silence about a
  pointer parameter is now load-bearing for leaks (*Assumptions*).
- RFC 0004: `WEAVEC_OWNED` on fields is consumed; a `WEAVEC_RAW` parameter
  and an integer cast are escapes; unsafe regions still suppress reports
  inside them only.
- RFC 0005: format and sidecar version bumps; families cross units.
- RFC 0006: the null edge of an outcome test also clears the tested place's
  record and every fact below the pointer; outcome classes gain null facts
  (*Per-outcome null stores*); the condition of the block that evaluates the
  right operand of `||`/`&&` is that operand (Clang hands back the whole
  condition), `__builtin_expect(c, k)` is `c`, and `(c) != 0` on a truth
  value is `c` — but under one of those wrappers (or `!`) an `&&`/`||`
  was computed as a value before the branch, so its operands are known
  only when the value decides them: a true `&&` or a false `||` holds
  each operand (`l_unlikely(newblock == NULL && nsize > 0)` makes
  `newblock` null on the true edge), anything else says nothing; a block ending in a `noreturn` call no longer flows into
  the exit block, so what such a path frees (`lua_close` behind `exit`)
  does not reach the summary, which describes the state after a *return*;
  the consume
  query of *Conflict rules* no longer counts a loan on an *ancestor* of
  the consumed place (`strm = &s->strm; init(&s->strm)` where `init` frees
  `s->strm.state->window`), amended in RFC 0006 itself; and a call with a
  mutably borrowed argument copies the callee's written paths below it into
  the caller's summary instead of marking the pointee itself `written`,
  which had made every caller forget everything below the argument (RFC
  0006, *What a callee wrote, its caller wrote*, amended there).

### Performance

One map per state, touched at assignments and calls; the death check is a
scan of the record map filtered by root at points where a variable dies,
which RFC 0006 already computes. The small corpus projects are within
noise. Lua (34 units as one program) takes about twice as long as before
(6 minutes → 12 on the same machine and run). The cost is not the resource
map but the precise `written` paths (RFC 0006, *What a callee wrote, its
caller wrote*): where the coarse `*L: written` folded a callee's writes into
one path, summaries now carry every written field below the state pointer
transitively — 24,000 written paths across seven units where there were
2,300 — and every call applies them. Applying them is made as cheap as the
representation allows: written paths are resolved in summary order, each
extending the previous path's chain of places rather than walking from the
root; the places they name at a call are cached until this function
interns a new place; and a callee's writes are copied into the caller's
summary once per (call, pointee) pair rather than on every visit of the
block. What remains is the enumeration itself. A scalar field's write is
inert for everything but "this callee mutates the object" (nothing lies
below `L->nci`, so nothing is forgotten), so recording it as a mark on its
object rather than as a path of its own would shrink the summaries back to
their previous size; that is a summary-format change and is left to a
later RFC.

## Annotation surface

No new macro. `WEAVEC_OWNED` on a struct field is now checked (*Owned
fields*); `weavec.h` moves to header version 0.3 for the documentation
change only. `docs/annotations.md` gains the two ids and the field rule.

## Diagnostics

Two new identifiers.

- `leak` (**warning** by default; `-Wno-weavec-leak` disables it,
  `-Werror=weavec-leak` promotes it). Forms:
  - `'<p>' is leaked` at the point `<p>` goes out of reach (a statement
    after its last use, a scope end, a `return`);
  - `'<p>' is leaked: it is overwritten without being released` at the
    assignment or the call that overwrote it;
  - `'<b>->p' is leaked when '<b>' is freed` at the release of the
    container (also `'*a' is leaked when 'a' is freed` for elements);
  - `result of '<f>' is leaked` at a discarded allocating call.

  Note: `allocated here` at the acquiring call (`(through '<q>')` is not
  used: the record travels with copies), or `'<p>' is declared WEAVEC_OWNED
  here` for a parameter or field.

- `mismatched-release` (**error**): `'<p>' is released with '<free>' but
  must be released with '<fclose>'` at the release, where both names are
  family names (the canonical releaser, even when the release went through
  a wrapper). Note: `allocated here`.

Snippets that must be reported:

```c
int leak_path(int c) { char *p = malloc(8); if (c) return -1; free(p); return 0; } /* leak at `return -1` */
void overwrite(void) { char *p = malloc(8); p = malloc(16); free(p); }             /* leak (overwritten) */
void owned_param(char *WEAVEC_OWNED p) { use(p); }                                  /* leak at the end */
void discarded(const char *s) { strdup(s); }                                        /* result leaked */
void container(struct buf *b) { b->data = malloc(8); free(b); }                     /* b->data leaked when b freed */
void declared(struct box *b) { free(b); }        /* struct box { char *WEAVEC_OWNED p; }: b->p leaked */
void family(const char *path) { FILE *f = fopen(path, "r"); if (!f) return; free(f); } /* mismatched-release */
```

Snippets that must be clean:

```c
int checked(void) { char *p = malloc(8); if (!p) return -1; free(p); return 0; }
char *handed_out(void) { char *p = malloc(8); return p; }
void stored(struct buf *b) { b->data = malloc(8); }               /* caller memory */
void kept(char *p) { static char *keep; keep = p; }
void to_unknown(void) { char *p = malloc(8); register_thing(p); }  /* unknown callee: escaped */
void lines(FILE *f) { char *l = NULL; size_t n = 0; while (getline(&l, &n, f) != -1) {} free(l); }
void loop(char **a, int n) { for (int i = 0; i < n; i++) free(a[i]); free(a); }
void nulled(struct box *b) { free(b->p); b->p = NULL; free(b); }
void maybe(struct box *b) { if (b->p) free(b->p); free(b); }
void wrapper(char *p) { xfree(p); }                               /* xfree calls free: family free */
```

## Drawbacks

- Another per-place component in the state, and another string in two
  summary records. Both are small; the family strings are interned by the
  compiler's string pool in practice and a handful of distinct values.
- Leak reports depend on the *absence* of facts (no alias, no escape),
  which is the first time the checker reports on what it does not see. The
  escape rules are conservative by construction, and the id is a warning,
  but a new kind of false positive (a retention the model does not
  understand) is possible on code it has not seen; the corpus is the check.
- The format bump invalidates every existing sidecar; `weavec-cc` users
  rebuild. Acceptable pre-1.0.
- Two rules for owned fields (recorded vs declared) with different triggers
  is more to explain than one. The asymmetry follows from what the checker
  can know: a record is a fact, an annotation is a promise about objects it
  did not build.

## Alternatives

- **Rust's rule: every owned value is dropped at scope end.** Rust does not
  report leaks at all (`mem::forget` is safe); it *prevents* them by
  inserting drops. C has no drops; reporting is the only option.
- **Track resources by allocation site rather than by place.** Cleaner for
  "is this resource still reachable", but it turns the model into a
  points-to analysis over symbolic heap objects, which RFC 0001 rejected
  for predictability. Places plus the alias relation give the same answer
  for the code the checker already handles and keep every report
  attributable to a name in the source.
- **Report leaks only at function exit.** Simpler than the death points
  above, but the report lands on a `}` rather than on the return or
  overwrite that lost the value, and overwrite leaks (`p = malloc();
  p = malloc();`) would be missed entirely.
- **Make `leak` an error.** A leak is not a memory-safety error in the
  sense of RFC 0001's guarantee (nothing is read or written wrongly), the
  merge conservatism means some correct code is reported, and migration
  needs a build that does not stop. Warning by default, promotable.
- **Families as a lattice element instead of a string.** A closed enum
  would be cheaper to compare but cannot name user allocators; strings
  leave room for a later annotation (`WEAVEC_OWNED_BY(pool)`) without a
  format change.
- **Type-level field ownership inference** (a field is owned in every
  function if any function stores a fresh value into it). Powerful, but a
  single constructor would make every consumer of the struct responsible,
  including code that only borrows it. Rejected in favour of annotations
  and per-function facts; a later RFC can add it with the program view.
- **Do nothing.** Ship a checker that enforces "not twice" but never
  "exactly once".

## Prior art

- **Clang static analyzer, `MallocChecker`**: reports leaks when the last
  symbolic reference to an allocated region dies, suppresses them on
  `noreturn` paths and when the pointer escapes to unknown code, and
  distinguishes allocator families (`malloc`/`free`, `new`/`delete`,
  `fopen`/`fclose` in `StreamChecker`). The escape rules here follow its
  `checkPointerEscape` classification closely; the difference is that
  WeaveC's summaries let a body's silence be trusted where the analyzer
  must be conservative.
- **Infer (biabduction / Pulse)**: `RESOURCE_LEAK` is reported when an
  allocated abstract address is unreachable from the post-condition;
  summaries record what a callee allocates and releases. The per-function
  "responsible for" set here is that idea over places.
- **Rust `Drop`, `Box`**: the shape of "exactly once" that `Owned` is named
  after; not directly applicable (no destructors in C).
- **Coverity `RESOURCE_LEAK`, `USE_AFTER_FREE` with allocator
  models**: family mismatch (`free` of `new[]`) as a distinct defect class
  dates from these tools.
- **The C++ Core Guidelines lifetime profile**: `owner<T>` must be
  `delete`d exactly once on every path; the profile's experience that
  *conditional* ownership at merges is the main source of noise informs
  the warning severity here.

## Unresolved questions

- **Per-outcome stores.** *Per-outcome null stores* records where a store
  did *not* take effect; a full per-class record of every caller-visible
  destination's source (which of several stores holds on which class) is
  still open. Nothing in the corpus needs it yet.
- **A family annotation** (`WEAVEC_OWNED_BY("pool")`) for allocators the
  table does not know and whose bodies are not visible. Families are
  strings so this needs no format change; deferred until a user asks.
- **Whether `main` should be exempt.** Many programs leak deliberately at
  exit. This RFC does not exempt it (`-Wno-weavec-leak` exists); the
  corpus decides.
- **Leaks inside unsafe regions.** A resource acquired inside a
  `WEAVEC_UNSAFE` block and lost outside it is reported outside; one lost
  inside is not. Consistent with RFC 0004, but whether users expect the
  region to also mean "I know this leaks" is open.

## Future work

- **Per-outcome stores** (above).
- **Nullability** (a later RFC): `null-dereference` on the untested result
  of an allocator, using the null facts this RFC starts tracking for
  leaks.
- **Program-level field ownership**: infer that a field is owned from the
  program's constructors and destructors together, once the whole-program
  view can attribute a struct type to its owners.
- **Corpus growth**: the acceptance criterion for this RFC is a triaged
  baseline on the ten tracked projects with every `leak` and
  `mismatched-release` report explained.
