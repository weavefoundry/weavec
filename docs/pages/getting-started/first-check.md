---
title: Your first check
description: Build small C programs with weavec-cc, read the enforcement ledger, and watch guards stop an out-of-bounds index, a heap overflow, a use after free and a use after scope.
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

The build succeeds as it would with Clang. The summary line counts the accesses the compiler saw: `proven` ones lost their guard to a local rule (or the optimiser removed the access), `guarded` ones kept it. The exact counts depend on the WeaveC version and the optimisation level. The ledger `ledger/lookup.o.ledger.json` has one row per access; this is the read of `table[i]`:

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
```

The program traps (`SIGTRAP`, or `SIGILL` on x86-64). `table` has a bound its type declares, so the index itself is checked against it; to make the function correct, reject negative indexes too (`if (i >= 0 && i < 4)`).

## Watch a guard

Often nothing in the code says how large an object is. Save this as `vec.c`:

<!-- example:vec.c -->

Inside `get`, nothing says how many elements `v->data` has. The runtime knows: its allocator gave `calloc` a 16-byte block, and every byte after it belongs to no object.

```console
$ weavec-cc -std=c17 vec.c -o vec
$ ./vec 2
0
$ ./vec 4
weavec: heap-buffer-overflow at vec.c:9:43: read of 4 bytes at 0xbd80000010
weavec: 0xbd80000010 is 0 bytes after the 16-byte heap object at 0xbd80000000
```

The first line names the kind of failure, the access and its address. The second places the address relative to its heap block. Addresses differ from run to run and between platforms.

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
weavec: heap-use-after-free at stale.c:18:3: read of 8 bytes at 0xbd80000000
weavec: 0xbd80000000 is inside a released heap block at 0xbd80000000
```

The loop releases millions of blocks of the same size, and the allocator reuses them: released blocks wait in a quarantine until a _sweep_, which stops the program, scans its stacks, registers, writable data and live heap for pointers, and frees for reuse every quarantined block that none points to. `s` still points to its block, so every sweep keeps that block, and the call through `s->on_close` traps. Without the call, `WEAVEC_RT_STATS=1 ./stale` prints the runtime's counters at exit, among them the sweeps and the blocks the last one kept. A second `free(s)` would stop the program in the same way, with `weavec: invalid release of 0x…: the block was already released`.

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
weavec: stack-use-after-scope at scope.c:9:18: read of 4 bytes at 0x16db82740
```

`n`'s address is taken, so `n` gets redzones in the frame and its scope is tracked: the end of its block poisons its bytes, at every optimisation level. A use of a local after its function has returned is not caught; see [what is not caught](/reference/guarantees/#what-is-not-caught).

## Investigate a failure

- `-fweavec-checks=report` prints each failing site's report once and lets the program continue, to see every failure in one run.
- `WEAVEC_RT_BACKTRACE=1` prints the stack after a report that stops the program.
- `WEAVEC_RT_REPORT_LOG=<path>` writes the reports to a file instead of stderr.

The [troubleshooting](/reference/troubleshooting/) page explains each report kind and what usually causes it.

## Continue

- [Read the safety guarantees](/reference/guarantees/) to see what a guarded build promises, and under which assumptions.
- [Integrate your build](/guides/build-integration/) with `CC=weavec-cc`.
- [Adopt WeaveC incrementally](/guides/adoption/): run your tests under a guarded build and fix what traps.
