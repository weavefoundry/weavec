---
title: Write safe C
description: Mark C functions WEAVEC_SAFE, or a whole unit -fweavec-safe, so weavec-cc checks their bounds, typed memory and ownership when they compile, with errors you or an agent can act on, and checks their accesses against their pointers' bounds when they run.
---

`weavec-cc` guards every access of the C it compiles, unchanged. A **safe function** goes further, as Rust's safe code does: `weavec-cc` checks it against a few local rules when it compiles, rejects it with an error naming the rule and the fix when it breaks one, and, once it compiles, checks its accesses against the bounds its pointers declare instead of guarding them. The rules cover three things: **bounds** (a pointer says how many elements it points to), **typed memory** (every pointer stored in memory keeps the bound its declaration states, so a safe function can believe a pointer it loads) and **ownership** (one pointer releases each block, once, and nothing uses it afterwards). The design is [RFC 0041](/rfcs/0041-safe-c/) (bounds), [RFC 0042](/rfcs/0042-typed-memory/) (typed memory) and [RFC 0043](/rfcs/0043-ownership/) (ownership).

Safe C is plain C. Its annotations come from `weavec.h` and expand to nothing under other compilers, so a converted file still builds anywhere.

## Making functions safe

| How                                                 | Effect                                                         |
| --------------------------------------------------- | -------------------------------------------------------------- |
| `-fweavec-safe` on the command line                 | every function the unit defines outside system headers is safe |
| `WEAVEC_SAFE` before a function definition          | that function is safe                                          |
| `WEAVEC_GUARDED` before a definition in a safe unit | that function stays guarded, as all code is without Safe C     |
| `WEAVEC_UNSAFE`, `no_sanitize("address")`           | that function stays unsafe (unguarded), as before              |

There is one kind of safe function. `WEAVEC_SAFE` on a definition in a unit compiled without the flag makes that function safe in the same sense, and it trusts the rest of its unit as it trusts any code that is not safe ([typed memory](#typed-memory)). `-fweavec-typed` was removed: `-fweavec-safe` now means what it meant, with ownership added.

A safe function may call guarded functions, and guarded code may call safe functions: convert a program one function at a time. The rules run whenever WeaveC does, under `-fsyntax-only` and `-fweavec-checks=none` too, so the loop is cheap:

```console
$ weavec-cc -fsyntax-only -fweavec-safe item.c
item.c:13:10: error: 'it' is used after it was moved to 'item_free' [weavec-safe:owner-moved]
item.c:12:3: note: moved here; use it before this, or keep what you need from it in a local first
item.c:13:10: note: 'weavec-cc --weavec-explain owner-moved' explains this rule
```

The first error of a compile names the command that explains its rule: `weavec-cc --weavec-explain <rule>` prints what the rule rejects, why, a rejected example and its accepted rewrite, and `weavec-cc --weavec-explain all` prints every rule, the brief to read once before a conversion. `-fdiagnostics-format=sarif` gives the same errors as SARIF for tools.

## Bounds

A pointer in a safe function points to **one object** unless its declaration says how many. The annotations go after the `*` of a pointer type, on parameters, results, fields and globals:

| Annotation                                                   | The pointer points to                                                                                  |
| ------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| none                                                         | one object of its type                                                                                 |
| `WEAVEC_COUNTED_BY(n)`                                       | `n` elements; `n` names an integer parameter, or is an integer constant or a macro that expands to one |
| `WEAVEC_SIZED_BY(n)`                                         | `n` bytes                                                                                              |
| `WEAVEC_ENDED_BY(e)`                                         | the elements before `e`, another pointer parameter                                                     |
| `WEAVEC_NULL_TERMINATED`                                     | a string, up to and with its NUL                                                                       |
| `WEAVEC_NULL_TERMINATED_OR_NULL`                             | a string, or null                                                                                      |
| `WEAVEC_COUNTED_BY_OR_NULL(n)`, `WEAVEC_SIZED_BY_OR_NULL(n)` | as above, or null whatever `n` is                                                                      |

On a field, a count names a sibling field. Clang's own `counted_by`, `sized_by` and their `_or_null` forms on struct fields (the Linux kernel's `__counted_by`) count as the WeaveC forms.

Write the same bounds on every declaration of a function (its header's and its safe definition's), and give a bound's macro one value in the file: a macro that is redefined or `#undef`'d is a `bad-annotation` error.

Arrays, string literals, the address of a variable, and the result of an allocation (`malloc`, `calloc`, `realloc` and any function declared `alloc_size`) have bounds. A local pointer variable has bounds when everything assigned to it does. A string is read at its current byte and stepped forward; an index into it needs its length first (`size_t n = strlen(s);`).

```c
#include <weavec.h>
#include <stddef.h>

WEAVEC_SAFE int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  int s = 0;
  for (const int *p = v; p < v + n; p++)
    s += *p;
  return s;
}

WEAVEC_SAFE size_t length(const char *WEAVEC_NULL_TERMINATED s) {
  size_t n = 0;
  while (*s) {
    s++;
    n++;
  }
  return n;
}
```

### Borrowed and escaping pointers

A safe function's pointer parameters are **borrowed** for the call: it reads and writes through them, keeps them in locals and passes them on, but does not keep them after it returns.

```c
struct node { struct node *next; int value; };
struct list { struct node *head; };

WEAVEC_SAFE void push(struct list *l, struct node *WEAVEC_ESCAPES n) {
  n->next = l->head; // l->head keeps n: n must be WEAVEC_ESCAPES
  l->head = n;
}

WEAVEC_SAFE struct node *second(struct node *n WEAVEC_LIFETIMEBOUND) {
  return n->next ? n->next : n; // n itself is returned: lifetimebound
}
```

An escaping parameter must not point into a stack; the program stops with `stack-escape` if a caller passes one, even when the caller's object happens to outlive what the function did with it (a test that links stack nodes into a list and unlinks them before returning): `WEAVEC_ESCAPES` is a promise about the argument, checked when the function is entered. A `WEAVEC_OWNED` parameter counts as escaping too. The fields `next` and `head` above are [references](#references): they keep a pointer the function does not own.

A parser that keeps its input in a cursor struct needs a struct that holds a borrowed pointer. Mark the field `WEAVEC_BORROWED`: a borrowed pointer may then be stored in it when the struct is a local variable (`buffer.content = text`, not through a pointer), a pointer loaded from it is borrowed, and the local's address is borrowed too, so it can be passed down but not kept.

```c
typedef struct {
  const char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(length) content;
  size_t length;
  size_t offset;
} cursor;

WEAVEC_SAFE int next(cursor *c) {
  return c->offset < c->length ? c->content[c->offset++] : -1;
}

WEAVEC_SAFE int first(const char *WEAVEC_COUNTED_BY(n) text, size_t n) {
  cursor c = {text, n, 0};
  return next(&c);
}
```

### Locals whose address escapes

Taking a local's address is never an error. When the address may outlive the local's scope (it is stored in memory, returned, passed to an escaping parameter, or used after the scope ends), `weavec-cc` allocates the local from the runtime's heap for its scope instead of the stack, so a pointer that outlives it reaches only its own bytes. `-Rpass=weavec-safe` lists those locals; move one off a hot path if its allocation costs.

## Typed memory

A safe function believes what the declarations of the pointers it loads say, as safe Rust believes a slice it loads from a struct. It pays one comparison where an index or a count is known only at run time, and nothing where it is not: no shadow read, no color check. A pointer loaded from a field is bounded by the field's declaration: one object when it has no bound, or the elements its count gives.

```c
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };

int grow(struct vec *v) {
  size_t cap = v->cap ? 2 * v->cap : 8;
  int *d = realloc(v->data, cap * sizeof *d);
  if (d == NULL)
    return -1;    // v->data still owns its block
  v->data = d;    // the pair: one comparison checks that d covers cap ints
  v->cap = cap;
  return 0;
}

int push(struct vec *v, int x) {
  if (v->len == v->cap && grow(v))
    return -1;
  v->data[v->len++] = x;   // one comparison with v->cap, no shadow read
  return 0;
}
```

### What the flag claims

The rules make every store of a safe function keep **typed memory**: each pointer in memory is null or points to what its declaration says (one object of its type, or the elements its count gives, or a string), and each owner holds a block no other owner holds. Code that is not safe, a guarded helper, an unsafe function or a library WeaveC did not compile, is **trusted** to keep it too, as Rust trusts `unsafe` code. A guarded function that stores a short buffer in a counted field with a large count is believed by the safe function that loads it. In return:

- `weavec-cc --weavec-audit <ledger directory>` lists what a program's safe functions trust (see [testing](#testing)). Read the list as Rust's `unsafe` blocks are read.
- `-fweavec-safe-checks=full` keeps every guard in safe functions, and `-fweavec-checks=verify` keeps each guard a safe function dropped as a monitor that reports `weavec.proven` if it would have failed: run the tests so, and a guarded function that breaks typed memory, or a use after free in safe code, is reported.
- A program with a safe unit records no allocation stacks by default; `WEAVEC_RT_HEAP_STACKS=<frames>` turns them on.

A use after free that the rules do not reject (through a pointer a safe function loaded before its block was released under it) reads the released block's own bytes, which stay as the program left them: the runtime does not reuse a block that something still refers to, or a block that such a block refers to. In the fast mode, the default, there is one exception: a block a safe function released is zeroed and reused at once by the next allocation of its own type (see [what runs](#what-runs)).

An allocation that becomes a `T *` is sized by what it holds: `malloc(sizeof *p)`, `calloc(n, sizeof *p)`, `malloc(sizeof(struct buf) + n)` for a struct with a flexible array. `struct node *n = malloc(sizeof(struct wide))` is `pointer-cast`, since the block is reused for the type its size names; a byte buffer may be sized by any type without pointers.

## Ownership

In a safe function every pointer to heap memory is one of three things:

- An **owner** is responsible for releasing its block. It is a place declared `WEAVEC_OWNED` (a field, a parameter, a result, a local, a global, an array's element) or a local that receives an owner: the result of a call that returns one (`malloc`, `strdup`, a function whose result is `WEAVEC_OWNED`), or what a call such as `getline` stores through the local's address.
- A **borrow** is a pointer the function took from an owner or from a parameter and keeps in a local or passes down. The checker decides when the function compiles whether each use of it is still valid.
- A **reference** is a pointer loaded from a field that is neither `WEAVEC_OWNED` nor `WEAVEC_BORROWED` (a back pointer, a cursor into someone else's buffer, a cache entry), from a global, or from a borrow of one. No rule follows it; it is tested when it is loaded.

`WEAVEC_OWNED` goes after the `*`, like the bounds. A `WEAVEC_OWNED` result gives the caller a block to release; a `WEAVEC_OWNED` parameter takes the caller's block, and the function must release it or keep it in an owner; a parameter without it is borrowed for the call. An owner field is released, or moved out, before the struct that holds it. Null is an owner that owns nothing: `free(NULL)` consumes it, and a test that shows an owner null on a path means it holds nothing there. Fresh blocks and locals that hold pointers are zero, so their owners start null. The library table makes owners of the allocation family's results (`malloc`, `calloc`, `realloc`, `aligned_alloc`, `strdup`, `strndup` and their kin), of what `asprintf`, `posix_memalign`, `getline` and `getdelim` store through their first argument, and releases owners passed to `free`.

### A constructor and a destructor

A constructor allocates into a local, fills it and returns it. On a failure halfway it releases what it built: since every owner at the label is null or holds its block, one `goto fail` covers every path. A destructor takes its argument `WEAVEC_OWNED` and releases each owner field before the struct; forgetting one is `owner-leak`. A struct the caller keeps by value (a local `struct vec v`, a field) has a destructor that takes it `WEAVEC_DROPS` instead: it releases the owners below its parameter and leaves them null, which the compiler checks at each of its returns. After a call that may change a struct's owners (one given a pointer to non-`const` memory that holds owners), they may hold blocks, so the caller must reach such a destructor on every path, error paths included; after the destructor they are null.

```c
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct item {
  char *WEAVEC_OWNED WEAVEC_NULL_TERMINATED name;
  int v;
};

char *WEAVEC_OWNED WEAVEC_NULL_TERMINATED copy_string(const char *WEAVEC_NULL_TERMINATED s) {
  size_t n = strlen(s) + 1;
  char *c = malloc(n);
  if (c == NULL)
    return NULL;
  memcpy(c, s, n);
  return c;
}

void item_free(struct item *WEAVEC_OWNED it) {
  if (it == NULL)
    return;
  free(it->name);
  free(it);
}

struct item *WEAVEC_OWNED item_new(const char *WEAVEC_NULL_TERMINATED name, int v) {
  struct item *it = calloc(1, sizeof *it);
  if (it == NULL)
    goto fail;
  it->name = copy_string(name);
  if (it->name == NULL)
    goto fail;
  it->v = v;
  return it;
fail:
  item_free(it);
  return NULL;
}
```

`strdup`'s result owns its block but has no bound, so it fits only an owner without one (`char *WEAVEC_OWNED name`); a `WEAVEC_NULL_TERMINATED` or counted field takes a copy made as `copy_string` makes it.

### A setter

A setter releases the old block before it stores the new one; storing over an owner that may still hold a block is `owner-leak`, the bug it usually is. Make the new block first, so that a failure leaves the struct as it was:

```c
int item_rename(struct item *it, const char *WEAVEC_NULL_TERMINATED name) {
  char *copy = copy_string(name);
  if (copy == NULL)
    return -1;
  free(it->name);
  it->name = copy;
  return 0;
}
```

To keep a block the caller made instead of a copy, take the parameter `char *WEAVEC_OWNED name`: storing a borrowed parameter in an owner is `owner-source`.

### Growing with `realloc`

`d = realloc(p, n)` consumes `p` only when `d` is not null. Assign the result to a new local, test it, then store it: on the null branch `p` still owns its block. `p = realloc(p, n)` is `owner-leak` (on failure the block is lost), and using `p` before `d` was tested is `owner-moved`. The pointer and its count are then assigned in adjacent statements (`counted-pair`), as `grow` above does. A vector of owners grows the same way:

```c
struct items {
  struct item *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data;
  size_t len, cap;
};

int items_push(struct items *v, struct item *WEAVEC_OWNED it) {
  if (v->len == v->cap) {
    size_t cap = v->cap ? 2 * v->cap : 8;
    struct item **d = realloc(v->data, cap * sizeof *d);
    if (d == NULL) {
      item_free(it); // it was given to this function: release it or keep it
      return -1;
    }
    v->data = d;
    v->cap = cap;
  }
  v->data[v->len++] = it;
  return 0;
}
```

A pointer taken into `v->data` before the `realloc` (or before a call given `v` that could reallocate it) cannot be used after it: that is `stale-borrow`. Take it again afterwards, or keep an index.

### Moving out of a slot: pop, take and clear

An owner the function does not name exactly (an element at an index, `*pp`) is moved out of only in a statement followed at once by an assignment to the same place (`owner-slot`), so that no owner keeps a pointer it gave away:

```c
struct item *WEAVEC_OWNED items_pop(struct items *v) {
  if (v->len == 0)
    return NULL;
  struct item *WEAVEC_OWNED it = v->data[--v->len];
  v->data[v->len] = NULL;
  return it;
}

struct item *WEAVEC_OWNED items_take(struct items *v, size_t i) {
  if (i >= v->len)
    return NULL;
  struct item *WEAVEC_OWNED it = v->data[i];
  v->data[i] = NULL;
  return it;
}

void items_clear(struct items *v) {
  while (v->len > 0)
    item_free(items_pop(v));
  free(v->data);
  v->data = NULL;
  v->cap = 0;
}
```

A local that takes a block out of a place says so: `struct item *WEAVEC_OWNED it = v->data[i];` moves, while `struct item *it = v->data[i];` borrows. `free(v->data); v->data = NULL;` moves an owner field out of a struct the caller owns and puts null back before the function returns, as `owner-moved` requires.

### What else the rules follow

- **Calls.** A call given a pointer to non-`const` memory that holds owners may change those owners: it ends the borrows taken below them, and the caller leaves them to the callee (it does not check them for leaks afterwards; the callee's own checks make it leave none moved). A callee that only reads takes `const`, and its callers' borrows survive the call. In one call, an argument the callee could change owners through does not overlap another argument (`call-alias`).
- **The path that owns.** An owner is moved, released or replaced only from a local owner, or from a parameter that is not a pointer to `const`, through owners (`owner-path`). Not through a reference: unlinking a node of a doubly linked list through its `prev` pointer is rejected; walk to it from the head. A reference is not passed where the callee could change the owners below it.
- **Globals.** A global owner is moved into a local, used, and moved back (`struct state *WEAVEC_OWNED s = g; g = NULL;`). Read in place, what it points to is a reference; the audit lists the global owners read in place.
- **Structs.** Copying a struct that holds owners moves them (`*a = *b`); `memcpy` between two such structs is `owner-source`.
- **Function pointers** carry the annotations in their type: an allocation hook is `void *WEAVEC_OWNED (*allocate)(size_t)` and its release `void (*release)(void *WEAVEC_OWNED)`.
- **Code that is not safe** is believed: a declaration without annotations borrows its parameters and returns references, as C that knows nothing of owners does.

### References

A pointer field that is neither `WEAVEC_OWNED` nor `WEAVEC_BORROWED` is a reference. Storing one is always allowed (`c->last = it` leaves `it` the owner). In the checked build (`-fweavec-safe-checks=full`, the default without optimization), when a safe function loads a reference and uses it for more than a comparison or a copy into memory, one shadow byte of the address it holds is tested: if it points into a released heap block, the program stops (report mode reports it once and goes on):

```text
weavec: heap-use-after-free at cache.c:12:41: reference to 0xba80000000 loaded from a field
```

followed by the stack and the released block's paragraph. The test is exact for the heap, since the runtime does not reuse a block while something refers to it. A null pointer, a pointer one past a live block and a pointer into memory the runtime does not track pass. A program that keeps a dangling reference and passes it on (to a function that only uses it as a key) stops too: clear the reference when its block is released, or keep the key as an integer. The ledger records the test with the reason `reference`, and the audit lists the reference fields. The fast build tests no reference, as Safe C++'s does not ([RFC 0055](/rfcs/0055-a-heap-for-safe-code/) §2): a released block of a known type is the next block of that type at once, so a dangling reference reads the zeroed object, or the next object of its type, and never another type. Test with the checked build.

## The rules

Each error ends with `[weavec-safe:<rule>]`. The [rules reference](/reference/rules/) gives each one's rejected example and its rewrite; `weavec-cc --weavec-explain <rule>` prints the same section.

| Rule             | Rejects                                                                                                                                                 |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `index-single`   | indexing or offsetting a pointer to one object (`p[1]`, `p + 2`, `p++`); `p[0]` is `*p`                                                                 |
| `int-to-pointer` | an integer converted to a pointer, other than the constant null                                                                                         |
| `pointer-cast`   | a conversion that could forge a pointer and is not one the runtime checks (a `void *` to a struct, a struct to one that begins with it, `container_of`) |
| `pointer-bytes`  | writing an object that holds pointers as bytes, or copying or zeroing part of one                                                                       |
| `union-pointer`  | reading a member that holds a pointer of a union without a tag; a tagged union's member's address, or the union apart from its struct                   |
| `escape`         | keeping a pointer parameter after the call (stored in memory, returned) unless it is `WEAVEC_ESCAPES` or `WEAVEC_LIFETIMEBOUND`                         |
| `call-args`      | a pointer to one object passed, stored or returned where a declaration gives a bound, or given to a library call that may access more                   |
| `variadic`       | a variadic call the [library table](https://github.com/weavefoundry/weavec/blob/main/lib/Core/LibrarySpec.txt) does not describe                        |
| `no-asm`         | inline assembly, `setjmp`, `longjmp`, `__builtin_frame_address`, computed `goto`                                                                        |
| `unsafe-block`   | a `WEAVEC_UNSAFE` block inside a safe function                                                                                                          |
| `bad-annotation` | a bound that names nothing of the right type, or differs between declarations                                                                           |
| `cxx`            | a safe function in C++                                                                                                                                  |
| `counted-pair`   | a count changed without its pointer in the adjacent statement (other than a step), or a struct initializer whose count exceeds its array                |
| `string-index`   | a string indexed or offset by a variable, or stepped back, before its length is known                                                                   |
| `string-write`   | a store through a `WEAVEC_NULL_TERMINATED` pointer, or passing it where the callee may write                                                            |
| `field-address`  | the address of a pointer field leaving the function                                                                                                     |
| `owner-moved`    | an owner used after it was moved or released (a second release included), or before the `realloc` that took it was tested                               |
| `owner-leak`     | an owner dropped while it may still hold a block: at a return, at the end of its scope, when assigned over, when its struct is released                 |
| `owner-source`   | something other than an owner's value stored in an owner (a borrow, a literal, an address, arithmetic), and an owner stepped                            |
| `owner-path`     | an owner moved, released or replaced through a reference, a pointer to `const` or a global read in place                                                |
| `owner-slot`     | an element or `*pp` moved out of or released without an assignment to the same place in the next statement                                              |
| `stale-borrow`   | a borrow used after the owner it was taken below was moved, released or replaced, by the function or by a call that could                               |
| `call-alias`     | one call given a pointer it could change owners through and another argument above, below or equal to it                                                |

A rule never tries to prove something about the code: it rejects a construct. When code needs something the rules reject, keep that piece in a small `WEAVEC_GUARDED` helper function and call it; the helper's own accesses stay guarded, and the audit lists it.

## What runs

- An access through a pointer with bounds is compared against them; outside, the program stops with `pointer-out-of-bounds`. A pointer loaded from a field is bounded by the field's declaration (`WEAVEC_ENDED_BY` fields, and counts in another struct, are not used: those loads keep their guards). An access at a constant offset inside one object needs no check; an element at an index is one comparison of the index with the count, which the optimizer hoists out of loops and removes where the loop's own test implies it.
- A store of a pointer into a field is compared with the field's bound (`bounds-conversion` when it does not cover it), and so are arguments of safe functions and the results they return. A count that grows alone (`n++`) is checked against the shadow.
- A string is read at its current byte and stepped forward where a test showed the byte was not zero (the loop's own test in `while (*s) s++;`): no check at all. In a fast unit (RFC 0048) a step no test covers (`p += 4` after `strncmp(p, "<!--", 4)`) tests the bytes it steps over where it is made, a load of each up to the terminator, and is then a string position too: it is handed on and returned with no search, and a read-only library call on strings the function holds (`strncmp`, `strlen`) calls the C library itself. A string that code which is not safe hands in is taken in constant time when it lies in a live heap block.
- Calls between safe functions, in one file or in another file that declares the callee `WEAVEC_SAFE`, go to the callee's body with no check of the shadow. Code that is not safe calls the function's entry, which checks each pointer argument against the shadow once, a bounded one over its bounds; an escaping or owned argument must not point into a stack (`stack-escape`, reported at the call). In a fast unit a local placed on the heap takes a block of its type's pool (RFC 0048 §5.1).
- Library calls whose accesses are given by sizes (`memcpy`, `memset`, `fread`, …) compare the sizes with their arguments' bounds. A pointer to one object given to one with a size that may be more than the object is `call-args` when the function compiles (RFC 0050 §5.1), not a stop in a test: declare it with its count.
- In the checked build, a reference loaded from a field and used is tested not to point into a released block (`heap-use-after-free`).
- An access through a pointer whose bound is unknown (loaded from a place no declaration bounds, such as `*pp` or an array of pointers outside a struct, or through `void *`) keeps its guard.

- In the fast mode (`-fweavec-safe` with optimization, or `-fweavec-safe-checks=bounds`; without optimization the unit is checked, RFC 0048) every allocation whose type the compiler can tell (`malloc(sizeof *p)`, `calloc(n, sizeof *p)`) carries it, and a block a safe function releases is zeroed and handed to the next allocation of the same type and size, at once, instead of waiting in the quarantine for a sweep. A stale pointer to it then reads an empty object, or the object of its type that lives there; it never reads another type. A `realloc` keeps its block's type: the block it gives comes from the pool of the old block's type, whatever type the call's site infers ([RFC 0055](/rfcs/0055-a-heap-for-safe-code/) §3). Releases by code that is not safe, and blocks of no known type, still wait for a sweep. Each thread keeps the free blocks of each type it uses, so an allocation and its release take a few loads and stores and no lock.

The ownership rules are checked only when the program compiles. In the checked mode (`-fweavec-safe-checks=full`) nothing at run time rests on them: every released block waits in the quarantine for a sweep, and no guard is removed because of them.

### What safe code can rely on

A safe function has no behavior C leaves undefined ([RFC 0051](/rfcs/0051-nothing-undefined/); [the list](/reference/undefined/) has each of the 221 items of C23's Annex J.2 and what disposes of it). Signed arithmetic wraps, a shift's count is taken modulo the width, a conversion from floating point saturates, every local starts as zeroes and a loop may run forever. A division by zero or of the least value by -1, `__builtin_unreachable()` reached, the end of a function that must return a value reached, and a full stack stop with a report (`division-by-zero`, `division-overflow`, `unreachable`, `missing-return`, `stack-exhaustion`). `restrict`, `__builtin_assume` and the attributes that assert are errors ([`assumed`](/reference/rules/#assumed)); bytes do not become a `bool`; a signal handler is registered by a `WEAVEC_GUARDED` function ([`signal-handler`](/reference/rules/#signal-handler)); a call through a declaration without a prototype, a format that does not match, and unsequenced changes are Clang's errors; and two safe units that declare one function or global two ways do not link ([`definition-mismatch`](/reference/rules/#definition-mismatch)).

## One of several types

A value that is one of several types is written as C writes it, and checked where the type is chosen ([RFC 0054](/rfcs/0054-one-of-several-types/)): each check reads a tag or the runtime's record of an object's type when it runs, in both builds, so no rule has to follow how the value got there.

**A tagged union.** The union says which field of its struct is its tag, and each member which values of the tag it is for. A member is used after a comparison of the tag, which the compiler adds and the optimizer removes where the program already made it; a store of a new value to the tag clears the union; and the address of a member is not taken, so no pointer into a member outlives its check.

```c
enum kind { KIND_NUMBER, KIND_TEXT, KIND_LIST };
struct value {
  enum kind kind;
  union {
    double number WEAVEC_WHEN(KIND_NUMBER);
    char *WEAVEC_OWNED WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
    struct list *WEAVEC_OWNED items WEAVEC_WHEN(KIND_LIST);
  } as WEAVEC_TAGGED_BY(kind);
};

double as_number(const struct value *v) {
  switch (v->kind) {
  case KIND_NUMBER: return v->as.number;
  case KIND_TEXT:   return strtod(v->as.text, NULL);
  default:          return 0;
  }
}
```

A member read under the wrong tag stops: `weavec: union-member at value.c:14:12: 'text' read while 'kind' is 0`. Set the tag before the member (the compiler moves a store of the tag that comes after its member, a certain fix), copy the struct rather than the union, and release an owned member before the tag changes: the store of the tag clears it (`owner-leak` otherwise). `WEAVEC_TAGGED_BY(tt, 0x3f)` compares the tag after a mask, for a tag that keeps other bits.

**A struct that begins with another.** An object header and the objects that begin with it convert both ways: up with no check, down with a check that an object of the derived type is there.

```c
struct object { int type; struct object *next; };
struct string { struct object header; size_t length; char *WEAVEC_OWNED WEAVEC_COUNTED_BY(length) bytes; };

size_t length_of(struct object *o) {
  return o->type == TYPE_STRING ? ((struct string *)o)->length : 0;
}
```

**Contexts and containers of `void *`.** `struct server *s = context;` asks the runtime whether a `struct server` is at `context`; a wrong one stops with `bad-cast`, which names the type of the block that is there. `container_of` (a pointer to a member, less the member's offset) is checked the same way. Every block of the runtime's heap knows the type its allocation named, and so does every global and every local of a safe function whose address becomes a `void *`; memory with no record is believed, and `WEAVEC_RT_STATS=1` counts those conversions.

## Converting a module

1. Compile the file with `-fsyntax-only -fweavec-safe` and read the errors (`--weavec-explain` for any rule that is new to you); mark functions that need inline assembly or `setjmp` `WEAVEC_GUARDED` first.
2. Let the compiler write what it can: `weavec-cc --weavec-fix -- <the file's compile command>` gives indexed pointers the bounds their uses agree on (a `memcpy`'s count, a loop's comparison, a byte tested against zero), marks the parameters a function keeps `WEAVEC_ESCAPES` and the ones it returns `WEAVEC_LIFETIMEBOUND`, runs the rules again and repeats ([`fix`](/reference/rules/#fix), RFC 0049). Each error it cannot fix says in its note what to write. For a whole project, survey the build (`-fweavec-safe-survey -fweavec-ledger=ledger/`) and give `--weavec-fix` the ledger directory.
3. Give the pointers the fixer left a bound, and new `call-args` errors show the callers that pass pointers without bounds.
4. Declare the owners: each pointer field that owns its block, each function that returns a block for the caller to release (its result), and each one that takes a block from its caller (its parameter) `WEAVEC_OWNED`. Annotate the function pointer types of allocation hooks too. Pointer fields that do not own stay references. Then fix the ownership errors: a release on each early return, a destructor that releases each owner field, `realloc` into a local, `NULL` after a release from a struct the caller owns, `const` on parameters a callee only reads.
5. Say each union's tag (`--weavec-fix` writes `WEAVEC_TAGGED_BY` and `WEAVEC_WHEN` when every read of the union's members is under a test of one field) and keep the conversions as they are: a `void *` callback context, a struct taken as the struct that begins with it and `container_of` are checked when they run. Keep what cannot be typed (a release through a back pointer, ownership that depends on a flag) in a few `WEAVEC_GUARDED` helpers.
6. String helpers that edit in place (trimming, splitting) take a counted buffer; struct initialization allocates into locals first.
7. [Test](#testing) the result.

An `owner-leak` or `owner-moved` error in code that used to compile is often a real leak or a real double release on an error path; read it before rewriting around it.

## Writing new code, or porting from another language

Safe C reads like the C an agent would write anyway, with the bounds and the ownership a Rust port would carry in its types written as annotations:

- **A vector of values** is a struct with a `WEAVEC_OWNED WEAVEC_COUNTED_BY(cap)` pointer, its length and its capacity, grown as `grow` above.
- **A vector of owners** is `T *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap)`: push takes the element `WEAVEC_OWNED`, pop and take move out with `x = v->data[i]; v->data[i] = NULL;`, and swap-remove moves the last element into the hole the same way.
- **A string builder** is an owned counted `char` buffer with a length; it is written through the buffer, and handed out as `const char *WEAVEC_NULL_TERMINATED` once terminated. Strings that are only read take `const char *WEAVEC_NULL_TERMINATED`.
- **A tree** owns its children (`struct tree *WEAVEC_OWNED left, *WEAVEC_OWNED right;`) and refers to its parent (`struct tree *parent;`, a reference); the function that links a child to `t` takes `t` `WEAVEC_ESCAPES`.
- **A singly linked list** owns each next node and is edited through `struct node *WEAVEC_OWNED *pp`: `struct node *WEAVEC_OWNED n = *pp; *pp = n->next;` unlinks a node.
- **A hash map** owns its bucket array and each bucket's chain; **an LRU cache** keeps its entries owned by the map and orders them with references.
- **A sum type** (Rust's `enum`) is a struct with a tag and a union that says it: `WEAVEC_TAGGED_BY(kind)` on the union, `WEAVEC_WHEN(KIND_TEXT)` on each member; a `switch` on the tag is `match` ([one of several types](#one-of-several-types)).
- **A callback** takes the type it needs (`int (*visit)(struct item *, struct state *)`) or, where a library hands back a `void *` context, converts it back: `struct server *s = context;` is checked when it runs.

```c
struct node { struct node *WEAVEC_OWNED next; int v; };

void list_remove(struct node *WEAVEC_OWNED *head, int v) {
  struct node *WEAVEC_OWNED *pp = head;
  while (*pp != NULL) {
    if ((*pp)->v == v) {
      struct node *WEAVEC_OWNED n = *pp;
      *pp = n->next;
      n->next = NULL;
      free(n);
      return;
    }
    pp = &(*pp)->next;
  }
}

void list_free(struct node *WEAVEC_OWNED head) {
  for (;;) {
    if (head == NULL)
      return;
    struct node *WEAVEC_OWNED next = head->next;
    head->next = NULL;
    free(head);
    head = next;
  }
}
```

### Notes from conversions

- **Strings in tables.** A table of strings that safe code indexes is cheapest as counted fields, which need no scan: `struct name { const char *WEAVEC_COUNTED_BY(size) text; size_t size; }` initialized `{"GET", sizeof "GET"}`. A string loaded from a plain array of `char *` outside a struct has no known bound and keeps its guard.
- **Steps in branches.** Step a string inside the branch that tested the bytes stepped over (`if (next == '\0') { f++; break; } f += 2;`), not once after the branches join, and test bytes with a comparison (`*p && isxdigit(*p)`), not only with a call: a step no comparison covers keeps its accesses guarded.
- **Cursors in structs.** A pointer loaded from a counted field is bounded from where it points to the end its count gives: it cannot reach back over bytes behind it, even bytes the function just wrote there. Keep the start in a local (or a field of its own) and index forward from it. Read through the pointer before its count shrinks: advance the pointer and decrease the count in adjacent statements after the read (`memcpy(buf, s->next, len); s->next += len; s->avail -= len;`), not `s->avail -= len;` first.
- **A bound's parameter** used nowhere else is unused to Clang (`-Wunused-parameter`); write `(void)n;` under `-Werror`.
- **Public headers.** A header that uses the annotations includes `weavec.h`, which other compilers do not have. Ship a copy with the project, or guard the include: `#if defined(__has_include) && __has_include(<weavec.h>)` / `#include <weavec.h>` / `#else` and empty definitions of the macros you use.
- **The summary line counts entries.** The checks an entry makes for callers that are not safe are counted as `guarded` in the summary line; the audit leaves them out, since they are the boundary, not a cost of safe code.

## Testing

A converted module is tested in three builds:

1. **`-fweavec-safe-checks=full`** (the default of a build without optimization) keeps every guard in safe functions as well as their comparisons, and every released block waits for a sweep: a use after free the rules did not reject, or a guarded helper that broke typed memory, is reported as in guarded code, with its allocation and release stacks.
2. **`-fweavec-checks=verify`** keeps each guard a safe function dropped as a monitor: a report that starts `weavec: weavec.proven:` means code that is not safe broke what safe code believes, or a rule has a hole.
3. **The plain build** (the fast mode), which is the one to measure and to ship. A heap report from it ends with a note that the program was built for speed.

Then build with `-fweavec-ledger=out/` and read `weavec-cc --weavec-audit out/`. It prints how many functions are safe (`<k> of <f> functions safe`) and lists what they trust: the functions that are not safe (with `, owns` after the tier when one has a `WEAVEC_OWNED` parameter or result, which safe code believes), the accesses of safe functions that keep a guard and why (`typed-unbounded`: a pointer whose bound the compiler does not know), the library calls that keep a checked wrapper, the reference fields, and the global owners read in place. `--format=json` writes it for tools. What safe functions guarantee, and assume, is listed under [the guarantees](/reference/guarantees/#safe-functions).
