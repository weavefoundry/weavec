---
title: Write safe C
description: Mark C functions WEAVEC_SAFE so weavec-cc checks them against Safe C's rules when they compile, with errors you or an agent can act on, and checks their accesses against their pointers' bounds when they run.
---

`weavec-cc` guards every access of the C it compiles, unchanged. A **safe function** goes further, as Rust's safe code does: `weavec-cc` checks it against a few local rules when it compiles, rejects it with an error naming the rule and the fix when it breaks one, and, once it compiles, checks its accesses against the bounds its pointers carry instead of guarding them. The design is [RFC 0041](/rfcs/0041-safe-c/).

Safe C is plain C. Its annotations come from `weavec.h` and expand to nothing under other compilers, so a converted file still builds anywhere.

## Making functions safe

| How                                                    | Effect                                                                                                 |
| ------------------------------------------------------ | ------------------------------------------------------------------------------------------------------ |
| `WEAVEC_SAFE` before a function definition             | that function is safe                                                                                  |
| `-fweavec-safe` on the command line                    | every function the unit defines outside system headers is safe                                         |
| `WEAVEC_GUARDED` before a definition in such a unit    | that function stays guarded, as all code is without Safe C                                             |
| `WEAVEC_UNSAFE`, `no_sanitize("address")`              | that function stays unsafe (unguarded), as before                                                      |

A safe function may call guarded functions, and guarded code may call safe functions: convert a program one function at a time. The rules run whenever WeaveC does, under `-fsyntax-only` and `-fweavec-checks=none` too, so the loop is cheap:

```console
$ weavec-cc -fsyntax-only -fweavec-safe list.c
list.c:14:12: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
list.c:14:12: note: give its declaration a bound: 'T *WEAVEC_COUNTED_BY(n) p' (or WEAVEC_SIZED_BY, WEAVEC_ENDED_BY, WEAVEC_NULL_TERMINATED), or index an array or an allocation
```

`-fdiagnostics-format=sarif` gives the same errors as SARIF for tools.

## Bounds

A pointer in a safe function points to **one object** unless its declaration says how many. The annotations go after the `*` of a pointer type, on parameters, results, fields and globals:

| Annotation                                     | The pointer points to                                     |
| ---------------------------------------------- | --------------------------------------------------------- |
| none                                           | one object of its type                                    |
| `WEAVEC_COUNTED_BY(n)`                         | `n` elements; `n` names an integer parameter, or is an integer constant or a macro that expands to one |
| `WEAVEC_SIZED_BY(n)`                           | `n` bytes                                                 |
| `WEAVEC_ENDED_BY(e)`                           | the elements before `e`, another pointer parameter        |
| `WEAVEC_NULL_TERMINATED`                       | a string, up to and with its NUL                          |
| `WEAVEC_NULL_TERMINATED_OR_NULL`               | a string, or null                                         |
| `WEAVEC_COUNTED_BY_OR_NULL(n)`, `WEAVEC_SIZED_BY_OR_NULL(n)` | as above, or null whatever `n` is           |

Clang's own `counted_by`, `sized_by` and their `_or_null` forms on struct fields (the Linux kernel's `__counted_by`) count as the WeaveC forms.

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

| Rule             | Rejects                                                                                                                         | Write instead                                                                                     |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- |
| `index-single`   | indexing or offsetting a pointer to one object: `p[1]`, `p + 2`, `p++` (`p[0]` is `*p`, and is fine)                             | a bound on its declaration, an array, or an allocation                                            |
| `int-to-pointer` | an integer converted to a pointer (`(int *)addr`), other than the constant null                                                  | keep the pointer itself                                                                           |
| `pointer-cast`   | a pointer converted to a type that holds pointers from another type (`(struct node *)bytes`, `(struct node *)ctx`), and function pointer conversions | convert only between types without pointers, or from an allocation's result (`struct node *n = malloc(sizeof *n)`) |
| `pointer-bytes`  | a writable byte view of an object that holds pointers: `(char *)node`, `memset(node, 1, …)`, `memcpy(node, other_type, …)`       | `*d = *s`, `memcpy` between two objects of the same type, `memset(…, 0, …)`; passing the object to a function that is not safe is fine |
| `union-pointer`  | reading a union member that holds a pointer                                                                                     | a tag beside the union and a struct per variant                                                   |
| `escape`         | storing a pointer parameter where it outlives the call (a field, a global, an array), returning it, or passing it to an escaping parameter | `WEAVEC_ESCAPES` after the parameter's `*` if the function keeps it; `WEAVEC_LIFETIMEBOUND` after its name if the function returns it; a `WEAVEC_BORROWED` field of a local struct to carry it down |
| `call-args`      | passing a pointer to one object for a parameter that has a bound                                                                | pass a pointer with bounds                                                                        |
| `variadic`       | calling a variadic function the [library table](https://github.com/weavefoundry/weavec/blob/main/lib/Core/LibrarySpec.txt) does not describe (`printf` is described), `va_start`, and `va_arg` of a type that holds pointers | take the arguments by type                                                                        |
| `no-asm`         | inline assembly (an empty `asm volatile("" ::: "memory")` barrier is fine), `setjmp`, `longjmp`, `__builtin_frame_address`, computed `goto`                                                | a guarded helper function                                                                         |
| `unsafe-block`   | a `WEAVEC_UNSAFE` block inside a safe function                                                                                  | move the block into a guarded or unsafe function                                                  |
| `bad-annotation` | a bound naming no parameter of the right type                                                                                   | name an integer parameter (or a pointer, for `WEAVEC_ENDED_BY`)                                   |
| `cxx`            | a safe function in C++                                                                                                          | Safe C is C only for now                                                                          |

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
