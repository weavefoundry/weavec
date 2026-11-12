# Safe C's rules

A safe function (`WEAVEC_SAFE`, or every function of a unit compiled with `-fweavec-safe`) is checked against these rules when it compiles. Each error ends with the rule's identifier, `[weavec-safe:<rule>]`, and `weavec-cc --weavec-explain <rule>` prints the rule's section of this page; `weavec-cc --weavec-explain all` prints all of it. Every rule is decided inside one function, from the declarations it can see. A rule rejects a construct and says what to write instead: none tries to prove that code it does not accept is correct. What cannot be written in a safe function goes in a small `WEAVEC_GUARDED` helper, which `weavec-cc --weavec-audit` lists.

The examples below include `<weavec.h>`, `<stddef.h>`, `<stdio.h>`, `<stdlib.h>` and `<string.h>`. The design is in [RFC 0041](/rfcs/0041-safe-c/) (bounds), [RFC 0042](/rfcs/0042-typed-memory/) (typed memory) and [RFC 0043](/rfcs/0043-ownership/) (ownership).

## index-single

A pointer without a bound points to one object: it is dereferenced (`*p`, `p->f`, `p[0]`) but not indexed or offset (`p[i]`, `p + 2`, `p++`).

```c rejected
int second(const int *p) { return p[1]; }
```

```c accepted
int second(const int *WEAVEC_COUNTED_BY(n) p, size_t n) {
  return n > 1 ? p[1] : 0;
}
```

Give the declaration a bound (`WEAVEC_COUNTED_BY(n)`, `WEAVEC_SIZED_BY(n)`, `WEAVEC_ENDED_BY(e)`, `WEAVEC_NULL_TERMINATED`), or index an array or an allocation. A struct's pointer field takes the same annotations, naming a sibling field.

## int-to-pointer

No integer becomes a pointer, other than the constant null.

```c rejected
int *at(unsigned long address) { return (int *)address; }
```

Keep the pointer itself, and an offset beside it if one is needed.

## pointer-cast

A pointer converts to another pointer type only when no pointer can be forged: memory that holds pointers converts to nothing but `void` and `const` bytes; a `void *` converts to no other type, but where an allocation made it (`struct node *n = malloc(sizeof *n)`); function pointers do not convert.

```c rejected
struct node { struct node *WEAVEC_OWNED next; int v; };
int peek(void *context) {
  struct node *n = context;
  return n->v;
}
```

```c accepted
struct node { struct node *WEAVEC_OWNED next; int v; };
int peek(const struct node *n) { return n->v; }
```

A library that hands back a `void *` context gets a `WEAVEC_GUARDED` adapter of a few lines that converts it and calls the safe function.

## pointer-bytes

Memory that holds pointers is not written as bytes, nor copied or zeroed in part: a pointer would be half written or forged.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void clear(struct item *it) { memset(it, 0, 4); }
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void clear(struct item *it) {
  free(it->name);
  memset(it, 0, sizeof *it);
}
```

Copy whole objects (`*d = *s`), zero whole objects (`memset(p, 0, n * sizeof *p)`).

## union-pointer

A union member that holds a pointer is not read: the union may hold something else.

```c rejected
union value { long number; char *text; };
char first(union value *v) { return v->text[0]; }
```

Keep a tag beside a struct per variant, or one pointer per variant in a struct.

## escape

A pointer parameter is borrowed for the call: the function reads and writes through it and passes it on, but does not keep it after it returns. Storing it in memory other than a local, returning it, or passing it to a `WEAVEC_ESCAPES` parameter is an error.

```c rejected
struct holder { int *kept; };
void keep(struct holder *h, int *p) { h->kept = p; }
```

```c accepted
struct holder { int *kept; };
void keep(struct holder *h, int *WEAVEC_ESCAPES p) { h->kept = p; }
```

`WEAVEC_ESCAPES` after the `*` if the function keeps a reference (callers then cannot pass a stack address); `WEAVEC_LIFETIMEBOUND` after the parameter's name if the function returns it; `WEAVEC_OWNED` if the function takes the block and is responsible for releasing it (see `owner-source`); a `WEAVEC_BORROWED` field of a local struct to carry it down a call tree.

## call-args

An argument for a parameter with a bound has bounds that cover it; a pointer to one object is not passed, stored or returned where a declaration gives a bound.

```c rejected
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
int one(const int *p) { return sum(p, 4); }
```

```c accepted
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
int four(const int *WEAVEC_COUNTED_BY(4) p) { return sum(p, 4); }
```

Pass a pointer with bounds (an array, an allocation, a counted parameter), or declare the parameter with the same bound.

## variadic

A variadic call is only to a function of the library table whose row describes its format (`printf` is described); `va_start` is not used, nor `va_arg` of a type that holds pointers.

```c rejected
int total(int count, ...);
int call(void) { return total(2, 1, 2); }
```

Take the arguments by type: a counted array and its length.

## no-asm

No inline assembly (an empty `asm volatile("" ::: "memory")` barrier is fine), `setjmp`, `longjmp`, `__builtin_frame_address` or computed `goto`.

```c rejected
int cycles(void) {
  int r;
  __asm__("mov %0, #1" : "=r"(r));
  return r;
}
```

Move it into a `WEAVEC_GUARDED` or `WEAVEC_UNSAFE` helper.

## unsafe-block

A safe function holds no `WEAVEC_UNSAFE` block: every exemption is a function of its own, visible in a signature and in the audit.

```c rejected
int peek(int *p) {
  WEAVEC_UNSAFE { return p[1]; }
}
```

Move the block into a `WEAVEC_UNSAFE` function.

## bad-annotation

A bound names a parameter (or, on a field, a sibling field) of the right type, an integer literal, or a macro that expands to one; every declaration of a function gives it the same bounds.

```c rejected
int sum(const int *WEAVEC_COUNTED_BY(count) v, size_t n);
```

```c accepted
int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
```

## cxx

Safe functions are C only for now: `weavec-c++` rejects `-fweavec-safe` and `WEAVEC_SAFE`. Compile the code as C, or keep it guarded.

## counted-pair

A count or end that bounds a pointer field changes only in the statement next to its pointer's assignment (either order), or alone by a step (`n++`, `n += k`, `n--`, `n -= k`, `n = 0`), which is checked when it runs. A struct initializer gives its counted pointer at least the elements its count says.

```c rejected
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
void shrink(struct vec *v, size_t cap) { v->cap = cap; }
```

```c accepted
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int grow(struct vec *v) {
  size_t cap = v->cap ? 2 * v->cap : 8;
  int *d = realloc(v->data, cap * sizeof *d);
  if (d == NULL)
    return -1;
  v->data = d;
  v->cap = cap;
  return 0;
}
```

Allocate into a local first, test it, then assign the pointer and its count in adjacent statements with no call between them.

## string-index

A null-terminated pointer is read at its current byte and stepped forward by constants; an index or offset that is not a constant needs its length first.

```c rejected
char at(const char *WEAVEC_NULL_TERMINATED s, size_t i) { return s[i]; }
```

```c accepted
char at(const char *WEAVEC_NULL_TERMINATED s, size_t i) {
  size_t n = strlen(s);
  return i < n ? s[i] : 0;
}
```

`size_t n = strlen(s);` gives `s` the bounds `[s, s + n + 1)` while `s` is not assigned again. Step a string inside the branch that tested the bytes stepped over.

## string-write

Nothing is stored through a null-terminated pointer, nor passed where the callee may write through it: a string is read-only in safe code. A buffer that is written is counted.

```c rejected
void trim(char *WEAVEC_NULL_TERMINATED s) { s[0] = 0; }
```

```c accepted
void trim(char *WEAVEC_COUNTED_BY(cap) s, size_t cap) {
  if (cap > 0)
    s[0] = 0;
}
```

## field-address

The address of a pointer field (or of an element of an array of them) does not leave the function: as an argument, a result, or a value stored in memory.

```c rejected
struct node { struct node *WEAVEC_OWNED next; };
struct node *WEAVEC_OWNED *link(struct node *n) { return &n->next; }
```

Pass the struct that holds the field, or read the field into a local, pass that, and assign the field back.

## owner-moved

A pointer declared `WEAVEC_OWNED` is the one responsible for releasing its block. An owner is not used after it was moved (passed to a `WEAVEC_OWNED` parameter, returned, stored in another owner) or released, on any path that reaches the use; nor before the result of `realloc` that took it was tested. A struct is not passed, moved or left behind at a return while an owner in it that the function moved out was not assigned again.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void item_free(struct item *WEAVEC_OWNED it);
int last(struct item *WEAVEC_OWNED it) {
  item_free(it);
  return it->v;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void item_free(struct item *WEAVEC_OWNED it);
int last(struct item *WEAVEC_OWNED it) {
  int v = it->v;
  item_free(it);
  return v;
}
```

Use the owner before you give it away, or keep what you need in a local. An owner field moved out of a struct the caller owns (`free(v->data)`) is assigned before the function returns (`v->data = NULL`). Assigning a moved owner (`p = NULL`) makes it usable again.

## owner-leak

An owner holds no block when it is dropped: when its scope ends or the function returns, when it is assigned over, when the struct that holds it is released, and when a call's owned result is discarded or stored where it is only a reference. "Holds no block" means: moved or released on every path, or tested null. `exit` and `abort` drop nothing.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);
int check(int c) {
  struct item *it = item_new(c);
  if (c > 100)
    return -1;
  item_free(it);
  return 0;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);
int check(int c) {
  struct item *it = item_new(c);
  int r = c > 100 ? -1 : 0;
  item_free(it);
  return r;
}
```

A destructor releases each owner field before the struct (`free(it->name); free(it);`). A setter releases the old block before it stores the new one. `p = realloc(p, n)` loses the block when `realloc` fails: assign the result to a new local, test it, then assign `p`. A local that receives an allocation (`char *p = malloc(n)`) is an owner without being declared one; a function that returns one declares its result `WEAVEC_OWNED`.

## owner-source

Only an owner's value goes into an owner (a field, a parameter or a result declared `WEAVEC_OWNED`, or an owner local): the result of a call that returns one, another owner (which moves it), or null. Not a borrowed parameter, a borrow or a reference, the address of a local or a global, a string literal, or a pointer computed by arithmetic. An owner is not stepped (`p++`), and a block is released only through its owner.

```c rejected
struct item { char *WEAVEC_OWNED name; int v; };
void set_name(struct item *it, char *name) {
  free(it->name);
  it->name = name;
}
```

```c accepted
struct item { char *WEAVEC_OWNED name; int v; };
void set_name(struct item *it, char *WEAVEC_OWNED name) {
  free(it->name);
  it->name = name;
}
```

Take the parameter `WEAVEC_OWNED` if the function keeps the block, or store a copy (`strdup(name)`). Step a borrow (`char *q = p; q++;`), not the owner. A struct that holds owners is assigned as a struct (`*a = *b`, which moves them), not copied as bytes.

## owner-path

An owner is moved, released or replaced only along the path that owns it: from a local owner, or from a parameter that is not a pointer to `const`, through owners. Not through a reference (a pointer field that is not `WEAVEC_OWNED`), not through a pointer to `const`, and not below a global read in place. A reference is not passed where the callee may change the owners below it.

```c rejected
struct node { struct node *WEAVEC_OWNED next; struct node *prev; int v; };
void node_free(struct node *WEAVEC_OWNED n);
void drop_next(struct node *n) {
  struct node *WEAVEC_OWNED next = n->prev->next;
  n->prev->next = NULL;
  node_free(next);
}
```

```c accepted
struct node { struct node *WEAVEC_OWNED next; struct node *prev; int v; };
void node_free(struct node *WEAVEC_OWNED n);
void drop_next(struct node *n) {
  struct node *WEAVEC_OWNED next = n->next;
  n->next = NULL;
  node_free(next);
}
```

Reach the owner from the pointer that owns it (a list's head, a tree's root), or keep the function `WEAVEC_GUARDED`. Move a global owner into a local first (`T *WEAVEC_OWNED s = g; g = NULL;`), change it, and move it back.

## owner-slot

An owner the function does not name exactly (an element at an index, `*pp`, a field reached through a borrow) is moved out of only in a statement followed at once by an assignment to the same place, so that no owner is left holding a pointer it gave away.

```c rejected
struct bag { char *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void drop(struct bag *b, size_t i) { free(b->items[i]); }
```

```c accepted
struct bag { char *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void drop(struct bag *b, size_t i) {
  free(b->items[i]);
  b->items[i] = NULL;
}
```

`x = a[--n]; a[n] = NULL;` and a swap through a local owner are accepted.

## stale-borrow

A borrow (a local pointer taken from an owner or from inside what it owns) is not used after that owner, or one above it, was moved, released or replaced by the function, nor after a call that could have done so: a call given a pointer to non-`const` memory that holds owners.

```c rejected
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int vec_grow(struct vec *v);
int first_after_grow(struct vec *v) {
  int *first = v->data;
  if (vec_grow(v))
    return -1;
  return first[0];
}
```

```c accepted
struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
int vec_grow(struct vec *v);
int first_after_grow(struct vec *v) {
  if (vec_grow(v))
    return -1;
  int *first = v->data;
  return v->cap > 0 ? first[0] : 0;
}
```

Take the pointer again after the call, keep an index instead, or declare the callee's parameter `const` if it changes no owner.

## call-alias

In one call, an argument through which the callee could change owners does not point to the same object as another argument that holds owners, nor above or below it, as far as the function knows where both came from.

```c rejected
struct item { char *WEAVEC_OWNED name; };
struct bag { struct item *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(len) items; size_t len; };
void bag_put(struct bag *b, struct item *it);
void again(struct bag *b) {
  if (b->len > 0)
    bag_put(b, b->items[0]);
}
```

Pass only the pointer the callee changes, an index into it, or make the other parameter `const`.
