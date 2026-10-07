# RFC 0035: Guard by default

- **Status**: Implemented (gates G7 and G9 carried forward as measured;
  see *Measured gates*)
- **Authors**: WeaveC authors
- **Created**: 2026-10-06
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes RFC 0030's enforcement:
  §1 (the compile pipeline), §2.5–§2.6 and §6 (the require levels), §10
  (checks inserted through Sema, the prelude, deferred CodeGen), §11
  (zero-initialisation lowering), §12 (the ledger, as far as it describes
  the enforcing build) and §13 (unit records and the link step); RFC 0032
  §3–§7 (guards, stack and global registration, the object table as a
  lookup structure); RFC 0033 §4 (the guard question), §7 (link modes and
  records) and its (V); RFC 0034 §1–§5 (guards lowered from declared calls,
  the shadow encoding, the slow path, stack objects, library-call guards)
  and §6 (confirmed errors as build failures). The analysis those RFCs
  define (RFCs 0030 §2–§9, 0031) stays, as an advisory analysis (§8).

This RFC was drafted before implementation. On 2026-10-06 the project
owner accepted the recommendation of the milestone analysis (invert the
pipeline: guard every memory access by default in the backend, remove a
guard only where a simple local rule proves it redundant, and take the
analysis out of the build so that it can neither fail a correct build nor
remove a needed guard; bring ownership back as checked contracts in RFC
0036) and authorized drafting this RFC and implementing it end to end in
one large change, with breaking changes and no compatibility layers.
Accepted status records that authorization. The status becomes
Implemented only when every gate in *Acceptance gates* passes on the final
tree, or an *Implementation amendment* records what was measured instead
and carries the gate forward.

## Summary

Five RFCs made WeaveC's static analysis decide what the build enforces:
a facet the engine proves gets no check, one it cannot prove gets a check
or a guard, and one it finds violated fails the build. Measured on main
after RFC 0034 (v0.15.0), on ten C projects it had never seen and on real
bugs:

- **The runtime does the catching.** Of 15 historic memory-safety bugs
  with public reproducers (Lua, jq, mujs), the default build stops 13 at
  the faulty access, ASan 15. None is a compile-time error. Without the
  runtime WeaveC stops 4.
- **The analysis is what goes wrong.** Eight of the ten new projects do
  not build and pass their tests with default flags: nine new classes of
  false error and false trap, and real but unreachable bugs failing
  builds; a definite error inside an autoconf probe silently disabled two
  features of Tcl. Four classes of wrong proof make the default build
  read freed memory without a trap, one of them in a real CVE.
- **The analysis is what costs.** Heavy files compile at 11 to 98 times
  Clang's instructions, 70–80% of it in the engine. The guards' inline
  checks make object code 3 to 24 times larger, and a stack or global
  guard runs a path of about 60 instructions.

This RFC inverts the pipeline:

1. **Every memory access is guarded unless a local rule proves the guard
   redundant.** An LLVM pass at the end of the optimisation pipeline
   guards each load, store, atomic operation and memory intrinsic, and the
   memory arguments of C library calls, against the runtime's shadow
   memory. The rules that remove a guard are local, simple and checked by
   `verify` mode: an access inside a stack or global object at a known
   offset, an access a dominating guard already covered with no release in
   between, an access a loop's range check covered.
2. **The pass lays out frames and globals.** Stack objects a guard can
   reach and the unit's globals get redzones, as in AddressSanitizer, so
   every tracked object is followed by bytes no object owns, and a guard on
   any of them is the same few instructions. A use of a local after its
   scope ends traps.
3. **The shadow says what is addressable.** One byte per 16 bytes, 0 when
   all 16 are addressable: untracked memory, the bulk of every program's
   accesses to memory WeaveC does not allocate, passes on the fast path.
   The shadow covers only the part of the address space programs use, so
   process start-up costs under a millisecond, not 45.
4. **Typed array indexes keep their bounds.** Clang's `array-bounds`
   checks, reported through WeaveC's runtime, check `a[i]` against the
   declared bound of `a`, which catches overflows inside a struct.
5. **The analysis is advisory.** `weavec-cc` no longer runs it by default;
   `-fweavec-diagnose` runs it and prints its findings as warnings, and the
   `weavec` tool keeps its errors. Nothing the analysis concludes changes
   the generated code.
6. **What existed only to put the analysis into the code is deleted**:
   checks inserted through Sema, deferred CodeGen, the check planner and
   emitter, the prelude, the zero-initialisation lowering, unit records,
   the link step, the require levels and the analysis ledger.

Ownership inference keeps its place as the analysis that will remove the
guards the local rules cannot: RFC 0036 makes inferred contracts checked
at function entry, where they are sound by construction.

## Motivation

Measured on main at `395441c` (v0.15.0), release build, darwin-arm64
(Apple M3), each project's own build and test commands with
`CC=weavec-cc`. The machine was loaded; ratios are of instructions
retired unless stated.

### Drop-in on ten new projects

| Project | Default build and tests | Cause | Run time (default / ASan) | Build |
| --- | --- | --- | --- | --- |
| json-c | builds; 10 of 29 tests trap | a deliberate word-at-a-time hash over-read in the project | 2.42 / 3.39 | 1.8× |
| gawk | stops: 2 false errors | an open function-pointer slot taken as closed | 2.58 / 3.49 | 7.6× |
| MicroPython | stops (a true but unreachable error); every run traps | a statically initialised global with a flexible array; a non-local jump in assembly leaves stack shadow behind | 5.06 / 1.81 | 2.3× |
| Tcl | tests: 13 files trap, one hangs; configure dropped two features | `getaddrinfo` with a null service; a definite error inside a configure probe | 5.08 / 2.57 | 5.0× |
| libarchive | test program stops: 2 false errors | `getcwd(NULL, n)` and `realpath(p, NULL)` taken as always null | 3.80 / 3.53 | 2.2× |
| git | stops: 2 false errors | a callback's effects merged over its targets | 3.11 / 2.59 | 4.0× |
| tcpdump + libpcap | 1 false trap | an array-typed element's subscript guarded as an access | 1.12 / 2.16 | 2.2× / 13.4× |
| libtiff | builds, passes | | 1.89 / 1.49 | 4.6× |
| BearSSL | builds, passes | | **17.5** / 2.28 | 2.0× |
| YARA | stops (a true error in a test); every rule compile traps | a trailing `char c_string[1]` taken as one byte | 7.41 / 3.34 | 5.4× |

Every program built with the runtime also spends about 45 ms of system
time starting and exiting (the 16 TiB shadow reservation), which makes
git's test suite take 4.5 times as long.

### Silent misses

Four classes of wrong proof, each a facet `proven` that `verify` mode
traps on:

- **Removing a node through a pointer-to-pointer loop**
  (`for (pp = &l->head; *pp; pp = &(*pp)->next) if (…) { d = *pp; *pp =
  d->next; free(d); return; }`): the summary has no release, so a caller's
  read of the freed node is proven live.
- **A struct passed by value to an unknown callee** (`fmt =
  b.p->data; jv_free(b); return *fmt;`): the pointer inside the struct is
  not treated as possibly released. This is jq's CVE-2025-49014, which the
  default build misses.
- **Replacing a hash table slot's value** frees the old value in a callee
  whose summary the caller does not connect to its own copy.
- **A callee's parameter dereference is proven under assumption A1**, so a
  use after free inside the callee, whose free is in another file, has no
  guard.

Each is a modelling gap in a whole-function abstract interpretation of the
C heap, and the analysis is trusted: a gap becomes a missed bug. The three
milestone analyses before this one each found new classes after the
previous RFC fixed the old ones.

### Where the build time goes

| Unit | Clang (G insns) | weavec-cc | Engine joins | Machine codegen |
| --- | --- | --- | --- | --- |
| sqlite3.c | 48.3 | 16.1× | 34% | 21% |
| expat xmlparse.c | 3.4 | 29.7× | 58% | 14% |
| Lua lvm.c | 2.2 | 28.0× | 51% | 22% |
| pcre2_match.c | 8.6 | 18.0× | 30% | 53% |
| libpcap grammar.c | | 98× | | |

The engine's joins rebuild the whole state at every merge. The work budget
gives up proofs exactly in the giant functions where accesses
concentrate (pcre2's `match`: 1.9% of its sites proven after 80 billion
instructions of analysis). Machine code generation pays for guards whose
inline form decodes RFC 0034's whole shadow encoding.

### What the analysis contributes

On the blind detection set (64 programs) the default build stops 60 and
ASan 58; of those 60, 7 are compile-time errors. On the 15 real bugs, the
compile-time contribution is none. What the analysis buys in the default
build is the guards it removes (55% of facets proven on the corpus) and
the bugs it reports early, at the price of every false stop and every
silent miss above.

## Soundness

### The guarantee

For a unit compiled by `weavec-cc` in an enforcing mode (`trap`, the
default, or `verify`; `report` with `WEAVEC_RT_ABORT=1`), with the runtime
linked, every memory access the unit's code makes outside a
`WEAVEC_UNSAFE` region satisfies:

- **(A) Accesses.** A load, store, atomic operation or memory intrinsic of
  `w` bytes at address `a` executes only if every byte of `[a, a + w)` is
  *addressable* when it runs; otherwise the program traps before the
  access. The same holds for each memory argument of a call to a function
  in the library table, over the bytes the table says the call accesses,
  computed from the arguments (or, for a string, up to its terminator).
- **(B) Bounds.** An index `a[i]` into an array whose bound the type
  declares (other than a trailing array of a struct, which may be
  flexible) traps unless `0 ≤ i < n`, or `i ≤ n` where only the address is
  taken.
- **(R) Releases.** A release through the runtime's allocator releases
  null or the start of a live heap object; anything else stops the program.

A byte is addressable unless the runtime knows it is not:

| Bytes | Not addressable |
| --- | --- |
| A heap object's slot (the runtime's allocator) | after the object's requested size; the whole slot while the object is released and quarantined |
| A local the pass tracks (§3) | its redzones, always; the object itself before its scope begins and after it ends within the activation |
| A global the unit defines and tracks (§4) | its redzone |
| Low memory | the null page (hardware) |

Every other byte is addressable: untracked memory (other allocators,
`mmap`, string literals, locals the pass proves need no tracking, frames
of code built without WeaveC) and the bytes of live tracked objects.

**What the guarantee means for the bug classes:**

- **Heap overflow and underflow**: an access that leaves a heap object
  forwards traps at its first byte past the object; one that jumps over
  the slot's tail into another live object, or reaches backwards into the
  previous slot's object, passes (as with RFC 0033's guards).
- **Heap use after free and double free**: trap while the block is in the
  quarantine (16 MiB by default); after its storage is reused, a stale
  pointer sees the new object (as before).
- **Stack and global overflow**: an access that leaves a tracked object
  into its redzone traps; one that jumps over a redzone into another
  object passes. Redzones are at least 16 bytes and grow with the object
  (§3, §4).
- **Use after scope**: traps. **Use after return**: not caught (the frame
  is unpoisoned when the function returns, because frames of code without
  WeaveC reuse the stack).
- **Null dereference**: stopped by the hardware fault on the null page at
  a small offset, and by the guard's null test of the base at a variable
  or large offset (§2.4).
- **Overflow inside an object**: caught by (B) for indexes into declared
  arrays; not caught for pointer walks or library calls (a `strcpy` into a
  field bounded only by the field's type).

### Assumptions

- **A1 — untracked memory.** An access whose bytes the runtime does not
  track (memory from another allocator, `mmap`, string literals, code
  built without WeaveC) is to a live object it stays inside.
- **A2 — library calls.** A function in the library table accesses only
  what its row says; a function outside it is not checked at the call (its
  own accesses are guarded if WeaveC built it).
- **A3 — no concurrency outside the runtime.** A release by another thread
  between a guard and its access (a data race) is not caught; the guard
  and the access are not atomic together.
- **A4 — the shadow.** The program does not write the runtime's shadow or
  metadata except through guarded accesses (a stray write there is caught
  only if it is guarded and lands in a tracked object's redzone).
- **A5 — non-local control flow.** A frame left by `longjmp`, a
  `noreturn` call or another non-local jump from WeaveC-built code has its
  redzones cleared (§3.5); one left by code without WeaveC (an exception
  unwinding through C frames) keeps them until the stack is next used by
  a WeaveC frame at that depth, and a guard that meets one can trap.

The assumptions RFC 0030 and its successors named about callers (A1),
trusted callees (A2), other code's heap invariants (A3) and initialisation
(A5) are not needed: no guard is removed because of what another function
does.

### Where this is more sound than RFC 0034

- **No guard rests on the analysis.** The four classes of wrong proof
  above, and any the engine has not shown yet, cannot remove a guard.
- **Use after scope** traps.
- **Locals and globals are tracked by redzones**, not by exact encoding,
  so an access that crosses from one tracked object into the next traps
  at any width.
- **Every library-table call is guarded**, including those whose need no
  term could state (the runtime evaluates it) and those RFC 0033 left
  `unresolved`.

### Where this is less sound than RFC 0034

- **No compile-time errors by default.** A definite bug the analysis would
  have reported stops the build only under `-fweavec-diagnose
  -Werror=weavec`, or in the `weavec` tool; otherwise it traps when it
  happens, if a guard covers it. The detection gates measure the loss.
- **Leaks** are reported only by the advisory analysis.
- **Subobject bounds of pointer arithmetic and library calls.** RFC 0030's
  `index` and `len` checks compared some accesses with a field's declared
  extent; only indexes into declared arrays keep that (B).
- **Uninitialised pointers** in memory other than locals and the runtime's
  heap (which are zero-filled) point anywhere; their accesses are guarded,
  and an access into untracked memory passes (A1).
- **Without the runtime nothing is enforced** except (B). RFC 0030's
  static checks, which needed no runtime, are gone.

### Bugs deliberately not caught

Use after return; heap accesses that jump over a slot's tail into another
object; use after free after the quarantine recycled the block; data
races; overflows inside an object reached by pointer arithmetic or a
library call; anything in a `WEAVEC_UNSAFE` region; accesses in code not
built by `weavec-cc`.

### Accepted false traps

None on correct code: the fresh projects measure it (gate G1). Programs
that read past an object's end on purpose (word-at-a-time string scans,
json-c's hash) trap, as they do under ASan; they need a `WEAVEC_UNSAFE`
region. A multidimensional array indexed past an inner bound
(`int m[4][4]; m[0][k]` with `k ≥ 4`) traps under (B).

## Detailed design

### 1. The pipeline

`weavec-cc` compiles a C unit as Clang does, with four additions:

1. **Zero-initialised locals.** `-ftrivial-auto-var-init=zero` in the
   enforcing modes (`-fno-weavec-zero-init` turns it off). The runtime's
   allocator zero-fills every block, as before.
2. **Array bounds.** `-fsanitize=array-bounds` with the handlers in
   WeaveC's runtime (§5.4), unless the command line already asks for a
   sanitizer that covers it.
3. **Locations.** When the command line asks for no debug information,
   the unit is compiled with location tracking only
   (`DebugInfoKind=LocTrackingOnly`): instructions carry their source
   locations for the pass and the reports, and no debug information is
   emitted.
4. **The guard pass** (§2–§4), registered at `OptimizerLastEP` (after
   inlining, scalar optimisation and vectorisation, at every optimisation
   level), and the unsafe-region collector (§2.6), which runs over the AST
   before code generation.

No AST rewriting, no prelude and no deferred code generation remain. A
unit compiled with `-fweavec-checks=none`, `-fno-weavec`, or as C++ or
Objective-C is compiled as Clang compiles it.

### 2. Guards

#### 2.1 What is guarded

For each instruction of each function the pass collects *accesses*:

| Instruction | Access |
| --- | --- |
| `load`, `store`, `atomicrmw`, `cmpxchg` | the bytes of its type at its pointer operand |
| `llvm.memcpy`, `llvm.memmove`, `llvm.memset` (and `.inline`) | a range for each memory operand |
| `llvm.masked.load`, `llvm.masked.store`, gathers and scatters | one access per lane, under the lane's mask bit |
| a call of a library-table function (§2.5) | a range or a string for each memory argument |

Not collected: accesses whose address is a constant expression of a
global the unit defines at an in-bounds constant offset, inline assembly
operands, volatile accesses to constant addresses (memory-mapped
registers), and the pass's own shadow accesses.

#### 2.2 The shadow

One shadow byte per 16-byte granule:

| Value | Meaning |
| --- | --- |
| `0x00` | all 16 bytes addressable |
| `0x01`–`0x0F` | the first k bytes addressable, the rest not |
| `0xF1`, `0xF2`, `0xF3` | a tracked frame's left, middle and right redzone |
| `0xF4` | a dynamic stack object's redzone (`alloca`, VLA) |
| `0xF8` | a tracked local out of scope |
| `0xF9` | a tracked global's redzone |
| `0xFA` | a heap slot after its object |
| `0xFD` | a released heap object |

The shadow of an address `a` is the byte at `base + ((a >> 4) & mask)`,
where `base` and `mask` are read from the runtime's descriptor once per
function. The runtime reserves the shadow for a *window* of the address
space at start-up, without backing: the whole user address space on Linux
(2^47 bytes, or the stack's address width), and 2^44 bytes on Darwin, where
the kernel's cost of a reservation grows with its size and user mappings
lie far below 2^44. An address above the window aliases a lower one; the
fast path may then fail spuriously, and the slow path, which knows the
window, lets it pass. Nothing the runtime tracks lies outside the window:
the arena is placed inside it, and a stack or image outside it is not
tracked. When the reservation fails, `mask` is 0 and `base` points to a
zero byte: every guard passes, and the runtime says so once on standard
error.

#### 2.3 A guard

A guard of `w ≤ 16` bytes at `a`, whose alignment keeps it inside one
granule, is:

```
k = shadow(a)
if (k != 0 && (int8)((a & 15) + w - 1) >= (int8)k)
  __weavec_rt_guard(a, w, site)
```

Four instructions when `k` is 0 (shift, mask, load, branch). An access that
may cross a granule checks the shadow of its first and last bytes; a range
of constant length up to 64 bytes checks its first and last granules and
requires the ones between to be 0; any other range calls
`__weavec_rt_range(a, n, site)`, which scans the shadow. The slow path
decides exactly (the window, partial granules, the poison kind), and in
trap mode prints

```
weavec: heap-buffer-overflow: read of 4 bytes at 0x… (file.c:12:9)
```

and traps; in report mode it prints the line once per site and returns.
`site` is a constant `{file, line, column, flags}` built from the
instruction's location, or null.

#### 2.4 Null bases

An access whose address is `p + x`, with `x` not a constant below 4,096
and `p` not known to be non-null (`isKnownNonZero`), also tests `p`: a null
base takes the slow path, which reports a null dereference. A constant
offset below 4,096 from a null base faults on the null page.

#### 2.5 Library calls

A direct call of a function in the library table (`LibrarySpec.txt`) is
guarded before the call, for each pointer argument that the row reads or
writes, over the bytes the row's term gives (`bytes(t)`, `count(t)` times
the element size, one element without a term), with the term evaluated on
the call's arguments; `strlen(aN)` is evaluated by the runtime inside
the string's addressable bytes. A `str` argument is guarded as a string:
the runtime scans it and fails if the terminator is not addressable. A
`printf`- or `scanf`-family call with a constant format guards each `%s`
argument as a string and each `%n` argument as an `int`. A row with a
`wrapper(…)` clause (`strcpy`, `stpcpy`, `strcat`, `sprintf`, `vsprintf`,
`gets`) is redirected to the runtime's checked version, which computes the
length the call writes before it writes. Fortified forms (`chk(…)`) are
guarded as their base row. Calls of `mmap` and `munmap` are redirected to
runtime wrappers that clear the mapping's shadow, so that a mapping placed
where a tracked object's poison was left behind is addressable.

A null pointer argument with a length that may be zero passes; a null
argument with a non-zero length faults in the callee.

#### 2.6 Unsafe regions

`WEAVEC_UNSAFE` on a function or a block makes its source range unsafe. A
collector run over the AST before code generation records the ranges; the
pass leaves unguarded each access, and removes each `array-bounds` check,
whose instruction's location lies in one (an access inlined from an unsafe
function keeps the location of its source).

### 3. Stack objects

#### 3.1 Which locals are tracked

A static `alloca` is tracked unless every access to it is proven in bounds
(§6.1) and its address does not escape (is not stored, passed to a call
other than a lifetime marker or a memory intrinsic on itself, returned,
or converted to an integer). Dynamic allocas (`alloca`, VLAs) are tracked
the same way.

#### 3.2 The frame

The tracked static allocas of a function are replaced by offsets into one
frame `alloca`, aligned to 32 bytes: a left redzone of 32 bytes, then each
object at a 32-byte boundary followed by its redzone (`max(32 - size % 32,
16)` bytes, more for large objects: one eighth of the object, up to 4 KiB,
rounded to the boundary). At entry the pass writes the frame's shadow with
constant stores (redzones poisoned, partial granules encoded); before each
return it writes zeros over it.

#### 3.3 Scopes

A lifetime start of a tracked object writes its shadow (addressable), a
lifetime end poisons it with `0xF8`. Clang emits lifetime markers when
optimising; at `-O0` scopes are not tracked.

#### 3.4 Dynamic objects

A tracked dynamic `alloca` of `n` bytes allocates `round32(n) + 32` bytes;
its tail is poisoned with `0xF4`. Before a stack restore and before each
return, the runtime clears the shadow of the dynamic objects made since
(`__weavec_rt_alloca_unpoison(top, bottom)`).

#### 3.5 Non-local exits

Before each call of a `noreturn` function other than the runtime's own and
the process's exit functions, the pass calls `__weavec_rt_unpoison_stack()`,
which clears the shadow from the current stack pointer to the thread's
stack base. A `longjmp`, `siglongjmp`, an interpreter's error jump written
in assembly but declared `noreturn` (MicroPython's `nlr_jump`), or
`pthread_exit` then leaves no poison in the frames it abandons.

### 4. Global objects

A global the unit defines is tracked unless it is thread-local, in a named
section, has a non-default address space, is a string literal or other
mergeable constant, is declared with an explicit alignment above 16 bytes
that the padding would break, is `common`, or is used only at in-bounds
constant offsets in this unit with external linkage that another unit could
not index (an internal-linkage global whose every access is proven). A
tracked global is replaced by a global of the same name, linkage and
visibility holding the original and a redzone (at least 16 bytes, one
eighth of the object up to 4 KiB, rounded so the whole is a multiple of 32),
aligned to 32. A module constructor at high priority calls
`__weavec_rt_globals_register(array, n)` with `{address, size, size with
redzone}` per global, which poisons each redzone; a module destructor
unregisters them (clears the shadow), so `dlclose` leaves nothing behind.

### 5. The runtime

#### 5.1 The allocator

As RFC 0032 §2 and RFC 0034 built it (size classes, the arena, the slot
words, the quarantine, the huge blocks, one runtime per process on Darwin,
zone promotion), with the shadow of §2.2: an allocation writes 0 over the
object's whole granules, `k` over its partial last granule and `0xFA` over
the rest of its slot; a release writes `0xFD` over the slot; a reused slot
is written again. The arena is reserved inside the window. A huge block
gets at least one poisoned granule after it.

#### 5.2 Entry points

| Entry point | Does |
| --- | --- |
| `__weavec_rt_guard(a, w, site)` | the slow path of §2.3 |
| `__weavec_rt_range(a, n, site)` | a range guard |
| `__weavec_rt_string(s, site)` | a string guard; returns the length |
| `__weavec_rt_strcpy`, `_stpcpy`, `_strcat`, `_sprintf`, `_vsprintf`, `_gets` | the checked library calls of §2.5 |
| `__weavec_rt_mmap`, `__weavec_rt_munmap` | §2.5 |
| `__weavec_rt_unpoison_stack()` | §3.5 |
| `__weavec_rt_alloca_unpoison(top, bottom)` | §3.4 |
| `__weavec_rt_globals_register`, `_unregister` | §4 |
| `__ubsan_handle_out_of_bounds`, `_abort` (weak) | §5.4 |
| `__weavec_rt_shadow` | the descriptor `{base, mask}` |

The object table's stack list, its global table, `__weavec_rt_find`, the
range and room queries of RFC 0034 and `libweavec_chk.a` are deleted.

#### 5.3 Reports

Each failure is named by its shadow value: `heap-buffer-overflow`,
`heap-use-after-free`, `stack-buffer-overflow`, `stack-use-after-scope`,
`dynamic-stack-buffer-overflow`, `global-buffer-overflow`,
`null-dereference`, `unterminated-string`, `index-out-of-bounds`, and the
allocator's `invalid-release` and `double-free`. Trap mode prints the line
and traps (`SIGTRAP`, or `SIGILL` on x86-64); report mode prints it once
per site and continues; `WEAVEC_RT_ABORT=1` makes report mode trap.
`WEAVEC_RT_REPORT_LOG` and `WEAVEC_RT_STATS` stay.

#### 5.4 Array bounds

Clang's `array-bounds` instrumentation calls
`__ubsan_handle_out_of_bounds_abort(data, index)` (trap mode) or
`__ubsan_handle_out_of_bounds(data, index)` (report mode,
`-fsanitize-recover`); WeaveC's runtime defines both weakly, reading the
source location from `data`, so a program that also links UBSan's runtime
gets UBSan's.

### 6. Removing guards

Every rule below is local to a function, needs no fact about another
function except LLVM's `nofree` attribute, and is monitored by `verify`
mode (§7): a guard a rule removed is emitted there as a *monitor*, whose
failure reports `weavec.proven`.

#### 6.1 In bounds of a local or global

An access whose address is `O + x`, where `O` is a static `alloca` or a
global with an exact definition in this unit of size `S`, and `x` is a
constant or an expression whose range (ScalarEvolution's signed range, or
known bits) lies in `[0, S - w]`, needs no guard. Its object is live for
the access: a local for the activation, a global for the program. (A
proven access to a local after its scope ends is not caught; §3.3 covers
the guarded ones.)

#### 6.2 Covered by a dominating guard

A guard of `[o2, o2 + w2)` from base `p` is removed when a guard of
`[o1, o1 + w1) ⊇ [o2, o2 + w2)` from the same base value dominates it and
no instruction on any path between them may release memory or poison the
shadow: a call not marked `nofree` (LLVM infers it, and the library
functions carry it), a lifetime end of a tracked local, a release. The
availability of guards is computed by a forward data-flow problem over the
function's blocks with those instructions as kills.

#### 6.3 Merged in a block

The guards of one block on one base with constant offsets whose hull is
at most 32 bytes, with no kill and no call that may not return between
them, become one guard of their hull at the first of them.

#### 6.4 Loop ranges

For an access in a loop whose address is an affine recurrence
`{start, +, step}` with `start` invariant in the loop, whose loop has a
computable maximum trip count and no kill, the pass computes the hull of
the addresses of every iteration in the loop's preheader and asks the
runtime whether it is addressable (`__weavec_rt_range_ok(lo, n)`, no
report). Inside the loop the guard becomes `if (!ok) guard(...)`: when the
whole range is addressable no iteration checks, and when it is not, each
iteration checks as before, so a loop that would have left early never
traps early. Loops with a constant trip count below four are left alone.

#### 6.5 What stays guarded

Everything else, including every access through a pointer loaded from
memory or passed in, however often a dominating guard on another pointer
covered its bytes.

### 7. Modes and options

| Option | Effect |
| --- | --- |
| `-fweavec-checks=trap` (default) | guards trap |
| `-fweavec-checks=report` | guards report once per site and continue |
| `-fweavec-checks=verify` | as `trap`, and §6's removed guards are emitted as monitors |
| `-fweavec-checks=none` | Clang's output; no runtime |
| `-fno-weavec-zero-init` | locals are not zero-initialised |
| `-fweavec-diagnose` | run the analysis (§8) and print its findings as warnings |
| `-fweavec-ledger=<path>` | write the enforcement ledger (§9) |
| `-fweavec-summary` | print the summary line |
| `-fno-weavec` | plain Clang |

The runtime is linked into every link of an enforcing build unless
`runtimeObstacle` finds a reason (a sanitizer that replaces the allocator,
`-ffreestanding`, `-nostdlib` and its kin, a target other than 64-bit
Darwin or Linux), which a note at the link names; units are then compiled
without guards. Deleted, with no aliases: `-fno-weavec-runtime`,
`-f[no-]weavec-stack-objects`, `-f[no-]weavec-global-objects`,
`-fweavec-require=`, `-fweavec-link=`, `-fweavec-link-budget=`,
`-fweavec-ledger-format=`, `-fweavec-print-prelude`, and the `.weavec`
unit records. `-fweavec-budget=`, `-fweavec-unit-budget=` and the
`-W…weavec…` flags apply to `-fweavec-diagnose`.

### 8. The advisory analysis

The engine, the kinds, the library table's analysis rows, the summaries
and the whole-program analysis stay. They run in `weavec` (and `weavec
--whole-program`) and under `-fweavec-diagnose`, and report diagnostics:
in `weavec` a confirmed definite finding is an error, as before; under
`-fweavec-diagnose` every finding is a warning (`-Werror=weavec` makes them
errors). Their outcomes are no longer enforcement outcomes, so:

- **The analysis outcomes** are `proven`, `violation`, `unresolved` and
  `trusted`. `checked` and `guarded` belong to the enforcement ledger
  (§9); the engine's check witnesses, the check planner, the boundary
  propagation into guards, `dropGuardedPossible`, the require levels and
  `WEAVEC_REQUIRE_SAFE` are deleted.
- **The analysis ledger** (`weavec --ledger`, format version 2) is
  deleted; `weavec` prints diagnostics and a summary line of its sites.
- **Unit records and the link step** are deleted. `weavec
  --whole-program` keeps the interface facts and the program's
  function-pointer slots in memory.

### 9. The enforcement ledger

The pass records each access it collected, before removal: its function,
location, operation (`load`, `store`, `rmw`, `copy`, `set`, `call:<name>`),
width, and outcome:

| Outcome | Reasons |
| --- | --- |
| `guarded` | `access`, `range`, `string`, `checked-call`, `loop-range` |
| `proven` | `in-bounds`, `dominated`, `merged` |
| `unguarded` | `unsafe`, `no-runtime` |

`-fweavec-ledger=<path>` writes them as JSON (`weavec-ledger` version 3: a
`units` array with `source`, `object`, `config`, `summary` and `rows`), one
file per unit (a directory path) or one per compile (a file path).
`-fweavec-summary` prints

```
weavec: file.c: 812 accesses: 431 proven, 381 guarded, 0 unguarded
```

### 10. Deletions

`lib/Frontend`: `CheckEmitter`, `DeferredCodeGenConsumer`, `Prelude`,
`ObjectRegistration`, `ZeroInit`, `UnitRecord`, `RecordPayload` (the
codec), `LinkStep` (but the slot and declaration verification
`--whole-program` uses), `DispatchEdges`, `GuardPasses` (replaced), the
analysis ledger writers. `lib/Analysis`: `CheckPlanner`, the engine's check
witnesses, the require levels. `lib/Core`: `CheckPlan`, the outcomes and
templates only checks used. `runtime/`: the stack list, the global table,
the exact stack and global encoding, `libweavec_chk.a`. The tests of all of
these; the scripts' legacy and golden modes; RFC 0034's corpus gates that
measure deleted outcomes.

### 11. Tests and the corpus

- **Lit.** `test/Guards/` pins the pass's IR (FileCheck on `-emit-llvm`)
  and its run-time behaviour; `test/Emission` and `test/Prelude` are
  deleted.
- **Unit tests** run the pass on IR text (`GuardPassTest`).
- **Cases.** The runner checks run-time behaviour against the default
  build and diagnostics against `weavec`: `BUG` markers against the tool,
  `TRAP: <kind>` (a §5.3 report kind) and `STOP` against the build. The
  facet-ledger markers are replaced by `GUARDED` and `UNGUARDED` on the
  enforcement ledger.
- **Corpus.** `scripts/corpus-gate.py` builds and tests each config,
  counts traps and wrong monitors, times the benchmarks, and keeps a
  ratchet of the ledger's guarded share.
- **Evaluation sets.** `fresh35` is the ten projects of the motivation
  (pinned at the SHAs measured); `sealed35` is five more, chosen and
  pinned at stage S0, built once at the end; the 15 historic bugs of the
  motivation and the 64-program blind detection set are the detection
  sets.

## Annotation surface

- `WEAVEC_UNSAFE` keeps its spelling; its meaning becomes: the accesses in
  the function or block are not guarded and its array indexes not checked.
- `WEAVEC_REQUIRE_SAFE` is deleted (with the require levels).
- The other annotations are read by the advisory analysis only, as before.
  RFC 0036 gives the ownership and extent annotations an enforcement role.

`weavec.h` moves to version 0.10.

## Diagnostics

Deleted: `unresolved-operation` and `unchecked-operation` (the require
levels). The other ids stay, reported by the advisory analysis; their
entries in `docs/annotations.md` and `docs/data/diagnostic-remedies.json`
say that they come from `weavec` and `-fweavec-diagnose`. Run-time reports
(§5.3) are not diagnostics and have no ids.

## Implementation plan

The stages land on `rfc0035-guard-by-default` and ship as one change:

- **S0 Tests first.** `fresh35` and `sealed35` configs; the wrong proofs
  and false stops of the motivation as cases; the CVE and blind sets as
  scripts.
- **S1 The runtime**: the shadow encoding, the window, the allocator's
  shadow, the entry points of §5.2, the deletions in `runtime/`.
- **S2 The pass**: accesses, guards, library calls, unsafe regions, sites,
  array bounds, the ledger.
- **S3 Frames and globals**: §3, §4.
- **S4 Removal**: §6.
- **S5 The driver**: §7, and the deletion of the emission path and
  records.
- **S6 The analysis**: §8.
- **S7 Tests**: lit, unit tests, the runner, the corpus gate, CI.
- **S8 Fresh iterations, gates, the sealed run, documentation.**

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference is
`$WEAVEC_LLVM_PREFIX/bin/clang`; darwin-arm64 unless named; ratios of
instructions retired, or best-of-three user CPU on an idle machine.

**Drop-in**

- **G1. Fresh projects.** Every `fresh35` project builds with its own build
  and `CC=weavec-cc` (default flags) and passes its own test suite with the
  reference compiler's results, in trap and report mode, with no trap
  except where the project reads out of bounds on purpose (json-c's
  `hashlittle` without `PRECISE_MEMORY_ACCESS`, MicroPython's conservative
  stack scan), which its config marks unsafe or switches off with the
  project's own option.
- **G2. Sealed projects.** Built once at the end and recorded as measured.
- **G3. No regression.** The `fresh34` and RFC 0033 fresh and sealed sets,
  and the corpus configs, build and pass.

**Detection**

- **G4. Real bugs.** At least 14 of the 15 historic bugs stop at the
  faulty access; no fixed revision traps.
- **G5. Blind sets.** On the 64-program blind set and the 48-program
  in-tree blind set, the default build stops at least as many bugs as ASan
  (`-O1 -fsanitize=address`), with no false stop on a twin.
- **G6. Monitors.** No `weavec.proven` report in `verify` mode on any case,
  on the `fresh35` test suites or on the benchmarks.

**Cost**

- **G7. Run.** Every `fresh35` and `fresh34` workload at most ASan's ratio
  or 2.0×, whichever is larger; their geometric mean at most 1.8×; Lua,
  zlib and cJSON at most 2.0×, 1.4× and 1.4×.
- **G8. Start-up.** An empty program starts and exits within 3 ms of the
  reference's, and git's test subset within 1.3× its wall time.
- **G9. Build.** Each project's build within 1.5× the reference
  compiler's CPU time; no unit above 3×.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes; the library's line count is
  at least 15,000 lines below RFC 0034's.
- **H2.** The documentation describes the change (README, guarantees, CLI,
  architecture, roadmap, AGENTS.md); `npm test && npm run build` in
  `docs/`; this RFC is marked Implemented.

## Implementation amendments

These record where the implementation departs from the design above. They
override the body.

1. **Guards are inserted early and expanded late (§1, §2).** One pass at
   `OptimizerLastEP` saw code the optimiser had already changed on the
   strength of the bug itself: an out-of-bounds read in a loop made the
   loop infinite, and a use after free became `undef`, before any guard
   existed. The pass is four:
   - **GuardInsert** at `PipelineStartEP` puts a *marker* call before each
     access: `__weavec.guard(ptr, i64 width, i32 flags, ptr nullBase, i32
     row)`, `__weavec.range(ptr, i64 length, i32 flags, i32 row)`,
     `__weavec.strlen(ptr, i64 max, i32 flags, i32 row)` (its result
     replaces a `strlen`), and `__weavec.disjoint(ptr, ptr, i64)` (§2.5).
     Markers are `nounwind`, `nomerge`, read only the runtime's memory
     (`strlen` the string too), capture nothing, and are not `willreturn`,
     so the optimiser may neither drop nor merge them nor move an access
     past one. It also records the ledger rows (§9).
   - **GuardPrune** at `PeepholeEP` removes the markers rule 6.1 proves on
     a local or global at constant offsets, so that SROA still promotes
     those locals.
   - **GuardLoop** at `VectorizerStartEP` applies rule 6.4 (amendment 6).
   - **GuardExpand** at `OptimizerLastEP` applies rules 6.1–6.3, lays out
     frames and globals and turns each marker into its check.
2. **Scope barriers (§3.3).** A loop-invariant marker could be hoisted
   above the `lifetime.start` of a local declared in the loop, where its
   shadow still says out of scope. Beside each lifetime marker
   GuardInsert puts `__weavec.scope()`, which writes the runtime's memory,
   so no marker crosses it; GuardPrune drops the barriers of locals SROA
   promoted and adds them beside the lifetime markers inlining brought.
   At `-O0` Clang emits lifetime markers too (use-after-scope is on).
3. **The shadow and the arena (§2.2, §5.1).**
   - The window on Darwin is 2^42 bytes, and the arena's class regions
     2 GiB (1 GiB when that cannot be reserved; a full region's blocks are
     mapped one by one). Together they cost about 1 ms of start-up, where
     2^44 and 4 GiB regions cost 2.7 ms. The runtime warns if the arena
     lies outside the window.
   - The shadow of the lowest 64 KiB is `0xFE`: a guard on an address
     there fails as a null dereference, even when the optimiser moved the
     access itself to the other arm of a `select` of the pointer.
   - Committed arena slots not yet handed out are `0xFA`, as is the
     granule before each region, so that a far index or an underflow of a
     region's first object fails. The poisoned tail of a slot after its
     object stops at 64 KiB, and the shadow of a large object is mapped
     afresh rather than written, so that untouched large blocks cost no
     memory; so are the whole pages of a recycled large slot rather than
     zeroed with stores. A released huge block (one mapped on its own) has
     its first
     and last 64 KiB poisoned; its middle, mapped without access, faults
     instead of reporting.
4. **Fast paths (§2.3).** A guard inside one granule is three instructions
   (shift and mask, load, branch); the partial-granule test is on the slow
   side of the branch. An access of up to 16 bytes loads the two shadow
   bytes it may touch at once, and a constant range of up to 113 bytes
   loads eight with a constant mask; a nonzero byte only sends the access
   to the exact check. On AArch64 the guarded compiles disable the
   backend's conditional compares, which folded the partial test back
   into the fast path. A guard merged by rule 6.3 checks each access it
   covers on its slow path, so a failure names the access that fails.
5. **Frames (§3.1, §3.2).** The left zone is 64 bytes and each redzone at
   least 32 bytes: a 16-byte redzone let a small index jump over it. A
   local is tracked when any of the objects a guarded pointer may be based
   on (through phis and selects) is that local.
6. **Loop ranges (§6.4)** version the loop instead of conditioning each
   guard: a loop with a known trip count, one exit block and nothing that
   may end a lifetime gets a copy without the guards of its invariant and
   affine accesses. When the trip has at least 16 iterations and each
   access's range over the trip is addressable (`__weavec_rt_range_ok`), the
   copy runs, and LLVM may vectorise it; otherwise the guarded loop runs.
   A loop with a constant trip count of at most 32 and at most 300
   instructions over the trip (markers aside) is unrolled fully instead,
   as LLVM unrolls it without guards: a marker is not `willreturn`, which
   keeps LLVM's own full unrolling away (BearSSL's field arithmetic ran at
   3.6x before, 1.8x after). Rule 6.3 then merges the unrolled guards, over
   a hull of up to 64 bytes (not 32). Verify mode does not version.
7. **Library calls (§2.5).**
   - `wrapper(NAME)` takes no flags any more and means: the call is
     replaced by `__weavec_rt_NAME`, with the call's arguments and the site
     (first when the call is variadic). The rows with one are `strcpy`,
     `stpcpy`, `strcat`, `sprintf`, `vsprintf`, `snprintf`, `vsnprintf`,
     whose wrappers compute what they write and check the `%s` and `%n`
     arguments a format names (positional ones too, through a `va_list`
     too); `memchr`, `strchr`, `strcmp`,
     `strncmp`, `strcasecmp` and `strncasecmp`, whose wrappers check only
     the bytes the call read up to where it stopped (a guard of their terms
     made a search loop quadratic, and failed where a comparison stopped
     early);
     `gets`, whose wrapper checks each byte before it writes it; the
     scanf family, whose wrappers check after the call what each
     conversion that ran wrote (a guard of every destination before the
     call failed on a `%c` whose conversion did not run, in giflib); and
     `mmap`
     and `munmap`. The calls that write through no pointer (the searches,
     comparisons and mappings) are redirected by GuardExpand, after the
     optimiser has folded what it could (`strcmp` of two constants, which an
     inlined tree of comparisons depends on); GuardInsert guards only the
     first byte of each of their string arguments, which the optimiser may
     turn into a load. Such a call that reads exactly a constant of at most
     113 bytes through each pointer that is not a literal keeps inline
     guards instead.
   - A row whose result is a `value(t)` it computes only from what it reads
     (`strlen`, `strnlen`) is replaced by the string guard that computes
     `t`.
   - The new row clause `wide` marks the wide-character rows: their
     `count` terms are scaled by the module's `wchar_size`, and their
     strings are not scanned (a known gap: a wide string argument is not
     guarded).
   - A null the row accepts (`null-ok`) passes: a string's site carries a
     flag the runtime honours, and other arguments are guarded over zero
     bytes when null. The arguments a call releases or reallocates are not
     guarded; the allocator validates them.
   - A copy's operands must not overlap: `llvm.memcpy` (except an aggregate
     assignment, which carries `!tbaa.struct`) and a row's `disjoint`
     clause get a `__weavec.disjoint` marker, and an overlap fails as
     `overlapping-copy`.
8. **Unguarded code (§2.6).** A function its author keeps from
   AddressSanitizer (`no_sanitize("address")`, `no_sanitize_address`,
   `disable_sanitizer_instrumentation`) is an unsafe region, so a
   conservative collector's stack scan needs no source change. An
   `available_externally` body (a C99 inline definition) is guarded, since
   the inliner copies it into the unit.
9. **The runtime (§5).** Reports are formatted by the runtime itself, not
   by `snprintf`, which a program may define. `__weavec_rt_string` is
   `__weavec_rt_strlen(s, max, site)`, which fails a null string as a null
   dereference unless the site allows null. New entry points:
   `__weavec_rt_range_ok`, `__weavec_rt_overlap`, `__weavec_rt_null`, and
   the wrappers of amendment 7. The allocator's failures are not §5.3
   report kinds: a release of what it did not hand out, or of a block it
   already took back, prints `weavec: invalid release of 0x…: <why>`
   without a location and stops the program in every mode.
10. **The ledger (§9).** A `proven` row may also have the reason
    `optimized` (the optimiser removed every copy of the access); the
    guarded reason `loop-range` is not used (amendment 6 keeps the guard
    in the copy that runs when a range fails), nor the unguarded reason
    `no-runtime` (a unit built without the runtime has no guard pass and
    so no rows).
11. **Zero-initialisation (§1)** stops at locals of 4 KiB: a larger one
    (or the limit of the command line's
    `-ftrivial-auto-var-init-max-size`) is left uninitialised, as without
    WeaveC. Clearing a 229 KiB local on every iteration of a loop cost
    opus's test suite 14 times its instructions.
12. **Assumptions.** `WEAVEC_ASSUME` no longer compiles to a run-time
    assertion: no guard depends on it, so it is a statement to the
    advisory analysis only.
13. **The case grammar (§11).** A `BUG` line may carry `MISS: <reason>`
    when the advisory analysis is known not to report it (most often: a
    call's argument against a callee's requirement, which only RFC 0034's
    call-site checks reported; the guard now stops in the callee). The
    file marker `TRAP-AT: <unit>:<line>` expects a stop in a unit other
    cases share. The ASan oracle builds with `-fsanitize=address,array-bounds`.
14. **Linux (§5), found by CI after the gates.** The runtime reserves its
    shadow in a constructor on every target, not only Darwin's: a
    function reads the shadow descriptor once on entry, so `main` kept
    the empty one, and passed every guard, when its first allocation
    reserved the shadow. A release the arena does not hold is an
    `invalid release` before it reaches the C library when it lies on
    the calling thread's stack or in a loaded image, as Darwin's zone
    lookup already found. A report's location skips the frames inlined
    from functions declared `artificial`, as glibc's fortified wrappers
    are, and names their call site (§5.3).

### Measured gates

Measured on 2026-10-06, darwin-arm64, ratios of instructions retired (best
of three) unless named, with frozen builds of the branch: `stage3` (tree
3adb3d2) for the fresh and sealed runs, the final tree for the rest. The
machine was shared during most runs; user-time ratios were noisy and are
not used.

- **G1 (fresh35): passes, with two notes.** All ten projects build with
  their own builds and `CC=weavec-cc`, with configure results identical to
  the reference compiler's, and pass their suites in trap, report and
  verify mode as the reference does: git (all 1061 scripts), tcpdump and
  libpcap, libtiff, BearSSL, YARA, gawk, libarchive, Tcl (526,311 tests),
  json-c with its `VALGRIND` option (its `hashlittle` over-read traps
  otherwise, as under ASan), MicroPython. MicroPython's stack scan carries
  `no_sanitize_address` under `#if __GNUC__ > 4 || 4.8`, which Clang's
  `__GNUC__` of 4 never reaches: it traps, as under ASan, until that
  condition also accepts Clang (one line). The earlier stage builds failed
  this gate on compiler crashes, null `null-ok` strings, `realloc` of empty
  blocks, Darwin semaphores and a `strncasecmp` miscompile, each fixed.
- **G2 (sealed35), recorded as measured.** GNU sed 4.9, GNU m4 1.4.19,
  wolfSSL 5.7.2, xxHash 0.8.2 and chibi-scheme 0.11, chosen and pinned
  after the fresh runs (not at S0 as planned), built once with `stage3`:
  wolfSSL, xxHash and chibi build and pass as the reference does; sed and
  m4 did not build, because the allocator archive defined `reallocarray`,
  which Darwin's libSystem lacks, and changed a configure probe (fixed
  since: the archive defines only what the C library has); with the probe
  overridden they passed but for gnulib tests that the runtime's `strlen`
  read past a page (fixed since) and three deliberate out-of-bounds tests.
  No wrong proof; every workload below ASan (sed 2.47x against 3.05x, m4
  1.86x against 3.31x, wolfSSL 1.88x against 2.07x, xxHash XXH3 1.63x
  against 3.02x, chibi 1.81x against 3.60x).
- **G3: passes, after one fix.** With `stage4`, through `corpus-gate.py
  --full`: RFC 0033's fresh and sealed sets (zstd, libuv, oniguruma, redis,
  expat, pcre2, libevent, libsodium, libxml2, libpng, mbedtls, msgpack-c,
  yyjson) and RFC 0034's (QuickJS, LMDB, Janet, brotli, xz, libdeflate,
  zlib-ng, curl, cmark, libgit2, libjpeg-turbo, opus, flac, giflib, wren)
  build and pass their suites in trap, report and verify mode, with no
  report and no `weavec.proven`, but giflib, whose `sscanf` `%c`
  destination was guarded for a conversion that did not run (fixed since:
  amendment 7's scanf wrappers). mbedtls's test helpers' over-read of a
  certificate array, which RFC 0034 reported, now lands inside the next
  global and passes, as under ASan (§ *Bugs deliberately not caught*).
- **G4: 13 of 15 stop with a report, 1 faults, 1 is neutralised.** Lua's
  `getlocal` bug stops by SIGBUS, as under ASan; jq's CVE-2025-48060 reads a
  string whose terminator zero-initialisation supplies, so it does not
  misbehave (inferred from its output). No fixed revision traps.
- **G5: passes.** 64-program blind set: 60 stop at the faulty access
  (ASan 58; a 61st stops in the callee, where ASan also reports it); the
  in-tree detection set 58 of 61 (ASan 52) and blind set 44 of 48 (ASan
  42); no twin stops.
- **G6: passes.** No `weavec.proven` report in verify mode on any case,
  fresh35 or sealed35 suite or benchmark.
- **G7: Lua, cJSON and one BearSSL subset carried forward.** Every fresh35
  and sealed35 workload is at most ASan's ratio or 2.0x but BearSSL's
  `EC_c25519_m31` subset, which loop unrolling (amendment 6) brought from
  3.6x to 1.8x (ASan 1.9x) after the run; geometric means 1.5x (set A) and
  1.76x (set B). Lua 2.24x (gate 2.0, ASan 2.49x), zlib 1.28x (gate 1.4),
  cJSON 1.76x (gate 1.4, ASan 3.05x): cJSON's remaining cost is a guard per
  byte in loops whose exit depends on the data (not versionable), and Lua's
  is three guards per bytecode instruction that no local rule removes.
  The fresh34 workloads are within their limits on the gate's best user
  CPU (geometric mean 1.49x) but not on instructions retired, where brotli
  is 2.59x (ASan 2.32x; its hash-chain walk is not affine) and the
  geometric mean 2.04x.
- **G8: passes.** An empty program starts and exits 1.15 ms (minimum) to
  1.5 ms (median) after the reference's (ASan: 90 ms); git's test suite
  runs in 1.10x the reference's wall time.
- **G9: carried forward.** The fresh35 builds are within 1.5x of the
  reference compiler's but gawk (1.51x), and sealed35's but chibi (1.52x);
  among the older sets, measured under load, QuickJS, LMDB, brotli,
  libjpeg-turbo, zstd, redis, pcre2, libpng and yyjson exceed 1.5x. Units
  above 3x: libtiff's `tif_pixarlog.c` (4.4x), flac's `lpc.c` (4.7x),
  brotli's `compress_fragment.c` (4.5x), yyjson.c (4.4x),
  `pcre2_match.c` (4.0x), cmark's `scanners.c` (3.8x), libjpeg-turbo's
  `jchuff.c` (3.7x), YARA's `grammar.c` (3.5x), wolfSSL's `tests/api.c`
  (3.2x). Instruction selection and the register allocator over the
  guards' slow paths dominate; ASan takes 1.4–1.8x on the units measured.
  Smaller slow paths (a site record that carries the width, a slow path
  shared by a block's guards) are the next step.
- **H1: passes.** The library is 55,992 lines (RFC 0034's limit was
  72,000); `check-hygiene.py` passes with a library limit of 57,000.
- **H2: passes.** The documentation describes the change; `npm test` and
  `npm run build` pass in `docs/`.

## Drawbacks

- **The analysis no longer protects anything by itself.** Until RFC 0036
  lands, ownership inference contributes diagnostics only, and the run
  time is that of guarding every access the local rules cannot prove.
- **Compile-time errors are opt-in.** A team that relied on WeaveC
  failing the build at a definite use after free must add
  `-fweavec-diagnose -Werror=weavec` (and accepts its false errors) or run
  `weavec` in CI.
- **A larger stack and data segment**: redzones around tracked locals and
  globals.
- **An LLVM pass.** The enforcement depends on the pass running in
  `weavec-cc`; an object compiled to bitcode and optimised elsewhere is
  guarded only if the pass ran before.

## Alternatives

- **Repair in place** (the milestone's option B): fix the nine classes of
  false stop and the four of wrong proof, rebuild joins, make definite
  errors warnings. Each of the last three milestone analyses found new
  classes after the previous RFC fixed the old ones; the analysis stays
  trusted.
- **Use AddressSanitizer itself.** It has the instrumentation, the frames
  and the globals, but no static removal beyond its own, no
  production-oriented trap mode, a heap with large redzones (memory 2–7×),
  interceptors that need a shared runtime on Darwin, and no ledger. The
  design borrows its layout and shadow encoding rather than its runtime.
- **Keep RFC 0034's exact stack encoding.** It catches a forward jump
  between adjacent locals that redzones miss, at about 60 instructions per
  guard and the false traps of compound literals and non-local jumps.
- **Run the analysis by default with warnings only.** It would keep the
  build cost (11–98× on heavy files) for diagnostics most builds do not
  read.
- **Hardware tagging (MTE, MIE).** Not available on the reference machine;
  future work.

## Prior art

- **AddressSanitizer** (Serebryany et al., USENIX ATC 2012): shadow
  encoding, frame and global redzones, use-after-scope via lifetime
  markers, `__asan_handle_no_return`. The shadow granule here is 16 bytes
  (the arena's), the heap needs no redzones because size classes leave
  one, and the window replaces a fixed full-address-space mapping on
  Darwin.
- **SoftBound/CETS, Low-fat pointers**: pointer-based bounds; rejected as
  ABI-visible or allocator-bound.
- **Bounds-check elimination in JVMs and in Go**: range checks hoisted out
  of loops with a fallback to per-iteration checks.
- **Clang `-fsanitize=array-bounds`, `-fbounds-safety`**: typed bounds.
- **Checked C, Rust**: contracts at boundaries checked locally, the model
  RFC 0036 takes for ownership.

## Unresolved questions

- **The window on Darwin.** 2^44 bytes is a guess from the layouts
  measured; S1 measures start-up cost against it.
- **How much the loop rule removes** in practice, and whether loop
  versioning (two copies of the loop) pays where the flag test does not.
- **Redzone sizes**: the cost in stack and data size of the sizes chosen.

## Future work

- **RFC 0036, ownership as checked contracts**: inferred function
  contracts (a borrowed parameter is live and has `n` bytes; a callee does
  not release it) checked once at entry, letting the body's guards go;
  contracts proven at the callers drop the entry check too.
- **Deterministic heap temporal safety**: freed memory reused only after a
  sweep finds no pointer to it.
- **Use after return** with a fake stack for frames whose locals escape.
- **Adoption** (records in object sections, baselines, `weavec.toml`,
  relocatable installs), renumbered RFC 0037.
- **Hardware tagging** where the processor has it.
