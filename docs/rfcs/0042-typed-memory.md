# RFC 0042: Typed memory

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-10
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Adds a mode to the safe functions of
  [RFC 0041](0041-safe-c.md) and carries out what its _Measured gates_
  left for "the next RFC" (gate G5). Restores, for that mode, three parts
  of RFC 0041's body that its amendments 1, 5, 7 and 13 withdrew (bounds
  from annotated fields, the string rules, library calls compared against
  bounds). Amends RFC 0035 §6 and AGENTS.md's rule that a guard is removed
  only by a rule local to a function; RFC 0036 §3.4 and RFC 0037 §1 (what
  a sweep marks); RFC 0040 amendment 4 (where a release stack is kept) and
  its default for allocation stacks; the enforcement ledger; the runtime
  ABI.

This RFC was written at the request of the project owner on 2026-11-10,
who asked for the single change that would do most to make an AI rewrite
into C or C++ as attractive as an AI rewrite into Rust, to be implemented
next. The owner accepted its central trade (below) on 2026-11-11, and it was
built the same day; the measurements in _Motivation_ were taken on main at
v0.22.0, the gates under _Measured gates_ on the built design, and the
_Implementation amendments_ override the body.

## Summary

RFC 0041 gave an agent Rust's loop for C: rules checked at compile time,
errors with a fix, and conversions that converge in 2 to 10 rounds. It did
not give Rust's cost. Safe C runs at the cost of guarded C, 1.25 to 2.5
times the reference build, because between a quarter and a half of the
accesses in a safe function still read the shadow: every pointer loaded
from memory is checked as guarded code checks it, since guarded code may
have written anything there.

Rust does not check a reference it loads from a struct. Its types carry an
invariant (a `&[T]` in memory is a valid pointer and a true length), safe
code can only keep the invariant, and `unsafe` code promises to. This RFC
gives safe C the same footing. A unit compiled with **`-fweavec-typed`**
relies on **typed memory**: every pointer in memory that its code can
reach is null or satisfies the bound its declaration states. The rules
make every store in such a unit keep that true, so its loads need no
check. A _typed function_ then pays for one comparison where an index or a
count is known only at run time, and for nothing else: no shadow read, no
color check, no redzone around a local that stays in typed code, no entry
scan between typed functions.

The price is the one Rust charges. Code that is not typed (a guarded
helper, an unsafe function, a library WeaveC did not compile) is trusted
to keep memory typed, as `unsafe` Rust is trusted; the ledger lists every
such function so the trusted set can be read like a `grep unsafe`.
`-fweavec-safe-checks=full` and `-fweavec-checks=verify` keep every guard
as a check of that trust for tests. Units without the flag are unchanged.

Expected result (gates, §13): the converted programs of RFC 0041,
recompiled typed after the changes the new rules ask for, run within about
1.2 times the reference build where they ran at 1.25 to 2.5; and an agent
asked to port a small library into typed C and into Rust finishes both, in
a comparable number of compile rounds, with programs of comparable speed.

## Motivation

### Why cost decides whether a rewrite is worth making

A project weighing "have an agent rewrite this in Rust" against "have an
agent convert this to Safe C" compares what ships. The Rust program ships
with its guarantee at full speed. The Safe C program of RFC 0041 has a
guarantee only when built by `weavec-cc`, and built by `weavec-cc` it is
1.25 to 2.5 times slower, so a project that chose C for speed ships the
reference build and keeps WeaveC for tests. Then the conversion bought a
compile-time linter and a slower sanitizer, and guarded C, which needs no
conversion at all, is as safe at the same cost. RFC 0041's own gates say
so: G5 was not met, and "safe C costs about what guarded C does".

The other differences from Rust matter less or are already covered. The
conversions change 2 to 19% of a project's lines where a Rust rewrite
changes all of them, keep its ABI, its build and its contributors, and
converge in as few rounds. What is missing is a reason to run the result.

### Where the time goes

Measured on darwin-arm64 on 2026-11-10 with the v0.22.0 compiler, on the
six converted projects of RFC 0041 with their benchmarks, best user CPU of
three serial runs, as multiples of the reference build R (`clang -O2`,
system allocator). G is the original guarded, S the conversion under
`-fweavec-safe`, and A the reference build's objects linked by `weavec-cc`:
no guard at all, WeaveC's allocator and sweep.

| Project  | G    | S    | A    | A, `WEAVEC_RT_HEAP_STACKS=0` |
| -------- | ---- | ---- | ---- | ---------------------------- |
| cJSON    | 2.46 | 2.46 | 1.72 | 1.31                         |
| sds      | 2.17 | 2.06 | 1.69 | 1.25                         |
| jsmn     | 1.54 | 1.68 | 1.00 | —                            |
| zlib     | 1.33 | 1.27 | 1.00 | —                            |
| inih     | 2.47 | 2.48 | 1.01 | —                            |
| tinyexpr | 1.11 | 1.25 | 1.01 | —                            |

Column A is the floor: a program that does not allocate in its hot path
pays nothing for the runtime, so everything S pays there is checks. For
the two that allocate, recording a stack at every allocation and release
(RFC 0040) costs 0.41 and 0.44 of the reference's time, and the allocator
and sweep 0.31 and 0.25. One recorded frame instead of the default depth
still costs 0.14 (cJSON) and 0.24 (sds).

To divide what S pays above A, a throwaway change to the safe-code pass
removed kinds of check one after another (not kept; each build is unsound
and is a lower bound on cost, not a design). X1 drops the guards on
pointers loaded from memory, X2 also the string-length guards, X3 also the
bounds comparisons. cJSON and sds are shown without allocation stacks; S
was rebuilt for this table and differs from the first by run-to-run noise
of 1 to 2%.

| Project  | S    | X1   | X2   | X3   | A    |
| -------- | ---- | ---- | ---- | ---- | ---- |
| cJSON    | 2.04 | 1.79 | 1.81 | 1.79 | 1.31 |
| sds      | 1.59 | 1.59 | 1.61 | 1.39 | 1.25 |
| jsmn     | 1.66 | 1.49 | 1.50 | 1.01 | 1.00 |
| zlib     | 1.25 | 0.98 | 1.00 | 0.98 | 1.00 |
| inih     | 2.51 | 2.04 | 1.55 | 1.42 | 1.01 |
| tinyexpr | 1.25 | 1.13 | 1.11 | 1.11 | 1.01 |

Reading the columns as shares of the reference's time:

- **Guards on loaded pointers** (S − X1): all of zlib's cost (0.27), 0.47
  for inih, 0.25 for cJSON, 0.17 for jsmn, 0.12 for tinyexpr. The ledger
  agrees on how common they are: 320 of 748 accesses in the converted
  `cJSON.c` keep a guard, 717 of 1,627 in zlib's `deflate.c`, 354 of 663
  in `inflate.c`, 112 of 300 in `sds.c`, 23 of 45 in `ini.c`.
- **String guards** (X1 − X2): 0.49 for inih, nothing elsewhere.
- **Bounds comparisons** (X2 − X3): 0.49 for jsmn, 0.21 for sds, 0.12 for
  inih, nothing measurable for the others. The comparison is the part a
  Rust program also pays; jsmn's share is far above what Rust pays for the
  same loop, which points at the comparison's form (§7).
- **Everything else in the code** (X3 − A): 0.48 for cJSON, 0.41 for inih,
  0.14 for sds, 0.10 for tinyexpr, none for jsmn and zlib. This is frames
  with redzones, the checked library wrappers, entry validation, and the
  source changes the conversions made.
- **The runtime** (A − 1): allocation stacks, then the allocator.

No single removal reaches the reference build. The shadow has to leave
safe code altogether: loads, strings, frames and library calls. That needs
one thing the current design does not have, a reason to believe a loaded
pointer.

### Why a loaded pointer cannot be believed today

RFC 0041's body proposed to validate a loaded pointer against the shadow
at the load; its amendment 1 kept the guard on each access instead, at the
same cost. Either way the shadow is read, because a safe function shares
memory with guarded functions, and a guarded function may store a pointer
to four bytes in a field that says `struct big *`.

A program written or rewritten to be safe has no such functions, or a
handful that somebody read. For that program the check is paid on every
access to defend against code that is not there. Rust's answer is to make
the absence a property of the language: a type's invariant holds in all
safe code by construction, and the few places that could break it are
marked. RFC 0041 listed this as _Future work_ ("all-safe programs"); the
measurements say it is the work.

## Soundness

### Typed memory

Memory is **typed** when every place a typed function can reach whose
declared type is a pointer (a parameter, a local, a field, an array
element, a global, a result) holds null or a pointer _p_ such that:

- **(W1) Single.** For `T *` with no annotation: the `sizeof(T)` bytes at
  _p_ lie in one object. For a function pointer: _p_ is a function of that
  type.
- **(W2) Counted.** For `WEAVEC_COUNTED_BY(n)`, `WEAVEC_SIZED_BY(n)` and
  `WEAVEC_ENDED_BY(e)`: the bytes the bound gives, with _n_ or _e_ read
  from the place the annotation names at that moment, lie in one object.
- **(W3) Terminated.** For `WEAVEC_NULL_TERMINATED`: a zero byte lies at
  or after _p_, no further than one byte past the end of _p_'s object.
- **(W4) Typed pointees.** The bytes (W1) and (W2) describe hold, wherever
  `T` has a pointer, a value that satisfies this definition.

"One object" is a live object, or a released heap block that has not been
reused (RFC 0036 (T)), or a local in its scope. Null is always allowed;
the `_OR_NULL` forms keep their meaning for counts.

### The guarantee for typed functions

For a program that meets the precondition of the existing guarantee and
assumptions T-A1 to T-A4 below:

- **(M1) Typed memory is kept.** Every store, call and return in a typed
  function leaves memory typed: the value has the destination's bound
  statically, or a comparison shows it (`bounds-conversion` when it
  fails), or the function did not compile.
- **(M2) Accesses.** A load, store or memory intrinsic in a typed
  function lies inside the object its pointer points to: it is at a
  constant offset inside a single pointer's pointee, or inside the bounds
  of a counted pointer by a comparison (`pointer-out-of-bounds`), or at a
  string's current byte, which is not past its terminator.
- **(M3) Heap lifetime.** A pointer to a released block reaches that
  block's bytes, which are still typed: nothing but the program writes
  them after the release, and a block that a pointer inside them refers to
  is not reused either (§9).
- **(M4) Entry.** A pointer that a typed function receives from a caller
  that is not typed, or from a function that is not typed, satisfies its
  bound when it arrives (a check against the shadow, as RFC 0041 (S2), now
  for single pointers too).
- (S4) and (S5) of RFC 0041 hold as they are; (S6) holds over the bounds
  the function knows (§8).

Safe functions outside a typed unit keep (S1)–(S6), and guarded functions
(A)–(P), unchanged.

### What gets weaker, in plain terms

This RFC trades soundness for cost in typed units, and only there:

1. **Trust in code that is not typed.** Today a safe function that loads
   a pointer a guarded function corrupted is stopped by the guard on its
   next access. A typed function uses the pointer. The guarantee of typed
   functions is conditional on T-A1, as safe Rust's is on its `unsafe`.
2. **No report of a use after free.** Safe functions today report a use
   after free through a loaded pointer, because the access is guarded
   (RFC 0041 amendment 1). Typed functions report none: the access reads
   the released block's own, still typed bytes (M3). It cannot reach
   another object.
3. **No colors and no redzones inside typed code.** (C) and the shadow's
   view of locals do not apply to accesses of typed functions; their
   bounds comparisons replace both, and are exact where the shadow rounds
   to an object.

`-fweavec-safe-checks=full` keeps every guard in typed functions, and
`-fweavec-checks=verify` keeps each removed guard as a monitor that
reports `weavec.proven` if it would have failed. A test suite run in
either mode checks T-A1 on the paths it runs and reports uses after free.

The project's owner accepted this trade on 2026-11-11, before any of the
design was built: typed functions trust code that is not typed, as safe
Rust trusts `unsafe`. The rest of the draft, its unresolved questions
included, is still open to review.

### Bugs caught

```c
/* weavec-cc -fweavec-typed */
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };

int get(const struct vec *v, size_t i) {
  return v->data[i];        /* i >= cap: pointer-out-of-bounds, by one
                               comparison with v->cap; no shadow read */
}

int grow(struct vec *v) {
  v->cap *= 2;              /* error: 'cap' bounds 'data'; assign them
                               together [weavec-safe:counted-pair] */
  int *d = realloc(v->data, v->cap * sizeof *d);
  if (d == NULL) return -1;
  v->data = d;
  return 0;
}

void trim(char *WEAVEC_NULL_TERMINATED s) {
  s[strlen(s) - 1] = '\0';  /* error: a string is read-only through a
                               null-terminated pointer; take a counted
                               buffer [weavec-safe:string-write] */
}

struct a { int *p; };  struct b { long n; };
long pun(struct a *x) {
  return ((struct b *)x)->n; /* error: 'struct a' holds a pointer
                                [weavec-safe:pointer-cast] */
}
```

An overflow inside an object is caught exactly: `get` compares against
`cap` even when the block behind `data` is larger, and an index past a
`char name[8]` field is stopped at the field, where the shadow sees only
the block.

### Bugs deliberately not caught

- **A use after free** in a typed function: silent, and confined to the
  released block (M3). Tests under `full` report it.
- **Typed memory broken by code that is not typed** (T-A1), on paths no
  test runs under `full` or `verify`.
- **Data races** on a pointer and its count, as RFC 0041 (S-A3).
- **A read past a string's terminator inside its object.** If a counted
  view of a buffer overwrites the terminator a null-terminated view relies
  on, a walk through the second view reads on to the end of the object
  (W3's zero byte stops it there). It reads bytes of the same object and
  writes nothing (rule `string-write`).
- Everything RFC 0041 lists: leaks, integer overflow outside the sizes the
  compiler computes, logic errors.

### Accepted rejections

Beyond RFC 0041's, a typed function rejects correct C that:

- writes through a null-terminated pointer (`string-write`): in-place
  trimming and splitting take a counted buffer;
- indexes a null-terminated pointer by a variable before its length is
  known (`string-index`);
- changes a count without assigning its pointer next to it, other than by
  a visible decrease (`counted-pair`);
- converts between pointer types when either pointee holds a pointer
  (`pointer-cast`, tightened): today only the target is examined, which
  lets an integer view overwrite a pointer that typed code would then
  believe;
- converts one allocation to two types.

Gate T2 measures how many more lines these cost the conversions.

### Assumptions

The assumptions of the existing guarantee and S-A1 to S-A5 hold. Typed
functions add:

|          | Assumption                                                                                                                                                                                                                                                                                                                                                                                        |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **T-A1** | Code that is not typed (guarded and unsafe functions, safe functions outside a typed unit, code WeaveC did not compile) leaves memory typed wherever typed code can reach it: it stores a pointer only where the pointer satisfies the place's declared bound, changes a count only with its pointer, writes no other bytes over a pointer, and does not overwrite the zero byte after an object. |
| **T-A2** | A declaration marked `WEAVEC_SAFE` in a typed unit names a function defined in a typed unit. A false one fails to link (§6); it cannot weaken the guarantee.                                                                                                                                                                                                                                      |
| **T-A3** | A pointer inside a released block that the program can still reach meets A6: the sweep sees it (§9).                                                                                                                                                                                                                                                                                              |
| **T-A4** | No data race on any pointer typed code loads together with a store that changes the type of what it points to; RFC 0041's S-A3 for counts.                                                                                                                                                                                                                                                        |

## Detailed design

### 1. Typed units

`-fweavec-typed` implies `-fweavec-safe` for the unit: every function it
defines outside system headers is safe unless marked `WEAVEC_GUARDED` or
`WEAVEC_UNSAFE`. Its safe functions are **typed functions**. The flag is a
statement about the program, made by whoever builds it: the code that
shares memory with this unit keeps memory typed. `WEAVEC_SAFE` on one
function of a unit without the flag keeps RFC 0041's meaning, which is the
mode for converting a program a function at a time.

| Tier                                      | Compile time              | Run time                                                                                                           |
| ----------------------------------------- | ------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| **Typed** (`-fweavec-typed`)              | RFC 0041's rules and §3's | comparisons against bounds; no shadow read except at entries from code that is not typed and in the calls §8 lists |
| **Safe** (`WEAVEC_SAFE`, `-fweavec-safe`) | RFC 0041's rules          | as today: bounds where the function knows them, guards elsewhere                                                   |
| **Guarded**                               | none                      | guards                                                                                                             |
| **Unsafe**                                | none                      | none                                                                                                               |

`weavec-c++` rejects the flag, as it rejects `-fweavec-safe` (§12).

### 2. What makes memory typed to begin with

- **Allocations.** The runtime's allocator zero-fills every block, and
  the bytes `realloc` adds, so a fresh block is typed for any type: every
  pointer in it is null.
- **Locals.** A local whose type holds a pointer is zero-initialized in a
  typed function whatever its size; `-fweavec-zero-init`'s 4 KiB limit
  applies only to locals without pointers. A local placed on the heap is
  allocated zeroed already.
- **Globals** are zero or initialized by constants the checker examines.
- **One type per allocation.** An allocation's result converts to one
  pointer type, at the call (`T *p = malloc(n * sizeof *p)`), with the
  size checked against `T`; a `void *` that holds an allocation does not
  convert later (rule `pointer-cast`).

### 3. The rules added for typed functions

RFC 0041's rules apply as amended. Typed functions add or tighten these,
each with a stable identifier, a fix note and tests under `test/Safe/`:

| Id             | Rule                                                                                                                                                                                                                                                                                                                                                                                             |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `counted-pair` | A count or end that bounds a pointer field is assigned only in the statement next to its pointer's assignment (either order; an initializer or a struct copy assigns both), or by a decrease the compiler sees (`n--`, `n -= k`, `n = 0`). `p = p` next to the count is accepted as the pointer's assignment. Was in RFC 0041 §3; its amendment 1 dropped it because field counts were not used. |
| `pointer-cast` | Tightened: a pointer converts to another pointer type only when neither pointee holds a pointer, with three exceptions: to `void *`; to a pointer to `const` bytes, for reading; and an allocation's result, once, at the call.                                                                                                                                                                  |
| `string-index` | A null-terminated pointer is dereferenced at its current byte and advanced by a constant; an index or offset that is not a constant needs its length first (`size_t n = strlen(s)` gives `s` the bounds [`s`, `s + n + 1`) while `s` is unchanged).                                                                                                                                              |
| `string-write` | No store through a null-terminated pointer, nor through a pointer whose bounds came from measuring one. A buffer that is written is counted or sized.                                                                                                                                                                                                                                            |
| `wide-store`   | A pointer computed by arithmetic and then converted to another pointee type is stored only in a declared place (a field, a parameter, a result, a global), where the compiler knows the size it must cover.                                                                                                                                                                                      |

`union-pointer`, `pointer-bytes`, `int-to-pointer`, `variadic`, `escape`,
`index-single`, `call-args`, `no-asm` and `unsafe-block` are unchanged;
with typed memory they are what makes (W4) hold, where before they were
defense behind the guards.

### 4. What a typed function's accesses cost

| Access                                                                                                    | Check                                                                                                                                                                                                     |
| --------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `*p`, `p->f`, at a constant offset from a single pointer (a parameter, a loaded pointer, a call's result) | none: the checker showed the offset inside `sizeof *p` (rule `index-single`), and (W1) gives the object. A null `p` faults for offsets under 4 KiB (`memory-fault`, RFC 0037 §6) and is tested above that |
| `p->arr[i]`, an array inside the pointee                                                                  | the index against the array's declared length ((B), as today)                                                                                                                                             |
| `p[i]` through a counted, sized or ended pointer, wherever it came from                                   | one comparison (§7)                                                                                                                                                                                       |
| `*s`, `s[k]` and `s += k` on a null-terminated pointer, _k_ a constant                                    | each byte stepped over is tested not to be zero; in `for (; *s; s++)` that is the loop's own test                                                                                                         |
| A local or a global                                                                                       | static, or one comparison                                                                                                                                                                                 |
| A pointer the pass cannot bound (§5 lists what it knows)                                                  | keeps its guard, counted in the ledger as `typed-unbounded`; gate T5 requires none in the conversions                                                                                                     |

No access in a typed function is checked against a color, and none reads
the shadow except the last row.

### 5. How the pass learns each pointer's bound

The guard passes work on LLVM IR, where a loaded pointer has no declared
type. RFC 0041 hands them a table of each function's parameter and result
bounds by symbol. Typed units extend what the checker hands over:

- **Parameters and results.** The table gains every single pointer's
  pointee size beside the bounded ones.
- **Fields.** When a struct or union definition is complete
  (`HandleTagDeclDefinition`, before any function that uses it reaches
  code generation), the checker adds an implicit `annotate("weavec.field",
kind, pointee size, count offset, count width)` attribute to each
  pointer field, and `annotate("weavec.count", …)` to each field that
  bounds one. Clang emits `llvm.ptr.annotation` around every access to an
  annotated field (checked on Clang 23.1 for loads and stores), so the
  pass sees, at each load or store of a pointer field, its kind, its
  pointee's size and where its count is. The pass removes the intrinsics
  when it has read them.
- **Globals** of pointer type: by name, in the table.
- **Arithmetic.** A pointer computed in the function has the bounds of
  what it derives from, as today; the element type of its last
  `getelementptr` gives the size a single destination needs.
- **Anything else** loaded from memory (an element of an array of
  pointers, `*pp`) is a single pointer of a size the pass does not know.
  Constant offsets from it were checked by the checker; a variable offset
  keeps its guard (`typed-unbounded`).

A loaded counted pointer takes its bounds from the count read at the same
point. The pair is a snapshot: if the field's count shrinks later, the
local copy keeps bounds that were true of the same object.

### 6. Stores, calls and returns: keeping memory typed

Where a pointer leaves the function's locals, its value must satisfy the
destination's bound:

- **Into a single place** (a field, an element, a parameter, a result, a
  global): a value loaded from a single place, a parameter, the address
  of a whole local or global, or an allocation's result of sufficient
  size needs nothing. A value computed by arithmetic is compared against
  its bounds for the pointee's size (`bounds-conversion`).
- **Into a counted place**: at the pair of stores rule `counted-pair`
  requires, the new count times the element size is compared with the new
  pointer's bounds. A lone pointer store is compared against the current
  count; a lone decrease of a count is compared against the old count.
- **Into a null-terminated place**: from a null-terminated value or a
  literal, nothing; from a bounded value, a search for a zero byte within
  its bounds (`unterminated-string`), which is the one linear check typed
  code makes and is visible in the source as a conversion.
- **Calls between typed functions.** A call to a typed function defined
  in the unit calls its body (`<name>.weavec.safe`, RFC 0041 amendment 18)
  after the comparisons above for each argument. In a typed unit the body
  of a function with external linkage is external too, and a call to a
  function whose declaration carries `WEAVEC_SAFE` binds to that symbol.
  If the definition is not typed the link fails with an undefined
  `<name>.weavec.safe`, so the annotation cannot lie (T-A2).
- **Entries.** A typed function's C symbol is its entry: it checks each
  pointer parameter against the shadow over its bound (a single pointer's
  one object as well, new here) and calls the body. Guarded callers,
  callers in other units without the declaration, and calls through a
  function pointer go through it.
- **Results of functions that are not typed** are checked against the
  shadow over their declared bound, as today, single ones included.

### 7. The form of a bounds comparison

RFC 0041 amendment 3 compares `offset < usub.sat(size + 1, width)` on byte
offsets from the root. jsmn's tokenizer pays 0.49 of the reference's time
for those comparisons, though the loop already tests `pos < len` against
the same count, so the optimizer is not folding the two. Typed functions
emit:

- **Index form.** For an access of one element at `p[i]` through a
  counted root, `i < n` on the index and the count as the source has
  them, unsigned, with no saturating arithmetic, so that a dominating
  test of the same values removes it. The byte form remains for accesses
  that are not one element at an index.
- **Loop ranges.** `GuardLoop` (RFC 0035 §6.4) versions a loop whose
  guarded accesses are affine in its induction variable, checking the
  whole range once. It learns the bounds marker: a loop whose comparisons
  are `i < n` for an affine `i` is entered through one comparison of the
  range, with the per-iteration form as the fallback version. The loop
  body then has no trap in it and can be vectorized.

S0 confirms the cause on jsmn before either is built (_Unresolved
questions_).

### 8. Strings, library calls and frames

- **Strings.** (W3) puts a zero byte at or after every null-terminated
  pointer, within one byte of its object's end. To make that hold even
  when a counted view overwrites a terminator, every object WeaveC lays
  out is followed by a zero byte that no typed store can reach and a
  guarded store traps on: the allocator's slack after a block (kept zero),
  a global's redzone, one byte after each local array in a typed frame. A
  walk therefore stops inside its object or on that byte. Typed code only
  reads through such a pointer (`string-write`), so a walk that ran past a
  destroyed terminator into other fields of the same object can change
  nothing.
- **Library calls.** For a row whose accesses are given by size arguments
  (`memcpy`, `memset`, `memcmp`, `fread`, `snprintf`'s destination and
  their kin), a typed function compares each size with the argument's
  bounds and calls the function itself, as RFC 0041 §9 specified and its
  amendment 7 postponed. A string argument takes a null-terminated value
  as it is. Rows the table cannot express that way (formats with `%s` and
  `%n`, the `scan` rows, `wide` rows) keep their checked wrappers and
  their shadow reads; they are counted in the ledger.
- **Frames.** A local of a typed function whose address reaches only
  typed code (it is used in the function, or passed to parameters of
  typed functions) is laid out as Clang lays it out: no redzone, no
  shadow written at entry or exit. A local whose address reaches code
  that is not typed keeps its redzones and its shadow (RFC 0041 amendment
  8), so guarded code's accesses to it are checked as before. Locals that
  may outlive their scope go on the heap, unchanged.

### 9. The runtime

- **A sweep traces through released blocks that are still referenced.**
  Today the mark reads the roots and every live block (RFC 0037 §1), and a
  released block's contents are not read: nothing could load them without
  a guard stopping it. A typed function can. If a dangling pointer reaches
  released block _A_, and _A_ holds a pointer to released block _B_, then
  _B_ must not be reused while _A_ can be read. After the mark, the sweep
  scans each quarantined block that was marked and has not been scanned,
  marks what it points to, and repeats until no new block is marked. Only
  referenced released blocks are scanned, so garbage that points to
  garbage is still recycled.
- **A released block keeps the program's bytes.** RFC 0040 amendment 4
  stores a block's release stack in its first eight bytes with its
  complement. Read as a counted pointer and its count by a typed function
  holding a dangling pointer, that is a forged pointer. The release stack
  moves to a table beside the allocation stacks' (one more `uint32_t` per
  slot), and a released block is not written by the runtime until it is
  reused.
- **Allocation stacks are off by default in a typed program.** A link
  with `-fweavec-typed` records in the program that its default for
  `WEAVEC_RT_HEAP_STACKS` is 0; the variable overrides it either way.
  Recording costs 0.4 of the reference's time where a program allocates
  heavily, and a typed function reports no use after free for the stacks
  to explain. Release reports (`double-free`, `invalid-free`) then say
  that stacks were not recorded and how to turn them on.
- **The zero byte after each block** (§8) is kept by the allocator.
- The runtime ABI becomes 42 (the release table, the program's defaults,
  `__weavec_rt_convert_fail`).

Nothing else in the allocator changes here. What it costs after stacks
(0.25 and 0.31 of the reference's time on sds and cJSON) is _Future work_.

### 10. Verify mode, the ledger and the audit

- **Guard removal.** RFC 0035 §6 requires a removed guard's rule to be
  local to a function. A typed function's removals are justified by typed
  memory, which each function keeps by rules checked inside it but which
  holds only if every writer keeps it. §6 and AGENTS.md are amended to
  admit this second justification, named `typed` in the ledger, on the
  same condition as the first: `verify` mode keeps each such guard as a
  monitor.
- **The ledger** (next version) adds the reasons `typed` (PROVEN) and
  `typed-unbounded` (GUARDED), and a `functions` list: each function the
  unit defines with its tier and, for a function that is not typed, its
  location. The summary line gives the counts.
- **The audit.** `weavec-cc --weavec-audit <ledger directory>` prints, for
  a program's ledgers, every function that is not typed, the accesses in
  typed functions that kept a guard and why, and the library calls that
  kept a wrapper: the list a reviewer or an agent reads to see what the
  guarantee rests on, as `unsafe` is searched for in Rust. `--format=json`
  for tools.

### 11. Documentation

`docs/pages/guides/safe-c.md` gains the typed mode: what the flag claims,
the added rules with a rejected example and its rewrite each, how to move
a converted project to it, and a section written for an agent starting a
new program or porting one from another language (the building blocks
that pass the rules: a counted vector, a string builder over a counted
buffer, a tagged struct in place of a union of pointers, typed callbacks).
`guarantees.md` gains (W1)–(W4), (M1)–(M4) and T-A1–T-A4; `cli.md` the
flag, the audit and the report kind.

### 12. C++

Not in this RFC. Typed memory is what a safe C++ would need first:
references, `std::span` and `std::unique_ptr` are single and counted
pointers with exactly these invariants, and the hardened C++ library's
checks are the comparisons of §4. RFC 0041 §12's plan stands, and its next
step should be written against typed memory rather than against guards.

### 13. Stages and gates

- **S0 On paper, and three probes.** The added rules applied by hand to
  jsmn, sds and zlib's `deflate.c` as converted, recording each rejection;
  the cause of jsmn's comparison cost found and the form of §7 tried on
  it; the field annotations of §5 tried on cJSON's structs; the port for
  gate T7 chosen and frozen with two new held-out conversions. Thresholds
  below are confirmed or changed here.
- **S1 The checker**: the flag, the rules of §3, the table's sizes, the
  field annotations.
- **S2 Loads**: bounds from fields and globals, guards removed with the
  reason `typed`, verify's monitors, the ledger.
- **S3 Stores and calls**: the conversions of §6, external bodies,
  entries that check single pointers.
- **S4 Strings and library calls** (§8).
- **S5 Frames and the comparison's form** (§7, §8).
- **S6 The runtime** (§9).
- **S7 Conversions and the port**, by agents; the guide; the gates on
  darwin-arm64, linux-aarch64 and linux-x86_64.

The gates:

- **T1 Rules.** Each added rule has an accepted and a rejected test and a
  fix note.
- **T2 Conversion.** RFC 0041's seven conversions compile typed with the
  same functions safe or more, their tests pass, and each changes at most
  10 points more of its project's lines than it did. Recorded: the rounds
  and the rejections by rule.
- **T3 Held-out.** Two new C projects converted to typed C by an agent
  given only the guide: at least 80% of functions typed, tests pass.
- **T4 Detection.** `test/cases/typed/`: each spatial class traps or is
  rejected, including an overflow inside an object; a pointer with a
  wrong bound stored by a guarded function is reported under `full` and
  under `verify`; a dangling pointer to a block that holds pointers reads
  only blocks that were not reused, across sweeps.
- **T5 No shadow.** In the conversions' typed functions the ledger shows
  no `typed-unbounded` access, and guards only in the calls §8 lists.
- **T6 Run.** The conversions' benchmarks, typed, against the reference
  build: geometric mean at most 1.20, none above 1.50, and those that do
  not allocate in their hot path (jsmn, zlib, inih, tinyexpr) at most
  1.15 each. Today: 1.80 geometric mean of S, 2.48 the worst.
- **T7 The port.** One agent protocol ports a small library chosen at S0
  from its original language (not C, not Rust) into typed C and into
  Rust, from the same specification and tests. Typed C: at least 95% of
  functions typed, tests pass. Recorded and compared: compile rounds,
  agent time, lines, and the benchmark, where typed C is within 1.25
  times the Rust port. This is the gate that measures the owner's goal
  directly; its thresholds are the least certain.
- **T8 No change elsewhere.** A unit without `-fweavec-typed` guards and
  proves the same accesses as on main (the ledger over the corpus quick
  set), and the corpus gate's results do not change. The sweep's trace
  adds no measurable pause to the corpus benchmarks.
- **T9 Verify.** `-fweavec-checks=verify` reports no `weavec.proven` in
  the conversions' tests.
- **H1 Hygiene.** The library within 13,500 lines (11,295 of 11,500
  today), the runtime within 10,500 (9,826 of 10,000).

## Annotation surface

No new macro. `weavec.h` becomes version 2.1 for two changes of meaning:

- `WEAVEC_SAFE` on a function **declaration**, in a typed unit, says the
  function is defined in a typed unit (§6). Elsewhere it is ignored on a
  declaration, as today.
- The bounds annotations on fields (`WEAVEC_COUNTED_BY`, `WEAVEC_SIZED_BY`,
  `WEAVEC_ENDED_BY`, `WEAVEC_NULL_TERMINATED`, Clang's `counted_by` family)
  are used at run time in typed units; RFC 0041 amendment 1 had left them
  to the checker alone.

The implicit `annotate("weavec.field", …)` and `annotate("weavec.count",
…)` attributes of §5 are added by the compiler and are not spellings a
program writes; their names live in `SafeCheck.h` beside the others.

## Diagnostics

**Compile-time errors**, in the form of RFC 0041, one per rule of §3:

```
vec.c:12:3: error: 'cap' bounds 'data' and is assigned without it [weavec-safe:counted-pair]
vec.c:12:3: note: assign 'data' in the next statement, or 'v->data = v->data' if only the count shrinks
str.c:4:19: error: store through null-terminated 's' [weavec-safe:string-write]
str.c:3:11: note: take the buffer and its size: 'char *WEAVEC_COUNTED_BY(cap) s, size_t cap'
```

The others: `string-index`, `wide-store`, and `pointer-cast`'s new cases.

**Run-time report kinds.** One new kind:

- `bounds-conversion`: a pointer stored, passed or returned whose value
  does not cover the bound of where it goes.
  `weavec: bounds-conversion at <file>:<line>:<col>: '<destination>' needs <n> bytes at <address>; the pointer's bounds are [<lo>, <hi>)`

  ```c
  struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t cap; };
  void set(struct vec *v, int *WEAVEC_COUNTED_BY(n) d, size_t n) {
    v->data = d;
    v->cap = n + 1;
  }
  ```

`pointer-out-of-bounds` gains the string case (its object line reads `…
past the terminator of the string at <address>`), and `unterminated-string`
is reported for a conversion to a null-terminated place that finds no zero
byte. The new kind gets a `test/Guards/` test, an entry in `cli.md` and in
`scripts/run-cases.py`.

## Drawbacks

- **A conditional guarantee.** WeaveC's guarantee has been unconditional
  on what other WeaveC-compiled code does. Typed functions give that up
  for speed. A project that mixes a typed library into a large guarded
  program trusts the guarded program more than it does today, and the
  flag makes that easy to do without thinking.
- **Stricter rules, more rewriting.** Read-only strings and paired counts
  will cost the conversions lines and rounds; T2 measures how many. inih
  and sds, which edit strings in place, will change most.
- **Two kinds of safe function** to explain, document and test.
- **Coupling to Clang's code generation.** §5 depends on
  `llvm.ptr.annotation` being emitted at every access to an annotated
  field and on front-end `getelementptr`s keeping their element types. A
  Clang that changes either needs another channel.
- **Reports get poorer in typed programs**: no use after free, no
  allocation stacks by default.
- **The sweep does more work**, in every program, typed or not, when
  released blocks are still referenced.
- **About 2,000 more library lines** and a budget raised again.

## Alternatives

- **Do nothing more; position WeaveC as a sanitizer.** Honest, and what
  the measurements describe today. It abandons the goal this RFC was asked
  to serve.
- **Check loaded pointers faster instead of trusting them.** Validate a
  loaded pointer in constant time from the allocator's metadata (RFC 0041
  amendment 12's idea) rather than the shadow. Keeps the guarantee
  unconditional, but still pays several instructions and a metadata load
  per pointer load, about a guard, and does nothing for stack and global
  pointers. It removes the wrong thing: the cost is having a check at all.
- **Whole-program proof that every writer is typed**, enforced at link
  time, in place of T-A1. Stronger, but no real program is all typed: it
  calls a C library, takes `argv`, has a few guarded helpers. Rust's
  answer (mark the exceptions, list them, trust them) is the one that
  ships; the audit of §10 is that list.
- **Safe C++ next.** C++ is where most code that considers Rust lives,
  and it has the containers an agent writing new code wants. But a safe
  C++ built on RFC 0041 as it stands would inherit guarded cost, which is
  the reason not to rewrite into it. Typed memory first, then C++ on it
  (§12).
- **Ownership checked at compile time** (unique pointers consumed exactly
  once, borrowed pointers that die with their owner): compile errors for
  use after free, double free and leaks, which are the errors Rust's loop
  is known for and which typed C leaves to the sweep and to tests. It is
  the natural RFC after this one, and is deferred, not rejected: it adds
  no speed, RFCs 0001–0029 show how such a checker grows, and its rules
  should be designed against typed memory's.
- **A faster allocator first.** It would help two of the six benchmarks
  and leave the other four where they are.
- **Trust by default under `-fweavec-safe`.** Fewer flags, but it would
  silently weaken programs that are mid-conversion, which is exactly when
  guarded code is writing the memory safe code reads.

## Prior art

- **Rust**: type invariants that safe code relies on and `unsafe` code
  must uphold; `&[T]` and `&str` as (pointer, length) pairs that are never
  rechecked when loaded; `&CStr` read-only, `CString::new` scanning once.
  Taken: all four, and `cargo-geiger`'s idea of counting what is trusted.
- **Apple's `-fbounds-safety`**: counts in fields trusted at loads and
  maintained at stores by requiring a pointer and its count to be assigned
  together; `__null_terminated` pointers that advance one element at a
  time and convert to indexable ones by measuring. Taken for §3 and §6. It
  trusts code compiled without it in the same way T-A1 does.
- **Deputy** and **CCured**: dependent counts checked at stores; CCured's
  SAFE pointers needing no check at a dereference because their type is
  an invariant, its WILD pointers paying on every access. Guarded code is
  WILD; this RFC adds SAFE.
- **MarkUs** (Ainsworth and Jones, 2020): marking transitively through
  quarantined objects that are still reachable, so that what they point
  to stays quarantined. Taken for §9. **MineSweeper** zeroes freed memory
  instead; rejected here because typed code may write a released block
  after it was zeroed.
- **Go**: memory safety from typed memory plus a collector, no borrow
  checker, races on multiword values left open. The same shape and the
  same hole (T-A4).
- **Fil-C**: memory-safe C and C++ with no source changes, by a collector
  and a capability beside every pointer, at 1.5 to 4 times the cost. The
  other end of the trade: no rewriting, and the cost is permanent.
- **LLVM's loop predication and inductive range check elimination**: the
  transformations §7's loop ranges specialize.

## Unresolved questions

1. **Why jsmn's comparisons cost 0.49.** §7 assumes the saturating byte
   form blocks folding with the loop's own test. S0 must confirm it on the
   IR before S5 commits to the index form.
2. **What typed cost really is.** X1–X3 are lower bounds: zlib's 0.98
   drops guards on `s->window[…]` that typed code replaces with a
   comparison against a loaded count. T6's thresholds are estimates until
   S2 runs zlib.
3. **Read-only strings.** Is `string-write` too strict for C as agents
   write it? The alternative, allowing a store that does not replace a
   zero byte with a nonzero one, is unsound when the string lies inside a
   struct that holds pointers after a terminator was destroyed. S0 counts
   the rewrites in inih and sds.
4. **T-A1 at the stores of guarded functions.** A guarded function in a
   typed unit could have its stores to annotated fields checked like a
   typed function's, shrinking the trust to casts and byte writes. Cheap
   to add after S3; worth it?
5. **Calls through function pointers** go through the entry and read the
   shadow for each pointer argument. Callback-heavy code (comparators,
   visitors) may need a typed function-pointer type whose calls bind to
   bodies.
6. **`void *` user data** (RFC 0041's first unresolved question) is still
   rejected on the way back to its type. Typed memory makes a checked
   answer possible (a type identifier beside the allocation); not
   designed here.
7. **Allocation stacks.** Is "off in typed programs" right, or should
   recording be made cheap for everyone (a cache by call site was
   considered and set aside: two call chains can share a frame address,
   and a report must not name the wrong one)?
8. **The zero byte after a local array** costs a byte and its
   initialization per array; is one shared byte per frame enough?
9. **The port for T7**: which library, from which language, and the
   agent protocol, so that the comparison with Rust is fair and can be
   repeated.

## Future work

- **Safe C++** on typed memory: references and `std::span` as typed
  pointers, the hardened library's checks as the comparisons.
- **Ownership at compile time**: `WEAVEC_OWNED` pointers consumed exactly
  once, errors for use after free, double free and leaks inside a
  function, the sweep kept for what ownership does not cover (shared and
  cyclic structures).
- **The allocator's remaining cost**, toward the system allocator's on
  allocation-heavy code.
- **Checked `void *`** and typed callbacks (questions 5 and 6).
- **Data races**: counted pairs stored atomically, or a rule for what
  threads may share.
- **A standard set of typed building blocks** (vector, string builder,
  map) shipped beside `weavec.h`, so that a port starts from them.

## Implementation amendments

These record where the implementation departs from the design above, from
S0's probes, from building it, and from what the conversion agents
reported (S7). They override the body.

1. **The comparison's cost (S0, §7).** jsmn's 0.49 is the saturating form:
   its check `usub.sat(len + 1, 1) > pos` is not the loop's own
   `len > pos`, and the optimizer does not fold the two. Typed functions
   emit the index form (`i < n` for an element at an index of a counted
   root, the count saturated where its bytes would overflow) and, for one
   byte, `offset < size`; an access at a constant offset inside a root of
   known size is decided when the function compiles and gets no check.
   `GuardLoop`'s loop ranges were not extended to the comparisons: with the
   index form the optimizer folds a comparison into a loop's own test where
   the test bounds the same index (jsmn's string scans), but not where it
   does not (jsmn's backward loop over its tokens), which keeps one
   comparison per iteration.
2. **`counted-pair` (§3).** A count stepped alone (`n++`, `n += k`, `n--`,
   `n -= k`, `n = 0`) is accepted: a decrease is checked against the old
   count with one comparison, an increase against the shadow (the pointer's
   object must have the bytes; ledger `typed-unbounded`). zlib's
   `put_byte` (92 of its 120 first-round errors) is that shape. A count set
   to another value without its pointer in the adjacent statement, or a
   pair whose second statement calls a function, is rejected, and so is a
   struct initializer whose constant count exceeds the constant array its
   pointer names (the initializer may be copied as a whole, not field by
   field). The pass finds the pair's two stores after the checker accepted
   them, through blocks whose other side traps; between them a load of the
   count is harmless, and a load of the pointer after the count's store
   (`n -= k; p += k`) reads the pointer with the count's old value. A count
   that bounds two pointers may only shrink alone. `WEAVEC_ENDED_BY` fields
   are not used at run time (their loads keep guards), so the rule does not
   cover ends.
3. **`pointer-cast` as built (§3).** Memory that holds pointers converts to
   nothing but `void` and `const` bytes; a `void *` (or a pointer to an
   incomplete type) converts to no other type, non-`const` bytes included,
   but where an allocation made it. A view larger than its object is
   allowed: in typed code its accesses are compared with the object's own
   bounds and its stores into declared places are converted, so it can read
   or write nothing outside. The body's rule ("neither pointee may hold a
   pointer") let a `void *` launder a struct with pointers into `long *`.
   `pointer-bytes` also requires whole objects in typed functions: a size
   that is a multiple of the type's, or `n * sizeof(T)`, so that no pointer
   is half written.
4. **What the pass trusts, and `field-address` in place of `wide-store`
   (§3, §5, §6).** Typed functions believe only the places a declaration
   bounds: struct fields (each element of a field that is an array of
   pointers too), parameters and results. A pointer loaded from anywhere
   else (`*pp`, an element of an array of pointers outside a struct, a
   global pointer variable: §5's "globals by name" is not built) is
   unknown, and the accesses through it keep their guards (ledger
   `typed-unbounded`); stores into such places need no check. For this to
   be sound, a place that is a field must never be written as an unknown
   place: rule **`field-address`** rejects the address of a pointer field
   (or of an element of an array of them) leaving the function, as an
   argument, a result or a value stored in memory. Inside the function the
   pass follows the address through phis and selects (`struct node **pp =
&list->head; while (*pp) pp = &(*pp)->next;` is typed). `wide-store` is
   not needed and is not implemented: a pointer computed by arithmetic and
   stored where a declaration bounds it is converted (compared) whatever
   its type.
5. **Field annotations (§5).** `weavec.field` has six arguments: kind,
   element size, count offset (relative to the field, or the constant),
   count width, flags (`_OR_NULL`, signed count, array of pointers), and
   an index into a table the end of the unit fills with the size of a
   pointee that was incomplete where the field was declared (zlib's
   `z_stream.state`). A count must be a sibling field of the same struct
   or a constant. A loaded counted pointer that is null has empty bounds,
   whatever its count.
6. **Strings (§4, §8).** No step test is inserted. A step forward over a
   string is a string position where every use it is put to is covered by
   tests that each byte stepped over is not zero: a comparison with zero
   or with another constant for equality, a `switch`, or a branch on a
   `&&`'s phi whose every incoming value is false, such a comparison, or
   from a block a test covers. A step no test covers (a test made by a
   call such as `isxdigit`, a step after a join of two paths) leaves its
   accesses guarded; the guide says to step inside the branch that tested.
   `size_t n = strlen(s)` gives `s` the bounds `[s, s + n + 1)` where it
   dominates. A library call's string argument that is a string position
   or a literal needs no scan, and a search (`strchr`, `scan` rows) over a
   string drops the checks of what it read. The zero byte after an object
   is the allocator's: the byte after each block's object is zeroed when
   the block is made and when `realloc` shrinks it in place. A block whose
   object fills its slot exactly has none, and neither do locals (frames
   keep their layout, below); a read through a string whose terminator a
   counted write destroyed may then go past its object into the next. It
   writes nothing.
7. **Library calls (§8).** Calls with size arguments are compared through
   the existing range checks of RFC 0041 wherever the pointer has bounds;
   nothing else changed in the library guards.
8. **Frames (§8).** Frames keep RFC 0041's layout, but a typed function's
   local whose address goes only to typed bodies is not tracked, so it gets
   no redzone and no shadow writes; one whose address reaches code that is
   not typed keeps both. Local arrays get no zero byte after them.
9. **Entries and bodies (§6).** In a typed unit every function with a
   pointer parameter or result is split into an entry and a body, and an
   external function's body (`<name>.weavec.safe`) is external: a typed
   caller in another typed unit that declares the callee `WEAVEC_SAFE`
   calls it after converting its arguments. Entries check single pointers
   of known size against the shadow (null allowed) and scan string
   parameters once. Typed bodies convert their returns, and typed callers
   trust typed callees' results; a single result of a function that is not
   typed is checked against the shadow at the call.
10. **The runtime (§9).** A released slot's release stack and size live in
    a table of two words per slot beside the allocation stacks; a large
    released slot is decommitted whole (its pages read zero, which is
    typed). After the mark, the sweeping thread reads each marked released
    slot once and marks what it points to, to a fixpoint (serially; released
    huge blocks are inaccessible and are not read). A typed unit's
    constructor calls `__weavec_rt_typed_program`, which turns allocation
    stacks off unless `WEAVEC_RT_HEAP_STACKS` was set: a program that links
    any typed unit records none by default, not only one linked with the
    flag. The runtime ABI is 42.
11. **Zeroed locals (§2).** In a typed function a local whose type holds a
    pointer and is larger than `-fweavec-zero-init`'s 4 KiB is zeroed where
    each of its scopes starts.
12. **The ledger and the audit (§10).** The ledger is version 5: the
    reasons `typed` (proven) and `typed-unbounded` (guarded), and a
    `functions` list of each function's tier, file and line; the summary
    line ends `; <t> of <f> functions typed`. `--weavec-audit` lists what
    is not typed, the guarded accesses of typed functions (entries' checks,
    which are the boundary, excluded) and their wrappers; `--format=json`
    writes `weavec-audit` version 1. `-fweavec-ledger=<dir>/` creates the
    directory.
13. **Reports (Diagnostics).** `bounds-conversion`'s line reads `needs <n>
bytes at <address>` (the destination is not named); its object line is
    `pointer-out-of-bounds`'s. A string read past its terminator is not a
    report of its own (no step test is inserted, amendment 6).
14. **Safe C fixes found on the way.** A pointer difference (`p - buf`) no
    longer places a local on the heap: a safe function cannot turn the
    integer back into a pointer. A checker error is reported once per place
    in the file, not once per macro expansion of its argument.
15. **Flexible array members.** An element of a struct's last field that is
    an array of at most one element (`T data[]`, `T data[0]`, or the struct
    hack's `T data[1]`, as Clang's `-fstrict-flex-arrays=1` reads them) may
    lie past the struct's size, so a pointer to the struct does not bound
    it: the access keeps its guard (`typed-unbounded`). A last field of more
    elements is bounded by the struct. tinyexpr's `te_expr.parameters[]`
    trapped on every expression before this.
16. **`call-args` in typed functions (§3).** A pointer to one object stored
    in a field, or returned as a result, whose declaration has a bound is
    rejected when the function compiles, as an argument for a bounded
    parameter is; it would otherwise stop at run time (`bounds-conversion`,
    `unterminated-string`) on every call. A string passed to `free` or
    `realloc` is not `string-write`. An expression whose type is declared a
    string (`(*pp)[0]`, `pp` a pointer to a string) is a string for
    `string-write`.
17. **Measured strings (§3, `string-index`).** "Not assigned again" allows
    one assignment to a variable declared without a value. The bounds that
    `strlen` gives a string bound the library calls that read it too
    (`memcpy(c, s, n + 1)`), not only its indexed accesses.
18. **Pairs, as built (§6).** The pass finds a pair's second store through
    the arms of an `if` or a `?:` that write nothing and meet again (zlib's
    `avail_in = in < last ? … : …`), and through a struct pointer loaded once
    per statement (`s->strm->next += k; s->strm->avail -= k;`), unless
    something stores where it was loaded from. An extern array declared
    with its size is bounded by that size in typed functions: C makes the
    definition agree.
19. **Found by the held-out conversions.** The check of a pair goes at the
    store the search found second (a check placed at the first, after
    other checks had split its block, broke bzip2's `BZ2_bzReadOpen`), and
    a counted pointer's bounds are built beside its count before any check
    splits a block. `weavec-cc` given several sources compiles each of them
    after one fails, as Clang's driver does, so one round shows every
    file's errors.
20. **Budgets (H1).** The library is 14,200 lines (after the review's fixes), over the 13,500 the
    body expected: the typed rules (about 800 lines of the checker), the
    pass's typed bounds, conversions, strings and pairs (about 1,200), the
    audit and the ledger's functions (about 200). Its limit is 14,200; the
    runtime's is 10,500 (9,940 used).
21. **Released blocks are cleared, not traced (§9).** Tracing through
    referenced released blocks kept whole freed structures alive: on cJSON's
    benchmark one stale word kept a freed tree of 300,007 blocks after the
    last sweep, and sweeps took 1.8 s against 0.15 s, in every program.
    Instead, in a program that links a typed unit, the sweep clears (zeroes,
    or decommits the pages of) each released block that something still
    refers to, after the mark and before recycling, while every thread is
    stopped. A typed function holding a pointer to it then reads nulls and
    zero counts, which are typed, and nothing it pointed to is kept for it;
    the values a running function already loaded are in its registers or
    stack, which the mark read. It is the use after free the body said
    typed functions do not report, now reading zeros after a sweep. A
    program without typed code does neither: its sweeps are main's, and
    its reference build runs at the same speed (cJSON 2.29 s against 2.30
    s, sds 1.20 s against 1.19 s). The body's §9 and its prior-art note
    on MineSweeper are replaced by this.
22. **The index form through a member (§7).** `&p[i]` followed by a member
    at a constant offset inside the element is compared by its index too;
    jsmn's backward loop over its tokens went from 1.49 to 1.30 times the
    reference with it.

23. **Found in review.** A null `_OR_NULL` pointer's count, and a counted
    result's (a typed callee may return null with any count), is empty for
    the index form as it was for the byte form: `i < n` let a null pointer
    with a large count reach an absolute address. A count that bounds two
    pointers and grows alone is checked against the shadow for each of
    them, not only the first.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, `dev` preset) on 2026-11-11.
The conversions were made by Claude subagents (the session's model), each
given a brief and the guide, from RFC 0041's Safe C conversions (T2) or from
the original project with only the guide (T3); rounds count whole compiles
of the library with `-fsyntax-only -fweavec-typed`. They used a copy of
the compiler frozen at their start; the compiler then changed (amendments
14–22) from what they reported, and every build below is the final
compiler's. Timing: serially on a quiet machine (load 2–3), five
interleaved rounds (three for http-parser), best user CPU, as multiples of
R. R is the original with `clang -O2`; G the original with `weavec-cc -O2`;
S RFC 0041's conversion with `-fweavec-safe`; T the typed conversion with
`-fweavec-typed`; A R's objects linked by this compiler (its runtime, no
guard), A0 by main's. Benchmark drivers are guarded in G, S and T.

| Project                | Functions typed (guarded) | Lines changed vs original (vs Safe C) | Rounds (first-round errors) | Tests, 3 modes   | G    | S    | T        | A    | A0   |
| ---------------------- | ------------------------- | ------------------------------------- | --------------------------- | ---------------- | ---- | ---- | -------- | ---- | ---- |
| jsmn                   | 6/6 (0)                   | 8.7% (0)                              | 1 (0)                       | pass             | 1.59 | 1.72 | **1.30** | 1.04 | 1.04 |
| zlib                   | 153/170 (17)              | 4.2% (+2.3%)                          | 3 (43)                      | pass             | 1.32 | 1.27 | **1.13** | 1.00 | 1.00 |
| sds                    | 48/53 (5)                 | 20.5% (+4.0%)                         | 2 (10)                      | pass             | 2.20 | 2.09 | **1.69** | 1.71 | 1.70 |
| cJSON                  | 116/122 (6)               | 13.5% (+7.8%)                         | 2 (11)                      | 17/18, as Safe C | 2.50 | 2.52 | **1.90** | 1.75 | 1.76 |
| inih                   | 11/14 (3)                 | 34% (+18%)                            | 2 (14)                      | pass             | 2.40 | 2.52 | **1.70** | 1.00 | 1.00 |
| tinyexpr               | 47/51 (4)                 | 32% (+16%)                            | 2 (14)                      | pass             | 1.10 | 1.24 | **1.15** | 1.00 | 1.00 |
| http-parser (held-out) | 18/18 (0)                 | 1.6%                                  | 3 (38)                      | pass             | 1.22 | —    | **1.17** | 1.00 | 1.00 |
| bzip2 (held-out)       | 61/73 (12)                | 9.6%                                  | 4 (599)                     | 12/12            | —    | —    | —        | —    | —    |

The port (T7): Go's `compress/lzw` (580 lines of Go and 550 of tests) into
typed C and into Rust by two agents given the same brief, specification,
golden vectors and benchmark at the same moment.

|                              | Rounds to a clean build (errors) | Rounds to tests passing | Wall time  | Library lines | Functions typed          | Benchmark (×R) |
| ---------------------------- | -------------------------------- | ----------------------- | ---------- | ------------- | ------------------------ | -------------- |
| Typed C                      | 3 (2, `call-args`, in the tests) | 3                       | 6 min 51 s | 503           | 19/19 (42/44 with tests) | 1.10           |
| Rust (std only, no `unsafe`) | 1 (0)                            | 2                       | 3 min 50 s | 471           | —                        | 0.93           |

R here is the typed C sources with `clang -O2`. Typed C runs at 1.18 times
the Rust port.

| Gate                   | Result                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| T1 Rules               | **Pass**: each added rule has accepted and rejected forms in `test/Safe/typed-rules.c`, each error a note.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| T2 Conversion          | **Partly met.** All six projects compile typed, with the same functions safe or more (sds one guarded helper fewer, zlib and cJSON one more: `void *` buffers and in-place string writes), and pass their tests in default, full and verify modes. The line criterion (at most 10 points more of the project's lines than Safe C changed) is met by four, not by inih (34% against 18%) and tinyexpr (32% against 19%), whose string handling (`string-write`, `string-index`) was rewritten into counted buffers and offsets. linenoise, the seventh conversion, was not moved. Rounds to zero errors: 1 to 3.                                                                                     |
| T3 Held-out            | **Pass**: http-parser 18/18 typed with no helper, bzip2 61/73 (83.6%, 60 of its 64 original functions); tests pass in the three modes.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| T4 Detection           | **Pass**: `test/cases/typed/` (8 cases) and `test/Guards/typed-*.c`: an index past a counted field, past an array field inside its object, a local, a conversion that does not cover its count, a guarded function's lie reported under `full` and `verify`, a use after free reported under `verify`, and a dangling chain that never reaches a reused block across sweeps.                                                                                                                                                                                                                                                                                                                        |
| T5 No shadow           | **Partly met.** No `typed-unbounded` access in jsmn, inih and http-parser; the others keep some: pointers loaded from places no declaration bounds (zlib's error-message array and `inflateBack`'s callback, sds's header idiom through a guarded helper, extern pointers), counts that grow alone, and string steps no comparison covers.                                                                                                                                                                                                                                                                                                                                                          |
| T6 Run                 | **Not met.** Geometric mean of T 1.45 (S: 1.82), worst 1.90 (cJSON), against 1.20 and 1.50. Of the programs that do not allocate in their hot path, zlib (1.13) and tinyexpr (1.15) meet 1.15; jsmn (1.30) keeps a comparison per iteration of a loop whose range nothing hoists (loop versioning for comparisons is not built, amendment 1), and inih (1.70) spends its time in its guarded parts (the benchmark driver's strings and the `void *` reader's helpers), which T-A1's boundary checks at each crossing. sds and cJSON run at their allocator's floor plus their typed code's cost: sds's 69 range checks on its header helper, cJSON's string library calls through checked wrappers. |
| T7 The port            | **Pass on the numbers it sets**: 100% of the library typed, tests pass in all three modes with no report and no compiler problem found, and typed C runs within 1.25 times Rust (1.18). Rust took fewer rounds (1 against 3) and about half the time.                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| T8 No change elsewhere | **Pass**: `corpus-gate.py --quick` passes, every config guarding and proving the same accesses as on main; R's objects run at the same speed on this runtime and on main's (A against A0), including the sweeps.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| T9 Verify              | **Pass**: no `weavec.proven` in any conversion's tests, nor in the port's.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| H1 Hygiene             | **Pass with amendment 20**: library 14,200 of 14,200 lines, runtime 9,938 of 10,500.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |

What the gates say about the goal: an agent moves C to typed C in one to
three rounds of compile errors, and writes new typed C from another
language's specification with every function typed, as fast as a Rust
port runs within a fifth; typed code that keeps its strings and buffers
in counted forms costs nothing where the index is known and one comparison
where it is not. The rest is not yet Rust's: a loop's comparison is not
hoisted out of the loop, every crossing into guarded code is checked
against the shadow, the allocator costs what RFC 0041 measured, and an
agent iterates faster in Rust (fewer rounds, half the time). Those are the
next steps: comparisons as loop ranges, typed callbacks and `void *`, and
the allocator.
