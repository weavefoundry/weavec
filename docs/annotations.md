# Annotations reference

WeaveC annotations live in the `weavec.h` header, which `weavec` puts on the
system include path automatically (`#include <weavec.h>`). Every macro expands
to `__attribute__((annotate("weavec.<name>")))` under Clang and to nothing
under compilers without the `annotate` attribute, so annotated code stays
portable C.

| Macro             | Applies to                     | Meaning                                                                  |
| ----------------- | ------------------------------ | ------------------------------------------------------------------------ |
| `WEAVEC_OWNED`    | pointer parameters, returns, variables, fields | The pointer uniquely owns its referent and must release it exactly once. On a struct field: the field owns its referent whenever the struct is live, so freeing the struct without releasing, moving or nulling the field first is a `leak` ([RFC 0007](rfcs/0007-resource-lifecycle.md)). |
| `WEAVEC_BORROWED` | pointer parameters, returns, variables, fields | Shared, read-only borrow. The referent outlives the borrow.              |
| `WEAVEC_MUT`      | pointer parameters, returns, variables, fields | Exclusive, mutable borrow.                                               |
| `WEAVEC_RAW`      | pointer parameters, returns, variables, fields, function-pointer types | No guarantee at all: the checker tracks the pointer but any dereference, release or transfer of ownership must happen inside a `WEAVEC_UNSAFE` region. |
| `WEAVEC_UNSAFE`   | function declarations, compound statements     | An *unsafe region*: raw operations are permitted and nothing inside is reported, but ownership still flows through it and out of it. |
| `WEAVEC_ENABLED`  | (macro, not an attribute)      | `1` when the TU is being processed by `weavec`, else `0`.                |

Annotations on a function-pointer type describe whatever is called through it
([RFC 0004](rfcs/0004-unsafe-boundaries.md)):

```c
typedef void (*dtor_t)(struct node *WEAVEC_OWNED);          /* every call consumes */
typedef WEAVEC_OWNED struct node *(*maker_t)(void);         /* every call allocates */
struct ops {
  void (*drop)(struct node *WEAVEC_OWNED);                  /* fields too      */
  void *(*WEAVEC_OWNED alloc)(size_t);                      /* result of alloc */
};
```

An annotation on the declarator of a function pointer (the typedef name, the
field or the parameter) describes the *result* of calls through it; the
annotations inside its parameter list describe the arguments.

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

An unsafe region is a boundary, not a hole: the checker still analyses what
happens inside it (so a `free` inside the block is a free as far as the code
after it is concerned, and the function's summary is still inferred) and only
stops *reporting* there. It is also the only place a `WEAVEC_RAW` pointer may
be dereferenced, released or handed to an owning parameter, and the place to
assert what a raw pointer really is:

```c
struct node *WEAVEC_OWNED node_from_handle(uintptr_t h) {
  WEAVEC_UNSAFE { return (struct node *)h; }   /* asserts: this is owned */
}

void stash(struct node *WEAVEC_OWNED n) {
  WEAVEC_UNSAFE { registry[key] = (uintptr_t)n; } /* ownership leaves the model */
}
```

Outside an unsafe region the same statements are `unsafe-operation` errors,
but the asserted kind still holds afterwards, so a single error replaces a
cascade. See [RFC 0004](rfcs/0004-unsafe-boundaries.md), *Laundering*.

### Where raw pointers come from

- a cast from an integer (`(struct node *)h`, `(void *)uintptr`);
- a declaration annotated `WEAVEC_RAW` (parameters, variables, fields,
  results and function-pointer results);
- a load through a raw pointer (`raw->next` is raw too);
- the result of a callee whose body returns or stores a raw value, or whose
  declaration says `WEAVEC_RAW`;
- under `--strict-externs`, every pointer that passes through a call the
  checker cannot resolve (see `annotation-required`).

Copying, comparing and converting a raw pointer back to an integer are fine
anywhere; passing it to a callee's `WEAVEC_RAW` parameter is fine too. Only a
dereference, a release, a move into an owning parameter, or a borrow by a
callee that reads or writes through it is a *raw operation*.

## Diagnostics

Every WeaveC diagnostic ends with a stable identifier in brackets, e.g.
`[weavec::use-after-free]`. The current identifiers are:

| Identifier            | Severity | Emitted when                                                          |
| --------------------- | -------- | --------------------------------------------------------------------- |
| `use-after-free`      | error    | A pointer (or any alias of it) is used after being passed to `free`. Note: `freed here` / `freed here (through '<q>')`. |
| `double-free`         | error    | A pointer (or any alias of it) is passed to `free` twice without reassignment. Note: `previously freed here [(through '<q>')]`. |
| `use-after-move`      | error    | A pointer is used after being passed to a `WEAVEC_OWNED` parameter, to `realloc`, or to a function that moves it (on every path, or on the paths whose result the caller has not ruled out; [RFC 0006](rfcs/0006-precision.md)). Note: `moved here`. |
| `conflicting-borrow`  | error    | An object is freed or moved while a live pointer into it exists: `cannot free '<p>' while it is borrowed`, `cannot move '<p>' while it is borrowed`. Note: `borrowed by '<q>' here`. A pointer is *live* until its last use ([RFC 0006](rfcs/0006-precision.md)). With `--exclusive-borrows` (`-fweavec-exclusive-borrows`), RFC 0001's exclusivity rules are enforced too: `cannot borrow '<x>' as mutable because it is already borrowed`, `... as shared because it is already mutably borrowed`, `cannot assign to '<x>' while it is borrowed`; the note names the other pointer. |
| `lifetime-too-short`  | error    | A pointer may outlive what it points to: `'<p>' may outlive '<x>', which it points to` (stored into an outer scope, a global or through a parameter) or `returned pointer may outlive '<x>', which it points to`. Notes: where `<x>` is declared and where it goes out of scope. |
| `unsafe-operation`    | error    | A raw operation outside a `WEAVEC_UNSAFE` region ([RFC 0004](rfcs/0004-unsafe-boundaries.md)): `dereference of raw pointer '<p>' outside an unsafe region`, `'<f>' dereferences raw pointer '<p>' ...` (also `releases`, `takes ownership of`), `raw pointer '<p>' is assigned to '<q>', which is declared WEAVEC_OWNED, outside an unsafe region` (any safe annotation), `raw pointer is returned from a function whose return type is annotated WEAVEC_OWNED outside an unsafe region`, and with `--strict-externs`, `unchecked call to '<f>' outside an unsafe region` / `unchecked call through '<fp>' ...`. Notes: why the pointer is raw (`'<p>' is raw: cast from an integer here`, `declared WEAVEC_RAW here`, `loaded through raw pointer '<q>' here`, `handed out by '<f>' here`, `returned by a call into unchecked code ('<f>') here`, each optionally `(through '<alias>')`) and `move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first`. |
| `mismatched-release`  | error    | A resource is released (or moved into a consuming parameter) by a function of another release family ([RFC 0007](rfcs/0007-resource-lifecycle.md)): `'<p>' is released with 'free' but must be released with 'fclose'`. Both names are family names, the canonical releaser of the allocator (`malloc`/`strdup`/`realloc` → `free`, `fopen` → `fclose`, `opendir` → `closedir`, ...), even when the release went through a wrapper defined in the program. Note: `allocated here`. |
| `leak`                | warning  | An owned resource is lost without being released, moved or stored where the caller can see it ([RFC 0007](rfcs/0007-resource-lifecycle.md)): `'<p>' is leaked` at the point its last holder goes out of reach (a `return`, a scope end, the statement after its last use); `'<p>' is leaked: it is overwritten without being released` at the assignment; `'<b>->p' is leaked when '<b>' is freed` (also `'*a' is leaked when 'a' is freed`) at the release of a container whose `WEAVEC_OWNED` field, or a field this function stored an owned value into, still owns something; `result of '<f>' is leaked` at a discarded allocating call. Notes: `allocated here`, or `'<p>' is declared WEAVEC_OWNED here` for a parameter or field. Not reported: pointers handed to callees the checker cannot follow or cast to integers (they are *escaped*), resources kept by globals or `static` locals when the function returns, blocks that end in a `noreturn` call, fields of an object this function allocated, and the old block after a failed in-place `realloc`. |
| `annotation-mismatch` | error    | A definition contradicts its own annotation: `'<p>' is annotated WEAVEC_BORROWED but is freed here` (also `WEAVEC_MUT`; also `moved`, `written through`; also `... but '<p>->f' is freed here` for a path under the parameter), `function returns a borrow but its return type is annotated WEAVEC_OWNED`, `function returns a fresh allocation but its return type is annotated WEAVEC_BORROWED` (or `WEAVEC_MUT`). Notes: `'<p>' is annotated here` / `annotated here`; `'<q>' is a copy of '<p>'` when through an alias. Callers keep trusting the annotation. |
| `annotation-required` | warning  | **On by default:** `call to '<f>' is not checked: it has no definition or ownership annotations here`, once per callee per program (RFC 0005: a definition in any unit analysed together with this one counts; alone, the unit is the program), for a callee with pointer parameters or a pointer result that has no body in the program, no annotations and no libc entry; callees from system headers are exempt. Notes: `'<f>' is declared here`, `annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this program`. Likewise `call through '<fp>' is not checked: its function type has no ownership annotations and no function of that type has its address taken in this program`, once per function-pointer type. With `--strict-externs` these calls are `unsafe-operation` errors instead (at every call site, including callees from system headers), and their pointer result is raw. **With `--report-unannotated`:** every exported (non-`static`) definition additionally gets `pointer parameter '<p>' of '<f>' is inferred WEAVEC_OWNED; add the annotation to its declaration` (or `WEAVEC_BORROWED` / `WEAVEC_MUT`; `return value of '<f>' is inferred ...`) with a fix-it that inserts the annotation, or `pointer parameter '<p>' has no inferable ownership; annotate it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT` when the body gives no evidence. |
| `invalid-annotation`  | warning  | A `weavec.*` annotation WeaveC does not recognise.                    |

The identifiers are defined in `include/weavec/Core/Diagnostic.h`
(`weavec::core::diag`). Renaming one is a breaking change. The rules behind
them are specified by [RFC 0001](rfcs/0001-ownership-model.md),
[RFC 0002](rfcs/0002-intraprocedural-checking.md),
[RFC 0003](rfcs/0003-signature-inference.md),
[RFC 0004](rfcs/0004-unsafe-boundaries.md),
[RFC 0006](rfcs/0006-precision.md) and
[RFC 0007](rfcs/0007-resource-lifecycle.md).

Fix-its are emitted through Clang, so `-fdiagnostics-parseable-fixits` and
editor integrations that apply Clang fix-its work unchanged:

```
$ weavec --report-unannotated buffer.c --
buffer.c:12:27: warning: pointer parameter 'b' of 'buffer_free' is inferred WEAVEC_OWNED; add the annotation to its declaration [weavec::annotation-required]
   12 | void buffer_free(struct buffer *b) { free(b->data); free(b); }
      |                                 ^
      |                                 WEAVEC_OWNED
```

## What the checker understands

- **Ownership sources**: `malloc`, `calloc`, `realloc`, `strdup`, `strndup`,
  `aligned_alloc`, `fopen`, `opendir`, `getaddrinfo`, `asprintf`, `getline`,
  `mmap`, `pthread_create`, ... (the shipped libc/POSIX table, about 490
  functions from `<stdlib.h>`, `<stdio.h>`, `<string.h>`, `<unistd.h>`,
  `<fcntl.h>`, `<dirent.h>`, `<sys/mman.h>`, `<netdb.h>`, `<pthread.h>`,
  `<time.h>`, `<pwd.h>`, `<grp.h>`, `<regex.h>`, `<dlfcn.h>`, `<wchar.h>`
  and friends), any function whose return type carries `WEAVEC_OWNED`, and
  any function defined in the program whose body returns a fresh
  allocation.
- **Releases and moves**: `free`, `fclose`, `closedir`, `freeaddrinfo`,
  `munmap`, `regfree`, `dlclose`, ..., passing a pointer to a
  `WEAVEC_OWNED` parameter, and passing it to a function defined in the
  program whose body frees or moves that parameter (through any depth of
  wrappers and through recursion). `realloc(p, n)` moves `p`; on the path
  where the result is tested null (`if (!q)`, `q == NULL`, ...), `p` is
  valid again.
- **Resource lifecycle** ([RFC 0007](rfcs/0007-resource-lifecycle.md)):
  every ownership source above puts a resource on the function's books,
  with the *release family* of its allocator (`free`, `fclose`, `closedir`,
  `freeaddrinfo`, `munmap`, `pclose`, `regfree`, `dlclose`, ...; a
  `WEAVEC_OWNED` parameter's family is unknown). It leaves the books when
  it is released by a function of the same family, moved into an owning
  parameter, returned, stored into caller-visible memory (`*out`, `b->data`,
  a global), copied into a struct that is returned or stored, handed to a
  callee the checker cannot follow, or cast to an integer. A resource still
  on the books when its last holder dies is a `leak`; a release by another
  family is a `mismatched-release`. Copies share one record (`q = p; ...
  use(q);` reports `q` once), a callee summarised from its body that keeps
  nothing is trusted not to retain the argument, and the null edge of a
  test of the holder (`if (!p) return -1;`) owns nothing. A constructor
  that reports failure through its result (`int make(char **out) { *out =
  malloc(n); return *out != NULL; }`) is summarised with the classes on
  which `*out` is null, so `if (!make(&s)) return;` is clean.
- **Outcome-conditional consumption** ([RFC 0006](rfcs/0006-precision.md)):
  a callee that frees or moves its argument only on the paths that return
  some class of value (`NULL` vs non-null, `0` vs positive vs negative) is
  summarised per class, and a test of its result selects the class: after
  `int rc = try_take(p);` the pointer `p` is gone where `rc == 0` and still
  yours where `rc != 0`, if `try_take` frees only when it returns `0`.
  Recognised tests: `x`, `!x`, `x == NULL`, `x != NULL`, `x == 0`, `x != 0`
  and comparisons with an integer constant (`x < 0`, `x == -1`, `x >= 0`,
  ...), on the call itself, on the variable its result was stored in, or
  on an assignment inside the condition (`if ((rc = f(p)) < 0)`). Wrappers
  (`char *q = realloc(p, n); if (!q) return NULL; return q;`) inherit the
  conditional behaviour. Untested, a conditional free is a may-free and the
  next use is reported.
- **Calls through function pointers**: the callee's signature is taken from
  the annotations on the function-pointer type (typedef, field or parameter)
  when it has any; otherwise from the join of the summaries of every
  function of that type whose address is taken anywhere in the program
  (`ops.drop = node_free;`, `qsort(a, n, sz, cmp)`), so a call through
  `ops->drop` frees what `node_free` frees. Pointers with neither are
  reported once per type as `annotation-required`.
- **The program**: with `weavec file.c --` the program is that one file;
  with `weavec --whole-program` or `weavec-cc` it is every file analysed or
  linked together, and a callee defined in another file is checked from
  its body there (a definition wins over the libc table, so a program that
  defines its own `strdup` is checked against its own). A whole-struct
  copy `b = a` copies the facts of every pointer field, so `b = a;
  free(a.data); b.data[0]` is a use after free.
- **Effects through parameters**: a callee that frees `b->data`, writes
  `*out`, or stores a fresh allocation into `*out` or a global has that
  effect at the call site. A callee that re-nulls what it freed
  (`free(b->data); b->data = NULL;`) leaves the field usable, because the
  caller's memory is described by the callee's exit state.
- **Result provenance**: a callee that returns one of its arguments or a
  pointer into one (`strchr`, `next_of(n)`, `&n->v`) makes the result an
  alias or a borrow of that argument in the caller, so freeing the argument
  then using the result is reported.
- **Aliases**: `q = p` makes `q` and `p` names for the same object, so a free
  through either is a free of both. Reassigning a pointer separates it. So
  does a test ([RFC 0006](rfcs/0006-precision.md)): on the edge where `p
  != q` holds the two are distinct (`if (l == sentinel) return; free(l);
  use(sentinel);` is clean), and on the edge where `p == q` holds they are
  the same object. `!=` separates only pointers that hold the *same value*
  (`q = p`); `q = p + 1` points into the same object and stays an alias.
- **Borrows**: `&x`, `&s->f`, array decay and `&a[i]` create a loan held by
  the pointer being assigned (mutable unless the pointer's pointee type is
  `const`). Arguments to `WEAVEC_BORROWED`/`WEAVEC_MUT` parameters, and to
  parameters that a defined callee reads or writes through, borrow for the
  duration of the call. A loan ends when its holder is reassigned or is
  *last used* ([RFC 0006](rfcs/0006-precision.md)): `int *a = &n->v; *a =
  1; free(n);` is fine, `free(n); *a = 1;` is not. Loans held through a
  pointer (`h->view = &n->v`), by a global, or by a local whose address is
  taken last until the holder is reassigned. Copying a pointer that holds
  a loan into a longer-lived pointer is checked like creating the loan
  there. Two live pointers into one object, or writing an object another
  pointer views, are accepted by default; `--exclusive-borrows` rejects
  them as RFC 0001 does.
- **Places**: `p`, `s.f`, `p->f`, `*pp`, `p->a->b`, file-scope variables;
  all elements of an array share one place, written `a[*]` in diagnostics
  (`*a` for a pointer `a`). A free or move of an element remembers which
  element was named ([RFC 0006](rfcs/0006-precision.md)): `free(a[i]);
  a[i][0] = 0;` and `free(a[0]); free(a[0]);` are reported, while `for (i)
  free(a[i]); free(a);`, `free(a[0]); use(a[1]);` and `free(a[i]); a[i] =
  NULL;` are clean. Assigning, incrementing or taking the address of the
  index variable makes the element unknown: it is then neither reported by
  element accesses nor cleared by element writes. An access without a
  subscript (`*a`, `free(a)` of the array's owner) matches every element.
- **Overwrites**: a callee that writes an object wholesale (`memcpy(root,
  &tmp, sizeof *root)`, or a defined function that writes through the
  parameter) makes every fact about the object's fields stale, so
  `free(root->string); memcpy(root, ...); use(root->string);` is clean
  ([RFC 0006](rfcs/0006-precision.md)).
- **Annotations are checked**: a body that frees a `WEAVEC_BORROWED`
  parameter is an `annotation-mismatch`; the annotation still governs what
  callers assume.
- **Pointer identity**: pointer arithmetic (`p + 1`, `p++`, `&a[i]`) and
  casts between pointer types (`(char *)p`, `(void *)p`) name the same
  object as the operand, so `free(p); use(p + 1)` is a use after free. Only
  a round trip through an integer loses identity, and what comes back is a
  *raw* pointer.
- **Raw pointers and unsafe regions**: a raw pointer is tracked (copies,
  comparisons and conversions to integers are fine) but dereferencing,
  releasing or handing it to an owning parameter outside a `WEAVEC_UNSAFE`
  region is an `unsafe-operation`. Unsafe regions are analysed like any
  other code with their diagnostics suppressed, so a `free` inside one is
  still a free afterwards.
- **Unchecked calls**: a call to a function with no body here, no annotations
  and no libc entry borrows its pointer arguments for the call, retains
  nothing, and returns an unknown value (this is what `annotation-required`
  reports). With `--strict-externs` the call is an `unsafe-operation` unless
  it is inside an unsafe region; its arguments are left alone and its
  pointer result is raw.

`weavec --dump-analysis file.c -- <flags>` prints the inferred places,
lifetimes, exit state (including which places hold raw pointers, and why,
and which hold an owned resource, with its release family) and summary of
every analysed function; with `--whole-program` it ends with
the program database (every exported summary). `weavec-cc` writes each
unit's exported summaries to `<object>.weavec` in the same text form.

## Controlling diagnostics

`weavec` and `weavec-cc` accept Clang-style warning flags for WeaveC's ids:

| Flag                          | Effect                                                                                            |
| ----------------------------- | ------------------------------------------------------------------------------------------------- |
| `-Wno-weavec-<id>`            | Disable a diagnostic whose default severity is *warning* (`annotation-required`, `invalid-annotation`, `leak`). Refused for an error. |
| `-Wweavec-<id>`               | Re-enable it.                                                                                     |
| `-Wno-error=weavec-<id>`      | Report an error as a warning (the migration path for a codebase that wants to build while it works through the reports). |
| `-Werror=weavec-<id>`         | Report a warning as an error.                                                                     |
| `-Wno-weavec`, `-Wno-error=weavec`, `-Werror=weavec` | The same for every WeaveC id (`-Wno-weavec` leaves the errors alone).                  |

The soundness statement assumes default severities. `weavec-cc` additionally
takes `-fno-weavec` (compile only), `-fweavec-strict` (`--strict-externs`),
`-fweavec-report-unannotated`, `-fweavec-analyze-headers`,
`-fweavec-dump-analysis`, `-fweavec-exclusive-borrows`
(`--exclusive-borrows`) and `-fno-weavec-link` (skip the link-time
whole-program step).

## Compatibility with other annotation schemes

`annotate` payloads that do not start with `weavec.` are ignored, so WeaveC
coexists with GSL (`gsl::owner`), Clang's own attributes, and project-specific
annotations.
