# Architecture

WeaveC is a drop-in C compiler that enforces memory safety at run time,
built on Clang and LLVM. [RFC 0037](rfcs/0037-complete-sweep-and-colors.md),
*A complete sweep and object colors*, defines the design over the reuse
after a sweep of [RFC 0036](rfcs/0036-deterministic-heap-reuse.md) and the
guards of [RFC 0035](rfcs/0035-guard-by-default.md), *Guard by default*;
each RFC's *Implementation amendments* override its body.

`weavec-cc` guards every memory access of the C and C++ code it compiles
(`weavec-c++`, a link to it, is its C++ driver: RFC 0039), in LLVM IR, against the runtime's shadow memory. It removes a guard only where a
simple local rule proves it redundant. The locals a guard can reach get
redzones in their frame (locals whose lifetimes never overlap share space),
globals get redzones, locals are zero-initialized, typed array indexes are
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

RFC 0037 is Accepted and being implemented; the [roadmap](roadmap.md) says
which stages have landed. This page describes the tree as it is. Earlier
designs (the advisory ownership analysis and the `weavec` tool, checked
mode, checks planned from the analysis, unit records and the link step) are
described in the RFCs that introduced and retired them.

## Compile pipeline

`weavec-cc` compiles a C translation unit as Clang does, in-process, with
these additions in an enforcing mode (`-fweavec-checks=trap`, the default,
`report` or `verify`; RFC 0035 §1 and amendment 1, RFC 0036 §1):

1. **Zero-initialized locals.** `-ftrivial-auto-var-init=zero`, for locals
   up to 4 KiB unless the command line sets its own limit
   (`-fno-weavec-zero-init` turns it off). The runtime's allocator
   zero-fills every block.
2. **Array bounds.** `-fsanitize=array-bounds`, reported through the
   runtime's weak `__ubsan_handle_out_of_bounds` and
   `__ubsan_handle_out_of_bounds_abort`, unless the command line already
   asks for a sanitizer; report mode adds `-fsanitize-recover` for it. A
   program that also links UBSan's runtime gets UBSan's handlers.
3. **Locations and stacks** (RFC 0040 §4.5, §8). Without `-g`, the unit
   is compiled with line tables (`-gline-tables-only`, with inlined calls
   and columns), so that the site records and every frame of a report's
   stacks carry source locations; a command line with any `-g` keeps its
   level, and one that ends in `-g0` keeps none. The locals keep their
   names (`-fno-discard-value-names`) for the frame descriptors, and on
   x86-64 Linux the frame pointers (`-fno-omit-frame-pointer`, unless the
   command line chooses) for the runtime's unwinder.
4. **Lifetime markers at every `-O` level**
   (`SanitizeAddressUseAfterScope`), so a use after scope traps at `-O0`
   too.
5. **No conditional compares on AArch64** (`-aarch64-enable-ccmp=false`;
   a later `-mllvm` option of the user's wins). The backend folded a
   guard's partial-granule test back into its fast path.
6. **Fortification** (§1, §6). On an optimizing glibc build whose command
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
   `lib/Frontend/GuardPass.cpp`), at every optimization level:

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

Guards are inserted first and expanded last because the optimizer exploits
undefined behavior: an out-of-bounds read in a loop can make the loop
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
optimizer may neither drop nor merge them, nor move an access past one, but
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
as Objective-C, Objective-C++, OpenCL or CUDA, is compiled as Clang compiles
it. A C++ unit is guarded as a C unit is, with the C++ libraries' bounds
checks defined (RFC 0039 §1); every pass treats an `invoke` as the call it
is, and a check after one goes on its normal path (§2).

## Guards and the shadow

The runtime keeps one shadow byte per 16-byte granule (RFC 0035 §2.2 and
amendment 3, RFC 0036 §5, RFC 0037 §7; the values are in
[`weavec_rt.h`](../runtime/weavec_rt.h)):

| Value | Meaning |
| --- | --- |
| `0x00` | all 16 bytes addressable, untracked or uncolored memory |
| `0x01`–`0x7F` | all 16 bytes addressable, in an object of this color |
| `0x80`–`0xEF` | `1ccc kkkk`: the first k bytes (1–15) addressable, in an object whose color's low bits are ccc |
| `0xF1` | a frame's left redzone |
| `0xF2` | a redzone between a frame's objects |
| `0xF3` | a frame's right redzone |
| `0xF4` | a dynamic stack object's redzone |
| `0xF5` | a frame that returned (RFC 0037 §9) |
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

**Colors** (RFC 0037 §7–§8). A color is one of 111 values that are never
0 and whose low three bits are never 7: heap slots take
`color(slot + 41 × region)`, a frame's locals consecutive colors from a
hash of the function's name, and the process's globals consecutive colors
in the order they register. An access whose address is computed from a
pointer (its *root*: the value under its chain of `getelementptr`s) loads
the root's shadow byte once, hoisted out of the loops the root does not
change in, and compares the access's byte with it; a walk from a constant
start near the root by a small constant step has no comparison (its guard
meets the redzone first), and in verify mode has one as a monitor. A
mismatch calls `__weavec_rt_jump`, which decides exactly for the heap (one
slot, or any root inside a block but at its start, passes) and by color
elsewhere. Rule 6.4's
preheader asks `__weavec_rt_color_ok` for each range that starts at a
variable offset from its root.

**Fast paths** (RFC 0035 amendment 4). A guard whose bytes lie in one
granule is three instructions: the shift and mask, the shadow load, and a
branch on the byte's sign bit (four with a root: a compare with the
root's byte), weighted cold. The partial-granule test (whether
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
through its shadow, even when the optimizer moved the access to the other
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
  is, a copy of the loop without those guards runs, and LLVM may vectorize
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
constant stores (redzones poisoned, objects in their colors, partial
granules encoded, objects whose scope has not begun `0xF8`) and stores two
words at the bottom of the left redzone, a tag with the runtime's ABI and
the address of the frame's descriptor (the function's name and each
tracked local's offset, size and name; RFC 0040 §5.3), from which a stack
report names the local, and writes it
`0xF5` (*returned*) before each return. A lifetime start of a tracked
object makes it addressable and a lifetime end poisons it with `0xF8`, so a
use after scope traps. A slow path that meets `0xF5` below its caller's
stack pointer on the thread's own stack reports `stack-use-after-return`;
any other `0xF5` is stale (a frame without tracked locals, or of code
without WeaveC, holds the bytes now) and is cleared (RFC 0037 §9). Runs of
4,096 granules or more go to `__weavec_rt_frame_shadow`, which writes only
what lies on the thread's stack, so the compiler's and the program's cost
grows with a frame's objects, not its size. A function with a `musttail` call or `localescape` keeps its locals
in place, without redzones.

**Shared space** (RFC 0036 §2, amendment 8). In a frame that would be
larger than 512 bytes, two tracked locals may share bytes when their
lifetimes never overlap: LLVM's `StackLifetime` analysis,
in its *may be live* mode, over the lifetime markers that remain after the
optimizer (the analysis SafeStack uses). Each local is placed at the lowest
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

**Non-local exits** (RFC 0035 §3.5, RFC 0039 §3). Before each call of a
function that does not return, other than the runtime's own and those the
library table marks `exits` (they end the process: `abort`, `exit`, the
assertion failures, `err` and the like) or `unwinds` (a throw:
`__cxa_throw`, `_Unwind_Resume` and the rest), the pass calls
`__weavec_rt_unpoison_stack()`, which clears the shadow from the current
stack pointer to the thread's stack base and turns color checks off on
that thread's stack. A `longjmp`, `siglongjmp`, `pthread_exit`, or an
interpreter's error jump written in assembly but declared `noreturn` then
leaves no poison in the frames it abandons (assumption A5). An exception
marks only the frames it unwound: the runtime stands in front of the
unwinder's `_Unwind_RaiseException` and `_Unwind_Resume_or_Rethrow`
(where the throw began) and `__cxa_begin_catch` (where it ended), and a
landing pad of a guarded function calls `__weavec_rt_landed()`; the bytes
between are marked as a frame that returned (`0xF5`), which `settle`
treats as RFC 0037 §9 says.

**C++'s library** (RFC 0039 §6–§7). GuardExpand turns each trap of the C++
library's hardening (`__builtin_verbose_trap`, whose message is the name of
the artificial function its location is inlined at) into a call of
`__weavec_rt_library_assertion`. The library table's mangled rows guard
the buffer arguments of the members of `std::string` and the streams that
the C++ library's shared object defines, as C library calls are guarded;
an `available_externally` callee counts as a declaration.

**Globals** (`GuardGlobals.cpp`, RFC 0035 §4). A global the unit defines exactly is
tracked unless it is thread-local, in a named section, in a comdat, in a
non-default address space, a string literal or other mergeable constant
(constant, `unnamed_addr` and local), `common`, appending, externally
initialized, aligned above 4 KiB, of size 0, or one of LLVM's or WeaveC's
own. A tracked global is replaced by a global of the same name, linkage and
visibility that holds the original and a redzone (sized as a frame
object's), aligned to 32 bytes. A module constructor of priority 1 calls
`__weavec_rt_globals_register(array, n, frames, m)` with
`{address, size, size with redzone, name, location}` per global (the name
and the `file:line` of its definition, self-relative, for a report; RFC
0040 §5.4) and the unit's frame descriptors, which poisons each redzone
`0xF9` and keeps both sorted. A shared library's or bundle's copy of the runtime sends the
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
  no pointer are redirected late, by GuardExpand, so that the optimizer can
  still fold `strcmp` of constants; GuardInsert guards only the first byte
  of their string arguments. Such a call that reads exactly a constant of
  at most 113 bytes keeps inline guards instead.
- A row whose result is a `value(t)` computed only from what it reads
  (`strlen`, `strnlen`) is replaced by the string guard that computes `t`.
- A `wide` row scales its `count` terms by the module's `wchar_size`; its
  strings are not scanned (a known gap: a wide string argument is not
  guarded).
- A null the row accepts (`null-ok`) passes. The arguments a call releases
  or reallocates are not guarded; the allocator validates them. The release
  functions' rows (`free`, `realloc`, `reallocf`, `reallocarray`, the
  `operator delete` forms) redirect the call to the runtime's
  `__weavec_rt_<name>_at`, which releases as the plain function does and
  hands the allocator the call's site for its report (RFC 0040 §5.5).
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
| `proven` | `in-bounds`, `dominated`, `merged`, `optimized` (the optimizer removed every copy of the access) |
| `unguarded` | `unsafe` |

`-fweavec-ledger=<path>` writes the rows as JSON, `weavec-ledger` version 4
(`EnforcementLedgerVersion`; each row's `display` is its function's name
demangled, RFC 0039 §8): an object with `schema`, `version`,
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

The sources build one shared library, `libweavec_rt.so.<abi>` (Linux) or
`libweavec_rt.<abi>.dylib` (Darwin), which exports what
[`weavec_rt.exports`](../runtime/weavec_rt.exports) lists (the entry points
`weavec_rt.h` marks `WEAVEC_RT_API`, ASan's interface and the allocator) and
hides the rest, and, on Linux, `libweavec_rt.a` for `-static` programs.

| Library | Source | Holds |
| --- | --- | --- |
| `libweavec_rt` | [`weavec_alloc.c`](../runtime/weavec_alloc.c) | the size-class arena allocator, its slot words and shadow writes, the quarantine and huge blocks, when to sweep, the allocator's side of the sweep; the shadow's reservation |
| | [`weavec_sweep.c`](../runtime/weavec_sweep.c) | the sweep: stopping the threads, the roots (the image table on Darwin), the conservative mark, recycling |
| | [`weavec_guard.c`](../runtime/weavec_guard.c) | the guards' slow paths (`__weavec_rt_guard`, `__weavec_rt_range`, `__weavec_rt_range_ok`, `__weavec_rt_strlen`, `__weavec_rt_overlap`, `__weavec_rt_null`, `__weavec_rt_limit_fail`), the frames' (`__weavec_rt_unpoison_stack`, `__weavec_rt_alloca_poison`, `_unpoison`), the globals' registration, and the weak `__ubsan_handle_out_of_bounds[_abort]` |
| | [`weavec_libc.c`](../runtime/weavec_libc.c) | the checked library wrappers `__weavec_rt_<name>`, with their limits for fortified forms, the number parsers, `mmap` and `munmap` |
| | [`weavec_asan.c`](../runtime/weavec_asan.c) | ASan's and LeakSanitizer's interface over WeaveC's shadow |
| | [`weavec_report.c`](../runtime/weavec_report.c) | reports (the first line, the stacks, the object paragraphs of heap blocks, frames and globals, the summary line, their JSON form, the bad releases' kinds), `__weavec_rt_trapping`, the fault handler, the `WEAVEC_RT_*` settings and the counters; the check for a second runtime and a foreign allocator |
| | [`weavec_stack.c`](../runtime/weavec_stack.c) | RFC 0040: the frame-pointer unwinder (`weavecRtUnwind`), the store of unique stacks that heap blocks name by identifier, and the threads' numbers |
| | [`weavec_symbolize.c`](../runtime/weavec_symbolize.c) | RFC 0040: naming a report's frames through a symbolizer (`weavec-cc --weavec-symbolize` on Linux, `atos` on Darwin), with the two-second limit and the fallback to module and offset |
| | [`weavec_malloc.c`](../runtime/weavec_malloc.c) | the standard allocation functions over the arena |
| | [`weavec_cxx.c`](../runtime/weavec_cxx.c) | RFC 0039: C++'s `operator new` and `operator delete` (weak, by their Itanium names) with the form of each block, the marking of frames an exception unwound (`__weavec_rt_landed`), and the C++ libraries' failed checks (`library-assertion`) |
| | [`weavec_unwind.c`](../runtime/weavec_unwind.c) | RFC 0039: the interposers of `_Unwind_RaiseException`, `_Unwind_Resume_or_Rethrow` and `__cxa_begin_catch` (Darwin: `__DATA,__interpose`; Linux: the library's own definitions; `-static`: `--wrap`). It and `weavec_cxx.c` are compiled once for the shared library and once for the archive, which on Darwin has no allocation functions |

### The allocator

The allocator reserves one arena inside the shadow's window at first use:
one region of 2 GiB (1 GiB when that cannot be reserved) per size class,
followed by the metadata. The classes are 16 to 128 bytes in steps of 16,
then four per doubling up to 2^28 bytes (a larger request is a huge block).
A request takes a slot of the smallest class strictly larger than it, so at
least one byte after every object belongs to no object. Each slot has one
32-bit word of metadata: a live slot's is `size << 4 | form << 2 | state`,
its object's size and the form that allocated it (`malloc`, `new`, `new[]`,
RFC 0039 §4), a free or released one's `next << 2 | state`, the next slot
on its list. Free lists and
the quarantine queues live in the metadata, never in freed memory. An
allocation writes the slot's color over the object's whole granules, the
partial byte over a partial last granule and `0xFA` over the rest of its
slot (up to 64 KiB); a release
writes `0xFD` over the slot. The shadow of a large object is mapped afresh
rather than written, so untouched large blocks cost no memory. Every block
is zero-filled.

A released slot joins its class's *quarantine* and is never reused from
there: only a sweep moves it to the free list (RFC 0036 §3.2), in address
order, so that slots are reused in order. Each thread caches, per small
class (up to 32 KiB), up to 32 free slots it takes from the free lists at
once and up to 32 released ones it gives the quarantine at once (RFC 0037
§2.1): a release claims the slot's word by compare-and-swap, so a racing
second release is a double free; a cached released slot is quarantined, and
a sweep leaves it in its cache. A class whose region is full takes one of 64
spare regions, unless a quarter of a region was released into it since the
last sweep, when it sweeps first (§2.3). A request no class
can hold is mapped on its own as a *huge block*, kept in a table, with at
least one poisoned granule after it; at release its mapping is replaced by
an inaccessible reservation, unmapped only by a sweep that finds no pointer
to it. The allocator validates every release: a release of an interior
pointer, of a released or never-allocated slot, or of a stack or global
object stops the program with an `invalid-free` report, and a release
by another form than the allocation's with an `alloc-release-mismatch`; a
release of a quarantined block is a `double-free`. Each slot also keeps
the identifiers of its allocation's and its release's stacks, in a side
table beside the slot words, for the reports (RFC 0040 §3). A pointer it does not own goes to the
next allocator (the pointer's own malloc zone on Darwin, `__libc_free` and
`__libc_realloc` or the next `free` and `realloc` in lookup order
elsewhere).

The library exports `malloc`, `calloc`, `realloc` and `free` and the rest
of the family (`reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`,
`free_sized`, `free_aligned_sized`, and `malloc_size`, `malloc_good_size`,
`reallocf` on Darwin or `memalign`, `pvalloc`, `malloc_usable_size`
elsewhere), unversioned, so that the C library's own versioned references
(`malloc@GLIBC_2.17`) bind to them too; a program's own definition comes
first in the loader's search order and replaces one. The family beyond the
four is weak in the static archive, so a program's own shim over `malloc`
links beside it.

### When to sweep

Let `gained` be the bytes the quarantine gained since the last sweep (what
the last sweep retained is not counted; released huge blocks count at
their size, so a program that frees only huge blocks still sweeps and
gives their address space back, RFC 0036 amendment 10), `Q` the configured quarantine
(`WEAVEC_RT_QUARANTINE`, 64 MiB by default) and `live` the bytes of live
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
   and `thread_suspend`, reading each thread's general and vector registers with
   `thread_get_state`; on Linux by signaling each task of
   `/proc/self/task` (`SIGPWR`, `SIGXCPU` when the program handles
   `SIGPWR`, or `WEAVEC_RT_SWEEP_SIGNAL`), whose handler records its stack
   pointer and waits. A thread that does not stop within a second ends the
   sweep without recycling: safety never depends on a sweep running.
2. **Mark.** Scan conservatively, word by aligned word, for addresses of
   quarantined slots and released huge blocks (interior addresses count):
   - each thread's registers, and its stack from its stack pointer (a
     mapping that holds a stack pointer but is not a thread's own stack,
     such as a coroutine pool, whole); on Darwin the main thread's TSD
     array, which lies outside every stack and image segment;
   - every other readable mapping of the process but the runtime's own,
     code it cannot write and shared files it cannot write through (memory
     the program made read-only is read; RFC 0037 §1). On Linux, of the files under `/dev` only shared memory
     is scanned (`/dev/shm`, shared anonymous mappings, which show as
     `/dev/zero`, and hugetlbfs), not device memory; a sweep that cannot
     read `/proc/self/maps` recycles nothing. On Darwin a mapping is asked first for its count of
     dirty and swapped-out pages (one call), and skipped when it has none,
     or when it is the kernel's reclaim ring; `mincore` then finds the pages
     to scan. The dyld shared cache is not walked: its writable mappings
     hold the data of every image in the cache. Instead the runtime keeps,
     from dyld's add-image and remove-image callbacks, a table of the
     writable segments of the images the process loaded from the cache,
     and scans those;
   - every live heap block and live huge block.

   Released blocks are not scanned. Marks go to a bitmap per region, set
   atomically: with 64 MiB of live heap or more the ranges are listed first
   and marked by the sweeping thread and up to seven worker threads of the
   runtime's, which are never stopped (RFC 0037 §3).
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
`__sanitizer_print_stack_trace`, `__sanitizer_set_report_path`, the
fiber-switch announcements, and LeakSanitizer's entry points. What ASan
does that WeaveC does not (leak checking, fake stacks) does nothing. RFC
0039 §5: the container annotations
(`__sanitizer_annotate_contiguous_container`, the double-ended form, and
the `verify` and `find_bad_address` queries) write `0xFC`
(`container-overflow`) over a heap block's container storage past its
elements; `WEAVEC_RT_CONTAINERS=0` makes them no-ops.

### One runtime per process

Every image linked by `weavec-cc` depends on the library (RFC 0038 §1), so
the dynamic loader loads one copy, initializes it before them (its
constructor reserves the shadow and the arena before any image's
constructor registers its globals) and binds every image's references to
it, whatever their version scripts or `-Bsymbolic`. On Linux the library
precedes the C library in the executable's `DT_NEEDED` list, so its
`malloc` serves every image; its own calls bind inside it
(`-Bsymbolic-functions`) and its thread-local storage is `initial-exec`,
so that a library loaded by `dlopen` fits the C library's static TLS. On
Darwin the arena is also registered as a malloc zone and promoted to the
process's default zone, so `malloc` from any image, the C library's own
(`strdup`, `getline`, `asprintf`) included, is served by the arena; blocks
the system's zones allocated before that go back to them. The library's
constructor stops the program when another runtime's image is loaded (images
of two WeaveC versions; it looks through `dl_iterate_phdr` or the dyld image
list), and on Linux notes once when `dlsym(RTLD_DEFAULT, "malloc")` is
neither its own nor the executable's (a guarded library in a program
`weavec-cc` did not link).

### Reports

A failed guard prints a report on standard error and traps (`SIGTRAP`, or
`SIGILL` on x86-64). Every report has one shape (RFC 0040 §5):

```
weavec: <kind> at <file>:<line>:<column>: <read|write> of <n> bytes at 0x<addr>
    #0 0x<pc> in <function> <file>:<line>:<column>
    #1 ...
weavec: 0x<addr> is <where> <object>[, <history>:]
    #0 ...
weavec: summary: <kind> at <file>:<line>:<column> in <function> (thread T<n>)
```

The first line has `<unknown>` for a missing location. An index check
prints `index-out-of-bounds at …: index <i>`, a monitor that fails in verify
mode `weavec: weavec.proven: <kind> at …`, and a fault the handler catches
`memory-fault at pc 0x<pc>: read at 0x<addr>`. The stack is the access's,
at most 32 frames from the function that made it (`weavecRtUnwind` follows
the frame records, dropping the runtime's own frames, which sit in a
section of their own); an inlined call repeats its frame's number. The
object paragraphs place the address:

- in a heap block: `is <k> bytes after the <m>-byte heap object at …,
  allocated by thread T<n>:`, or `is <k> bytes inside the <m>-byte heap
  block at …, released by thread T<n>:` followed by `the block was
  allocated by thread T<n>:`, each with the stack the allocator recorded
  (two 32-bit stack identifiers per slot and per huge block, in a side
  table beside the slot words; the stacks themselves, 16 frames at most,
  are kept once each in a store the runtime maps and never frees,
  `weavec_stack.c`);
- in a frame: `is <k> bytes after 'buf' (8 bytes at offset 64) in the frame
  of <function> (thread T<n>), whose locals are:` and a `[lo, hi) 'name'`
  line per tracked local. A tracked frame stores a tag and the address of
  its descriptor (`struct __weavec_rt_frame`: the function's name and each
  local's offset, size and name, which the compile keeps with
  `-fno-discard-value-names`) at the bottom of its left redzone, and the
  runtime accepts a descriptor only if a unit registered it. A use after
  return usually gets only `is in a frame that returned`, since the
  report's own frames overwrite the returned one's;
- in a global: `is <k> bytes after the <m>-byte global '<name>' defined at
  <file>[:<line>]`, from the global's record, which carries its name and
  the location of its definition;
- for a color mismatch, first `is <k> bytes after its base …, in another
  object`, then the paragraph of the object it landed in; for a fortified
  call, `is <k> bytes inside the <m>-byte destination at …`.

The allocator stops a bad release in every mode with a report of its own
kind: `double-free`, `invalid-free` (`inside the <m>-byte heap block at …`,
`no block was allocated there`, `not heap memory`) or
`alloc-release-mismatch` (`allocated by <form>`, `size <n> for a <m>-byte
block`), as `weavec: double-free at <file>:<line>:<col>: free of 0x<p>`. A
release call in a guarded unit goes to the runtime's `_at` wrapper
(`__weavec_rt_free_at`, `__weavec_rt_realloc_at`, the `operator delete`
forms'; `wrapper(…_at)` rows of the library table), which passes its site;
any other release is located at its stack's first frame with a line.

Frames are named by a symbolizer the runtime runs (`weavec_symbolize.c`):
on Linux `weavec-cc --weavec-symbolize` (`lib/Frontend/Symbolizer.cpp`,
LLVM's `Symbolize` library), found at `<prefix>/bin/weavec-cc` beside the
runtime's `<prefix>/lib/weavec`, started once with `posix_spawn` and kept;
on Darwin `/usr/bin/atos` per module, which reads the debug map to the
objects' line tables, falling back to `weavec-cc`. It answers in
`llvm-symbolizer`'s format and gets five seconds per report. A set-user-ID
program, `WEAVEC_RT_SYMBOLIZE=0` or a failed spawn leaves a frame as
`in <function> (<module>+0x<offset>)`, which `weavec-cc --weavec-symbolize
<log>` rewrites later. So that frames carry lines, an enforcing compile
emits line tables (`-gline-tables-only`) unless the command line asks for
debug information or ends in `-g0`, and keeps frame pointers on x86-64
Linux.

Reports are formatted by the runtime itself, without `snprintf` (which a
program may define) and without allocating, into a buffer per thread, and
written a line at a time. Before every trap,
`__weavec_rt_trapping` unblocks `SIGTRAP` and `SIGILL` in the thread and
resets an ignored disposition, so the trap ends the program even when it
blocked them (a handler the program installed still runs). In report mode
(`-fweavec-checks=report`) each site reports once and the program goes on,
with no guarantee. The settings:

| Variable | Effect |
| --- | --- |
| `WEAVEC_RT_ABORT=1` | report mode traps |
| `WEAVEC_RT_REPORT_LOG=<path>` | append reports to that file instead of standard error (ignored in set-uid programs) |
| `WEAVEC_RT_REPORT_FORMAT=json` | write each report as one line of JSON (`weavec`: the ABI, `kind`, `proven`, `site`, `what`, `access`, `message`, `thread`, `stack`, `object`) |
| `WEAVEC_RT_HEAP_STACKS=<n>` | frames per allocation and release stack (8; 0 records none) |
| `WEAVEC_RT_SYMBOLIZE=0` | run no symbolizer: frames by module and offset |
| `WEAVEC_RT_SYMBOLIZER=<path>` | the symbolizer to run instead of `weavec-cc` or `atos` |
| `WEAVEC_RT_STATS=1` | print the counters at exit as `weavec: runtime: <n> <what>` (allocations, releases, recycled slots, huge blocks, slow guards, range guards, string guards, stack unpoisons, sweeps), then the sweeps' total and longest pause, what the last one retained, the peaks of the live slots and of the quarantine, the stacks recorded and their memory, and the writable memory the runtime does not track; the process's one runtime counts every image's work |
| `WEAVEC_RT_NOTES=0` | no note that the process's allocator is not the runtime's |
| `WEAVEC_RT_QUARANTINE=<bytes>` | the quarantine's least growth before a sweep (`Q`) |
| `WEAVEC_RT_SWEEP_SIGNAL=<n>` | the signal number that stops threads for a sweep on Linux (default `SIGPWR`, or `SIGXCPU` when the program handles `SIGPWR`) |

Reports have no ids: the kind, the first line and the JSON schema are the
interface. The case runner and the corpus gate read the JSON form from a
report log of their own.

## Linking

`addRuntimeLibraries` (`lib/Frontend/Driver.cpp`) adds the runtime's library
(`<rt>/libweavec_rt.dylib` or `<rt>/libweavec_rt.so`, or
`<rt>/libweavec_rt.a` for `-static` and `-static-pie`), `-rpath <rt>` (or
`-fweavec-rpath`'s directory; none with `-fno-weavec-rpath` or a static
link) and `-lpthread` on Linux to every link job of an enforcing build that
targets the host, executables, shared objects and bundles alike, before the
first library of the line; a relocatable link (`-r`) gets nothing. `<rt>` is
the toolchain's `lib/weavec`. A missing library is an error: `cannot find
the WeaveC runtime (libweavec_rt.dylib), which an enforcing link carries
(-fweavec-checks=none builds without it)`.

`runtimeObstacle` reads the command line for a reason the build cannot use
the runtime: `-ffreestanding`; `-nostdlib`, `-nodefaultlibs` or `-nolibc`;
a sanitizer that replaces the allocator (`address`, `hwaddress`, `memory`,
`thread`, `leak`, `kernel-address`); or a target other than 64-bit Darwin
or Linux. The units are then compiled without guards, as under
`-fweavec-checks=none`, and the link prints
`weavec-cc: note: building without the WeaveC runtime (<reason>): memory
accesses are not guarded`.

When an input of the link, an object or a member of an archive, defines
`malloc` strongly, `noteProgramAllocator` prints a note: the loader binds
`malloc` to the program's definition first, so the program keeps its
allocator, its heap is untracked (unless it poisons it through ASan's
interface), guards pass on it, and releases are not validated. Stack and
global objects are still guarded.

A Darwin link's `-lto_library` that names a file the toolchain does not
have is dropped, so the system linker uses its own.

### The toolchain

`weavec-cc` records no path of the LLVM it was built with (RFC 0038 §6).
Clang's driver finds the resource directory, `lib/clang/<major>`, from the
executable, and `createAction` builds Clang's actions for a `-cc1` job
itself (code generation, preprocessing, syntax checks, precompiled headers),
so that `clangFrontendTool` and the static analyzer it brings are not in
the link. Assembly sources and `-save-temps` compiles get
`-fno-integrated-as` and use the system assembler, since Clang's `-cc1as`
is not a library. With `WEAVEC_SELF_CONTAINED` (the `release` preset)
`weavec-cc` links LLVM's and Clang's static archives on Linux
(`cmake/WeaveCLLVM.cmake`), and on Darwin, whose Homebrew static Clang lacks
the code generator's target builtins, the install copies the LLVM shared
libraries it loads, and theirs, into `lib/weavec/llvm` with an rpath to them
(`cmake/WeaveCBundleLLVM.cmake`). The installed tree (`bin/weavec-cc`,
`lib/weavec`, `lib/clang/<major>`, `share/doc/weavec`) may be moved anywhere;
`cpack` packs it as `weavec-<version>-<os>-<arch>.tar.xz`, and
`scripts/check-toolchain.py` checks an archive moved after unpacking.

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
`LibrarySpec::shipped()` parses it once on first use. Library behavior
goes in the table, never in a comparison of a callee's name.

## `weavec::Frontend` — Clang and LLVM integration

| Component | Role |
| --- | --- |
| `GuardPass` (`GuardPass.h`; `GuardPass.cpp`, `GuardPassImpl.h`, `GuardInsert.cpp`, `GuardLibrary.cpp`, `GuardLoops.cpp`, `GuardExpand.cpp`, `GuardFrames.cpp`, `GuardGlobals.cpp`) | The four passes of *Compile pipeline* and `registerGuardPasses`, which adds them through `CodeGenOptions::PassBuilderCallbacks` with the mode (trap, report, verify), the unit's unsafe regions and its enforcement ledger. |
| `EnforcementLedger` | The rows the passes record, their rollup, the `weavec-ledger` version 4 JSON writer and the summary line. |
| `UnsafeRegions` | The `WEAVEC_UNSAFE` and `no_sanitize` ranges of a unit, by file, the AST consumer that collects them, and `UnsafeAnnotation`. |
| `Driver` | `weavec-cc`: `DriverOptions` (WeaveC's flags, split from Clang's), the in-process `-cc1` jobs with the enforcement set-up (`configureEnforcement`), ASan's contract and fortification, the wrapper action that adds the unsafe-region collector, the runtime's link line (`addRuntimeLibraries`, `runtimeObstacle`, `allocatorDefinedBy`, `dropAllocatorIfDefined`), `--help-weavec`, and `--weavec-symbolize` (`Symbolizer.cpp`: the runtime's symbolizer over LLVM's `Symbolize` library, and the rewriter of a saved report's frames). |
| `ResourceDir` | Locates `weavec.h`, the runtime's library and the Apple SDK. |

## `tools/weavec-cc`

`weavec-cc` is the drop-in compiler: `CC=weavec-cc CXX=weavec-c++ make`.
Invoked as `weavec-c++` (a link the build and the installation make) it is
in Clang's `g++` mode. Its own flags,
which `weavec-cc --help-weavec` lists, are `-f[no-]weavec`,
`-fweavec-checks=trap|report|verify|none`, `-f[no-]weavec-zero-init`,
`-fweavec-ledger=<path>`, `-f[no-]weavec-summary`,
`-f[no-]weavec-asan-compat`, `-f[no-]weavec-colors` and
`-fweavec-rpath=<dir>`/`-fno-weavec-rpath`; an unknown `-fweavec-*` flag is an error, and
everything else is Clang's. `weavec-cc --weavec-symbolize [<log>...]`
compiles nothing: without an operand it answers `CODE <module> 0x<offset>`
lines on standard input as `llvm-symbolizer` does (the runtime's
symbolizer on Linux), and with a saved report's path it prints the report
with the frames the runtime named only by module and offset symbolized. When WeaveC is on, the driver defines
`__WEAVEC__=1` and adds `weavec.h`'s directory with `-isystem`.
`--version` prints Clang's version block, then `weavec-cc version …`, the
LLVM version, and the runtime's interface version and platform. The
command line is that of RFC 0035 §7 as RFC 0036 §7, RFC 0038 §7 and RFC
0040 §8 amend it.

## Tests and gates

- **Unit tests** (`unittests/`, GoogleTest). `LibrarySpecTest.cpp`
  (`WeaveCCoreTests`) checks every library entry against an independent,
  hand-written expectation table. `GuardPassTest.cpp`
  (`WeaveCFrontendTests`) runs the guard passes on IR text, each removal
  rule alone, and checks the IR and the ledger rows they leave.
- **The runtime test** ([`runtime/test/rt_test.c`](../runtime/test/rt_test.c),
  CTest `runtime`) is one C program linked with the runtime's objects (its
  internal functions included), so its `malloc` is the arena's. It tests the shadow
  and its encoding, the size classes, alignment, reallocation, the sweep
  (recycling, retention, following the heap, stopping threads, pacing,
  the C library's data through the image table), invalid releases (in
  child processes that must die), the unwinder, the stack store and the
  heap blocks' stacks, huge blocks, foreign pointers, the
  guards, strings, the checked calls, globals, dynamic allocas and the
  stack's unpoisoning, ASan's interface, the number parsers and limits,
  mappings, the array-bounds handlers, threads, `fork` and a restored
  fault handler. Test names as arguments run only those.
- **Lit tests** (`test/Guards`, `test/Driver`) pin exact output and
  behavior. `test/Guards` pins the passes' IR (FileCheck on `-emit-llvm`:
  `ir-*.c`, among them `ir-frames.c`, and `shared-frame.c`) and, by
  building and running programs, the run-time
  behavior, each report line, and the enforcement ledger;
  `test/Driver` pins the runtime's link line and fallbacks
  (`runtime-link*.c`), one runtime across shared libraries with version
  scripts, `-Bsymbolic` and any link order, plug-ins in guarded and
  unguarded programs and two runtimes in one process (`shared-images.c`),
  fortification (`fortify.c`), ASan's macros (`asan-compat*.c`) and the
  globals of a shared library and a plug-in (`image-globals.c`).
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
  every pull request, `--full` and the programs tier `--programs` weekly on
  linux-x86_64, linux-aarch64 and darwin-arm64); see its
  [README](../test/corpus/README.md).

RFC 0036's acceptance gates (detection G1–G3, drop-in G4–G5, cost G6–G9,
hygiene H1–H2) are measured with these tools. `scripts/check-hygiene.py`
implements gate H1: no libc name comparisons outside the library table, no
corpus project named under `lib/`, Core free of Clang and LLVM, the
runtime's includes, its entry points declared in `weavec_rt.h`, its export
list against its `WEAVEC_RT_API` declarations, and the line budgets of the
library (11,500) and of the runtime (10,000).
[`scripts/codegen-identity.py`](../scripts/codegen-identity.py) checks that
objects built with `-fweavec-checks=none` are byte-identical to Clang's.

## Build structure

- The top-level `CMakeLists.txt` builds `lib/` (Core, Frontend), `tools/`,
  `runtime/` and, with `WEAVEC_BUILD_TESTS`, `unittests/` and `test/`.
- `cmake/WeaveCLLVM.cmake` finds LLVM and Clang and provides the
  `weavec::llvm` interface target and `weavec_link_llvm`/`weavec_link_clang`,
  which respect `LLVM_LINK_LLVM_DYLIB` and `CLANG_LINK_CLANG_DYLIB`, or, with
  `WEAVEC_SELF_CONTAINED` on Linux, link the static archives. It links
  Clang's resource directory into the build tree's `lib/clang/<major>`.
- `cmake/WeaveCHelpers.cmake` provides `weavec_add_library` and
  `weavec_add_executable`. Only `USES_LLVM` targets get LLVM's include
  paths, and Core is not one of them.
- `lib/Core/CMakeLists.txt` embeds `LibrarySpec.txt` as a byte array at
  configure time; editing the table re-runs the configure step.
- `runtime/CMakeLists.txt` builds the runtime's objects once,
  position-independent, with hidden visibility, never as LTO bitcode, and
  always with `-O2` and without the allocation builtins, whatever the build
  type, and from them the shared library (with the export list) into the
  build tree's `lib/weavec` and the static archive.
  With `WEAVEC_BUILD_TESTS` it also builds `weavec_rt_test`, which
  `test/CMakeLists.txt` registers as the CTest `runtime`.
- `cmake/WeaveCInstall.cmake` installs the toolchain's tree (only
  `weavec-cc` of the C++ targets) and configures `cpack`.
