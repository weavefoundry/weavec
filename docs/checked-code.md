# Checked code

Checked code is an opt-in safety check for selected C functions. A successful
check means that the supported operations are established under the function's
reported entry requirements and trusted boundaries. It does not certify
unselected code, external libraries, the compiler implementation, or arbitrary C.
It adds no runtime checks and changes no pointer ABI.

## Select functions

```sh
weavec --checked-function=main main.c --
weavec --whole-program --checked-function=main main.c helpers.c --
weavec --checked --checked-report=safety.json module.c --
```

Repeat `--checked-function` to name several definitions. A missing name fails
the invocation. `--checked` selects definitions in input files;
`--analyze-headers` extends module selection to header definitions. A
specifically named or annotated header function is reported even without that
flag. Unannotated
helper bodies contribute contracts whenever checking is enabled.

Selection can also live in source:

```c
#include <weavec.h>

WEAVEC_CHECKED int get(unsigned index) {
  int values[4] = {10, 20, 30, 40};
  if (index >= 4) return 0;
  return values[index];
}
```

`WEAVEC_CHECKED` requests a body check. Adding it to an unavailable function's
declaration does not prove that function or grant its caller a trusted contract.

## Build through the compiler

```sh
weavec-cc -fweavec-checked -c helpers.c -o helpers.o
weavec-cc -fweavec-checked-function=main -c main.c -o main.o
weavec-cc -fweavec-checked-report=safety.json main.o helpers.o -o app
```

The compile step may defer an external helper until link time. Such a
provisional result is explicitly marked `deferred` and is not complete. The
normal link analysis resolves the helper and checks its contract at the call.
Sidecars preserve selection when it is not repeated on the link command.

Object contents, source/header contents, and the recorded compiler command
are bound to sidecar records. Rebuild an object if these inputs change or
become unavailable. Reanalyzing changed source alone cannot prove the contents
of an older object. Metadata from an older WeaveC format must also be rebuilt.
Archives do not yet transport member sidecars automatically; an unavailable
member cannot establish the checked contract of a dependency.

## Understand contracts

```c
static void touch(char *p, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    if (p[i] == 42) break;
    p[i] = 1;
  }
}
```

This helper may access any byte in `[0,n)`, even though it can exit early. Its
sufficient contract requires live memory with that accessible, initialized and
writable interval. A caller providing four initialized bytes can satisfy `touch(p,4)`;
it cannot establish `touch(p,8)` from those facts. The latter is an unresolved
precondition, without claiming that execution necessarily reaches byte eight.

Exported requirements can be stronger when a helper's condition depends on a
private global. For example, a private callback flag guarding `free(p)` may
produce an unconditional portable release requirement for `p`. Callers must
still establish that requirement; this does not turn a private condition into
an assumed fact or remove a condition from an output guarantee.

Write permission is a separate requirement: string literals and `const` objects
remain read-only even through a cast. Mutable local objects and modeled
allocations provide positive writability evidence; helper callers must establish
exported writable intervals.

Ownership alone does not initialize memory. `malloc` storage needs writes
before reads; `calloc` supplies zeroed bytes. Writing one cell does not establish
another cell. At branch joins, initialization is retained only where every
incoming path establishes it. Supported complete copies establish the copied
range. Complete helper contracts can export initialization postconditions.

For example, a complete fill establishes the bytes that a later read needs:

```c
static void fill(char *bytes, unsigned size) {
  for (unsigned i = 0; i < size; ++i) bytes[i] = 1;
}
int main(void) {
  char bytes[8];
  fill(bytes, 8);
  return bytes[7];
}
```

Skipping a store or exiting the loop early cannot establish that full interval.
The same output facts can cross nested buffer fields, allocation-returning
constructors and successful output parameters. A status test selects only
facts guaranteed for that outcome; overwriting an output or its length retires
the earlier facts. A saved alias of a released object remains invalid after
a helper installs a replacement.

`memcpy` checks disjoint intervals, including slices of the same object.
`memmove` uses the source's initialization before the move. Positive-size
`realloc` preserves initialized bytes within the old and new extents on
success; its grown tail needs writes. Failure retains the incoming storage.
String models check accessible initialized data through a terminator.
A known zero byte can witness termination without inventing an exact length.

## Check traversals and cursor helpers

[RFC 0021](rfcs/0021-practical-c-traversal.md) extends checked contracts to
counted `while` and `do` loops, same-array cursors, guarded variable advances,
and supported early exits. For example:

```c
static char *fill(char *p, unsigned n) {
  char *end = p + n;
  while (p < end) *p++ = 1;
  return p;
}
int main(void) {
  char bytes[8];
  char *end = fill(bytes, 8);
  return end[-1];
}
```

The returned cursor is one past the initialized array. Reading `end[0]`
still fails. Subtracting or ordering pointers requires live positions in the
same array, compatible element types and a representable target result.
Distinct array members of one structure do not become one array.

The checker preserves supported relationships such as `output <= input`
through in-place compaction and complete pointer-to-pointer helpers. It
checks every entry and back edge before retaining a loop fact. `continue`,
skipped writes and early returns cannot establish an unwritten interval.
Direct local `goto` follows the actual CFG and retains initialization and
lifetime checks, including when it jumps into a loop or past a declaration.

Terminated scans require an initialized prefix through a known zero byte.
That witness need not be the first zero. Reading a nonzero byte before the
witness can justify advancing the cursor; lookahead has its own bounds.
A potentially overlapping write must preserve the witness, establish a new
zero, or lose the termination evidence. Separating pointer holders does not
separate the arrays they reference.

## Read a report

`--checked-report=path` computes contracts and writes JSON without selecting
additional functions. Compiler spelling: `-fweavec-checked-report=path`.

Version 2 contains the invocation result, tool/model versions, totals and
units with source, target and function records. Each function records:

- `selected`, `complete`, `deferred`, and `limited` flags;
- `status`: proven, conditional, trusted, or incomplete;
- sufficient `requirements` and guaranteed `establishes`, including `when`
  input guards and optional `on` returning outcomes;
- `obligations` with property, outcome, source location, reason and call origins.

Obligation outcomes distinguish proven facts, entry requirements, explicit
trust, unresolved coverage and violations. A complete conditional helper still
requires its caller to establish the exported preconditions. Trust in
`WEAVEC_UNSAFE`, `WEAVEC_ASSUME`, declared annotation assumptions and modeled
library contracts propagates to callers and remains visible.
A trust marker alone does not supply missing storage facts; modeled effects
and complete postconditions still govern subsequent checks.
The `trusted` status can also carry entry requirements; callers still need to
establish them. `invocation_ok: false` means the command did not establish its
requested result, even if an independent function has a complete contract.

Reports are deterministic for identical inputs and are replaced atomically.
They are also written on checked failures. A report output error fails the
invocation instead of leaving a successful status without the requested result.

Memory contracts can name dereferenced parameters, record fields, selected
elements, globals and result paths. Requirements distinguish validity, extent,
initialization, writability, release family, separation, string termination and
nonoverflowing byte sums. `initialized` and `zeroed` postconditions establish
byte intervals; `copied` preserves only the input bytes already initialized
within an interval. It does not claim that unused input capacity was initialized
or that the bytes retain a particular value.

Traversal contracts additionally use `position` for an output pointer's
inclusive byte-displacement interval from a captured entry pointer. `progress`
relates two cursors' advances from their respective entry values; it preserves
a caller's proved order without equating unrelated arrays. An endpoint such
as `terminator param 0 scale 1 plus 0` names the selected entry witness's
byte index. It is not a C integer argument or an exact `strlen` result.
Entry `terminated` requirements use `begin` for the minimum witness index;
their `end` is zero. A `terminated` output uses `begin` for the initialized
prefix start and `end` for the known zero byte. Input values are captured
before a call overwrites their holders, and conditional outputs apply only
on their recorded outcomes.

These facts use summary format 17, sidecar format 18 and checked-record
encoding 4. Expanded JSON remains version 2; compact JSON remains version 3.

## Check opaque pointers and callback interfaces

RFC 0022 carries checked facts through ordinary synchronous C interfaces:

```c
static void *identity(void *p) { return p; }
static void fill(char *p) { *p = 7; }
static void invoke(void (*fn)(char *), char *p) { fn(p); }
int main(void) {
    int value = 7;
    int *restored = identity(&value);
    char byte;
    invoke(fill, &byte);
    return *restored == byte;
}
```

Check the closed caller with `weavec --checked-function=main file.c --`.
The original `int` storage justifies restoring `int *`. The instantiated
`fill` contract justifies reading `byte`. Replacing `fill` with a callback
that skips the store leaves the read incomplete. Restoring `float *` from
that same `int` object fails even on a target where their sizes match.

An opaque-input helper may export an `object-type` requirement. Its descriptor
records target byte size, alignment and canonical type or record layout. This
requirement supplies no validity, initialization, capacity or release permission.
Allocated storage eligible for a supported view and original declared objects
can discharge it; arbitrary type punning and enclosing-record recovery remain
unsupported. Const storage retains its existing write restrictions.

Known callback values retain their actual contracts through forwarding helpers,
hook records and setters. Direct library operations also retain their checked
interpretation through established function pointers. A custom allocator's
available body determines its capacity and release behavior. Selecting a safe
callback alongside an unsafe, null or unknown alternative cannot make the call
complete. Context bounds and unknown alternatives remain visible in reports.
An open generic callback helper may be incomplete while its specialization
under the caller's known target is complete.

A helper that writes `*out = malloc(n)` and initializes the allocation on
success can export an `initialized` output with `if_nonnull: true`. It refers
to the final pointer stored in `*out`, independently of the pointer passed on
entry. Callers must still test the output before reading it. Replacing that
pointer or losing the required input premises retires the dependent evidence.

Private file-scope function-pointer cells cross translation units under
source-qualified identities. This lets a setter in one module establish
which allocator a later call uses. These internal identities do not add a
user annotation or a runtime ABI, and do not make arbitrary private storage
available to foreign source code. Rebuild objects with older sidecars.

## Current limits

The model is bounded, single-threaded C. Unsupported unions, assembly,
nonlocal control flow, uncertain identity, type reinterpretation, unavailable
call effects and exhausted limits fail selected checking. General recursive
heap invariants and arbitrary loop induction are not implemented. Complex
pointer-containing paths, callbacks and dynamic expressions require complete
facts from their existing models; otherwise the result stays unresolved.
Unproved pointer relationships and floating-to-integer conversions remain
incomplete. Traversal reasoning is bounded to 64 active variables, 32 loop
iterations/candidates and 4,096 relation steps per operation. Exhaustion is
reported and supplies no proof. Conservative rejection of valid C is expected.

Library proof models cover allocation/release, bounded memory operations,
positive-size `realloc`, `strlen`, `strnlen`, `strcpy`, `stpcpy`, `strcat`,
`strdup`, `strndup` and `strncpy`, including supported compiler/fortified
spellings. Unknown string lengths or unproved arithmetic can still prevent
checking a modeled call. An entry in the broader ordinary ownership table does not by
itself establish a complete safety contract. Keep unmodeled dependencies
outside selected scope or mark an intentional trust boundary explicitly.

`checking-incomplete` identifies missing proof; `checking-failed` identifies a
violation. Lowering their diagnostic severity does not discharge the underlying
obligation. The compiler still fails selected checking.

The authoritative design and acceptance criteria are in
[RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md) and
[RFC 0019](rfcs/0019-practical-checked-memory-contracts.md), extended by
[RFC 0021](rfcs/0021-practical-c-traversal.md) and
[RFC 0022](rfcs/0022-checked-c-interfaces.md).
