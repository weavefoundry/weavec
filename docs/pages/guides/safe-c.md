---
title: Write safe C
description: Mark C functions WEAVEC_SAFE so weavec-cc checks them against Safe C's rules when they compile, with errors you or an agent can act on, and checks their accesses against their pointers' bounds when they run.
---

`weavec-cc` guards every access of the C it compiles, unchanged. A **safe function** goes further, as Rust's safe code does: `weavec-cc` checks it against a few local rules when it compiles, rejects it with an error naming the rule and the fix when it breaks one, and, once it compiles, checks its accesses against the bounds its pointers carry instead of guarding them. The design is [RFC 0041](/rfcs/0041-safe-c/).

Safe C is plain C. Its annotations come from `weavec.h` and expand to nothing under other compilers, so a converted file still builds anywhere.

## Making functions safe

| How                                                 | Effect                                                         |
| --------------------------------------------------- | -------------------------------------------------------------- |
| `WEAVEC_SAFE` before a function definition          | that function is safe                                          |
| `-fweavec-safe` on the command line                 | every function the unit defines outside system headers is safe |
| `WEAVEC_GUARDED` before a definition in such a unit | that function stays guarded, as all code is without Safe C     |
| `WEAVEC_UNSAFE`, `no_sanitize("address")`           | that function stays unsafe (unguarded), as before              |

A safe function may call guarded functions, and guarded code may call safe functions: convert a program one function at a time. The rules run whenever WeaveC does, under `-fsyntax-only` and `-fweavec-checks=none` too, so the loop is cheap:

```console
$ weavec-cc -fsyntax-only -fweavec-safe list.c
list.c:14:12: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
list.c:14:12: note: give its declaration a bound: 'T *WEAVEC_COUNTED_BY(n) p' (or WEAVEC_SIZED_BY, WEAVEC_ENDED_BY, WEAVEC_NULL_TERMINATED), or index an array or an allocation
```

`-fdiagnostics-format=sarif` gives the same errors as SARIF for tools.

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

Clang's own `counted_by`, `sized_by` and their `_or_null` forms on struct fields (the Linux kernel's `__counted_by`) count as the WeaveC forms.

Write the same bounds on every declaration of a function (its header's and its safe definition's), and give a bound's macro one value in the file: a macro that is redefined or `#undef`'d is a `bad-annotation` error.

Arrays, string literals, the address of a variable, and the result of an allocation (`malloc`, `calloc`, `realloc` and any function declared `alloc_size`) have bounds. A local pointer variable has bounds when everything assigned to it does.

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
  size_t i = 0;
  while (s[i])
    i++;
  return i;
}
```

## The rules

Each error ends with the rule's identifier.

| Rule             | Rejects                                                                                                                                                                                                                      | Write instead                                                                                                                                                                                       |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `index-single`   | indexing or offsetting a pointer to one object: `p[1]`, `p + 2`, `p++` (`p[0]` is `*p`, and is fine)                                                                                                                         | a bound on its declaration, an array, or an allocation                                                                                                                                              |
| `int-to-pointer` | an integer converted to a pointer (`(int *)addr`), other than the constant null                                                                                                                                              | keep the pointer itself                                                                                                                                                                             |
| `pointer-cast`   | a pointer converted to a type that holds pointers from another type (`(struct node *)bytes`, `(struct node *)ctx`), and function pointer conversions                                                                         | convert only between types without pointers, or from an allocation's result (`struct node *n = malloc(sizeof *n)`)                                                                                  |
| `pointer-bytes`  | a writable byte view of an object that holds pointers: `(char *)node`, `memset(node, 1, …)`, `memcpy(node, other_type, …)`                                                                                                   | `*d = *s`, `memcpy` between two objects of the same type, `memset(…, 0, …)`; passing the object to a function that is not safe is fine                                                              |
| `union-pointer`  | reading a union member that holds a pointer                                                                                                                                                                                  | a tag beside the union and a struct per variant                                                                                                                                                     |
| `escape`         | storing a pointer parameter where it outlives the call (a field, a global, an array), returning it, or passing it to an escaping parameter                                                                                   | `WEAVEC_ESCAPES` after the parameter's `*` if the function keeps it; `WEAVEC_LIFETIMEBOUND` after its name if the function returns it; a `WEAVEC_BORROWED` field of a local struct to carry it down |
| `call-args`      | passing a pointer to one object for a parameter that has a bound                                                                                                                                                             | pass a pointer with bounds                                                                                                                                                                          |
| `variadic`       | calling a variadic function the [library table](https://github.com/weavefoundry/weavec/blob/main/lib/Core/LibrarySpec.txt) does not describe (`printf` is described), `va_start`, and `va_arg` of a type that holds pointers | take the arguments by type                                                                                                                                                                          |
| `no-asm`         | inline assembly (an empty `asm volatile("" ::: "memory")` barrier is fine), `setjmp`, `longjmp`, `__builtin_frame_address`, computed `goto`                                                                                  | a guarded helper function                                                                                                                                                                           |
| `unsafe-block`   | a `WEAVEC_UNSAFE` block inside a safe function                                                                                                                                                                               | move the block into a guarded or unsafe function                                                                                                                                                    |
| `bad-annotation` | a bound naming no parameter of the right type                                                                                                                                                                                | name an integer parameter (or a pointer, for `WEAVEC_ENDED_BY`)                                                                                                                                     |
| `cxx`            | a safe function in C++                                                                                                                                                                                                       | Safe C is C only for now                                                                                                                                                                            |

A rule never tries to prove something about the code: it rejects a construct. When code needs something the rules reject, keep that piece in a small `WEAVEC_GUARDED` helper function and call it; the helper's own accesses stay guarded.

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

An escaping parameter must not point into a stack; the program stops with `stack-escape` if a caller passes one, even when the caller's object happens to outlive what the function did with it (a test that links stack nodes into a list and unlinks them before returning): `WEAVEC_ESCAPES` is a promise about the argument, checked when the function is entered. A pointer loaded from memory (`n->next`) is not borrowed.

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

## What runs

- An access through a pointer with bounds is compared against them; outside, the program stops with `pointer-out-of-bounds`. The comparisons are plain code the optimizer hoists out of loops and removes where the loop condition implies them.
- A counted, sized or ended parameter is checked when the function is called from guarded code (or through a pointer, or from another file): its bytes must be addressable, so a guarded caller that overstates a count stops there. A safe caller in the same file that knows the argument's bounds checks the count against them with one comparison instead. A `WEAVEC_NULL_TERMINATED` parameter may be indexed, and its accesses keep their guards (a scan at every call would cost more than it saves).
- An access through a pointer loaded from memory, or returned by a function whose result has no bound, keeps its guard, as in guarded code.
- A use after free in a safe function reads or writes the released block's own bytes, never another object's: the runtime does not reuse a block while a pointer to it exists. It is not reported. `-fweavec-safe-checks=full` keeps every guard in safe functions too, so tests report it as `heap-use-after-free`.

## Converting a module

1. Compile the file with `-fsyntax-only -fweavec-safe` and read the errors; mark functions that need inline assembly or `setjmp` `WEAVEC_GUARDED` first.
2. Give every pointer parameter, result and field that is indexed a bound. Most errors go away here, and new `call-args` errors show the callers that pass pointers without bounds.
3. Mark parameters the function keeps `WEAVEC_ESCAPES` and parameters it returns `WEAVEC_LIFETIMEBOUND`.
4. Replace casts between types that hold pointers with typed code; keep what cannot be typed (a `void *` callback context cast back, a header found by stepping back from a pointer) in a few `WEAVEC_GUARDED` helpers.
5. Run the project's tests. A `pointer-out-of-bounds` report is a bug or a bound that is too small.

What safe functions guarantee, and assume, is listed under [the guarantees](/reference/guarantees/#safe-functions).

## Typed C: safe at full speed

A safe function still checks every pointer it loads from memory against the shadow, because guarded code may have written anything there. That keeps safe C at the cost of guarded C. **`-fweavec-typed`** makes a unit _typed_ ([RFC 0042](/rfcs/0042-typed-memory/)): it is `-fweavec-safe` with a few more rules, and its safe functions, **typed functions**, believe what the declarations of the pointers they load say, as safe Rust believes a slice it loads from a struct. A typed function pays one comparison where an index or a count is known only at run time, and nothing where it is not: no shadow read, no color check.

```console
$ weavec-cc -O2 -fweavec-typed -c vec.c
```

| Tier                     | When it compiles                   | When it runs                                                                                                    |
| ------------------------ | ---------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| Typed (`-fweavec-typed`) | Safe C's rules and the rules below | comparisons against bounds; the shadow only at entries from code that is not typed and where a bound is unknown |
| Safe (`-fweavec-safe`)   | Safe C's rules                     | bounds where the function knows them, guards elsewhere                                                          |
| Guarded                  | none                               | guards                                                                                                          |
| Unsafe (`WEAVEC_UNSAFE`) | none                               | none                                                                                                            |

### What the flag claims

The rules make every store of a typed function keep **typed memory**: each pointer in memory is null or points to what its declaration says (one object of its type, or the elements its count gives, or a string). Code that is not typed, a guarded helper, an unsafe function or a library WeaveC did not compile, is **trusted** to keep it too, as Rust trusts `unsafe` code. A guarded function that stores a short buffer in a counted field with a large count is believed by the typed function that loads it. In return:

- `weavec-cc --weavec-audit <ledger directory>` lists what a program's typed functions trust: every function that is not typed, every access of a typed function that kept a guard and why, and the library calls that kept a checked wrapper. Build with `-fweavec-ledger=<directory>/` first. Read the list as Rust's `unsafe` blocks are read.
- `-fweavec-safe-checks=full` keeps every guard in typed functions, and `-fweavec-checks=verify` keeps each guard a typed function dropped as a monitor that reports `weavec.proven` if it would have failed: run the tests so, and a guarded function that breaks typed memory, or a use after free in typed code, is reported.
- A program with a typed unit records no allocation stacks by default; `WEAVEC_RT_HEAP_STACKS=<frames>` turns them on.

A typed function does not report a use after free: the access reads the released block's own bytes, which stay as the program left them, and the runtime does not reuse a block that something still refers to, or a block that such a block refers to.

### The rules typed functions add

| Rule            | Rejects                                                                                                                                                        | Write instead                                                                                                                                                                    |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `counted-pair`  | a field that counts a pointer field set to a new value (`v->cap *= 2`, `v->len = n`) without the pointer in the next statement, or with a call between the two | assign the pointer and its count in adjacent statements: allocate into a local first, then `v->data = d; v->cap = n;` (`n++`, `n += k`, `n--`, `n -= k`, `n = 0` alone are fine) |
| `string-write`  | a store through a `WEAVEC_NULL_TERMINATED` pointer (or one derived from it), or passing it where the callee may write                                          | take the buffer with its size, `char *WEAVEC_COUNTED_BY(cap) s, size_t cap`, or declare the parameter `const`                                                                    |
| `string-index`  | indexing or offsetting a string pointer by a variable, or moving it back (`s[i]`, `s + n`, `s--`)                                                              | step forward by constants (`s++`, `s[1]`), or measure it first: `size_t n = strlen(s);` with `s` never assigned again                                                            |
| `field-address` | the address of a pointer field leaving the function (`&node->next` passed, returned or stored)                                                                 | pass the struct that holds the field, or copy the pointer into a local, pass that, and assign the field back                                                                     |
| `pointer-cast`  | (tightened) converting memory that holds pointers to anything but `void` or `const` bytes, and converting a `void *` to another type                           | convert an allocation's result where it is made; take the type itself as the parameter; keep a `void *` context in a `WEAVEC_GUARDED` helper                                     |
| `pointer-bytes` | (tightened) copying or zeroing part of an object that holds pointers                                                                                           | copy or zero whole objects: `memset(p, 0, n * sizeof *p)`                                                                                                                        |
| `counted-pair`  | a struct initializer whose constant count exceeds the array its pointer points to                                                                              | give the count the array's number of elements                                                                                                                                    |

```c
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };

int grow(struct vec *v) {
  size_t cap = v->cap ? 2 * v->cap : 8;
  int *d = realloc(v->data, cap * sizeof *d);
  if (d == NULL)
    return -1;
  v->data = d;   // the pair: one comparison checks that d covers cap ints
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

### What runs

- A pointer loaded from a field is bounded by the field's declaration: one object, or the count read beside it (`WEAVEC_ENDED_BY` fields, and counts in another struct, are not used: those loads keep their guards). An access at a constant offset inside one object needs no check at all; an element at an index is one comparison of the index with the count, which the optimizer removes where the loop's own test implies it.
- A store of a pointer into a field is compared with the field's bound (`bounds-conversion` when it does not cover it), and so are arguments of typed functions and the results they return. A count that grows alone (`n++`) is checked against the shadow.
- A string is read at its current byte and stepped forward where a test showed the byte was not zero (the loop's own test in `while (*s) s++;`): no check at all. A step no test covers keeps its guard.
- Calls between typed functions, in one file or another typed file that declares the callee `WEAVEC_SAFE`, go to the callee's body with no check of the shadow; code that is not typed calls the function's entry, which checks each pointer argument against the shadow once.
- Library calls whose accesses are given by sizes (`memcpy`, `memset`, `fread`, …) compare the sizes with their arguments' bounds; a string argument that is a string is taken as it is.

### Notes from conversions

- **Strings in tables.** A table of strings that typed code indexes is cheapest as counted fields, which need no scan: `struct name { const char *WEAVEC_COUNTED_BY(size) text; size_t size; }` initialized `{"GET", sizeof "GET"}`. A string loaded from a plain array of `char *` outside a struct has no known bound and keeps its guard.
- **Steps in branches.** Step a string inside the branch that tested the bytes stepped over (`if (next == '\0') { f++; break; } f += 2;`), not once after the branches join, and test bytes with a comparison (`*p && isxdigit(*p)`), not only with a call: a step no comparison covers keeps its accesses guarded.
- **Cursors in structs.** A pointer loaded from a counted field is bounded from where it points to the end its count gives: it cannot reach back over bytes behind it, even bytes the function just wrote there. Keep the start in a local (or a field of its own) and index forward from it. Read through the pointer before its count shrinks: advance the pointer and decrease the count in adjacent statements after the read (`memcpy(buf, s->next, len); s->next += len; s->avail -= len;`), not `s->avail -= len;` first.
- **A bound's parameter** used nowhere else is unused to Clang (`-Wunused-parameter`); write `(void)n;` under `-Werror`.
- **Public headers.** A header that uses the annotations includes `weavec.h`, which other compilers do not have. Ship a copy with the project, or guard the include: `#if defined(__has_include) && __has_include(<weavec.h>)` / `#include <weavec.h>` / `#else` and empty definitions of the macros you use.
- **The summary line counts entries.** The checks an entry makes for callers that are not typed are counted as `guarded` in the summary line; the audit leaves them out, since they are the boundary, not a cost of typed code.

### Moving a safe project to typed C

1. Compile with `-fsyntax-only -fweavec-typed` and fix the errors above. String helpers that edit in place (trimming, splitting) take a counted buffer; struct initialization allocates into locals first.
2. Give pointer fields the bounds their loads need; a field with no annotation points to one object.
3. Build with `-fweavec-ledger=out/` and read `weavec-cc --weavec-audit out/`: each access that kept a guard says why (`typed-unbounded`: a pointer whose bound the compiler does not know, such as one loaded through `void *` or from an array of pointers outside a struct).
4. Run the tests under `-fweavec-safe-checks=full` and under `-fweavec-checks=verify`, then measure the plain build.

### Writing new code, or porting from another language

Typed C reads like the C an agent would write anyway, with the bounds a Rust port would carry in its types written as annotations:

- **A vector** is a struct with a `WEAVEC_COUNTED_BY(cap)` pointer, its length and its capacity, grown as above.
- **A string builder** is a counted `char` buffer with a length; it is written through the buffer, and handed out as `const char *WEAVEC_NULL_TERMINATED` once terminated. Strings that are only read take `const char *WEAVEC_NULL_TERMINATED`.
- **A sum type** is a tag and one struct pointer per variant, not a union of pointers.
- **A callback** takes the type it needs (`int (*visit)(struct item *, struct state *)`), not a `void *` context; a library that needs `void *` gets a `WEAVEC_GUARDED` adapter of a few lines.
- **Ownership** is not checked when the program compiles: release each block once. A double free stops the program (`double-free`), and a use after free in typed code reads only the released block.
