# The weavec.h header

`weavec.h` declares `WEAVEC_UNSAFE` and the annotations of
[safe functions](pages/guides/safe-c.md) ([RFC 0041](rfcs/0041-safe-c.md)).
`weavec-cc` puts the header on the system include path
(`#include <weavec.h>`) and defines `__WEAVEC__` to `1` in every unit it
compiles with WeaveC enabled. The header is at version 2.0
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
| `struct S { WEAVEC_UNSAFE int f(…) { … } };` | before a member function (C++) | The member's body is an unsafe region. |

In C++ ([RFC 0039](rfcs/0039-cxx.md) §8) a function in a namespace or a
class takes the annotation as a function does; a template defined inside an
unsafe region is unsafe in every instantiation, and a lambda's body inside
an unsafe block is in the block.

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

## Safe functions

| Form | Placement | Meaning |
| ---- | --------- | ------- |
| `WEAVEC_SAFE` | before a function definition | The function is safe: checked against Safe C's rules when it compiles, its accesses checked against its pointers' bounds. |
| `WEAVEC_GUARDED` | before a function definition | In a unit compiled with `-fweavec-safe`, the function stays guarded. |
| `T *WEAVEC_COUNTED_BY(n) p` | after a pointer type's `*` | `p` points to `n` elements; `n` is an integer parameter or field by name, or a constant. |
| `T *WEAVEC_COUNTED_BY_OR_NULL(n) p` | the same | As above, or null. |
| `void *WEAVEC_SIZED_BY(n) p`, `WEAVEC_SIZED_BY_OR_NULL(n)` | the same | `p` points to `n` bytes (or is null). |
| `T *WEAVEC_ENDED_BY(e) p` | the same | `p` points to the elements before the pointer parameter `e`. |
| `char *WEAVEC_NULL_TERMINATED s` | the same | `s` points to a string, up to and with its NUL. |
| `T *WEAVEC_ESCAPES p` | after a parameter type's `*` | The function may keep `p` after it returns; callers must not pass a stack address. |
| `T *p WEAVEC_LIFETIMEBOUND` | after a parameter's name | The function's result may point into `p` (Clang's `lifetimebound`). |

The `WEAVEC_SAFE` and `WEAVEC_GUARDED` forms expand to `annotate`
attributes as `WEAVEC_UNSAFE` does. The pointer annotations expand to
`[[clang::annotate_type("weavec.…", "n")]]` under Clang (in every C mode;
`-pedantic` warns that `[[…]]` is a C23 extension before C23) and to
nothing under other compilers. Clang's own `counted_by`, `sized_by`,
`counted_by_or_null` and `sized_by_or_null` on struct fields count as the
WeaveC forms.

The ownership annotations of earlier versions (`WEAVEC_OWNED`,
`WEAVEC_BORROWED`, `WEAVEC_ASSUME` and the rest) were deleted with the
ownership analysis that read them
([RFC 0036](rfcs/0036-deterministic-heap-reuse.md)); a program that still
uses one fails to compile until it stops. `WEAVEC_COUNTED_BY` returned
with RFC 0041, with the meaning above. `annotate` payloads that do not
start with `weavec.` are ignored. The [unsafe boundaries
guide](pages/guides/unsafe.md) says when a region is the right tool.
