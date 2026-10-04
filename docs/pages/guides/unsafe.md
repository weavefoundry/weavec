---
title: Make unsafe boundaries explicit
description: Isolate raw pointer operations in narrow WEAVEC_UNSAFE regions, and understand what a region trusts and what it still checks.
---

Some C operations depend on knowledge that the analyzer cannot establish: a hardware address, an external allocator contract, or a representation crossing. Make that dependency visible at a small, reviewable boundary.

## Identify raw operations

Pointers annotated `WEAVEC_RAW`, and values loaded through, derived from or handed out as raw pointers, have no safe ownership guarantee. Copying or comparing raw pointers is allowed. Dereferencing, releasing or transferring them into an owning contract outside an unsafe region is an `unsafe-operation` error. A value is raw only when it is raw on every path; one that is raw on some paths only is treated like a pointer of unknown origin.

A pointer converted from an integer (`(struct node *)h`, `(void *)(uintptr_t)n`) is not raw: it is a pointer of unknown provenance. Its accesses are `unresolved(raw-cast)`, and so guarded in a `weavec-cc` build with the runtime and checked for null; they are never an error. Inside a `WEAVEC_UNSAFE` region they are `trusted(unsafe)` like any other spatial or null facet there.

```c
#include <stdint.h>
#include <weavec.h>

WEAVEC_UNSAFE void write_register(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}
```

This declaration marks a trusted operation. Its caller must ensure that the address refers to the intended mapped register and that the write is valid. The annotation does not establish those facts.

## What a region trusts

Inside a `WEAVEC_UNSAFE` function or block:

- raw operations are permitted, and their facets are recorded as `trusted(unsafe)`;
- spatial and null facets are `trusted(unsafe)`, and neither runtime checks nor spatial guards are inserted for them;
- temporal state is tracked exactly as outside the region: a use after a definite free is still an error, and a possible one is a warning or, in a `weavec-cc` build with the runtime, a guarded facet like any other;
- a definite spatial or null violation is still an error;
- `WEAVEC_ASSUME` keeps its runtime assertion: the region trusts raw memory operations, not assumptions.

Nothing inside a region is suppressed. What the region does to the surrounding code is also visible: freeing an object inside it invalidates pointers used afterward.

## Keep the boundary narrow

Every trusted facet is listed in the ledger with its reason, so the regions are easy to review and count. Prefer an interface that states the actual ownership and lifetime expectations, and keep each region to the operation that needs it. Document why each trusted operation is valid and who maintains that assumption when the code changes.

Every level of `-fweavec-require` (`guarded`, `checked`, `proven`) allows trusted facets: trust is explicit and listed, not hidden. A result that depends on trusted code is conditional on that code meeting its obligations; see [safety guarantees](/reference/guarantees/).

## Assumptions are checked

`WEAVEC_ASSUME(expr)` tells the analysis a fact it cannot derive. It is not trusted: when the analysis proves `expr`, nothing changes; when it refutes it, the assumption is a `contradicted-assumption` error; otherwise `weavec-cc` replaces it with a runtime assertion that traps when `expr` is false. The analysis assumes `expr` afterwards in every case.

A region is also where code goes that is correct but trips a guard: reading a word at a time past the end of a string's allocation, or using the slack an allocator leaves after a block. The region removes the spatial guard, not the allocator: a `free` inside it is still validated by the runtime.

See [annotation placement](/reference/annotation-placement/) and [RFC 0004](/rfcs/0004-unsafe-boundaries/), as amended by [RFC 0030](/rfcs/0030-prove-or-trap/), [RFC 0032](/rfcs/0032-runtime-enforcement/) and [RFC 0033](/rfcs/0033-drop-in-by-default/), for the precise contract.
