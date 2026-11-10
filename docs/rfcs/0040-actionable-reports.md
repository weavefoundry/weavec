# RFC 0040: Reports you can act on

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-08
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0035 §5.3 (the report
  format), RFC 0037 §6 (the fault report) and RFC 0039's *Diagnostics*
  (the invalid release report and its reasons); the library table's
  release rows (RFC 0035 §2.5, RFC 0036 §6); the global records of RFC
  0035 §4; the frame layout of RFC 0035 §3 (a descriptor in the left
  redzone); `weavec-cc`'s debug information (RFC 0035 §1, amendment 1);
  AGENTS.md rule 9's line budgets. Finishes RFC 0038's and RFC 0039's
  stage S7 (§0). The guarantee is unchanged.

This RFC was drafted before implementation. On 2026-11-08 the project
owner accepted the recommendation of a milestone assessment of v0.20.0:
first make the gates green and settle an untriaged report in CPython's
test suite on Linux (option 0), then make every report say what a
developer needs to fix the bug without reproducing it under another tool
(option A), in one large change, with breaking changes and no
compatibility layers. Tracking programs' own allocators, deterministic
spatial checks (escape checks, exact heap bounds), Windows and
deterministic use after return were considered and are not taken
(*Alternatives*).

## Summary

WeaveC stops a program at its first bad access and says where the access
is. It does not say where the memory came from. A use after free names
the read but not the `malloc` that made the block or the `free` that
released it; a double free names no source location at all; a stack
overflow does not name the local; `WEAVEC_RT_BACKTRACE=1` prints raw
addresses and offsets, starting in the runtime's own frames. ASan prints
all of these, so a developer who finds a bug with WeaveC rebuilds with
ASan to fix it. Separately, the weekly corpus run has been red since
2026-10-12, and its last run logged a `heap-use-after-free` in CPython's
test suite on both Linux platforms that nobody has explained.

This RFC:

0. **Makes the gates green** (§0): settles the CPython report (a WeaveC
   false trap is fixed; a CPython bug is triaged with evidence), fixes
   the runner-specific failures of the weekly corpus run, and finishes
   RFC 0038's and RFC 0039's stage S7, so that both become Implemented
   and this RFC's gates start from a green tree.
1. **Records where every heap block was allocated and released** (§1–§3):
   a frame-pointer unwinder, a store of unique call stacks, and two stack
   identifiers per heap slot and huge block.
2. **Symbolizes stacks in the process** (§4): `weavec-cc` doubles as the
   symbolizer on Linux, `atos` serves Darwin, and every compile emits line
   tables unless its command line says `-g0`, so a report's frames carry
   file and line in a release build.
3. **Rewrites the reports** (§5): every report prints the stack of the
   failing access; a heap report adds the block's release and allocation
   stacks; a stack report names the local and its function from a
   descriptor the frame carries; a global report names the global; a
   release report names the release's source location and gets a kind of
   its own; every report ends in one summary line.
4. **Writes reports as JSON on request** (§6), and the case runner and
   the corpus gate read them that way instead of scraping stderr.
5. **Says how much memory the runtime does not track** (§7): a counter of
   `WEAVEC_RT_STATS=1`.

Deleted: `WEAVEC_RT_BACKTRACE`, the `invalid release of` line and its
parsers, the scripts' regular expressions over report text. Expected
cost: allocation-heavy programs up to 10% more CPU, the corpus
benchmarks within 3%; compiles up to 8% more CPU and objects about 30%
larger (line tables).

## Motivation

Measured on main at `0aa819d9` (v0.20.0) on darwin-arm64 (`dev` preset),
against Homebrew's `clang` 23.1 with `-fsanitize=address`, and on the
weekly corpus workflow's run `42396134193` (2026-11-07, the tree of
PR 56).

### A use after free says where it happened, not why

```c
struct s { char name[16]; void (*cb)(void); };
__attribute__((noinline)) struct s *make(void) { struct s *p = malloc(sizeof *p); strcpy(p->name, "x"); return p; }
__attribute__((noinline)) void drop(struct s *p) { free(p); }
__attribute__((noinline)) int use(struct s *p) { return p->name[0]; }
int main(void) { struct s *p = make(); drop(p); return use(p); }
```

```console
$ weavec-cc -g -O1 uaf.c -o uaf && WEAVEC_RT_BACKTRACE=1 ./uaf
weavec: heap-use-after-free at uaf.c:6:56: read of 1 bytes at 0xbc80000000
weavec: 0xbc80000000 is inside a released heap block at 0xbc80000000
  #0 0x1021e92ac in trapNow + 80
  #1 0x1021ea964 in weavecRtFail + 700
  #2 0x1021e31f4 in failAt + 292
  #3 0x1021e2774 in __weavec_rt_guard + 84
  #4 0x1021a45dc in use + 88
  #5 0x19b523e80 in start + 6688
```

The report names the read. It does not say which `free` released the
block or which `malloc` made it, which in a real program are in other
files, often other threads, and long before. The backtrace is opt-in,
starts with four frames of the runtime, has no file or line, and is
printed for an access only. ASan's report for the same program gives the
block's size and the offset into it, the stack that freed it and the
stack that allocated it.

### A double free has no location at all

```console
$ ./df a b
weavec: invalid release of 0xbd80000000: the block was already released
```

The allocator stops a bad release, and it knows no source location: the
report has none, and neither the release nor the earlier one is named.
In the weekly run's injection table, 8 of the 33 injections that stop
are reported as `invalid-release at no location`, and the corpus gate
and the case runner carry special cases for reports with no location
(`UNLOCATED_STOPS`, `_FATAL`).

### A stack or global report does not name the object

```c
__attribute__((noinline)) int idx(int i) { char buf[8]; snprintf(buf, sizeof buf, "%d", i); return buf[i]; }
```

WeaveC reports `index-out-of-bounds at df.c:5:99: index 8`, or, for an
access through a pointer, `stack-buffer-overflow at …` with no second
line. ASan's report says the access lies in the frame of `idx`, which
has one object, `[32, 40) 'buf'`, and that the access overflows it. The
global report has no second line either.

### Faults and assertions

A `memory-fault` report has a program counter and an address; its
backtrace, under `WEAVEC_RT_BACKTRACE=1`, is `backtrace_symbols_fd`'s.
A `library-assertion` stops in the C++ library's inlined code, where the
access's site is the only location printed.

### What it costs to record stacks

On this machine a `malloc` and `free` pair costs 26.5 ns under WeaveC
(17.2 ns with the reference compiler). A prototype that walks 12 frame
records and looks the stack up in an open-addressing store costs 10 ns
per capture with five calls included, so two captures per block add
about 16 ns to the pair: a microbenchmark that only allocates pays about
60%, a program that spends a tenth of its time allocating about 6%.
Emitting line tables (`-gline-tables-only`) for SQLite's amalgamation
costs 5% more compile CPU (10.40 s to 10.94 s) and makes the object 28%
larger (5.97 MB to 7.63 MB); keeping value names, which the frame
descriptors need for the names of locals, another 2–3%.

`llvm-symbolizer` cannot symbolize a Darwin executable linked from object
files without a `.dSYM` (it prints `??:0:0`): the line tables stay in the
object files, and only the debug map in the executable points at them.
`atos`, from Xcode's Command Line Tools, which WeaveC already requires,
reads the debug map and prints `use (in uafsep) (uaf.c:6)`.

### The gates are red

The weekly corpus workflow has failed every run since 2026-10-12. Its
last run, on PR 56's tree:

- **CPython, linux-aarch64 and linux-x86_64**: the suite passes, but the
  report-mode build logs `heap-use-after-free at
  Include/cpython/pyatomic_gcc.h:347:10` (with the two deliberate
  `null-dereference`s of `test_faulthandler`). darwin-arm64 logs no such
  report. It is either a use after free in CPython that only Linux's
  code paths reach, or a pointer the sweep did not read on Linux, which
  would be a false trap in trap mode and a hole in guarantee (T)'s
  premise that the sweep sees every pointer. Which test reaches it is not
  recorded.
- **darwin-arm64, full tier**: libevent's CMake configure fails with the
  reference compiler too (CMake 4 no longer accepts its
  `cmake_minimum_required`), and libuv's suite fails three tests that
  need multicast or a link-local IPv6 address the runner does not have.
- **darwin-arm64, programs tier**: twelve of SQLite's `busy2` and
  `walsetlk` tests fail with the reference compiler too, and two more
  (timing-dependent lock tests) with `weavec-cc` only.
- linux-aarch64 and linux-x86_64 pass the full tier.

v0.20.0's tree (C++) has had no weekly run. RFC 0038 and RFC 0039 stay
Accepted until the weekly run passes on all three platforms; the
Homebrew formula is published with every release, but its tap repository
does not exist, so `brew install weavefoundry/tap/weavec` fails.

### What is not the problem

- **Detection.** No report kind is missing; this RFC changes what a
  report says, not when the program stops.
- **The access's own location.** The site records of RFC 0035 §5.3 give
  every guarded access its file, line and column without any debug
  information, and keep doing so.

## Soundness

### The guarantee

Unchanged: (A), (B), (R), (K), (T), (F), (P) and (U) of RFC 0039 hold as
stated, under the same assumptions. No guard is added or removed, the
shadow encoding is unchanged, and a report traps exactly where it traps
today, after printing more.

### What must not change

- **The sweep.** The stack store holds return addresses (code), and the
  slot side table holds 32-bit identifiers: neither can keep a heap block
  alive. Both live in mappings the runtime makes, which the sweep already
  skips (its own metadata). A frame descriptor is a pointer to constant
  data in the left redzone of a frame, which is not a heap pointer either.
- **Reports cannot crash the reporter.** A report reads memory the program
  may have corrupted (a frame's left redzone, a frame record). The
  unwinder accepts a frame record only inside the thread's stack bounds,
  aligned, and above the previous one; a frame descriptor is accepted only
  if the runtime registered it (§5.3); the symbolizer runs in another
  process. A report that cannot learn something prints less.
- **Reports cannot hang the program.** The symbolizer is spawned with a
  timeout (§4.4); a program the runtime cannot spawn from (a sandbox, a
  set-user-ID program, an exhausted process table) prints unsymbolized
  frames.
- **Report mode stays report-once.** A site prints once (RFC 0035 §5.3);
  its stacks print with it.

### Bugs deliberately not caught

Unchanged from RFC 0039.

### Accepted false traps

None new.

### Assumptions

Beyond RFC 0039's: frame records exist where reports need them. On
AArch64 (Darwin and Linux) and Darwin x86-64, the platform keeps frame
pointers; on Linux x86-64 `weavec-cc` adds `-fno-omit-frame-pointer` to
every compile (§8). Code `weavec-cc` did not compile (the C library, a
prebuilt dependency) may omit them; a stack through such a frame stops
there or skips frames, and says nothing false about the frames it
prints.

## Detailed design

### 0. The gates (option 0)

Before any other stage, on the tree of v0.20.0:

1. **CPython on Linux.** Build CPython 3.13.7 with `weavec-cc` in report
   mode on linux-aarch64 (Docker, as RFC 0038's S7 did) with
   `WEAVEC_RT_REPORT_LOG` and `WEAVEC_RT_BACKTRACE=1`, find the test that
   reaches `pyatomic_gcc.h:347`, and decide:
   - a pointer the sweep did not read: fix the sweep under RFC 0037 §1's
     roots (a bug fix; an amendment here if the root is new), with an
     `rt_test` test that holds the pointer the same way;
   - a use after free in CPython: an entry in `triage.json` with the
     verdict `project-bug`, the test, and the evidence (the release, the
     access, ASan's verdict with `PYTHONMALLOC=malloc` on the same test),
     and a report upstream left to the owner.
2. **The darwin runners.** libevent's configure gets
   `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` in its manifest entry (the
   reference needs it too). libuv's three network tests are excluded on
   `darwin` in the manifest with the reason. SQLite's twelve tests that
   fail with the reference go into its `referenceFailures`; its two
   timing-dependent lock tests that fail only with `weavec-cc` are run
   alone three times: if they pass alone they go into its
   `acceptedFailures` for darwin, naming this RFC and the evidence (they
   time out under the runner's load, not under WeaveC's checks), and if
   they fail alone they are a WeaveC bug, fixed here.
3. **The weekly run** is dispatched on main's tree after 1 and 2 and must
   pass on all three platforms, C and C++ configs, the full and programs
   tiers. RFC 0038 and RFC 0039 then record their linux-x86_64 numbers in
   their *Measured gates* and are marked Implemented.
4. **The tap.** Creating `weavefoundry/homebrew-tap` and its token secret
   is the owner's; the README's Homebrew line is removed until it exists.

No design changes in this section; it is the precondition for measuring
this RFC's gates against a known tree.

### 1. Stacks

The runtime gets one unwinder, `weavecRtStack(frames, max, start)`, that
follows frame records (`[fp] = caller's fp`, `[fp + 8] = return
address`, on both architectures and both platforms) from a given frame
pointer:

- a frame record is accepted only when it is 16-byte aligned (8 on
  x86-64), inside the current thread's stack (its bounds read once per
  thread, the main thread's from the process's limits) or the signal
  stack the fault handler runs on, and above the previous one;
- the return address is stripped of a pointer-authentication signature
  (`xpaclri` on arm64e, a no-op elsewhere) and recorded as the call's
  address (return address minus 1), so that the symbolizer names the
  call's line, not the next one;
- frames in the runtime's code at the start of a stack are dropped: a
  stack starts at the program's frame that called the runtime. The
  runtime's functions are placed in a section of their own
  (`weavec_rt_text`, with the linker's start and stop symbols), so that
  its code is known in the shared library and in a `-static` program,
  where it shares an image with the program.

At most 32 frames are taken for a report's own stack and 16 for a heap
block's (§3). Capture uses no lock, no allocation and no system call.

### 2. The stack store

Unique stacks are kept once, in a store the runtime maps and never frees:

- an open-addressing table of 2^20 32-bit entries, each the offset of a
  stack in a bump area, indexed by the stack's 64-bit hash; an entry is
  claimed with one compare-and-swap, so concurrent captures of one new
  stack store it at most twice (harmlessly) and a lookup of a known stack
  takes no lock;
- a bump area of up to 256 MiB, reserved at the first capture and
  committed a page at a time, holding `{hash, count, frames[count]}`;
- an identifier is the entry's index plus one; 0 means *not recorded*
  (recording off, the store full, a capture in a signal handler that
  interrupted the allocator).

Its memory is excluded from the sweep and from core dumps as the shadow
is (`dumpable`). `WEAVEC_RT_STATS=1` adds a line with its entries and
bytes.

### 3. Heap blocks' stacks

Each slot gets two 32-bit stack identifiers, its allocation's and its
release's, in a side table parallel to the slot words: per region, at
`meta2 + slot * 8`, reserved with the metadata and committed as the
region's slot words are. A huge block keeps its two identifiers in its
table entry.

- An allocation (every form: `malloc`, `calloc`, `realloc`'s new block,
  `aligned_alloc`, `posix_memalign`, `operator new` in all forms)
  captures its stack and stores the allocation identifier when it makes
  the slot live, and clears the release identifier.
- A release (every form, cached or not) captures its stack and stores
  the release identifier when it claims the slot (`claim`), before the
  shadow says released, so that a report of an access to a quarantined
  block always finds it.
- A recycled slot keeps both identifiers until it is allocated again.
- `WEAVEC_RT_HEAP_STACKS=0` turns recording off (both identifiers 0); its
  reports say `(not recorded: WEAVEC_RT_HEAP_STACKS=0)`.

A capture made while the thread is nested in the allocator (RFC 0037
amendment 11: a signal handler that allocates) records 0.

### 4. Symbolization

#### 4.1 The protocol

A report collects its frames as `(module path, offset in module)` pairs
(`dladdr`, or `dl_iterate_phdr` on Linux for an image `dladdr` cannot
name), groups them by module, and asks a symbolizer for each module's
frames, reading back for each frame one or more lines `function file:line:column`
(several for inlined calls, innermost first). A C++ name comes back
demangled.

#### 4.2 Linux: `weavec-cc` is the symbolizer

`weavec-cc --weavec-symbolize` reads `CODE <module> 0x<offset>` lines on
standard input and answers in `llvm-symbolizer`'s output format, using
LLVM's `Symbolize` library (DWARF line tables and inlining, `.gnu_debuglink`
and build-id debug files, demangling), which the toolchain links already
in its static LLVM. The runtime finds `weavec-cc` beside it: the
runtime's image is `<prefix>/lib/weavec/libweavec_rt.so.<abi>` in a
toolchain and in a build tree, so the driver is `<prefix>/bin/weavec-cc`.
`WEAVEC_RT_SYMBOLIZER=<path>` names another executable that speaks the
same protocol (an `llvm-symbolizer`).

The child is started once per process at the first report that needs it
and kept, its pipes marked close-on-exec; after a `fork` the child
process starts its own.

#### 4.3 Darwin: `atos`

`atos -o <module> -l <load address> -i` with the module's addresses on
standard input reads the debug map that links an executable to its
object files' line tables, which `llvm-symbolizer` does not
(*Motivation*). The runtime runs `/usr/bin/atos` once per module per
report and parses its `function (in module) (file:line)` lines; `-i`
prints inlined frames. When `atos` is missing (no Command Line Tools at
run time), or when the module has a `.dSYM` and `atos` failed, the
runtime falls back to `weavec-cc --weavec-symbolize` as on Linux.

#### 4.4 Limits and fallbacks

The runtime spawns with `posix_spawn` (no allocation in the child),
passes the process's environment, and gives a symbolizer two seconds for
a report's frames (`poll` on its output). It does not spawn in a
set-user-ID or set-group-ID program, under `WEAVEC_RT_SYMBOLIZE=0`, or
when the spawn fails; then, and for any frame the symbolizer answers
`??`, the frame prints as its function from `dladdr` and its module and
offset:

```
    #1 0x1021a4610 in use+0x38 (uaf+0x4610)
```

`weavec-cc --weavec-symbolize <log>` rewrites such frames in a saved
report (a report log, a CI artifact) on another machine with the same
binaries.

#### 4.5 Line tables

Today an enforcing compile without `-g` keeps locations only inside the
compiler (`LocTrackingOnly`), so the site records have them and the
object file has none. It emits line tables instead
(`DebugLineTablesOnly`, with inlining information and columns), so that
every frame of a report carries a file and line, not only the access's.
A command line with any `-g` keeps its level; one that ends in `-g0`
keeps today's behavior (no debug sections; stacks name functions only).
Build cost and object size are G7's.

### 5. Reports

#### 5.1 The common shape

Every report is a first line, the access's stack, zero or more object
paragraphs, and a summary line:

```
weavec: <kind> at <file>:<line>:<col>: <what>
    #0 <pc> in <function> <file>:<line>:<col>
    #1 <pc> in <function> <file>:<line>:<col>
    ...
weavec: <object paragraph's first line>
    #0 ...
weavec: summary: <kind> at <file>:<line>:<col> in <function> (thread T<n>)
```

- **The first line is unchanged** for every access kind (RFC 0035 §5.3,
  RFC 0039): the corpus gate's triage keys stay valid.
- **The access's stack** is printed for every report, in every mode: it
  starts at the frame of the failing access (the guard's caller, the
  faulting instruction, the C++ library's trap) and has at most 32
  frames.
- **Frames** are indented four spaces and numbered from `#0`; inlined
  frames repeat their number with the inlined function, as ASan's do.
- **Threads.** The runtime numbers threads in the order it first sees
  them (the main thread is `T0`) and prints a thread's name when it has
  one (`T3 'worker'`).
- **The summary line** repeats the kind and the location and names the
  innermost function and the thread, for a reader that greps logs.

#### 5.2 Heap

The second line of RFC 0035 §5.3 becomes a paragraph per stack:

```
weavec: heap-use-after-free at uaf.c:6:56: read of 1 bytes at 0xbc80000000
    #0 0x1021a45dc in use uaf.c:6:56
    #1 0x1021a4614 in main uaf.c:7:44
weavec: 0xbc80000000 is 0 bytes inside the 32-byte heap block at 0xbc80000000, released by thread T0:
    #0 0x1021a4560 in drop uaf.c:5:42
    #1 0x1021a460c in main uaf.c:7:37
weavec: the block was allocated by thread T0:
    #0 0x1021a4520 in make uaf.c:4:48
    #1 0x1021a4604 in main uaf.c:7:28
weavec: summary: heap-use-after-free at uaf.c:6:56 in use (thread T0)
```

A live block's paragraph says `is <n> bytes after the <size>-byte heap
object at <base>, allocated by thread T<n>:` and its allocation stack; a
color mismatch whose access lands in a heap object names that object and
its allocation stack after RFC 0037's `in another object` line; a
`container-overflow` keeps RFC 0039's line and adds the allocation stack.

#### 5.3 Stack frames

A tracked frame stores two words at the start of its left redzone at
entry (RFC 0035 §3: the left redzone is 64 bytes, its shadow 0xF1): the
constant `0x5745415645430000 | ABI` and the address of a frame
descriptor, a constant the unit emits:

```
struct __weavec_rt_frame {
  int64_t function;   /* self-relative: the function's name */
  uint32_t objects;   /* how many */
  uint32_t reserved;
  struct { uint32_t offset, size; int64_t name; } object[];  /* name self-relative */
};
```

with each tracked local's offset in the frame, size and name. The names
are the allocas' names, which the compile keeps (`-fno-discard-value-names`
in enforcing modes, §8): Clang names them after their declarations (a
parameter's copy `x.addr`, printed as `x`). The unit registers its
descriptors with its globals (§5.4), so the runtime accepts a descriptor
it finds in a frame only if it is one it registered.

For an access whose shadow is a stack value (0xF1–0xF5, 0xF8) or a
local's color, the runtime scans the shadow down from the access to the
lowest granule of the first run of 0xF1 below it, reads the two words
there, and, when the descriptor is registered, prints:

```
weavec: stack-buffer-overflow at df.c:5:99: write of 1 bytes at 0x16f16a2e8
    #0 0x100c94954 in idx df.c:5:99
    #1 0x100c94a20 in main df.c:6:62
weavec: 0x16f16a2e8 is 0 bytes after 'buf' (8 bytes at offset 64) in the frame of idx (thread T0), whose locals are:
    [64, 72) 'buf'
weavec: summary: stack-buffer-overflow at df.c:5:99 in idx (thread T0)
```

(`… is <n> bytes before 'x'`, `… is inside 'x', whose scope has ended`,
`… is inside 'x' of a frame of f that returned`.) A dynamic object
(`alloca`, a variable-length array) is described as such. An address
with no registered descriptor below it, or one whose frame a non-local
exit cleared, gets no paragraph.

The two stores cost one paired store per tracked frame's entry, which
already writes the frame's shadow.

#### 5.4 Globals

RFC 0035 §4's global record gains the global's name and the location of
its definition (self-relative, 0 when the unit has neither):

```
struct __weavec_rt_global {
  uintptr_t address;
  uint64_t size;
  uint64_t padded;
  int64_t name;        /* self-relative */
  int64_t location;    /* self-relative "<file>:<line>", or 0 */
};
```

A unit's frame descriptors are registered by the same constructor
(`__weavec_rt_globals_register(globals, n, frames, m)`). The runtime keeps
both sorted. A `global-buffer-overflow`, and a color mismatch that lands
in a global, print:

```
weavec: 0x100004010 is 0 bytes after the 16-byte global 'g' defined at df.c:4
```

The definition's location comes from the global's debug information when
the unit has it and from its first use's site otherwise; a static local's
name is its function's and its own (`idx.table`), demangled for C++.

#### 5.5 Releases

A bad release gets a kind of its own, a location, the release's stack
and the block's:

| Kind | Release | Was |
| --- | --- | --- |
| `double-free` | of a block already released (in a cache, the quarantine or the free list) | `invalid release of …: the block was already released` |
| `invalid-free` | of an address that is not the start of a live block: inside a block, a never-allocated slot, a stack or global address, a pointer into untracked memory that the C library's allocator does not own | `not the start of its block`, `not the start of a live block`, `no block is allocated there`, `not a heap block` |
| `alloc-release-mismatch` | by another form than the allocation's, or a sized `delete` of another size (RFC 0039 §4) | `allocated by … and released by …`, `released by … with size …` |

```
weavec: double-free at df.c:3:40: free of 0xbd80000000
    #0 0x100c94600 in drop df.c:3:40
    #1 0x100c94a10 in main df.c:6:55
weavec: 0xbd80000000 is the 8-byte heap block at 0xbd80000000, released by thread T0:
    #0 0x100c94600 in drop df.c:3:40
    #1 0x100c94a04 in main df.c:6:47
weavec: the block was allocated by thread T0:
    #0 0x100c949f0 in main df.c:6:30
weavec: summary: double-free at df.c:3:40 in drop (thread T0)
```

The location: a direct call of a release function in a guarded unit
passes its site. The library table gets rows for `free`, `realloc`,
`reallocf`, `reallocarray` and the replaceable `operator delete` forms
(mangled, as RFC 0039 §7's rows are) with a `wrapper` clause, so that
GuardLibrary redirects such a call to `__weavec_rt_<name>_at(…, site)`
(`wrapper(free_at)` and so on), which releases as the plain function does
and hands the allocator the site. A release through a function pointer, from unguarded code or from
the C library reaches the plain entry points: its location is its stack's
first frame, when the symbolizer gives one, else `<unknown>`. The
allocator still stops every bad release in every mode (RFC 0035 §5.3:
it cannot go on from one), and `__weavec_rt_fatal` is deleted with the
`invalid release of` format.

#### 5.6 Faults and assertions

`memory-fault` keeps its first line (RFC 0037 §6) and gets the stack
unwound from the signal context's frame pointer, with the faulting
instruction as frame `#0`, and, for a heap address, the heap paragraph.
`library-assertion` (RFC 0039 §6) gets the access's stack. The
`null-dereference` reports get theirs.

#### 5.7 Report mode, logs and the report's own budget

Report mode prints a site once with its stacks (RFC 0035 §5.3). A report
is formatted into a 16 KiB static buffer per thread (in the runtime's
thread state) and emitted in one write per line, as today; a report
larger than the buffer is cut at a frame boundary with `    ...`.
`WEAVEC_RT_REPORT_LOG=<path>` appends the whole report.

### 6. JSON

`WEAVEC_RT_REPORT_FORMAT=json` writes each report as one line of JSON
instead of text (to the log, or stderr):

```json
{"weavec":40,"kind":"heap-use-after-free","proven":false,
 "site":{"file":"uaf.c","line":6,"column":56},
 "access":{"op":"read","width":1,"address":"0xbc80000000"},
 "thread":{"id":0,"name":null},
 "stack":[{"pc":"0x1021a45dc","module":"/…/uaf","offset":"0x45dc",
           "function":"use","file":"uaf.c","line":6,"column":56,"inlined":false}],
 "object":{"memory":"heap","base":"0xbc80000000","size":32,"offset":0,
           "state":"released","released":[…],"allocated":[…]}}
```

`object.memory` is `heap`, `stack` (with `function`, `name`, `locals`),
`global` (with `name`, `location`) or absent. A release report's `site`
is its location and `access` is `{"op":"free"|…}`. The schema's fields
are documented in the command-line reference; the version is the
runtime's ABI.

`scripts/run-cases.py` and `scripts/corpus-gate.py` set
`WEAVEC_RT_REPORT_FORMAT=json` and a `WEAVEC_RT_REPORT_LOG` per run, and
read every report from the log: reports a test harness swallows reach
them too. Their regular expressions over report text, the special cases
for reports with no location (`_FATAL`, `INVALID_RELEASE_RE`,
`UNLOCATED_STOPS`) and the per-kind second-line parsing are deleted;
they still read a run's output for the program's own failures and trap
signs.

### 7. Untracked memory

`WEAVEC_RT_STATS=1` adds a line with the bytes of writable private
mappings, at exit, that the runtime did not make and that are not a
thread's stack, measured with the sweep's mapping walk:

```
weavec: runtime: 480 MiB of writable memory the runtime does not track (mappings the program or its libraries made: objects an allocator of their own carves from them are not checked)
```

It is the evidence a developer needs to suspect a program's own allocator
(RFC 0039 *Future work*); this RFC does not track such objects.

### 8. The driver

In the `trap`, `report` and `verify` modes, `configureEnforcement`:

- emits line tables (§4.5) unless debug information is already on or the
  command line ends in `-g0`;
- keeps value names (`CodeGenOptions::DiscardValueNames = false`) for the
  frame descriptors (§5.3);
- on x86-64 Linux, keeps frame pointers (`-fno-omit-frame-pointer`)
  unless the command line says `-fomit-frame-pointer` last.

`weavec-cc --weavec-symbolize [<log>]` is the symbolizer (§4.2) and the
log rewriter (§4.4); `--help-weavec` lists it. The ledger is unchanged.

### 9. The runtime's interface

`WEAVEC_RT_ABI` becomes 40: the global records and their registration
(§5.4), the frame descriptors (§5.3), the release wrappers (§5.5). New
exports: `__weavec_rt_free_at`, `__weavec_rt_realloc_at`,
`__weavec_rt_reallocf_at`, `__weavec_rt_reallocarray_at` and the
`operator delete` forms' `_at` wrappers, each taking the plain function's
arguments and a site; `__weavec_rt_globals_register` takes the frame
descriptors too. The line
budgets of AGENTS.md rule 9 become 9,000 for the runtime and 9,500 for
the library code; the symbolizer's client, the unwinder, the stack store
and the report formatting are expected to add about 1,100 runtime lines,
the symbolizer mode, the descriptors and the release rows about 400
library lines.

### 10. Deletions

- `WEAVEC_RT_BACKTRACE`, `printBacktrace`, the runtime's uses of
  `backtrace` and `backtrace_symbols_fd` (and `<execinfo.h>`).
- The `invalid release of <address>: <why>` format, `__weavec_rt_fatal`
  and `weavecRtFatalRelease` (the allocator reports through §5.5).
- The kind `invalid-release` (replaced by §5.5's three), in the scripts'
  lists of kinds, the triage file and the documentation.
- The scripts' report-text parsing (§6).
- The README's Homebrew line until the tap exists (§0).

### 11. Tests

- `test/Guards/`: one test per report shape pinning its lines with
  FileCheck patterns for addresses (`report-heap.c`, `report-stack.c`,
  `report-global.c`, `report-release.c`, `report-fault.c`,
  `report-json.c`, `report-cxx.cpp` for demangled frames, inlined frames
  and `alloc-release-mismatch`); every existing test that pins a second
  line is updated.
- `test/Driver/`: line tables by default and not under `-g0`; value
  names; the frame-pointer flag on x86-64 Linux;
  `--weavec-symbolize` on a known object.
- `runtime/test/rt_test.c`: the unwinder's bounds (a corrupt frame record
  stops it), the store under eight threads, a full store returning 0,
  identifiers kept across quarantine and recycling, a report from a
  release while an allocator lock is held, a report from a signal
  handler, a symbolizer that never answers (the two-second limit), a
  symbolizer that is missing.
- `unittests/Core/LibrarySpecTest.cpp`: the release rows.
- `test/cases/`: the case runner reads JSON; every case keeps its verdict.

### 12. Documentation

The command-line reference (kinds, the report's shape, the JSON schema,
the variables), a *Reading a report* section in troubleshooting, the
README's examples, the getting-started first check, architecture,
AGENTS.md (the table's *Debug what a guarded program did* row and rule 9's
budgets), development, roadmap.

## Annotation surface

None.

## Diagnostics

Every report gains its stack and summary line (§5.1). Changed second
lines: the heap's (§5.2), new paragraphs for stack (§5.3) and global
(§5.4) objects. New report kinds:

```
weavec: double-free at <file>:<line>:<col>: <form> of <address>
weavec: invalid-free at <file>:<line>:<col>: <form> of <address>: <why>
weavec: alloc-release-mismatch at <file>:<line>:<col>: <form> of <address>: allocated by <form>
weavec: alloc-release-mismatch at <file>:<line>:<col>: <form> of <address>: size <n> for a <m>-byte block
```

where the release form is `free`, `realloc`, `operator delete` or
`operator delete[]`, the allocation form `malloc`, `operator new` or
`operator new[]`, and `<why>` one of `inside a <n>-byte heap block at
<base>`, `no block was allocated there`, `not heap memory`.

```c
char *p = malloc(8); free(p); free(p);               /* double-free */
char *q = malloc(8); free(q + 1);                    /* invalid-free */
```
```cpp
int *a = new int[4]; delete a;                       // alloc-release-mismatch
```

Removed kind: `invalid-release`. New variables:
`WEAVEC_RT_HEAP_STACKS=0`, `WEAVEC_RT_SYMBOLIZE=0`,
`WEAVEC_RT_SYMBOLIZER=<path>`, `WEAVEC_RT_REPORT_FORMAT=json`. Removed:
`WEAVEC_RT_BACKTRACE`. New driver mode: `--weavec-symbolize`. Each kind
gets a `test/Guards/` test pinning its lines, an entry in the
command-line reference's list of kinds, and an entry in the scripts'
lists of kinds.

## Implementation plan

The stages land on one branch and ship as one change:

- **S0 The gates** (§0): the CPython report settled, the runner fixes,
  the weekly run green on all three platforms, RFC 0038 and RFC 0039
  Implemented (their status flips can land on main on their own, as fix
  PRs, if the owner prefers); the baselines of this RFC's cost gates on
  v0.20.0.
- **S1 Stacks and the store** (§1, §2), with their `rt_test` tests.
- **S2 Heap stacks** (§3), and their cost measured against G5 before
  anything else is built on them.
- **S3 Symbolization** (§4): `--weavec-symbolize`, `atos`, the fallbacks,
  line tables.
- **S4 Reports** (§5): the common shape, heap, frames and their
  descriptors, globals, releases and their rows, faults.
- **S5 JSON and the scripts** (§6), untracked memory (§7), the deletions
  (§10).
- **S6 Fresh iterations**: the gates on all three platforms, the
  documentation, this RFC's *Measured gates*.

## Acceptance gates

Binaries are the `release` preset's on the final tree; v0.20.0 is built
from its tag on the same machine; darwin-arm64 is the assessment's
machine, linux-x86_64 and linux-aarch64 are GitHub's runners unless
named.

**Gates green**

- **G1. Weekly corpus.** The weekly corpus workflow passes on all three
  platforms, full and programs tiers, C and C++ configs, on S0's tree and
  again on the final tree; the CPython report is fixed or triaged with
  evidence.

**Reports**

- **G2. Heap.** In a program compiled with `-O2` and no `-g` on all three
  platforms, every heap use after free, heap overflow and double free of
  the case suites prints the release stack (where there is one) and the
  allocation stack, each frame with a file and line for frames
  `weavec-cc` compiled; C++ frames demangled.
- **G3. Stack and globals.** Every stack overflow, use after scope and
  use after return of the case suites names its local and function, and
  every global overflow its global, on all three platforms.
- **G4. Releases.** Every bad release of the case suites and of the
  corpus injections has a source location and its kind; the corpus gate
  has no special case for a report without one.
- **G5. Faults and fallbacks.** A fault prints a symbolized stack; with
  the toolchain moved away (no symbolizer), with `WEAVEC_RT_SYMBOLIZE=0`,
  from a set-user-ID program, and with a symbolizer that never answers,
  a report prints module-and-offset frames within three seconds and the
  program stops as it does today.

**Detection**

- **G6. No change.** RFC 0039's G4 and G5 hold: the case suites pass with
  the same verdicts read from JSON; the C++ probes, 114 of 117 detection
  probes, 15 of 20 jump probes, 7 of 10 use-after-return probes and 33 of
  39 injections stop; verify mode reports no `weavec.proven`.

**Cost**

- **G7. Run.** darwin-arm64 and linux-x86_64: the corpus benchmarks' (C
  and C++) geometric mean at most 1.03 times v0.20.0's CPU time, none
  above 1.10; peak memory at most 1.10 times v0.20.0's; with
  `WEAVEC_RT_HEAP_STACKS=0`, at most 1.01 times. linux-x86_64's frame
  pointers are part of this cost.
- **G8. Build.** Compile CPU of the corpus configs at most 1.08 times
  v0.20.0's (geometric mean), none above 1.15; object and executable
  sizes recorded, not limited.

**Releases**

- **G9. Toolchains.** RFC 0038's G8 holds, and each archive's example
  program, built without `-g` on its clean runner, prints a symbolized
  report for a planted use after free.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes with the budgets of §9; the
  export list matches the header.
- **H2.** The documentation describes the change (§12); `npm test && npm
  run build` in `docs/`; the documentation examples' checker passes; this
  RFC is marked Implemented.

## Implementation amendments

These record where the implementation departs from the design above. They
override the body.

1. **The CPython report is CPython's (§0.1).** On v0.20.0 in Docker
   (linux-aarch64), every test file run alone and three full `-j4` runs on
   four CPUs reported only the `null-dereference`s of `test_capi` and
   `test_faulthandler` that crash on purpose; a full `-j8` run on three
   CPUs reproduced the `heap-use-after-free at pyatomic_gcc.h:347`, and so
   did the first such run with this RFC's runtime, whose stacks name it: a
   daemon thread in `os.write` → `PyEval_SaveThread` → `drop_gil` reads
   `tstate->eval_breaker` after `drop_gil_impl` has unlocked the GIL's
   mutex (CPython's own comment there names the race, python/cpython issue
   104341), and the main thread, finalizing after `SystemExit`, freed that
   thread state in `_PyThreadState_DeleteList` in between. The report is
   triaged `project-bug` with that evidence; the ASan run §0.1 asks for was
   not made (it would need the same timing), and the report upstream is
   the owner's. The sweep is not at fault: the block was released.
2. **The darwin runners (§0.2).** Besides CMake's policy flag, libevent's
   `evutil.c` failed to compile on the runner: the support header, made on
   the macOS 27 SDK, said `pipe2` exists, and the runner's macOS 15 SDK
   does not declare it; the header now asks the SDK. The gate's compile
   failure names the first `error:` line, not the last line of the output.
   SQLite's two timed lock tests passed alone in three runs of three with
   `weavec-cc`: `acceptedFailures` for darwin.
3. **Thread numbers on Darwin (§5.1).** A thread's number is
   thread-specific data, not a thread-local variable, and the unwinder
   reads the thread's stack bounds from its thread structure: dyld creates a
   thread's variables with `calloc`, whose capture asked for them again and
   recursed until the stack ran out. A thread's name is printed only for the
   reporting thread, other than T0 (the main thread's is the program's).
4. **Release stacks live in the released block (§3).** The side table holds
   only the allocation identifier (4 bytes per slot, beside the slot
   words), so that a 16-byte slot's metadata grows by a quarter, not by
   half. A release writes its identifier and the block's requested size,
   each with its complement, into the block's first 16 bytes before the
   shadow says released (a large slot keeps its first page when it gives
   the rest back); a report reads them back when the complements match, so
   a write the program made in report mode leaves "not recorded", not a
   wrong stack. A released block's paragraph names its requested size.
5. **Releases through the program's own allocator (§5.5).** A guarded
   `free` goes to `__weavec_rt_free_at`, which calls the program's `free`
   when the program defines one (Linux: `dlsym(RTLD_DEFAULT)` finds it in
   another image than the runtime's and the C library's; Darwin:
   `RTLD_MAIN_ONLY`), so that a program with an allocator of its own keeps
   it. The rows exist for `free`, `realloc`, `reallocf`, `reallocarray`
   and the four unaligned `operator delete` forms; the aligned and nothrow
   forms release without a site (their location is their stack's first
   frame). The `_at` wrappers free memory: the guard-removal rules treat
   a call of one (and of `__weavec_rt_munmap`, which they did not) as a
   release.
6. **The frame descriptor's `reserved` field is the frame's size
   (§5.3).** The runtime looks for the descriptor at every 32-byte boundary
   below the address whose shadow is a left redzone or a returned frame,
   and accepts the first whose frame reaches the address. A use after
   return rarely finds its frame: the report's own frames overwrite the
   returned frame's first words below the stack pointer. It then prints
   `<address> is in a frame that returned`, as before; G3's use-after-return
   part is not met.
7. **Names are offsets from their records (§5.3, §5.4).** A global's name
   and location, and a local's name, are offsets from the record that holds
   them; a frame's function name from its descriptor. A global's location is
   its debug information's `file:line` under `-g`, else its unit's file
   (the first use's site was dropped: "defined at" would name a use).
8. **The report's shape (§5.1, §5.7).** A report is formatted in a 64 KiB
   buffer under a lock and written in one write (a JSON line is a write); a
   JSON stack stops adding frames before the buffer is too full to end the
   line. Clang's `__clang_trap_msg` frames are dropped. A release report's
   first line keeps the location without a column when only the symbolizer
   gives it on Darwin (`atos` names lines, not columns).
9. **One-step builds on Darwin.** The driver adds line tables as
   `-gline-tables-only` to the command line, so that Clang's driver runs
   `dsymutil` after a link that compiled its sources in the same step, as
   it does for `-g`: the objects of such a link are temporary, and `atos`
   reads the `.dSYM` instead.
10. **The runtime's line budget (§9).** The unwinder, the store, the
    symbolizer's client, the report formats, JSON and the untracked-memory
    walk came to about 2,100 lines, not 1,100: rule 9's runtime budget is
    9,800.

11. **Heap stacks keep 8 frames, and capture is lean (§1, §3; G7).**
    A capture at every allocation and release cost cJSON's benchmark 1.43
    times v0.20.0's CPU with 16 frames: it makes 21.6 million allocations,
    so 43 million captures, and the walk is a chain of dependent loads. The
    capture now starts at the allocator's own frame, reads the thread's
    stack bounds and number from its allocator cache (not libpthread, not a
    thread-local variable), hashes the frames with independent products,
    and looks a stack up first in a 4,096-entry cache of recent stacks
    shared by the threads (a stack's hash's high 44 bits and its index in
    one word, so that a read is never torn) before the store: about 4.5 ns
    and 1 ns a frame. A heap stack keeps 8 frames by default;
    `WEAVEC_RT_HEAP_STACKS=<n>` keeps `n` (at most 32; 0 records none). A
    report's own stack keeps 32. G7 is not met with stacks on (*Measured
    gates*); it is met with `WEAVEC_RT_HEAP_STACKS=0`. On 2026-11-09 the
    project owner chose to keep heap stacks on by default and accept that
    cost: G7's limits apply to `WEAVEC_RT_HEAP_STACKS=0`, and with stacks
    on the run cost is recorded, not limited.
12. **A release's redirection is no ledger row (§5.5).** The `_at` wrappers
    check no access, so redirecting a release records nothing in the
    ledger: counted as `checked-call` rows they lowered the corpus's
    `provenShare` ratchet with no change in what is proven.

## Measured gates

Measured on 2026-11-08, darwin-arm64 (the assessment's machine) and
linux-aarch64 (Docker on it); linux-x86_64 and the weekly corpus run are
CI's, after this change lands.

- **G1.** Not yet: the darwin runners' failures are fixed (amendment 2)
  and the CPython report is triaged (amendment 1); the weekly run on this
  tree is pending, so RFC 0038 and RFC 0039 stay Accepted.
- **G2.** Heap reports carry release and allocation stacks with files and
  lines from `-O2` builds without `-g`, C++ frames demangled
  (`test/Guards/report-heap.c`, `report-release.c`, `report-cxx.cpp`,
  `report-json.c`), on darwin-arm64 and linux-aarch64.
- **G3.** Stack overflows and uses after scope name the local and its
  function (`report-stack.c`), global overflows the global
  (`report-global.c`); a use after return mostly does not (amendment 6).
- **G4.** Every bad release has a kind and a location; the case suites'
  release markers and the ten corpus injections that relied on a stop with
  no location now stop at their release's line (three at the project's
  release helper, named by `stop`), and the gate has no special case left.
- **G5.** A fault prints a symbolized stack; with `WEAVEC_RT_SYMBOLIZE=0`,
  a missing symbolizer and one that never answers, frames name module and
  offset and the report ends within the two seconds (`report-fault.c`).
  The set-user-ID path and a moved toolchain were not run.
- **G6.** The case suites pass with their verdicts read from JSON on
  darwin-arm64 and linux-aarch64; the 52 detection probes of the
  2026-11-01 evaluation stop 27 times, with the same kinds, as v0.20.0.
  The full injection and verify-mode runs are CI's.
- **G7.** User CPU against v0.20.0, best of five, the corpus benchmarks
  whose sources this machine has: with stacks (8 frames) cJSON 1.22, zlib
  0.99, Lua 1.03 (geometric mean 1.08, over the 1.03 and 1.10 limits); with
  `WEAVEC_RT_HEAP_STACKS=0` 1.01, 0.99, 1.01 (1.00). Peak memory 1.07,
  1.19 (3.8 MiB against 3.2) and 1.07. Met with stacks off; with stacks
  on, the default, the cost is accepted by the owner (amendment 11).
- **G8.** Compile CPU of the corpus's quick tier against v0.20.0 (line
  tables and value names included): geometric mean 1.04, at most 1.13
  (jansson).
- **G9.** Not run: the release toolchains are built by the release
  workflow.
- **H1.** `scripts/check-hygiene.py` passes with the budgets of amendment
  10 (runtime 9,800; library 9,500).
- **H2.** The documentation describes the change; `npm test` and the
  documentation examples' checker pass.

## Drawbacks

- **Allocation pays for reports it may never print.** Two stack captures
  per block are the bulk of G7's budget; allocation-heavy interpreters
  pay the most. `WEAVEC_RT_HEAP_STACKS=0` is the escape hatch, at the
  price of reports without the block's history.
- **Line tables by default** make objects about 30% larger and builds a
  few percent slower, for every user, to make reports better for the
  ones who hit a bug. `-g0` opts out.
- **A report spawns a process.** That is new behavior for a trapping
  program (a symbolizer may be slow, or forbidden by a sandbox). It is
  bounded (§4.4) and ASan has done the same for a decade.
- **Two symbolizers.** Darwin's `atos` and LLVM's library differ in what
  they print for inlined frames and in their failure modes; the runtime
  normalizes both, and its tests run both.
- **Value names** cost compile time and memory in every enforcing build
  for the names of locals.
- **More runtime.** About 1,100 lines, and a larger interface (ABI 40).

## Alternatives

- **Do nothing; tell users to reproduce under ASan.** Every bug WeaveC
  finds costs a second build with another tool, and bugs that depend on
  WeaveC's layout or timing (its quarantine reuses memory later than
  ASan's) may not reproduce.
- **Option B: programs' own allocators** (Valgrind's client requests,
  routing a program's `malloc` replacement to the runtime, documented
  switches). Most of it helps only when a project's build already turns
  on its own sanitizer or Valgrind support, and no generic mechanism finds
  the objects of a pool that does not describe them. Its first step, the
  measurement of untracked memory, is §7.
- **Option C: deterministic spatial checks** (pointers checked where they
  leave a function, exact heap bounds from the slots). It closes 5 of the
  20 jump probes' misses; RFC 0037 measured escape checks at 7.8% more
  cycles, and exact heap bounds need power-of-two slots. A later RFC can
  take it on its own.
- **Option D: Windows.** The largest reach, 15–30K lines, with no Windows
  machine to iterate on; its allocator replacement needs ASan's hot
  patching of the C runtime.
- **Option E: deterministic use after return** (fake stacks or arena
  frames): measured at 1.35–1.49 times in RFC 0037.
- **Allocation stacks only, or sampled.** GWP-ASan and HWASan's sampling
  keep cost down by recording few blocks. A use after free of an
  unrecorded block would print no history, which is the case this RFC
  exists for. Recording only the allocation stack saves half the cost
  and loses the `free` that a use after free most needs.
- **One frame (the caller of `malloc`) per stack.** Nearly free, and
  useless behind the wrappers every C project has (`xmalloc`,
  `zmalloc`, `sdsnewlen`).
- **Unwinding with the platform's unwinder** (`_Unwind_Backtrace`,
  `backtrace`): correct through frames without frame pointers, 10–50
  times slower per capture, and it takes locks the allocator may already
  hold.
- **Symbolizing in the runtime** (parsing DWARF in the process): several
  thousand lines of C in the runtime, a parser running in a corrupted
  process, and no reading of Darwin's debug maps.
- **Only `llvm-symbolizer`, shipped in the toolchain.** A second LLVM
  executable in every archive, and it cannot read Darwin's debug maps.
- **Frame descriptors from debug information only.** No names in release
  builds, which are what drop-in users build; with value names kept,
  names come with no `-g`.
- **ASan's shadow dump** (the bytes around the address). Useful to
  WeaveC's developers, noise to its users; the JSON report carries what
  tools need.

## Prior art

- **AddressSanitizer** records a compressed allocation and release stack
  per chunk in its header and a stack depot (`sanitizer_stackdepot.cpp`),
  unwinds with frame pointers by default (`fast_unwind_on_malloc`),
  stores a frame description string and the function's address in each
  instrumented frame (`kCurrentStackFrameMagic`,
  `asan_globals.cpp`'s `__asan_global` with names and source locations),
  and symbolizes out of process (`llvm-symbolizer`, `atos` on Darwin). Its
  report shape (access stack, freed by, previously allocated by,
  `SUMMARY:`) is the one developers know; this RFC takes it, without the
  shadow dump.
- **Valgrind's memcheck** prints the same three stacks, from its own
  unwinder.
- **HWASan and GWP-ASan** keep a ring buffer or sample of stacks to keep
  their cost low, at the price of reports without history.
- **Scudo** records allocation and release stacks only on request, which
  is the switch `WEAVEC_RT_HEAP_STACKS` gives the other way round.

## Unresolved questions

- **The CPython report** (§0): a WeaveC bug or CPython's. The answer
  decides whether S0 is a fix or a triage.
- **Frame pointers on x86-64 Linux** (§8): their cost on the corpus
  benchmarks is measured in S2; if they cost more than 2%, the unwinder
  falls back to an `.eh_frame` unwinder for reports only and the heap's
  stacks stop at the first frame without a record.
- **Stack depth.** 16 frames for a block, 32 for an access, is ASan's
  practice halved; S2 measures what 8 and 32 cost.
- **Inlined frames** from line tables only (`-gline-tables-only` emits
  inlining information): to be checked on both symbolizers in S3.
- **Static linking.** A `-static` Linux program's report finds
  `weavec-cc` through the toolchain path compiled into the archive, or
  `WEAVEC_RT_SYMBOLIZER`; S3 decides.

## Future work

- Programs' own allocators (option B), with §7's measurement as the
  evidence of where they matter.
- Deterministic spatial checks (option C).
- Thread creation stacks (`created by thread T0 at …`), which need the
  runtime to stand in front of `pthread_create`.
- Windows.
