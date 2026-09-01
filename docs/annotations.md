# Annotations reference

WeaveC annotations live in the `weavec.h` header, which `weavec` puts on the
system include path automatically (`#include <weavec.h>`). Every macro expands
to `__attribute__((annotate("weavec.<name>")))` under Clang and to nothing
under compilers without the `annotate` attribute, so annotated code stays
portable C.

| Macro             | Applies to                     | Meaning                                                                  |
| ----------------- | ------------------------------ | ------------------------------------------------------------------------ |
| `WEAVEC_OWNED`    | pointer parameters, returns, variables, fields | The pointer uniquely owns its referent and must release it exactly once. |
| `WEAVEC_BORROWED` | pointer parameters, returns, variables, fields | Shared, read-only borrow. The referent outlives the borrow.              |
| `WEAVEC_MUT`      | pointer parameters, returns, variables, fields | Exclusive, mutable borrow.                                               |
| `WEAVEC_UNSAFE`   | function declarations, compound statements     | Opt out of checking for the function body or the block.                  |
| `WEAVEC_ENABLED`  | (macro, not an attribute)      | `1` when the TU is being processed by `weavec`, else `0`.                |

## Placement

Attributes bind to the declarator they precede, so put them immediately before
the name for parameters and variables:

```c
struct buffer *WEAVEC_OWNED buffer_new(size_t n);           /* return value */
void buffer_free(struct buffer *WEAVEC_OWNED b);            /* consumes b   */
size_t buffer_len(const struct buffer *WEAVEC_BORROWED b);  /* borrows b    */
void buffer_push(struct buffer *WEAVEC_MUT b, int v);       /* mutably      */

int *WEAVEC_OWNED p = malloc(sizeof *p);
```

For functions, `WEAVEC_UNSAFE` goes before the declaration:

```c
WEAVEC_UNSAFE void poke_hardware(volatile uint32_t *reg) { *reg = 1; }
```

For blocks, before the opening brace:

```c
void f(int *p) {
  free(p);
  WEAVEC_UNSAFE {
    /* p is dangling here; we know the allocator keeps the page mapped. */
    log_address(p);
  }
}
```

## Diagnostics

Every WeaveC diagnostic ends with a stable identifier in brackets, e.g.
`[weavec::use-after-free]`. The current identifiers are:

| Identifier            | Severity | Emitted when                                                          |
| --------------------- | -------- | --------------------------------------------------------------------- |
| `use-after-free`      | error    | A pointer is read after being passed to `free`.                       |
| `double-free`         | error    | A pointer is passed to `free` twice without reassignment.             |
| `use-after-move`      | error    | A moved-out owned pointer is used. *(reserved; not emitted yet)*       |
| `conflicting-borrow`  | error    | A borrow violates the aliasing rules. *(reserved)*                    |
| `lifetime-too-short`  | error    | A borrow may outlive its referent. *(reserved)*                       |
| `unsafe-operation`    | error    | A `Raw` pointer is used outside `WEAVEC_UNSAFE`. *(reserved)*         |
| `annotation-required` | warning  | Ownership could not be inferred (`--report-unannotated`).             |
| `invalid-annotation`  | warning  | A `weavec.*` annotation WeaveC does not recognise.                    |

The identifiers are defined in `include/weavec/Core/Diagnostic.h`
(`weavec::core::diag`). Renaming one is a breaking change.

## Compatibility with other annotation schemes

`annotate` payloads that do not start with `weavec.` are ignored, so WeaveC
coexists with GSL (`gsl::owner`), Clang's own attributes, and project-specific
annotations.
