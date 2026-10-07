---
title: Make unsafe boundaries explicit
description: Use narrow WEAVEC_UNSAFE regions for code that must run unguarded or handle raw pointers, and understand what the analysis still checks there.
---

`weavec-cc` guards every memory access of the code it compiles. A few correct programs touch memory a guard rejects on purpose, and some operations depend on knowledge the analysis cannot establish: a hardware address, an allocator's own bookkeeping, a representation crossing. `WEAVEC_UNSAFE` marks that code at a small, reviewable boundary.

## What a region changes in the build

`WEAVEC_UNSAFE` is the only annotation that changes the generated code. In a `weavec-cc` build, the accesses whose source location lies in the function or block are not guarded, and its array indexes are not checked. Code inlined into the region keeps its own location: a guarded helper inlined there stays guarded. The runtime's allocator still validates every `free` made in a region.

A function its author keeps from AddressSanitizer is unguarded the same way, with no source change: `__attribute__((no_sanitize("address")))`, `no_sanitize_address` and `disable_sanitizer_instrumentation` each make its body an unguarded region. The analysis treats such a function as ordinary code; only `WEAVEC_UNSAFE` also changes what the analysis trusts.

## When to use one

Use a region for code that is correct but reads or writes memory a guard rejects:

- a deliberate over-read, such as a word-at-a-time string scan or hash that reads past the end of an allocation within the same aligned word;
- an allocator's internals, which use the slack after a block or the headers between blocks;
- a conservative garbage collector's scan of the stack or of memory it does not own.

Under AddressSanitizer these need `no_sanitize("address")` for the same reason; code that already has it needs nothing more. A region is not a way to silence a report you have not understood: when the code was not written to read past an object, a failing guard is usually a real bug.

```c
#include <stdint.h>
#include <weavec.h>

WEAVEC_UNSAFE void write_register(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}
```

This function's write is not guarded. Its caller must ensure that the address refers to the intended mapped register and that the write is valid; the annotation does not establish those facts.

## Raw pointers

Pointers annotated `WEAVEC_RAW`, and values loaded through, derived from or handed out as raw pointers, have no safe ownership guarantee. Copying or comparing raw pointers is allowed. Dereferencing, releasing or transferring them into an owning contract outside an unsafe region is an `unsafe-operation` error in `weavec`. A value is raw only when it is raw on every path; one that is raw on some paths only is treated like a pointer of unknown origin.

A pointer converted from an integer (`(struct node *)h`, `(void *)(uintptr_t)n`) is not raw: it is a pointer of unknown provenance. The analysis leaves its accesses not proven and never reports them; a `weavec-cc` build guards them like any other access outside a region.

## What the analysis still checks in a region

Inside a `WEAVEC_UNSAFE` function or block, `weavec` and `weavec-cc -fweavec-diagnose`:

- permit raw operations, and trust them;
- trust spatial and null operations: a possible overrun or null dereference there is never reported;
- track temporal state exactly as outside the region: a use after a definite free is still reported, and a possible one is a warning;
- still report a definite spatial or null violation, such as a constant index past the end of an array.

Nothing inside a region is suppressed, and what the region does to the surrounding code is visible: freeing an object inside it invalidates pointers used afterwards.

## Keep the boundary narrow

Every access in a region runs without a guard, so a bug there is not caught at run time. Keep each region to the operation that needs it, prefer an interface that states the actual ownership and lifetime expectations, and document why each operation is valid and who maintains that assumption when the code changes. The trusted sites are counted in the summary line of `weavec`, and the enforcement ledger of a build (`-fweavec-ledger=`) lists each unguarded access with the reason `unsafe`. See [safety guarantees](/reference/guarantees/).

## Assumptions are not checked at run time

`WEAVEC_ASSUME(expr)` tells the analysis a fact it cannot derive. When the analysis refutes `expr`, that is a `contradicted-assumption`; otherwise it assumes `expr` from there on. Nothing checks an assumption at run time, and no guard depends on one, in a region or outside it: an assumption affects only what the analysis reports.

See [annotation placement](/reference/annotation-placement/) and [RFC 0004](/rfcs/0004-unsafe-boundaries/), as amended by [RFC 0030](/rfcs/0030-prove-or-trap/) and [RFC 0035](/rfcs/0035-guard-by-default/), for the precise contract.
