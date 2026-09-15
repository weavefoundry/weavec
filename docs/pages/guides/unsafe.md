---
title: Make unsafe boundaries explicit
description: Isolate operations outside WeaveC's supported ownership model with narrow, documented unsafe regions.
---

Some C operations depend on knowledge that the analyzer cannot establish: a hardware address, an external allocator contract, or a representation crossing. Make that dependency visible at a small, reviewable boundary.

## Identify raw operations

Pointers cast from integers, pointers annotated `WEAVEC_RAW`, and values propagated from raw storage have no safe ownership guarantee. Under strict external checking, unresolved calls also become raw operations.

Copying or comparing raw pointers is allowed. Dereferencing, releasing, or transferring them into an owning contract requires an unsafe region.

```c
#include <stdint.h>
#include <weavec.h>

WEAVEC_UNSAFE void write_register(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}
```

This declaration marks a trusted operation. Its caller must ensure that the address refers to the intended mapped register and that the write is valid. The annotation does not establish those facts.

## Keep the boundary narrow

A `WEAVEC_UNSAFE` block permits raw operations and suppresses ordinary reporting inside the region. WeaveC still follows its effects: freeing an object inside an unsafe block invalidates pointers used afterward.

Prefer an interface that states the actual ownership and lifetime expectations. Document why each trusted operation is valid and who maintains that assumption when the code changes.

## Review the checked report

Checked mode records trusted boundaries and assumptions. A result that depends on trusted code is conditional on that code satisfying its stated obligations. `WEAVEC_ASSUME` is also a trust assertion; a false assumption can hide a real error.

See [safety guarantees](/reference/guarantees/), [annotation placement](/reference/annotation-placement/), and [RFC 0004](/rfcs/0004-unsafe-boundaries/) for the precise contract.
