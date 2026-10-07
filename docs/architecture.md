# Architecture

WeaveC is a drop-in C compiler that enforces memory safety at run time, and
an advisory ownership and memory-safety analysis for C, both built on Clang
and LLVM. [RFC 0035](rfcs/0035-guard-by-default.md), *Guard by default*,
defines the design; its *Implementation amendments* override its body.

`weavec-cc` guards every memory access of the C code it compiles, in LLVM
IR, against the runtime's shadow memory, and removes a guard only where a
simple local rule proves it redundant. It lays out the stack objects a guard
can reach and the unit's globals with redzones, zero-initialises locals,
checks typed array indexes against their declared bounds, and links a
runtime whose allocator leaves at least one byte after every heap object
that no object owns and keeps released blocks in a quarantine. A bad access
traps with a report. The analysis of RFCs 0030 and 0031 (pointer kinds,
sites and facets, the object engine behind the `SafetyEngine` seam,
summaries, whole-program analysis) is advisory: it runs in the `weavec` tool
and under `weavec-cc -fweavec-diagnose`, reports diagnostics, and never
changes the generated code. Ownership enforcement is planned as checked
contracts in RFC 0036.

The code is three C++ libraries, two thin command-line tools and a C
runtime. The arrows point from a layer to what it may depend on.

```
        ┌──────────────────────┐  ┌──────────────────────────┐
        │  tools/weavec        │  │  tools/weavec-cc         │
        │  (libTooling)        │  │  (drop-in C compiler)    │
        └──────────┬───────────┘  └────────────┬─────────────┘
                   └───────────┬───────────────┘
                               ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  guard passes, enforcement
                 │  lib/Frontend            │  ledger, unsafe regions,
                 │                          │  analysis action, whole-
                 │                          │  program analysis, driver
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Analysis        │  kinds, sites, slots, the
                 │  lib/Analysis            │  engine seam, the object
                 │                          │  engine
                 └──────┬───────────┬───────┘
                        ▼           ▼
        ┌────────────────────┐   ┌────────────────────┐
        │  weavec::Core      │   │  Clang / LLVM      │
        │  lib/Core          │   │  (external)        │
        │  no Clang/LLVM     │   └────────────────────┘
        └────────────────────┘
```

The guard passes in Frontend use LLVM and Core's library table directly and
nothing from Analysis. `runtime/` is C code that `weavec-cc` links into
every enforcing link (see *The runtime*). It depends on nothing above and
includes no WeaveC header but its own. The layering rule is strict:

- Core includes nothing from `clang/` or `llvm/`. It is the one library
  built without their include paths.
- Analysis is the only layer that knows both Core and Clang's AST.
- Inside Analysis, only the `Engine*.cpp` files include the engine's
  private header `lib/Analysis/Engine.h` (RFC 0031 §2, gate H2), so the
  components on the near side of the engine seam (see *The engine seam*),
  `SiteCollector`, `AttributeReader`, `KindInference`, `SlotCollector`,
  `BoundaryInvariants` and `LedgerAdapter`, never see the engine's
  internals. The rest of the code names the engine only as `ObjectEngine`
  behind `SafetyEngine`.

RFC 0035 is Accepted and implemented in stages; the [roadmap](roadmap.md)
says which have landed. This page describes the tree as it is. Earlier
designs (checked mode, checks and guards planned from the analysis, unit
records and the link step) are described in the RFCs that introduced and
retired them.

## Compile pipeline

`weavec-cc` compiles a C translation unit as Clang does, in-process, with
these additions in an enforcing mode (`-fweavec-checks=trap`, the default,
`report` or `verify`; RFC 0035 §1 and amendment 1):

1. **Zero-initialised locals.** `-ftrivial-auto-var-init=zero`
   (`-fno-weavec-zero-init` turns it off). The runtime's allocator
   zero-fills every block.
2. **Array bounds.** `-fsanitize=array-bounds`, reported through the
   runtime's weak `__ubsan_handle_out_of_bounds` and
   `__ubsan_handle_out_of_bounds_abort`, unless the command line already
   asks for a sanitizer; report mode adds `-fsanitize-recover` for it. A
   program that also links UBSan's runtime gets UBSan's handlers.
3. **Locations.** Without `-g`, the unit is compiled with location tracking
   only (`DebugInfoKind=LocTrackingOnly`, with columns): instructions carry
   source locations for the passes and the reports, and no debug
   information is emitted.
4. **Lifetime markers at every `-O` level**
   (`SanitizeAddressUseAfterScope`), so a use after scope traps at `-O0`
   too.
5. **No conditional compares on AArch64** (`-aarch64-enable-ccmp=false`;
   a later `-mllvm` option of the user's wins). The backend folded a
   guard's partial-granule test back into its fast path.
6. **The unsafe-region collector** (`UnsafeRegions`) runs over the AST
   before code generation (see *Unsafe regions*).
7. **Four LLVM passes** (`registerGuardPasses` in
   `lib/Frontend/GuardPass.cpp`), at every optimisation level:

   | Pass | Extension point | Files | Does |
   | --- | --- | --- | --- |
   | `GuardInsert` | `PipelineStartEP` | `GuardInsert.cpp`, `GuardLibrary.cpp` | puts a marker before each access and each memory argument of a library call, redirects wrapped library calls, records the ledger rows |
   | `GuardPrune` | `PeepholeEP` | `GuardInsert.cpp` | after inlining, removes the markers rule 6.1 proves on a local or global at constant offsets, so that SROA still promotes those locals; manages the scope barriers |
   | `GuardLoop` | `VectorizerStartEP` | `GuardLoops.cpp` | rule 6.4: versions loops whose whole ranges one check covers |
   | `GuardExpand` | `OptimizerLastEP` | `GuardExpand.cpp`, `GuardFrames.cpp`, `GuardGlobals.cpp` | rules 6.1–6.3, frame layout, global redzones, and each marker into its inline check |

   `GuardPass.cpp` and the private header `GuardPassImpl.h` hold what the
   passes share: the module's context, the markers, the runtime's entry
   points, the site records, and the inline checks of the shadow. The
   passes run on 64-bit targets only, and skip declarations, naked
   functions and the runtime's own functions. An `available_externally`
   body (a C99 inline definition) is guarded, since the inliner copies it
   into the unit.

Guards are inserted first and expanded last because the optimiser exploits
undefined behaviour: an out-of-bounds read in a loop can make the loop
infinite, and a read after `free` can become `undef`, before a pass at the
end of the pipeline sees the access. GuardInsert's markers are

```
void __weavec.guard(ptr at, i64 width, i32 flags, ptr nullBase, i32 row)
void __weavec.range(ptr at, i64 length, i32 flags, i32 row)
i64  __weavec.strlen(ptr string, i64 max, i32 flags, i32 row)
void __weavec.disjoint(ptr destination, ptr source, i64 length)
```

They are `nounwind` and `nomerge`, read only the runtime's memory (a string
marker the string too), capture nothing, and are not `willreturn`: the
optimiser may neither drop nor merge them, nor move an access past one, but
it may hoist and remove them as it does loads. Beside each lifetime marker
GuardInsert puts `__weavec.scope()`, which writes the runtime's memory, so
that no marker is hoisted above the `lifetime.start` of a local declared in
a loop; GuardPrune drops the barriers of locals SROA promoted and adds them
beside the lifetime markers inlining brought (amendment 2).

What is collected (§2.1): a `load`, `store`, `atomicrmw` or `cmpxchg` is a
guard of the bytes of its type; `llvm.memcpy`, `llvm.memmove` and
`llvm.memset` a range for each memory operand; a masked load or store, a
gather or a scatter one access per lane under its mask bit; a call of a
library-table function a range or a string for each memory argument. An
access to a stack or global object at a constant offset inside it gets no
marker and no ledger row, so the object stays promotable to registers.
Inline assembly operands, accesses in a non-default address space and the
passes' own shadow accesses are not collected.

Nothing else changes: no AST is rewritten and code generation is not
deferred. A unit compiled with `-fweavec-checks=none` or `-fno-weavec`, or
as C++, Objective-C, OpenCL or CUDA, is compiled as Clang compiles it.
Under `-fweavec-diagnose` the analysis runs beside Clang's code generator
in the same job (see *The advisory analysis*).

## Guards and the shadow

The runtime keeps one shadow byte per 16-byte granule (§2.2 and amendment
3; the values are in [`weavec_rt.h`](../runtime/weavec_rt.h)):

| Value | Meaning |
| --- | --- |
| `0x00` | all 16 bytes addressable |
| `0x01`–`0x0F` | the first k bytes addressable, the rest not |
| `0xF1`, `0xF2`, `0xF3` | a tracked frame's left, middle and right redzone |
| `0xF4` | a dynamic stack object's redzone (`alloca`, VLA) |
| `0xF8` | a tracked local out of scope |
| `0xF9` | a tracked global's redzone |
| `0xFA` | a heap slot after its object; also committed arena slots not yet handed out, and the granule before each class region |
| `0xFD` | a released heap object |
| `0xFE` | the lowest 64 KiB (the null page) |

The shadow of an address `a` is the byte at `base + ((a >> 4) & mask)`;
GuardExpand reads `base` and `mask` from the descriptor `__weavec_rt_shadow`
once per function. The runtime reserves the shadow for a *window* of the
address space at start-up, without backing: the whole user address space on
Linux (the width the stack's address gives), and 2^42 bytes on Darwin,
where the kernel's cost of a reservation grows with its size. An address
above the window aliases a lower one; the fast path may then fail
spuriously, and the slow path, which knows the window, lets it pass. When
the reservation fails, `mask` is 0 and `base` points to zero bytes: every
guard passes, and the runtime says so once on standard error.

**Fast paths** (amendment 4). A guard whose bytes lie in one granule is
three instructions: the shift and mask, the shadow load, and a branch on a
nonzero byte, weighted cold. The partial-granule test (whether the access
ends within the first k bytes) is on the slow side of the branch, before
the call. An access of up to 16 bytes that may cross a granule loads the
two shadow bytes it may touch at once; a range of constant length up to 113
bytes loads eight with a constant mask. A nonzero byte only sends the
access to the exact check. Any other range calls
`__weavec_rt_range(a, n, site)`, which scans the shadow.

**The slow path.** `__weavec_rt_guard(a, w, site)` decides exactly (the
window, partial granules, the poison kind) and names a failure by the
shadow value that caused it (see *Reports*). `site` is a constant
`__weavec_rt_site` record `{file, line, column, flags}` built from the
instruction's location (the flags say write, report mode, a monitor, and
whether a null string is accepted), or null.

**Null bases** (§2.4). An access whose address is `p + x`, with `x` not a
constant below 4,096 and `p` not known to be non-null, also tests `p`: a
null base takes the slow path, which reports a null dereference. A constant
offset below 4,096 from a null base faults on the null page, and a guard on
an address in the lowest 64 KiB fails as a null dereference through its
shadow, even when the optimiser moved the access to the other arm of a
`select`.

## Removing guards

Every rule is local to a function, needs no fact about another function but
LLVM's `nofree` attribute, and is monitored by verify mode
(`-fweavec-checks=verify`), which emits each guard a rule removed as a
*monitor*: a failing monitor reports `weavec.proven` (RFC 0035 §6).

- **6.1 In bounds of a local or global.** An access whose address is
  `O + x`, where `O` is a static `alloca` or a global with an exact
  definition in the unit of size `S`, and `x` is a constant or an
  expression whose range (ScalarEvolution's signed range, or known bits)
  lies in `[0, S - w]`, needs no guard, provided a local's scope holds the
  access. GuardPrune applies the constant-offset case after inlining, and
  GuardExpand the rest.
- **6.2 Covered by a dominating guard.** A guard is removed when a guard of
  the same base value whose bytes cover it is available: executed on every
  path to it with no *kill* since. A kill is an instruction that may
  release memory or change the shadow: a call not marked `nofree` (LLVM
  infers it, and the library functions carry it), a lifetime end of a
  tracked local, a release. Availability is a forward data-flow problem
  over the function's blocks.
- **6.3 Merged in a block.** The guards of one block on one base at
  constant offsets whose hull is at most 32 bytes, with no kill and no call
  that may not return between them, become one guard of the hull at the
  first of them. Its slow path checks each access it covers, so a failure
  names the access that fails.
- **6.4 Loop ranges** (amendment 6). GuardLoop versions a loop with a known
  trip count, one exit block and nothing that may end a lifetime. Its
  preheader asks `__weavec_rt_range_ok(lo, n)` (no report) whether the
  whole range of each invariant or affine access over the trip is
  addressable; when the trip has at least 16 iterations and every range
  is, a copy of the loop without those guards runs, and LLVM may vectorise
  it; otherwise the guarded loop runs and stops at the access that fails.
  Verify mode does not version.

Everything else stays guarded, including every access through a pointer
loaded from memory or passed in, however often a guard on another pointer
covered its bytes.

## Stack and global objects

**Which locals are tracked** (§3.1, amendment 5). A local (an `alloca`) is
tracked when a guard that remains may reach it: any of the objects a
guarded pointer may be based on, through phis and selects, is that local,
or its address escapes (stored, passed to a call other than a lifetime
marker or a memory intrinsic on itself, returned, or converted to an
integer). Other locals are left as they are.

**The frame** (`GuardFrames.cpp`, §3.2). The tracked static allocas of the
entry block become offsets into one frame `alloca`, aligned to 32 bytes: a
left redzone of 64 bytes, then each object at a 32-byte boundary followed
by its redzone, at least 32 bytes and one eighth of the object up to
4 KiB, so the object and its redzone fill a multiple of 32 bytes
(`guard::paddedSize`). At entry the pass writes the frame's shadow with
constant stores (redzones poisoned, partial granules encoded, objects whose
scope has not begun `0xF8`), and clears it before each return. A lifetime
start of a tracked object makes it addressable and a lifetime end poisons
it with `0xF8`, so a use after scope traps; a use after return is not
caught. A function with a `musttail` call or `localescape` keeps its locals
in place, without redzones.

**Dynamic objects** (§3.4). A tracked dynamic `alloca` (an `alloca` call, a
VLA, or a static alloca outside the entry block) of `n` bytes allocates
`round32(n) + 32` bytes, and its tail is poisoned `0xF4`. Before a stack
restore and before each return, `__weavec_rt_alloca_unpoison(top, bottom)`
clears the shadow of the dynamic objects made since.

**Non-local exits** (§3.5). Before each call of a function that does not
return, other than the runtime's own and those that end the process
(`abort`, `exit`, the assertion failures, `err` and the like), the pass
calls `__weavec_rt_unpoison_stack()`, which clears the shadow from the
current stack pointer to the thread's stack base. A `longjmp`,
`siglongjmp`, `pthread_exit`, or an interpreter's error jump written in
assembly but declared `noreturn` then leaves no poison in the frames it
abandons. A frame left by code not built by `weavec-cc` (an exception
unwinding through C frames) keeps its poison until the stack is reused
(assumption A5).

**Globals** (`GuardGlobals.cpp`, §4). A global the unit defines exactly is
tracked unless it is thread-local, in a named section, in a comdat, in a
non-default address space, a string literal or other mergeable constant
(constant, `unnamed_addr` and local), `common`, appending, externally
initialised, aligned above 4 KiB, of size 0, or one of LLVM's or WeaveC's
own. A tracked global is replaced by a global of the same name, linkage and
visibility that holds the original and a redzone (sized as a frame
object's), aligned to 32 bytes. A module constructor of priority 1 calls
`__weavec_rt_globals_register(array, n)` with
`{address, size, size with redzone}` per global, which poisons each redzone
`0xF9`; a module destructor unregisters them and clears their shadow, so
`dlclose` leaves nothing behind.

## Library calls

`GuardLibrary.cpp` guards a direct call of a function of the library table
([`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt)) that the unit does not
define, before the call (§2.5, amendment 7):

- Each pointer argument the row reads or writes is guarded over the bytes
  its term gives (`bytes(t)`, `count(t)` times the element size, one
  element without a term), evaluated on the call's arguments; a `strlen`
  term is evaluated by the runtime inside the string's addressable bytes.
- A `str` argument is guarded as a string: `__weavec_rt_strlen(s, max,
  site)` scans it and fails with `unterminated-string` when the terminator
  is not addressable. A `printf`- or `scanf`-family call with a constant
  format guards each `%s` argument as a string and each `%n` argument as an
  `int`. Fortified forms (`chk(…)`) are guarded as their base row.
- A row's `wrapper(NAME)` clause replaces the call with
  `__weavec_rt_NAME`, with the call's arguments and the site (first when
  the call is variadic). The wrappers (`runtime/weavec_libc.c`) of
  `strcpy`, `stpcpy`, `strcat`, `sprintf`, `vsprintf`, `snprintf` and
  `vsnprintf` compute what the call writes before it writes, and the
  `printf`-family ones check each `%s` and `%n` argument, through a
  `va_list` too; those of `memchr`, `strchr`, `strcmp`, `strncmp`,
  `strcasecmp` and `strncasecmp` check only the bytes the call read up to
  where it stopped; `gets` checks each byte before it writes it; `mmap`
  and `munmap` clear the shadow of what they map, so a mapping placed where
  a tracked object's poison was left behind is addressable. A wrapper is
  named after its row's function (the parser checks it). The calls that
  write through no pointer are redirected late, by GuardExpand, so that the
  optimiser can still fold `strcmp` of constants; GuardInsert guards only
  the first byte of their string arguments. Such a call that reads exactly
  a constant of at most 113 bytes keeps inline guards instead.
- A row whose result is a `value(t)` computed only from what it reads
  (`strlen`, `strnlen`) is replaced by the string guard that computes `t`.
- A `wide` row scales its `count` terms by the module's `wchar_size`; its
  strings are not scanned (a known gap: a wide string argument is not
  guarded).
- A null the row accepts (`null-ok`) passes. The arguments a call releases
  or reallocates are not guarded; the allocator validates them.
- A copy's operands must not overlap: `llvm.memcpy` (except an aggregate
  assignment, which carries `!tbaa.struct`) and a row with a `disjoint`
  clause get a `__weavec.disjoint` marker, and an overlap fails as
  `overlapping-copy`.

A function outside the table is not checked at the call; its own accesses
are guarded if `weavec-cc` built it (assumption A2).

## Unsafe regions

`WEAVEC_UNSAFE` on a function definition or a compound statement makes its
source range unsafe (§2.6). `UnsafeRegions` collects the ranges by file
over the AST before code generation, only when the unit expanded the macro
or spelled the annotation, and also records as unsafe every function its
author keeps from AddressSanitizer: `no_sanitize("address")`,
`no_sanitize_address` and `disable_sanitizer_instrumentation` (amendment 8),
so a conservative collector's stack scan needs no source change. GuardInsert
leaves each access whose instruction's location lies in a region unguarded
(it gets a ledger row with reason `unsafe` and no marker), and its
`array-bounds` checks are removed. An access inlined from an unsafe
function keeps the location of its source, and so stays unguarded.
`WEAVEC_UNSAFE` is the only annotation that changes generated code.

## The enforcement ledger

GuardInsert records each access it collected, before any rule removes its
guard, in an `EnforcementLedger` (`lib/Frontend/EnforcementLedger.cpp`,
§9 and amendment 10); GuardPrune and GuardExpand record what became of
each copy of its marker. A row has its function, file, line, column,
operation (`load`, `store`, `rmw`, `copy`, `set`, `lane`, `call:<name>`),
bytes (0 when computed at run time), outcome and reason:

| Outcome | Reasons |
| --- | --- |
| `guarded` | `access`, `range`, `string`, `checked-call` |
| `proven` | `in-bounds`, `dominated`, `merged`, `optimized` (the optimiser removed every copy of the access) |
| `unguarded` | `unsafe` |

`-fweavec-ledger=<path>` writes the rows as JSON, `weavec-ledger` version 3
(`EnforcementLedgerVersion`): an object with `schema`, `version`,
`producer` and `units`, each unit with its `source`, `object`, `target`,
`config` (`checks`, `zeroInit`), `summary` (`accesses`, `proven`,
`guarded`, `unguarded`) and `rows`. A path ending in `/`, or an existing
directory, receives `<object>.ledger.json` per unit; a file receives one
ledger, and an invocation that would write more than one is an error. Files are written
through a temporary renamed into place. `-fweavec-summary` (the default
when a ledger is written) prints the summary line on standard error:

```
weavec: lookup.c: 3 accesses: 1 proven, 2 guarded, 0 unguarded
```

The enforcement ledger is not the analysis's ledger: `guarded` and the
reasons above are words of the enforcing build only.

## The runtime

`runtime/` holds the only code WeaveC links into user programs. It is C,
built for the host into two archives installed under `lib/weavec` next to
`weavec.h`. [`weavec_rt.h`](../runtime/weavec_rt.h) is its internal
interface (the shadow encoding, the descriptors, the entry points), shared
by its sources and its test and not installed; compiled code reaches the
runtime only through the entry points the guard passes declare, the shadow
descriptor and the standard allocation functions.

| Archive | Source | Holds |
| --- | --- | --- |
| `libweavec_rt.a` | [`weavec_alloc.c`](../runtime/weavec_alloc.c) | the size-class arena allocator, its shadow writes, the quarantine and huge blocks; the shadow's reservation |
| | [`weavec_guard.c`](../runtime/weavec_guard.c) | the guards' slow paths (`__weavec_rt_guard`, `__weavec_rt_range`, `__weavec_rt_range_ok`, `__weavec_rt_strlen`, `__weavec_rt_overlap`, `__weavec_rt_null`), the stack's unpoisoning (`__weavec_rt_unpoison_stack`, `__weavec_rt_alloca_poison`, `__weavec_rt_alloca_unpoison`), global registration, and the weak `__ubsan_handle_out_of_bounds[_abort]` |
| | [`weavec_libc.c`](../runtime/weavec_libc.c) | the checked library wrappers `__weavec_rt_<name>`, `mmap` and `munmap` included |
| | [`weavec_owner.c`](../runtime/weavec_owner.c) | the table of entry points `__weavec_rt_dispatch`, and on Darwin the choice of the copy that owns the process |
| | [`weavec_report.c`](../runtime/weavec_report.c) | reports, `__weavec_rt_fatal`, `__weavec_rt_trapping` and the counters |
| `libweavec_alloc.a` | [`weavec_malloc.c`](../runtime/weavec_malloc.c) | the standard allocation functions over the arena, for the image the archive is linked into |

### The allocator

The allocator reserves one arena inside the shadow's window at first use:
one region of 2 GiB (1 GiB when that cannot be reserved) per size class,
followed by the metadata. The classes are 16 to 128 bytes in steps of 16,
then four per doubling up to 2^30 bytes. A request takes a slot of the
smallest class strictly larger than it, so at least one byte after every
object belongs to no object. Each slot has one 32-bit word of metadata,
`size << 2 | state`; free lists and the quarantine queues live in the
metadata, never in freed memory. An allocation writes 0 over the object's
whole granules, k over a partial last granule and `0xFA` over the rest of
its slot (up to 64 KiB); a release writes `0xFD` over the slot. The shadow
of a large object is mapped afresh rather than written, so untouched large
blocks cost no memory. Every block is zero-filled.

A released slot waits in a quarantine (16 MiB by default,
`WEAVEC_RT_QUARANTINE=<bytes>`) before it is reused, so a use after free or
a double free traps while the block is in it; after reuse a stale pointer
sees the new object. A request no class can hold is mapped on its own as a
*huge block*, kept in a table, with at least one poisoned granule after it.
The allocator validates every release: a release of an interior pointer, of
a released or never-allocated slot, or of a stack or global object stops
the program through `__weavec_rt_fatal`. A pointer it does not own goes to
the next allocator (the pointer's own malloc zone on Darwin, `__libc_free`
and `__libc_realloc` or the next `free` and `realloc` in lookup order
elsewhere).

`libweavec_alloc.a` defines `malloc`, `calloc`, `realloc` and `free`
strongly; the rest of the family (`reallocarray`, `aligned_alloc`,
`posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized`, and
`malloc_size`, `malloc_good_size`, `reallocf` on Darwin or `memalign`,
`pvalloc`, `malloc_usable_size` elsewhere) is weak, so a program's own shim
over `malloc` replaces it and keeps the arena. On ELF the executable's
definitions serve every shared library.

### One runtime per process on Darwin

Every image linked by `weavec-cc` carries the archives. On Darwin the copy
whose `__weavec_rt_dispatch` `dlsym(RTLD_DEFAULT, …)` finds is the *owner*;
every other copy takes the owner's arena and shadow descriptors and
forwards to it, so one arena, one shadow and one quarantine serve every
image (RFC 0033 §6.2). A table of another layout (another WeaveC version's
runtime) is not used. The arena is also registered as a malloc zone and
promoted to the process's default zone, so `malloc` from any image, the C
library's own (`strdup`, `getline`, `asprintf`) included, is served by the
arena; blocks the system's zones allocated before that go back to them.
Start-up, the shadow's and the arena's reservations included, costs about
1 ms.

### Reports

A failed guard prints one line on standard error and traps (`SIGTRAP`, or
`SIGILL` on x86-64):

```
weavec: <kind> at <file>:<line>:<column>: <read|write> of <n> bytes at 0x<addr>
```

with `<unknown>` for a missing location. A heap address gets a second line
that places it (`… is <k> bytes after the <m>-byte heap object at …`, or
`… is inside a released heap block at …`). The kinds are
`heap-buffer-overflow`, `heap-use-after-free`, `stack-buffer-overflow`,
`stack-use-after-scope`, `dynamic-stack-buffer-overflow`,
`global-buffer-overflow`, `null-dereference`, `unterminated-string`,
`overlapping-copy`, and `buffer-overflow` or `invalid-access` for poison
the runtime cannot attribute. An index check prints
`weavec: index-out-of-bounds at <file>:<line>:<column>: index <i>`, and a
monitor that fails in verify mode `weavec: weavec.proven: <kind> at …`. The
allocator stops an invalid release with
`weavec: invalid release of 0x<p>: <why>`, without a location.

Reports are formatted by the runtime itself, without `snprintf` (which a
program may define) and without allocating. Before every trap,
`__weavec_rt_trapping` unblocks `SIGTRAP` and `SIGILL` in the thread and
resets an ignored disposition, so the trap ends the program even when it
blocked them (a handler the program installed still runs). In report mode
(`-fweavec-checks=report`) each site reports once and the program goes on,
with no guarantee; `WEAVEC_RT_ABORT=1` makes report mode trap.
`WEAVEC_RT_REPORT_LOG=<path>` appends reports to that file instead of
standard error (ignored in set-uid programs). `WEAVEC_RT_STATS=1` prints
the runtime's counters at exit as `weavec: runtime: <n> <what>`
(allocations, releases, recycled slots, huge blocks, slow guards, range
guards, string guards, stack unpoisons); on Darwin only the owner prints,
counting every image's work. Run-time reports are not diagnostics and have
no ids. The case runner and the corpus gate attribute traps through this
output.

## Linking

`addRuntimeLibraries` (`lib/Frontend/Driver.cpp`) adds
`-u malloc libweavec_alloc.a libweavec_rt.a` to every link job of an
enforcing build that targets the host, before the first library of the
line, and `-lpthread` on Linux. An archive member is linked only when
referenced, except the allocator, which `-u` forces in. Missing archives
are an error: `cannot find the WeaveC runtime (libweavec_rt.a,
libweavec_alloc.a), which an enforcing link carries (-fweavec-checks=none
builds without it)`.

`runtimeObstacle` reads the command line for a reason the build cannot use
the runtime: `-ffreestanding`; `-nostdlib`, `-nodefaultlibs` or `-nolibc`;
a sanitizer that replaces the allocator (`address`, `hwaddress`, `memory`,
`thread`, `leak`, `kernel-address`); or a target other than 64-bit Darwin
or Linux. The units are then compiled without guards, as under
`-fweavec-checks=none`, and the link prints
`weavec-cc: note: building without the WeaveC runtime (<reason>): memory
accesses are not guarded`.

When an input of the link defines a symbol that `libweavec_alloc.a`
defines strongly (`allocatorDefinedBy` reads the set from the archive
itself), `dropAllocatorIfDefined` takes the archive and its `-u` off the
line and prints a note: the program keeps its allocator, its heap is
untracked, guards pass on it, and releases are not validated. Stack and
global objects are still guarded.

A Darwin link with `-flto` gets `-lto_library` naming the libLTO of the
LLVM WeaveC was built with, when the driver's default does not exist. There
is no link step: a link analyses nothing.

## The advisory analysis

The analysis reads the AST of a unit and decides, for each safety facet
(spatial, null, temporal) of each memory operation, or *site*, one outcome:
`proven`, `violation`, `unresolved` with a reason, or `trusted` with a
reason (RFC 0030 §2 as RFC 0035 §8 leaves it). It reports diagnostics: a
definite finding is an error and a possible temporal finding a warning. The
outcomes stay in memory and feed a summary line; nothing the analysis
concludes reaches the generated code.

- **`weavec`** (`tools/weavec`) analyses each source of its command line or
  compilation database (with `-p` and no source, every file of it), prints
  the diagnostics, and exits 1 after an error. A compile command that asks
  for `-fdiagnostics-format=sarif` is an error. It prints one summary line
  per unit:

  ```
  weavec: example.c: 11 sites: 6 proven, 3 not proven, 2 violations, 0 trusted; 2 errors, 1 warning
  ```

  where *not proven* counts `unresolved` sites.
- **`weavec --whole-program`** analyses the files as one program
  (`ProgramAnalysis`, RFC 0005): it discovers every unit's exports, orders
  the units by strongly connected component, analyses acyclic units once
  and cyclic groups to a fixpoint against a program database of the
  summaries analysed so far, and publishes in the last round only.
  `ProgramChecks` solves the program's function-pointer slots, propagates
  the boundary rows, and verifies each declaration's annotations and kinds
  against the defining unit (`annotation-mismatch`), from each unit's
  `InterfaceFacts`. Everything stays in memory. It adds a program line:
  `weavec: program <name>: N sites in M units: …`, named after the unit that
  defines `main`. A group of units that cannot be made to converge keeps
  its widened summaries, with a note.
- **`weavec-cc -fweavec-diagnose`** runs the analysis beside Clang's code
  generator in each compile job, per unit, with every finding a warning
  (`-Werror=weavec` makes them errors) and `leak` off by default
  (`-Wweavec-leak`). It prints no summary line, and the code is the same as
  without it. `-fweavec-budget=` and `-fweavec-unit-budget=` bound its
  work.

## `weavec::Core` — the model

`lib/Core` holds the model and depends only on the C++ standard library, so
it can be unit-tested without parsing code and reused by another frontend.
It never sees a `clang::VarDecl`, only an opaque `core::Handle` the
Analysis layer assigns, and never a `clang::SourceLocation`, only a
`core::SourceLocation` whose `opaque` field the frontend fills in.

### The ledger and its companions

| Header | Purpose |
| --- | --- |
| `Ledger.h` | Sites, facets and outcomes (RFC 0030 §2, RFC 0035 §8): `SiteKind`, `Facet`, `SiteOutcome` (`proven`, `violation`, `unresolved`, `trusted`), the closed reason lists, merging by rank, the defaults for undecided facets, `UnitLedger` and `Ledger` with requirement records and diagnostics, and the rollup behind the summary lines (`unitSummaryLine`, `programSummaryLine`). |
| `PointerKind.h` | The kind lattice (§7.1): `single`, `counted(e)`, `sized(e)`, `ended-by(q)`, `nul-terminated` or `unknown`, with nullability, a source (declared, inferred, default) and an `ExtentTerm` over a sibling parameter or field. Every kind is a lower bound. `ExtentClass` says whether an extent is exact, declared or a lower bound; only an exact extent can make an access a violation. |
| `LibrarySpec.h`, [`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt) | The one declarative table of C library, POSIX, platform and builtin functions (§8), and its parser: per argument, the access, required length, nullability, ownership effect, release family, state slots and callback clause; per call, the result, disjointness, `exits`/`noreturn`, format arguments, fortified aliases, the `wrapper(NAME)` the guard passes redirect to, and `wide`. It also lists the platform headers of §5.2. CMake embeds the text. Both the guard passes and the analysis read it. |
| `FnSlots.h` | Function-pointer slots (§9.3): the constraints `f ∈ S`, `S ⊆ T` and `open(S)` over field, global, parameter, result and local slots, and the solver that computes each slot's targets and whether it is closed. |

A site is one operation in one emitted function, identified by
`{function, ordinal, kind, location}`. Its kind is `deref`, `index`,
`ptr-arith`, `cast`, `int-to-ptr`, `lib-call`, `release`, `call` (a call or
a function exit), `assume` or `raw` (§2.1), and a facet exists only where it
has meaning. `assume` sites have a fourth facet, *assertion*. Records of one
facet merge by rank within one pass: `violation > unresolved > trusted >
proven`. The unresolved reasons are `undecided` (the facts neither prove
nor refute the facet), `unknown-extent`, `unknown-index`, `inexpressible`,
`may-released`, `may-moved`, `may-alias-released`, `may-invalid-release`,
`may-mismatched-release`, `may-dangle`, `may-conflict`, `unknown-callee`,
`callback`, `setjmp`, `budget`, `unanalysed`, `raw-cast`, `dangling-escape`,
`second-owner`, `no-zero-init` and `unconfirmed` (a definite finding the
replay did not confirm). The trust reasons are `unsafe`, `system-api`,
`library-spec`, `extern-contract`, `caller-contract`, `external-unit` and
`concurrency`. Adding a reason requires an RFC.

### The abstract domain

The object engine's domain (RFC 0031 §4) is in Core, so it is unit-tested
without Clang. Questions only the frontend can answer (whether two types may
name one object, the value of a cell this activation never wrote) go
through the `HeapOracle` interface the engine implements.

| Header | Purpose |
| --- | --- |
| `Heap.h` | Symbols, objects, cells and states (§4.1–§4.9). A `Sym` names one runtime value; `SymInfo` is what is known of it: for an integer its C type, defining operation and linear form; for a pointer its points-to targets (at most 8 `(object, offset)` pairs, else "any object"), nullness with the `allocatorSource` bit, `ReleaseRecord`, share count, raw origin, owning-slot ancestors (D6) and pending result cases; for a function value a set of at most 32 functions. `ObjectTable` interns objects per function by their origin (`Local`, `Global`, `Literal`, `Function`, `HeapRecent`, `HeapOld`, `Entry`, `EntrySummary`, `Materialized`, `Focus`, `CallResult`, `Unknown`), so two states name an object the same way. `ObjectState` holds an object's cells (by `CellKey`: a byte offset, a selected element cell over an index symbol, or the summary cell of an element position), element segments, extent, life (`Live`, `Released`, `MayReleased`, `UnknownReleased`, `Ended`, `MayEnded`), release record, family and ownership, and string facts. `HeapState` is one program point: objects, symbols, the zone and the values of expressions carried between blocks. `Heap` is the operations: loads and stores (strong or weak), releases, the temporal and spatial verdicts, distinctness (D1–D6), join, widening and garbage collection. |
| `Zone.h` | The numeric domain (§4.4): closed difference bounds `x − y ≤ c` between integer symbols, with symbol 0 for the constant zero; relational bounds for at most 64 symbols per state, the rest keep bounds against constants only. Joins and widenings are paired: the heap decides which symbol of each side a result symbol stands for. |
| `Persistent.h` | `PMap`, the sorted vector shared by reference count and copied on the first write through a shared handle, so a state is copied along a CFG edge for one reference (§4.8). |
| `Path.h` | `SummaryPath`: a root (`param(i)`, `global(g)` or `result`) followed by dereference, field and index steps. Summaries, boundary facts and pointer kinds name places this way. An anonymous or positional member is spelled by its byte offset (`.#16`). |
| `Effects.h` | The summary, `FunctionEffects` (see *Summaries* below), with the join, the widening and the global renumbering the program database needs. |
| `Integer.h` | Target integers of 1 to 64 bits, modular ranges of at most two intervals, conversions, and the checked arithmetic of invalid operations (RFC 0017). |
| `Diagnostic.h` | `Diagnostic`, `Certainty`, the ids in `diag::` with their default severities, `FixItHint` and `DiagnosticSink`. |
| `SourceLocation.h`, `Scc.h`, `AnalysisStats.h` | Frontend-neutral positions with an `opaque` slot; Tarjan's strongly connected components for call and unit graphs; work counters, never part of a proof. |

### Certainty

Every diagnostic is *definite* or *possible* (RFC 0030 §3). The
`ReleaseRecord` of RFC 0030 §3.1 lives on values and objects rather than on
places. It is definite when it has `allPaths` (it holds on every path merged
since), is not `conditional` (from an effect that holds only on some outcome
classes or paths, or from a `lossy` one), is not of unknown origin (the
unknown-callee default or an open slot, never diagnosed) and is not
`aliasOnly` (made by a weak merge of values that elements or aliases do not
tell apart, never diagnosed). A pointer's `allocatorSource` bit keeps a null
allocation result an `unresolved(undecided)` null facet rather than a
`null-dereference` error. A release that conflicts with a stored borrow is
a `conflicting-borrow` error only when the cell holds the borrow on every
path and the release is unconditional. A join keeps a record present on one
side only with `allPaths` cleared, and joins the lives of an object released
on one side and live on the other into `MayReleased`. A correlated bug is
therefore a warning, not an error.

### Summaries

`FunctionEffects` (`Effects.h`) is what a function does to its *entry
heap*, the objects its parameters and the globals reach, named by
`SummaryPath`s, and what it returns (RFC 0031 §6.1). It holds whether the
function returns, whether the summary is incomplete (and why), path effects
(release with family and interior offset, move, the unknown-callee default,
escape, share up and down), stores into entry cells, result alternatives
(null, a fresh object of a family with an extent term over the parameters,
an entry path, static storage, an integer range, a pointer to the callee's
dead frame), non-null facts per result class (RFC 0030 §9.2), and the paths
it reads and writes. Effects, stores and results carry RFC 0030 §9.1's case,
result classes optionally narrowed by a parameter's zero test, with `may`
and `lossy` bits; an effect on array elements carries its element range
(§4.9). Summaries stay in memory; `toText` spells one for the dumps.
Pointer kinds are not part of the summary.

## `weavec::Analysis` — the bridge

`lib/Analysis` is the only library allowed to include both `weavec/Core/*`
and `clang/*`. Besides the engine (next section), it holds shared services
and the components around the engine seam.

| Service | Role |
| --- | --- |
| `Annotations.h`, `ClangLocation.h` | Recognise WeaveC annotations on declarations, statements and function-pointer types, including a function's ownership signature; convert source locations both ways. |
| `ProgramDatabase.h` | What a unit exports (`UnitExports`: summaries with linkage and type keys, imports, indirect-call types, boundary rows, context requests) and the database of other units' exports, joined by name (`findEffects`) and by function type for indirect calls (`candidateEffects`), with globals renumbered by name (RFC 0031 §7). |
| `KindTable.h`, `SourceTerm.h` | The kind of every parameter, result and slot with its source (§7); a quantity spelled over C names at a site, with which the engine names the symbols of its messages (`'b->cap' bytes`). |
| `Concurrency.h`, `BypassedDeclarations.h` | The shared set G of RFC 0030 §5.3 (what threads and signal handlers reach); the locals a jump can bypass, so zero-initialisation does not reach them (§11). |

Before the engine, these components read the AST and the `LibrarySpec`, with
no engine fact:

- `AttributeReader` reads the declared kinds (§7.2): the `WEAVEC_*` extent,
  string and nullability macros, Clang's `counted_by`, `sized_by`,
  `alloc_size` and nullability attributes, and `[static N]` and VLA
  parameters. `WEAVEC_*` annotations win over ecosystem attributes, which win
  over the `LibrarySpec` entry and then system-header attributes; a finding
  that rests only on the last is `trusted(system-api)`.
- `KindInference` fills the rest of the `KindTable` (§7.3–7.6): parameter
  kinds with their `reliesOnSingle` flags, result kinds, slot kinds (a
  greatest fixpoint that demotes `single` to `unknown` at any store that is
  not Single-valid), the must-access requirements R1–R5, store groups, and
  the counted-field candidates of §7.6 with their disqualifications
  (`weavec --dump-kinds` prints them). The engine runs the Houdini rounds
  over the candidates of the records the unit defines in its main file
  (`EngineInvariants.cpp`, RFC 0031 *Implementation amendments*); a
  surviving invariant gives its pointer field an exact extent.
- `SiteCollector` enumerates the sites of every emitted function, with their
  ordinals and facets, into the `SiteIndex` and the unit's undecided rows
  (§2.6). It runs after the kinds, because PtrArith and Cast sites exist
  only in required positions.
- `SlotCollector` collects the unit's function-pointer constraints for
  `FnSlots` (§9.3).

After the engine, `BoundaryInvariants` checks at every call boundary and
function exit that no place reachable from a parameter or global may hold a
released or dangling pointer (`unresolved(dangling-escape)`), and that no
two owning places may hold the same object (`unresolved(second-owner)`). At
a call the reachable places are the objects the arguments point to and the
globals; at the exit that returns to the caller they are the places whose
storage has died, the result included, because a release there is already
in the summary. It then downgrades every temporal facet the unit *proved*
for a place of a broken class, which is the facet that relied on the entry
assumption the boundary broke (§9.4). Under `--whole-program` the other
units' rows come along, so the propagation is program-wide.
`LedgerAdapter`, `SafetyEngine` and `ObjectEngine` form the seam described
in *The engine seam*.

`UnitPipeline` (`runUnitAnalysis`) runs the analysis of one unit. It builds
the kinds and the unit's slot solution, collects the sites, runs
`ObjectEngine` through an authoritative `LedgerAdapter` (a discarding one
for a silent fixpoint round), calls `finish`, and reports the diagnostics
in publication order. A discovery-only run returns the unit's exports
(`ObjectEngine::discover`) without analysing it. The §7.6 field candidates
it passes are empty. The Frontend calls it through
`analyzeTranslationUnit`.

## The engine: the object engine

`ObjectEngine` (`include/weavec/Analysis/ObjectEngine.h`) is the prover
behind the seam ([RFC 0031](rfcs/0031-object-engine.md)). Its facts live on
abstract objects and symbolic values: a pointer is a symbol with a
points-to set, memory maps object cells to symbols, and copying a value
copies its symbol, so every alias of a value sees every fact about it and
about the objects it points to. The engine's classes are declared in the
private header `lib/Analysis/Engine.h`:

| File in `lib/Analysis` | Responsibility |
| --- | --- |
| `EngineUnit.cpp` | `ObjectEngine` and `UnitRun`: the unit's call graph, summary rounds, the authoritative pass, owning slots, imported summaries and exports (§3, §7). |
| `EngineRun.cpp` | `FunctionRun`: the always-add CFG, the entry state, the fixpoint with widening at loop heads, the final (publishing) pass, object interning, materialisation of the entry heap, and the replay that confirms definite findings (§3, §4; RFC 0034 §6). |
| `EngineExpr.cpp` | `Transfer`'s evaluation: rvalues to symbols, lvalues to addresses (`evaluate`, `addressOf`), loads, stores, casts, pointer arithmetic, integer operations and conversions (§5.1, RFC 0017). |
| `EngineCalls.cpp` | `CallApplier`: callee resolution and the effects of summaries, contracts, library rows, platform declarations and unknown callees; indirect calls; alias contexts (§5.4, §6.6). |
| `EngineSummary.cpp` | Summary derivation at the exits and instantiation at calls (§6.2, §6.3). |
| `EngineDecide.cpp` | `Decider`: the temporal, null and spatial facets of every site, releases, invalid releases and conflicting borrows (§5.2, §5.3, §5.5). |
| `EngineLibrary.cpp`, `EngineStrings.cpp` | The requirement records of `LibrarySpec` arguments (each buffer argument's need against what it points into), `disjoint` ranges and format calls; RFC 0012's string facts over objects. |
| `EngineKinds.cpp` | What the kinds give the engine: parameter extents and nullness at entry, the accesses a must-access requirement covers, and the requirement records at calls (RFC 0030 §7.2–§7.5). |
| `EngineContexts.cpp` | Contexts across units: the portable keys of the contexts a call asks of another unit's function, and the runs that serve the contexts other units asked (§7 *Amendment (cross-unit contexts)*). |
| `EngineInvariants.cpp` | Counted-field invariants: Houdini over `KindInference`'s candidates, and the functions that read a standing invariant's field analysed again with it (RFC 0030 §7.6, restored by RFC 0031). |
| `EngineLifetimes.cpp` | Frame storage whose lifetime ended, the boundary facts of calls and exits with their place classes (the class of a cell is the field that holds it, `fieldHolding`, so an element of an array is of its array's class), `WEAVEC_ASSUME`, raw-pointer laundering (§5.6, §5.7). |
| `EngineAnnotations.cpp` | `annotation-mismatch` of a definition against its own declaration (RFC 0003 reconciliation, RFC 0012 sized-field stores). |
| `EngineIntegers.h` | Target integer types of Clang types and bit-fields, the overflow builtins and operators (RFC 0017). |

### The domain

Every value is a symbol (§4.1): an integer, whose numeric facts live in the
zone; a pointer, with its targets, nullness, release record, share count,
raw origin and the owning slots it was derived from; a function value; or
unknown. An object (§4.2) has an origin that names it deterministically: a
local, a global, a literal, a function, the most recent or the older
allocations of a site (the recency abstraction), an entry object named by a
`SummaryPath` from a parameter, a global or a callee's result, a k-limited
entry summary, a callee's result, or the unknown object behind a raw
pointer. A singular object stands for one runtime object and takes strong
updates and definite releases; the others take weak ones. Cells are keyed
by byte offset from Clang's record layout; a variable index names a
*selected* cell over its index symbol, and what stores through indices the
engine cannot name wrote goes to the element position's summary cell.
Segments `[from, to) ↦ v` describe ranges of elements, so a cleanup loop
releases a range and a summary can say so (§4.9). Bytes that code the engine
does not see rewrote are *forgotten*: the whole object (`havocked`), a byte
range (a member copied over, a union a callee wrote), or a range forgotten on
some paths only, where an unwritten cell reads as its value otherwise merged
with an unknown one. A value also records the entry cells it was computed
from (`entryOrigins`), which the boundary propagation follows (RFC 0031
*Implementation amendments*).

Entry objects are materialised on first load, with the kind of the slot
they were loaded from (§4.6). An entry path in which one field repeats more
than twice, or longer than 6 steps, folds into an entry summary object. At
a join where a variable points to a different object on each side, the join
makes a *focus* object with the two as candidates; an entry or focus object
that a path released and no root reaches any more becomes a *dead copy*,
which keeps the release for the summary without overlapping the live object
of the next iteration (RFC 0031 *Implementation amendments*). Garbage
collection at every block end drops what no root (the locals, the
parameters' cells, the globals, the carried expression values, the result)
reaches; dropping an owned, unreleased, unescaped allocation is a leak.

Two pointers may be equal unless one of the distinctness rules D1–D6 (§4.5)
separates their targets: incompatible types under C's effective-type rules,
two owning places, the owner forest (an object reached through an owning
step is distinct from the objects on its path), freshness, identity of two
singular objects the engine created, and derivation through owning slots.
The owner forest is the acyclicity clause RFC 0031 adds to assumption A3.

### Transfer

`FunctionRun` evaluates Clang's CFG built with `setAllAlwaysAdd()`, so
every subexpression is an element in evaluation order and `?:`, `&&`, `||`
and `,` need no special order; an expression's value is kept in the block's
memo, and values a later block reads travel in the state (§2). There is no
separate IR. Blocks are visited from a worklist in reverse post-order; a
loop head widens after two joins, jumping grown bounds to the program's
constants or the type's limits. A condition edge adds its constraint to the
successor's zone and prunes the edge when it is unsatisfiable (§4.4).
States are persistent maps, so a state is copied per edge for one
reference and a join costs the size of the difference (§4.8).

Loads and stores go through addresses (§5.1): `x` is its object's cell,
`*e`, `e->f` and `e[i]` add the field's offset or the scaled index to the
pointer's targets, pointer arithmetic moves the offset term, and pointer
casts keep the symbol. An integer-to-pointer conversion makes a pointer of
unknown provenance to the unknown object, whose accesses are
`unresolved(raw-cast)` (RFC 0033 §2: only `WEAVEC_RAW` declarations make
raw values, and a merged value is raw only when every value merged is); a
pointer read back from reinterpreted bits is `raw-cast`; a construct the
engine does not evaluate yields unknown values and `unresolved(unanalysed)`
for its sites. `memcpy`, `memmove` and record assignments copy leaf by
leaf, every source cell read before any is written. A run whose work (the
sizes of the states it transfers and joins) exceeds the budget
(`--budget`, `-fweavec-budget`), whose entry states hold more than
1,000,000 symbols and objects, whose graph has more than 100,000 blocks, or
that visits one block more than its limit stops (RFC 0034 §7): its facets
take the defaults with reason `budget`, and its summary is incomplete.

### Confirmation

A definite finding of the authoritative run is a candidate
(`FunctionRun::confirmCandidates`, RFC 0034 §6). The function is replayed
in witness mode: a depth-first search along single paths with no joins
(at most 256 paths and 20,000 transfers), each block transferred with the
evidence its fixpoint entry state had, a loop left from its fixpoint state
after two turns, and an edge that refinement or a symbol's excluded
constants (`SymInfo::excluded`) empties ending the path. A candidate some
feasible path reaches, and that every such path decides alike, stays an
error; otherwise `LedgerAdapter::unconfirm` makes its facet
`unresolved(unconfirmed)` and its diagnostic a warning with the note
`not confirmed on a feasible path`.

### Decisions

After the fixpoint, the final pass transfers each reachable block once more
from its entry state and decides every site `SiteCollector` enumerated
there (§5.2), publishing through `LedgerAdapter`. The tables of RFC 0030 §3
apply; their premises are read from the operand's value at the site. A
definite release record on the value is a violation and a possible one a
warning; a released target the value has no record of is
`unresolved(may-alias-released)`; a target an unknown callee reached, or a
pointer into the unknown object, is `unresolved(unknown-callee)`. Null
facets are proven, `unresolved(undecided)` or, for a definite null that is
not an allocation result, a violation. A spatial access is compared, in the
zone, with the extent of every target: in bounds for all targets is proven,
out of bounds for every value against an exact extent is a violation, an
undecided access against an exact or declared extent is
`unresolved(undecided)`, and one that only a lower-bound kind covers
`unresolved(unknown-extent)`. Diagnostics are reported once per site and
id, with RFC 0030's messages and notes taken from the records. Nothing is
suppressed in a `WEAVEC_UNSAFE` region, and in a function that calls
`setjmp` every temporal facet is `unresolved(setjmp)`.

### Calls

A call's own sites and its boundary facts are decided from the state before
its effects. `CallApplier::applyDirect` then resolves a direct callee
(§5.4): the summary of a definition in the unit, or under `--whole-program`
the program database's (`UnitRun::summaryOf`); a declared ownership
contract that covers every pointer argument; the declaration's ownership
annotations; the `LibrarySpec` row; a platform-header declaration, which
borrows its arguments under `trusted(system-api)`; else the unknown-callee
default of RFC 0030 §5.1. That default marks every object reachable from a
pointer argument through non-`const` pointees, every escaped object and
every object reachable from an externally visible global as
`UnknownReleased` and forgets their cells; a cell whose address is passed
(`&cmd`) holds a fresh value of unknown nullness afterwards. A library row's
`alloc` creates a recent heap object of the row's family with the extent the
row gives, and its argument requirements become requirement records;
lengths that may be zero make the null requirement `null-if-zero`.

Indirect calls resolve through the flow-sensitive function value, then the
slot solution (RFC 0030 §9.3). Several targets are each applied to a copy
of the state and the results joined. An open slot without targets gets the
unknown-callee default with reason `callback`. At a call whose pointer
arguments (or globals) point into the same object, the callee is re-analysed
in an *alias context* with those entry objects unified and the constant
integer arguments bound, at most 16 contexts per callee and 3 deep (§6.6).
The context run's adapter only collects; each use-after-free, double free or
use-after-move it finds is reported with the note "called here with related
pointer arguments" and linked to the call's temporal facet. Under
`--whole-program` a call into another unit asks the defining unit for the
callee's context (`EngineContexts.cpp`, RFC 0031 §7 *Amendment (cross-unit
contexts)*), which runs it, exports its summary and reports what it finds;
a run that asked a context not yet served reports nothing until it has
been.

### Summaries

A summary is read off the exit states against the entry heap (§6.2). An
entry object that is released, moved or reached by an unknown callee gives
the effect for its path; a cell of an entry object or global that was
written gives a `store`, told apart from an unchanged cell by the entry
value each materialised symbol remembers; a result pointing to a new object
gives a fresh result, with its extent re-expressed over the parameters
when the zone relates it to one, and the new object's contents as stores
below `result`. An exit whose result carries pending cases is derived once
per result class. Effects on some exits only are keyed by the result
classes that separate them, by a parameter's zero test, or both, and are
otherwise possible; a release records what the state knew of the releasing
function's unmodified integer parameters (RFC 0031 *Implementation
amendments*).

At a call the summary is instantiated (§6.3): each path is walked through
the caller's memory from the arguments and globals, materialising entry
objects as a load would, every store's place and value are read before any
is written, and each effect is applied to the objects the path reaches,
weakly when it reaches several. A fresh result becomes a heap object named
by the call site. A case keyed on the result becomes a *pending case* on the
result symbol, applied when a later test of the result selects its class;
cases the arguments already decide are applied at the call. An incomplete
summary adds the unknown-callee default to its known effects (RFC 0030
§5.5).

### The unit driver

`UnitRun` drives a unit (§3). It computes the unit's owning slots (the
fields and globals some function releases a value loaded from, D2), builds
the call graph over the unit's definitions (direct calls, plus the targets
the slot solution gives indirect calls), and takes its strongly connected
components bottom up (`core::Scc`). A recursive component iterates its
members' summaries from empty through a discarding adapter, at most 8
rounds; summaries that have not converged are marked incomplete. Then each
member gets one run: authoritative (`LedgerAdapter::beginFunction`) for a
function the unit reports, a summary run otherwise. Outside a cycle that run
also produces the function's summary, so a leaf function is analysed once.
`exports()` gives the summaries of the external and address-taken
definitions, with globals renumbered by portable name, and the unit's
imports and indirect-call types; `discover()` gives the same without
analysing anything.

Under `--whole-program`, the same driver runs with the program database and
the program-wide slot solution (RFC 0030 §13.2 as RFC 0035 §8 keeps it). A
callee another unit defines is known by its imported summary, whose globals
are renumbered into the unit's; an effect through a global the importing
unit does not declare makes the summary incomplete there. An indirect call
that neither its value nor the slots resolve reaches every address-taken
function of its type, the unit's own and the database's joined candidate
summary; in a unit alone it keeps the unknown-callee default.

`SafetyEngine::dump` is the hook of `--dump-analysis`: `ObjectEngine::dump`
re-runs a function and prints the entry state of every block, its objects
with their kind, name, extent and life, their cells with the symbols they
hold, and the zone. With `WEAVEC_ENGINE_DUMP` set, the engine prints every
summary it computes or imports to stderr; `WEAVEC_ENGINE_DUMP=2` adds each
run's exit states and `WEAVEC_ENGINE_DUMP=3` each run's block states.

RFCs [0001](rfcs/0001-ownership-model.md) (model),
[0002](rfcs/0002-intraprocedural-checking.md) (dataflow),
[0003](rfcs/0003-signature-inference.md) (summaries),
[0004](rfcs/0004-unsafe-boundaries.md) (unsafe boundaries),
[0005](rfcs/0005-whole-program-analysis.md) (whole program),
[0006](rfcs/0006-precision.md) (precision),
[0007](rfcs/0007-resource-lifecycle.md) (resources),
[0008](rfcs/0008-pointer-validity.md) (validity) and
[0009](rfcs/0009-value-conditional-behaviour.md) (guards) specify the model.
RFC 0030 restates them and RFC 0031 restates over objects how their facts
are represented; RFC 0035 §8 keeps that analysis as an advisory one.

## The engine seam

Everything an engine produces flows through one interface (RFC 0030 §14),
which is how RFC 0031 replaced the engine by implementing `SafetyEngine`
without touching the ledger, kinds or library table. The types are in
`include/weavec/Analysis/SafetyEngine.h` and `LedgerAdapter.h`.

`EngineInput` is everything an engine gets for one unit: the `ASTContext`,
the `SiteIndex`, the `KindTable` with what `KindInference` found besides it,
the `LibrarySpec`, the unit's slot constraints and their solution (local,
or program-wide under `--whole-program`), the `ProgramDatabase` of the other
units, the field candidates assumed at entry, and `EngineOptions` (the
function and unit budgets, zero-initialisation, strict aliasing, the
functions to report, the dump stream and statistics). `LedgerAdapter` is
the only channel back:

| Method | Carries |
| --- | --- |
| `beginFunction` | the start of a function's authoritative pass; rows from earlier passes are discarded |
| `decide` | one outcome, with reason and detail, for one facet of a known site; records merge by rank |
| `decideAs` | the same, naming the site kind and boundary, for a statement that stands for several sites (a call that does not return, and its exit) |
| `suggest` | the annotation a row's fix-it offers; the first suggestion of a pass stands |
| `requirement` | one requirement record of a LibCall, Release or Call facet, kept with its own outcome |
| `report` | a diagnostic with its certainty, linked to its site and facet |
| `boundary`, `reliesOn` | the places reachable from parameters and globals that may hold released pointers or aliased owners, with place classes (an owning cycle is an aliased-owner fact with its `cycle` flag, RFC 0031 §8); the place class a proven temporal facet rests on |
| `overBudget` | a function that exceeded its budget |
| `storeVerdict` | a store group's verdict on a field-invariant candidate: holds, violated or unknown (kept for §7.6; the object engine publishes none) |
| `unconfirm` | a definite finding the replay did not confirm |
| `finish` | fills the defaults, applies the unsafe, `setjmp`, concurrency and boundary rules, and returns the unit's `core::Ledger` |

An adapter is authoritative, discarding (summary and fixpoint rounds keep
nothing), collecting (alias-context runs decide no row and keep their
diagnostics for the caller), or a witness (the confirming replay: nothing is
published, every decision goes to an observer). A decision about a statement
`SiteCollector` did not enumerate is an internal error, and an
`unresolved(unanalysed)` row in a release build.

`SafetyEngine` is what an engine implements: `analyzeUnit(input, out)`,
`exports()` for the program database, and `dump(function, os)` for
`--dump-analysis`. `ObjectEngine` implements it.

Two rules keep the seam honest, and gate H2 (`scripts/check-hygiene.py`,
RFC 0031 §2) checks both:

- The engine publishes nothing except through `LedgerAdapter`, diagnostics
  included. It receives no `DiagnosticSink`.
- Only the `Engine*.cpp` files include `lib/Analysis/Engine.h`.
  `SiteCollector`, `AttributeReader`, `KindInference`, `SlotCollector`,
  `BoundaryInvariants` and `LedgerAdapter` itself never reach it.

## Heap postconditions (RFC 0013)

[RFC 0013](rfcs/0013-interprocedural-heap-state.md)'s constructors and
returned fields are ordinary summary content in the object engine. A result
pointing to an object the function created is a fresh result, and the
object's cells, down to the objects they point to, are stores below
`result`. A fresh value names which of the function's new objects it is, so
two fields initialised from one allocation stay aliases in the caller, and
two call sites create independent objects (§6.3). A store of a new object
keyed on a result class is weak, and the object is absent on the other
classes: a test of the result that selects them disowns it, so a
constructor whose failure path stored nothing leaks nothing (RFC 0031
*Implementation amendments*). At a call the callee's stores are read before
any is written, so extraction (`p = *slot; *slot = NULL; return p`) stays
precise. Symbols are immutable, so an allocation's extent is the size's
value at allocation time whatever the size variable holds later.

## Pointer identity and call effects (RFC 0014)

A function value is a symbol holding a bounded set of at most 32 functions,
or unknown (§4.1), and an indirect call resolves against it before the slot
solution. Function pointers stored in fields and globals are resolved by
the slots of RFC 0030 §9.3, which replace RFC 0014's callback-global
fixpoint and its `callbackGlobals` export. A call through a closed slot with
one target is applied as a direct call, and with several targets as the
join of their summaries. An open slot with known targets gives their
temporal facts only, under `trusted(extern-contract)`; an open slot without
targets gets the unknown-callee default with reason `callback`. Every
indirect call has a null facet on its callee operand. A call through a
callback parameter takes the parameter's slot solution (RFC 0031
*Unresolved questions*).

Pointer arithmetic and pointer casts keep the symbol's targets, so
`free(p); use(p + 1)` is a use after free. `memcpy`, `memmove` and record
assignments copy pointer and integer leaves cell by cell; a pointer loaded
from a cell whose last store was of another type, or from a partial copy,
is `unresolved(raw-cast)` where it is used (§4.2).

## Arrays and containers (RFC 0015)

Array elements are cells of their object (RFC 0031 §4.9): a constant index
is a concrete cell, an affine index a selected cell over its index symbol
(at most 32 per object), and stores the engine cannot name go to the
element position's summary cell. Segments `[from, to) ↦ v` (at most 4 per
element position) describe ranges of elements, each holding its own value;
at a loop head the element cells an iteration changed fold into a segment
that grows with the induction variable, so `for (i = 0; i < n; i++)
free(a[i]);` releases `[0, n)`. A join that cannot keep a selected cell or
match a segment evicts it as a weak store to the elements it described.
Summaries carry element effects as ranges over constants or integer
parameters the body never assigns; a range that cannot be so expressed is
exported without one, as a possible effect on some elements. `realloc`'s
new block starts with the old block's cells and ranges below its size.

## Compositional calls (RFC 0016)

A summary is derived assuming distinct parameter objects except where
D1–D6 cannot separate them, where the entry objects are joined already and
the summary holds for aliased calls. Where a caller passes arguments that
point into one object, the callee is re-analysed in an alias context (§6.6;
see *Calls* above). A context run never decides the rows of the function it
analyses (§2.6); it keeps its diagnostics, each reported at the use in the
callee with a note naming the call and linked to that call's Call site.

## Target integers and compositional bounds (RFC 0017)

[RFC 0017](rfcs/0017-c-integer-semantics-and-spatial-safety.md) specifies
the target-integer model. Core represents integer types of 1 to 64 bits:
`IntegerValue` is an unsigned bit pattern, `IntegerRange` holds at most two
intervals, and transfers model unsigned wrap, signed validity and
conversions. An invalid operation supplies no invented value. The object
engine keeps each integer symbol's interval in its type's range and the
relations between symbols in the zone: `a = b + c` with a constant `c`
records `a − b = c`, other arithmetic computes intervals only, and a
comparison of an access against an extent with the same scale is a zone
query (§4.4). `EngineExpr.cpp` evaluates arithmetic, compound assignments
in their promoted type, conversions and the overflow builtins;
`invalid-integer-operation` is reported when the operands make the
operation invalid for every value (§5.10). A declaration or typedef of a
variable-length array captures its dimensions where it runs, and `sizeof`
and extents use the captured values; a subscript of a multi-dimensional
array is bounded by its own dimension (RFC 0031 *Implementation
amendments*).

C values and byte intervals are distinct: `malloc(n * sizeof(T))` receives
the actual C product, and a byte size that may wrap `size_t` gives no
extent, so a wrapped product can establish a violation but never that an
access fits. The spatial verdict needs a lower and an upper bound to prove
an access. It maps onto the spatial facet (RFC 0030 §3.3): a violation
against an exact extent is an `out-of-bounds` error; an undecided access
against an exact or declared extent is `unresolved(undecided)`; an access
that only a lower-bound kind covers is `unresolved(unknown-extent)`.
Call-site findings come from the must-access requirements of §7.5.

## `weavec::Frontend` — Clang and LLVM integration

`lib/Frontend` holds the guard passes and their ledger, adapts the analysis
to Clang's frontend machinery, runs the whole-program analysis, and drives
`weavec-cc`.

| Component | Role |
| --- | --- |
| `GuardPass` (`GuardPass.h`; `GuardPass.cpp`, `GuardPassImpl.h`, `GuardInsert.cpp`, `GuardLibrary.cpp`, `GuardLoops.cpp`, `GuardExpand.cpp`, `GuardFrames.cpp`, `GuardGlobals.cpp`) | The four passes of *Compile pipeline* and `registerGuardPasses`, which adds them through `CodeGenOptions::PassBuilderCallbacks` with the mode (trap, report, verify), the unit's unsafe regions and its enforcement ledger. |
| `EnforcementLedger` | The rows the passes record, their rollup, the `weavec-ledger` version 3 JSON writer and the enforcement summary line. |
| `UnsafeRegions` | The `WEAVEC_UNSAFE` and `no_sanitize` ranges of a unit, by file, and the AST consumer that collects them. |
| `FrontendAction` | `WeaveCAction`, an `ASTFrontendAction` for libTooling; `createWeaveCConsumer`, which `weavec-cc -fweavec-diagnose` runs beside Clang's code generator; and `analyzeTranslationUnit` and `analyzeRetainedUnit`, which run `UnitPipeline`. Every emitted function is analysed, including `static inline` functions from user headers (§5.6). |
| `AnalysisSummary` | The analysis's summary lines, with the `-W` flags applied to the counts. |
| `ProgramAnalysis` | The whole-program algorithm of RFC 0005 over an abstract `ProgramUnit`, for `weavec --whole-program`: discovery, the order by strongly connected component, fixpoints for cyclic groups, publication in the last round. |
| `InterfaceFacts`, `ProgramChecks` | A unit's interface facts (its definitions' kinds, its imports' declared kinds and ownership annotations, its slot constraints with local slots eliminated), and what `--whole-program` does with every unit's at once: `solveProgramSlots`, `programBoundaries`, `verifyDeclarations`. |
| `Driver` | `weavec-cc`: `DriverOptions` (WeaveC's flags, split from Clang's), the in-process `-cc1` jobs with the enforcement set-up and the wrapper action that adds the unsafe-region collector and the analysis, the runtime's link line (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`), and `--help-weavec`. |
| `DiagnosticControl` | Applies the `-W` flags by each diagnostic's id and certainty. An error can be lowered but never disabled, and a flag naming a removed id is refused. `FilteringSink` drops what an earlier run already reported. |
| `ClangDiagnosticSink` | Forwards `core::Diagnostic`s, with notes and fix-its, to Clang's `DiagnosticsEngine`, so they render exactly like Clang's own. |
| `ResourceDir`, `AnalysisStats` | Locate `weavec.h`, the runtime archives, Clang's resource directory and `clang`; write the work counters of `--analysis-stats`. |

## `tools/weavec` and `tools/weavec-cc`

`weavec` is a libTooling application (see *The advisory analysis*):
`weavec file.c -- <compiler flags>`, or `weavec -p build/ file.c` with a
compilation database. It injects `-isystem <resource-dir>/include` and
`-D__WEAVEC__=1`, so user code can `#include <weavec.h>`. `--budget` and
`--no-zero-init` model the analysis of a `weavec-cc -fweavec-diagnose`
build; `--dump-analysis`, `--dump-kinds` and `--analysis-stats` are
debugging aids.

`weavec-cc` is the drop-in compiler: `CC=weavec-cc make`. Its own flags,
which `weavec-cc --help-weavec` lists, are `-f[no-]weavec`,
`-fweavec-checks=trap|report|verify|none`, `-f[no-]weavec-zero-init`,
`-fweavec-ledger=<path>`, `-f[no-]weavec-summary`,
`-f[no-]weavec-diagnose`, the analysis's budgets and debugging output
(`-fweavec-budget=`, `-fweavec-unit-budget=`, `-fweavec-dump-analysis`,
`-fweavec-analysis-stats=`) and the `-W…weavec…` flags; an unknown
`-fweavec-*` flag is an error, and everything else is Clang's. When WeaveC
is on, the driver defines `__WEAVEC__=1` and adds `weavec.h`'s directory
with `-isystem`. `--version` prints Clang's version block, then
`weavec-cc version …`. The command line is that of RFC 0035 §7.

## Diagnostics contract

Every diagnostic carries a stable id from `weavec::core::diag`, printed as
`[weavec::<id>]`. Scripts and editors filter on ids, so renaming one is a
breaking change. Diagnostics come from the advisory analysis only: `weavec`
and `weavec-cc -fweavec-diagnose`. There are 18:

| Ids | Default severity |
| --- | --- |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`, `mismatched-release`, `invalid-release` | error when definite, warning when possible |
| `null-dereference`, `use-of-uninitialized`, `out-of-bounds` | error, reported only when definite |
| `unsafe-operation`, `annotation-mismatch`, `invalid-integer-operation`, `contradicted-assumption` | error |
| `leak`, `invalid-annotation` | warning (`leak` off by default under `-fweavec-diagnose`: `-Wweavec-leak`) |
| `allocation-failure` | warning, off by default (`-Wweavec-allocation-failure`) |

`diag::defaultSeverity(id, certainty)` gives these severities;
`-fweavec-diagnose` then lowers every error to a warning. Possible null and
spatial findings are `unresolved(undecided)` rows rather than diagnostics.
`-Werror` in project flags does not promote WeaveC warnings;
`-Werror=weavec[-<id>]` does. An error can be lowered with
`-Wno-error=weavec-<id>` but not disabled. RFC 0030 removed
`analysis-incomplete`, `annotation-required`, `checking-incomplete` and
`checking-failed`, and a `-W` flag naming one of them is an error; RFC 0035
removed `unresolved-operation` and `unchecked-operation` with the require
levels. Run-time reports are not diagnostics.

## Tests and gates

- **Unit tests** (`unittests/`, GoogleTest) test each component alone.
  `LibrarySpecTest.cpp` checks every library entry against an independent,
  hand-written expectation table, and `HeapTest.cpp` builds the state a
  violation of each domain invariant I1–I6 (RFC 0031 §4.7) would produce and
  checks that the decision is not proven. The Analysis tests run
  `ObjectEngine` over snippets (`unittests/Analysis/TestUtils.h`), and
  `EffectsTest.cpp` checks the join and the widening of summaries.
  `GuardPassTest.cpp` runs the guard passes on IR text; the other Frontend
  tests cover the summary lines, the `-W` flags, interface facts and the
  whole-program analysis.
- **The runtime test** ([`runtime/test/rt_test.c`](../runtime/test/rt_test.c),
  CTest `runtime`) is one C program linked with `libweavec_rt.a` and
  `libweavec_alloc.a`, so its `malloc` is the arena's. It tests the shadow
  and its encoding, the size classes, alignment, reallocation, the
  quarantine, invalid releases (in child processes that must die), huge
  blocks, foreign pointers, the guards, strings, the checked calls, globals,
  dynamic allocas, the stack's unpoisoning, mappings, the array-bounds
  handlers, threads and `fork`. Test names as arguments run only those.
- **Lit tests** (`test/Analysis`, `test/Annotations`, `test/Driver`,
  `test/Guards`, `test/WholeProgram`) pin exact messages and behaviour.
  `test/Guards` pins the passes' IR (FileCheck on `-emit-llvm`: `ir-*.c`)
  and, by building and running programs, the run-time behaviour, the report
  formats and the enforcement ledger; `test/Driver/runtime-*.c` pins the
  runtime's link line and fallbacks.
- **`test/cases`** is one tree of executable C cases by feature, with
  expectations as line-comment markers (see its
  [README](../test/cases/README.md)).
  [`scripts/run-cases.py`](../scripts/run-cases.py) analyses each case with
  `weavec` (`BUG`, `CLEAN`, `ALLOW`; `MISS: <reason>` on a `BUG` line the
  analysis is known not to report), builds it with `weavec-cc` (`GUARDED`,
  `PROVEN`, `UNGUARDED` and `EXPECT-LEDGER` on the enforcement ledger), and
  runs it, attributing every stop to a line and a kind (`TRAP: <kind>`,
  `TRAP-AT: <unit>:<line>`, and `STOP` in a `DETECT` case). `--asan` adds
  an oracle built with `-fsanitize=address,array-bounds`; `--checks verify`
  fails any `weavec.proven` report. CTest registers one `cases-<suite>`
  test per top-level directory, so `ctest -j` runs the suites in parallel.
- **`test/corpus`** pins real projects, built and tested with `weavec-cc`
  by [`scripts/corpus-gate.py`](../scripts/corpus-gate.py) (`--quick` on
  every pull request); see its [README](../test/corpus/README.md).

RFC 0035's acceptance gates (drop-in G1–G3, detection G4–G6, cost G7–G9,
hygiene H1–H2) are measured with these tools. `scripts/check-hygiene.py`
implements gate H2 of RFCs 0030 and 0031: no retired names outside
`docs/rfcs/`, no libc name comparisons outside the library table, no corpus
project named under `lib/`, the seam rules, and the line budgets, the
runtime's own among them (RFC 0032 gate H1).
[`scripts/codegen-identity.py`](../scripts/codegen-identity.py) checks that
objects built with `-fweavec-checks=none` are byte-identical to Clang's
(RFC 0030's gate G7).

## Build structure

- The top-level `CMakeLists.txt` builds `lib/` (Core, Analysis, Frontend),
  `tools/`, `runtime/` and, with `WEAVEC_BUILD_TESTS`, `unittests/` and
  `test/`.
- `cmake/WeaveCLLVM.cmake` finds LLVM and Clang and provides the
  `weavec::llvm` interface target and `weavec_link_llvm`/`weavec_link_clang`,
  which respect `LLVM_LINK_LLVM_DYLIB` and `CLANG_LINK_CLANG_DYLIB`.
- `cmake/WeaveCHelpers.cmake` provides `weavec_add_library` and
  `weavec_add_executable`. Only `USES_LLVM` targets get LLVM's include
  paths, and Core is not one of them.
- `lib/Core/CMakeLists.txt` embeds `LibrarySpec.txt` as a byte array at
  configure time; editing the table re-runs the configure step.
- `runtime/CMakeLists.txt` builds `libweavec_rt.a` and `libweavec_alloc.a`
  into the build tree's `lib/weavec`, position-independent, never as LTO
  bitcode, and always with `-O2` and without the allocation builtins,
  whatever the build type. With `WEAVEC_BUILD_TESTS` it also builds
  `weavec_rt_test`, which `test/CMakeLists.txt` registers as the CTest
  `runtime`.
- A CMake package config (`find_package(WeaveC)`) exports `weavec::Core`,
  `weavec::Analysis` and `weavec::Frontend` to external tools.
