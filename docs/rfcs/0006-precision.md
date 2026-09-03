# RFC 0006: Precision: non-lexical loans, condition facts, element places and outcome-conditional summaries

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-02
- **Accepted**: 2026-09-02
- **Tracking issue**: TBD
- **Supersedes / superseded by**: supersedes RFC 0002 *Loans and lifetimes*
  (loans now end at the holder's last use), the *Borrow rules* paragraph of
  RFC 0001 *The model* as the default behaviour (exclusive borrowing becomes
  opt-in), RFC 0002 *`realloc` and the null edge* (subsumed by
  outcome-conditional summaries), the RFC 0002 rule that `a[i]` and `a[j]`
  are indistinguishable for moves (element witnesses), and the RFC 0003
  summary text format version 1 (bumped to 2). Resolves the two follow-ups
  the roadmap lists under Milestone 2 (*may-moves for `a[*]` element
  places*, *pointer-equality guards*).

## Summary

RFC 0002 through 0005 made the checker sound and whole-program; on the
corpus every one of its reports is a false positive, and every one of them
has one of five causes. This RFC removes those causes without weakening the
guarantee:

1. **Loans end at the holder's last use**, not at the end of its scope
   (Rust's non-lexical lifetimes), and by default a live loan conflicts only
   with operations that *invalidate* the borrowed object: freeing, moving or
   reallocating it. Writing a borrowed object, or borrowing it a second
   time, is not a memory-safety error in C and is no longer reported unless
   the new `--exclusive-borrows` option asks for Rust's exclusivity rule.
2. **Condition facts on CFG edges.** `p == q` unites the two pointers'
   alias classes on its true edge; `p != q` separates them on its true edge
   when the alias came from a plain copy (the model records whether an alias
   is *exact* or *interior*). Tests of a call's result select among the
   callee's *outcomes* (below). This is the mechanism RFC 0002 built for
   `realloc` alone, generalised.
3. **Element witnesses.** A move through an element access (`free(a[i])`)
   remembers *which* element: the constant, or the index variable until it
   is next assigned. A later access is a use of that element only if it is
   spelled with the same witness; otherwise it may be a different element
   and is not reported.
4. **Outcome-conditional summaries.** A summary records, per class of return
   value (`null` / `nonnull` for pointers, `zero` / `positive` / `negative`
   for integers), what the callee consumed on the paths that return that
   class. A caller that tests the result keeps only the consumption of the
   classes the edge selects. `realloc` is the special case where the
   argument is consumed only on `nonnull`.
5. **`written` forgets what lies below.** A callee that overwrites an object
   (`memcpy(root, &tmp, sizeof *root)`) invalidates what the caller knew
   about the pointers stored in it, so stale `freed` records are dropped
   before the callee's stores are applied.

Together these clear every diagnostic in the tracked corpus baseline and
turn the three probe idioms that motivated them (guarded free after a
failed transfer, freeing the elements of an array in a loop, reading a local
after handing out a pointer to it) into clean code.

## Motivation

`scripts/corpus.py` over the seven tracked projects reports 30 diagnostics,
all triaged as false positives (`scripts/corpus/README.md`). By cause:

| Cause                                                           | Reports | Example                                                                                                   |
| --------------------------------------------------------------- | ------- | --------------------------------------------------------------------------------------------------------- |
| Loop over `a[*]` frees the element summary place twice          | 5       | `for (i = 0; i < n; i++) free(v[i]);` (linenoise `freeCompletions`, cJSON `cJSON_ParseWithOpts`)         |
| Pointer-equality guard the alias relation cannot see            | 15      | `if (line != linenoiseEditMore) free(line)`; sentinel values shared through a global                      |
| Lexical loan outlives its last use                              | 4       | `char *p = buf; ... /* p dead */ buf[0] = 0;`, `int *pa = &s->a; *pa = 1; s->a = 2;`                       |
| Consumption correlated with the callee's return value           | 4       | `if (!try_take(p)) free(p);`, `if (cJSON_AddItemToArray(a, item) == 0) cJSON_Delete(item);`             |
| `memcpy(root, &replacement)` after `free(root->string)`         | 2       | cJSON `cJSON_ReplaceItemViaPointer`                                                                       |

A checker whose every report is wrong is not adoptable however sound it is:
users learn to ignore it. Each cause above is a *precision* gap, not a
soundness one, and each has a well-understood remedy (Rust's NLL, standard
path-sensitive refinement on conditions, weak/strong updates for summarised
locations, conditional effects in summaries). This RFC adopts the smallest
form of each that clears the corpus and keeps RFC 0001's guarantee intact.

The cost of not doing this is that Milestone 4 ships a tool whose baseline
is "everything it says is noise".

## Soundness

The guarantee of RFC 0001 is unchanged: outside `WEAVEC_UNSAFE`, WeaveC
reports every use-after-free, double-free and dangling-pointer escape it
can express, or refuses to analyse. Every change below either keeps the
over-approximation (it only *drops* facts that were known to be stale) or
trades a bug class the model never reliably caught for one it does. Where a
change makes the checker less sound in some corner, the corner is named.

### Bugs caught

- Unchanged: use after `free`/move through any alias, double free, freeing
  or moving an object while a live pointer into it exists, escaping a
  pointer to a shorter-lived object, raw operations outside unsafe regions.
- **Newly caught.** `free(a[i]); a[i]->x = 1;` (same element, same witness)
  is a use after free; before, it was the same report as `free(a[i]);
  a[j]->x` and was drowned in the noise of the loop idiom. `r = f(p); if (r
  == 0) use(p);` where `f` consumed `p` only when returning `0` is a use
  after move; before, `p` was reported at every use regardless of the test.

### Bugs deliberately not caught

- **Non-exclusive mutation.** `int *a = &x; x = 1; use(a);` and `int *a =
  &x; int *b = &x;` are accepted by default. They are not memory-safety
  errors in single-threaded C; Rust forbids them to make data races and
  iterator invalidation impossible, which RFC 0001 already puts outside the
  model (*Assumptions*: no data races). Users porting Rust intuition can
  pass `--exclusive-borrows` to get the RFC 0001 rule back.
- **Different-witness elements.** `free(a[i]); use(a[j]);` and `for (i)
  free(a[i]); use(a[0]);` are not reported: `i` may differ from `j`, and
  after `i` was incremented the freed element is unknown. Only an access
  with the *same* witness is a use of a freed element. This is a
  precision/soundness trade: a whole-array free followed by a read of one
  element is a real bug the model now misses; the loop idiom it accepts is
  ubiquitous in C. A summary derived from such a body still says `param 0
  *` is freed (elements are collapsed in summary paths), so callers see the
  strong fact.
- **Weak reinitialisation.** `free(a[i]); a[i] = NULL;` clears the freed
  record of `a[*]` because the witnesses match (same `i`, not reassigned
  between). `free(a[i]); a[j] = NULL; use(a[i]);` keeps the record (the
  witnesses differ) and reports the use. `free(a[i]); i = k; a[i] = NULL;
  use(a[i]);` also keeps it: assigning `i` makes the record's witness
  *unknown*, and an element write clears only a record with a matching
  witness. Records with an unknown witness are therefore never cleared by
  element writes and never reported by element reads; they are the RFC
  0002 summary place with its reports turned off, which is the trade this
  RFC makes for the loop idiom.
- **`!=` on interior pointers.** `q = p + 1; if (p != q) { free(p); use(q); }`
  is *not* accepted: the alias `q ~ p` is recorded as *interior* (the values
  differ, the object is the same), and `!=` separates only *exact* aliases.
  The model records exactness per alias edge; a copy through a summary is
  exact only if the callee's `copy` source says so (the shipped table marks
  `strchr`-like results interior and `memcpy`-like results exact).

### Accepted false positives

- Merge-point conservatism (RFC 0002) remains: `if (c) free(p); if (!c)
  use(p);` is still reported. Condition facts are about pointer values and
  call results, not arbitrary booleans.
- Loans held by *fields* of a local struct end with the last use of the
  struct variable, not of the field.
- A loan whose holder's address is taken (`&a` for `int *a`) lasts to the
  end of the holder's scope: it may be read through the pointer at any
  time.
- An outcome test that is not one of the recognised shapes (below) selects
  nothing; the may-consumption stands.

### Assumptions

Unchanged from RFC 0001 through 0005. In addition: an element index that is
a variable is assumed not to change between the free and the use unless the
function assigns it (an aliasing write through `int *ip = &i` counts:
taking `&i` makes every witness on `i` unknown from that point).

## Detailed design

### Loans end at the last use of their holder

**Core.** No change to `Loan` or `BorrowState`. `BorrowState::expireHolders
(pred)` (new) drops every loan whose holder satisfies a predicate.

**Analysis.** After the CFG is built, `FunctionDataflow::computeLiveness()`
runs a standard backward liveness analysis over the CFG blocks with the
local variables of the function (parameters included) as the domain. The
transfer per CFG element is:

- a `DeclRefExpr` to a local that is not the entire left operand of a plain
  `=` is a *use*;
- a plain `=` whose left operand is a bare `DeclRefExpr` is a *def* of that
  variable; a `DeclStmt` is a def of each variable it declares;
- everything else (compound assignment, `++`, `&v`, `v.f = ...`,
  `v[i] = ...`) uses `v`.

The result is, per block and element index, the set of locals live *before*
the element. `transfer` expires, before each element, every loan whose
holder place has no `Deref` step (the loan is stored in the variable itself
or in a field or element of a local aggregate), whose root variable is a
local (not a global, not a `static` local) that is not address-taken
anywhere in the function, and that is not live before the element. Loans
held through a pointer (`node->buf`) or by a global never expire this way;
they end when the holder is reassigned or, for loans on locals, when the
`lifetime-too-short` check at the assignment already rejected them.

Liveness is a property of the program point, so expiring on it is a
deterministic transfer and the fixpoint iteration is unaffected. Loans still
also end when the holder is reassigned (`dropHolder`) or leaves scope, which
liveness subsumes but which keeps the exit state clean for the dump.

The same point in the transfer drops the alias edges of a dead local (not a
parameter) and of the places below it; see *Performance* for why this is
safe and why it matters.

**Conflict rules.** `findLoanConflict` is queried from three places today:
a new borrow (`applyBorrow`, `checkTemporaryBorrow`), a direct write
(`doMutationCheck`) and a consume (`doConsume`). Under this RFC only the
consume query is on by default: a live loan on the consumed place or a
descendant of it is a `conflicting-borrow` (`cannot free '<p>' while it is
borrowed`, `cannot move ...`). A loan on an *ancestor* of the consumed place
is not invalidated and does not count: `z_streamp strm = &state->strm;
inflateInit2(&state->strm, ...)`, where the callee frees
`state->strm.state->window`, leaves `strm` pointing at storage nothing
released (amended with RFC 0007, whose more precise summaries first made
this case reachable). The two other queries run
only under `AnalysisOptions::exclusiveBorrows` (`weavec
--exclusive-borrows`, `weavec-cc -fweavec-exclusive-borrows`), reproducing
RFC 0001's rule verbatim (messages unchanged). Loan *kinds* (shared /
mutable) are still recorded and still drive summaries (`borrowKind`).

Why not keep exclusivity on by default and accept the noise? Because the
idioms it rejects are not bugs: `char *p = buf; snprintf(buf, ...);` writes
a buffer that another pointer views, which is how every string routine in C
is written. Rejecting it leaves users with no fix short of `WEAVEC_UNSAFE`,
which teaches them to reach for the escape hatch.

### Condition facts on CFG edges

`applyEdge(from, succ, state)` refines the state on the two edges of a
conditional terminator. The condition is matched, after stripping parens
and implicit casts, against these shapes; nothing else refines anything.

**Pointer equality.** `p == q`, `p != q`, where both operands resolve to
pointer places (`resolvePointerValue`; an operand that is an assignment,
`(res = feed()) == sentinel`, stands for its left side):

- on the edge where they are *equal*: `aliases.unite(p, q, exact)`;
- on the edge where they are *unequal*: `aliases.separateExact(p, q)`,
  which removes the alias edge between `p` and `q` if it is exact, and
  nothing else (aliases each may have with third places stay).

**Alias exactness (Core).** `AliasRelation` records a boolean per edge:
*exact* (the two places hold the same pointer value) or *interior* (they
point into the same object at possibly different offsets). `unite(a, b,
exact)` relates `a` to `b`'s aliases with exactness `exact && exact(b, x)`
and vice versa; `join` keeps an edge present on either side, exact only if
both sides have it exact. `ValueOrigin` and `ValueSource::Copy` carry an
`interior` flag: pointer arithmetic (`p + k`, `p++`, `p += k`) makes a copy
interior; everything else (`q = p`, casts, `?:` arms) is exact. Summary
`copy` sources are printed `copy <path>` (exact) or `interior <path>`. The
shipped table marks the functions that return their first argument
unchanged (`memcpy`, `memmove`, `memset`, `strcpy`, `strncpy`, `strcat`,
`strncat`, `fgets`, `gets_s`, `getcwd`, `realpath`, `inet_ntop`, the
`*_r` time functions returning their buffer, ...) as exact and every other
argument-returning function (`strchr`, `strstr`, `strtok`, `memchr`,
`basename`, ...) as interior.

**Outcome tests.** A test on a call result selects outcome classes (next
section). Recognised shapes, for an operand `x` that is a call expression
directly, or a place holding a pending outcome (`r = f(p)`, `if ((r = f(p))
< 0)`):

| Shape                                   | True edge selects                 | False edge selects                |
| --------------------------------------- | --------------------------------- | --------------------------------- |
| `x` (pointer)                           | `nonnull`                         | `null`                            |
| `!x` (pointer), `x == NULL`, `NULL == x` | `null`                            | `nonnull`                         |
| `x != NULL`                             | `nonnull`                         | `null`                            |
| `x` (integer)                           | `positive`, `negative`            | `zero`                            |
| `!x`, `x == 0`                          | `zero`                            | `positive`, `negative`            |
| `x != 0`                                | `positive`, `negative`            | `zero`                            |
| `x OP k`, `k OP x` (integer constant `k`) | the classes with a value satisfying the comparison | the classes with a value falsifying it |

The last row is computed from the class ranges (`negative` = (−∞, −1],
`zero` = {0}, `positive` = [1, ∞)): `x < 0` selects `negative` on the
true edge and `zero`, `positive` on the false edge; `x == -1` selects
`negative` on the true edge and all three on the false edge (a negative
value other than −1 also falsifies it). For an unsigned result the domain
is `zero`, `positive`.

**Pending outcomes (Core).** `AnalysisState::pending : Map<PlaceId,
PendingOutcome>` replaces `reallocs`. A `PendingOutcome` maps each outcome
class the callee may produce to the caller places whose consumption at the
call depends on the result being in that class (`consumedBy : Map<Outcome,
[PlaceId]>`). On an edge that selects `S`:

- if `S ∩ keys(consumedBy)` is empty the edge is infeasible as far as the
  summary knows; nothing changes;
- otherwise every place that is consumed in *no* selected class and still
  carries the move record is reinitialised (`moves.reinitialize`) and its
  summary path is removed from `consumed` (below: the consumption did not
  happen on this path), and the entry is narrowed to the selected classes.
  The narrowed entry is kept even once nothing more can be retracted: it
  records which classes the result can still be in, which a `return` of
  that result needs (*Inference*).

`join` keeps an entry only when both sides have it identical, as RFC 0002
did for `reallocs`. `forget(place)` drops the entry (the result was
reassigned). A pending outcome is attached to the place a call's result is
stored in (`q = f(p)`, `int r = f(p)`, including the assignment inside a
condition) at the assignment; a result that is tested directly (`if
(!f(p))`) is matched to the call expression, which the CFG places in the
same block as its terminator, through the dataflow's record of the last
call with a pending outcome in the block being transferred.

A reinitialisation on an edge is sound for the same reason it was for
`realloc`: any consumption of the same place between the call and the test
was reported when it happened (`doConsume` sees the place already moved),
so a retraction only ever follows a report.

### Element witnesses

**Core.** `MoveRecord` gains `element : ElementWitness`, where an
`ElementWitness` is one of *whole* (the access named the place without a
subscript; matches every access), *constant k*, *variable v* (a
`PlaceId`), or *unknown* (an element that can no longer be identified).
Two witnesses *match* if either is *whole*, or both are the same constant,
or both are the same variable. `MoveTracker` gains:

- `markMoved(place, reason, location, via, element)`: if `place` has a
  record whose witness matches, that record is returned (a double move) and
  kept; if it has a record whose witness does *not* match, the new record
  *replaces* it (the most recent element is the one later accesses in the
  same iteration name) and nothing is returned; otherwise the record is
  inserted;
- `movedAt(place, element)`: the record if one exists and its witness
  matches `element`, else nothing;
- `reinitialize(place, element)`: erases the record only if the witnesses
  match (a *whole* reinitialisation erases any record);
- `forgetWitness(variable)`: every record whose witness is `variable`
  becomes *unknown*;
- `join`: where both sides have a record for a place, equal witnesses are
  kept as they are; a *whole* witness on either side gives *whole* (that
  path consumed every element, so every later access is a use on it);
  otherwise the kept record's witness becomes *unknown*.

**Analysis.** `PlaceRef` gains `element`, set by `PlaceBuilder::resolve`
from the outermost `ArraySubscriptExpr` (or `*(p + e)`) on the path: an
integer constant expression gives *constant*, a `DeclRefExpr` to a variable
gives *variable*, anything else (including a second subscript) gives
*unknown*; a path with no subscript is *whole*. `doRead`, `doConsume` and
`reinit` pass the access's witness to the tracker. A write to, increment of,
or address-of a variable calls `forgetWitness` on it. `mirrorSubtree`,
`copyRecord` and summary application copy records with their witnesses; a
consume applied from a summary is *whole*. `*a` on an array-typed `a` is
`a[*]` with a constant witness `0`.

Loans and raw records are unaffected: `a[*]` stays one place for them.

### Outcome-conditional summaries

**Core.** `Outcome` is `Null | NonNull | Zero | Positive | Negative`.
`FunctionSummary` gains `outcomes : Map<Outcome, Map<SummaryPath,
PlaceEffect>>`: for each class the callee may return, the consumption
(`freed` / `moved`) that holds on the paths returning it. The unconditional
`effects` remain the may-union over all paths, so a reader that ignores
`outcomes` is exactly as sound as before. `reallocLike` is removed;
`realloc` and `reallocarray` are spelled `effect param 0 moved`, `return
fresh`, `return null`, `outcome null`, `outcome nonnull param 0 moved`.
`join` unions per class. `remapGlobals` applies to outcome paths as to
effects.

`AnalysisState` gains `consumed : Map<SummaryPath, PlaceEffect>`, the
flow-sensitive record of the consumption of parameter roots (and of paths
under reassigned parameters, RFC 0003) on the current path. It joins by
union and is never cleared: the argument stays consumed when the
parameter variable is reassigned.

**Inference.** At each `return e` in the final pass, the returned value is
classified: a null constant is `Null`; a borrow is `NonNull`; an integer
constant expression gives its sign class; anything else gives every class
of the result type (`{Null, NonNull}`, `{Zero, Positive, Negative}`, or
`{Zero, Positive}` for unsigned). For each class the effects on this path
are `state.consumed` plus the summary paths of `state.moves` (the RFC 0003
exit-state rule, applied at the return point). If the returned value is a
call result with a pending outcome (`return realloc(p, n)`, `char *q =
realloc(p, n); return q;`), the classes are first narrowed to those the
pending entry still allows (after `if (!q) return NULL;`, `return q` is
`nonnull` only), and each class then retracts what the pending entry
retracts for it, so wrappers around `realloc` and around error-returning
functions inherit the conditional behaviour. When the
classes are done, a summary in which every consumed path is consumed in
every class has nothing conditional to say and records no `outcomes` at
all; most summaries are of this kind and stay exactly as small as before.

**Application.** In `applySummary`, a target consumed by the callee is
*conditional* if some class in `outcomes` does not consume its path. The
conditional targets, per class, form a `PendingOutcome` recorded for the
call; the assignment that stores the result attaches it to the destination
(minus the destination itself, for `p = realloc(p, n)`), and a direct test
of the call matches it as described above. Consumption itself is still
applied unconditionally at the call; the edges retract.

**Text format.** `SummaryFormatVersion` becomes 2, `SidecarFormatVersion`
becomes 2. New lines: `outcome <class>` (the class is possible, nothing
conditional) and `outcome <class> <path> <flags>`. `copy <path>` gains the
sibling `interior <path>`. `realloc-like` is no longer written; a version-1
sidecar is rejected by its header as before.

### `written` forgets what lies below

In `applySummary`, after consumption and before stores, every summary path
with `written` that is not itself consumed is resolved and the facts about
every place *below* it are forgotten (`state.forget` on the descendants:
moves, aliases, loans, kinds, raw records). The place itself keeps its
facts: the object still exists. Stores from the same summary then
re-establish what the callee put there. Applied at every call site,
including inside the callee whose exit state produces the summary, so
`free(root->string); memcpy(root, &tmp, sizeof *root);` yields no `freed`
on `param 0 *.string` (the exit state no longer has it) and a caller of
`memcpy` directly gets the same treatment.

This drops a fact that was *known* to be stale (the callee overwrote the
storage); it never drops a fact about storage the callee did not write.

**What a callee wrote, its caller wrote** (amended with RFC 0007). A call
whose argument is mutably borrowed used to put `written` on the pointee
itself into the caller's summary. Under the rule above that told *its*
callers the whole object had been overwritten, and they forgot every fact
below it: in Lua, every function that bumps `L->nci` wiped what its callers
knew about `L`, and `close_state` lost the frees `freestack` reported to it.
Now the callee's written paths below `param(i)*` are copied into the
caller's summary under the pointee's own path (and the paths of its
mirrors), by prefix substitution and bounded by the place depth limit;
only a summary with no written path below the pointee — an annotation's —
is replayed as `written` on the pointee itself. The summaries therefore
name every written field transitively, which is what makes them long on a
program that threads one state pointer through every call (RFC 0007,
*Performance*).

### Interaction with existing RFCs

- RFC 0001 *The model*, borrow rules: the exclusivity rules become the
  `--exclusive-borrows` behaviour; the invalidation rule is the default.
  The lifetime rules are unchanged.
- RFC 0002 *Loans and lifetimes*: "a loan lives while its holder is in
  scope" becomes "while its holder is live". *`realloc` and the null
  edge*: subsumed. *Places*: `a[*]` is still one place; the witness lives in
  the move record, not the place. *Accepted false positives*, "two mutable
  borrows", "writes while borrowed": no longer false positives by default.
- RFC 0003 *Summaries*: the summary gains `outcomes`; the `copy` source
  gains exactness. `annotation-required`, `annotation-mismatch` unchanged.
  RFC 0003's rule that a consumed *parameter* returned as `copy` makes the
  result the same resource, now owned by the result (the `realloc` wrapper),
  is generalised to any consumed path: `Value *resizearray(L, t, ...)`
  reallocates `t->array` and returns `copy *t->array`; the result owns the
  new array, it is not a dangling copy of the old one. A callee that really
  returns a pointer it freed is reported in the callee, at the `return`.
- RFC 0004: unchanged. Raw records are not element-sensitive.
- RFC 0005: format and sidecar version bumps; `ProgramDatabase` prints
  outcomes in the dump.

### Performance

Liveness is one backward pass per function, linear in CFG size. Alias
exactness adds a bit per edge. Witness matching is a constant-time
comparison. Outcome classification adds a map per summary that is empty
for most functions (those without conditional consumption). Measured on
the corpus: no change on the existing projects.

Liveness also pays for itself. When a local dies its *alias edges* are
dropped along with its loans (`AliasRelation::separateIf`), for the same
reason: nothing will read it again, and every fact was propagated to its
aliases when it was made (`doConsume`, `mirrorSubtree`), so no other place
loses anything. Parameters are exempt: a live local's edge to a dead
parameter is how its accesses reach the summary (`m = n; return m->v` is a
borrow of `n`). Without this, a function with hundreds of block-scoped
pointers into one object (Lua's `luaV_execute`: 968 blocks, 80 `StkId ra`
locals, and computed `goto` dispatch, so their scope ends never appear in
the CFG) grows a dense alias relation over dead variables and takes a
minute per fixpoint; with it, three seconds.

## Annotation surface

None. `weavec.h` is untouched. The only user-visible switch is the
`--exclusive-borrows` / `-fweavec-exclusive-borrows` option, which is not
an annotation.

## Diagnostics

No new identifiers. Changes to existing ones:

- `conflicting-borrow`: by default only `cannot free '<p>' while it is
  borrowed` and `cannot move '<p>' while it is borrowed` are emitted (note:
  `borrowed by '<q>' here`). `cannot borrow '<x>' as mutable because it is
  already borrowed`, `... as shared because it is already mutably borrowed`
  and `cannot assign to '<x>' while it is borrowed` are emitted only under
  `--exclusive-borrows`, with their notes unchanged.
- `use-after-free` / `double-free` on element places: unchanged text (`use
  of 'a[*]' after it was freed`, `'a[*]' is freed twice`), now emitted only
  when the witnesses match.

Snippets that must be reported (default mode):

```c
void same_element(char **a, int i) { free(a[i]); a[i][0] = 0; }  /* use-after-free */
void same_constant(char **a) { free(a[0]); free(a[0]); }         /* double-free */
int after_test(char *p) {                                         /* use-after-move */
  int rc = try_take(p);   /* consumes p only when it returns 0 */
  if (rc == 0) use(p);
  return rc;
}
void free_borrowed(struct node *n) { int *a = &n->v; free(n); *a = 1; } /* conflicting-borrow */
```

Snippets that must be clean (default mode):

```c
void loop_free(char **a, int n) { for (int i = 0; i < n; i++) free(a[i]); free(a); }
void different_elements(char **a) { free(a[0]); use(a[1]); }
void null_out(char **a, int n) { for (int i = 0; i < n; i++) { free(a[i]); a[i] = NULL; } use(a[0]); }
void last_use(void) { char buf[8]; char *p = buf; use(p); buf[0] = 0; }
void two_views(struct s *s) { int *pa = &s->a; *pa = 1; s->a = 2; }
void guarded(char *p) { int rc = try_take(p); if (rc != 0) free(p); }
void sentinel(void) { char *l = get(); if (l == sentinel_value) return; free(l); }
void replace(struct n *root) { free(root->string); memcpy(root, &tmp, sizeof *root); use(root->string); }
```

## Drawbacks

- Core grows: a witness in every move record, a bit per alias edge, a map
  per summary, a map per state. Each is small, but the lattice has more
  components to keep monotone.
- Two borrow modes to test and document. The exclusive mode exists to keep
  RFC 0001's rule reachable; it is not expected to be the common
  configuration.
- The format bump invalidates every existing sidecar; `weavec-cc` users
  rebuild. Acceptable pre-1.0.
- Element witnesses are a heuristic dressed as a rule: they are exact for
  the idioms named here and blind to index arithmetic (`a[i + 1]` is
  *unknown*). The RFC accepts that in exchange for predictability: the
  user can read off from the source whether two accesses will be treated
  as the same element.

## Alternatives

- **Keep exclusivity, add NLL only.** Clears the `int *pa = &s->a; *pa = 1;
  s->a = 2;` shape but not `char *p = buf; snprintf(buf, ...); use(p);`,
  which is the majority of the borrow noise. Rejected.
- **Kill loans on mutation instead of reporting.** Treat a write to `x`
  as ending loans on `x` (the borrow is "stale"). Unsound: it would make
  `int *a = &n->v; n->v = 1; free(n); *a` clean. Rejected.
- **Full path sensitivity (per-condition state splitting).** Clears the
  `if (c) free(p); if (!c) use(p);` shape too, at exponential cost and with
  a much larger Core. Deferred; the condition facts here are the subset
  with a bounded state.
- **General null lattice on every pointer place.** Considered as the
  vehicle for the `realloc` generalisation. It adds a component that
  nothing else consumes: with `reinit` on `p = NULL` already handled, the
  only consumer of nullness is the outcome test, which pending entries
  serve directly. Rejected as dead weight.
- **Index-sensitive places (`a[i]` as its own place).** The precise
  answer to the loop idiom, and the reason Rust forbids moving out of an
  index. It needs a theory of when `a[i]` and `a[j]` are the same place,
  which is the witness in a different coat with unbounded place sets.
  Rejected in favour of witnesses on the record.
- **Do nothing.** Ship a whole-program checker whose reports are all
  noise. Rejected.

## Prior art

- **Rust NLL (RFC 2094)**: loans live while the reference may still be
  used; the liveness-based expiry here is that idea on a CFG with the
  holder variable standing for the reference. Rust computes liveness of
  regions; we compute liveness of holder variables, which is coarser (a
  struct variable is live if any field is) but needs no region inference.
- **Rust two-phase borrows / Polonius**: not adopted; C has no
  reborrowing.
- **Clang static analyzer `MallocChecker`**: path-sensitive with a symbolic
  store; it gets `if (!try_take(p)) free(p)` right by forking the state on
  the return value's symbol. We take the outcome-class discretisation
  instead, which fits in a join-based dataflow and a text summary.
- **Infer's biabduction / Pulse**: summaries with disjunctive
  post-conditions per return class are what `outcomes` is a bounded form
  of.
- **Andersen/Steensgaard exactness**: recording whether an alias is a
  copy or an offset is standard in points-to analyses that support
  pointer comparison; we only need the bit.
- **Weak vs strong updates** (Chase, Wegman, Zadeck 1990): the element
  witness is a strong update when the witness matches and a weak one
  otherwise, with the extra step of *replacing* a non-matching record so
  the most recent element stays trackable.

## Unresolved questions

- Whether `--exclusive-borrows` should also make temporary borrows for
  calls (`peek(&x)` while `int *a = &x` is live) conflict. This RFC says
  yes (it reproduces RFC 0001 verbatim); it may be too noisy even for that
  mode.
- Whether summaries should carry element weakness (`free(a[0])` in a
  callee freeing only `param 0 *` "weakly"). Deferred until the corpus
  shows a case.
- Whether outcome classes should include booleans distinct from integers
  (`_Bool` results are `{Zero, Positive}` here). Deferred.

## Future work

- **Leak detection and release families** (a later RFC): with the
  precision here in place, "owned value not released on some path" and
  "`fopen` result passed to `free`" become reportable without drowning
  in the current noise.
- **Owned fields**: `struct box { char *WEAVEC_OWNED p; }` with a leak on
  `free(b)` while `b->p` is owned. Same RFC as leaks.
- **Corpus growth**: the acceptance criterion for this RFC is a clean
  baseline on the seven tracked projects plus at least two mid-size
  additions (zlib, Lua) with every remaining report triaged.
