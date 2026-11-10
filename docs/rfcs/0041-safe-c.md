# RFC 0041: Safe C

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-09
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Adds a tier to the guarantee of RFC
  0035–0039 (*Soundness*) without changing what it says about the code
  that does not opt in. Amends AGENTS.md rule 2 (WeaveC gets compile-time
  diagnostics, for safe functions only), rule 3 (`WEAVEC_UNSAFE` stops
  being the only annotation) and rule 9's library line budget; the
  enforcement ledger (a new reason, RFC 0035 §9); the case
  runner's report kinds. Revisits, with a different premise, the opt-in
  checked code of RFCs 0018–0029 (*Motivation*, *Alternatives*). Its
  stage S0 checks on paper that the design extends to C++ (§12) before
  any of it is built.

This RFC was written at the request of the project owner on 2026-11-09,
who stated the goal it serves: C and C++
projects, and projects that would otherwise move to Rust, have AI agents
rewrite code to Rust because Rust's compiler rejects memory-unsafe code
and an agent can iterate on its errors until the code is safe. WeaveC
should give them a reason to stay in C (and later C++) by offering the
same loop: a compiler that rejects code it cannot show is memory safe,
with errors an agent can act on, and code that, once accepted, is as safe
as safe Rust's and about as cheap. The same day the owner asked for it to
be implemented end to end in one change instead of being reviewed alone;
Accepted records that authorization, not an independent review. The
*Implementation amendments* at the end override the body.

## Summary

WeaveC today makes unmodified C and C++ memory safe at run time: every
access is guarded against the runtime's shadow, and released memory is
reused only after a sweep finds no pointer to it. That costs about 1.7 to
1.9 times the reference build's CPU time, says nothing until a bad access
runs, and gives an agent nothing to iterate on at compile time.

This RFC adds **safe functions**: C functions, marked `WEAVEC_SAFE` or
compiled under `-fweavec-safe`, that WeaveC checks against a small set of
local rules at compile time, rejects with an error naming the rule and the
fix when they break one, and then compiles with bounds checks in place of
guards. The design divides memory safety in two:

1. **Spatial safety by a type discipline** (§2–§4). Every pointer in a
   safe function has bounds the compiler knows: a pointer in a signature,
   a field or a global points to one object unless an annotation says how
   many (`WEAVEC_COUNTED_BY(n)`, `WEAVEC_SIZED_BY(n)`, `WEAVEC_ENDED_BY(e)`,
   `WEAVEC_NULL_TERMINATED`, Clang's `counted_by` on fields); a local
   pointer has the bounds of what it was computed from. Arithmetic,
   indexing, casts and stores that could leave those bounds are compile
   errors or, where only the run-time value can tell, a comparison against
   the bounds that traps. This is the model of Apple's `-fbounds-safety`
   (*Prior art*), checked by WeaveC because Clang 23 does not implement it.
2. **Temporal safety by the sweep** (§6). Released heap memory is not
   reused while any pointer to it exists, as RFC 0036 and RFC 0037 already
   guarantee for every program WeaveC links. A safe function therefore
   needs no check against use after free to stay memory safe: a dangling
   pointer can only reach the bytes of the block it pointed to, never
   another object, as in a garbage-collected language. The stack needs
   two small rules. A safe function's pointer parameters are *borrowed*
   for the call (§5): storing one is an error unless the parameter is
   `WEAVEC_ESCAPES`, and returning one is an error unless it is
   `lifetimebound`, Clang's attribute for "the result may point into this
   argument". A local whose address may outlive its scope is not an
   error: the compiler places it on the heap, as Go does, and the sweep
   keeps its bytes from reuse while a pointer to it exists (§6).

Pointers that enter safe code from code the checker did not see (a
parameter of a function guarded code can call, a pointer loaded from
memory, the result of a call to a function that is not safe) are
validated once, against the shadow, when they enter (§5); inside, accesses
compare against bounds and never read the shadow. Guarded code is
unchanged: a program can convert one function at a time, and an
unconverted function keeps today's guarantee.

Expected result: an agent can convert small and medium C libraries
function by function to safe C, guided only by WeaveC's errors and its
guide; the converted code runs within about 1.25 times the reference
build, against 1.7 for the same code guarded (*Detailed design* §14, the
gates). C++ is not implemented here, but every decision above is chosen
to extend to it, and S0 checks that on paper by converting a C++ library
before the checker is written (§12).

## Motivation

### What WeaveC offers today, compared with Rust

The guarantee of RFCs 0035–0040 holds for C and C++ that nobody changed,
which is what a drop-in compiler is for. Measured against what an agent
rewriting a program to Rust gets:

- **When bugs show.** Rust rejects code whose memory safety it cannot
  show, before the program runs, on every path. WeaveC stops a bad access
  when it runs: a bug on a path no test runs ships, and stops the program
  in production instead of corrupting it. WeaveC has no compile-time
  diagnostics (AGENTS.md rule 2).
- **What an agent can iterate on.** An agent rewriting to Rust compiles,
  reads errors, and fixes them; the borrow checker's errors are the loop
  that makes the rewrite converge. An agent improving C under WeaveC can
  only run tests and read reports, which requires tests that reach the bug.
- **Cost.** Safe Rust pays for bounds checks on indexing and nothing for
  temporal safety. Guarded C paid 1.72 times the reference build's cycles
  on the corpus benchmarks on darwin-arm64 (RFC 0037, G7), 1.93 times on
  linux-aarch64 (RFC 0038, G6; ASan 2.17), and C++ 1.90 (RFC 0039, G7).
  Projects choose C and C++ largely for speed; a safe version that costs
  70–90% more loses to safe Rust on the axis they care about.

So for a project deciding between "rewrite in Rust" and "stay in C",
WeaveC is today a hardening tool for the C it keeps, not a target to
rewrite into.

### Why checked code failed before, and why that does not decide this

RFCs 0018–0029 made checked code opt-in: selected functions had to be
proved against safety contracts. RFC 0030's *Motivation* records the
result: complete contracts on the corpus stalled at 150 of 1,619
definitions after nine RFCs (RFCs 0026–0029 added about 157K lines for
two more), checked mode rejected 17 of 28 correct probe programs, cJSON
took 45–65 s to check and Lua more than 1,188 s, and the default mode's
bug claims were about 92% false.

Every one of those measurements was taken on **upstream code nobody was
allowed to change**. Checked code tried to prove C as people already
write it: realloc wrappers that lose the correlation between result and
input, object invariants, state machines, recursive workflows. Each idiom
needed an inference family, and the families never converged.

Rust does not work that way. Its checker is simple, local and strict, and
code is written to fit it: what it cannot check is written differently, or
behind `unsafe` in a small library (`Vec`, slices) that safe code uses.
The rewriting is the price, and an agent now pays most of it. This RFC
takes the Rust bargain for C: the rules are simple enough that each is
checked inside one function from that function's own declarations, and
code that does not fit them is rejected, not inferred. It never tries to
prove an existing idiom; it tells the agent which rule the code breaks
and what the accepted form is.

### Why the temporal half is already done

The hardest part of Rust's model to write code for, and the part RFC 0001
modeled first, is ownership and lifetimes: who frees what, and that no
reference outlives its referent. WeaveC's runtime already guarantees, for
every program it links, that a released heap block is not reused while a
pointer to it exists (RFC 0037 (T)). With no reuse, a dangling heap
pointer cannot read or corrupt another object; that is the guarantee
garbage-collected languages give, and it is enough for memory safety
without a borrow checker. Only pointers to the stack need a rule, and a
small one (§5). This makes a safe C dialect far smaller than Rust's
ownership model or Checked C's and Cyclone's (*Prior art*).

## Soundness

### The guarantee for safe functions

For a program that meets the precondition of the existing guarantee
(`docs/pages/reference/guarantees.md`: every unit compiled by `weavec-cc`
in an enforcing mode, one runtime, the runtime's allocator), every access
in a safe function (§1) satisfies:

- **(S1) Bounds.** A load, store or memory intrinsic of *w* bytes at
  address *a* in a safe function lies within the bounds of the pointer it
  is computed from (§2), or the program traps before it
  (`pointer-out-of-bounds`), or the function did not compile.
- **(S2) Entry.** A pointer that enters safe code (§5) points, when it
  enters, to the start or inside of bytes that are addressable across its
  whole bound: one live tracked object (every tracked object is followed
  by bytes no object owns, so an addressable range cannot span two) or
  memory the runtime does not track (A1). Otherwise the program traps
  (`invalid-bounds`, or `heap-use-after-free` for a released block).
- **(S3) Heap lifetime.** A heap pointer in safe code reaches only the
  bytes of the block it was derived from, whether or not the block has
  been released: a released block is not reused while the pointer exists
  (T), and its bytes stay mapped or fault.
- **(S4) Stack lifetime.** No pointer to a stack object of a safe
  function outlives the object's scope: a local whose address may outlive
  it is on the heap (§6), and a borrowed parameter (§5) is not stored in
  memory, and is returned only through a `lifetimebound` parameter, whose
  result the caller treats as borrowed from the same argument. An escaping
  parameter (`WEAVEC_ESCAPES`) does not point into a stack
  (`stack-escape`).
- **(S5) No forged pointers.** No pointer in safe code is made from an
  integer, from bytes of a non-pointer type, or from a union member that
  may hold something else (§3).
- **(S6) Library calls.** A call from a safe function to a function in
  the library table accesses, through each memory argument, only the
  bytes the row says it does, and those lie within the argument's bounds,
  or the program traps (§9).

Accesses in guarded functions keep (A) through (P) unchanged.

### Bugs caught

```c
#include <weavec.h>
#include <stddef.h>

WEAVEC_SAFE int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  int s = 0;
  for (size_t i = 0; i <= n; i++)  /* reads v[n]: pointer-out-of-bounds */
    s += v[i];
  return s;
}

WEAVEC_SAFE int first(const int *p) {
  return p[1];                     /* error: p is a single pointer;
                                      indexing it needs WEAVEC_COUNTED_BY
                                      [weavec-safe:index-single] */
}

struct node { struct node *next; int v; };
WEAVEC_SAFE struct node *keep(struct node *n) {
  static struct node *last;
  last = n;                        /* error: n is borrowed; storing it needs
                                      WEAVEC_ESCAPES [weavec-safe:escape] */
  return n;                        /* error: returning it needs
                                      lifetimebound [weavec-safe:escape] */
}
```

Every overflow through a bounded pointer traps at the access or is
rejected at compile time, including an overflow inside an object (a
counted field that indexes past its own count, which the shadow cannot
see). Every pointer handed in from guarded code is validated once.

### Bugs deliberately not caught

- **A use after free in safe code is not reported.** It reads or writes
  the released block's own bytes (S3), so it cannot corrupt another
  object or leak another object's data, but the program goes on with
  stale data, as it would in Go or Java after logically discarding an
  object. `-fweavec-safe-checks=full` (§7) keeps every guard in safe
  functions too, so tests report it as today.
- **Data races.** Two threads that race on a pointer and its count (a
  counted field and its `n`) can let a reader pair a new pointer with an
  old count (S-A3). Rust rules this out with `Send` and `Sync`; this RFC
  does not (*Future work*).
- **Bad bounds from code WeaveC does not compile.** A precompiled
  library that stores a pointer with a wrong count into an annotated
  field is caught when safe code loads the pointer (S2) only if the bytes
  the count claims are not all addressable; a count that overstates a
  pointer into memory the runtime does not track passes (A1).
- **Overflow inside an object, through a pointer validated on entry.**
  Validation (S2) works at the shadow's granularity: an over-long count
  for a field inside a heap block is caught only if it reaches past the
  block.
- **Leaks**, integer overflow other than in sizes the checker computes,
  and logic errors.

### Accepted rejections

A safe function rejects correct C that uses an idiom the rules do not
cover: pointer arithmetic on a single pointer, `void *` user data cast
back to its type, unions holding pointers, pointer tagging, `container_of`,
storing a parameter in a structure without `WEAVEC_ESCAPES`, and others
(§3). That is the design, not a defect: the code is rewritten to an
accepted form, or kept in a guarded function that the safe one calls.
The cost of the trade is measured by gates G2 and G3.

### Assumptions

The existing assumptions A1–A9 hold. Safe functions add:

| | Assumption |
| --- | --- |
| **S-A1** | Annotations on a function that is not safe (a guarded or precompiled function's declaration that safe code calls) are true: its pointer results have the bounds their annotations state. They are validated against the shadow on return (S2), so a false one is caught when it overstates past an object. |
| **S-A2** | Code not compiled by `weavec-cc` does not write a borrowed pointer into memory that outlives the call, and does not keep a pointer it was passed beyond the call unless its declaration says `WEAVEC_ESCAPES`. |
| **S-A3** | No data race on a pointer and the count or end that bounds it. |
| **S-A4** | The pointers that keep a heap block alive meet A6 (the sweep sees them). |

## Detailed design

### 1. Safe, guarded and unsafe functions

Every function WeaveC compiles is in one of three tiers:

| Tier | How | Compile time | Run time |
| --- | --- | --- | --- |
| **Safe** | `WEAVEC_SAFE` on the definition, or any function of a unit compiled with `-fweavec-safe` | the rules of §3; an error stops the build | bounds checks (§4), entry validation (§5); no guard reads the shadow |
| **Guarded** | the default; `WEAVEC_GUARDED` in a `-fweavec-safe` unit | none, as today | guards, as today |
| **Unsafe** | `WEAVEC_UNSAFE`, `no_sanitize("address")`, as today | none | none, as today |

A safe function contains no `WEAVEC_UNSAFE` block: code that needs one is
moved into a guarded or unsafe function the safe one calls, which keeps
every exemption visible in a signature. (Whether to allow unsafe blocks
inside safe functions, as Rust does, is an *Unresolved question*.)

The tier is a property of the definition. A declaration may carry the
annotations of §2 whatever the tier of its definition; a call from a safe
function to a function whose definition is not visible treats it as not
safe (§5).

Under a compiler that is not WeaveC, every annotation in this RFC expands
to nothing: safe C is plain C, and the code builds anywhere.

### 2. Pointer bounds

Every pointer value in a safe function has bounds, a range of bytes
[*lo*, *hi*), known to the compiler either statically or as values it
keeps alongside the pointer. Where they come from:

| Pointer | Bounds |
| --- | --- |
| A parameter, a function's result, a field, a global, or a pointer loaded from memory, with no annotation (**single**) | one object of the pointee type: [*p*, *p* + `sizeof *p`) |
| `T *WEAVEC_COUNTED_BY(n) p` (also `counted_by(n)` on a field, Clang's attribute) | [*p*, *p* + *n* × `sizeof(T)`) |
| `void *WEAVEC_SIZED_BY(n) p` (also Clang's `sized_by(n)` on a field) | [*p*, *p* + *n*) |
| `T *WEAVEC_ENDED_BY(e) p` | [*p*, *e*) |
| `char *WEAVEC_NULL_TERMINATED s` | [*s*, the first NUL] (§3, rule `string`) |
| The `_OR_NULL` forms (`WEAVEC_COUNTED_BY_OR_NULL`, Clang's `counted_by_or_null`, …) | as above, or null with any count |
| An array `T a[N]`, or the address of a local or global | the object |
| A string literal | the literal with its NUL |
| The result of an allocation (`malloc(n * sizeof *p)`, `calloc(n, sizeof *p)`, `realloc(q, …)`, `aligned_alloc`) | the bytes requested, with the multiplication checked for overflow |
| A local pointer variable (**wide**) | the bounds of the value last assigned to it, carried as hidden values |
| `p + i`, `&p[i]`, `p->f` (the address of a field) | `p + i` keeps *p*'s bounds; `&p->f` has the field's bounds |

A count or end names a parameter of the same function, a field of the
same struct, or an integer constant; it is not a general expression in
this RFC. A count is read when the pointer is used, so a function that
changes its own count parameter must assign the pointer and the count
together (§3, rule `counted-pair`).

Wide locals never reach memory or another function as wide: storing one
converts it to the destination's kind, with a check (§4). That keeps
every ABI the C ABI: a safe function is called, and its structs are laid
out, exactly as the same C compiled by Clang.

### 3. The rules

Each rule has a stable identifier, printed with every error it raises
(`[weavec-safe:<id>]`) and documented in the guide (§13) with a rejected
example and its accepted rewrite. The rules are checked inside one
function, from the declarations it can see; no rule depends on what
another function does.

| Id | Rule |
| --- | --- |
| `index-single` | A single pointer is dereferenced (`*p`, `p->f`) but not indexed or offset (`p[i]`, `p + i`, `p++`) except by 0. |
| `arith-bounds` | Arithmetic is on a pointer with bounds; the result keeps them. Subtracting two pointers needs both to derive from one bounded value. |
| `int-to-pointer` | No integer is converted to a pointer, except the constant null. |
| `pointer-cast` | A pointer converts only to its own type with fewer qualifiers, to `char *`/`unsigned char *`/`void *` (keeping its bounds in bytes), or from `void *` when the value is an allocation's result or a pointer that was converted from the same type in this function. Function pointers do not convert. |
| `union-pointer` | A union member that holds a pointer is not read. |
| `pointer-bytes` | No memory that holds a pointer is written as bytes (`memcpy`, `memset` with a nonzero value, a `char *` store) except by copying from an object of the same type. |
| `counted-pair` | A counted or ended pointer and its count or end are assigned together, in adjacent statements, from one bounded value; decreasing a count alone is allowed and checked at run time. |
| `escape` | A borrowed pointer (a parameter not marked `WEAVEC_ESCAPES`, or anything derived from one, including the result of a call that received it through a `lifetimebound` parameter) is not stored in memory other than a local, not passed to a parameter marked `WEAVEC_ESCAPES`, and returned only through a `lifetimebound` parameter. The address of a local is never an error: where it may outlive the local's scope, the local is placed on the heap (§6). |
| `string` | A null-terminated pointer is read at index 0; it advances by one only where a test that its current byte is not NUL dominates the advance; `strlen(s)` gives it bounds [*s*, *s* + `strlen(s)` + 1) until it changes. |
| `call-args` | An argument to a parameter with bounds has bounds that cover it (checked statically or at run time); an argument to a single parameter points to at least one object; a call to a function in the library table passes each memory argument with bounds that cover what the row says the call accesses (§9). |
| `variadic` | A variadic call is only to a function of the library table whose row describes its format. |
| `no-asm` | No inline assembly, `setjmp`, `longjmp`, `__builtin_frame_address` or computed `goto`. |
| `unsafe-block` | No `WEAVEC_UNSAFE` block inside a safe function. |

The rules are written to be satisfiable by rewriting, never by proof: a
rule rejects a construct, and the guide says what to write instead. Where
a rule cannot be decided from the code (an index against a count known
only at run time), the compiler inserts a check (§4) rather than an
error. Each error is a Clang diagnostic with a fix note, so an agent can
read it in Clang's SARIF output (`-fdiagnostics-format=sarif`).

### 4. Checks inserted in safe functions

- **Bounds.** An access through a pointer with run-time bounds compares
  [*a*, *a* + *w*) against [*lo*, *hi*) and traps
  (`pointer-out-of-bounds`) if it is outside. The comparisons are plain
  IR, so LLVM hoists them out of loops and folds those an induction
  variable already implies (`for (i = 0; i < n; i++) v[i]` keeps none).
- **Conversions.** Storing a wide local into a single field checks that
  one object fits; into a counted field with its count, that the count
  fits; a decreased count, that it is not more than the old one.
- **Null.** A single pointer that may be null is dereferenced at an
  offset under 4 KiB without a test (the null page faults, and the fault
  handler reports `memory-fault`); a larger offset, and any bounded
  pointer whose count may be nonzero while it is null, is tested.

No access in a safe function is guarded against the shadow, except the
validations of §5 and the library calls of §9.

### 5. The boundary: borrowing and validation

**Borrowed and escaping pointers.** A safe function's pointer parameters
are borrowed for the call unless marked `WEAVEC_ESCAPES` (rule `escape`):
the function may read and write through them, pass them on to other
borrowed parameters, and keep them in locals, but not store them where
they outlive the call. That is the only lifetime the rules know besides
"heap or global", and `lifetimebound` ties a result to an argument (a
search returning a pointer into its buffer, an accessor returning a
pointer into its object). A caller's local passed to a borrowed parameter
stays on the stack; passed to an escaping one, or kept through a
`lifetimebound` result beyond its scope, it is placed on the heap (§6). A
function that stores its argument in a structure, a list push, says so in
its signature.

**Validation.** A pointer *enters* safe code when the checker did not see
where its bounds came from:

1. **Parameters**, when the function can be called from code that is not
   safe (external linkage, or its address is taken). Such a function gets
   two entry points: its C symbol validates each pointer parameter
   against its bound and then calls the internal entry, which safe
   callers in the same image call directly. Validation is the existing
   range guard (`__weavec.guard.range`) over [*lo*, *hi*); an escaping
   parameter is also tested not to lie in the current thread's stack.
2. **Loads**: a pointer loaded from memory is validated once, at the load,
   against its bound (a single pointer's one object, a counted field's
   count).
3. **Results** of calls to functions that are not safe, against their
   declared bound.

A validated pointer then carries static or wide bounds like any other.
Validation also catches a pointer to a released block when it enters
(the range guard reports `heap-use-after-free`).

Validation at loads is the cost of mixing safe and guarded code: guarded
code may write any field. A pointer-chasing loop in safe code pays one
range guard per pointer it loads, about what it pays today for its guarded
accesses; an indexing loop pays only bounds comparisons. A program whose
every unit is safe could skip load validation (*Future work*).

### 6. Temporal safety

- **Heap.** Nothing is added: (T) already makes a released block's bytes
  unreusable while any pointer to it exists, for every image the runtime
  serves. (S3) follows from (T) and from the runtime keeping a released
  slot's metadata outside the slot (`weavec_alloc.c`: the free lists and
  quarantine queues live in the region metadata) and huge blocks reserved
  without access until a sweep.
- **Stack: parameters.** Rule `escape` keeps borrowed pointers in the
  frames that borrowed them.
- **Stack: locals placed on the heap.** The checker decides, inside the
  function, whether the address of each local (an array, a struct, a
  scalar whose address is taken) may outlive the local's scope: whether it
  or a pointer derived from it may be stored in memory other than a local,
  returned, passed to an escaping parameter, or kept through a
  `lifetimebound` result past the scope. Such a local is allocated from
  the runtime's allocator (zeroed, as locals are) when its scope begins
  and released when it ends, and is the heap block (S3) protects: a
  pointer that outlived it reaches only its bytes, and they are not reused
  while the pointer exists. Every other local stays in the frame. The
  checker emits a remark for each local it places on the heap
  (`-Rweavec-safe-heap-local`), so an agent can see and restructure the
  ones on a hot path; their cost is measured by G5.
- The existing frame layout and (U) are unchanged for guarded functions.
- **Globals** live as long as the image.

### 7. Guard removal, `verify` mode and testing mode

The guard passes treat safe functions as follows: `GuardInsert` places
its markers as today; the safe-code pass (§8) replaces each access's
`__weavec.guard` with its bounds check, and records the removed guard
with the ledger reason `safe` (PROVEN). The removal depends only on the
function's own code and its entry validation, so it is local to the
function as RFC 0035 §6 requires, and `-fweavec-checks=verify` keeps
every removed guard as a monitor that reports `weavec.proven` if it would
have failed.

`-fweavec-safe-checks=bounds` (the default) compiles safe functions as
above. `-fweavec-safe-checks=full` keeps every guard in safe functions as
well as their bounds checks: tests then report uses after free in safe
code, at the guarded cost.

### 8. Implementation

- **The checker** (`lib/Frontend/SafeCheck.cpp`, `include/weavec/
  Frontend/SafeCheck.h`): an AST visitor over safe functions, after Sema,
  in the `weavec-cc` cc1 job that already runs the unsafe-region
  collector. It reads the annotations from the types (`annotate_type`
  attributes and Clang's `CountAttributedType`), tracks wide locals' bounds
  along Clang's CFG, applies the rules, and emits Clang diagnostics with a
  custom diagnostic ID per rule and a fix note.
- **Bounds in IR** (`lib/Frontend/GuardSafe.cpp`): at
  `PipelineStartEP`, after `GuardInsert`, for each guarded access in a
  safe function, trace the pointer to its bounded root (a parameter, an
  allocation, an `alloca`, a global, a load from an annotated field, a
  call result) with the root-tracing of `GuardExpand` (RFC 0038 §3), and
  replace the guard with a `__weavec.bounds(ptr, lo, hi, width)` marker
  that `GuardExpand` expands into the comparison. The checker hands the
  pass a table of each safe function's parameter bounds and annotated
  field offsets. An access the pass cannot root keeps its guard and is
  counted in the ledger (`safe-unrooted`); the test suites require none.
- **Validation**: the two entry points, the load and result validations
  as range markers, inserted by the same pass.
- **The runtime**: report kinds (§*Diagnostics*), the stack-bounds test
  for escaping parameters (`__weavec_rt_on_stack`), about 200 lines.

### 9. Library calls

The library table is the source of what a call accesses (rule 9). In a
safe function, a call to a function with a row checks each memory
argument's bounds against the bytes the row says the call reads or
writes, with comparisons where the bounds are known (`memcpy(d, s, n)`
with both counted: `n` against both counts) and the row's existing guard
where they are not. Calls the table redirects to the runtime's checked
wrappers keep them. A string argument of a row takes a null-terminated
or bounded pointer. Functions with no row and no visible safe definition
take single or bounded arguments and are trusted to stay within them
(S-A1); their results are validated (§5).

### 10. The driver

- `-fweavec-safe`: every function defined in the unit is safe unless
  marked `WEAVEC_GUARDED` or `WEAVEC_UNSAFE`.
- `-fweavec-safe-checks=bounds|full` (§7).
- Errors of the rules are errors, not warnings; `-Wno-…` does not turn
  them off. A function the checker rejects stops the build.
- `-fweavec-checks=none` and the other modes without the runtime (the
  existing guarantee's *Without the runtime*) still run the checker, so an
  agent can iterate on errors without the runtime; they insert no checks.

### 11. The ledger

Ledger version 4 adds the reason `safe` under PROVEN and the counter
`safe-unrooted`; the summary line (`-fweavec-summary`) gives the number of
safe functions and of their accesses that kept a guard.

### 12. C++

`weavec-c++` rejects `WEAVEC_SAFE` and `-fweavec-safe` with an error in
this RFC; safe C++ is a later RFC, written once safe C's conversions
(S5) have measured the rules. The design is chosen so that it extends,
and S0 checks that on paper before anything is built.

**What carries over.**

- **Bounds.** `std::span`, `std::string_view`, `std::array` and the
  containers are bounded types already, and the hardened C++ library
  checks their indexing (RFC 0039, `library-assertion`). Raw pointers take
  the same annotations: `annotate_type` is a C++ attribute in the first
  place.
- **Heap lifetimes.** C++'s worst memory bugs, iterator invalidation and
  references into reallocated storage (`auto &x = v[0]; v.push_back(1);
  x = 2;`), are memory safe under the sweep without a borrow checker: the
  old buffer is not reused while `x` points into it. The write is a logic
  bug, not a corruption of another object. Use after `delete` and after a
  smart pointer's release are the same case.
- **The unsafe constructs** (`reinterpret_cast`, unions, placement `new`,
  type punning) map onto rules `pointer-cast`, `union-pointer` and
  `pointer-bytes`.
- **`lifetimebound`** is Clang's C++ attribute, with the same meaning and
  the same spelling.

**The risk: stack lifetimes.** C++ makes pointers into the stack in more
ways than C, and they are where a checked C++ subset needs the most
(Safe C++ added lifetimes and a new standard library for them):

- references as parameters and results, which are borrowed pointers
  under another name;
- lambdas capturing by reference, which escape when stored in a
  `std::function` or returned;
- temporaries: `std::string_view sv = std::string("hi");` views a
  temporary whose characters, with the short-string optimization, are in
  the temporary itself, on the stack;
- `this` escaping from a constructor or a member function; coroutine
  frames.

Locals placed on the heap (§6) are the answer the C design already
contains: a local, a temporary or a lambda's captured frame whose address
may outlive its scope goes on the heap and is protected by the sweep,
with no lifetime annotations. The open question is cost and how often it
fires in idiomatic C++, which S0 estimates and the later RFC measures.

**Decisions here that must extend.** The annotations are type attributes;
borrowed parameters are what references become; validation happens where
a pointer enters safe code, which in C++ includes a virtual call's
arguments; the two entry points work with mangled names; the library
table's mangled rows (RFC 0039) describe the C++ library's calls as its C
rows describe C's.

### 13. Documentation

- `docs/pages/guides/safe-c.md`: the tiers, the annotations, every rule
  with a rejected example and its accepted rewrite, and a procedure for
  converting a module, written to be given to an agent as its only
  instructions.
- `docs/pages/reference/guarantees.md`: (S1)–(S6) and S-A1–S-A4.
- `docs/pages/reference/cli.md`: the flags and the report kinds.
- `docs/annotations.md` and `weavec.h`: the annotations.

### 14. Stages and gates

The stages:

- **S0 The rules on paper and the baseline.** The guide's rules written
  first; jsmn and sds converted by an agent against the paper rules, with
  no checker, and the diffs reviewed for rules that are unworkable; the
  conversion set and the held-out set frozen (the held-out projects'
  commits recorded and not opened until S6); the guarded cost of the
  conversion set measured on main. If converting jsmn or sds needs more
  than a few guarded helpers, the rules change before S1. **The C++
  check:** the rules of §12 written on paper for references, lambdas,
  temporaries and views; Snappy (RFC 0039's `fresh39`) converted on paper
  by an agent against them; a record, for each construct the conversion
  meets, of the rule or the heap-placed local that handles it or the
  guarded helper it needs, and of the locals and temporaries that would go
  on the heap on Snappy's benchmark path. S0 ends with a decision: if a
  decision of this RFC (§2–§6, the annotation surface) does not extend to
  C++, it changes here, before S1.
- **S1 The annotations and the checker**: `weavec.h`, `SafeCheck`, every
  rule with lit tests (`test/Safe/<rule>.c`, Clang's `-verify` for the
  errors).
- **S2 Bounds in IR**: `GuardSafe`, the `__weavec.bounds` marker and its
  expansion, guard removal, the ledger.
- **S3 The boundary**: two entry points, load and result validation,
  escaping parameters.
- **S4 Library calls.**
- **S5 Conversion**: agents convert the conversion set; rule and message
  changes follow what they hit; the guide.
- **S6 Held-out conversions and the gates** on darwin-arm64,
  linux-aarch64 and linux-x86_64.

The gates (thresholds to be confirmed by S0):

- **G0 C++ (S0).** Snappy's paper conversion maps every construct to a
  rule, a heap-placed local or a guarded helper, with guarded helpers for
  at most 10% of its functions, and needs no change to §2–§6 that safe C
  could not take; otherwise S0 amends this RFC before S1.
- **G1 Rules.** Every rule has an accepted and a rejected lit test, and
  every error carries a fix note.
- **G2 Conversion.** jsmn, sds, cJSON and linenoise converted with every
  function safe except at most three guarded helpers each, and zlib with
  at least 90% of its functions safe; each project's own tests pass; at
  most 30% of each project's lines changed. Recorded: the agent's rounds
  of compile errors, its time, the guarded helpers and why.
- **G3 Held-out.** Two C projects from the held-out set, converted by an
  agent given only the guide: at least 80% of functions safe, tests pass.
- **G4 Detection.** `test/cases/safe/`: each spatial bug class traps or
  is rejected; each use after free in safe code reads only its own block
  (and traps under `full`); bad bounds handed in from guarded code trap as
  `invalid-bounds`; an escaping stack pointer traps as `stack-escape`.
- **G5 Run.** The converted programs' benchmarks: geometric mean at most
  1.25 times the reference build's CPU time and below the same code
  guarded, on each platform; the locals placed on the heap and their share
  of the time recorded.
- **G6 No change elsewhere.** A unit with no safe function compiles to
  the same object as on main (the corpus quick set), and the corpus gate's
  results do not change.
- **G7 Verify.** `-fweavec-checks=verify` reports no `weavec.proven` in
  the converted projects' tests; no access is `safe-unrooted`.
- **H1 Hygiene.** The checker and pass within a library budget raised from
  9,500 to 13,500 lines (the library code is at 8,844 today); the runtime
  within its budget plus 200.

## Annotation surface

New macros in `resources/include/weavec.h` (version 2.0). Under Clang
with `weavec-cc` they expand as listed; under any other compiler, to
nothing. Their spellings live in `weavec.h` and in `SafeAnnotations` in
`include/weavec/Frontend/SafeCheck.h`, which must agree (AGENTS.md
rule 3, amended).

| Macro | Attaches to | Expands to |
| --- | --- | --- |
| `WEAVEC_SAFE` | a function definition | `__attribute__((annotate("weavec.safe")))` |
| `WEAVEC_GUARDED` | a function definition in a `-fweavec-safe` unit | `__attribute__((annotate("weavec.guarded")))` |
| `WEAVEC_COUNTED_BY(n)` | a pointer type, after the `*` | `[[clang::annotate_type("weavec.counted_by", #n)]]` |
| `WEAVEC_COUNTED_BY_OR_NULL(n)` | the same | `[[clang::annotate_type("weavec.counted_by_or_null", #n)]]` |
| `WEAVEC_SIZED_BY(n)`, `WEAVEC_SIZED_BY_OR_NULL(n)` | the same | `annotate_type("weavec.sized_by…", #n)` |
| `WEAVEC_ENDED_BY(e)` | the same | `annotate_type("weavec.ended_by", #e)` |
| `WEAVEC_NULL_TERMINATED` | `char` pointer types | `annotate_type("weavec.null_terminated")` |
| `WEAVEC_ESCAPES` | a pointer type of a parameter | `annotate_type("weavec.escapes")` |
| `WEAVEC_LIFETIMEBOUND` | a parameter, after its name | `__attribute__((lifetimebound))`, Clang's own attribute, which Clang 23 accepts on C parameters as on C++ ones |

`annotate_type` keeps the annotation in the type, so it follows typedefs,
fields, parameters and results; Clang 23 accepts the `[[clang::…]]`
spelling in C11 and C17 as well as C23 (checked on Homebrew's Clang
23.1.0; the GNU spelling is not accepted). Clang's own `counted_by`,
`sized_by`, `counted_by_or_null` and `sized_by_or_null` on fields (the
Linux kernel's `__counted_by`) are read as the WeaveC forms. Clang 23
rejects `counted_by` on parameters, which is why WeaveC needs its own
spelling for them. `WEAVEC_UNSAFE` keeps its meaning.

## Diagnostics

**Compile-time errors** (new: WeaveC had none). One per rule of §3:

```
<file>:<line>:<col>: error: <what the code does> [weavec-safe:<id>]
<file>:<line>:<col>: note: <the accepted form>
```

For example:

```
sum.c:9:10: error: 'p' is a single pointer and cannot be indexed [weavec-safe:index-single]
sum.c:7:30: note: declare its count: 'const int *WEAVEC_COUNTED_BY(n) p'
```

**Run-time report kinds** (each pinned by a `test/Guards/` test and
listed in `cli.md` and `scripts/run-cases.py`):

- `pointer-out-of-bounds`: an access in a safe function outside its
  pointer's bounds.
  `weavec: pointer-out-of-bounds at <file>:<line>:<col>: <read|write> of <n> bytes at <address>, outside [<lo>, <hi>) of '<expression>'`

  ```c
  WEAVEC_SAFE int at(const int *WEAVEC_COUNTED_BY(n) v, size_t n, size_t i) { return v[i]; }
  int main(void) { int a[4] = {0}; return at(a, 4, 4); }
  ```
- `invalid-bounds`: a pointer entering safe code whose bound is not
  addressable.
  `weavec: invalid-bounds at <file>:<line>:<col>: <what> claims <n> bytes at <address>; <address> is not addressable`

  ```c
  WEAVEC_SAFE int total(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
  int main(void) { int *p = malloc(4 * sizeof *p); return total(p, 8); } /* guarded caller */
  ```
- `stack-escape`: an escaping parameter that points into a stack.
  `weavec: stack-escape at <file>:<line>:<col>: escaping parameter '<name>' points into the stack at <address>`

  ```c
  WEAVEC_SAFE void push(struct list *l, struct node *WEAVEC_ESCAPES n);
  void f(struct list *l) { struct node local; push(l, &local); } /* guarded caller */
  ```

## Drawbacks

- **It is a language.** The rules are a dialect of C, and the work is
  language design: every rule that is too strict costs conversions, and
  every one that is too loose costs the guarantee. RFCs 0018–0029 show
  how a checker can grow without converging; the protection here is that
  rules reject rather than infer, and the gates measure conversions, not
  proofs.
- **Converted code is WeaveC's dialect.** It still builds with any C
  compiler (the annotations vanish), but its safety exists only under
  WeaveC.
- **Mixed programs pay at the boundary.** Load validation costs about a
  guard per loaded pointer; G5 decides whether that is acceptable.
- **Weaker than Rust in three places**: data races (S-A3), a silent use
  after free that reads stale bytes (S3), and trust in the annotations of
  code that is not safe (S-A1).
- **WeaveC gets compile-time diagnostics again.** They are confined to
  functions that opt in, so drop-in compiles keep RFC 0033's promise of no
  false stops.
- **Line budget.** About 4,000 more library lines.

## Alternatives

- **Do nothing.** WeaveC remains a way to keep C without rewriting
  ("recompile, don't rewrite"), which is valuable but does not give a
  project a reason to choose C over Rust for new or rewritten code. This
  RFC keeps that path unchanged.
- **Rust's model: ownership, borrowing with aliasing rules, lifetimes.**
  The model of RFC 0001. It would also rule out data races and make
  temporal safety static, but it is the hardest part of Rust to write code
  for, needs annotations C has no syntax for, and repeats what the sweep
  already guarantees. Locals placed on the heap where they escape, and
  two lifetimes for parameters (borrowed for the call, heap or
  global) cover the stack, which is all the sweep leaves open.
- **Explicit span types** (`struct { T *ptr; size_t len; }` through
  macros) instead of annotated pointers: a smaller checker, but C has no
  generics, so every element type needs a declared span type; signatures
  change ABI; and code looks less like C and less like what agents and
  the Linux kernel already write (`__counted_by`).
- **Wait for Clang's `-fbounds-safety`.** Apple is upstreaming it, but
  Clang 23 has only `counted_by`, `sized_by` and their `_or_null` forms on
  fields; `-fexperimental-bounds-safety` exists as a `-cc1` flag that does
  not accept the pointer attributes (`__single`, `__indexable`,
  `ended_by`) and inserts no checks (measured on Homebrew's Clang
  23.1.0). This RFC takes its model and its field spellings now; if
  upstream lands, WeaveC can read its attributes as aliases.
- **Infer the bounds** (CCured's approach, RFCs 0011–0029's): measured to
  stall on unchanged code. Inference within one function, for wide
  locals, is kept; across functions, signatures say.
- **Compile errors only, guards kept.** Gives the agent its loop but no
  cost advantage over guarded code; kept as `-fweavec-safe-checks=full`.
- **Reject escaping locals** instead of placing them on the heap: no
  hidden allocations, but every `&local` that flows into a structure or a
  result becomes a rewrite, and C++'s temporaries and lambdas would make
  it the most common error of safe C++. Placing them on the heap costs an
  allocation where the address escapes, which the remark shows and G5
  measures.
- **C++ first, or a C++ RFC next.** C++ projects are more likely to
  consider Rust, but its rules are harder (references, templates,
  iterators, temporaries), and a C++ RFC written now would rest on C
  rules nobody has converted code against. C first proves the approach on
  the smaller language; S0's paper check (§12, G0) guards against a C
  design that would not extend.

## Prior art

- **Rust**: safe and `unsafe` code, slices carrying their length, bounds
  checks on indexing at run time, `extern` declarations trusted. Taken:
  the bargain (strict local rules, code rewritten to fit), run-time bounds
  checks, trusted declarations for foreign code. Not taken: the borrow
  checker's aliasing rules and lifetimes, `Send`/`Sync`.
- **Apple's `-fbounds-safety`** (the Clang RFC, 2023, and its
  documentation): pointers in ABI positions are `__single` by default;
  `__counted_by`, `__sized_by`, `__ended_by`, `__null_terminated`; locals
  are wide by default; assignments to a counted pointer and its count are
  grouped. Taken almost whole for spatial safety. It does not address
  temporal safety; WeaveC's sweep does.
- **CCured** (Necula et al., 2002–2005): pointer kinds (SAFE, SEQ, WILD)
  inferred over whole programs, with a conservative garbage collector for
  temporal safety. The closest precedent for "bounds plus a collector".
  Its whole-program inference and changed pointer representation hurt
  adoption; this RFC requires signatures to state kinds and keeps the C
  ABI.
- **Deputy** (Condit et al., 2007): dependent types for C (`count(n)`)
  checked locally with run-time checks; the ancestor of `counted_by`.
- **Checked C** (Microsoft Research, 2016–): checked pointer types
  (`_Ptr`, `_Array_ptr` with bounds declarations) and checked scopes.
  Spatial only, and a new type syntax that needs its own compiler.
- **Cyclone** (Jim et al., 2002): fat pointers, regions for lifetimes,
  and a conversion burden that kept it a research language; the lesson is
  to keep the lifetime system tiny.
- **Go and MarkUs/MiraclePtr**: escape-to-heap and quarantine-until-
  unreferenced as temporal safety without a borrow checker. Go's compiler
  decides inside a function which locals escape and allocates those on
  the heap; §6 does the same, with the sweep in place of a collector.
- **Clang's `lifetimebound`** and its `-Wdangling` warnings: an
  annotation, already used by C++ libraries, saying a result may point
  into an argument. Taken as the only way a borrowed pointer is returned.
- **Safe C++ (Circle) and C++ profiles**: proposals to add a checked
  subset to C++. Safe C++ needed lifetimes and a new standard library,
  largely for the stack references §12 lists; the bet here is that heap
  placement and the sweep make most of that unnecessary.

## Unresolved questions

1. **`void *` user data and callbacks** (`qsort`, `pthread_create`, every
   C library with a `void *ctx`). Rule `pointer-cast` rejects the cast
   back. Options: a typed-callback convention in the guide, a run-time
   type tag in the heap slot (the allocator knows the allocation site), or
   keeping such callbacks guarded. S0 should measure how often conversion
   hits this.
2. **Unsafe blocks inside safe functions.** Rust allows them; this RFC
   forbids them to keep exemptions in signatures. Conversions may show
   that a block is far easier for an agent than a helper function.
3. **Insertion in IR versus the AST.** §8 inserts bounds in IR at
   `PipelineStartEP`, before `mem2reg`, tracing roots through `alloca`s of
   locals. If that proves fragile, checks could be inserted in the AST
   through Sema as RFC 0030 did, at the cost of the deferred-CodeGen
   machinery RFC 0036 deleted.
4. **Load validation's cost** in pointer-chasing code, and whether a unit
   (or program) that is all safe may skip it.
5. **Null-terminated strings entering safe code** are validated by
   scanning to the NUL (the runtime's guarded scan): O(*n*) per entry. Is
   that acceptable, or should strings enter bounded?
6. **Generic containers.** C's vectors and hash tables are macros or
   `void *` code; what the guide recommends (macro-generated typed code is
   accepted by the rules as written) needs conversion experience.
7. **The gates' thresholds** (30% changed lines, 80% held-out, 1.25×) are
   guesses until S0's baseline and the first conversions.
8. **The agent protocol**: which model, which prompt, and how its rounds
   are counted, so G2 and G3 are reproducible.
9. **Locals placed on the heap**: how often it fires in converted C and
   in Snappy, and whether a local whose scope runs in a loop needs a cache
   of its block rather than an allocation per iteration.
10. **C++'s stack cases that heap placement does not settle** on paper:
    a `std::function` holding a lambda's references, `this` escaping a
    constructor, coroutine frames. S0 records each; the C++ RFC decides.

## Future work

- **C++** (a later RFC): `std::span` and `std::string_view` as bounded
  pointers, references as borrowed, the hardened standard library inside
  safe code.
- **All-safe programs**: no load validation where every unit that writes
  a type's annotated fields is safe.
- **Data races**: a `Send`/`Sync`-like rule for pointers shared between
  threads, or atomic counted pairs.
- **More lifetimes**: a parameter borrowed for as long as another
  (`WEAVEC_BORROWS(p)`), if conversions need it.
- **Tooling for agents**: `weavec-cc --weavec-safe-report` listing each
  function's tier and, for a guarded one, the first rule that blocks it;
  fix-its applied by Clang's `-fixit`.

## Implementation amendments

These record where the implementation departs from the design above, from
S0's paper conversions (jsmn, sds, and Snappy for C++) and from building it.
They override the body.

1. **What keeps a guard (§5).** A pointer loaded from memory, and one a call
   returns without a bound, are not validated where they enter: accesses
   through them keep their guards, as in guarded code. A guard is replaced by
   a bounds check only where the pointer derives, through arithmetic, phis
   and selects, from a root whose bounds the function knows: a bounded
   parameter, a local, a global the unit defines (its exact size), an
   allocation (`allocsize`), or a call whose declared result has a bound
   (checked against the shadow when the callee is not safe). This costs a
   pointer-chasing loop about what the load validation would have, needs no
   pointee sizes in IR, keeps guarded code's detection of a use after free
   through loaded pointers, and is sound in mixed programs whatever guarded
   code writes into a field. Counts of fields are therefore not used at run
   time; rule `counted-pair` is not needed and is not implemented.
2. **One entry point (§5).** A safe function validates its bounded and
   escaping parameters at its own entry, whoever calls it, instead of
   having a validating symbol and an internal one. This closes the hole S0
   found for a `static` safe function called by guarded code in the same
   unit (jsmn is header-only); a safe caller pays one range guard per
   bounded argument. Counts are the parameters' values at entry: a function
   that changes its own count parameter (`while (n--) v[n]`) keeps its
   bounds.
3. **Bounds in IR (§4, §8).** The checker hands the passes a table of each
   function's parameter and result bounds by symbol. In a safe function,
   before GuardInsert, promotable locals are promoted and `inbounds` is
   dropped from its address arithmetic, so that an out-of-bounds address is
   never poison the optimizer could use to delete its check. After
   GuardInsert, each replaceable guard becomes `offset < usub.sat(size + 1,
   width)` on offsets from the root: one comparison per access, whose limit
   is loop-invariant (a `for` loop over a counted parameter compiles to one
   compare and branch per iteration). Verify mode keeps the guard as its
   monitor; `-fweavec-safe-checks=full` keeps it as a guard; the ledger
   records the replacement with the reason `safe` under PROVEN. The ledger's
   version is unchanged (a new reason value); no `safe-unrooted` counter
   exists, since an access without rooted bounds simply stays guarded.
4. **Borrowed fields (§5).** S0's C++ check (Snappy) and cJSON's
   `parse_buffer` need a struct that carries a borrowed pointer down the
   call tree. `WEAVEC_BORROWED` after a field type's `*` lets a borrowed
   pointer be stored in that field of a local variable (through `.` only);
   a pointer loaded from such a field is borrowed, and so are the local's
   address and a copy of it, so none of them can be stored, returned or
   passed to an escaping parameter. Storing through a pointer
   (`b->content = v`) stays an error: it would need a lifetime relating the
   two objects.
5. **The rules (§3).** `arith-bounds` is part of `index-single`, which
   allows an offset of constant 0 (`p[0]` is `*p`) and does not fire inside
   a system header's macro (glibc's `ctype` macros index
   `*__ctype_b_loc()`; the access keeps its guard). Subtracting pointers is
   allowed: it touches no memory. `pointer-cast` rejects only conversions to
   a type that holds pointers (a view between types without pointers cannot
   forge one), with the allocation's result as the exception, and function
   pointer conversions. `pointer-bytes` allows a byte view passed directly
   to a function that is not safe (its own accesses are guarded and S-A2
   holds), to a library call that reads it or releases it, to a copy whose
   other side has the same type, and to a zero fill. `string` is replaced by
   scanning a null-terminated parameter at entry (amendment 1), after which
   it is bounded. `variadic` also rejects `va_arg` of a type that holds
   pointers (S0, sds: a `va_list` parameter). `no-asm` allows an empty
   template with no operands, a compiler barrier (S0, Snappy). New
   identifiers: `bad-annotation` (a bound naming no parameter of the right
   type) and `cxx` (§12).
6. **Report kinds (§*Diagnostics*).** `invalid-bounds` is not a kind of its
   own: an entry validation that fails reports what the shadow says
   (`stack-buffer-overflow`, `heap-buffer-overflow`, `heap-use-after-free`,
   `unterminated-string`, `null-dereference`), at the function's line.
   `pointer-out-of-bounds`'s object line reads `… bytes after the <size>-byte
   bounds of the pointer at …`. `stack-escape` is as specified.
7. **Library calls (§9).** A library call in a safe function keeps the
   row-driven guards and wrappers it has in guarded code; its arguments are
   not compared against their pointers' bounds. The table describes what a
   call accesses at object granularity, which is what (S6) now says.
8. **Heap-placed locals (§6).** The passes decide which locals go on the
   heap, from the IR after promotion: an address stored in memory, returned,
   converted to an integer, passed to an escaping parameter, kept through a
   `lifetimebound` result, or used where the local may be out of scope. The
   block is allocated with `calloc` (`aligned_alloc` above 16-byte
   alignment) at each `lifetime.start` and released at each `lifetime.end`
   and return. The remark is `-Rpass=weavec-safe`, Clang's own remark
   mechanism, not `-Rweavec-safe-heap-local`. A safe function's locals that
   stay in its frame and are passed to guarded callees keep their redzones
   (S0, Snappy D6: checked). *Unresolved question* 9's per-loop cache of a
   block is withdrawn: reusing a block before a sweep would break (T).
9. **(S3) and the release record.** RFC 0040 amendment 4 keeps a released
   block's release stack in its first 16 bytes. A write in a safe function
   through a pointer to a released block may overwrite that record; its
   complements then make a later report say "not recorded". The allocator's
   own state is outside the block, so the guarantee is unchanged.
10. **Annotation spellings.** `[[clang::annotate_type]]` is accepted in C11
    and C17 as an extension; `-std=c99 -pedantic -Werror` builds fail on
    it (S0, jsmn). The header keeps the attribute; such a build compiles
    the annotations away by defining `WEAVEC_TYPE_ANNOTATE_` empty first.
11. **Budgets and ABI.** The library budget is 11,500 lines (the
    implementation adds about 2,000), the runtime's 10,000; the runtime ABI
    is 41, and the compiler's frame descriptors take the ABI from the
    build, which a hard-coded 40 had let drift.
12. **What S0 left for later.** Counts through an out-parameter
    (`size_t *len`) and counts given by a macro or an expression (Snappy
    D2, D3; sds), a checked way to find the heap block around a pointer
    (sds's header before its string), and validation in constant time from
    the allocator's metadata instead of the shadow's range (sds) are
    *Future work*. Snappy's C++ findings D8–D12 belong to the C++ RFC.

