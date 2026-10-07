---
title: Your first check
description: Find a use-after-free through an alias with the analysis, fix it, then build programs whose bad accesses stop at run time with a precise report.
---

This tutorial starts with one C file. You need a working [WeaveC installation](/getting-started/installation/).

## Find a lifetime bug

Save this as `example.c`:

<!-- example:use-after-free.c -->

Run the analysis. The `--` separator introduces flags for Clang:

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
weavec: example.c: 5 sites: 3 proven, 1 not proven, 1 violation, 0 trusted; 1 error, 0 warnings
```

There are two pointer variables, but they refer to the same object. `node_free(alias)` releases the object that `n` also names. WeaveC infers the helper's release behaviour from its body. The release happens on every path to the read, so this is a definite violation and an error. Had it happened on some paths only, the diagnostic would be a warning saying `n` _may_ have been freed.

The last line summarises the analysis: every memory operation (_site_) it saw, and how many it proved safe, could not decide, or found violated. The exact counts depend on the WeaveC version. The analysis is advisory: it reports bugs before the program runs, and it never changes what `weavec-cc` compiles.

## Fix the lifetime

Read the value before releasing its storage:

<!-- example:fixed.c -->

Replace the original contents with this version and rerun the command. The error is gone and the summary line reports no violation. The function still consumes its input; callers must not use their pointer after the call.

## Watch an index check

`weavec-cc` is a C compiler. Every memory access it compiles is _guarded_: before the access runs, a few inline instructions ask the runtime linked into the program whether the bytes it touches are addressable, and the program stops at the first one that is not. Save this as `lookup.c`:

<!-- example:lookup.c -->

The test `i < 4` does not exclude a negative index. Build the program, writing the enforcement ledger into a directory:

```sh
weavec-cc -std=c17 -fweavec-ledger=ledger/ -c lookup.c -o lookup.o
weavec-cc lookup.o -o lookup
```

```text
weavec: lookup.c: 3 accesses: 1 proven, 2 guarded, 0 unguarded
```

The build succeeds as it would with Clang. The summary line counts the accesses the compiler saw: `proven` ones lost their guard to a local rule (or the optimiser removed the access), `guarded` ones kept it. The ledger `ledger/lookup.o.ledger.json` has one row per access:

```json
{
  "function": "lookup",
  "line": 8,
  "column": 12,
  "operation": "load",
  "bytes": 4,
  "outcome": "guarded",
  "reason": "access"
}
```

Run the program. An index inside the table works as before; a negative one stops the program at the access instead of reading unrelated memory:

```console
$ ./lookup 2
30
$ ./lookup -1
weavec: index-out-of-bounds at lookup.c:8:12: index -1
```

The program traps (`SIGTRAP`, or `SIGILL` on x86-64). `table` has a bound its type declares, so the index itself is checked against it; to make the function correct, reject negative indexes too (`if (i >= 0 && i < 4)`).

## Watch a guard

Often nothing in the code says how large an object is. Save this as `vec.c`:

<!-- example:vec.c -->

Inside `get`, nothing says how many elements `v->data` has. The runtime knows: its allocator gave `calloc` a 16-byte block, and every byte after it is marked as belonging to no object.

```console
$ weavec-cc -std=c17 vec.c -o vec
$ ./vec 2
0
$ ./vec 4
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The first line names the kind of failure, the access and its address; for the heap, the second places the address relative to its object. The same guards stop a read of a freed block while it waits in the runtime's quarantine (`heap-use-after-free`), an overflow of a local or a global into its redzone, a use of a local after its scope ends, and a `free` of anything but the start of a live block.

To see every failure while you investigate, build with `-fweavec-checks=report`: each failing site prints its report once and the program continues. [Safety guarantees](/reference/guarantees/) says exactly what is and is not caught.

## Continue

- [Read the safety guarantees](/reference/guarantees/) to see what a guarded build promises, and under which assumptions.
- [Integrate your build](/guides/build-integration/) with `CC=weavec-cc`, and run the analysis on the same sources.
- [Look up a diagnostic](/reference/diagnostics/) for its meaning and resolution.
