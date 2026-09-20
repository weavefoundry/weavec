---
title: Your first check
description: Find a use-after-free through an alias, fix it, then build a program whose unproven array index is checked at run time and recorded in the ledger.
---

This tutorial starts with one C file. You need a working [WeaveC installation](/getting-started/installation/).

## Find a lifetime bug

Save this as `example.c`:

<!-- example:use-after-free.c -->

Run the analyzer. The `--` separator introduces flags for Clang:

```sh
weavec example.c -- -std=c17
```

The command fails with `[weavec::use-after-free]`:

```text
example.c:14:10: error: use of 'n' after it was freed [weavec::use-after-free]
   14 |   return n->value;
      |          ^
example.c:13:3: note: freed here (through 'alias')
   13 |   node_free(alias);
      |   ^
weavec: example.c: 5 sites: 3 proven, 0 checkable (not enforced), 1 violation, 1 unresolved, 0 trusted; 1 error, 0 warnings
```

There are two pointer variables, but they refer to the same object. `node_free(alias)` releases the object that `n` also names. WeaveC infers the helper's release behavior from its body. The release happens on every path to the read, so this is a definite violation and an error. Had it happened on some paths only, the diagnostic would be a warning saying `n` _may_ have been freed.

The last line is the summary of the file's ledger: every memory operation (_site_) and how each was decided. The exact counts depend on the WeaveC version.

## Fix the lifetime

Read the value before releasing its storage:

<!-- example:fixed.c -->

Replace the original contents with this version and rerun the command. The error is gone and the summary line reports no violation. The function still consumes its input; callers must not use their pointer after the call.

## Watch a runtime check

Some operations cannot be proven safe from the source alone. Save this as `lookup.c`:

<!-- example:lookup.c -->

The guard `i < 4` does not exclude a negative index, so `table[i]` is not proven in bounds. Build the program with `weavec-cc`, writing the ledger into a directory:

```sh
weavec-cc -std=c17 -fweavec-ledger=ledger/ -c lookup.c -o lookup.o
weavec-cc lookup.o -o lookup
```

```text
weavec: lookup.c: 8 sites: 5 proven, 2 checked, 1 unresolved, 0 trusted; 0 errors, 0 warnings
```

The build succeeds: the possible overrun is not an error. Instead, `weavec-cc` inserted a check before the access, and the ledger `ledger/lookup.o.ledger.json` records it:

```json
{
  "kind": "index",
  "line": 8,
  "column": 12,
  "text": "table[i]",
  "facets": {
    "spatial": { "outcome": "checked", "check": { "template": "index" } }
  }
}
```

Run the program. An index inside the table works as before; a negative one stops the program at the access instead of reading unrelated memory:

```sh
./lookup 2     # prints 30
./lookup -1    # traps (SIGTRAP or SIGILL, depending on the target)
```

To see which check failed during development, rebuild with `-fweavec-checks=report`: a failed check then prints `weavec: runtime check failed: index at lookup.c:8:12` and the program continues. To make the function safe without a check, reject negative indices too (`if (i >= 0 && i < 4)`); the facet then becomes `proven` and the check disappears.

## Continue

- [Read the safety guarantees](/reference/guarantees/) to see what proven and checked outcomes promise, and under which assumptions.
- [Integrate your build](/guides/build-integration/) to use real include paths and defines.
- [Look up a diagnostic](/reference/diagnostics/) for its meaning and resolution.
