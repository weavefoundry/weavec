# RFC 0009: Value-conditional behaviour: scalar facts, guarded effects and inferred `noreturn`

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-03
- **Accepted**: 2026-09-03
- **Tracking issue**: TBD
- **Supersedes / superseded by**: extends the RFC 0006 *condition facts*
  from pointer tests and tests of call results to tests of integer places;
  extends the RFC 0003 summary vocabulary with `when` guards on effects,
  stores and returns and with `never-returns`; supersedes the RFC 0006 and
  RFC 0007 *Accepted false positives* entries "Merge-point conservatism"
  and the RFC 0008 entry "Tested-then-merged" in its unannotated-`noreturn`
  form.

## Summary

The checker reasons about pointers and about the *outcome* of a call (RFC
0006), but knows nothing about integers, and nothing about *why* a callee
did what it did. Three consequences show up on every real code base:

1. **A callee that never returns is not known to never return.** Lua's
   `luaG_runerror`, `luaD_throw` and every `luaL_error` wrapper end a
   path; unless the declaration carries `noreturn` the checker continues
   past the call into code the author knows is dead, and reports what it
   finds there (`lifetime-too-short`, `use-after-free`, `null-dereference`
   in the corpus, RFC 0008 *Accepted false positives*).
2. **Two tests of the same integer are two unrelated tests.**
   `if (n == 0) free(p); ... if (n != 0) use(p);` is a `use-after-free`
   today because the second test does not know the first excluded it. The
   pattern is the shape of every "conditionally owned" field and of every
   size-driven allocator (`if (nsize == 0) { free(ptr); return NULL; }
   return realloc(ptr, nsize);`, Lua's `l_alloc`).
3. **A summary cannot say "on this argument".** Once a callee's effect is
   summarised it applies to every call. `l_alloc(ud, ptr, osize, 0)` frees
   `ptr` and `l_alloc(ud, ptr, osize, 64)` does not, but both are "may free
   `ptr`", so a caller that keeps using the block after a growing
   reallocation is reported. Likewise `if (!b->noalloc) free(b->data)`
   frees on one class of callers only, and `gz_error(state, err, msg)`
   stores `msg` only when `msg` is non-null.

This RFC adds:

- **Scalar facts**: a per-place fact about an integer value (its
  *class*, `zero`, `positive` or `negative`, and, when known, its exact
  constant), refined by the same condition edges that already refine
  nullness, and joined at merges.
- **Guards**: a record in the state (a move, a held resource) and an effect
  in a summary (a consume, a store, a return alternative) may carry a
  *guard*: a conjunction of facts about places (in the state) or summary
  paths (in a summary) under which alone it holds. A guard is refuted by a
  later test or by the argument written at the call, and the record or
  effect it protects then disappears. A record's guard is the facts that
  held on the path that created it; at a join, records present on both
  sides keep what their guards agree on.
- **Inferred `noreturn`**: a function whose exit is unreachable is
  summarised as `never-returns`, transitively through wrappers and across
  units, and a call to it ends the current path like a call to a function
  declared `noreturn` does.

No new diagnostic, annotation or option. The summary text format becomes
version 5 and the sidecar version 5.

## Motivation

The corpus (`scripts/corpus/README.md`) triages Lua's remaining reports
into three groups, in decreasing order of size:

- **Unannotated `noreturn`.** Lua's `luaG_runerror`, `luaG_typeerror`,
  `luaD_throw`, `luaL_error` and `luaL_argerror` all end in `longjmp` or
  `abort` and are declared `l_noret`, which expands to nothing under some
  compilers and to `__attribute__((noreturn))` under others. When the
  attribute is absent every `if (bad) luaG_runerror(L, ...); use(p);` is
  analysed as if `use(p)` were reachable on the bad path. The same shape
  is in every project that has an error function: SQLite's `sqlite3_log`
  wrappers around `abort`, zlib's test drivers, cJSON's `fail:` helpers.
  Annotating them is not an answer at the scale of a real code base, and
  the information is already in the callee's body.
- **Correlated integer tests.** `luaH_resize` frees the old array when the
  new size is zero and `luaM_realloc_` returns the old block when the size
  is unchanged; `lua_newstate` and `luaE_freethread` guard cleanup on a
  count; zlib's `inflateEnd` guards `ZFREE(strm, state->window)` on
  `state->window != Z_NULL` (a pointer test the checker already
  understands) *and* on `state->wrap` (an integer it does not).
- **Effects that depend on an argument.** `l_alloc` and every user
  allocator of Lua's `lua_Alloc` shape; `luaM_realloc_` and its callers;
  `gz_error` in zlib; cJSON's `ensure` with a `noalloc` hook.

The three are connected. Group 1 is a fact about a *function*: its exit is
unreachable. Group 2 is a fact about a *path*: an integer has a class here.
Group 3 is group 2 crossing a call boundary: the class of a parameter
determines an effect, and the caller knows the class of the argument. The
same guard machinery serves both sides of the boundary, and `noreturn`
inference is what makes the paths through group 2 code well-formed in the
first place (the `nsize == 0` arm that frees and *then* throws).

## Soundness

### Bugs caught

Nothing new is reported. Everything the checker reports today under a
guard that is *not* refuted is still reported: a guarded move used
unconditionally is a `use-after-free`, a guarded resource whose last holder
dies is a `leak`, a guarded store into a caller's object is a `store`.

Precision gains (reports that disappear because the code is correct):

- `if (c) free(p); if (!c) use(p);` and its `switch` and `n == 0`/`n != 0`
  forms: the move of `p` is guarded by `c` non-zero and refuted on the
  `!c` edge.
- `void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) { if
  (nsize == 0) { free(ptr); return NULL; } return realloc(ptr, nsize); }`
  summarised as `effect param 1 freed(free) when param 3 zero; return null
  when param 3 zero; return fresh(free) when param 3 positive; return null
  when param 3 positive` (the last two are joined into an unguarded `return
  null` and a guarded `return fresh`). A caller `p = l_alloc(ud, p, 8,
  16)` no longer sees `p` consumed; `l_alloc(ud, p, 8, 0)` still does.
- `if (!b->noalloc) free(b->data);` in a callee, `b.noalloc = 1; ...;
  release(&b); use(b.data);` in a caller: the consume of `param 0 *.data`
  is guarded by `param 0 *.noalloc zero`, and the caller's scalar fact
  `b.noalloc = 1` refutes it.
- `if (msg != NULL) state->msg = msg;` summarised as `store param 0 *.msg =
  copy param 2 when param 2 nonnull`; `gz_error(state, err, NULL)` stores
  nothing and does not extend the lifetime of anything.
- `if (bad) die(...); use(p);` where `die` is `void die(...) { ...;
  abort(); }` or a wrapper around `longjmp`, unannotated: `die` is inferred
  `never-returns` and `use(p)` is analysed on the good path only. This
  also removes the `leak` reports for resources that "go out of reach" at
  the end of a block that is in fact dead, and the `lifetime-too-short`
  reports for loans that "outlive" a scope that is never left.

### Bugs deliberately not caught

- **Correlations the model does not track.** Two integers related by
  arithmetic (`n2 = n + 1; if (n2 == 1) ...` after `if (n == 0)`), tests
  of a value through a different alias, or relations between two
  variables (`i < n`) are not tracked; the corresponding guards stay
  unrefuted and the checker reports as it does today. This is a precision
  limit, not a hole: an unrefuted guard is the status quo.
- **Integer overflow and truncation.** A guard `param 3 zero` refers to the
  parameter's value as the callee sees it. When the argument is `(size_t)n`
  or `n * 8` the caller's fact about `n` is carried through (zero-ness and
  sign of `n` determine those of `n * 8` for a positive constant `8`, and
  a cast changes neither, absent overflow). Wrap-around and narrowing
  casts are outside the model, as they are for every other rule (RFC
  0001, *Out of scope*).
- **`noreturn` through function pointers.** A function whose every path
  ends in a call through a pointer is not inferred `never-returns`: the
  summary of an indirect call is the join of its candidates (RFC 0005),
  and the join keeps `never-returns` only when every candidate has it.
- **`noreturn` that depends on an argument.** `void check(int ok) { if
  (!ok) abort(); }` returns on some paths; it is not `never-returns`, and
  the call `check(0)` still returns in the caller. Argument-conditional
  termination is *Future work*.

### Accepted false positives

- **A guard refuted by a fact the checker never learns.** Facts are
  learnt from `==`, `!=`, `<`, `<=`, `>`, `>=` against an integer
  constant, from truthiness tests, from `switch` cases and from
  assignments of a constant or of a value with a known fact. A test of
  a computed expression (`if ((n & 1) == 0)`) or of a function's result
  compared with a variable establishes nothing, and the guard survives.
- **A guard whose place is reassigned.** Writing to the place a guard
  names drops the guard's conjunct, not the record: `if (c) free(p); c =
  0; if (c) use(p);` still reports, because after `c = 0` the move of `p`
  is unconditional as far as the checker knows. (It *is* a dead path here;
  the checker does not evaluate `if (c)` against a constant it assigned
  itself—`c = 0` does record `c` as the constant `0`, so this particular
  program is in fact clean; the general statement is about a reassignment
  from an unknown value.)
- **Loops.** Facts about a loop counter are joined to "unknown" at the
  loop header after the first iteration (the lattice has no intervals);
  guards on records created inside the loop keep the conjuncts that
  distinguish the arms of the body but lose those about the counter.

### Assumptions

- The RFC 0001 assumptions, and RFC 0004 for what a pointer's integer
  representation means.
- An integer's class is that of its mathematical value: signed wrap-around
  and unsigned wrap-around are not modelled (an unsigned value is `zero`
  or `positive`; `negative` is unreachable for it, and a fact set that
  covers both `zero` and `positive` is trivial for an unsigned place even
  though the lattice does not know it—guards therefore never *refute*
  based on the type, only on facts learnt from the program). A constant is
  read in its own type (`ULONG_MAX` is 2^64−1, not −1; `unsigned x = -1`
  is `UINT_MAX`, `positive`), and one an `int64_t` does not hold is no
  constant at all, so a test against `SIZE_MAX` decides nothing. A
  comparison is decided in its operands' common type: in an unsigned one
  of `N` bits a `negative` operand takes part as its value plus 2^N, above
  every non-negative value, and `case -1:` on an `unsigned` scrutinee
  selects `UINT_MAX`. Getting this wrong is not a precision loss but a
  soundness one: reading `ULONG_MAX` as −1 made the edge on which
  `if (index > ULONG_MAX)` fails infeasible for `index = 0` and silently
  dropped a `null-dereference` in cJSON.
- Every path through a function reaches its exit unless it ends in a call
  to a function that is `noreturn` (declared or inferred) or in a loop
  the CFG shows to be infinite. Functions that exit through `longjmp` are
  `noreturn` by Clang's builtin knowledge; `setjmp` returning twice is not
  modelled (RFC 0001, *Out of scope*).

## Detailed design

### Value facts

A **value fact** is a non-empty set of *classes* and, optionally, an exact
constant:

```
classes ⊆ {zero, positive, negative}        for an integer place
classes ⊆ {null, nonnull}                   for a pointer place
constant ∈ ℤ, present only when classes = {class of constant}
```

The classes are exactly the RFC 0006 outcome classes (`Zero`, `Positive`,
`Negative`, `Null`, `NonNull`), so a fact about a call's result and a fact
about an integer place have the same shape and the same operations:

- **join** (at a merge): the union of the classes; the constant survives
  only when both sides agree on it.
- **narrow** (on a condition edge): the intersection of the classes, the
  constant of whichever side has one, if both have one they must agree;
  an empty intersection is *refuted*.
- **disjoint** (for a guard): no class in common.

A fact whose classes are all three integer classes (or both pointer
classes) is *trivial* and is not stored. The lattice is finite and the
dataflow fixpoint terminates as before.

### Scalar facts in the state

`AnalysisState` gains a `ScalarTracker`: a map from a place to a value
fact, for integer places only (pointer facts remain in the RFC 0008
`NullTracker`; the two are read through one interface when guards are
evaluated). It is maintained by:

- **Declarations and assignments** of an integer place from an integer
  constant (`n = 0`, `int n = 3`) set the fact to that constant; from a
  place with a fact, copy the fact; from anything else, forget. Compound
  assignments and increments forget. A struct copy mirrors the facts
  below the source to the destination (RFC 0006, *Mirrors*); a pointer
  copy mirrors the facts below the pointee.
- **Condition edges** (RFC 0006, *Condition facts on CFG edges*): `x == k`,
  `x != k`, `x < k`, `x <= k`, `x > k`, `x >= k` with `k` an integer
  constant, `x` and `!x`, and each `case` of a `switch`, where `x` is an
  integer place, narrow the fact on `x` on the corresponding edge. `x < 0`
  is `negative`, `x > 0` is `positive`, `x <= 0` is `zero|negative`, `x ==
  0` is `zero`, `x != 0` is `positive|negative`, `x == k` for `k > 0` is
  `positive` with the constant `k`, `x != k` for `k ≠ 0` learns nothing
  (both classes remain possible), `x >= 1` is `positive`, `x <= -1` is
  `negative`, and so on. The `default` of a `switch` learns nothing.
- **Writes through the callee** (`written` effects, RFC 0006) forget the
  facts below the written object and on it when it is an integer; an
  unknown write to a place forgets its fact. A write through a pointer
  this function holds (`*q = 1`, `q->n = 1`) is applied to every place
  the pointer borrows (the RFC 0006 loans: `q = &local` makes `local` the
  image of `*q`), so the local's fact is replaced, not left stale.
- **Join**: intersection of the domains, join of the facts.

Integer locals and parameters are tracked, including those whose address
is taken: a callee that writes through the pointer reports `written` (or,
unknown, forgets what it reaches), and a write through a pointer this
function holds is applied to what it borrows, the same bet the RFC 0008
nullness tracker makes. Globals are not tracked (any callee may write
them, and summaries do not report integer writes).

A condition edge whose fact the state **refutes** (`int c = 0; if (c)
free(p);`) is one no path takes: the state is not propagated along it.
Only a fact about a variable's own storage is trusted that far; a fact
about memory behind a pointer (`q->n`) may be stale under an alias the
model does not know (*Bugs deliberately not caught*), so such an edge
keeps flowing and only the guards on it are refined.

### Guards

A **guard** is a conjunction of `(key, fact)` pairs, at most one per key.
In the state the keys are places (a **place guard**); in a summary they
are summary paths (a **path guard**). A guard with no conjuncts is
*trivial* and means "always".

Operations:

- **join** (the record exists on both sides of a merge): keep the keys
  present on both sides, join their facts; a joined fact that becomes
  trivial is dropped. The result is weaker than either side, which is the
  sound direction ("only when A" joined with "only when B" is "only when A
  or B", which the conjunction cannot express except by dropping the
  conjunct).
- **refine** by a fact about one key: no conjunct on the key, unchanged;
  the fact is disjoint from the conjunct, the guard is **refuted**; the
  fact implies the conjunct, the conjunct is discharged (dropped: it is
  now known to hold); otherwise the conjunct is narrowed.
- **learn** a fact about one key on the current path: disjoint from the
  conjunct on the key, the guard is **refuted**; otherwise the conjunct is
  narrowed to the fact, or added when there was none (the guard is the
  path's facts, whichever came first).
- **drop** a key: the conjunct on it is removed (the guard weakens; used
  when the key's place is overwritten).

A guard holds at most `MaxGuardConjuncts` (8) conjuncts; a fact that would
be the ninth is not recorded, which weakens the guard (the sound
direction) and bounds the cost of every operation above.

The following carry a guard:

- `MoveRecord` (a consumed place, RFC 0001/0006), `ResourceRecord` (a
  held resource, RFC 0007) and `NullRecord` (a `Null` or `MaybeNull`
  pointer, RFC 0008) in the state. A null record additionally says
  whether the pointer is *otherwise non-null*: `char *p = NULL; if (n >
  0) p = malloc(n);` leaves `p` `MaybeNull when n zero, otherwise
  non-null`, so the edge `n > 0` makes it `NonNull` rather than merely
  forgetting the null. The promise survives a join only when every side
  made it.
- `PlaceEffect` (`when`: the consume applies only under it), `ValueSource`
  (a `store` or `return` alternative applies only under it) in a summary.
- `PendingOutcome`'s per-class consumed places (RFC 0006), so that a
  guarded consume attached to an outcome class keeps its guard when the
  class is selected.
- `ValueOrigin` (Analysis): the alternative of a classified value applies
  only under it.

### Deriving guards

**On the path.** A record created in the state takes as its guard the
*path guard*: every scalar fact currently held, every definite `null`, and
every `nonnull` established by a test (`NullReason::Tested`, RFC 0008),
each on a place the state tracks. A fact learnt later on the path
(*Refuting guards in the state*) is added to every guard that has no
conjunct on its place and narrows the conjunct of every guard that has one.
Records present on both sides of a join keep what their guards agree on
(the *join* above); a record present on one side only keeps its guard,
which is exactly the facts that distinguished that side.

This is the mechanism behind `if (c) free(p); ... if (!c) use(p);`: the
free happens on the edge where `c` is `positive|negative`, so the move of
`p` is recorded `when c positive|negative`; on the other arm there is no
record; the merge keeps the one record with its guard; the edge `!c`
learns `c zero`, which is disjoint, and the move is dropped. A record
present on both sides—`free(p); if (c) {...} else {...}`—gains `c
positive|negative` on one arm and `c zero` on the other, and the join of
the two is trivial, so the move is unconditional again.

**At an event, for a summary.** When a consume, a store or a return is
recorded into this function's summary the *path guard* attached to it is:

- the record's or value's place guard, each key translated to a summary
  path (RFC 0003 *stable paths*: a parameter root not reassigned, or a
  field or dereference at most two steps below one, or a global);
  conjuncts on places without a stable path are dropped (weakening);
- **and** the facts about parameter-rooted places that hold on the current
  path: a scalar fact on a parameter or on a field one dereference below
  a pointer parameter, and a `null`/`nonnull` fact established by a
  *test* (`NullReason::Tested`, RFC 0008) on the same.

Facts established by anything other than a test (a dereference, a
`notnull` contract) are not guards: they do not distinguish this path
from another.

**Replaced values under a guard.** RFC 0008 marks a consumed path
`replaced` when no return still holds the consumed value. A path whose
exit record carries a guard (`if (b == NULL) finish(L); else append(L,
b);`, where `finish` frees `L->stack` and `append` frees and replaces it,
leaves `L->stack` gone `when b null`) was replaced, or never consumed, on
every returning path the guard does not cover. The summary's consume of
that path is then *unreplaced under the exit record's guard* (conjoined
with whatever guard the effect already had), and the replaced consume of
the other paths is not claimed. Joining the two into an unconditional,
unreplaced consume would tell a caller passing a buffer that its stack was
freed, while the store that reinitialises it keeps the guard the caller's
argument refutes; this was the largest cluster of new `double-free`
reports in Lua's `ldump.c` when guards were first introduced. The
information given up—that on the other paths the old value's other names
are dead—is a possible missed use-after-free, never a false report.

A caller applying a *replaced* consume that finds the value already gone
reports the double release once and then treats the place as holding the
callee's new value, as RFC 0008 says it does, whether or not a store
names the place in the caller's terms (a callee that reached it through an
alias of its own). Keeping the old record there made every later call
report again.

A guard in a summary speaks of the caller's memory *as it was on entry*.
When the summary is finalised, every conjunct on a path this function
wrote—an integer it assigned, a place a callee's `written` effect names,
or anything below an object it overwrote—is dropped: the path may have
held another value when the guard was formed (`n = 0; if (n == 0) free(p)`
frees unconditionally as far as the caller is concerned). Dropping only
weakens.

**At a call, for the caller.** A summary's path guard is *translated* to
the call: `param i` becomes the class of the `i`th argument if it is an
integer constant or a null constant (and the conjunct is discharged or
refuted on the spot), otherwise the caller's place that holds the argument
(through parentheses, casts and multiplication by a positive constant, see
*Assumptions*; the constant of the fact is dropped under scaling);
`param i *.f` becomes the caller's place for it as `resolveSummaryPath`
does today; `global g` becomes `g`. A conjunct whose key has no caller
place is dropped. The translated guard is then **pruned** against the
caller's current facts (scalars and nullness), which may discharge or
refute it. A refuted effect is not applied at all: no consume, no
resource, no store, no return alternative. A discharged or narrowed guard
is attached to the record the effect creates (the move for a consume, the
held resource for a `fresh` return, the origin for a copy) and lives on in
the caller's state, where a later test may still refute it.

**Return alternatives.** `originFromSource` attaches the translated guard
to each alternative; an alternative whose guard is refuted is dropped;
a conditional value with a single surviving alternative is that
alternative; a value with no surviving alternative is opaque (the callee
returned *something*; the model's knowledge was inconsistent with the
guards, which can only happen through an untracked correlation).

### Refuting guards in the state

On a condition edge, after the fact on a place is narrowed, every move,
resource and null record *learns* it: a guard with a conjunct on the place
is refined, a guard without one gains it. A refuted move record is
**reinstated** (RFC 0006, *Retraction*):
the place is initialised again, and the `consumed` entry for its summary
path is erased with the same justification RFC 0006 gives (a second
consumption of the place between the guarded consume and the test would
already have been reported). A refuted resource record is **cleared**: the
place never held the resource on this path, and no `leak` is reported for
it. A refuted null record is erased, or made `NonNull` when it was
*otherwise non-null*.

Writing a place a guard names **drops** that conjunct from every guard in
the state (the record stays, unconditional). This is the only way a guard
gets *weaker* on a single path; it keeps every report the checker makes
today.

### Inferred `noreturn`

`FunctionSummary` gains `neverReturns`. It is set by `finalizeSummary`
when the function's exit block was never reached by the dataflow—no path
reaches it because every path ends in a block with a `noreturn` element
(declared, or a call to a callee whose summary is `never-returns`) or in
a loop without an exit—and the fixpoint converged. A function whose
dataflow did not converge (RFC 0006, `MaxVisitsPerBlock`) is not
`never-returns`.

A call to a callee whose resolved summary is `never-returns` **ends the
block**: the remaining elements of the block are not analysed, the block's
state is not propagated to its successors, and its end-of-block checks
(dead locals, leaks at scope exit) are not run, exactly as for a block
Clang marks `hasNoReturnElement()`. The callee's effects *before* the
termination (consumes, stores) are applied first, so a `noreturn` function
that frees its argument still leaves the caller's copy consumed on the
(dead) path—a distinction without a difference, since the path does not
continue.

`neverReturns` joins by **conjunction**: the join of two summaries is
`never-returns` only when both are. An empty summary (the identity of the
join) cannot be told from a candidate that does nothing and returns, so a
join that starts from the empty summary and folds in candidates settles
the bit separately: `lookupIndirect` (RFC 0005) records whether *any*
candidate—local or from the program database—returns and clears the bit
if so; the program database folds duplicate definitions of one function
under the same rule.

The intra-unit fixpoint (RFC 0003, *Recursion*) starts each SCC member
from the empty summary, which is not `never-returns`: a mutually
recursive pair whose only exits are `abort()` is inferred `never-returns`
on the second iteration, which is sound because the first iteration's
callers saw "may return" and were re-analysed.

### Summary text format (version 5)

```
never-returns
effect param 1 freed(free) when param 3 zero
outcome null param 1 freed(free) when param 3 zero
store param 0 *.msg = copy param 2 when param 2 nonnull
return fresh(free) when param 3 positive
return copy param 1 when param 3 =0 and param 2 positive|zero
```

`when` introduces a guard: conjuncts separated by `and`, each a path and
a fact; a fact is `=<k>` for a known constant (its class is implied) or
the classes joined by `|` (`zero`, `positive`, `negative`, `null`,
`nonnull`). A reader of version 4 rejects version 5 text; the
`SummaryFormatVersion` and `SidecarFormatVersion` are 5 and 5.

### `--dump-analysis`

- `exit:` and per-block states gain `scalars{n =0, m positive|negative}`
  (a constant is `=k`, a class set is the classes joined by `|`), printed
  only when non-empty.
- Guarded records render `when[c positive|negative]` after the record:
  `moved{p@3:5 freed(free) when[c positive|negative]}`, `owned{q@4:3
  allocated free when[n positive]}`, `nulls{p maybe-null when[n =0]
  otherwise-nonnull}`.
- Summary dumps render `never-returns` first after `summary:`, and guarded
  effects, stores and returns with the same `when[...]` suffix (`b->data:
  freed(free) when[b->noalloc =0]`, `stores{s->msg = copy msg when[msg
  nonnull]}`, `returns{fresh(free) when[nsize positive|negative], null}`).

## Annotation surface

None. `WEAVEC_NORETURN` is not added: Clang already honours
`__attribute__((noreturn))` and C11 `_Noreturn`, and the point of this RFC
is that the attribute is not needed for the checker's purposes.

## Diagnostics

None new. Existing diagnostics are unchanged in message and id; the set of
programs that trigger them shrinks.

## Drawbacks

- **State size.** Every move and resource record grows by a (usually
  empty) map; every state gains a scalar map. The corpus measurements
  (*Implementation notes*) bound the cost.
- **A second place where summaries branch.** RFC 0006 introduced
  per-outcome effects (what the callee did *given its result*); this RFC
  introduces per-argument effects (what the callee did *given its
  inputs*). They compose (an `outcome` line may carry `when`) but a reader
  of a summary must understand both.
- **Guards are weakened, never strengthened, on reassignment.** The
  alternative—tracking that `c` was `zero` when `p` was freed even after
  `c` changes—is a form of path sensitivity the state does not have.

## Alternatives

- **Annotate `noreturn` in the corpus / require it of users.** Rejected:
  the information is in the body; a checker that needs the attribute to
  be right on the most common error-handling shape in C is not usable on
  unannotated code.
- **Intervals or octagons instead of classes.** Rejected for this RFC: the
  three classes plus an exact constant cover the corpus patterns (`== 0`,
  `!= 0`, `> 0`, `== k` in switches) and keep the lattice finite without
  widening; the class set is the RFC 0006 outcome set so the two designs
  share one vocabulary. Intervals are *Future work* if bounds checks are
  ever attempted.
- **Path-sensitive analysis (split states on every test).** Rejected on
  cost: RFC 0006 measured the current design at 29 minutes for Lua and
  this RFC must not multiply it.
- **A `WEAVEC_WHEN(param, class)` annotation on effects.** Rejected: the
  inference covers the cases seen; an annotation can be added later with
  the same summary vocabulary if a boundary needs it.
- **Guards as full `PendingOutcome`s.** Rejected: an outcome is settled by
  *one* test of *one* value (the call's result), and a guard may involve
  several places and be refuted by tests in any order.

## Prior art

- **Clang Static Analyzer** is fully path-sensitive with a constraint
  manager over symbolic integers; its `noreturn` handling is the same
  "end the path" rule, applied to the declared attribute and to a
  hard-coded list of functions. This RFC infers the attribute instead.
- **Infer** (Pulse) summarises functions with pre/post *specs* selected by
  the caller's state—the same idea as argument-conditional effects, over a
  richer separation-logic domain.
- **Frama-C EVA** and **Astrée** track integer intervals/congruences with
  widening; the trade-off noted under *Alternatives*.
- **GCC `-fanalyzer`** infers `noreturn` for functions ending in `abort`
  in the same translation unit (and warns to add the attribute with
  `-Wmissing-noreturn`); this RFC does it across units through summaries.
- **Rust** has no equivalent problem: `!` is in the type system and
  effects are in the types.

## Unresolved questions

- Whether `never-returns` should be reported (a suggestion to add
  `_Noreturn`) under a new warning-level id. Left for a diagnostics RFC;
  the information is in the summary dump.
- Whether guards should be pruned against a *type* fact (an unsigned place
  can never be `negative`). Not done: it would let the checker refute
  based on a type it might be wrong about (bitfields, enums with negative
  enumerators); the loss is only precision.

## Future work

- **Argument-conditional termination**: `never-returns when param 0 zero`
  for `void check(int ok) { if (!ok) fail(); }`, so `check(0); use(p);`
  is a dead path. The guard vocabulary is in place; the block-ending rule
  would apply when the translated guard is discharged.
- **Relational facts** between two places (`i < n`), needed before any
  bounds check.
- **Facts about call results kept as scalars**: `n = strlen(s); if (n ==
  0)` refines `n`, but a summary cannot yet say "returns `zero` when
  `param 0 *` is `zero`".

## Implementation notes

Files:

- `include/weavec/Core/Scalar.h`, `lib/Core/Scalar.cpp`: `Outcome` (moved
  here from `Summary.h` so both can use it), `ValueFact`, `GuardOn<Key>`
  (`PlaceGuard`, `PathGuard`; `require`, `learn`, `refine`, `join`, `drop`,
  bounded by `MaxGuardConjuncts`), `ScalarTracker`.
- `include/weavec/Core/Moves.h`, `Resource.h`, `Nullness.h`: `guard` on
  the records (`NullRecord::otherwiseNonNull` too); `join` joins the
  guards of records on both sides; `learn` refutes and erases (or, for an
  otherwise-non-null record, promotes); `dropGuardsOn`.
- `include/weavec/Core/AnalysisState.h`: `scalars`; `pathGuard()` (the
  facts of the current path as a new record's guard); `factOf` (scalars,
  then definite nullness); `learn` and `dropGuardsOn` across the three
  trackers; `forget` drops guards on the place; `PendingOutcome` keeps a
  guard per consumed place per class.
- `include/weavec/Core/Summary.h`: `PlaceEffect::when`,
  `ValueSource::when`, `FunctionSummary::neverReturns` (joins by
  conjunction); `addReturn`, `addStore` and `addEffect` merge alternatives
  that differ only in their guard by joining the guards.
- `lib/Core/SummaryIO.cpp`: version 5, `when` clauses, `never-returns`;
  a guard on a global the resolver declines is dropped.
- `lib/Analysis/Dataflow.cpp`: `tracksScalar` (locals and parameters,
  address-taken included; not globals); scalar transfer (`assignScalar`
  with the borrowed images of a write through a pointer, condition edges,
  `switch`, `!x` as `x == 0`); `edgeInfeasible` for a refuted edge on a
  variable's own storage; path guards at every event; `pruneGuard` /
  `pruneOrigin` for a callee's translated guards (a discarded call whose
  surviving alternatives are not fresh is not a `leak`);
  `writtenScalarPaths` / `dropUnstableGuards` at summary finalisation;
  `finalizeSummary` narrows an unreplaced consume to its exit record's
  guard (*Replaced values under a guard*) and `doConsume` reinitialises a
  replaced place after a `double-free` report; `replayWrites` no longer
  turns a callee's consume below a pointer into `written` on the whole
  pointee (the coarse fallback is for summaries that say nothing below it);
  `blockNeverReturns` (memoised) ends the block and is honoured by the
  liveness pass, `neverReturns` set when the exit was never reached.
- `lib/Analysis/PlaceBuilder.cpp`: `translateGuard` (constants and null
  constants decide on the spot; casts and positive scaling looked through,
  the constant dropped under scaling); `ValueOrigin::guard`;
  `integerConstant` (the mathematical value of a constant, read in its own
  type; none when an `int64_t` does not hold it) and `integerConvertedTo`
  (a `case` label in the scrutinee's type), shared with `Dataflow.cpp`,
  whose `classesSatisfying` decides an unsigned comparison in unsigned
  order (*Assumptions*).
- `lib/Analysis/Summaries.cpp`: `lookupIndirect` settles `neverReturns`
  from whether any candidate returns. `lib/Analysis/ProgramDatabase.cpp`:
  duplicate definitions fold the bit by conjunction.
- `lib/Frontend/Sidecar.cpp`: version 5.

Tests: `unittests/Core/ScalarTest.cpp`, additions to `MovesTest`,
`ResourceTest`, `NullnessTest`, `SummaryTest`, `SummaryIOTest`,
`AnalysisStateTest`; `unittests/Analysis/ValueConditionalTest.cpp`;
`test/Analysis/rfc0009-noreturn.c`, `rfc0009-scalars.c`,
`rfc0009-arguments.c`, `rfc0009-replaced.c`,
`test/WholeProgram/rfc0009-noreturn-units.c`. The
RFC 0007 `merged` case (`if (c) p = malloc(8); if (!c) return -1;
free(p);`) that `rfc0007-leaks.c` pinned as an accepted false positive is
clean now.

### Performance

Every record in every state carries a guard, and states are copied at
every block, so the representations were chosen not to allocate:
`ValueFact::classes` is an `OutcomeSet` (one byte, not a `std::set`), and
a guard's conjuncts are a `FlatMap` (one sorted vector, not a node per
conjunct). Before that, a single unit of Lua (`lvm.c`) was 48% slower
than before the RFC; after it, 14% (`lparser.c` 17%, `lgc.c` 28%, all
under a second).

The corpus (`scripts/corpus.py`) is the measure. Lua as one program
(RFC 0005), same machine and build, both runs with `-ferror-limit=0`
(Clang's default cap of 20 errors per unit hid most of `lstrlib.c` and
`lauxlib.c` from every earlier baseline): 26.0 minutes before this RFC,
33.0 and 34.5 minutes after, in two runs (+27–33%). The per-unit overhead does not account for
all of it; the rest is the whole-program fixpoint, where guarded
summaries change more often between rounds. The RFC asked for "a few
percent"; this is not that, and it is recorded here as the cost the
precision was bought at rather than hidden. The next step, if it is to
be taken, is the one RFC 0008's *Performance* note already names for
`luaV_execute`: fewer places consumed and reinitialised per
`luaM_realloc`-family call, not cheaper guards.

What the RFC asked for in reports it delivers: no project reports
anything it did not report before, and Lua loses 131 reports (123 of them
`null-dereference`: the `luaL_check*`/`tag_error` group is gone), cJSON
three in each of its two configurations, zlib three. The triage is in
`scripts/corpus/README.md`; it found the three checker bugs recorded above
(*Replaced values under a guard*, *Assumptions*), one of them a report the
first implementation had silently dropped.
