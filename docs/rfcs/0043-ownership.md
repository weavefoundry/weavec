# RFC 0043: Ownership

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-11
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Adds rules to the safe functions of
  [RFC 0041](0041-safe-c.md) and [RFC 0042](0042-typed-memory.md), and
  carries out what RFC 0042 deferred under _Alternatives_ ("ownership
  checked at compile time") and _Future work_. Supersedes RFC 0041 §1 and
  RFC 0042 §1 (the tiers: one safe tier in place of safe and typed; the
  flag `-fweavec-typed` is removed), RFC 0041 §6 and RFC 0042's "no report
  of a use after free" in part (a reference is checked when it is loaded),
  and the guide's statement that ownership is not checked. Amends the
  library table (owner results), the enforcement ledger and the audit, the
  driver's flags, `weavec.h` and the runtime ABI. Removes no guard, no
  quarantine and no sweep.

This RFC was written at the request of the project owner on 2026-11-11,
who asked again for the single change that would do most to make an AI
rewrite into C or C++ as attractive as an AI rewrite into Rust, "being
able to iterate with memory-based compiler errors", to be implemented
next, with breaking changes and the removal of old code welcome. The
measurements in _Motivation_ were taken on main at v0.23.0 (`eca6698f`).

The owner accepted the design as drafted on 2026-11-11, before any of it
was built, and with it the three decisions that were theirs to make:
ownership is mandatory in safe units, so existing typed units stop
compiling until their owners are declared; the rules are not load-bearing
at run time, so this RFC makes nothing faster; and the safe and typed
tiers become one. The thresholds of §14 and the _Unresolved questions_
are still open, and S0 is where they are settled.

## Summary

RFC 0041 and RFC 0042 gave C half of Rust's loop. A safe function that
indexes past a buffer, forges a pointer or breaks a bound is rejected when
it compiles or stopped by one comparison when it runs, at 1.1 to 1.9 times
the reference build. The other half is missing. A safe function that reads
a block after releasing it, releases it twice, forgets to release it, or
keeps a pointer into an array across the call that reallocates it compiles
without a word. The runtime keeps each of those from touching another
object, which is memory safety; it is not an error an agent can read, fix
and compile again. Those errors are the ones Rust is known for, and
ownership is the part of a C program an agent gets wrong most easily,
because in C it writes every release by hand.

This RFC makes ownership part of Safe C. A pointer declared
**`WEAVEC_OWNED`** is the one pointer responsible for releasing its block.
The checker follows each owner through the function that holds it, and
rejects, with the rule and the fix:

- a use of an owner after it was moved or released, a second release
  included (`owner-moved`);
- a path on which an owner is dropped while it may still hold a block: a
  leak (`owner-leak`);
- a pointer taken from inside what an owner owns and used after the owner
  was released or replaced, by the function or by a call that could do it:
  the stale pointer into a grown vector (`stale-borrow`);
- and the few constructs that would make those three unsound
  (`owner-source`, `owner-path`, `owner-slot`, `call-alias`).

A pointer field that is not an owner is a **reference**: a back pointer, a
cursor into someone else's buffer, a cache entry. No rule follows it; it is
tested, when a safe function loads it, not to point into a released block,
and the program stops with `heap-use-after-free` if it does.

The rules are local to a function, written to be satisfied by rewriting,
never by proof, and they are **not** load-bearing for memory safety:
nothing the runtime does today is removed on their strength. A hole in a
rule is a missed diagnostic, and the sweep still confines what it missed.
That is the difference from RFCs 0001 to 0034, and it is deliberate.

Two cleanups come with it. The two safe tiers become one: `-fweavec-safe`
and `WEAVEC_SAFE` mean what `-fweavec-typed` meant, plus ownership, and the
tier of RFC 0041 that kept guards on loaded pointers is deleted. And every
rule can be read from the compiler: `weavec-cc --weavec-explain <rule>`
prints it with a rejected example and its rewrite, as `rustc --explain`
does, because no model was trained on this dialect.

Expected result (gates, §14): of a frozen set of temporal bugs seeded into
the converted projects, at least 90% of those in safe functions stop the
compile, where none does today; the conversions and a new port take a
comparable number of rounds; and the benchmarks do not move.

## Motivation

### What an agent can iterate on today

| A bug an agent writes | Rust | Safe C today (RFC 0042) | This RFC |
| --- | --- | --- | --- |
| An index past a buffer | panic when it runs | trap when it runs (one comparison) | unchanged |
| A pointer stored with the wrong type or bound | compile error | compile error | unchanged |
| A block read after its release, through its pointer or a copy | compile error | compiles; reads the released block, no report | compile error |
| A pointer into a container, used after the container grew | compile error | compiles; reads the released block, no report | compile error |
| A block released twice | cannot be written | compiles; `double-free` when it runs, if the optimizer left the calls | compile error |
| A block never released on an error path | cannot be written by default (`Drop`) | compiles; never reported | compile error |
| A back pointer or cache entry to a block its owner released | does not compile; restructured (`Rc`/`Weak`, an index) | compiles; reads the released block, no report | trap when the pointer is loaded |
| A data race | compile error | not caught | not caught |

The guide says so in one line: "Ownership is not checked when the program
compiles: release each block once."

### Measured

A probe of six functions, each with one temporal bug an agent could
plausibly write, compiled as a typed unit (darwin-arm64, 2026-11-11, the
`dev` build of `eca6698f`; kept in `build/rfc43/probe/temporal.c`):

| Bug | Compile, `-fweavec-typed` | Run, `-O2` | Run, `-O0` | Run, `-O2 -fweavec-safe-checks=full` |
| --- | --- | --- | --- | --- |
| 1. `free(it); return it->v;` | accepted | exit 0; prints 18137304 (the function compiles to `ret i32 undef`) | exit 0; prints the stale value | `heap-use-after-free` |
| 2. the same through `alias = it` | accepted | as 1 | as 1 | `heap-use-after-free` |
| 3. `free(it); free(it);` | accepted | exit 0, no report (the optimizer removed the allocation and both calls) | `double-free` | `double-free` |
| 4. an early return without `free(it)`, and `it->name` never released | accepted | nothing | nothing | nothing |
| 5. `first = &l->items[0]`, `realloc(l->items, …)`, `(*first)->v` | accepted | exit 0; prints the stale value | as `-O2` | `heap-use-after-free` |
| 6. `c->last = it; free(it);` then `c->last->v` in another typed function | accepted | exit 0; prints 8, a value never stored in that field | exit 0; prints the stale value | `heap-use-after-free` |

No bug stops the compile. In the build a project ships, all six run to a
clean exit: three print a value the program never computed, one a stale
one, one leaks, and the double free is optimized away. The mode that
reports five of them costs what guarded code costs, needs a test that
reaches the bug, and never reports the leak. Under
`-fweavec-checks=verify` bugs 1, 2, 5 and 6 are `weavec.proven` reports:
the monitors exist, which is what lets this RFC test its own rules (§11).

Three more measurements say what is missing is a declaration, not an
analysis:

- **Clang's static analyzer** (`clang --analyze`, the `unix.Malloc`
  checker) reports all six on that file, as warnings, because every
  allocation and release is in sight. On four of the same bugs written
  against a constructor and a destructor declared in a header and defined
  in another file (`build/rfc43/probe/u.c`), it reports **none of four**.
  With `ownership_returns` and `ownership_takes` attributes on the two
  declarations (`u2.c`) it reports three of four; it has no way to say
  that a field owns, so the fourth (a struct keeping a pointer the caller
  then frees) passes. Ownership has to be in the signatures.
- **Clang 23.1's lifetime-safety analysis** has an experimental C mode
  (`-Xclang -fexperimental-lifetime-safety-c -Wlifetime-safety`). On the
  probe it reports nothing; on a file with stack errors (`s.c`) it reports
  a returned stack address and a use after scope, and not
  `free(p); return *p;`. It covers the stack, which RFC 0041 already
  settles by heap placement, and not the heap.
- **The conversions.** A textual count of calls to `free`, `realloc` and
  each project's wrappers in the typed conversions of RFC 0042 (kept in
  `build/rfc43/baseline/`; S0 recounts by hand): cJSON 21, sds 41, zlib
  41, tinyexpr 36, bzip2 18, inih 3, the `lzw` port 3, jsmn and
  http-parser none. About 160 release sites, in code that was converted
  to be safe, that nothing checks when it compiles. In RFC 0042's port
  (gate T7) the Rust agent wrote no release at all; the C agent wrote
  each by hand.

### Why "the temporal half is already done" is not enough

RFC 0041 argued that the sweep makes a borrow checker unnecessary: with no
reuse while a pointer exists, a dangling pointer cannot reach another
object. That is true and stays true. It answers "is the program memory
safe", not "does the compiler tell the agent what it got wrong":

1. **Nothing to iterate on.** The agent's bug compiles, runs and prints a
   wrong answer. Finding it takes a test that reaches it and a mode that
   costs twice the run time; in Rust it takes one compile.
2. **Three classes are never errors.** A leak is never reported. A double
   free is reported when it runs, unless the optimizer removed it. A use
   after free in safe code is reported only under `full`.
3. **The run-time cost cannot fall while the runtime is the only thing
   that knows.** The allocator's quarantine and sweep are 0.25 to 0.31 of
   the reference's time on the two conversions that allocate (RFC 0042,
   _Motivation_). They can be skipped only for blocks something else has
   shown unreferenced. This RFC does not skip them (_Future work_); it
   builds what that would rest on and the monitors to measure it.

### Why this is not RFCs 0001 to 0029 again

Those RFCs inferred ownership in C nobody was allowed to change, and RFCs
0030 to 0034 then removed run-time checks where the inference claimed a
proof. RFC 0030 records the result of the first (150 of 1,619 definitions
after nine RFCs); the assessment before RFC 0035 found three classes of
wrong proof in the second. Three things differ here:

- **Declared, not inferred.** A signature and a field say which pointers
  own. The checker never guesses a function's contract from its body.
- **Rules that reject.** Code that does not fit is rewritten, or kept in
  a `WEAVEC_GUARDED` helper the audit lists. RFC 0041 and RFC 0042 showed
  agents converge on rules of this kind in one to four rounds.
- **Nothing rests on them at run time.** This is the one that matters.
  Every block still passes through the quarantine and the sweep, so
  (M3) of RFC 0042 holds whether or not the rules are right.

## Soundness

### Owners, borrows and references

In a safe function every pointer to heap memory is one of three things:

- An **owner**: a place declared `WEAVEC_OWNED` (a local, a parameter, a
  result, a field, an element), or a local that holds an owner's value
  (§2). It is responsible for releasing the block.
- A **borrow**: a pointer the function derived from an owner or from a
  parameter, kept in a local or passed down. Its validity is decided when
  the function compiles.
- A **reference**: a pointer loaded from a place that is not an owner (a
  field without the annotation, a global). Nothing is decided about it
  when the function compiles; it is checked when it is loaded.

Typed memory (RFC 0042, (W1) to (W4)) gains one clause:

- **(W5) Owned.** A place declared `WEAVEC_OWNED` holds null or a pointer
  into a live heap block that no other owner holds.

### The guarantee

For a program that meets RFC 0042's preconditions and O-A1 to O-A3 below:

- **(O1) One release.** Every release in a safe function consumes an
  owner: the function holds the owner, has not moved or released it on
  any path that reaches the call, and cannot use it afterwards. A safe
  function does not release a block it borrowed or loaded as a reference.
- **(O2) No use of what an owner no longer holds.** A safe function does
  not use an owner after moving or releasing it, and does not use a borrow
  after the owner it was taken from, or any owner above that one, was
  moved, released or replaced, by the function itself or by a call it
  made that could do so (§5).
- **(O3) No owner dropped.** On every path, a safe function moves or
  releases each owner it names (§4) before the owner goes out of scope,
  is assigned over, or the function returns; or the owner is null there.
- **(O4) References are checked when loaded.** A pointer a safe function
  loads from a place that is not an owner is tested, before the function
  first accesses memory through it or passes it on, not to point into a
  released heap block (`heap-use-after-free`).
- **(O5) What is left.** A use after free in a safe function can then go
  only through a pointer that was live when a safe function loaded it as
  a reference (or received it from code that is not safe, or read it in
  place below a global owner) and whose block was released while that
  function, or one it called, still used it. (M3) confines it to the
  released block, as today.

(O1) to (O3) are facts about the source. They hold in whatever build the
program ships in, by whatever compiler, as long as the code that compiled
is the code that is built. (O4), (M1) to (M4) and every spatial check need
`weavec-cc`.

### What gets weaker

Nothing. No guard that exists today is removed, no block skips the
quarantine, and the sweep runs as it does. A typed unit of RFC 0042 that
compiles under this RFC runs the same checks plus (O4)'s.

What changes for existing code is that less of it compiles: a safe unit
must declare its owners (_Accepted rejections_).

### Accepted false traps

One. A safe function that loads a reference to a released block and does
more with it than compare or copy it stops ((O4)), even if no code would
have dereferenced it: a dangling pointer passed to a callee that only
uses it as a key. C leaves the value of such a pointer indeterminate; the
fix is to clear the reference when its block is released, or to keep the
key as an integer. _Unresolved question_ 3 asks whether the exemption is
drawn in the right place.

### Bugs caught

```c
/* weavec-cc -fweavec-safe */
#include <weavec.h>
#include <stdlib.h>

struct item { char *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) name; size_t cap; int v; };
struct list { struct item *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(n) items; size_t n; };
struct cache { struct item *last; };          /* a reference */

struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);

int after(void) {
  struct item *it = item_new(1);              /* 'it' is an owner */
  if (it == NULL) return -1;
  item_free(it);
  return it->v;       /* error: 'it' is used after it was moved to
                         'item_free' [weavec-safe:owner-moved] */
}

int leak(int c) {
  struct item *it = item_new(4);
  if (c > 100) return -2;  /* error: returning while 'it' may still own
                              its block [weavec-safe:owner-leak] */
  item_free(it);
  return 0;
}

void drop(struct item *WEAVEC_OWNED it) {
  free(it);           /* error: 'it->name' may still own its block when
                         'it' is released [weavec-safe:owner-leak] */
}

int grow(struct list *l, struct item *WEAVEC_OWNED it) {
  struct item **first = &l->items[0];         /* a borrow below l->items */
  struct item *WEAVEC_OWNED *bigger = realloc(l->items, (l->n + 1) * sizeof *bigger);
  if (bigger == NULL) { item_free(it); return -1; }
  bigger[l->n] = it;
  l->items = bigger;
  l->n = l->n + 1;
  return (*first)->v; /* error: 'first' points into what 'l->items' owned
                         before 'realloc' released it
                         [weavec-safe:stale-borrow] */
}

void remember(struct cache *c) {
  struct item *it = item_new(6);
  c->last = it;       /* a reference to the block; 'it' still owns it */
  item_free(it);
}
int recall(struct cache *c) {
  return c->last->v;  /* runs: heap-use-after-free when 'c->last' is
                         loaded, in every check mode */
}
```

### Bugs deliberately not caught

- **A reference released under a running function.** A safe function
  loads a reference, the test of (O4) passes, and the block is released
  before the function's last use of it: by a call it makes, through the
  block's owner. The use reads the released block's own bytes ((M3)), as
  every use after free in typed code does today. `full` and `verify`
  report it.
- **A leak through a place the function does not name.** An assignment
  over an element at an index, or through a pointer to an owner the
  function did not name (`*pp`), is not checked for the block it may
  overwrite, and the release of an array of owners is not checked for the
  elements it still holds (§4). A cycle of owners, a block whose owner is
  alive at `exit`, and an owner held by code that is not safe are not
  reported either.
- **Shared ownership.** A block with several holders and a count is not
  modeled. Its holders are references, and its retain and release are
  guarded helpers (_Future work_).
- **Data races.** A thread that moves, releases or replaces an owner
  another thread is using (O-A3).
- **Owners broken by code that is not safe** (O-A1), on paths no test
  runs under `full` or `verify`.
- Everything RFC 0042 lists that is not temporal.

### Accepted rejections

A safe function rejects correct C that:

- releases a block through a pointer that is not its owner: unlinking a
  node of a doubly linked list from a pointer to the node alone, a `free`
  of a `prev` or a `parent`;
- owns conditionally: a field that is released unless a flag says the
  block belongs to someone else (cJSON's `cJSON_IsReference` and
  `cJSON_StringIsConst`), a parameter the function takes on success and
  leaves with the caller on failure;
- passes one call a structure it may change and a pointer into the same
  structure (`list_insert_after(list, node, new)`);
- keeps a pointer into a structure across a call that takes the
  structure as non-`const`, even when the call releases nothing: the
  pointer is taken again after the call, or the callee's parameter
  becomes `const`;
- copies a struct that holds owners and goes on using both copies;
- stores a string literal, or the address of a local or a global, in an
  owner.

Each is rewritten (the guide gives the accepted form, §12), or kept in a
`WEAVEC_GUARDED` helper that the audit lists. Gates G3 and G4 measure the
price in functions, lines and rounds, and S0 measures it on paper before
anything is built.

### Assumptions

T-A1 to T-A4 of RFC 0042 hold, with (W5) part of what T-A1 trusts code
that is not safe to keep. Owners add:

| | Assumption |
| --- | --- |
| **O-A1** | Code that is not safe (guarded and unsafe functions, code WeaveC did not compile) honors the declarations safe code sees: a `WEAVEC_OWNED` result is a block no other owner holds, a `WEAVEC_OWNED` parameter is released or kept in an owner, a parameter that is not `WEAVEC_OWNED` is not released, and owners reached through a pointer to `const` are not moved, released or replaced. |
| **O-A2** | The arguments with which code that is not safe calls a safe function do not overlap in the way rule `call-alias` forbids. |
| **O-A3** | No other thread, and no signal handler, moves, releases or replaces an owner while a safe function uses it or a borrow below it. |

## Detailed design

### 1. One safe tier

Ownership is stated over typed memory: a loaded owner is believed, as a
loaded bound is. Stating it for the tier of RFC 0041 as well, where every
loaded pointer is guarded because nothing is believed, would add a fourth
kind of function and nothing a user wants. The tiers become three:

| Tier | How | Compile time | Run time |
| --- | --- | --- | --- |
| **Safe** | `-fweavec-safe` for a unit, `WEAVEC_SAFE` on a definition | the rules of RFC 0041, RFC 0042 and §3 | RFC 0042's: comparisons against bounds, the shadow at entries from code that is not safe and where a bound is unknown; and (O4)'s test |
| **Guarded** | the default; `WEAVEC_GUARDED` in a safe unit | none | guards |
| **Unsafe** | `WEAVEC_UNSAFE` | none | none |

- `-fweavec-safe` means what `-fweavec-typed` meant. `-fweavec-typed` is
  removed; the driver rejects it with a message that names
  `-fweavec-safe`.
- `WEAVEC_SAFE` on a definition in a unit without the flag makes that one
  function safe in this sense. It trusts the rest of its unit as RFC 0042
  T-A1 says; RFC 0042 rejected that default as "silently weakening
  programs mid-conversion", and the answer is the one its guide already
  gives: convert under `-fweavec-safe-checks=full`, which keeps every
  guard, and read the audit. The held-out conversions and the port of RFC
  0042 went straight to typed C in three to four rounds; nobody needed
  the step between.
- The code of the deleted tier goes: the paths of `GuardSafe.cpp` and
  `SafeCheck.cpp` that are taken only when a safe function is not typed,
  their tests, and the "typed function" vocabulary in the documentation,
  the ledger (`tier` is `safe`, `guarded` or `unsafe`) and the summary
  line. "Typed memory" stays the name of the invariant.
- `-fweavec-safe-checks=bounds|full` and `-fweavec-checks=verify` are
  unchanged.

`weavec-c++` rejects the flag as before (§13).

### 2. Owners

`WEAVEC_OWNED` goes after the `*` of a pointer type, like the bounds
annotations, on a field, a parameter, a function's result, a local, a
global, or the element type of an array of pointers:

```c
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
struct vec *WEAVEC_OWNED vec_new(void);
void vec_free(struct vec *WEAVEC_OWNED v);
int vec_take(struct vec *v, int *WEAVEC_OWNED *out);   /* writes an owner */
```

- **A result** marked `WEAVEC_OWNED` gives the caller a block to release.
- **A parameter** marked `WEAVEC_OWNED` takes the block: the caller's
  owner is moved, and the callee holds an owner it must move or release.
  A parameter without it is borrowed for the call, as RFC 0041 §5 says;
  `WEAVEC_ESCAPES` keeps its meaning (the callee keeps a _reference_).
- **A field or an element** marked `WEAVEC_OWNED` owns: releasing the
  struct or the array without first moving or releasing it is a leak.
- **A local** is an owner when it is declared `WEAVEC_OWNED`, or when its
  initializer, or the first value assigned to it, is an owner's value
  that can go nowhere else: the result of a call that returns an owner.
  `struct item *it = item_new(1);` needs no annotation. A local that
  takes a block _out of a place_ says so:
  `struct node *WEAVEC_OWNED n = *pp;` moves; `struct node *n = *pp;`
  borrows. A local is an owner or not for its whole scope.
- **The library table** says which calls make and take owners: a result
  flag `owned` on the allocation family (`malloc`, `calloc`,
  `aligned_alloc`, `strdup`, `strndup`, `realpath` with a null buffer and
  their kin), the existing parameter flags `release` (consumes an owner)
  and `realloc` (§8), and a parameter flag `owner` for a pointer to a
  place the call stores an owner in (`asprintf`, `getline`,
  `posix_memalign`).
- **Arithmetic on an owner** gives a borrow. `p++` on an owner is
  rejected (`owner-source`): an owner keeps pointing where its block can
  be released from.
- A fresh block is zero (RFC 0042 §2), so its owners are null; a local
  that holds pointers is zeroed too. Null is an owner that owns nothing,
  and `free(NULL)` consumes it.

A struct that holds owners (in its fields, or in structs inside it) is
moved when it is copied: `*a = *b` moves each owner of `*b`, and
`memcpy` between two such objects is rejected (`owner-source`).

### 3. The rules

Each has a stable identifier, a fix note, tests under `test/Safe/` and a
page `--weavec-explain` prints (§12).

| Id | Rule |
| --- | --- |
| `owner-moved` | An owner is not used after it was moved (passed to a `WEAVEC_OWNED` parameter, returned, stored in another owner) or released, on any path that reaches the use: not read, not passed, not released again. A struct is not passed to a call, returned, or left behind at a return while an owner in it that the function names is moved and not yet assigned. Assigning to a moved owner (`p = NULL` included) makes it usable again. |
| `owner-leak` | An owner the function names holds no block when it is dropped: when its scope ends or the function returns, when it is assigned over, when the struct that holds it is released or overwritten, and when a call's `WEAVEC_OWNED` result is discarded or goes to a place that is not an owner. "Holds no block" is decided by the function: the owner was moved or released on every path, or a test on the path showed it null. Calls that do not return (`exit`, `abort`) drop nothing. |
| `owner-source` | Only an owner's value goes into an owner. A borrow, a reference, the address of a local or a global, a string literal and the result of pointer arithmetic are not stored in an owner, passed for a `WEAVEC_OWNED` parameter or returned as a `WEAVEC_OWNED` result; one assignment moves from one named owner (not from `c ? a : b`); a struct that holds owners is not copied by `memcpy`. |
| `owner-path` | An owner is moved, released or replaced only along the path that owns it: from a local owner, or from a parameter that is not a pointer to `const`, through owners. Not through a pointer to `const`, not through a reference, and not below a global owner read in place (move the global into a local, use it, and move it back). |
| `owner-slot` | An owner the function does not name exactly (an element at an index, `*pp`, a field reached through a borrow of unknown origin) is moved out of, or released, only in a statement followed at once by an assignment to the same place: `free(a[i]); a[i] = NULL;`, `struct node *WEAVEC_OWNED n = *pp; *pp = n->next;`, `x = a[--n]; a[n] = NULL;`. |
| `stale-borrow` | A borrow is not used after an owner it was taken below was moved, released or replaced by the function, nor after a call that could have done so (§5). Taking the pointer again after the call makes it usable. |
| `call-alias` | In one call, an argument through which the callee could change owners (§5) does not point to the same object as another pointer argument, nor above or below it, as far as the function knows where both came from. |

The rules `escape`, `field-address`, `counted-pair` and the others of RFC
0041 and RFC 0042 are unchanged. `counted-pair` and `owner-slot` use the
same notion of adjacent statements.

Where paths join, an owner that holds a block on one and was moved on
another is _partly moved_: a later use is `owner-moved`, and reaching the
end of its scope is `owner-leak`. Each error's notes name both paths.

The common C shapes fall out of these without a rule of their own:

- **`goto fail`.** Locals are zero when declared, so every owner at the
  label is null or holds its block, and the label releases each. An owner
  that was moved earlier is assigned `NULL` after the move, as careful C
  already does.
- **A constructor** allocates into a local, fills it, and returns it; on
  a failure halfway it releases what it built.
- **A destructor** takes its argument `WEAVEC_OWNED`, releases each owner
  field, then the struct. Forgetting a field is `owner-leak` at the
  struct's release.
- **A setter** (`p->name = strdup(s)`) releases the old block first, or
  is an `owner-leak`: the bug it usually is.
- **An initializer of caller-provided storage** (`vec_init(&v)`) assigns
  to owners that may hold a block as far as it knows; it releases them
  first (a `free` of null, since the storage is zero), or the function
  returns the struct instead.

### 4. The places a function names, and the ones it does not

The checker keeps a state (_null_, _holds_, _moved_, _partly moved_) for
each owner the function **names exactly**: a local or a parameter, and a
path of field selections and of dereferences of owners from one
(`it->name`, `c.buf`, `p->left->key`), with no index and no pointer whose
origin the function does not know. An implementation limit on the length
of a path (four selections, S0 confirms) turns longer ones into the other
kind.

On entry, an owner below a parameter _holds_ (it may be null; the
function has not been told). The function may move such an owner out and
put another in; rule `owner-moved` makes it whole again before any call
that could see it and before it returns.

Every other owner (an element `a[i]`, `*pp`, a field of a struct reached
by a pointer the function did not name) has no state. Rule `owner-slot`
keeps (W5) true of it at every statement boundary instead: it is moved
out of only together with its reassignment. An assignment _into_ such a
place is accepted without knowing what it held; that is the leak this RFC
does not see, and the price of writing `v->data[v->len++] = it` without
ceremony.

### 5. Borrows, and what ends them

A borrow remembers the owners it was taken below: `s = v->data` below
`v->data`; `n = t->root->left` below `t->root` and `t->root->left`; the
result of a call that took a `lifetimebound` argument, somewhere below
that argument. A borrow from a parameter that passes through no owner
(`p`, `&p->field`) is below none and lasts for the call, as today. A
pointer taken through a reference, or below a global owner read in place,
is a reference: these rules say nothing of it.

A borrow ends when:

1. **the function moves, releases or replaces an owner it is below**
   (`free(v->data)`, `v->data = d`, `realloc(v->data, …)` succeeding);
2. **the function moves out of, releases or replaces an owner it does not
   name exactly** (rule `owner-slot`): every borrow below the same local
   or parameter ends, except the pointer the place was reached through
   and those above it. The checker cannot tell `a[i]` from `a[j]`;
3. **the function makes a call that could change an owner it is below.**
   A call can change the owners in and below what an argument points to
   when the parameter is a pointer to a type that is not `const` and that
   holds an owner (in a field, or below one), or is a pointer to an
   owner. It can change nothing through a pointer to `const`, through a
   pointer to a type that holds no owner (a `char *` buffer, an `int *`),
   or through an argument that is not a pointer. A call to a function of
   the library table changes owners only as its row says. An indirect
   call is judged by the function pointer's type.

Using a borrow after it ended is `stale-borrow`, with a note at what
ended it. Assigning the local again starts a new borrow.

This is why `const` matters and why rule `owner-path` exists. A callee
handed `const struct tree *` cannot release a node, so a caller's pointer
into the tree survives the call; and no safe function changes owners
through a reference, so a callee that was not handed the tree at all
cannot reach it to change it. Together with `call-alias`, which makes a
callee's parameters distinct as far as owners go, that is what makes (O2)
a statement about the whole call tree and not only about one function.

`call-alias` is checked where the function knows both arguments' origins:
the same local, parameter or owner, one above the other. Arguments of
types that hold no owner are never in conflict (`memmove(b, b + 1, n)` is
not this rule's business), and a reference is not compared with anything.

### 6. Calls into and out of safe code

- **A safe callee** believes its parameters: a `WEAVEC_OWNED` parameter
  is a block it now owns, and the others stay valid for the call.
- **Code that is not safe** calling a safe function is trusted for the
  ownership of what it passes (O-A1, O-A2). The entry's checks against
  the shadow (RFC 0042 §6) still run, so an argument that points into a
  released block is stopped there.
- **A safe function calling code that is not safe** believes the
  declaration it sees: its `WEAVEC_OWNED` annotations, its `const`. A
  declaration without annotations borrows everything and returns
  references, which is what C that knows nothing of owners does.
- **Function pointers** carry the annotations in their type
  (`void (*release)(void *WEAVEC_OWNED)`), and converting between
  function pointer types that differ in them is `pointer-cast`.

### 7. References

A pointer field without `WEAVEC_OWNED` (and without `WEAVEC_BORROWED`,
which is a borrow kept in a local struct) is a reference. Storing one is
always allowed: `c->last = it` leaves `it` the owner and makes `c->last`
a second pointer the rules do not follow. `WEAVEC_ESCAPES` on a parameter
says the callee does that with its argument.

When a safe function loads a pointer from such a field, and uses it for
anything but a comparison or a copy into another reference, the pass
tests the shadow byte of the address it holds: if it says _released heap
block_ (`WeavecRtShadowHeapFreed`), the program stops with
`heap-use-after-free`. The test is exact for the heap: a released block
is not reused while something refers to it, so its shadow still says
released whenever the reference is loaded. A null pointer, a pointer one
past the end of a live block (it lies in the block's own slot), and a
pointer into memory the runtime does not track pass.

The test is one shadow load and a compare for each load of a reference
field that is used, not for each access through it. It restores, for
every check mode, most of what RFC 0042 gave up ("no report of a use
after free"), at a small fraction of the cost of the guards RFC 0042
removed, because owners need none: in zlib's `deflate_state`, `window`,
`prev`, `head` and `pending_buf` are owners, and `strm`, `pending_out`
and the caller's `next_in` are references loaded a few times a call.
Gate G7 bounds the cost.

A pointer loaded from a place no declaration bounds (`*pp`, an element of
an array of pointers outside a struct) keeps its guard, as today
(`typed-unbounded`), which already covers it.

### 8. `realloc`

`d = realloc(p, n)` consumes `p` if and only if `d` is not null. The
checker keeps that: after the call `p` is _moved if `d`_; the branch of a
test of `d` where it is null has `p` holding its block again, and the
other has it moved. Using `p` before `d` was tested is `owner-moved`, and
`p = realloc(p, n)` is `owner-leak` (the classic one: on failure the
block is lost). `reallocf` and `reallocarray` follow their rows.

The pair RFC 0042 requires composes with it:

```c
int *d = realloc(v->data, cap * sizeof *d);
if (d == NULL) return -1;     /* v->data still owns its block */
v->data = d;                  /* was moved by realloc; now owns d */
v->cap = cap;
```

### 9. Globals

A global may be an owner or hold owners. No function has it to itself,
so it is not a place a function names: rule `owner-path` has safe code
move it into a local (`struct state *WEAVEC_OWNED s = g; g = NULL;`), use
it, and move it back, and while it is out a callee that looks finds null.
Read in place, what it points to is a reference: usable, checked when
loaded, with no claim (O5). The audit lists global owners read in place.

### 10. How it is checked

- **Where.** A new file, `lib/Frontend/SafeOwnership.cpp`, called by the
  checker for each safe function after the existing rules, on Clang's
  `CFG` (`clangAnalysis`, a library WeaveC does not link today) built
  with scope ends.
- **What.** One forward dataflow pass to a fixed point, in reverse
  post-order. The state is a map from named owner places to their state,
  with `realloc`'s condition; and, for each local pointer, the owners it
  is below or that it has ended (and where). Joins are pointwise. Branch
  conditions refine null tests of owners and of `realloc`'s result
  (`p`, `!p`, `p == NULL`, `p != NULL`).
- **Errors** are reported in a final pass over the converged states, once
  per place and rule, at the first use in source order, with a note at
  the move, release or call that caused it and the fix. An expression the
  checker does not model for owners (a comma expression, a statement
  expression, an owner through `?:`) is rejected as `owner-source` with a
  note, not guessed at.
- **Nothing reaches the passes** but two bits: the `weavec.field`
  annotation's flags (RFC 0042 amendment 5) gain _owner_, and the bounds
  table gains _owned_ for parameters and results (for the ledger). The
  pass's only new code is the test of §7.
- **Clang's own lifetime-safety analysis** is not used: its fact
  generator is internal, its C mode does not model `free`, and its
  origins are C++ ones. Its structure (loans, origins, a liveness pass)
  is the model for this file, and S0 looks again at reusing it.
- **Budget.** About 2,200 lines for the checker, 150 for the pass, the
  table and the driver, 150 for the ledger and the audit, less what §1
  deletes (S0 measures; about 300). The library's limit becomes 16,500
  (14,200 today, full); the runtime's stays 10,500.

### 11. `verify`, the ledger and the audit

- **The monitors already exist.** `-fweavec-checks=verify` keeps every
  guard a safe function dropped as a monitor, and
  `-fweavec-safe-checks=full` keeps them as guards. A use after free
  through an owner or a borrow that the rules should have rejected is
  therefore a `weavec.proven` report in a test run, today (bugs 1, 2 and
  5 of the probe). Gate G8 runs the seeded bugs that compile under
  `verify`: one that reports through an owner or a borrow is a hole in a
  rule, found by the runtime, and fixed before the RFC is Implemented.
- **The ledger** (version 6): `tier` values as §1; a `fields` list of
  each pointer field of the structs the unit defines, with its kind
  (`owner`, `reference`, `borrowed`); the reason `reference` for the test
  of §7. The summary line gains the counts of owner and reference fields.
- **The audit** (`weavec-audit` version 2) adds what the ownership
  guarantee rests on, to be read as RFC 0042's list is: the reference
  fields, the global owners read in place, and each function that is not
  safe and has a `WEAVEC_OWNED` parameter or result (the helpers (O-A1)
  trusts).

### 12. The loop: explanations, and the guide

A model writes Rust from what it was trained on. It writes this dialect
from what the compiler tells it, so the compiler has to carry the manual.

- **`weavec-cc --weavec-explain <rule>`** prints a rule's page: what it
  rejects, why, a rejected example and its accepted rewrite, and the
  shapes that usually trip it. `--weavec-explain all` prints every page
  (the brief an agent reads once before a conversion), and
  `--weavec-explain` alone lists the rules. Every rule of RFC 0041, RFC
  0042 and this RFC has a page.
- **One source.** The pages live in `resources/safe-c/rules.md`, one
  section per rule; CMake embeds the file in the compiler as it embeds
  the library table, the documentation site renders it as the rules
  reference, and `docs/scripts/check-examples.py` compiles every example
  in it and checks that the rejected ones fail with the rule they name.
- **The first error of a compile** carries one extra note:
  `note: 'weavec-cc --weavec-explain owner-moved' explains this rule`.
- **The guide** (`docs/pages/guides/safe-c.md`) is rewritten around one
  tier, with an ownership section and the building blocks an agent
  starts from, each of which compiles: a vector of values and a vector
  of owners (push, pop, swap-remove), a string builder, a tree with
  owned children and a reference to the parent, a singly linked list
  edited through `struct node *WEAVEC_OWNED *`, a hash map with owned
  chains, an LRU cache (owned by the map, ordered by references), a
  constructor and destructor pair with `goto fail`.
- `guarantees.md` gains (W5), (O1) to (O5) and O-A1 to O-A3; `cli.md`
  the flag changes, `--weavec-explain` and the report line of §7;
  `docs/annotations.md` the macro.

### 13. C++

Not in this RFC, and nearer after it. A safe C++ needs exactly these
three kinds: `std::unique_ptr` and containers are owners, references and
`std::span` are borrows, raw pointers kept in members are references.
`stale-borrow` with a container's non-`const` member functions as the
calls that end borrows is iterator invalidation. The C rules are the
smaller language to get them right on; the C++ RFC should be written
against them.

### 14. Stages and gates

- **S0 On paper, and probes.** The rules applied by hand to the typed
  conversions of cJSON, sds and zlib's `deflate.c` and to the `lzw` port
  (`build/rfc43/baseline/`): every pointer field classed owner, reference
  or borrowed, every release site and every rejection recorded by rule. A
  throwaway dataflow over Clang's CFG for `owner-moved` and `owner-leak`
  on the probe, to confirm the CFG gives what §10 needs. The seeded-bug
  set for G2, two held-out projects with at least 50 release sites
  between them, and a port whose data structure has real ownership (an
  LRU cache or a B-tree, from a language that is not C or Rust) are
  chosen and frozen. The lines §1 deletes are counted. Thresholds below
  are confirmed or changed here.
- **S1 One safe tier** (§1), so that what follows lands on one.
- **S2 Owners**: the annotation, the table's flags, states of locals and
  parameters; `owner-moved`, `owner-leak`, `owner-source`; `realloc`.
- **S3 Fields and slots**: named places, structs made whole before calls
  and returns, `owner-slot`, `owner-path`, globals.
- **S4 Borrows**: what each is below, what ends it, `stale-borrow`,
  `call-alias`.
- **S5 References**: the test of §7, the ledger and the audit.
- **S6 Explanations and the guide** (§12), `guarantees.md`, `cli.md`.
- **S7 Conversions, the held-out projects, the port and the seeded
  bugs**, by agents; the gates on darwin-arm64, linux-aarch64 and
  linux-x86_64.

The gates:

- **G1 Rules.** Each rule has accepted and rejected tests, a fix note
  and an explanation whose examples are checked.
- **G2 Seeded bugs.** The measure of the owner's goal. A frozen set of at
  least 80 temporal bugs, seeded one at a time into the converted
  projects and the port by a script and by an agent asked for plausible
  mistakes, in these classes: a release removed on one path; a release
  duplicated; a use moved after a release; a member's release removed; an
  owner assigned over; a borrow kept across a release, a `realloc` and a
  call that reallocates; a released pointer returned or stored. Of the
  bugs inside safe functions on owners and borrows, at least 90% stop the
  compile with the right rule. Of the rest, each is reported at run time
  by (O4) or by the project's tests under `full`, or is listed with the
  reason. Today: none stops the compile.
- **G3 Conversion.** The eight conversions of RFC 0042 and its port
  compile under this RFC after an agent given only the guide and
  `--weavec-explain` adds ownership; their tests pass in the three check
  modes. Each keeps at least 90% of its functions safe, and changes at
  most 5 points more of its project's lines. Recorded: rounds, errors by
  rule, and each rejection the agent classed as a real defect, a rewrite,
  or a helper.
- **G4 Held-out.** The two new projects, from the original: at least 80%
  of functions safe, tests pass.
- **G5 The port, against Rust again.** One protocol, safe C and Rust,
  from the same specification and tests. Safe C: at least 95% of
  functions safe; tests pass in the three modes with no temporal report.
  Recorded and compared: rounds, time, lines, the benchmark (within 1.25
  times the Rust port, as RFC 0042 T7), and the reference fields the C
  port needed where the Rust port needed `Rc`, an index or `unsafe`.
- **G6 Defects found.** Every `owner-leak`, `owner-moved` and
  `stale-borrow` error of the first round of G3 and G4 that the agent
  classes as a defect in the original project is reproduced (a leak under
  a leak checker, a use after free under `full`) and recorded. The count
  may be zero; it is reported either way.
- **G7 Cost.** The conversions' benchmarks within 3% of RFC 0042's T
  column, each; `-fsyntax-only` on each conversion at most 10% slower
  than without the ownership pass.
- **G8 Verify.** No `weavec.proven` in the conversions' tests, and none
  through an owner or a borrow among the seeded bugs that compile.
- **G9 No change elsewhere.** A unit with no safe function guards and
  proves the same accesses as on main; the corpus gate's results do not
  change.
- **H1 Hygiene.** The library within 16,500 lines, the runtime within
  10,500.

## Annotation surface

`weavec.h` becomes version 3.0.

- **New: `WEAVEC_OWNED`**, after the `*` of a pointer type. It expands to
  `[[clang::annotate_type("weavec.owned")]]` under Clang (through
  `WEAVEC_TYPE_ANNOTATE_`, so a build that must not see `[[…]]` can
  define it away) and to nothing elsewhere: owned C is plain C. The
  spelling lives in `weavec.h` and as `OwnedAnnotation` in
  `include/weavec/Frontend/SafeCheck.h`.
- **Changed: `WEAVEC_SAFE`** and `-fweavec-safe` name the one safe tier
  (§1). On a declaration, `WEAVEC_SAFE` keeps RFC 0042's meaning (a safe
  unit defines the function), in any unit with a safe function.
- **Unchanged in spelling, sharper in meaning:** `WEAVEC_ESCAPES` (the
  callee keeps a reference to the argument), `WEAVEC_BORROWED` (a borrow
  kept in a local struct), `WEAVEC_LIFETIMEBOUND` (the result is a borrow
  below the argument).

No annotation is added for references: a pointer field is one unless it
says otherwise, and the audit lists them.

## Diagnostics

**Compile-time errors**, in the form of RFC 0041, each pinned by a
`test/Safe/` test:

```
item.c:13:10: error: 'it' is used after it was moved to 'item_free' [weavec-safe:owner-moved]
item.c:12:3: note: moved here; use it before this call, or keep what you need from it in a local first
item.c:13:10: note: 'weavec-cc --weavec-explain owner-moved' explains this rule

item.c:18:16: error: returning while 'it' may still own its block [weavec-safe:owner-leak]
item.c:17:21: note: 'it' has owned the result of 'item_new' since here; release it (a function taking 'struct item *WEAVEC_OWNED'), return it as a 'WEAVEC_OWNED' result, or store it in an owner before this return

item.c:24:3: error: 'it->name' may still own its block when 'it' is released [weavec-safe:owner-leak]
item.c:24:3: note: release 'it->name' first: 'free(it->name);'

list.c:35:11: error: 'first' points into what 'l->items' owned before 'realloc' released it [weavec-safe:stale-borrow]
list.c:29:28: note: released here when the result is not null; take the pointer again afterwards: 'first = &l->items[0];'

tree.c:41:14: error: 'n' was taken from 'tree' before the call to 'tree_insert', which may release or replace what 'tree' owns [weavec-safe:stale-borrow]
tree.c:40:3: note: take 'n' again after the call, or declare the parameter 'const struct tree *' if 'tree_insert' changes no owner

cache.c:9:13: error: 'item_new' returns an owner and 'c->last' is not one: the block would have no owner [weavec-safe:owner-leak]
cache.c:9:13: note: declare the field 'struct item *WEAVEC_OWNED last', or keep the result in a local owner and store a reference to it

set.c:7:15: error: 'p->name' is an owner and 's' is borrowed for this call [weavec-safe:owner-source]
set.c:7:15: note: store a copy the struct can own ('strdup(s)'), or take the string as 'char *WEAVEC_OWNED s'

dll.c:22:3: error: 'node->prev->next' is reached through the reference 'prev' and cannot be replaced from here [weavec-safe:owner-path]
dll.c:22:3: note: change an owner from the pointer that owns it: walk to the node from the list's head, or keep this function WEAVEC_GUARDED

vec.c:30:21: error: 'v->items[i]' is moved out and still holds the pointer [weavec-safe:owner-slot]
vec.c:30:21: note: assign the element in the next statement: 'v->items[i] = NULL;'

list.c:50:3: error: 'at' points into 'list', which 'list_insert_after' may change through its first argument [weavec-safe:call-alias]
list.c:50:3: note: pass only the pointer the callee changes, or make the other parameter 'const'
```

**Run-time report kinds.** None is new. `heap-use-after-free` gains one
access wording, for the test of §7:

`weavec: heap-use-after-free at <file>:<line>:<col>: reference to <address> loaded from a field`

followed by the stack and the released block's paragraph as for any other
`heap-use-after-free`. A `test/Guards/reference-load.c` pins the line;
`cli.md` describes it; `scripts/run-cases.py` already knows the kind.

```c
struct cache { struct item *last; };
int recall(struct cache *c) { return c->last->v; }  /* c->last released */
```

The test's slow path is a new runtime entry point
(`__weavec_rt_reference_fail`), declared in `weavec_rt.h` and listed in
`weavec_rt.exports`; the runtime ABI becomes 43.

## Drawbacks

- **Existing safe code stops compiling.** Every typed unit written
  against RFC 0042 needs its owners declared before it builds again.
  That is the point, and before 1.0 it is affordable, but it is the
  largest break since RFC 0035.
- **More C is rejected.** Conditional ownership, release through a back
  pointer and a container passed with a pointer into it are common in
  libraries that exist. Each becomes a rewrite or a guarded helper, and
  G3's thresholds may not hold for cJSON and sds.
- **A flow-sensitive checker.** RFC 0041's rules reject constructs; these
  follow state along paths. That is where RFCs 0001 to 0029 grew without
  bound. The limits here are that nothing is inferred across functions,
  nothing is proved about values other than null tests, and an expression
  the checker does not model is rejected, not approximated.
- **`const` starts to matter.** A callee that only reads but does not say
  `const` ends its callers' borrows. The fix improves the code, and
  cascades through signatures the way `&` and `&mut` do.
- **A second way to stop when loading a pointer.** A program that keeps a
  dangling reference and uses it for something the test does not exempt
  (it passes the pointer to a function that only compares it) stops
  where it ran before.
- **Leaks through elements are still silent**, which is where a vector of
  owners leaks.
- **About 2,300 more library lines**, a third budget increase in three
  RFCs, and a new Clang library to link.
- **No speed.** This RFC makes nothing faster.

## Alternatives

- **Do nothing; the sweep is the temporal story.** Memory safe, and
  silent. It leaves the half of Rust's loop that people mean when they
  say "the borrow checker" to tests the agent has to think of.
- **Make release optional: a collector.** The sweep already marks
  everything reachable; typed C with `free` as a hint is Go's model, and
  removes use after free, double free and leaks as classes. It gives up
  what makes C the alternative to Rust rather than to Go (no collector,
  memory released when the program says), makes safe C wrong under every
  other compiler, and gives the agent no error to read.
- **Infer ownership** (RFCs 0001 to 0029; the Clang static analyzer; GCC's
  `-fanalyzer`). Measured above: none of four across a file boundary,
  three of four once two declarations carry attributes. Warnings from a
  path-sensitive search are useful and are not a contract: there is no
  rule to write code against and no statement of what a clean run means.
- **Build on Clang's lifetime-safety analysis.** Measured above: in C it
  covers the stack. It will grow, and S0 looks at it again; today its
  facts cannot be extended from outside Clang.
- **Rust's model in full**: lifetime parameters on types and functions,
  shared and unique borrows that can be stored. It would make references
  static too. It needs syntax C does not have, and it is the part of Rust
  agents and people spend their rounds on; RFC 0041's lesson from Cyclone
  ("keep the lifetime system tiny") stands. Here a borrow lives in a
  local or goes down a call, and a pointer kept in memory is an owner or
  a reference the runtime checks. If conversions show what one more
  lifetime would buy, it is added then (_Future work_).
- **Ownership as warnings.** No break, and no guarantee: (O1) to (O3)
  could not be stated, and nothing later could rest on them.
- **Optional ownership**: check a pointer only once it is declared an
  owner, and let safe code release anything else. Existing typed units
  would keep compiling. But the first store of an allocation into an
  unannotated field would end the checking silently, which is where most
  blocks go; the checker would be a lint that looks like a rule.
- **Skip the quarantine for blocks an owner releases, now.** This is the
  speed the rules could buy: the allocator's floor on programs that
  allocate. It would make the rules load-bearing for memory safety on the
  day they are written. RFCs 0030 to 0034 did that with an analysis and
  the assessment found wrong proofs. First the rules, with `verify`
  watching them on real conversions; then an RFC that spends them.
- **Checks instead of rules for references**: a generation number beside
  every reference (Vale), or a count in every block. Exact, and several
  instructions and a word per pointer. The sweep already keeps a released
  block's shadow truthful for as long as anything refers to it, so one
  shadow byte at the load gives the same answer.
- **Safe C++ next.** Most code that considers Rust is C++, and C++ has
  the containers. A safe C++ would need owners, borrows and references
  on its first day (§13) and would have to design them among templates,
  temporaries and the standard library's headers. Designing them in C
  first is cheaper and the result carries over.
- **The cost work RFC 0042 names next** (comparisons as loop ranges,
  typed callbacks and `void *`, the allocator). Each moves a number that
  is already near where it needs to be (1.45 times the reference, 1.18
  times Rust on the port). None changes what an agent sees when it
  compiles a mistake. They remain the next items after this one.
- **Release inserted by the compiler** at the end of an owner's scope
  (`__attribute__((cleanup))`, C2y's `defer`). Fewer errors to fix, but
  hidden calls that other compilers would not make, and destructors that
  are functions of the program's own, which the compiler would have to be
  told. An error with the missing call in its note costs the agent one
  round and keeps the program plain C.

## Prior art

- **Rust**: ownership and moves; borrows that conflict when one place is
  a prefix of another, and that end at the last use (non-lexical
  lifetimes); `Option::take` and `mem::replace` for moving out of a place
  that must stay valid; partial moves out of a struct. Taken: all of
  these as rules `owner-moved`, `stale-borrow`, `owner-slot` and the
  named places of §4. Not taken: lifetime parameters, borrows stored in
  the heap, `Send` and `Sync`.
- **The C++ Core Guidelines' lifetime profile** (Sutter, 2015 to 2019):
  Owners and Pointers, and a Pointer derived from an Owner invalidated by
  any non-`const` use of the Owner. That is rule 3 of §5, with `const`
  playing the same part. It warned; with typed memory beneath it the same
  rule can be an error.
- **Clang's lifetime-safety analysis** (2025 onward): origins and loans
  propagated over the CFG, modeled on Polonius. The structure §10
  follows.
- **Clang's `-Wconsumed`** (typestate for C++ classes: `consumable`,
  `callable_when`, `set_typestate`) and the analyzer's `ownership_returns`,
  `ownership_takes` and `ownership_holds`: typestate and ownership
  declared in signatures, the first as warnings on classes only, the
  second for a path-sensitive checker. WeaveC may read the second as
  aliases on declarations it cannot change (_Unresolved questions_).
- **Cyclone** (2002): unique pointers consumed by use, with an explicit
  swap to move out of a place that must stay valid, beside regions. Its
  unique pointers were the usable part; its regions the burden.
  **Vault** and typestate (Strom and Yemini, 1986): states of a resource
  checked along paths.
- **Hylo** (Val) and **Swift**'s `borrowing` and `consuming` parameters
  and its exclusivity rule: borrows that are never stored, only passed
  down, which makes lifetimes unnecessary. `call-alias` is Swift's "law
  of exclusivity" restricted to what owns. **Austral**: linear types with
  a borrow checker small enough to state in a page, the design goal here.
- **Vale**'s generational references and **CETS** (Nagarakatte et al.,
  2010): a liveness check where a non-owning pointer is used. (O4) is
  that check, at the load, paid for by the sweep instead of a key per
  pointer.
- **MarkUs** and RFC 0036: quarantine until unreferenced, the net under
  all of it.

## Unresolved questions

1. **Do the rules fit C that exists?** cJSON owns by flag, sds hands out
   a pointer into its block, zlib's state and stream point at each other.
   S0 applies the rules to each on paper; G3's 90% and 5 points are
   guesses until then.
2. **Ownership by outcome.** A function that takes its argument only
   when it succeeds, and an out-parameter that is set only on success,
   are rejected or cost a `free` of null. RFC 0010 went down this road;
   how often conversions meet it decides whether a result-conditional
   form is worth one annotation.
3. **False stops from the reference test.** It exempts comparisons and
   copies. Is that the right line, or do conversions pass dangling
   references to functions that never dereference them?
4. **`const`.** How many `stale-borrow` errors in the first round are
   only a missing `const` on a callee, and should the error offer the
   fix-it?
5. **Inferred owner locals.** A local initialized from an owner's call
   is an owner without saying so. Is that clearer than requiring the
   annotation everywhere, for a reader and for an agent?
6. **Path length and precision.** Are four selections enough, and how
   often does rule 2 of §5 (an unnamed owner ends every borrow below the
   same root) force a pointer to be taken again?
7. **Allocation hooks and `void *` contexts.** cJSON calls
   `global_hooks.deallocate`; a function pointer's type can say
   `WEAVEC_OWNED`, but an owner passed as a `void *` callback context is
   still RFC 0042's open question 6.
8. **Reading Clang's `ownership_*` attributes** and GCC's
   `malloc(deallocator)` as aliases on system and third-party headers.
9. **The merge of tiers.** Does anything use `-fweavec-safe` without
   `-fweavec-typed` and want to stay there? Nothing known.
10. **Compile time** of the dataflow on functions the size of zlib's
    `inflate`.
11. **The seeded-bug set**: how to keep it honest (frozen before S2,
    scripted where possible, classes weighted as bugs occur in the wild
    rather than as the rules catch them).

## Future work

- **Release without the quarantine.** A block released through an owner
  that no reference was ever taken to can be reused at once. With owners
  checked and `verify` monitoring each such release against the next
  sweep, an RFC can measure how often that holds and then spend it: the
  allocator's floor (0.25 to 0.31 of the reference's time where programs
  allocate) is the largest cost Safe C has left.
- **Safe C++** on owners, borrows and references (§13).
- **Shared ownership**: counted owners, checked like owners with a
  retain that makes one.
- **One more lifetime**: a reference field tied to an owner beside it
  (an arena and what was allocated from it), if conversions need it.
- **A leak report at exit**, from the sweep's mark, for the leaks §4
  does not see.
- **Other resources**: `FILE *`, descriptors and locks as owners, by
  rows of the library table.
- **Fix-its** for the notes that name the missing statement, and
  `defer` when C has it.
- **Data races**, and the cost items RFC 0042 lists.

## Implementation amendments

These record where the implementation departs from the design above, from
S0's paper review of cJSON, sds, zlib and the `lzw` port (its findings M1
to M20, kept in `build/rfc43/s0/`), from building it, and from what the
conversion agents reported (S7). They override the body.

1. **One tier, as built (§1).** Every C unit the checker runs on gets RFC
   0042's field annotations; they are removed before the optimizer runs, so
   guarded code compiles as before (gate G9). A unit registers the program's
   typed defaults (no allocation stacks) only when it defines a safe
   function. The ledger's removal reason stays `typed`; there is no `safe`
   reason any more. RFC 0041's tier was not a separate body of code: its
   differences were branches on the unit's flag, which are gone.
2. **Owner locals (§2).** A local is an owner when declared one, when a
   call that returns an owner is assigned to it anywhere in the function
   (`char *p = NULL; ... p = malloc(n);`, S0's M12), or when its address is
   passed where a call stores an owner (`getline(&line, ...)`); but a local
   whose initializer is something else (`char *b = stack_buffer;`) is a
   borrow, and an allocation assigned to it later is `owner-leak` ("it
   started as one").
3. **The library table (§2).** The result flag is spelled `ptr:owned`. The
   `owner` parameter flag distinguishes, by the parameter's access, a call
   that replaces what was there (`w`: `asprintf`, which drops it) from one
   that reallocates it (`rw`: `getline`).
4. **Places through borrows (§4, §5).** A path through a local borrow is not
   named: moving an owner out through one is `owner-slot`. Whether it may
   change owners at all (`owner-path`) is decided by where the borrow comes
   from: one assigned only from parameters, owners and addresses reached
   through them (or a `?:` of such) may; one assigned a reference may not.
   A pointer read from a reference field is a reference: nothing the
   function does to its own owners ends it.
5. **A call's argument (§5; S0's M2).** Owners below what a call may change
   (a pointer to non-`const` memory that holds owners) may hold blocks after
   it: the caller keeps answering for them. A new parameter annotation,
   **`WEAVEC_DROPS`**, says the opposite of a destructor of a struct the
   caller keeps (`void vec_free(struct vec *WEAVEC_DROPS v)`): after the call
   those owners are null, and the function is checked to leave them null at
   every return. An early design that forgot the owners after any such call
   (S0's choice "unchecked") was built first and dropped: the conversions
   showed every caller's missing destructor call compiling silently. A call
   given a reference where it may change owners below it is `owner-path`
   (S0's M6).
6. **Fresh blocks (§2; S0's M15).** Besides the library's allocations, an
   owned `void *` result (a program's allocator, an allocation hook) and an
   owned result of a function declared `__attribute__((malloc))` have null
   owners.
7. **Null tests (§10).** A test of an owner that took a place's block, found
   null, shows that the place held null (`n = l->head; if (!n) return;`). A
   branch the owner's known state rules out is not taken. The exit is
   reached along each edge, so a loop's test that leaves the function
   refines what it tested.
8. **More rejected forms, found by the conversions.** An owner moved to a
   call and passed to it again (`s = sdscatsds(s, s)`, upstream's use after
   free) is `owner-moved`; a pointer to owners passed where the parameter
   says neither `T *WEAVEC_OWNED *` nor `T *const *` is `owner-source`
   (S0's M9); a byte copy of memory that holds owners is `owner-source`, a
   fill with zero assigns them null after checking them; a struct copied
   from below a pointer to `const` is `owner-path`.
9. **What a `lifetimebound` call returns (§5)** is reached through that
   argument, for `owner-path` and `owner-slot`: an accessor that returns a
   pointer into its argument (zlib's state of a stream) needs no guarded
   helper. An owner field returned where the result is not an owner is a
   borrow, not a leak; a local owner returned so is `owner-leak`, at the
   return.
10. **Errors.** One error per move: after reporting a use, the place is
    quiet. Distinct ownership errors at one place (two owners leaked at one
    return) are each reported; other rules keep one error per place. A leak
    of an owner last stored where it is only a reference says so in its
    note. `call-alias` is reported once per call.
11. **References (§7).** The test exempts uses that are comparisons, copies
    into memory, and the bounds the pass built and did not use; fields of
    function pointers and of unsized `void` are not tested. A null
    reference's own field is read in its place, so the shadow of null is
    never read. The ledger row is `guarded` with the reason `reference`.
12. **The CFG (§10).** Clang's CFG with every expression an element and the
    ends of locals' lifetimes; a function whose dataflow does not settle in
    64 visits per block is not checked (none in the conversions). Clang's
    `clangAnalysis` library is linked.
13. **Measurement switches.** `-mllvm -weavec-reference-tests=0` and
    `-mllvm -weavec-ownership-rules=0` turn the tests and the rules off, for
    gate G7 only; they are not documented for users.
14. **Not built, from S0.** M1 (owners below a parameter that an init
    function fills, unset on entry: `WEAVEC_INIT`), M3 (ownership by
    outcome: a parameter taken only when the call succeeds), M4 (ownership
    by flag: cJSON's `IsReference`), M5 (an owner that points inside its
    block: sds's header), M7 (an unannotated pointer result below a
    parameter's owner), M8 (tests of references loaded from arrays of
    pointers), M10 (allocator families), M11 (annotations inherited from a
    header declaration), M13 (an owners-only `const`). Each costs the
    conversions a guarded helper where they meet it; each is _Future work_.
    Owners in a fixed array inside a struct are not checked when the struct
    is released.
15. **Budgets (H1).** The library's limit is 17,500 lines (17,397 used): the checker is about
    2,600 (the body estimated 2,200), the pass's test, the ledger, the
    audit and `--weavec-explain` about 300, and the tier merge deleted less
    than the body expected (amendment 1). The runtime stays within 10,500.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, `dev` preset) on 2026-11-11.
The conversions were made by Claude subagents (the session's model), each
given a brief, the rules page and the RFC, starting from RFC 0042's typed
conversions; they used a copy of the compiler frozen at their start, and
the compiler changed after them (amendments 5 to 9, from what they
reported). Every result below is the final compiler's: each library was
compiled again with it (one more round where it reported something: zlib,
bzip2, cJSON, the `lzw` port and the C port), and every project's tests
were rebuilt and run with it in the three modes (D default, F
`-fweavec-safe-checks=full`, V `-fweavec-checks=verify`). Rounds count whole
compiles of the library with `-fsyntax-only -fweavec-safe`. Lines changed
are added plus removed lines of the library against RFC 0042's typed
version, as a share of the original's lines. The working copies, records
and logs are in `build/rfc43/` (not committed).

| Project | Functions safe (guarded): RFC 0042 → 0043 | Lines vs typed | Rounds to zero (first annotated round's errors) | Tests, D/F/V | First-round errors that were real defects |
| --- | --- | --- | --- | --- | --- |
| jsmn | 6/6 → 6/6 (0) | 0 | 0 (0) | pass | none (no pointer fields) |
| http-parser | 18/18 → 18/18 (0) | 0 | 0 (0) | pass | none (4 reference fields) |
| sds | 48/53 → 47/54 (7) | 14.0% | 2 (38) | 46/46 | 3, all upstream: failure paths that lose the string (`s = sdsMakeRoomFor(s, …)`, `sdscatvprintf`, `sdssplitargs`' `vector = s_realloc(vector, …)`) |
| cJSON | 116/122 → 115/129 (14) | 22.5% | 4 (127) | 17/18, as RFC 0042 (`stack-escape` in `misc_tests`) | 1, upstream: `cJSON_AddItemReferenceToObject` leaks the reference when `strdup` fails |
| zlib | 153/170 → 155/172 (17) | 2.6% | 6 (120) | pass | none |
| tinyexpr | 47/51 → 47/51 (4) | 12.6% | 3 | pass | none |
| inih | 11/14 → 11/15 (4) | 6.4% | 2 | 15/15 configurations | none |
| bzip2 (held out by RFC 0042) | 61/73 → 64/78 (14) | 6.5% | 4 (48) | 12/12 | none |
| `lzw` port | 19/19 → 19/20 (1) | 2.8% | 2 | 971/971 | its benchmark driver's leaks on error returns (the port's, not Go's); one more found by the final compiler |

The port (G5): an LRU cache specified in `build/rfc43/g5/BRIEF.md` (a
doubly linked list and a hash map written by hand, eleven operations,
property tests against a model, a fixed benchmark), written by two agents
started at the same moment, one in Safe C and one in safe Rust.

| | Rounds to a clean build | Rounds to tests passing | Wall time | Library lines | Functions safe | Benchmark (×R, 20 runs, best of 3) |
| --- | --- | --- | --- | --- | --- | --- |
| Safe C | 2 (8 errors: `owner-path` 4, `escape` 2, `owner-leak` 2), and 1 with the final compiler (1 `owner-leak`) | 2 | 4 min 31 s | 337 | 22/22, no guarded helper; 1 owner field, 4 reference fields | 1.81 (1.71 without reference tests) |
| Rust (no `unsafe`) | 1 (0) | 2 | 1 min 31 s | 349 (286 without comments and blanks) | — | 1.32 |

R is the Safe C sources with `clang -O2`; both print the same line.

| Gate | Result |
| --- | --- |
| G1 Rules | **Pass**: `test/Safe/ownership.c` has accepted and rejected forms of every rule, each error a note; `resources/safe-c/rules.md` gives each rule a rejected and an accepted example, which `docs/scripts/check-examples.py` compiles. |
| G2 Seeded bugs | **Pass, on mechanical seeds.** `build/rfc43/g2/seed.py` seeds, at every release in a safe function of the nine conversions and the C port, a removed release, a duplicated one, a use right after it and a borrow read after it, and duplicates each allocation (372 mutants; 6 of them, a second call of zlib's `WEAVEC_DROPS` destructor and reads after it, are not bugs). Of the 366 bugs, 347 (94.8%) stop the compile with the expected rule and none with another. The 19 that compile: releases through cJSON's hooks after a call that leaves an owner unknown, tinyexpr's `te_free_parameters` (a helper that releases a fixed array of owners, which is not checked, amendment 14), and zlib's `deflateEnd` in two error paths (owners below a borrowed parameter are the caller's). The agent-written plausible bugs the body asked for were not made. Today: none stops the compile. |
| G3 Conversion | **Partly met.** Every project compiles and passes its tests in the three modes. The 90% of functions safe holds for jsmn, http-parser, zlib, tinyexpr and the port; not for cJSON (89.1%), sds (87.0%), inih and bzip2 (73% and 82%, as under RFC 0042). The 5 points of lines hold for jsmn, http-parser, zlib and the port; not for sds, cJSON, tinyexpr and inih, whose ownership by flag, by outcome and through an interior pointer (S0's M3 to M5, not built) was rewritten into safe code or guarded helpers. |
| G4 Held-out | **Not run.** The two held-out projects chosen (parson and c-algorithms) were fetched, but this session's permission check refused to have an agent convert freshly downloaded code; the owner decides. |
| G5 The port | **Partly met**: 100% of the C library safe with no helper, tests pass in the three modes, the same output. Safe C runs at 1.37 times the Rust port, over the 1.25 the gate sets; the reference tests are 0.10 of R of it, the allocator and the checks the rest. Rust took 1 round and 1.5 minutes, Safe C 2 rounds (3 with the final compiler) and 4.5. The C design needed 4 reference fields where the Rust one used indices into a vector. |
| G6 Defects found | Five defects in the original code, all leaks on failure paths: sds's three and cJSON's one above, and bzip2's `BZ2_bzclose` (when its second close fails), found while reading for the conversion. One use after free found at run time: zlib's `deflateReset` leaves `gzhead` pointing at a header the caller may free; a safe function's load of that reference stops with `heap-use-after-free`, and AddressSanitizer reports the same read in the original zlib (`build/rfc43/s7/zlib/probes/own/gzhead-uaf.c`). The leaks were not reproduced under a leak checker. |
| G7 Cost | **Run time, of the reference tests: pass** (each program with and without them, same sources and compiler: cJSON, http-parser, jsmn, `lzw`, sds 1.00; inih, tinyexpr 1.01; zlib 1.01; the C port 1.06). **Against RFC 0042's T: not met for sds and cJSON**: 2.09 and 2.11 times R against 1.69 and 1.90. RFC 0042's typed sources built with the final compiler run at 1.69 and 1.88, so the difference is in the conversions' rewrites (sds's guarded rebase helper, whose results are checked against the shadow; cJSON's list edits that now walk from the parent, and its helpers), not in the compiler. The others are within run-to-run noise of T (jsmn 1.26, zlib 1.15, inih 1.71, tinyexpr 1.16, http-parser 1.17, `lzw` 1.13). **Compile time: not met**: `-fsyntax-only` takes 1.36 to 1.54 times as long with the rules as without (`-mllvm -weavec-ownership-rules=0`) on zlib's `deflate.c` and `inflate.c`, `cJSON.c` and bzip2's files, 20 to 35 ms more per file. |
| G8 Verify | **Pass for the tests**: no `weavec.proven` in any conversion's tests in V (sds's probe of the upstream `sdscatsds(s, s)` use after free, outside its tests, reports it). The seeded bugs that compile were not run under V. |
| G9 No change elsewhere | **Pass**: `corpus-gate.py --quick` passes, and every config of the quick set guards, proves and leaves unguarded exactly the accesses it did in RFC 0042's run (`build/corpus-quick-42.json` against `build/rfc43/corpus-quick-43.json`). The Linux build and tests (Docker, `ci-release`) pass, and so does `ci-tidy` over the new code. |
| H1 Hygiene | **Pass with amendment 15**: library 17,397 of 17,500 lines, runtime 9,976 of 10,500. |
