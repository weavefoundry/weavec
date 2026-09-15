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
| Mutable borrow | Exclusive mutable access under the ownership model.                       | `WEAVEC_MUT`      |
| Raw            | Outside the safe pointer contract; operations require an unsafe boundary. | `WEAVEC_RAW`      |

Inference determines kinds and effects from ordinary C whenever possible. Annotations make an interface's expectations explicit. Include `weavec.h` to use them.

```c
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(unsigned capacity);
void buffer_free(struct buffer *WEAVEC_OWNED buffer);
unsigned buffer_size(const struct buffer *WEAVEC_BORROWED buffer);
void buffer_clear(struct buffer *WEAVEC_MUT buffer);
```

These declarations express the intended public interface. They do not prove unavailable implementations. Checked callers must also satisfy the relevant safety and trust requirements.

## Copying a pointer does not copy the object

If `alias = owner`, both pointers name the same allocation. Releasing it through either pointer invalidates both. The checker follows that identity through supported assignments, fields, calls, and control flow.

Moving ownership changes who may release or use the resource. After a consuming call, the caller cannot continue using the old owner as if the transfer had not happened.

## A borrow has a lifetime

A borrow must end before its referent is destroyed. WeaveC uses the pointer's last use when determining whether the borrow is live; the end of the lexical scope is not always the end of the borrow.

Ordinary mode checks release, move, and reallocation conflicts with live borrows. Enable `--exclusive-borrows` (or `-fweavec-exclusive-borrows`) to enforce the model's additional shared-versus-mutable exclusivity rules.

## Ownership is one part of safety

Owning memory does not prove it is initialized, large enough, or non-null. A `malloc` result can be null, and newly allocated bytes need initialization before a checked read. [Checked contracts](/guides/contracts/) account for these separate obligations.

For the model's precise semantics, read [RFC 0001](/rfcs/0001-ownership-model/). For syntax, use the [annotation reference](/reference/annotations/).
