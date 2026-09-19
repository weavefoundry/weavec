---
title: Ownership and borrowing
description: Learn how WeaveC models owning pointers, shared and mutable borrows, moves, and lifetimes in C.
---

An allocation needs someone responsible for releasing it. Other code may use it temporarily, but those uses must finish while the object is still alive. Ownership and borrowing give names to that discipline.

## Own, borrow, or cross a boundary

| Pointer kind   | Meaning                                                                   | Annotation        |
| -------------- | ------------------------------------------------------------------------- | ----------------- |
| Owned          | Responsible for the referent's resource lifetime.                         | `WEAVEC_OWNED`    |
| Shared borrow  | Temporary, read-only access to a live referent.                           | `WEAVEC_BORROWED` |
| Mutable borrow | Temporary mutable access to a live referent.                              | `WEAVEC_MUT`      |
| Raw            | Outside the safe pointer contract; operations require an unsafe boundary. | `WEAVEC_RAW`      |

Inference determines kinds and effects from ordinary C whenever possible. Annotations make an interface's expectations explicit. Include `weavec.h` to use them.

```c
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(unsigned capacity);
void buffer_free(struct buffer *WEAVEC_OWNED buffer);
unsigned buffer_size(const struct buffer *WEAVEC_BORROWED buffer);
void buffer_clear(struct buffer *WEAVEC_MUT buffer);
```

These declarations express the intended public interface. Callers are checked against them, and when the definitions are linked in, the link step checks each declaration against its definition.

## Copying a pointer does not copy the object

If `alias = owner`, both pointers name the same allocation. Releasing it through either pointer invalidates both. The checker follows that identity through supported assignments, fields, calls, and control flow.

Moving ownership changes who may release or use the resource. After a consuming call, the caller cannot continue using the old owner as if the transfer had not happened.

## A borrow has a lifetime

A borrow must end before its referent is destroyed. WeaveC uses the pointer's last use when determining whether the borrow is live; the end of the lexical scope is not always the end of the borrow.

WeaveC checks release, move and reallocation conflicts with live borrows (`conflicting-borrow`). Two live pointers into one object, or a write to an object another pointer views, are accepted.

## Ownership is one part of safety

Ownership decides the temporal facet of an operation: whether the object is still alive. Whether the pointer is non-null and whether the access stays inside the object are separate facets. A `malloc` result can be null, and an index can run past the allocation. WeaveC proves these facets where it can; otherwise `weavec-cc` checks them at run time, or the ledger lists them as unresolved. See [safety guarantees](/reference/guarantees/).

For the model's precise semantics, read [RFC 0001](/rfcs/0001-ownership-model/). For syntax, use the [annotation reference](/reference/annotations/).
