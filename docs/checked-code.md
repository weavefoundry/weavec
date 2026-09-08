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

Write permission is a separate requirement: string literals and `const` objects
remain read-only even through a cast. Mutable local objects and modeled
allocations provide positive writability evidence; helper callers must establish
exported writable intervals.

Ownership alone does not initialize memory. `malloc` storage needs writes
before reads; `calloc` supplies zeroed bytes. Writing one cell does not establish
another cell. At branch joins, initialization is retained only where every
incoming path establishes it. Supported complete copies establish the copied
range. Complete helper contracts can export initialization postconditions.

## Read a report

`--checked-report=path` computes contracts and writes JSON without selecting
additional functions. Compiler spelling: `-fweavec-checked-report=path`.

Version 1 contains the invocation result, tool/model versions, totals and
units with source, target and function records. Each function records:

- `selected`, `complete`, `deferred`, and `limited` flags;
- `status`: proven, conditional, trusted, or incomplete;
- sufficient `requirements` and initialization `establishes`;
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

## Current limits

The model is bounded, single-threaded C. Unsupported unions, assembly,
nonlocal control flow, uncertain identity, type reinterpretation, unavailable
call effects and exhausted limits fail selected checking. General recursive
heap invariants and arbitrary loop induction are not implemented. Complex
pointer-containing paths, callbacks and dynamic expressions require complete
facts from their existing models; otherwise the result stays unresolved.
Pointer differences, ordered pointer comparisons and floating-to-integer
conversions currently require an unsafe boundary. Conservative rejection of valid C is expected.

The initial library proof models cover allocation/release and bounded memory
operations. An entry in the broader ordinary ownership table does not by
itself establish a complete safety contract. Keep unmodeled dependencies
outside selected scope or mark an intentional trust boundary explicitly.

`checking-incomplete` identifies missing proof; `checking-failed` identifies a
violation. Lowering their diagnostic severity does not discharge the underlying
obligation. The compiler still fails selected checking.

The authoritative design and acceptance criteria are in
[RFC 0018](rfcs/0018-checked-code-and-safety-contracts.md).
