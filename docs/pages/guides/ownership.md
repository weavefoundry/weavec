---
title: Ownership and borrowing
description: Learn how WeaveC's advisory analysis models owning pointers, borrows, moves and lifetimes in C, and how its findings relate to the run-time guards.
---

An allocation needs someone responsible for releasing it. Other code may use it temporarily, but those uses must finish while the object is still alive. Ownership and borrowing give names to that discipline.

WeaveC's ownership analysis is advisory. `weavec` runs it and reports what it finds, and `weavec-cc -fweavec-diagnose` runs it beside the compiler; neither changes the generated code. What stops a use after free in a build is the run-time guards `weavec-cc` puts on every memory access, which work without any annotation.

## Own, borrow, or cross a boundary

| Pointer kind   | Meaning                                                                   | Annotation        |
| -------------- | ------------------------------------------------------------------------- | ----------------- |
| Owned          | Responsible for the referent's resource lifetime.                         | `WEAVEC_OWNED`    |
| Shared borrow  | Temporary, read-only access to a live referent.                           | `WEAVEC_BORROWED` |
| Mutable borrow | Temporary mutable access to a live referent.                              | `WEAVEC_MUT`      |
| Raw            | Outside the safe pointer contract; operations require an unsafe boundary. | `WEAVEC_RAW`      |

Inference determines kinds and effects from ordinary C whenever possible. Annotations make an interface's expectations explicit to the analysis. Include `weavec.h` to use them.

```c
#include <weavec.h>

struct buffer *WEAVEC_OWNED buffer_new(unsigned capacity);
void buffer_free(struct buffer *WEAVEC_OWNED buffer);
unsigned buffer_size(const struct buffer *WEAVEC_BORROWED buffer);
void buffer_clear(struct buffer *WEAVEC_MUT buffer);
```

These declarations express the intended public interface. The analysis checks callers against them and each definition against its own annotations, and `weavec --whole-program` also checks each declaration against the definition in another file.

## Copying a pointer does not copy the object

If `alias = owner`, both pointers name the same allocation. Releasing it through either pointer invalidates both. The analysis follows that identity through supported assignments, fields, calls, and control flow.

Moving ownership changes who may release or use the resource. After a consuming call, the caller cannot continue using the old owner as if the transfer had not happened.

## A borrow has a lifetime

A borrow must end before its referent is destroyed. WeaveC uses the pointer's last use when determining whether the borrow is live; the end of the lexical scope is not always the end of the borrow.

WeaveC reports release, move and reallocation conflicts with live borrows (`conflicting-borrow`). Two live pointers into one object, or a write to an object another pointer views, are accepted.

## Findings and guards

In `weavec`, a finding that holds on every path is an error (`use-after-free`, `double-free`, `use-after-move`, ...) and one that holds on some paths only is a warning; under `weavec-cc -fweavec-diagnose` every finding is a warning, and `-Werror=weavec` makes them errors. Run `weavec` in CI, or build with `-fweavec-diagnose -Werror=weavec`, to fail on them. See [diagnostics](/reference/diagnostics/).

The analysis is not what makes a build safe. A `weavec-cc` build guards every access, whatever the analysis concluded and whatever the annotations say:

- a use of a heap object after its `free` traps while the released block is in the runtime's quarantine (16 MiB of released blocks by default); once the block is reused, a stale pointer sees the new object;
- every `free` is validated by the runtime's allocator, so a double free or the release of an interior or non-heap pointer stops the program;
- a use of a local after its scope ends traps; a use after the function returns does not.

The analysis also reports what the guards cannot catch: a leak, a mismatched release family, a use after free once the block has been recycled. See [safety guarantees](/reference/guarantees/).

RFC 0036 (planned; see the [roadmap](/project/roadmap/)) gives ownership an enforcement role: contracts checked at function entry. For the model's precise semantics, read [RFC 0001](/rfcs/0001-ownership-model/) as amended by [RFC 0035](/rfcs/0035-guard-by-default/). For syntax, use the [annotation reference](/reference/annotations/).
