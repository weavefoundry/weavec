---
title: Make unsafe boundaries explicit
description: Use narrow WEAVEC_UNSAFE regions for correct code that must run unguarded, and keep track of what runs without a guard.
---

`weavec-cc` guards every memory access of the code it compiles. A few correct programs touch memory a guard rejects on purpose: a word-at-a-time scan, an allocator's bookkeeping, a hardware register. `WEAVEC_UNSAFE` marks that code at a small, reviewable boundary.

## What a region changes in the build

`WEAVEC_UNSAFE` is WeaveC's only annotation. It comes from `weavec.h`, which `weavec-cc` puts on the include path, and it marks a function (placed before its definition) or a block (placed before a compound statement). In a `weavec-cc` build, the accesses whose source location lies in the function or block are not guarded, and its array indexes are not checked. Code inlined into the region keeps its own location: a guarded helper inlined there stays guarded. The runtime's allocator still validates every `free` made in a region. See [the weavec.h header](/reference/annotations/) for both forms.

A function its author keeps from AddressSanitizer is unguarded the same way, with no source change: `__attribute__((no_sanitize("address")))`, `no_sanitize_address` and `disable_sanitizer_instrumentation` each make its body an unguarded region. Code that keeps a scan from ASan with `#if` on `__SANITIZE_ADDRESS__` or `__has_feature(address_sanitizer)` takes its ASan path under `weavec-cc` too, and memory the program poisons through ASan's interface (`ASAN_POISON_MEMORY_REGION`) traps when guarded code touches it.

## When to use one

Use a region for code that is correct but reads or writes memory a guard rejects:

- a deliberate over-read, such as a word-at-a-time string scan or hash that reads past the end of an allocation within the same aligned word;
- an allocator's internals, which use the slack after a block or the headers between blocks;
- a conservative garbage collector's scan of the stack or of memory it does not own;
- an access to a fixed hardware address.

Under AddressSanitizer these need `no_sanitize("address")` for the same reason; code that already has it needs nothing more. A region is the last resort, not a way to silence a report you have not understood: when the code was not written to read past an object, a failing guard is usually a real bug.

```c
#include <stdint.h>
#include <weavec.h>

WEAVEC_UNSAFE void write_register(uintptr_t address, uint32_t value) {
    *(volatile uint32_t *)address = value;
}
```

This function's write is not guarded. Its caller must ensure that the address refers to the intended mapped register and that the write is valid; the annotation does not establish those facts.

## Keep the boundary narrow

Every access in a region runs without a guard, so a bug there is not caught at run time, and a pointer the region corrupts can make a later guarded access fail far from the cause. Keep each region to the operation that needs it, prefer a block around the one statement to a whole function, and document why each operation is valid and who maintains that assumption when the code changes.

The accesses a region leaves unguarded are counted. `weavec-cc -fweavec-summary` prints a line per file whose last count is the unguarded accesses, and the enforcement ledger of a build (`-fweavec-ledger=`) lists each one with the outcome `unguarded` and the reason `unsafe`:

```text
weavec: scan.c: 214 accesses: 96 proven, 117 guarded, 1 unguarded
```

Review a change to that count as you would any other unchecked code. See [safety guarantees](/reference/guarantees/) for what a region removes from the guarantee, and [RFC 0035](/rfcs/0035-guard-by-default/) §2.6 for the precise contract.
