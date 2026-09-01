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
| `use-after-free`      | error    | A pointer (or any alias of it) is used after being passed to `free`. Note: `freed here` / `freed here (through '<q>')`. |
| `double-free`         | error    | A pointer (or any alias of it) is passed to `free` twice without reassignment. Note: `previously freed here [(through '<q>')]`. |
| `use-after-move`      | error    | A pointer is used after being passed to a `WEAVEC_OWNED` parameter or to `realloc` (without a null test). Note: `moved here`. |
| `conflicting-borrow`  | error    | A borrow violates the aliasing rules: `cannot borrow '<x>' as mutable because it is already borrowed`, `... as shared because it is already mutably borrowed`, `cannot assign to '<x>' while it is borrowed`, `cannot free '<p>' while it is borrowed`, `cannot move '<p>' while it is borrowed`. Note names the other pointer. |
| `lifetime-too-short`  | error    | A pointer may outlive what it points to: `'<p>' may outlive '<x>', which it points to` (stored into an outer scope, a global or through a parameter) or `returned pointer may outlive '<x>', which it points to`. Notes: where `<x>` is declared and where it goes out of scope. |
| `unsafe-operation`    | error    | A `Raw` pointer is used outside `WEAVEC_UNSAFE`. *(reserved)*         |
| `annotation-required` | warning  | Ownership could not be inferred (`--report-unannotated`).             |
| `invalid-annotation`  | warning  | A `weavec.*` annotation WeaveC does not recognise.                    |

The identifiers are defined in `include/weavec/Core/Diagnostic.h`
(`weavec::core::diag`). Renaming one is a breaking change. The rules behind
them are specified by [RFC 0001](rfcs/0001-ownership-model.md) and
[RFC 0002](rfcs/0002-intraprocedural-checking.md).

## What the checker understands

- **Ownership sources**: `malloc`, `calloc`, `realloc`, `strdup`, `strndup`,
  `aligned_alloc`, and any function whose return type carries `WEAVEC_OWNED`.
- **Releases and moves**: `free`, and passing a pointer to a `WEAVEC_OWNED`
  parameter. `realloc(p, n)` moves `p`; on the path where the result is
  tested null (`if (!q)`, `q == NULL`, ...), `p` is valid again.
- **Aliases**: `q = p` makes `q` and `p` names for the same object, so a free
  through either is a free of both. Reassigning a pointer separates it.
- **Borrows**: `&x`, `&s->f`, array decay and `&a[i]` create a loan held by
  the pointer being assigned (mutable unless the pointer's pointee type is
  `const`). Arguments to `WEAVEC_BORROWED`/`WEAVEC_MUT` parameters borrow for
  the duration of the call. A loan ends when its holder is reassigned or goes
  out of scope.
- **Places**: `p`, `s.f`, `p->f`, `*pp`, `p->a->b`; all elements of an array
  share one place, written `a[*]` in diagnostics.
- **Not tracked**: pointer arithmetic (`p + 1`, `p++`) and casts between
  unrelated pointer types yield opaque values that are neither checked nor
  reported; calls to unannotated functions neither move nor borrow their
  arguments.

`weavec --dump-analysis file.c -- <flags>` prints the inferred places,
lifetimes and exit state of every analysed function.

## Compatibility with other annotation schemes

`annotate` payloads that do not start with `weavec.` are ignored, so WeaveC
coexists with GSL (`gsl::owner`), Clang's own attributes, and project-specific
annotations.
