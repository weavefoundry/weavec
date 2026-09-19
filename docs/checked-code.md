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

Supported private globals retain their identities and conditions in exported
requirements (RFC 0028). A caller can establish such a condition through a
verified initializer or setter. Unsupported private storage can still require
stronger entry assumptions or prevent complete export; a missing premise never
becomes an assumed fact or disappears from an output guarantee.

Write permission is a separate requirement: string literals and `const` objects
remain read-only even through a cast. Mutable local objects and modeled
allocations provide positive writability evidence; helper callers must establish
exported writable intervals.

Ownership alone does not initialize memory. `malloc` storage needs writes
before reads; `calloc` supplies zeroed bytes. Writing one cell does not establish
another cell. At branch joins, initialization is retained only where every
incoming path establishes it. Supported complete copies establish the copied
range. Complete helper contracts can export initialization postconditions.

A verified cleanup wrapper can export `allocation-consumed` for a pointer
parameter. This guarantees cleanup of that entry allocation, including a null
entry that has nothing to release. It does not cover child allocations or
buffer payloads; complete container cleanup needs `container-consumed` and its
ownership requirements. A conditional or omitted free cannot establish an
unconditional cleanup output. Current forwarding supports unconditional
outputs for direct pointer parameters and the `free` allocation family.

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

## Check input cases and union members

RFC 0025 rechecks helpers under bounded facts established by a caller,
including read-only helpers. For example, `read_if(0, 0)` can be checked when
`read_if` returns immediately for a zero selector, even if its other branch
has unresolved arithmetic or memory accesses. Forwarding helpers and resolved
callbacks retain the case premises. Every evaluated selector still needs
initialized storage; a tag value supplies no payload or pointee permission.

Named, complete unions with scalar or pointer members can be checked directly:

```c
union value { int number; int *pointer; };
int main(void) {
    int n = 7;
    union value v = {.pointer = &n};
    return *v.pointer;
}
```

Reading a member requires evidence that this member was initialized. Reading
another member, or changing an enclosing tag without establishing the selected
payload, leaves checking incomplete. A pointer member independently needs live
storage, sufficient bounds and initialized pointee bytes. Compatible complete
record copies preserve member evidence; partial byte writes and unknown writes
invalidate overlapping values. Guarded branch joins retain only evidence
justified on every applicable incoming path.

A helper can require an input member or establish a member through an output
parameter or a returned record. The `union-member` requirement is separate
from initialized byte ranges. Unsupported aggregate/array members, anonymous
member promotion, bit-fields, volatile/atomic union storage and representation
punning remain incomplete. There is no union or tag annotation.

## Read a report

`--checked-report=path` computes contracts and writes JSON without selecting
additional functions. Compiler spelling: `-fweavec-checked-report=path`.

Version 2 contains the invocation result, tool/model versions, totals and
units with source, target and function records. Each function records:

- `selected`, `complete`, `deferred`, and `limited` flags;
- `status`: proven, conditional, trusted, or incomplete;
- sufficient `requirements` and guaranteed `establishes`, including `when`
  input guards and optional `on` returning outcomes;
- `obligations` with property, outcome, source location, reason and call origins;
- `case_inputs`, the optional candidate input paths, and `cases`, each with
  canonical `premises` and its own complete contract and obligation ledger.

Case results do not change the generic function's status or the totals of
selected definitions. Selecting both a successful caller and its incomplete
generic helper still fails the invocation. Compact reports preserve the same
case records when expanded with `scripts/checked-report.py`.

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

## Linked containers

[RFC 0023](rfcs/0023-inductive-container-contracts.md) infers sufficient contracts
for finite, null-ended linked chains. No annotation or special field name is
needed. For example, this helper requires a live chain with initialized nodes:

```c
struct node { unsigned value; struct node *next; };

static unsigned count(const struct node *p) {
    unsigned n = 0;
    while (p) {
        ++n;
        p = p->next;
    }
    return n;
}

int main(void) {
    struct node last = {2, 0}, first = {1, &last};
    return count(&first) != 2;
}
```

Check the closed caller with `weavec --checked-function=main example.c --`.
The caller establishes the chain from its initialized local objects and has no
entry requirements. The current descriptor includes all named record fields,
so unused data fields can also require initialization. Making
`last.next = &first` creates a cycle and fails the finite-chain precondition.
A generic `count` contract remains conditional on its caller's input;
declaring a recursive pointer type does not establish it.

A cleanup loop additionally needs allocation-base ownership of every node:

```c
static void destroy(struct node *p) {
    while (p) {
        struct node *next = p->next;
        free(p);
        p = next;
    }
}
```

Include `<stdlib.h>` for `free`. Saving the successor before release preserves
the live remainder; reading `p->next` after release fails. The borrowed local
chain above cannot be passed to `destroy`. Owned payload cleanup requires each
payload allocation to be distinct from every node and other owned payload.
A shared or borrowed payload does not become owned because of its field name.

Supported operations include runtime prepend construction, allocation-failure
cleanup, traversal/search, reversal, concatenation of disjoint chains, head
detachment through an output pointer, and node/payload destruction. Helpers and
compiler objects transport the same sufficient premises and guaranteed output
facts. Use `--checked-report=report.json` to inspect `container` requirements
and missing chain/separation obligations. Existing `checking-incomplete` and
`checking-failed` identifiers retain their meanings.

Unknown writes, callbacks and unmodeled alias effects invalidate evidence.
Ordinary checks still cover arithmetic, other fields, initialization and leaks.
A derived output describes nodes from its input chains plus fresh additions;
it does not establish that every original node reaches that output. Thus a
wrapper such as `destroy(reverse(p))` needs a separate conservation proof.
RFC 0027 supplies that proof for supported transformations, described below.
General graphs, cyclic ownership, volatile/atomic links and concurrent access
remain outside the model.

Summary format 26 and sidecar format 27 require rebuilding older compiler
objects. Persistent caches validate executable, source, preprocessing and
callee dependencies before reusing a container contract. The RFC 0023
validation report (removed by RFC 0030) recorded the frozen acceptance
population, real-source callers and measured cost.

## Recursive object ownership

[RFC 0027](rfcs/0027-recursive-object-ownership.md) extends inferred container
contracts to finite acyclic trees and child/sibling forests. For example:

```c
#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
static void destroy(struct node *p) {
    if (!p) return;
    destroy(p->left);
    destroy(p->right);
    free(p);
}
```

The generic helper requires a finite initialized structure with independently
owned nodes and a matching release family. It proves complete consumption using
proper-child induction. A closed caller must establish that requirement from
actual allocations and initialized links. Borrowed stack trees support reads;
they cannot satisfy release permission. Runtime construction and cleanup use
inductive invariants, independent of the number of nodes allocated at runtime.

Structural validity and complete cleanup are separate facts. Reports expose
`container-preserved`, `container-consumed`, `container-partition`, and
`container-combined` outputs alongside their structural and separation premises.
A detachment can partition an input between its remaining parent and returned
child. Both allocations still need cleanup or transfer. A reverse wrapper may
preserve every allocation, while a wrapper that discards its head cannot settle
its caller's cleanup obligation. Entry release permission by itself does not
transfer a caller's cleanup duty to a helper that only partially releases input.

Supported initialized integer flag tests can distinguish owned children and
payloads from borrowed references. An inactive ownership edge grants no permission
to access or release its pointee. Changing a flag or link retires old evidence;
it cannot make a lost allocation disappear. Nonowning previous links may cycle,
but owning links must remain acyclic and separate. Unknown callback targets,
unproved mutation and exhausted metadata limits remain incomplete.

The unchanged cJSON validation clients explicitly establish default allocation
hooks before creating, attaching, traversing, detaching and deleting objects.
This covers the selected lifecycle slice, not the parser, printer or arbitrary
custom hooks. Generic direct recursive destruction and supported helper wrappers
are inferred from bodies; no library-name certificate or new annotation is used.
Mutual recursive cleanup, arbitrary shared graphs and general logical ownership
predicates remain unsupported. The domain bounds metadata to 64 footprint
variables and relations; exceeding those bounds loses proof.

Source analysis, compiler objects and validated checkpoints transport the same
contracts. Cross-unit callers need compatible object evidence for recursive
contracts; a forward declaration alone does not supply it. RFC 0028 transports
that evidence from verified constructors and preserves supported private hook
state across separate translation units, as described below.
Summary format 26, sidecar format 27 and checked encoding 12 reject
older metadata; rebuild old objects. Expanded JSON version 2 and compact version
3 retain their existing meanings. The RFC 0027 validation record (removed by
RFC 0030) recorded fixed populations, counterexamples, test results and cost
observations.

## Growable buffers and vectors

[RFC 0026](rfcs/0026-growable-buffer-contracts.md) adds inferred contracts for
ordinary records with a data pointer and unsigned logical-length and capacity
fields. Discovery proposes a shape; callers establish it from real storage and
initialization. Field names and annotations do not grant the predicate.

The basic contract relates `0 <= length <= capacity` to accessible backing
storage and initialized elements in `[0,length)`. Capacity can be smaller than
a separately known physical allocation. Increasing either count never creates
storage or initializes bytes. A terminated byte buffer additionally has an
initialized zero at `data[length]` and room for it.

Reserve and append compose through inferred helpers, including checked
`realloc` and allocate/copy/free patterns. Success-only guarantees activate
after the result is tested. An intervening write retires the dependent
guarantee. Overflow guards must establish the evaluated C allocation size;
wrapped arithmetic cannot be interpreted as a larger mathematical size.
Runtime loops retain only invariants established by their entry and every
back edge. No fixed number of successful appends establishes an arbitrary loop.

Logical truncation and clearing do not release the backing allocation. A steal
retains the returned allocation's actual extent and initialized contents while
leaving the source in the state its code establishes. Saved pointers into old
backing storage remain invalid after successful replacement.

Pointer-element vectors distinguish initialized cells, borrowed pointees and
separately owned pointees. A proved append can transfer a distinct owned
allocation into an owned prefix; insertion failure leaves its caller responsible
for cleanup. Complete element cleanup discharges those obligations before the
array is released. Duplicate insertion, skipped cleanup and byte-level mutation
cannot manufacture a valid ownership transfer. Unsupported quantified element
transfers remain incomplete.

The portable `buffer`, `buffer-preserved` and `buffer-appended` records travel
through the normal source, object and cache workflows. They add no annotation
spelling or pointer ABI. Rebuild older object sidecars for format 27.
Shape discovery is bounded to 16 descriptors and 64 instances; descriptors are
limited to 16 KiB. Exhaustion is reported as incomplete. The initial discovery
rule requires one non-function data pointer and two unsigned, non-Boolean count
fields, with fixed-size scalar or pointer elements. Embedded buffer records are
supported; ambiguous layouts can decline inference. Recursive element records,
nested vector inference and shared-reference protocols are outside this
structural family. Quantified pop, truncate and steal of owned pointer elements
also remain incomplete.

## C runtime contracts

Implicit output through `puts`, `putchar`, `printf` and `vprintf` carries a
named `standard-stream` requirement for stdout through helpers. The C entry
point begins with standard streams available; known closure, replacement or
unknown mutation prevents later discharge. This condition is separate from
format and argument validation.

[RFC 0024](rfcs/0024-checked-runtime-contracts.md) adds checked contracts for
comparison/search operations, scalar math, stream and descriptor I/O, formatted
output and variadic forwarding. Calls must satisfy the operation's memory and
type requirements. Reports identify the C library as a modeled trusted dependency.
Source definitions and resolved callback implementations take precedence over
a familiar library name.

`memcmp`, `strcmp`, bounded comparisons and searches require initialized input.
A search result remains a nullable borrow from the original object. Freeing that
object invalidates a saved search result. Floating operations such as `fabs`
and `floor` add no memory requirements; prediction builtins preserve their
first argument and both possible branch outcomes.

`read` and `fread` initialize the prefix described by their successful returned
count, in bytes and complete elements respectively. Testing success never
initializes the unused tail. `fgets` establishes a terminated initialized prefix
only on its non-null result. Streams must have live matching provenance;
standard streams have explicit library provenance.

```c
char buffer[64];
ssize_t received = read(fd, buffer, sizeof buffer);
if (received > 0) {
    use_byte(buffer[0]);       /* initialized */
    /* buffer[63] still needs proof that received is at least 64 */
}
```

The bounded format parser checks ordinary narrow integer, character, string,
pointer and floating conversions. Consumed arguments need the correct promoted
types, including `int` for `*` width and precision. A precision can bound a `%s`
read without requiring a terminator beyond that bound. Formatting input and
output must satisfy the operation's non-overlap requirement.

For `snprintf`, the result is the length that would have been written. After a
nonnegative result and positive capacity, the destination contains an initialized
terminated prefix. This does not prove the entire capacity initialized. Exact
literal bounds can establish more; unbounded `sprintf` requires a sufficient
output bound. Fortified forms retain their additional object-size checks.

`va_start` creates an active argument list. `va_copy` creates an independent
active cursor. A `vprintf` family traversal consumes its cursor, and every
locally started or copied list needs `va_end` on each returning path. Checked
helpers can forward a format and trailing arguments, or an incoming argument
list, through portable requirements. Callers must discharge those requirements
against their actual format and arguments. A helper that may consume an incoming
list retires the caller's cursor for further traversal.
The initial cursor model covers local variables and incoming parameters;
argument lists embedded in records remain unsupported.

The parser is limited to 4,096 format bytes, 64 conversions and 128 argument
slots. Dynamic formats that cannot be bound through a supported interface,
positional and wide conversions, `%n`, direct `va_arg`, escaping lists and
unrepresented output bounds remain explicit coverage gaps. Directly ending an
incoming caller-owned cursor is outside this milestone; end locally owned lists.
No format annotation or warning suppression supplies missing checked evidence.

For a constant positive `snprintf` capacity, testing the returned count can also
establish the exact written prefix. If `0 <= n && n < sizeof buffer`, `buffer[n]`
is the written terminator. If `n >= sizeof buffer`, truncation initializes the
capacity and its final NUL. A negative or overwritten result establishes neither.

Runtime records use checked encoding 12, summary format 26 and sidecar format 27.
Rebuild objects carrying older sidecars. The cache validates the executable and
source dependencies before reusing these records.

## Opaque objects and private library state

[RFC 0028](rfcs/0028-opaque-objects-and-library-state.md) preserves supported
inferred contracts when a client's public header contains only `struct node;`
or `struct buffer;`. Analyze the implementation and client together, or compile
both with `weavec-cc` and select the client at link time:

```sh
weavec --whole-program --checked-function=main client.c library.c -- -std=c11
weavec-cc -c library.c -o library.o
weavec-cc -c client.c -o client.o
weavec-cc -fweavec-checked-function=main client.o library.o -o client
```

A verified constructor supplies the object's actual allocation and initialized
shape. That evidence follows pointer copies, supported returned borrows and
ownership transfers. Forwarding helpers can export sufficient input predicates;
a closed client must discharge them. A forward declaration, cast or matching
layout alone supplies no ownership or memory permission. Double destruction,
lost detached ownership and use of a saved borrow after destruction remain
failures.

Returned buffers carry their verified initialized-prefix relation and captured
capacity bounds. Successful append can establish initialized elements; a failed
append or an increased capacity does not initialize additional bytes. Accessors
retain the backing allocation's identity, including when called directly in an
indexing expression. Replacing or releasing storage invalidates saved borrows.

Private setters and initializers transport actual scalar and callback state.
Separate modules' same-spelled static variables stay distinct. An established
callback target uses its analyzed implementation or existing modeled libc
contract; unknown or nullable targets cannot acquire a contract from their
prototype. Writes invalidate dependent state. Callers cannot assume an
initializer still describes storage after reachable mutations.

The pinned cJSON public-header clients establish default or concrete custom
hooks and verify supported lifecycle operations with its unchanged implementation
in a separate source unit. For compiler-object builds, request an unselected
contract report while compiling the library, then select the client at link:

```sh
weavec-cc -fweavec-checked-report=library-contracts.json -c cJSON.c -o cJSON.o
weavec-cc -c client.c -o client.o
weavec-cc -fweavec-checked-function=main client.o cJSON.o -o client
```

An unselected compile report collects contracts without claiming that the whole
library checks. The selected link must still discharge every client obligation.

Portable descriptions support ordinary scalar types, pointers, C function
types, structs and fixed arrays, including cyclic pointer layouts. They are
bounded to 128 type nodes, 64 fields per record and 64 KiB per description.
Unions, bitfields, atomic/volatile access, flexible arrays, variable-length
storage and exhausted limits can prevent complete inference. These descriptions
preserve layout; they do not imply that every operation on a supported type is
provable. General shared graphs and asynchronous callback protocols remain
outside this milestone.

The metadata remains internal to analysis. It neither completes the client's
forward declarations nor inserts private names into C lookup. Conflicting
layouts lose evidence. Rebuild older artifacts: summary format 26, sidecar
format 27 and checkpoint format 3 intentionally reject previous artifacts.
Source/header, preprocessing, target and object-content validation still apply;
this does not support source-free checked linking.


## Composing recursive and stateful helpers (RFC 0029)

A mutable output record can have extra counters, flags and nested hook records.
The analyzer uses indexing, loop bounds and pointer arithmetic to nominate its
backing pointer, logical length and capacity. The caller must still establish
live storage, sufficient allocation extent and initialized contents. Changing a
capacity or logical length does not create either storage or initialized bytes.
A helper's transported field roles are checked against the caller's actual
record layout, so a separate-source caller need not repeat its indexing code.
Capacity is a guaranteed accessible range; the allocation may be larger. A
helper can require additional initialized entry bytes while its backing pointer
is unchanged. Its caller must prove those bytes independently of capacity.

A reader loop that advances a field cursor requires the entire interval it may
visit. After the cursor changes, its current value cannot be exported as the
incoming cursor value. A stable upper bound may instead supply a sufficient
extent and initialization requirement. A caller with only the first cell
initialized, or with less storage than the advertised end, fails that contract.
A separate byte pointer and unsigned count can supply the same explicit input
interval when a helper compares a pointer distance against that count. The
caller must establish live initialized storage and a representable distance;
the comparison itself supplies no storage evidence.

A helper that advances a byte writer by `strlen` can require an initialized
terminated prefix between its incoming cursor and capacity. The caller must
establish a real zero inside that bound; unused capacity need not be initialized.
The returned length ends at the first zero, which can precede another known zero.
This relationship lets a cursor update retain its initialized prefix and strict
capacity bound. Replacing the backing pointer or changing the relevant bytes
invalidates the associated termination evidence.

Byte-pointer endpoint helpers can require a live initialized span in one array,
with a distance representable by the target `ptrdiff_t`. Callers must prove
that interval from actual storage; unrelated arrays and uninitialized tails
fail the requirement. An integer-returning helper can also guarantee a
nonnegative count no larger than its incoming span. This guarantee must hold
on every return, including joined branches, and promises no processing or writes.

An unconditional reverse byte-writing loop with a stable bound can establish
the suffix it visits. Index zero requires its own store. Conditional writes,
skipped iterations and early exits do not establish that whole suffix.
A helper that advances an output pointer can preserve initialization up to its
actual final position, including a zero advance on an early return. A bound on
the largest possible advance alone never initializes those bytes. Conditional
integer arguments retain the range of their actual converted values at call
entry; neither arm is replaced by an assumed exact maximum.

For eligible direct or mutually recursive cleanup and read-only traversal
functions, WeaveC checks the complete group before publishing any member's
guarantee. Proper-child calls
supply progress; forwarding the unchanged input is permitted only when every
cycle also contains a proper-child step. Base cases, each owned child and the
head release all remain obligations. The initial group rule covers at most 32
single-parameter functions on one record type, with no hidden global effects or
unverified external helpers. A node's own callback binding cannot stand for its
children's bindings. Shared children and owning cycles do not satisfy the
finite ownership-forest predicate.

An initial recursive construction rule accepts functions taking a constant
byte pointer and an unsigned remaining count, returning a nullable fresh
initialized forest. The caller supplies the entire readable input interval.
Each recursive call stays inside that interval, and every cycle decreases
the immutable entry count. Failure paths must release all partial allocations;
successful paths must transfer the complete fresh forest. Complete inferred
helpers can perform reads and partial-tree cleanup. Private hypotheses cannot
become cached callback or memory specializations. These contracts compose
through separate source files, ordinary objects and validated checkpoints.
An integer-returning constructor may instead publish through a third, pointer
output parameter: positive returns establish a non-null fresh forest and zero
returns must actually leave the slot null. The slot needs writable storage
separated from the input; its previous value supplies no ownership. Immediate
success tests transfer the whole forest, including a child attached by a helper.
The byte pointer and remaining count can also reside in a reader record, with
unrelated fields. Passing it by value preserves the caller's record. Passing a
pointer to it requires initialized writable reader storage separated from the
input bytes; changing its cursor/count invalidates the caller's old field facts.
The recursive hypothesis supplies no post-call reader bounds. Both forms prove
progress against the original input and account for every allocation.

A constructor taking an owned node pointer and unsigned count can extend an
existing initialized head whose owned links and payloads are initially null.
Every recursive cycle must decrease the entry count. The head remains live and
unchanged on every return; newly acquired descendants must be attached exactly
once or released, including failure paths. Its `container-extended` output keeps
the original head separate from the fresh acquisition ledger. Broader mutable
reader and partial-consumption interfaces, owned-tree writers, and the mandatory
cJSON parsing and printing goals remain incomplete.

A helper may also publish an allocated byte payload through a local pointer
or a complete allocating helper. The caller must establish an owned head with
the required empty slots. Successful and failed returns retain its ownership;
each acquired payload must be attached exactly once or released. Interior,
released, duplicated, borrowed and lost allocations do not satisfy this proof.
Payload fields discovered from imported cleanup contracts nominate the shape
only; actual storage, initialization and ownership still need proof.

The modeled `strtod`, `strtof` and `strtold` boundary requires a terminated,
initialized input. A non-null end-pointer slot must be writable and separate
from that input. The resulting pointer retains the input object's bounds and
lifetime; a null slot is permitted. The model makes no claim that the numeric
result is finite or safe to convert to an integer.

A complete callee that stores a copied pointer into a confined automatic slot
can preserve the source allocation's ownership. This includes an end pointer
stored by `strtod`. The slot address must remain confined to that represented
call. The copy preserves aliases: freeing the base invalidates the end pointer,
an interior pointer cannot be released, and losing every local holder still
leaks the allocation.

Generic synchronous allocation and release helpers can now export explicit
`callback-allocate` and `callback-release` requirements:

```c
void *make(void *(*allocate)(size_t), size_t bytes) {
  if (!bytes) return NULL;
  unsigned char *p = allocate(bytes);
  if (p) p[bytes - 1] = 7;
  return p;
}
```

The helper's contract requires a non-null callback returning either null or a
fresh `free`-compatible allocation of the requested extent. A closed caller
passing `malloc` discharges that requirement through the modeled C-library
boundary. A callback allocating fewer bytes, an unknown external target, a
nullable binding or an incompatible cast does not discharge it. Complete
inferred wrappers are accepted only when their checked interface and effects
establish the same behavior. This adds no annotation or runtime dispatch.

`free(p); p = NULL;` preserves the earlier release of the entry allocation.
Assigning a new allocation to `p` cannot discharge the old allocation's cleanup
obligation. Zero-initialized byte ranges outside a proved store are preserved;
unknown overlapping writes discard that evidence.

Scans can also use up to 64 exact initialized bytes from an ordinary character
array initializer, including an immutable static local array. Known contents
can exclude a branch only after the read's storage, bounds and initialization
are proved. Mutation retires overlapping contents; unchanged slices and proved
separate storage can retain them. Actual byte-pointer call cases transport the
captured contents within the existing context budget. This can weaken a
sufficient generic precondition only after rechecking the callee for that input;
it does not certify arbitrary strings or bypass the generic body. An actual
const-qualified array object can retain its contents across a represented write
to another object. A const-qualified pointer alone supplies no such premise,
and writes to the constant array still require ordinary write-permission checks.

These records require summary format 26, sidecar format 27 and checked encoding
12. Older object sidecars and checkpoints, including interim RFC 0029 development
artifacts, must be rebuilt.

Floating-to-integer conversions can be proved locally for finite constants or
unchanged scalar inputs under true finite bounds. The bounds are checked using
the target floating format and integer width, including strict endpoints around
64-bit limits. For example, `x >= INT_MIN && x <= INT_MAX` excludes NaN on its
true branch when those constants are represented exactly. Testing and rejecting
`x < INT_MIN || x > INT_MAX` leaves NaN on the remaining branch and supplies no
such proof. Taken addresses, changes to the guarded value, bypassing jumps and
nonstandard floating modes prevent this inference.

Run the frozen primary, independent serializer, reader, transport, object and cache
populations with `scripts/checked-workflows.py`. The separate `upstream`
population retains the broader cJSON parse/print acceptance goals. Those goals
are not implied by passing the smaller workflows; the RFC 0029 validation
record (removed by RFC 0030) listed outstanding coverage. These changes do not
establish arbitrary recursive construction, general floating-point conversion
safety or a complete cJSON library certificate.

RFC 0029 also supports a recursive byte writer taking immutable byte input, an
unsigned remaining count and a pointer to a discovered buffer record. A checked
writer requires the full initialized input interval, writable output storage
and separation of input, header and backing. Its verified output describes the
actual initialized prefix on every return, including partial failure. A success
return alone does not imply that all input was copied or that the output is
zero terminated. Clients must use the established current length when reading
that prefix. This does not yet cover the required recursive tree printer.

Reader records with a const byte pointer, initialized input length and changing
cursor can carry a reusable reader predicate. The cursor remains between zero
and the readable input length, including on a partial failure return. The
entire input interval must be initialized; the predicate supplies no permission
to write it. Copied records and separate-source helpers retain that distinction.
A reader output alone specifies no exact consumed count or parsing result.

An independently checked input case can avoid a generic recursive limit by
proving the recursive branch unreachable. Exact values captured through live
whole scalar objects may help establish that branch; invalidated storage,
partial objects and incompatible views supply no value. The generic function
still reports its own exhaustion when selected independently.

Exact record-array cells use the same identity for `a->field`, `a[0].field`
and `(*a).field`, including contracts forwarded to a helper. Complete current
zero-byte intervals can establish zero-valued ordinary integer fields in
automatic records and exact array cells. Partial byte coverage and later writes
cannot supply that fact. A stored string terminator can survive an update to a
separate header only when the separation is proved or exported as an explicit
caller requirement using unchanged entry identities.

Small fixed-size automatic integer arrays retain values for exact elements.
Writes through possibly overlapping indices discard those values. Advancing a
pointer retains its allocation identity but retires facts about its previous
pointee. Borrow diagnostics can therefore identify an exact element such as
`a[1]` where the earlier diagnostic used the whole-array spelling `a[*]`.

A local numeric byte prefix can exclude NaN from a modeled `strtod`, `strtof`
or `strtold` result. The checker must establish actual numeric characters
through stores, a validated scan, or a byte-preserving copy. Plain initialized
storage is insufficient. Infinity remains possible, so converting the result
to an integer still requires unchanged, target-representable bounds. A null
end-pointer argument does not export ownership of the input allocation.
