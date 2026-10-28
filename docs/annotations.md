# The weavec.h header

`weavec.h` declares the one annotation WeaveC has: `WEAVEC_UNSAFE`.
`weavec-cc` puts the header on the system include path
(`#include <weavec.h>`) and defines `__WEAVEC__` to `1` in every unit it
compiles with WeaveC enabled. The header is at version 1.0
(`WEAVEC_H_VERSION_MAJOR`, `WEAVEC_H_VERSION_MINOR`).

`WEAVEC_UNSAFE` expands to `__attribute__((annotate("weavec.unsafe")))`
under compilers that understand Clang's `annotate` attribute and to nothing
under the others, so code that uses it stays buildable with any C
toolchain. Copy the header into a project that must also build without
WeaveC installed.

| Form | Placement | Meaning |
| ---- | --------- | ------- |
| `WEAVEC_UNSAFE void f(…) { … }` | before a function definition | The function's body is an *unsafe region*. |
| `WEAVEC_UNSAFE { … }` | before a compound statement | The block is an unsafe region. |

In an unsafe region `weavec-cc` does not guard memory accesses and does not
check array indexes ([RFC 0035](rfcs/0035-guard-by-default.md) §2.6,
amendment 8). A region is decided by source location: code inlined into it
from elsewhere keeps its guards, and the runtime's allocator still validates
every `free` made in it. The enforcement ledger records each access in a
region as `unguarded` with the reason `unsafe`.

```c
#include <stdint.h>
#include <weavec.h>

WEAVEC_UNSAFE void write_register(uintptr_t address, uint32_t value) {
  *(volatile uint32_t *)address = value;
}

unsigned long first_word(const char *s) {
  unsigned long word;
  WEAVEC_UNSAFE {
    word = *(const unsigned long *)s; /* may read past the string on purpose */
  }
  return word;
}
```

A function kept from AddressSanitizer (`__attribute__((no_sanitize("address")))`,
`no_sanitize_address` or `disable_sanitizer_instrumentation`) is an unsafe
region too, with no source change.

Every other annotation of earlier versions (`WEAVEC_OWNED`,
`WEAVEC_BORROWED`, `WEAVEC_COUNTED_BY`, `WEAVEC_ASSUME` and the rest) was
deleted with the ownership analysis that read it
([RFC 0036](rfcs/0036-deterministic-heap-reuse.md)); a program that still
uses one fails to compile until it stops. `annotate` payloads that do not
start with `weavec.` are ignored. The [unsafe boundaries
guide](pages/guides/unsafe.md) says when a region is the right tool.
