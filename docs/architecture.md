# Architecture

WeaveC is a drop-in C compiler that enforces memory safety at run time,
built on Clang and LLVM. [RFC 0036](rfcs/0036-deterministic-heap-reuse.md),
*Deterministic heap reuse*, defines the design over the guards of
[RFC 0035](rfcs/0035-guard-by-default.md), *Guard by default*; each RFC's
*Implementation amendments* override its body.

`weavec-cc` guards every memory access of the C code it compiles, in LLVM
IR, against the runtime's shadow memory. It removes a guard only where a
simple local rule proves it redundant. The locals a guard can reach get
redzones in their frame (locals whose lifetimes never overlap share space),
globals get redzones, locals are zero-initialised, typed array indexes are
checked against their declared bounds and fortified library calls against
the object size they carry. The runtime's allocator leaves at least one
byte after every heap object that no object owns, and reuses a released
block only after a *sweep* of the program's memory finds no pointer to it.
A bad access traps with a report.

The code is two C++ libraries, one thin command-line tool and a C runtime.
The arrows point from a layer to what it may depend on.

```
                 ┌──────────────────────────┐
                 │  tools/weavec-cc         │
                 │  (drop-in C compiler)    │
                 └────────────┬─────────────┘
                              ▼
                 ┌──────────────────────────┐
                 │  weavec::Frontend        │  guard passes, unsafe
                 │  lib/Frontend            │  regions, enforcement
                 │                          │  ledger, driver
                 └──────┬───────────┬───────┘
                        ▼           ▼
        ┌────────────────────┐   ┌────────────────────┐
        │  weavec::Core      │   │  Clang / LLVM      │
        │  lib/Core          │   │  (external)        │
        │  the library table │   └────────────────────┘
        │  no Clang/LLVM     │
        └────────────────────┘
```

`runtime/` is C code that `weavec-cc` links into every enforcing link (see
*The runtime*). It depends on nothing above. The layering rules are strict:

- Core includes nothing from `clang/` or `llvm/`. It is the one library
  built without their include paths, and holds only the library table.
- The runtime includes no WeaveC header but its own
  [`weavec_rt.h`](../runtime/weavec_rt.h), and the entry points it declares
  there match the ones the passes declare (`scripts/check-hygiene.py`
  checks that each one the passes name is declared).

RFC 0036 is Accepted and being implemented; the [roadmap](roadmap.md) says
which stages have landed. This page describes the tree as it is. Earlier
designs (the advisory ownership analysis and the `weavec` tool, checked
mode, checks planned from the analysis, unit records and the link step) are
described in the RFCs that introduced and retired them.

## Compile pipeline

`weavec-cc` compiles a C translation unit as Clang does, in-process, with
these additions in an enforcing mode (`-fweavec-checks=trap`, the default,
`report` or `verify`; RFC 0035 §1 and amendment 1, RFC 0036 §1):

1. **Zero-initialised locals.** `-ftrivial-auto-var-init=zero`, for locals
   up to 4 KiB unless the command line sets its own limit
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
6. **Fortification** (§1, §6). On an optimising glibc build whose command
   line neither defines nor undefines `_FORTIFY_SOURCE`, the driver adds
   `-D_FORTIFY_SOURCE=3`; Darwin's SDK fortifies at level 2 by default. The
   guards check the object size a fortified call carries.
7. **ASan's contract** (§1, §5). The unit is parsed with AddressSanitizer in its
   language options, so `__has_feature(address_sanitizer)` is true,
   `__SANITIZE_ADDRESS__` is defined and `no_sanitize("address")` is
   accepted; at the end of the unit, before code generation, the option is
   taken out again, so ASan's instrumentation never runs, and GuardInsert
   removes the `sanitize_address` function attribute. Clang's
   `_FORTIFY_SOURCE=0` predefine for ASan on Darwin is undone unless the
   command line chose a level. Preprocessor-only jobs get the same macros.
   `-fno-weavec-asan-compat` keeps the language options as the command line
   set them, and a command line that asks for an address sanitizer itself
   keeps its own.
8. **The unsafe-region collector** (`UnsafeRegions`) runs over the AST
   before code generation (see *Unsafe regions*).
9. **Four LLVM passes** (`registerGuardPasses` in
   `lib/Frontend/GuardPass.cpp`), at every optimisation level:

   | Pass | Extension point | Files | Does |
   | --- | --- | --- | --- |
   | `GuardInsert` | `PipelineStartEP` | `GuardInsert.cpp`, `GuardLibrary.cpp` | puts a marker before each access and each memory argument of a library call, redirects the library calls whose wrappers write, records the ledger rows, drops `sanitize_address` |
   | `GuardPrune` | `PeepholeEP` | `GuardInsert.cpp` | after inlining, removes the markers rule 6.1 proves on a local or global at constant offsets, so that SROA still promotes those locals; manages the scope barriers |
   | `GuardLoop` | `VectorizerStartEP` | `GuardLoops.cpp` | rule 6.4: versions loops whose whole ranges one check covers |
   | `GuardExpand` | `OptimizerLastEP` | `GuardExpand.cpp`, `GuardFrames.cpp`, `GuardGlobals.cpp`, `GuardLibrary.cpp` | rules 6.1–6.3; the frame layout, shared space and scopes; global redzones; the unpoisoning before `noreturn` calls; the late library redirections; each marker into its inline check |

   `GuardPass.cpp` and the private header `GuardPassImpl.h` hold what the
   passes share: the module's context, the markers, the runtime's entry
   points, the site records, and the inline checks of the shadow. The
   passes run on 64-bit targets only, and skip declarations, naked
   functions and the runtime's own functions. An `available_externally`
   body (a C99 inline definition) is guarded, since the inliner copies it
   into the unit. Verify mode skips GuardLoop.

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
beside the lifetime markers inlining brought (RFC 0035 amendment 2).

What is collected (RFC 0035 §2.1): a `load`, `store`, `atomicrmw` or
`cmpxchg` is a guard of the bytes of its type; `llvm.memcpy`,
`llvm.memmove` and `llvm.memset` a range for each memory operand; a masked
load or store, a gather or a scatter one access per lane under its mask
bit; a call of a library-table function a range or a string for each
memory argument. An access to a stack or global object at a constant
offset inside it gets no marker and no ledger row, so the object stays
promotable to registers. Inline assembly operands, accesses in a
non-default address space and the passes' own shadow accesses are not
collected.

Nothing else changes: no AST is rewritten and code generation is not
deferred. A unit compiled with `-fweavec-checks=none` or `-fno-weavec`, or
as C++, Objective-C, OpenCL or CUDA, is compiled as Clang compiles it.

## Guards and the shadow

The runtime keeps one shadow byte per 16-byte granule (RFC 0035 §2.2 and
amendment 3, RFC 0036 §5; the values are in
[`weavec_rt.h`](../runtime/weavec_rt.h)):

| Value | Meaning |
| --- | --- |
| `0x00` | all 16 bytes addressable |
| `0x01`–`0x0F` | the first k bytes addressable, the rest not |
| `0xF1` | a frame's left redzone |
| `0xF2` | a redzone between a frame's objects |
| `0xF3` | a frame's right redzone |
| `0xF4` | a dynamic stack object's redzone |
| `0xF7` | poisoned by the program through ASan's interface |
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

**Fast paths** (RFC 0035 amendment 4). A guard whose bytes lie in one
granule is three instructions: the shift and mask, the shadow load, and a
branch on a nonzero byte, weighted cold. The partial-granule test (whether
the access ends within the first k bytes) is on the slow side of the
branch, before the call. An access of up to 16 bytes that may cross a
granule loads the two shadow bytes it may touch at once; a range of
constant length up to 113 bytes loads eight with a constant mask. A nonzero
byte only sends the access to the exact check. Any other range calls
`__weavec_rt_range(a, n, site)`, which scans the shadow.

**The slow path.** `__weavec_rt_guard(a, w, site)` decides exactly (the
window, partial granules, the poison kind) and names a failure by the
shadow value that caused it (see *Reports*). `site` is a constant
`__weavec_rt_site` record `{file, line, column, flags}` built from the
instruction's location (the flags say write, report mode, a monitor, and
whether a null string is accepted), or null. The slow paths are
`preserve_all` on AArch64 and x86-64, so an inline guard costs the code
around it no spills.

**Null bases** (RFC 0035 §2.4). An access whose address is `p + x`, with
`x` not a constant below 4,096 and `p` not known to be non-null, also tests
`p`: a null base takes the slow path, which reports a null dereference. A
constant offset below 4,096 from a null base faults on the null page, and a
guard on an address in the lowest 64 KiB fails as a null dereference
through its shadow, even when the optimiser moved the access to the other
arm of a `select`.

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
- **6.4 Loop ranges** (RFC 0035 amendment 6). GuardLoop versions a loop with a known
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

**Which locals are tracked** (RFC 0035 §3.1, amendment 5). A local (an `alloca`) is
tracked when a guard that remains may reach it: any of the objects a
guarded pointer may be based on, through phis and selects, is that local,
or its address escapes (stored, passed to a call other than a lifetime
marker or a memory intrinsic on itself, returned, or converted to an
integer). Other locals are left as they are.

**The frame** (`GuardFrames.cpp`, RFC 0035 §3.2). The tracked static allocas of the
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

**Shared space** (RFC 0036 §2, amendment 8). In a frame that would be
larger than 512 bytes, two tracked locals may share bytes when their
lifetimes never overlap: LLVM's `StackLifetime` analysis,
in its *may be live* mode, over the lifetime markers that remain after the
optimiser (the analysis SafeStack uses). Each local is placed at the lowest
32-byte-aligned offset after the left redzone where its object and redzone
meet no other local's that may be live with it, so locals in the disjoint
arms of a `switch` take the space of one, as in Clang's own frames. A local
without lifetime markers (every local at `-O0`) is live throughout and
shares nothing. At entry a shared granule is `0xF8` (out of scope); a
lifetime start writes its object addressable, and its redzone `0xF2` again
when the redzone overlaps another local's object, so that an overflow into
it reports a `stack-buffer-overflow`.

**Dynamic objects** (RFC 0035 §3.4). A tracked dynamic `alloca` (an `alloca` call, a
VLA, or a static alloca outside the entry block) of `n` bytes allocates
`round32(n) + 32` bytes, and its tail is poisoned `0xF4`. Before a stack
restore and before each return, `__weavec_rt_alloca_unpoison(top, bottom)`
clears the shadow of the dynamic objects made since.

**Non-local exits** (RFC 0035 §3.5). Before each call of a function that does not
return, other than the runtime's own and those that end the process
(`abort`, `exit`, the assertion failures, `err` and the like), the pass
calls `__weavec_rt_unpoison_stack()`, which clears the shadow from the
current stack pointer to the thread's stack base. A `longjmp`,
`siglongjmp`, `pthread_exit`, or an interpreter's error jump written in
assembly but declared `noreturn` then leaves no poison in the frames it
abandons. A frame left by code not built by `weavec-cc` (an exception
unwinding through C frames) keeps its poison until the stack is reused
(assumption A5).

**Globals** (`GuardGlobals.cpp`, RFC 0035 §4). A global the unit defines exactly is
tracked unless it is thread-local, in a named section, in a comdat, in a
non-default address space, a string literal or other mergeable constant
(constant, `unnamed_addr` and local), `common`, appending, externally
initialised, aligned above 4 KiB, of size 0, or one of LLVM's or WeaveC's
own. A tracked global is replaced by a global of the same name, linkage and
visibility that holds the original and a redzone (sized as a frame
object's), aligned to 32 bytes. A module constructor of priority 1 calls
`__weavec_rt_globals_register(array, n)` with
`{address, size, size with redzone}` per global, which poisons each redzone
`0xF9`. A shared library's or bundle's copy of the runtime sends the
registration to the process's owning runtime first (RFC 0036 §4), so its
globals are poisoned in the shadow its guards read. A module destructor
unregisters them and clears their shadow, so `dlclose` leaves nothing
behind.

## Library calls

`GuardLibrary.cpp` guards a direct call of a function of the library table
([`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt)) that the unit does not
define, before the call (RFC 0035 §2.5 and amendment 7, RFC 0036 §6):

- Each pointer argument the row reads or writes is guarded over the bytes
  its term gives (`bytes(t)`, `count(t)` times the element size, one
  element without a term), evaluated on the call's arguments; a `strlen`
  term is evaluated by the runtime inside the string's addressable bytes.
- A `str` argument is guarded as a string: `__weavec_rt_strlen(s, max,
  site)` scans it and fails with `unterminated-string` when the terminator
  is not addressable. A `printf`- or `scanf`-family call with a constant
  format guards each `%s` argument as a string and each `%n` argument as an
  `int`.
- **Fortified forms** (`chk(…)`, RFC 0036 §6). A call of a fortified alias
  (`__strcpy_chk(d, s, 8)`) whose object-size argument is not
  `(size_t)-1` checks that it writes no more than that size: inline, or by
  passing the size to its wrapper as `limit`. A write past the size within
  addressable bytes reports `intra-object-overflow`.
- A row's `wrapper(NAME)` clause replaces the call with
  `__weavec_rt_NAME`, with the call's arguments, a `limit` when the row has
  a fortified alias, and the site (first when the call is variadic). The
  wrappers (`runtime/weavec_libc.c`) of `strcpy`, `stpcpy`, `strcat`,
  `sprintf`, `vsprintf`, `snprintf`, `vsnprintf` and `gets` compute what
  the call writes before it writes; the `printf`-family ones check each
  `%s` and `%n` argument, through a `va_list` too; the `scanf` family checks
  what each conversion wrote; those of `memchr`, `strchr`, `strcmp`,
  `strncmp`, `strcasecmp` and `strncasecmp` check only the bytes the call
  read up to where it stopped; the number parsers (`strtol` to `strtold`,
  `strtoimax`, `strtoumax`, `atoi`, `atol`, `atoll`, `atof`) check after the
  call the bytes the C library read (up to the end pointer and the
  character that stopped it); `mmap` and `munmap` clear the shadow of what
  they map. A wrapper is named after
  its row's function (the parser checks it). The calls that write through
  no pointer are redirected late, by GuardExpand, so that the optimiser can
  still fold `strcmp` of constants; GuardInsert guards only the first byte
  of their string arguments. Such a call that reads exactly a constant of
  at most 113 bytes keeps inline guards instead.
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
source range unsafe (RFC 0035 §2.6). It is the only annotation `weavec.h`
defines; its spelling, `annotate("weavec.unsafe")`, is `UnsafeAnnotation`
in `include/weavec/Frontend/UnsafeRegions.h`. `UnsafeRegions` collects the
ranges by file over the AST before code generation, only when the unit
expanded the macro or spelled the annotation, and also records as unsafe
every function its author keeps from AddressSanitizer:
`no_sanitize("address")`, `no_sanitize_address` and
`disable_sanitizer_instrumentation` (RFC 0035 amendment 8), so a
conservative collector's stack scan needs no source change. GuardInsert
leaves each access whose instruction's location lies in a region unguarded
(it gets a ledger row with reason `unsafe` and no marker), and its
`array-bounds` checks are removed. An access inlined from an unsafe
function keeps the location of its source, and so stays unguarded.

## The enforcement ledger

GuardInsert records each access it collected, before any rule removes its
guard, in an `EnforcementLedger` (`lib/Frontend/EnforcementLedger.cpp`,
RFC 0035 §9 and amendment 10); GuardPrune and GuardExpand record what
became of each copy of its marker. A row has its function, file, line,
column, operation (`load`, `store`, `rmw`, `copy`, `set`, `lane`,
`disjoint`, `call:<name>`), bytes (0 when computed at run time), outcome and
reason:

| Outcome | Reasons |
| --- | --- |
| `guarded` | `access`, `range`, `string`, `checked-call`, `loop-range` |
| `proven` | `in-bounds`, `dominated`, `merged`, `optimized` (the optimiser removed every copy of the access) |
| `unguarded` | `unsafe` |

`-fweavec-ledger=<path>` writes the rows as JSON, `weavec-ledger` version 3
(`EnforcementLedgerVersion`): an object with `schema`, `version`,
`producer` and `units`, each unit with its `source`, `object`, `target`,
`config` (`checks`, `zeroInit`), `summary` (`accesses`, `proven`,
`guarded`, `unguarded`) and `rows`. A path ending in `/`, or an existing
directory, receives `<object>.ledger.json` per unit; a file receives one
ledger, and an invocation that would write more than one is an error. Files
are written through a temporary renamed into place. `-fweavec-summary` (the
default when a ledger is written) prints the summary line on standard
error:

```
weavec: lookup.c: 3 accesses: 1 proven, 2 guarded, 0 unguarded
```

## The runtime

`runtime/` holds the only code WeaveC links into user programs. It is C,
built for the host into two archives installed under `lib/weavec` next to
`weavec.h`. [`weavec_rt.h`](../runtime/weavec_rt.h) is its internal
interface (the shadow encoding, the slot words, the descriptors, the entry
points), shared by its sources and its test and not installed; compiled
code reaches the runtime only through the entry points the guard passes
declare, the shadow descriptor and the standard allocation functions.

| Archive | Source | Holds |
| --- | --- | --- |
| `libweavec_rt.a` | [`weavec_alloc.c`](../runtime/weavec_alloc.c) | the size-class arena allocator, its slot words and shadow writes, the quarantine and huge blocks, when to sweep, the allocator's side of the sweep; the shadow's reservation |
| | [`weavec_sweep.c`](../runtime/weavec_sweep.c) | the sweep: stopping the threads, the roots (the image table on Darwin), the conservative mark, recycling |
| | [`weavec_guard.c`](../runtime/weavec_guard.c) | the guards' slow paths (`__weavec_rt_guard`, `__weavec_rt_range`, `__weavec_rt_range_ok`, `__weavec_rt_strlen`, `__weavec_rt_overlap`, `__weavec_rt_null`, `__weavec_rt_limit_fail`), the frames' (`__weavec_rt_unpoison_stack`, `__weavec_rt_alloca_poison`, `_unpoison`), the globals' registration, and the weak `__ubsan_handle_out_of_bounds[_abort]` |
| | [`weavec_libc.c`](../runtime/weavec_libc.c) | the checked library wrappers `__weavec_rt_<name>`, with their limits for fortified forms, the number parsers, `mmap` and `munmap` |
| | [`weavec_asan.c`](../runtime/weavec_asan.c) | ASan's and LeakSanitizer's interface over WeaveC's shadow |
| | [`weavec_owner.c`](../runtime/weavec_owner.c) | the table of entry points `__weavec_rt_dispatch` (version 4), and on Darwin the choice of the copy that owns the process |
| | [`weavec_report.c`](../runtime/weavec_report.c) | reports, `__weavec_rt_fatal`, `__weavec_rt_trapping`, the `WEAVEC_RT_*` settings and the counters |
| `libweavec_alloc.a` | [`weavec_malloc.c`](../runtime/weavec_malloc.c) | the standard allocation functions over the arena, for the image the archive is linked into |

### The allocator

The allocator reserves one arena inside the shadow's window at first use:
one region of 2 GiB (1 GiB when that cannot be reserved) per size class,
followed by the metadata. The classes are 16 to 128 bytes in steps of 16,
then four per doubling up to 2^30 bytes. A request takes a slot of the
smallest class strictly larger than it, so at least one byte after every
object belongs to no object. Each slot has one 32-bit word of metadata,
`value << 2 | state`: a live slot's value is its object's size, a free or
released one's the next slot on its list. Free lists and
the quarantine queues live in the metadata, never in freed memory. An
allocation writes 0 over the object's whole granules, k over a partial last
granule and `0xFA` over the rest of its slot (up to 64 KiB); a release
writes `0xFD` over the slot. The shadow of a large object is mapped afresh
rather than written, so untouched large blocks cost no memory. Every block
is zero-filled.

A released slot joins its class's *quarantine* and is never reused from
there: only a sweep moves it to the free list (RFC 0036 §3.2), in address
order, so that slots are reused in order. A request no class
can hold is mapped on its own as a *huge block*, kept in a table, with at
least one poisoned granule after it; at release its mapping is replaced by
an inaccessible reservation, unmapped only by a sweep that finds no pointer
to it. The allocator validates every release: a release of an interior
pointer, of a released or never-allocated slot, or of a stack or global
object stops the program through `__weavec_rt_fatal`; a release of a
quarantined block is a double free. A pointer it does not own goes to the
next allocator (the pointer's own malloc zone on Darwin, `__libc_free` and
`__libc_realloc` or the next `free` and `realloc` in lookup order
elsewhere).

`libweavec_alloc.a` defines `malloc`, `calloc`, `realloc` and `free`
strongly; the rest of the family (`reallocarray`, `aligned_alloc`,
`posix_memalign`, `valloc`, `free_sized`, `free_aligned_sized`, and
`malloc_size`, `malloc_good_size`, `reallocf` on Darwin or `memalign`,
`pvalloc`, `malloc_usable_size` elsewhere) is weak, so a program's own shim
over `malloc` replaces it and keeps the arena. On ELF the executable's
definitions serve every shared library.

### When to sweep

Let `gained` be the bytes the quarantine gained since the last sweep (what
the last sweep retained is not counted), `Q` the configured quarantine
(`WEAVEC_RT_QUARANTINE`, 16 MiB by default) and `live` the bytes of live
heap blocks. A release (or an allocation) sweeps before it returns
(RFC 0036 §3.3) when `gained` reaches the *hard* budget `max(2 Q, live /
2)`; when it reaches the *soft* budget `max(Q, live / 4)` and the last sweep
ended at least eight of its durations ago, so that sweeps take at most
about a ninth of the run time and the quarantine grows instead; or when a
class's region is exhausted. The clock is read once per sixteenth of the
soft budget released.

### The sweep

The sweep (RFC 0036 §3.4, `weavec_sweep.c`):

1. **Stop.** Take the image table's lock, every allocator lock and the
   sweep lock, then stop every other thread: on Darwin with `task_threads`
   and `thread_suspend`, reading each thread's registers with
   `thread_get_state`; on Linux by signalling each task of
   `/proc/self/task` (`SIGPWR`, `SIGXCPU` when the program handles
   `SIGPWR`, or `WEAVEC_RT_SWEEP_SIGNAL`), whose handler records its stack
   pointer and waits. A thread that does not stop within a second ends the
   sweep without recycling: safety never depends on a sweep running.
2. **Mark.** Scan conservatively, word by aligned word, for addresses of
   quarantined slots and released huge blocks (interior addresses count):
   - each thread's registers, and its stack from its stack pointer;
   - every other readable, writable mapping of the process but the
     runtime's own. On Darwin a mapping is asked first for its count of
     dirty and swapped-out pages (one call), and skipped when it has none,
     or when it is the kernel's reclaim ring; `mincore` then finds the pages
     to scan. The dyld shared cache is not walked: its writable mappings
     hold the data of every image in the cache. Instead the runtime keeps,
     from dyld's add-image and remove-image callbacks, a table of the
     writable segments of the images the process loaded from the cache,
     and scans those;
   - every live heap block and live huge block.

   Released blocks are not scanned. Marks go to a bitmap per class.
3. **Recycle** each quarantined slot without a mark to its class's free
   list, and unmap each released huge block without one; marked blocks stay.
4. **Resume** the threads and release the locks.

The sweep runs on the thread that asked for it and calls nothing that takes
a lock a stopped thread could hold: no allocation, no stdio, no dyld. With
little live heap a sweep takes about a millisecond on Darwin; it grows with
the live heap and the dirtied writable data.

### ASan's interface

`weavec_asan.c` (RFC 0036 §5) defines, with ASan's signatures and meaning,
`__asan_poison_memory_region` (whole granules inside the region become
`0xF7`, `use-after-poison`), `__asan_unpoison_memory_region`,
`__asan_address_is_poisoned`, `__asan_region_is_poisoned`,
`__asan_describe_address`, `__asan_handle_no_return` (which clears the
stack's shadow, as before a `noreturn` call),
`__asan_get_current_fake_stack`, `__asan_addr_is_in_fake_stack`,
`__sanitizer_print_stack_trace`, `__sanitizer_set_report_path`,
`__sanitizer_annotate_contiguous_container`, the fiber-switch
announcements, and LeakSanitizer's entry points. What ASan does that
WeaveC does not (leak checking, fake stacks) does nothing.

### One runtime per process on Darwin

Every image linked by `weavec-cc` carries the archives. On Darwin the copy
whose `__weavec_rt_dispatch` `dlsym(RTLD_DEFAULT, …)` finds is the *owner*;
every other copy takes the owner's arena and shadow descriptors and
forwards to it the allocation and the global registration entry points, so
one arena, one shadow and one quarantine serve every image (RFC 0033 §6.2,
RFC 0036 §4). A table
of another layout (another WeaveC version's runtime) is not used. The arena
is also registered as a malloc zone and promoted to the process's default
zone, so `malloc` from any image, the C library's own (`strdup`,
`getline`, `asprintf`) included, is served by the arena; blocks the
system's zones allocated before that go back to them.

### Reports

A failed guard prints one line on standard error and traps (`SIGTRAP`, or
`SIGILL` on x86-64):

```
weavec: <kind> at <file>:<line>:<column>: <read|write> of <n> bytes at 0x<addr>
```

with `<unknown>` for a missing location. For the heap a second line places the address:
`… is <k> bytes after the <m>-byte heap object at …`, or `… is inside a
released heap block at …`; for a fortified call,
`… is <k> bytes after the <m>-byte destination at …`. The kinds are
`heap-buffer-overflow`, `heap-use-after-free`, `stack-buffer-overflow`,
`dynamic-stack-buffer-overflow`, `stack-use-after-scope`,
`global-buffer-overflow`, `intra-object-overflow`, `use-after-poison`,
`null-dereference`, `unterminated-string`, `overlapping-copy`, and
`invalid-access` for poison the runtime cannot attribute. An index check prints
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
with no guarantee. The settings:

| Variable | Effect |
| --- | --- |
| `WEAVEC_RT_ABORT=1` | report mode traps |
| `WEAVEC_RT_REPORT_LOG=<path>` | append reports to that file instead of standard error (ignored in set-uid programs) |
| `WEAVEC_RT_BACKTRACE=1` | print the stack of the access that stops the program |
| `WEAVEC_RT_STATS=1` | print the counters at exit as `weavec: runtime: <n> <what>` (allocations, releases, recycled slots, huge blocks, slow guards, range guards, string guards, stack unpoisons, sweeps), then the sweeps' total and longest pause, what the last one retained, and the peaks of the live slots and of the quarantine; on Darwin only the owner prints, counting every image's work |
| `WEAVEC_RT_QUARANTINE=<bytes>` | the quarantine's least growth before a sweep (`Q`) |
| `WEAVEC_RT_SWEEP_SIGNAL=<n>` | the signal number that stops threads for a sweep on Linux (default `SIGPWR`, or `SIGXCPU` when the program handles `SIGPWR`) |

Reports have no ids: the kind and the line format are the interface. The
case runner and the corpus gate attribute traps through this output.

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
untracked (unless it poisons it through ASan's interface), guards pass on
it, and releases are not validated. Stack and global objects are still
guarded.

A Darwin link with `-flto` gets `-lto_library` naming the libLTO of the
LLVM WeaveC was built with, when the driver's default does not exist.

## `weavec::Core` — the library table

`lib/Core` depends only on the C++ standard library. It holds one
declarative table, [`LibrarySpec.txt`](../lib/Core/LibrarySpec.txt), and its
parser (`include/weavec/Core/LibrarySpec.h`, `lib/Core/LibrarySpec.cpp`):
per C library, POSIX and platform function, which pointer arguments the
call reads or writes and how many bytes, whether null is allowed, which
arguments it releases or reallocates, which must not overlap, how a format
reads its arguments, the result a `value(t)` computes, the runtime's
checked version (`wrapper(NAME)`), `wide` rows, and the fortified aliases
with their object-size argument (`chk`). CMake embeds the text;
`LibrarySpec::shipped()` parses it once on first use. Library behaviour
goes in the table, never in a comparison of a callee's name.

## `weavec::Frontend` — Clang and LLVM integration

| Component | Role |
| --- | --- |
| `GuardPass` (`GuardPass.h`; `GuardPass.cpp`, `GuardPassImpl.h`, `GuardInsert.cpp`, `GuardLibrary.cpp`, `GuardLoops.cpp`, `GuardExpand.cpp`, `GuardFrames.cpp`, `GuardGlobals.cpp`) | The four passes of *Compile pipeline* and `registerGuardPasses`, which adds them through `CodeGenOptions::PassBuilderCallbacks` with the mode (trap, report, verify), the unit's unsafe regions and its enforcement ledger. |
| `EnforcementLedger` | The rows the passes record, their rollup, the `weavec-ledger` version 3 JSON writer and the summary line. |
| `UnsafeRegions` | The `WEAVEC_UNSAFE` and `no_sanitize` ranges of a unit, by file, the AST consumer that collects them, and `UnsafeAnnotation`. |
| `Driver` | `weavec-cc`: `DriverOptions` (WeaveC's flags, split from Clang's), the in-process `-cc1` jobs with the enforcement set-up (`configureEnforcement`), ASan's contract and fortification, the wrapper action that adds the unsafe-region collector, the runtime's link line (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`), and `--help-weavec`. |
| `ResourceDir` | Locates `weavec.h`, the runtime archives, Clang's resource directory and `clang`. |

## `tools/weavec-cc`

`weavec-cc` is the drop-in compiler: `CC=weavec-cc make`. Its own flags,
which `weavec-cc --help-weavec` lists, are `-f[no-]weavec`,
`-fweavec-checks=trap|report|verify|none`, `-f[no-]weavec-zero-init`,
`-fweavec-ledger=<path>`, `-f[no-]weavec-summary` and
`-f[no-]weavec-asan-compat`; an unknown `-fweavec-*` flag is an error, and
everything else is Clang's. When WeaveC is on, the driver defines
`__WEAVEC__=1` and adds `weavec.h`'s directory with `-isystem`.
`--version` prints Clang's version block, then `weavec-cc version …`. The
command line is that of RFC 0035 §7 as RFC 0036 §7 amends it.

## Tests and gates

- **Unit tests** (`unittests/`, GoogleTest). `LibrarySpecTest.cpp`
  (`WeaveCCoreTests`) checks every library entry against an independent,
  hand-written expectation table. `GuardPassTest.cpp`
  (`WeaveCFrontendTests`) runs the guard passes on IR text, each removal
  rule alone, and checks the IR and the ledger rows they leave.
- **The runtime test** ([`runtime/test/rt_test.c`](../runtime/test/rt_test.c),
  CTest `runtime`) is one C program linked with `libweavec_rt.a` and
  `libweavec_alloc.a`, so its `malloc` is the arena's. It tests the shadow
  and its encoding, the size classes, alignment, reallocation, the sweep
  (recycling, retention, following the heap, stopping threads, pacing,
  the C library's data through the image table), invalid releases (in
  child processes that must die), huge blocks, foreign pointers, the
  guards, strings, the checked calls, globals, dynamic allocas and the
  stack's unpoisoning, ASan's interface, the number parsers and limits,
  mappings, the array-bounds handlers, threads and `fork`. Test
  names as arguments run only those.
- **Lit tests** (`test/Guards`, `test/Driver`) pin exact output and
  behaviour. `test/Guards` pins the passes' IR (FileCheck on `-emit-llvm`:
  `ir-*.c`, among them `ir-frames.c`, and `shared-frame.c`) and, by
  building and running programs, the run-time
  behaviour, each report line, and the enforcement ledger;
  `test/Driver` pins the runtime's link line and fallbacks
  (`runtime-*.c`), fortification (`fortify.c`), ASan's macros
  (`asan-compat*.c`) and the globals of a dylib and a bundle
  (`image-globals.c`).
- **`test/cases`** is one tree of executable C cases by feature, with
  expectations as line-comment markers (see its
  [README](../test/cases/README.md)).
  [`scripts/run-cases.py`](../scripts/run-cases.py) builds each case with
  `weavec-cc` (`GUARDED`, `PROVEN`, `UNGUARDED` and `EXPECT-LEDGER` on the
  enforcement ledger) and runs it, attributing every stop to a line and a
  kind (`TRAP: <kind>`, `TRAP-AT: <unit>:<line>`, and `STOP` in a `DETECT`
  case). `test/cases/determinism` holds a bug per class RFC 0036 makes
  deterministic, each with its correct twin (gate G1), and, as `XFAIL`, the
  bugs it leaves to a later RFC. `--asan` adds an
  oracle built with `-fsanitize=address,array-bounds`; `--checks verify`
  fails any `weavec.proven` report. CTest registers one `cases-<suite>`
  test per top-level directory, so `ctest -j` runs the suites in parallel.
- **`test/corpus`** pins real projects, built and tested with `weavec-cc`
  by [`scripts/corpus-gate.py`](../scripts/corpus-gate.py) (`--quick` on
  every pull request); see its [README](../test/corpus/README.md).

RFC 0036's acceptance gates (detection G1–G3, drop-in G4–G5, cost G6–G9,
hygiene H1–H2) are measured with these tools. `scripts/check-hygiene.py`
implements gate H1: no libc name comparisons outside the library table, no
corpus project named under `lib/`, Core free of Clang and LLVM, the
runtime's includes, its entry points declared in `weavec_rt.h`, and the
line budgets of the library (9,000) and of the runtime (6,000).
[`scripts/codegen-identity.py`](../scripts/codegen-identity.py) checks that
objects built with `-fweavec-checks=none` are byte-identical to Clang's.

## Build structure

- The top-level `CMakeLists.txt` builds `lib/` (Core, Frontend), `tools/`,
  `runtime/` and, with `WEAVEC_BUILD_TESTS`, `unittests/` and `test/`.
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
- A CMake package config (`find_package(WeaveC)`) exports `weavec::Core`
  and `weavec::Frontend` to external tools.
