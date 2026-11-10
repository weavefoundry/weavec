---
title: Your first check
description: Build small C programs with weavec-cc, read the enforcement ledger, and watch guards stop an out-of-bounds index, a heap overflow, a use after free and a use after scope, and read the reports' stacks.
---

This tutorial builds a few small C programs with `weavec-cc` and reads what they print when they go wrong. You need a working [WeaveC installation](/getting-started/installation/). Work in an empty directory.

## Build a program

`weavec-cc` is a C compiler. Every memory access it compiles is _guarded_: before the access runs, a few inline instructions check that the bytes it touches belong to a live object, and the program stops at the first one that does not. Save this as `lookup.c`:

<!-- example:lookup.c -->

The test `i < 4` does not exclude a negative index. Build the program, writing the enforcement ledger into a directory:

```sh
mkdir ledger
weavec-cc -std=c17 -fweavec-ledger=ledger/ -c lookup.c -o lookup.o
weavec-cc lookup.o -o lookup
```

```text
weavec: lookup.c: 4 accesses: 0 proven, 4 guarded, 0 unguarded
```

The build succeeds as it would with Clang. The summary line counts the accesses the compiler saw: `proven` ones lost their guard to a local rule (or the optimizer removed the access), `guarded` ones kept it. The exact counts depend on the WeaveC version and the optimization level. The ledger `ledger/lookup.o.ledger.json` has one row per access; this is the read of `table[i]`:

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

The reason `access` says that an inline check of the bytes the read touches stays in the program.

## Watch an index check

Run the program. An index inside the table works as before; a negative one stops the program at the access instead of reading unrelated memory:

```console
$ ./lookup 2
30
$ ./lookup -1
weavec: index-out-of-bounds at lookup.c:8:12: index -1
    #0 0x100a6872f in lookup lookup.c:8
    #1 0x100a68a47 in main lookup.c:13
    #2 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: index-out-of-bounds at lookup.c:8:12 in lookup (thread T0)
```

The program traps (`SIGTRAP`, or `SIGILL` on x86-64). The first line names the failure and its source location; the indented lines are the stack of calls that reached it, innermost first (on Linux each line has a column too, and a call the optimizer inlined repeats the number of the frame it was inlined into); the last line sums the report up in one line. `table` has a bound its type declares, so the index itself is checked against it; to make the function correct, reject negative indexes too (`if (i >= 0 && i < 4)`).

## Watch a guard

Often nothing in the code says how large an object is. Save this as `vec.c`:

<!-- example:vec.c -->

Inside `get`, nothing says how many elements `v->data` has. The runtime knows: its allocator gave `calloc` a 16-byte block, and every byte after it belongs to no object.

```console
$ weavec-cc -std=c17 vec.c -o vec
$ ./vec 2
0
$ ./vec 4
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xc080000010
    #0 0x1041e0853 in get vec.c:9
    #1 0x1041e0aeb in main vec.c:15
    #2 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0xc080000010 is 0 bytes after the 16-byte heap object at 0xc080000000, allocated by thread T0:
    #0 0x1041e095b in main vec.c:12
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: heap-buffer-overflow at vec.c:9:43 in get (thread T0)
```

The first line names the kind of failure, the access and its address, and the stack under it shows that `main` called `get`. The next paragraph places the address relative to its heap block, and its stack says which call allocated the block: the `calloc` on line 12. Addresses differ from run to run and between platforms. A frame without a file, such as `start` here, is in a system library that has no line tables.

## Use a pointer after `free`

A released block is not reused while a pointer to it remains. Save this as `stale.c`:

```c title="stale.c"
#include <stdio.h>
#include <stdlib.h>

struct session {
  void (*on_close)(void);
};

static void bye(void) { puts("bye"); }

int main(void) {
  struct session *s = malloc(sizeof *s);
  if (!s)
    return 1;
  s->on_close = bye;
  free(s);
  for (int i = 0; i < 4000000; i++)
    free(malloc(sizeof *s));
  s->on_close();
  return 0;
}
```

```console
$ weavec-cc -std=c17 stale.c -o stale
$ ./stale
weavec: heap-use-after-free at stale.c:18:3: read of 8 bytes at 0xbe00000000
    #0 0x10412c737 in main stale.c:18
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0xbe00000000 is 0 bytes inside the 8-byte heap block at 0xbe00000000, released by thread T0:
    #0 0x10412c64f in main stale.c:15
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: the block was allocated by thread T0:
    #0 0x10412c573 in main stale.c:11
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: summary: heap-use-after-free at stale.c:18:3 in main (thread T0)
```

The loop releases millions of blocks of the same size, and the allocator reuses them: released blocks wait in a quarantine until a _sweep_, which stops the program, scans its stacks, registers, writable data and live heap for pointers, and frees for reuse every quarantined block that none points to. `s` still points to its block, so every sweep keeps that block, and the call through `s->on_close` traps. The report gives the stack of the `free` that released the block (line 15) and of the `malloc` that allocated it (line 11). Without the call, `WEAVEC_RT_STATS=1 ./stale` prints the runtime's counters at exit, among them the sweeps and the blocks the last one kept. A second `free(s)` would stop the program in the same way, with `weavec: double-free at stale.c:…: free of 0x…` and the same two stacks.

## Use a local after its scope

Save this as `scope.c`:

```c title="scope.c"
#include <stdio.h>

int main(void) {
  int *p;
  {
    int n = 42;
    p = &n;
  }
  printf("%d\n", *p);
  return 0;
}
```

```console
$ weavec-cc -std=c17 scope.c -o scope
$ ./scope
weavec: stack-use-after-scope at scope.c:9:18: read of 4 bytes at 0x16d7c2320
    #0 0x10263c7b7 in main scope.c:9
    #1 0x19b523e7f in start (/usr/lib/dyld+0x31e7f)
weavec: 0x16d7c2320 is 0 bytes inside 'n' (4 bytes at offset 64), whose scope has ended, in the frame of main (thread T0), whose locals are:
    [64, 68) 'n'
weavec: summary: stack-use-after-scope at scope.c:9:18 in main (thread T0)
```

`n`'s address is taken, so `n` gets redzones in the frame and its scope is tracked: the end of its block poisons its bytes, at every optimization level. The report names the local and its function, and lists the frame's tracked locals with their offsets. A use of a local after its function has returned traps too (`stack-use-after-return`) until another call reuses the frame's bytes; see [what is not caught](/reference/guarantees/#what-is-not-caught).

## Investigate a failure

- `-fweavec-checks=report` prints each failing site's report once and lets the program continue, to see every failure in one run.
- `WEAVEC_RT_REPORT_LOG=<path>` writes the reports to a file instead of stderr, and `WEAVEC_RT_REPORT_FORMAT=json` writes each as one line of JSON.
- A report's frames carry files and lines because `weavec-cc` keeps line tables unless the command line ends in `-g0`; on macOS keep the `.dSYM` directory a one-step build writes beside the program.

The [troubleshooting](/reference/troubleshooting/) page explains [how to read a report](/reference/troubleshooting/#reading-a-report), each report kind and what usually causes it.

## Continue

- [Read the safety guarantees](/reference/guarantees/) to see what a guarded build promises, and under which assumptions.
- [Integrate your build](/guides/build-integration/) with `CC=weavec-cc`.
- [Adopt WeaveC incrementally](/guides/adoption/): run your tests under a guarded build and fix what traps.
