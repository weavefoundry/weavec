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
| `WEAVEC_NULLABLE` | pointer parameters, returns, variables, fields | The pointer may be null ([RFC 0008](rfcs/0008-pointer-validity.md)). On a parameter: the body must test it before dereferencing it, and callers may pass null. On a return type: callers must test the result. On a variable or field: every load is treated as maybe-null until it is tested. Says nothing about ownership; combine with `WEAVEC_OWNED`/`WEAVEC_BORROWED`/`WEAVEC_MUT` as needed. |
| `WEAVEC_NONNULL`  | pointer parameters, returns, variables, fields | The pointer is never null ([RFC 0008](rfcs/0008-pointer-validity.md)). On a parameter: callers must pass a pointer the checker knows is non-null, even if the body never dereferences it. On a return type: callers need not test the result. On a variable or field: it is never reported as null. Says nothing about ownership. |
| `WEAVEC_RETAINS`  | pointer parameters             | The callee takes a reference on the argument's object ([RFC 0010](rfcs/0010-shared-ownership.md)): the caller's pointer gains a *share*, which the next copy of it carries away. On a declaration with no body whose result has the parameter's type and no ownership annotation, the result is a copy of the argument (the shape of `g_object_ref`). |
| `WEAVEC_RELEASES` | pointer parameters             | The callee releases one reference ([RFC 0010](rfcs/0010-shared-ownership.md)): the argument's name is dead afterwards; other shares of the object are untouched. |
| `WEAVEC_REFCOUNT` | integer fields                 | The field is a reference count ([RFC 0010](rfcs/0010-shared-ownership.md)): a share taken through it and never released is a `leak` even when no function in the program releases through it. Inference recognises the field by its increments and decrements regardless. |
| `WEAVEC_OWNED_BY(f)` | pointer parameters and returns, next to `WEAVEC_OWNED` | The release family is `f` ([RFC 0010](rfcs/0010-shared-ownership.md)): `struct handle *handle_open(void) WEAVEC_OWNED WEAVEC_OWNED_BY(handle_close);` makes `free(handle_open())` a `mismatched-release`. Without `WEAVEC_OWNED` it is an `invalid-annotation`. |
| `WEAVEC_SIZED_BY(n)` | pointer parameters          | The caller passes at least `n` elements behind the pointer (bytes for `void *`), `n` being another parameter of the same function by name ([RFC 0011](rfcs/0011-spatial-safety.md)): `void fill(char *WEAVEC_SIZED_BY(len) buf, size_t len);`. Inside the body the parameter has that extent, so `buf[len]` is `out-of-bounds`; at every call the argument must have at least that many, so `fill(small, 8)` on `char small[4]` is `out-of-bounds`. Says nothing about ownership or nullness; combine with the others as needed. On a non-pointer, or naming a parameter that does not exist or is not an integer: `invalid-annotation`. |
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

struct node *WEAVEC_BORROWED WEAVEC_NULLABLE find(int key); /* may return NULL */
int node_value(const struct node *WEAVEC_BORROWED WEAVEC_NONNULL n);
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
| `use-after-free`      | error    | A pointer (or any alias of it) is used after being passed to `free`. Note: `freed here` / `freed here (through '<q>')`. After a share release ([RFC 0010](rfcs/0010-shared-ownership.md)): `use of '<p>' after its reference was released`, note `reference released here`. |
| `double-free`         | error    | A pointer (or any alias of it) is passed to `free` twice without reassignment. Note: `previously freed here [(through '<q>')]`. Two share releases of one name ([RFC 0010](rfcs/0010-shared-ownership.md)): `'<p>' is released twice`, note `previously released here`. |
| `use-after-move`      | error    | A pointer is used after being passed to a `WEAVEC_OWNED` parameter, to `realloc`, or to a function that moves it (on every path, or on the paths whose result the caller has not ruled out; [RFC 0006](rfcs/0006-precision.md)). Note: `moved here`. |
| `conflicting-borrow`  | error    | An object is freed or moved while a live pointer into it exists: `cannot free '<p>' while it is borrowed`, `cannot move '<p>' while it is borrowed`. Note: `borrowed by '<q>' here`. A pointer is *live* until its last use ([RFC 0006](rfcs/0006-precision.md)). Not reported for a pointer *derived* from `<p>` (`q = &p->f`, `q = p + 1`): that is a name for the same object, and its later use is a `use-after-free` ([RFC 0011](rfcs/0011-spatial-safety.md)). With `--exclusive-borrows` (`-fweavec-exclusive-borrows`), RFC 0001's exclusivity rules are enforced too: `cannot borrow '<x>' as mutable because it is already borrowed`, `... as shared because it is already mutably borrowed`, `cannot assign to '<x>' while it is borrowed`; the note names the other pointer. |
| `lifetime-too-short`  | error    | A pointer may outlive what it points to: `'<p>' may outlive '<x>', which it points to` (stored into an outer scope, a global or through a parameter) or `returned pointer may outlive '<x>', which it points to`. Notes: where `<x>` is declared and where it goes out of scope. Reported at the store, but decided when `<x>` dies ([RFC 0011](rfcs/0011-spatial-safety.md)): a store undone before then (`ls->fs = fs.prev`), or into a holder that is dead by then, is not reported. |
| `unsafe-operation`    | error    | A raw operation outside a `WEAVEC_UNSAFE` region ([RFC 0004](rfcs/0004-unsafe-boundaries.md)): `dereference of raw pointer '<p>' outside an unsafe region`, `'<f>' dereferences raw pointer '<p>' ...` (also `releases`, `takes ownership of`), `raw pointer '<p>' is assigned to '<q>', which is declared WEAVEC_OWNED, outside an unsafe region` (any safe annotation), `raw pointer is returned from a function whose return type is annotated WEAVEC_OWNED outside an unsafe region`, and with `--strict-externs`, `unchecked call to '<f>' outside an unsafe region` / `unchecked call through '<fp>' ...`. Notes: why the pointer is raw (`'<p>' is raw: cast from an integer here`, `declared WEAVEC_RAW here`, `loaded through raw pointer '<q>' here`, `handed out by '<f>' here`, `returned by a call into unchecked code ('<f>') here`, each optionally `(through '<alias>')`) and `move this operation into a WEAVEC_UNSAFE block or function, or assert the pointer's ownership first`. |
| `mismatched-release`  | error    | A resource is released (or moved into a consuming parameter) by a function of another release family ([RFC 0007](rfcs/0007-resource-lifecycle.md)): `'<p>' is released with 'free' but must be released with 'fclose'`. Both names are family names, the canonical releaser of the allocator (`malloc`/`strdup`/`realloc` → `free`, `fopen` → `fclose`, `opendir` → `closedir`, ...), even when the release went through a wrapper defined in the program. Note: `allocated here`. |
| `leak`                | warning  | An owned resource is lost without being released, moved or stored where the caller can see it ([RFC 0007](rfcs/0007-resource-lifecycle.md)): `'<p>' is leaked` at the point its last holder goes out of reach (a `return`, a scope end, the statement after its last use); `'<p>' is leaked: it is overwritten without being released` at the assignment; `'<b>->p' is leaked when '<b>' is freed` (also `'*a' is leaked when 'a' is freed`) at the release of a container whose `WEAVEC_OWNED` field, or a field this function stored an owned value into, still owns something; `result of '<f>' is leaked` at a discarded allocating call. Notes: `allocated here`, `'<p>' is declared WEAVEC_OWNED here` for a parameter or field, or `reference taken here` for a share retained by a count increment and dropped ([RFC 0010](rfcs/0010-shared-ownership.md); reported only through a *known count*: a field some function in the program releases through, or one annotated `WEAVEC_REFCOUNT`). Not reported: pointers handed to callees the checker cannot follow or cast to integers (they are *escaped*), resources kept by globals or `static` locals when the function returns, blocks that end in a `noreturn` call, fields of an object this function allocated, and the old block after a failed in-place `realloc`. |
| `null-dereference`    | error    | A pointer that is null, or may be null, on some path reaching here is dereferenced ([RFC 0008](rfcs/0008-pointer-validity.md)): `dereference of '<p>', which may be null` / `dereference of '<p>', which is null`; or passed to a callee that dereferences its parameter without testing it (a `requires` fact in the callee's summary, or `WEAVEC_NONNULL` on its declaration): `'<p>', which may be null, is passed to '<f>', which dereferences it` (also `which is null`, `a null pointer is passed to '<f>' ...`). The note says why: `'<p>' may be null: it is the result of '<f>' here` (every allocator in the shipped table, every searching function and every function the program defines whose body can return null), `'<p>' may be null: it is set by '<f>' here` (a callee's store), `'<p>' is assigned NULL here`, `'<p>' may be null: it is compared with NULL here` (tested, and the null edge merged back), `'<p>' is declared WEAVEC_NULLABLE here` / `the result of '<f>' is declared WEAVEC_NULLABLE here`; for a call, also `'<f>' is declared here`. Not reported: pointers with no fact (parameters, loaded fields, results of unchecked code), dereferences inside an unsafe region, and a second dereference of the same pointer. |
| `use-of-uninitialized`| error    | A pointer variable, or a pointer field of a record variable, declared without an initialiser is read, dereferenced, copied or released before it is assigned ([RFC 0008](rfcs/0008-pointer-validity.md)): `use of '<p>' before it was initialized` (also `'<s>.f'`). Note: `'<p>' is declared here`. Any assignment, a callee's store (`init(&p)`), a mutable borrow for a call, `memset` or a whole-object write initialises it; `static` and address-taken variables are not tracked. |
| `invalid-release`     | error    | A releaser (or a consuming parameter) is handed a pointer that is not the start of a heap allocation ([RFC 0008](rfcs/0008-pointer-validity.md)): `'<p>' is released but points to '<x>', which is not a heap object` (a stack or static variable, an array, a field of one; `'<x>' is released but is not a heap object` when `<p>` is `<x>` itself), `'<p>' is released but points to a string literal`, `'<p>' is released but points 4 elements past the start of its allocation` / `points to field 'in' of its allocation` / `does not point to the start of its allocation` (`p + 1`, `strchr(p, c)`, `p++`, `&o->in`; the offset is named when the checker knows it, [RFC 0011](rfcs/0011-spatial-safety.md)). Notes: `'<x>' is declared here` / `allocated here`. |
| `out-of-bounds`       | error    | An access reaches past the object it is in, or before its start ([RFC 0011](rfcs/0011-spatial-safety.md)). Direct accesses: `'<p>[<i>]' is out of bounds: index <k> of an object of <n> bytes` (both constant; the index is spelled as written, with its folded value in parentheses when that differs), `'<p>[<i>]' is out of bounds: '<i>' is the number of elements of '<p>'` / `'<i>' is at least '<n>', the number of elements of '<p>'` / `'<i>' is above '<n>', ...` (the index related to the count by a condition), `'<p>[<i>]' may be out of bounds: '<i>' may equal '<n>', the number of elements of '<p>'` (`i <= n`: the boundary is one past) / `'<i>' may reach one below '<n>', and '<p>' has <n> * 4 bytes` (`p[i + 1]` under `i < n`), `'<p>[<i>]' may be out of bounds: '<i>' may be 7 in an object of 4 bytes` (the index bounded above by a constant: `for (i = 0; i < 8; i++)`), `'<p>[<i>]' is out of bounds: index <k> is before the start of '<p>'`. Library calls with a buffer and a length (`memcpy`, `memmove`, `memset`, `memcmp`, `fgets`, `snprintf`, `read`, `write`, `strncpy`, ...): `'memcpy' accesses 16 bytes of '<p>', which has 8 bytes` and the relational forms (`'memset' accesses 'm' bytes of 'p', which has 'n' bytes ('m' is above 'n')`, `'memset' may access past the end of 'buf': 'n' may be 8, and 'buf' has 4 bytes`). A callee's requirement at the call: `'put7' requires 8 bytes behind '<p>', which has 4 bytes`. Notes: `'<p>' is allocated here` / `'<p>' is declared here` / `the object behind '<p>' is declared here`. Extents come from allocations (`malloc(n)`, `calloc(n, sz)`, `realloc(p, n)`, and every function in the program that returns one: `xmalloc(n)` returns `fresh extent=n`), from the declared size of a variable, an array or an array member, from string literals and from `WEAVEC_SIZED_BY`. Not reported: an access whose index the checker cannot relate to the extent (nothing is proved safe either), a pointer at a field or unknown offset into its object, and an object of unknown size. |
| `annotation-mismatch` | error    | A definition contradicts its own annotation: `'<p>' is annotated WEAVEC_BORROWED but is freed here` (also `WEAVEC_MUT`; also `moved`, `written through`; also `... but '<p>->f' is freed here` for a path under the parameter), `function returns a borrow but its return type is annotated WEAVEC_OWNED`, `function returns a fresh allocation but its return type is annotated WEAVEC_BORROWED` (or `WEAVEC_MUT`). Notes: `'<p>' is annotated here` / `annotated here`; `'<q>' is a copy of '<p>'` when through an alias. Callers keep trusting the annotation. |
| `annotation-required` | warning  | **On by default:** `call to '<f>' is not checked: it has no definition or ownership annotations here`, once per callee per program (RFC 0005: a definition in any unit analysed together with this one counts; alone, the unit is the program), for a callee with pointer parameters or a pointer result that has no body in the program, no annotations and no libc entry; callees from system headers are exempt. Notes: `'<f>' is declared here`, `annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this program`. Likewise `call through '<fp>' is not checked: its function type has no ownership annotations and no function of that type has its address taken in this program`, once per function-pointer type. With `--strict-externs` these calls are `unsafe-operation` errors instead (at every call site, including callees from system headers), and their pointer result is raw. **With `--report-unannotated`:** every exported (non-`static`) definition additionally gets `pointer parameter '<p>' of '<f>' is inferred WEAVEC_OWNED; add the annotation to its declaration` (or `WEAVEC_BORROWED` / `WEAVEC_MUT`; `return value of '<f>' is inferred ...`) with a fix-it that inserts the annotation, or `pointer parameter '<p>' has no inferable ownership; annotate it with WEAVEC_OWNED, WEAVEC_BORROWED or WEAVEC_MUT` when the body gives no evidence. |
| `invalid-annotation`  | warning  | A `weavec.*` annotation WeaveC does not recognise, `WEAVEC_NULLABLE` and `WEAVEC_NONNULL` on the same declaration, `WEAVEC_RETAINS` and `WEAVEC_RELEASES` on the same declaration, `WEAVEC_OWNED_BY(f)` without `WEAVEC_OWNED` ([RFC 0010](rfcs/0010-shared-ownership.md)), or `WEAVEC_SIZED_BY(n)` on a non-pointer or naming no integer parameter (`'<p>' is declared WEAVEC_SIZED_BY(n) but is not a pointer` / `... but 'n' is not an integer parameter`, [RFC 0011](rfcs/0011-spatial-safety.md)). Reported on definitions. |

The identifiers are defined in `include/weavec/Core/Diagnostic.h`
(`weavec::core::diag`). Renaming one is a breaking change. The rules behind
them are specified by [RFC 0001](rfcs/0001-ownership-model.md),
[RFC 0002](rfcs/0002-intraprocedural-checking.md),
[RFC 0003](rfcs/0003-signature-inference.md),
[RFC 0004](rfcs/0004-unsafe-boundaries.md),
[RFC 0006](rfcs/0006-precision.md),
[RFC 0007](rfcs/0007-resource-lifecycle.md),
[RFC 0008](rfcs/0008-pointer-validity.md) and
[RFC 0009](rfcs/0009-value-conditional-behaviour.md) (which adds no
diagnostic; it shrinks the set of programs that trigger the existing ones).

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
  effect at the call site. A callee that releases a value and then
  reinitialises the place (`free(b->data); b->data = NULL;`, `v->items =
  realloc(v->items, n)`) is summarised as `freed,replaced` ([RFC
  0008](rfcs/0008-pointer-validity.md), *Replaced values*): the place
  itself is usable afterwards, but every copy of the *old* value the caller
  kept (`int *old = v->items; grow(v); old[0]`) is dead. Only what happens
  on a path that returns counts (`free(g); exit(1);` is no effect), and a
  callee that frees *an element* (`free(history[len])`) says so
  (`freed,element`): which element is not the caller's to know, so calling
  it twice is not a `double-free`.
- **Struct-by-value results** ([RFC 0008](rfcs/0008-pointer-validity.md)):
  a function that returns a record hands the caller its pointer fields
  (`stores{result.data = fresh(free)}`), so `struct buf b = make(); ...`
  owns `b.data` and must release it.
- **Nullness** ([RFC 0008](rfcs/0008-pointer-validity.md)): the checker
  knows when a pointer is null (`p = NULL`) or may be null (the result of
  `malloc`, `strchr`, `fopen`, `getenv`, ... or of any function in the
  program that can return null; a pointer compared with null whose null
  edge merged back). Dereferencing it, or passing it to a callee whose body
  dereferences the parameter without testing it, is a `null-dereference`.
  Every test idiom clears the fact on the non-null edge (`if (!p) return;`,
  `if (p && p->x)`, `p ? p->x : 0`, `while ((q = f()) != NULL)`,
  `__builtin_expect`), for the pointer and its copies; a callee's outcome
  does too (`if (!make(&p)) return -1; p[0]` is clean when `make` returns
  `*out != NULL`). Pointers the checker knows nothing about (parameters,
  loaded fields, results of unchecked code) are trusted; `WEAVEC_NULLABLE`
  and `WEAVEC_NONNULL` say otherwise. Summaries record which parameters a
  function requires non-null (`requires{s}`) and the classes on which an
  out-parameter is non-null (`notnull{*out}`).
- **Uninitialised pointers** ([RFC 0008](rfcs/0008-pointer-validity.md)):
  `char *p;` and the pointer fields of `struct buf b;` hold nothing until
  they are assigned; using them first is a `use-of-uninitialized`.
- **Invalid releases** ([RFC 0008](rfcs/0008-pointer-validity.md)):
  `free` (or any releaser, or a consuming parameter) of a pointer to a
  stack or static object, a string literal, or the middle of an allocation
  (`p + 1`, the result of `strchr`) is an `invalid-release`.
- **Integer facts and guards** ([RFC
  0009](rfcs/0009-value-conditional-behaviour.md)): the checker knows the
  class (`zero`, `positive`, `negative`) and, when it can, the exact value
  of an integer local, parameter or field: from a constant assigned to it,
  from the edges of `if (n == 0)`, `if (n > 0)`, `if (!c)`, `if (n != 3)`
  and every comparison with an integer constant, and from the `case`
  labels of a `switch`. A free, a move, a held resource or a null pointer
  established while such a fact holds is remembered *under* it, and the
  edge of a later test that contradicts the fact drops it: `if (c) free(p);
  ... if (!c) use(p);`, `switch (n) { case 0: free(p); } ... switch (n) {
  case 1: use(p); }` and `char *p = NULL; if (n > 0) p = malloc(n); if (n
  <= 0) return -1; ... free(p);` are clean. A branch whose condition the facts contradict
  (`int c = 0; if (c) free(p);`) is not taken. Reassigning the integer from
  an unknown value forgets the fact and weakens every guard that named it;
  two integers related by arithmetic, or a computed test (`(n & 1) == 0`),
  are not related. Values are mathematical, read in their own type:
  `unsigned x = -1` is `UINT_MAX` (`positive`), an unsigned comparison is
  decided in unsigned order, and a constant no `int64_t` holds (`SIZE_MAX`,
  `ULONG_MAX`) is not one the checker knows, so `if (i > ULONG_MAX)`
  decides nothing and both of its edges stay live.
- **Argument-conditional summaries** ([RFC
  0009](rfcs/0009-value-conditional-behaviour.md)): a callee whose free,
  move, store or returned value depends on a fact about its parameters is
  summarised with a `when` guard (`param 1 freed(free) when param 3 zero`,
  `store param 0 *.msg = copy param 2 when param 2 nonnull`, `return
  fresh(free) when param 3 positive|negative`), on a parameter, a field or
  dereference below one, or a global. At the call the guard is translated
  to the arguments (a constant or `NULL` argument decides it on the spot; a
  variable, a cast of one or `n * 8` is looked up in the caller's facts)
  and the effect is applied only when it is not refuted: `l_alloc(ud, p,
  8, 0)` frees `p` and returns null, `l_alloc(ud, p, 8, 16)` does not free
  and returns a fresh block, `b.noalloc = 1; release(&b); use(b.data)` is
  clean when `release` frees only `if (!b->noalloc)`, and `gz_error(s, err,
  NULL)` stores nothing. What survives translation stays attached to the
  record in the caller, where a later test may still refute it.
- **Inferred `noreturn`** ([RFC
  0009](rfcs/0009-value-conditional-behaviour.md)): a function whose every
  path ends in `abort`, `exit`, `longjmp`, a function declared `noreturn`
  or `_Noreturn`, an infinite loop, or a call to another such function is
  summarised `never-returns`, in the same file or across the program, and
  a call to it ends the path: `if (bad) die("..."); use(p);` is analysed on
  the good path only, nothing is leaked at the end of a block that is never
  left, and code after the call is dead. A function that returns on *some*
  path (`void check(int ok) { if (!ok) die(); }`) returns as far as the
  checker knows, and a call through a function pointer never returns only
  if every candidate never returns. No annotation is needed or added.
- **Shared ownership and reference counts** ([RFC
  0010](rfcs/0010-shared-ownership.md)): a resource on the books has a
  number of *shares*. A function that increments an integer field of its
  argument's object (`o->rc++`, `o->rc += 1`, `__atomic_fetch_add(&o->rc,
  1, m)`, `__sync_add_and_fetch(&o->rc, 1)`, in its own body or a callee's)
  *retains* the argument, and the caller's pointer gains a share. A function
  that frees the object when a decrement of that field reaches zero (`if
  (--o->rc == 0) free(o)`, `o->rc-- == 1`, the atomic and `__sync_` forms,
  or a helper such as `dec_and_test(&o->rc)` whose result says the count is
  zero) *releases a share*: the argument's name is dead, and the object is
  gone only when that was the holder's last share. A copy of a holder with
  a surplus takes one share with it (`b = obj_ref(a); obj_unref(b);
  use(a)` is clean), a copy of a holder with one share shares it (`b = a;
  obj_unref(b); use(a)` is a `use-after-free`), and a plain `free` kills
  every name. Releasing through a name this function never retained (a
  parameter it was handed) is a discipline, not a bug: the name is dead
  afterwards and nothing else changes. A share retained on a local and
  dropped is a `leak` when the field is a *known count*; a share retained
  on a parameter or global is the caller's (`increment param 0 *.rc` in the
  summary). Libraries with no body in view are described with
  `WEAVEC_RETAINS` / `WEAVEC_RELEASES`.
- **Per-outcome stores and facts** ([RFC
  0010](rfcs/0010-shared-ownership.md)): a callee that stores an argument
  only on some outcome classes (`int bag_put(struct bag *b, char *s)`
  returning `-1` when full) is summarised with `stored <class> <path>`
  lines, and on the edge where the caller rules those classes out the store
  is retracted: `if (bag_put(b, s) < 0) free(s);` is clean, and dropping
  `s` on that edge is a `leak`. Likewise what the callee left in the
  caller's integer memory per class (`fact negative param 0 *.filled =0`)
  is known to the caller on the edge it takes.
- **Stores out of sight** ([RFC 0010](rfcs/0010-shared-ownership.md)): a
  callee that copies its argument into memory its summary cannot name (a
  node it allocated and linked into the caller's table: `p->value = value;
  t->first = p;`) is summarised `param 1 escaped`, and the caller treats the
  argument as it treats any value a callee stored a copy of: it has a
  second home, so it is not reported leaked and a share the call gave it is
  not lost. Wrappers pass the effect on, including through `f(obj_ref(v))`
  (a result that is `v` or null resolves to `v`).
- **Derived pointers** ([RFC 0011](rfcs/0011-spatial-safety.md)): a
  pointer is an object and an *offset* into it. `&p->f`, `&p[3]`, `p + 4`,
  `p->payload` (an array member decaying) and `(char *)p -
  offsetof(struct outer, in)` are names for `p`'s object at a known offset,
  so `free(container_of(i, struct outer, in))` frees what `i` was derived
  from, a callee that does so is summarised `freed @-struct outer.in`, a
  field pointer kept across `free(p)` is a `use-after-free` on its next use
  (not a `conflicting-borrow` at the free), and `free(&o->in)` or
  `free(p + 1)` names the offset in its `invalid-release`. Two field steps,
  an element step below a field, or a step by a variable amount make the
  offset unknown, which is still `p`'s object.
- **Extents and bounds** ([RFC 0011](rfcs/0011-spatial-safety.md)): an
  allocation carries its size (`malloc(n)`: `n` bytes; `calloc(n, sz)`;
  `realloc(p, n)`; a wrapper's result: `xmalloc(n)` is summarised `fresh
  extent=n`), a variable and an array member their declared size, a string
  literal its length, and a `WEAVEC_SIZED_BY(n)` parameter `n` elements. An
  access `p[i]`, `*(p + i)`, `(p + i)->f`, or a library call's buffer and
  length (`memcpy`, `memset`, `fgets`, `read`, `snprintf`, ...) is compared
  with the extent: constant against constant outright; a symbolic index
  through what the path knows of it — the same counter (`p[n]` on
  `malloc(n)`), a relation learnt from a condition (`i < n`, `i <= n`, `i >=
  n`, also through one copy `j = i`), or a constant bound (`i < 8`) — and
  a pointer's own offset is added (`p = buf + 4; p[4]` on eight bytes). A
  write to the index or the counter forgets what was known. A callee that
  accesses more of a parameter than its type promises (`b[7]` on `char *b`,
  `for (i = 0; i < n; i++) b[i]`, `for (i = 0; i < 8; i++) b[i]`,
  `memset(b, 0, n)`) is summarised with a *requirement* on that parameter's
  extent (`requires-extent{b: 8}`, `{b: n*4}`), checked at every call
  against what the argument has and passed on through wrappers; an access
  under a condition a summary cannot spell (`if (n > 4) b[4]`) is not
  exported.
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
which hold an owned resource, with its release family, and which are null,
maybe-null or known non-null) and summary of every analysed function; with `--whole-program` it ends with
the program database (every exported summary). `weavec-cc` writes each
unit's exported summaries to `<object>.weavec` in the same text form.

## Controlling diagnostics

`weavec` and `weavec-cc` accept Clang-style warning flags for WeaveC's ids:

| Flag                          | Effect                                                                                            |
| ----------------------------- | ------------------------------------------------------------------------------------------------- |
| `-Wno-weavec-<id>`            | Disable a diagnostic whose default severity is *warning* (`annotation-required`, `invalid-annotation`, `leak`). Refused for an error (including `null-dereference`, `use-of-uninitialized`, `invalid-release` and `out-of-bounds`; lower those with `-Wno-error=`). |
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
