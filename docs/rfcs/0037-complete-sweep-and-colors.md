# RFC 0037: A complete sweep and object colors

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-02
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes RFC 0035 §2.2's shadow
  encoding and §3.2's clearing of a frame at return; RFC 0036 §3.3's sweep
  when a class's region is exhausted and its 16 MiB default, §3.4's roots,
  and its *Future work* entry for spatial determinism and use after return;
  the future work of RFC 0036 amendment 9 (field sizes from the address).
  RFC 0035's guards, removal rules, frames, globals, modes and ledger, and
  RFC 0036's sweep-before-reuse, fortified sizes and ASan contract stay,
  amended below.

This RFC was drafted before implementation. On 2026-11-02 the project
owner accepted the recommendation of a milestone assessment of v0.17.0:
make the guarantees RFC 0036 states hold, and make the runtime fit for
threaded servers (option A), and catch the jumps over a redzone and the
uses after return that the README leaves to this RFC (option B), in one
large change, with breaking changes and no compatibility layers. A third
option, cutting the guards' cost, was measured and is not taken
(*Alternatives*).

## Summary

RFC 0036 reuses a released heap block only after a sweep finds no pointer
to it. Measured on v0.17.0, that guarantee does not hold for pointers the
sweep does not read (a vector register, a coroutine's stack, a file
mapping after `msync`, read-only memory, the main thread's thread-specific
data), and a loop the guard pass versions can overrun its buffer without a
report. 32 of 84 library calls that ASan stops pass. The allocator slows
down as threads are added, a size class that fills its region sweeps on
every allocation, and a sweep over a pointer-dense gigabyte stops the
program for a third of a second.

This RFC:

1. **Completes the sweep's roots** (§1): the stopped threads' vector
   registers and thread-specific data, every stack in a mapping, file
   mappings the program can write, memory it made read-only, and a thread
   list compared by identity. Signals are blocked while a sweep runs.
2. **Makes the allocator scale** (§2): per-thread caches, shared counters
   on their own cache lines, further regions for a full class, and a
   quarantine of 64 MiB by default.
3. **Marks in parallel** (§3): while the world is stopped, worker threads
   divide the roots and the live heap between them.
4. **Closes the guards' holes** (§4): loop ranges that overflow, top-byte
   tags, the generic atomic calls, dynamic objects whose shadow cost grew
   with their size, and frames whose compile-time cost did.
5. **Guards the library calls ASan guards** (§5): wrappers for calls whose
   bytes depend on a C type or a structure, wide strings, formats that are
   not constants, and library functions called through a pointer.
6. **Reports faults** (§6): a `SIGSEGV` or `SIGBUS` in the program prints a
   `memory-fault` report.
7. **Colors objects** (§7, §8): each tracked object's shadow carries one
   of 111 colors; an access computed from a pointer traps when its bytes
   lie in an object of another color. A jump over a redzone into a live
   neighbor, which passes today and under ASan, traps.
8. **Poisons returned frames** (§9): a frame's shadow says *returned* after
   it returns, so a use after return traps until another frame of WeaveC's
   code reuses its bytes.
9. **Limits string calls to their field** (§10): a `strcpy` or `sprintf`
   into an array field is checked against the field, whatever the C
   library's headers pass.
10. **Shrinks the site records** (§11) to read-only data without
    relocations.

The corpus benchmarks are expected to run at about 1.70–1.75 times the
reference compiler (v0.17.0: 1.63).

## Motivation

Measured on main at `1f12e5cd` (v0.17.0), release build, darwin-arm64
(Apple M3, 8 cores, 24 GiB), with the reference `clang` 23.1.0 and ASan
(`-O2 -fsanitize=address`). Probes are small C programs, each built and
run under a cap on memory and time.

### Silent reuse

A use after free traps only while no sweep has recycled the block. A sweep
recycles a block when it finds no word pointing to it, so a pointer the
sweep does not read lets a stale access reach the block's next owner. Each
probe frees a block while it keeps the only pointer in one place, allocates
until a sweep runs, and uses the pointer:

| Where the only pointer is | Rounds with silent reuse | ASan | Cause |
| --- | --- | --- | --- |
| A NEON register of another thread | 490 of 513 | stops | only `ARM_THREAD_STATE64` is read (`runtime/weavec_sweep.c:230`) |
| A file mapping after `msync` | 489 of 512 | stops | clean pages of a file are skipped (`worthScanning`, `:351`) |
| A stack below another thread's stack pointer in a shared mapping (coroutines, fiber pools, `pthread_attr_setstack`) | 128 of 133 | stops | a mapping holding a stack is scanned only from its lowest stack pointer (`scanMapping`, `:165`) |
| Memory the program made read-only with `mprotect` | reused | stops | only readable and writable mappings are scanned |
| The main thread's `pthread_setspecific` | reused | stops | its data lies in the shared cache outside the image table (RFC 0036 amendment 3) |

The control (the pointer in an ordinary heap block) is reused in 0 rounds,
and so are pointers in general registers, stacks, thread-local storage,
globals, anonymous and shared anonymous mappings, and dirty file pages.
With each cause fixed in an evaluation build the silent rounds fell to 0
of 508, 0 of 504 and 0 of 131. Separately, `threadsChanged` (`:285`)
compares only the number of threads, so a thread that exits while another
starts during a sweep leaves the new thread running unseen.

### Silent overflows, crashes and hangs

- **Loop ranges wrap.** Rule 6.4 computes a loop's last address as
  `first + step × count` (`lib/Frontend/GuardLoops.cpp:113`) without
  checking for overflow. `malloc(n * 8)` with `n = 2^61 + 1` wraps to an
  8-byte block, and `for (i = 0; i < n; i++) p[i] = 0` runs the unguarded
  copy until it faults: no report. So do a `do … while` loop whose count is
  0 and a loop with a large stride. Verify mode, which does not version,
  traps.
- **Tagged pointers.** On AArch64 the hardware ignores an address's top
  byte; a guard on a tagged address goes to the slow path, which finds it
  outside the shadow's window and lets it pass.
- **Generic atomics.** `__atomic_load`, `__atomic_store`,
  `__atomic_exchange` and `__atomic_compare_exchange` (the calls Clang
  emits for atomics too large to be lock-free) are not guarded.
- **Compiling a large local exhausts memory.** The frame layout builds a
  vector of one shadow byte per 16 bytes of frame
  (`lib/Frontend/GuardFrames.cpp:244`, `:260`): `char a[1L << 40]` or
  `alloca((size_t)1 << 40)` with a constant made `weavec-cc` grow past
  100 GiB (the reference compiler: about 1 MiB). A run of probes that
  included one ended with the machine's kernel watchdog panic.
- **A variable-length array of negative size** writes shadow for its
  wrapped size at run time: memory grew past 3 GiB.
- **Faults are silent.** A wild access the shadow cannot see (into an
  unmapped page, past the poisoned end of a block of 512 MiB or more)
  ends the program with the kernel's signal and no report; ASan prints
  one.
- **`malloc(2^46)` returns non-null**, where the reference returns null.
- **A signal handler that allocates during a sweep hangs.** While the
  sweep runs, its thread is the only one the kernel can deliver a signal
  to; a handler that calls `malloc` spins on the allocator lock the sweep
  holds. A 1 kHz timer whose handler allocates hung the program within a
  second; the reference ran on.

### Library calls

Of 84 probes of library calls with a bad buffer, 32 pass where ASan stops
them: `readv`, `writev`, `recvmsg` and `sendmsg` (the buffers an `iovec`
names are not guarded); `getline` and `getdelim` with a capacity larger
than the buffer; `inet_pton`, `inet_ntop`, `stat`, `gmtime_r`, `uname`,
`getsockopt`, `getsockname` and `accept` (the bytes are a C type's size or
a length the call reads through a pointer); `sysctl`, `sysctlbyname`,
`basename_r`, `dirname_r` and `if_indextoname` (not in the table);
`wcscpy`, `wcscat` and `wcslen` (wide strings are not scanned);
`sprintf_l` and `snprintf_l`; `fprintf` with a format that is not a
constant (its `%s` and `%n` arguments); `memcpy`, `strcpy` and `read`
called through a function pointer; `memcpy` found with `dlsym`; a read past
a `scandir` entry; and a buffer given to `setvbuf` and then freed. 28
pass under both.

### Servers and threads

| Measurement | v0.17.0 | Reference | ASan |
| --- | --- | --- | --- |
| Allocations per second, small, thread-local, 1/2/4/8 threads (millions) | 33.9 / 10.7 / 9.7 / 5.5 | 42.6 / 32.3 / 36.8 / 139 | 8.5 / 14.9 / 23.4 / 26.4 |
| Replacements per second, 256-byte class, 2,040 / 2,048 MiB live | 173,000 / 6 | 4.9 million | |
| Sweep pause, mean, 256 MiB / 1 GiB / 2 GiB live (25% of words pointers) | 77 / 320 / 672 ms | | |
| The same with every word a pointer | 47 / 207 / 473 ms | | |
| Redis 8.10.2, ~1 GiB, 50 clients: `SET` requests per second | 200–207,000 | 222–245,000 | 180,000 |
| The same, longest request | 149–154 ms | 1.0–1.5 ms | 16 ms |
| Longest request during `BGSAVE` | 187–214 ms | 32–44 ms | 45–54 ms |
| Peak memory (Redis) | 1,735 MiB | 1,124 MiB | 2,359 MiB |

- **Threads slow the allocator down** because they share cache lines: the
  counters of live and quarantined bytes, the statistics, and each class's
  state, which are not padded, behind 100 spinlocks and no thread cache.
  An evaluation patch that padded them restored threads each in its own
  class to 58.6 / 101 / 141 / 128 million; threads in one class gained only
  2.2 times.
- **A full class sweeps on every allocation.** When a class's 2 GiB region
  is exhausted and its quarantine holds slots, `weavec_rt_alloc` sweeps
  before it falls back to a mapping per block
  (`runtime/weavec_alloc.c:977`); a sweep the program's dangling words keep
  from freeing anything runs again at the next allocation (175 ms each),
  and each fallback block costs about 18 KiB.
- **Pauses.** Marking is 74–93% of a pause, and the thread count (up to 64)
  does not matter. RFC 0036's 33 ms at 256 MiB holds for a pointer-sparse
  heap; with dangling words the longest was 155 ms, over its own 150 ms
  gate, and a 1 GiB heap with dangling words did not finish within 300 s.
  Under flat-out churn sweeps took 19–57% of the run time, not the ninth
  RFC 0036 §3.3 states: the hard budget overrides the pacing.

### What passes by design

RFC 0036 leaves to this RFC an index or pointer that jumps over a redzone
into another live object, and a use after return. Twenty probes of jumps
(heap to a neighbor, to a slot 500 away, to another class; within a frame
and into the caller's; global to global, global to heap and back; a
variable-stride pointer; `memcpy` and `strlen` from a jumped pointer; a
versioned loop) pass on v0.17.0 and under ASan. Ten probes of use after
return pass on v0.17.0; ASan's fake stack stops all ten. And a `strcpy`
into an array field passes on glibc, where the C library's inline wrappers
pass the rest of the object as the size (RFC 0036 amendment 9).

### Where the time goes

Each mechanism switched off in turn on v0.17.0 (13 corpus benchmarks,
instructions and cycles retired, geometric mean, ratio to the reference
compiler):

| Build | Instructions | Cycles |
| --- | --- | --- |
| v0.17.0 | 2.14 | 1.634 |
| no guards (and no optimizer barrier) | 1.37 | 1.342 |
| no library guards and wrappers | 1.98 | 1.580 |
| quarantine of 4 GiB (no sweeps; peak memory 2.98 times the reference's) | 2.06 | 1.554 |
| frames without redzones | 2.07 | 1.640 |
| no ASan contract | 2.13 | 1.640 |
| every local and global rule of *Alternatives* that loses nothing | 2.11 | 1.636 |
| that, with a 128 MiB quarantine | 2.05 | 1.577 |

Guards are 45% of the cost, and most executed guards have no redundancy a
local rule could find: rules that removed 3% of the instructions removed
1% of the cycles. The quarantine and the library wrappers are where the
safe gains are. This RFC spends what option B costs (about 7% of cycles)
and recovers part of it from the quarantine (§2.4).

### Linux

The weekly full corpus run (`.github/workflows/corpus.yml`) has failed for
six weeks and last ran on v0.16.0 (101 problems, most of them libuv and
libevent compiling Darwin-specific files); v0.17.0 has never had a full
Linux run, and the README says Linux is not measured.

## Soundness

### The guarantee

For a program whose every unit is compiled by `weavec-cc` in an enforcing
mode, with the runtime linked, every memory access outside a
`WEAVEC_UNSAFE` region satisfies (A), (B) and (R) of RFC 0035 and (T),
(F) and (P) of RFC 0036, with (T) and (F) amended, and two new properties:

- **(T) Temporal.** As RFC 0036, with the sweep's roots of §1: a byte of a
  released heap block is not addressable until a sweep has found no
  pointer to the block in the threads' registers (general and vector),
  stacks and thread-specific data, the process's readable memory but
  executable mappings, device memory and file mappings it cannot write
  through, and the live heap.
- **(F) Fortified and field sizes.** As RFC 0036, and a string or
  formatting call that writes into an array field of a struct (not its
  last) writes no more than the field's size (§10).
- **(C) Colors.** An access whose address the function computes from a
  pointer, its *root*, by arithmetic (§8.1) traps when its bytes are
  addressable but lie in a tracked object whose color differs from the
  color of the root's object (§7), unless the root lies inside a live
  heap object other than at its start (amendment 12). Colors are not
  exact: two heap slots
  whose numbers (§7.2) differ by less than 111 (so any two slots of a
  class fewer than 111 apart), the locals of one frame, and globals
  registered fewer than 111 apart always differ; any other two
  objects share a color with probability 1 in 111 (1 in 7 when either
  byte is in an object's partial last granule).
- **(U) Returned frames.** An access to bytes of a frame of WeaveC's code
  that has returned traps as `stack-use-after-return`, until a frame of
  WeaveC's code with tracked locals is laid over those bytes, another
  thread accesses them, or the access runs on another stack (§9).

**What the guarantee means for the bug classes:**

- **Heap, stack and global overflow** by an index or pointer into a
  neighbor: traps when the index jumps over the redzone into an object
  of another color, deterministically for neighbors (§7.2).
- **Use after return**: traps through a stale pointer to a frame of
  WeaveC's code, unless a later call at that depth laid a tracked frame
  over it (ASan's fake stack catches that case too).
- **Use after free, double free**: as RFC 0036, now through any pointer
  the program holds in the memory (T) names.
- **Overflow inside an object** by a string or formatting call into an
  array field: traps on every C library, fortified or not.

### Assumptions

RFC 0035's A1–A5 and RFC 0036's A6–A7 stay, with these changes:

- **A6 — visible pointers** names the memory of (T): a pointer stored only
  in memory the program made inaccessible (`PROT_NONE`), in a shared file
  mapping it then made read-only, in device memory, or as a transformed
  integer, does not keep its block from reuse.
- **A8 — colors are inherited, not proven.** A pointer the function did
  not compute (a parameter, a loaded value, a call's result) is taken to
  point into the object it is meant to point into; (C) checks arithmetic
  from it, not the pointer itself. A pointer moved into another object in
  one function and used in another passes (§8.4).

### Where this is more sound than RFC 0036

The silent reuses, the loop overrun, the tagged and atomic accesses and the
library calls of the motivation trap; a fault is reported; a jump into a
neighbor and a use after return trap; a field overflow by a string call
traps on glibc.

### Where this is less sound than RFC 0036

Nowhere. A color check or a returned frame only adds traps; a sweep with
workers reads the same roots as one without; a thread cache holds released
blocks in the quarantine as the class lists do (§2.1).

### Bugs deliberately not caught

A jump into an object of the same color (one in 111 for objects not named
in (C)); a jump from a pointer into the middle of a heap block (amendment
12); a pointer moved into another object and used in another function;
a jump at a constant offset that the root's own guard covers; use after
return through a frame a later WeaveC frame reused, through another thread,
or on a stack that is not a thread's own; `memcpy` and `memset` past a
field (§10); a library function found with `dlsym`; a library's own use of
a buffer the program freed (`setvbuf`: A2); and everything RFC 0036 leaves
out but the two this RFC takes on.

### Accepted false traps

- **Arithmetic between objects** that C leaves undefined: `p + (q - p)`
  with `p` and `q` in different objects (a relocation of interior pointers
  after `realloc`, written as `new + (old_ptr - old)`, traps when the old
  block is still live; ASan lets it pass). No case of the 23 corpus suites
  does it. `WEAVEC_UNSAFE` or `-fno-weavec-colors` (§12) builds such code.
- **Biased bases** are not false traps: a root in poison (one past an
  object, before it, released), and a root inside another live heap object
  other than at its start (a one-based array, `malloc(n) - 1`, a
  compressor's window base), pass (§8.3, amendment 12).
- **Indexing one heap block from another's start**: zstd's dictionary
  trainer (`ZSTD_compressBlock_doubleFast_dictMatchState` under COVER and
  ZDICT) reads one block through an index computed from the start of
  another, and traps in the corpus gate's `zstd` suite. ASan lets it pass.
  `-fno-weavec-colors` builds it. The rule stays strict by decision:
  letting a root at a block's start reach other blocks would let a jump
  over a heap redzone into a neighbor pass, which is most of what heap
  colors catch. A false trap is not triaged (`test/corpus/README.md`), so
  the weekly full gate stays red on zstd until the rule or the gate's
  zstd config changes.

### Cost

The color checks cost about 7% of cycles on the corpus benchmarks; the
returned frames about 3%; the larger quarantine gives back about 4% for
memory (§2.4). Thread caches and parallel marking are expected to cost
nothing single-threaded. *Acceptance gates* bound all of it.

## Detailed design

### 1. The sweep's roots

RFC 0036 §3.4 stays but for its *Mark* step's roots, which become:

1. **Registers.** On Darwin, each stopped thread's general registers and
   its vector registers (`ARM_NEON_STATE64` on AArch64; `x86_AVX_STATE64`,
   or `x86_FLOAT_STATE64` where AVX is absent, on x86-64), each register
   scanned as aligned words. On Linux the kernel saves both on the stopped
   thread's stack, between the two stack pointers the handler records,
   which is scanned. The sweep's signal is installed without `SA_ONSTACK`,
   so that the handler's frame lies on the thread's stack.
2. **Stacks.** A thread's stack is scanned from its stack pointer (less the
   ABI's red zone) to the end of the thread's stack (on Darwin
   `pthread_get_stackaddr_np`, on Linux `pthread_getattr_np`, cached per
   thread at its first sweep). A stack pointer outside its thread's own
   stack (a coroutine, a fiber, a signal's alternate stack) is scanned from
   itself to the end of the mapping that holds it. The rest of every
   mapping, including the parts of a thread's stack mapping that are not
   its live stack, is scanned as ordinary memory under the rules below.
   Only the bytes below a thread's stack pointer in its own stack are
   skipped.
3. **Thread-specific data.** Each stopped thread's thread-specific values,
   read with `pthread_getspecific` semantics over the keys the process
   created: on Darwin from the thread's `pthread_t` (`pthread_from_mach_
   thread_np`), whose key slots the sweep scans; on Linux they live in the
   thread's descriptor, inside its stack mapping or the static TLS block,
   which are scanned already.
4. **Mappings.** Every readable mapping but the runtime's own, executable
   mappings, device memory (RFC 0036 amendment 10), and shared file
   mappings whose current protection does not allow writing (the program
   cannot have stored a pointer through them since they were mapped so).
   Anonymous and private mappings are read whatever their protection, so
   memory the program made read-only after writing it is read. On Darwin a
   private or anonymous mapping is read where its pages are anonymous,
   modified or paged out (`mincore`), which is where the program wrote, and
   a writable shared file mapping wherever its pages are resident (a page
   written and then written back by `msync` is clean, not empty). On Linux,
   which has no cheap page state, the mappings are read whole, as before.
   The image table of RFC 0036 amendment 3 stays for the dyld shared
   cache.
5. **The live heap**, as before.

**The thread list.** After the mark the sweep compares the list of threads
with the list it stopped, by identity (Darwin's thread ports, Linux's task
ids, sorted); any difference recycles nothing.

**Signals.** The sweeping thread blocks every signal it may block from
before it takes the allocator's locks until it releases them, so that no
handler runs on it while it holds them. A stopped thread on Linux waits in
its handler with every signal blocked, as before.

### 2. The allocator

#### 2.1 Thread caches

Each thread has, per small class, an *allocation cache* of up to 32 free
slots and 32 KiB, and a *release cache* of up to 32 released slots and
32 KiB, found through a `pthread_key_t` created at start-up (not
`__thread`, whose first use on Darwin calls `calloc`). An allocation takes
a slot from its cache and refills it from the class's free list under the
class lock; a release validates the slot (R), writes `0xFD` over it as
before, and pushes it to its release cache, which moves to the class's
quarantine under the lock when full. A slot in a release cache is
quarantined: its slot word says released, and a second release of it is a
double free.

A sweep reads but never changes another thread's caches (a stopped thread
may be inside a push): the slots of the release caches are marked as the
quarantine is, but only slots on the class's quarantine list are
recycled. A thread's caches move to the class lists at its exit (the key's
destructor) and in a child after `fork` (every other thread's caches, from
the list of caches the runtime keeps).

#### 2.2 Shared state

The counters of live and quarantined bytes become per-thread deltas,
folded into the shared counters every 256 KiB of change and by each sweep.
Each class's state is aligned to 128 bytes. The statistics counters are
incremented only when `WEAVEC_RT_STATS` is set, which a global flag read
once per call tells.

#### 2.3 Full classes

A class whose region is exhausted takes a further region from the arena's
reserve (up to 16 regions per class), and a region-full allocation sweeps
only when the class's quarantine gained at least a quarter of a region
since the last sweep; otherwise it takes a fresh slot from the new region.
The fallback to a mapping per block goes, with the sweep it ran before.
A request the arena cannot place inside the shadow's window (on Darwin,
more than a quarter of the window) fails with `ENOMEM`.

#### 2.4 The quarantine's budget

`WEAVEC_RT_QUARANTINE` defaults to 64 MiB (was 16 MiB); the soft and hard
budgets of RFC 0036 §3.3 and its amendment 1 stay as formulas of it. The
cost measurement found 128 MiB 3.6% faster than 16 MiB at 1.67 times the
reference's peak memory (16 MiB: 1.21); 64 MiB takes part of the gain for
part of the memory, and the gates (G7, G9) check both.

### 3. Parallel marking

The mark reads every root and every live heap block once, with no tracing,
so it divides without coordination. When the live heap and the roots
exceed 64 MiB, the sweep hands ranges of 4 MiB to *mark workers*: threads
the runtime creates at the first such sweep (before it stops the world),
`min(cores - 1, 7)` of them unless `WEAVEC_RT_SWEEP_THREADS=<n>` says
otherwise (`0` marks on the sweeping thread alone). They wait on a
semaphore between sweeps, are never stopped and are not roots, set marks
with atomic `or`, and call nothing that takes a lock. A child of `fork`
has none; its first large sweep creates them. A worker that cannot be
created leaves the sweep to the sweeping thread.

### 4. Guards that must not miss

- **Loop ranges (rule 6.4).** The preheader computes each range's span
  with overflow checks (`llvm.umul.with.overflow` and
  `llvm.uadd.with.overflow` on the trip count, the step and the start), and
  a wrap of any of them, or a trip count the loop's own test could make
  `UINT_MAX` (a `do … while` whose count is 0), runs the guarded loop.
- **Tagged addresses.** On AArch64 the slow path clears the top byte of an
  address outside the window before deciding; an address whose untagged
  form is inside it is decided as that address.
- **Generic atomics.** Rows for `__atomic_load`, `__atomic_store`,
  `__atomic_exchange` and `__atomic_compare_exchange` (their first
  argument is the size).
- **Dynamic objects.** `__weavec_rt_alloca_poison` writes the shadow only
  of the part of a dynamic object that lies inside the thread's stack (§1's
  bounds). A larger one (a variable-length array of negative size wraps to
  one) cannot be used without leaving the stack, and its first access
  faults (§6), as it does without WeaveC. Its cost no longer grows past the
  stack's size.
- **Large frames.** The frame layout describes the shadow as runs of a
  value, not one byte per granule, and writes a run longer than 4 KiB of
  shadow with a runtime call rather than stores. Its compile-time memory
  grows with the number of locals, not their size.

### 5. Library calls

- **Wrappers.** A call whose bytes are a C type's size or are read through
  a structure is redirected (`wrapper(NAME)`) to a runtime wrapper, which
  the runtime's build compiles with the target's headers: `readv`,
  `writev`, `preadv`, `pwritev`, `recvmsg`, `sendmsg` (each `iovec`'s
  buffer), `getline` and `getdelim` (the buffer over the capacity the call
  is given), `inet_pton`, `inet_ntop`, `stat`, `lstat`, `fstat`,
  `fstatat`, `gmtime_r`, `localtime_r`, `uname`, `getsockopt`,
  `getsockname`, `getpeername`, `accept`, `recvfrom` (the length a pointer
  argument holds, then the bytes the call reports writing), `sysctl`,
  `sysctlbyname`, `basename_r`, `dirname_r`, `if_indextoname`,
  `sprintf_l`, `snprintf_l` and `vsnprintf_l`.
- **Wide strings.** `__weavec_rt_wcslen(s, max, site)` scans a wide string
  as `__weavec_rt_strlen` scans a narrow one; the `wide` rows guard their
  strings with it (RFC 0035 amendment 7's known gap).
- **Formats that are not constants.** A `printf`- or `scanf`-family call
  whose format is not a constant is redirected to its wrapper, which
  parses the format at run time and checks the `%s` and `%n` arguments, as
  the `sprintf` wrappers already do.
- **Calls through a pointer.** Where a unit takes the address of a
  function whose row guards an argument (not a release), GuardInsert
  replaces the address with that of a *thunk*: a `linkonce_odr` function
  named `__weavec_thunk_NAME` with the function's type, whose body is a
  direct, guarded call of the function. Every unit's thunk of a function
  is the same function, so its address compares equal across units built
  by `weavec-cc`; it differs from the address code built elsewhere takes.
  A function found with `dlsym` is not guarded (A2).

### 6. Faults

The runtime installs handlers for `SIGSEGV` and `SIGBUS` at start-up where
their disposition is the default, and, on a fault, prints

```
weavec: memory-fault at pc 0x<pc>: <read|write|access> at 0x<address>
weavec: <what the runtime knows of the address>
```

(the second line as `__asan_describe_address` writes it: inside a released
block, past a heap object, the inaccessible part of a huge block, the
shadow), then the stack with `WEAVEC_RT_BACKTRACE=1`, restores the default
disposition and returns, so that the fault repeats and the program dies of
it as before. A handler the program installs later replaces it; a fault on
a stack overflow, which has no stack to run the handler on, is the kernel's
as before. `WEAVEC_RT_FAULTS=0` installs none.

### 7. The shadow encoding

#### 7.1 Values

The shadow byte of a granule (RFC 0035 §2.2) becomes:

| Value | Meaning |
| --- | --- |
| `0x00` | all 16 bytes addressable, untracked or uncolored memory |
| `0x01`–`0x7F` | all 16 bytes addressable, in an object of this color |
| `0x80`–`0xEF` | `1ccc kkkk`: the first `k` bytes (1–15) addressable, in an object whose color's low three bits are `ccc` |
| `0xF0`–`0xFF` | not addressable: RFC 0035's poison values, `0xF5` (§9), `0xF7` (RFC 0036 §5) and `0xFE` (RFC 0035 amendment 3) |

A color is never `0x00` and its low three bits are never `111`, so a
partial granule is never mistaken for poison: there are 111 colors,
`color(n) = ((m / 7) << 3) | (m % 7)` with `m = n mod 111 + 1`.

A guard without a root (§8.1) tests the sign bit: a granule below `0x80` is
whole and addressable. The one-granule fast path stays three instructions
(on AArch64 `ldrb`, `tbnz`), the partial test stays on the slow side, and
the multi-byte forms of RFC 0035 amendment 4 test `0x80` in each byte they
load. The slow paths, `__weavec_rt_range_ok`, ASan's interface and
`weavecRtQuickOk` read the encoding through the runtime header's
functions.

#### 7.2 Who gets which color

- **Heap slots**: `color(slot + 41 × class)`, the slot's *number*, from
  its index in its class's regions. Neighbors in a class always differ, and so do slots
  fewer than 111 apart. A reused slot has its previous occupant's color
  (use after free is (T)'s). An object of four pages or more, whose shadow
  is mapped afresh (RFC 0035 amendment 3), is uncolored (`0x00`) but for
  its partial last granule.
- **Locals**: a tracked frame's locals get `color(h + i)`, `h` a hash of
  the function's name and `i` the local's index in the layout: the locals
  of one frame differ. Dynamic objects are uncolored.
- **Globals**: `color(n)` for the `n`-th global the process registers: a
  unit's globals differ from each other.
- **Poisoned pools**: `__asan_unpoison_memory_region` writes the color of
  the heap slot that holds the region, or `0x00` outside the heap.

### 8. Color checks

#### 8.1 Which accesses

A guarded access whose address is a `getelementptr` chain from a pointer
value (its *root*: not a constant, not an `inttoptr`) gets a color check,
but:

- an access whose offset from its root is an affine recurrence with a
  constant step of at most 32 bytes and a constant start within 4 KiB of
  the root: such a walk reaches the object's redzone before any neighbor,
  where the guard itself stops it; in verify mode its color check is
  emitted as a monitor and reports `weavec.proven` (RFC 0035 §6);
- accesses whose guard a rule removed (RFC 0035 §6), which inherit the
  color check of the guard that covers them, or need none (rule 6.1).

A string argument's root is its pointer stripped of constant offsets; its
color is checked at its first byte. A range (`memcpy`) is checked at its
first byte.

#### 8.2 The inline check

GuardExpand loads each root's shadow byte once, at the nearest common
dominator of its accesses, hoisted out of the loops in which the root does
not change. A stale value is good: the root's block is not reused while the
root is held (T), and the access's own shadow byte says whether it is still
live. A one-granule guard with a root compares the access's shadow byte
with the root's (sign-extended, so a partial byte never equals a whole
one): equal passes, which is four instructions on AArch64. A local's
color is the frame layout's constant. Otherwise the check block tests
poison and the partial granule as before, and then, when the bytes differ,
calls

```
__weavec_rt_jump(address, width, root, rootShadow, site)
```

Wider guards test the color of their first granule apart from their
shadow test.

#### 8.3 The slow path

`__weavec_rt_jump` decides, in this order: the access's byte is poison
(the guard reports it), or the root's is (a root one past or before its
object, or released, cannot be compared): pass; the colors are equal, or
either byte is partial and their low three bits are equal: pass; the root
and the access lie in one heap object (the slot words say so): pass; the
root lies inside another live heap object within 4 KiB of the accessed
one, other than at that object's start (a one-based array, a biased
pointer): pass; a stack byte whose frame lost its colors to
`__weavec_rt_unpoison_stack` (a non-local exit clears the stack to `0x00`)
on either side: pass; otherwise the access traps with the kind of the
accessed memory (`heap-buffer-overflow`, `stack-buffer-overflow`,
`global-buffer-overflow`) and the second line of *Diagnostics*.

#### 8.4 What it does not see

A pointer the function received or loaded is a root in its own right, so a
pointer moved into another object by one function and used by another is
checked against the object it points into, which passes. Checking pointers
where they leave a function (a store, a return, an argument) catches that
for 8% more cycles and traps on a biased pointer stored far from its
object; it is not part of this RFC.

#### 8.5 Rules and modes

Rule 6.2 and 6.3 keep their meaning; a merged guard checks the color of
its hull's first granule against the root of its first access. Rule 6.4's
preheader also compares the color of each range's first granule with its
root's; a mismatch runs the guarded loop. `-fno-weavec-colors` emits no
color checks (the shadow is colored all the same). The ledger is
unchanged.

### 9. Returned frames

At each return a tracked frame's shadow is written `0xF5` (*returned*),
not `0x00`. A guard that meets `0xF5` takes its slow path, which decides:

- a granule on the current thread's own stack (§1's bounds) below the
  caller's stack pointer is a returned frame: `stack-use-after-return`;
- any other `0xF5` is stale (a frame of code without WeaveC, or an untracked
  frame of WeaveC's code, now holds those bytes; or the access runs on an
  alternate stack, a coroutine or another thread): the slow path writes
  `0x00` over the granule and decides the access again.

The caller's stack pointer is the frame address of the `WEAVEC_RT_SLOW`
entry point, which passes it to its helper (no thread-local storage on the
slow path). `__weavec_rt_unpoison_stack` (before a `noreturn` call) and
the sweep are unchanged; a tracked frame's entry writes its whole shadow,
over any `0xF5`, as before.

### 10. Field limits

A call of a wrapper that takes a limit (`strcpy`, `stpcpy`, `strcat`,
`sprintf`, `vsprintf`, `snprintf`, `vsnprintf`, and their fortified forms)
whose destination is a `getelementptr` to an array field of a struct that
is not the struct's last element passes the field's size as its limit, or
the smaller of it and the `chk` size. A write past it within addressable
bytes reports `intra-object-overflow`, as RFC 0036 §6. `memcpy`, `memset`
and the other byte copies are not limited: code that copies over
consecutive fields on purpose (`memset(&s->a, 0, sizeof *s - offsetof(…))`)
is common and correct in practice.

### 11. Site records

A site (`struct __weavec_rt_site`) becomes 16 bytes: a signed 32-bit offset
from the record to its file name, the line, the column and the flags. The
records and the names are constants in read-only data with no relocations
(SQLite's 21,080 sites took 506 KB of `__DATA,__const`, 24 bytes each, each
with a rebased pointer).

### 12. The runtime and the driver

- **The runtime's version** (the dispatch table of `weavec_owner.c`) goes
  to 5: the shadow encoding, the site record and the entry points change.
  New entry points: `__weavec_rt_jump`, `__weavec_rt_wcslen`, the wrappers
  of §5 and `__weavec_rt_frame_shadow(frame, runs, count)` (§4).
- **Options.** New: `-fno-weavec-colors`. New environment variables:
  `WEAVEC_RT_SWEEP_THREADS`, `WEAVEC_RT_FAULTS`.
- **The runtime's line budget** rises from 6,000 to 7,000 lines (AGENTS.md
  rule 9, RFC 0036 gate H1): §1–§3, §5, §6 and §9 are runtime code; the
  library's 9,000 stays.

### 13. Deletions

The allocator's fallback to a mapping per block for a full region and the
sweep it runs first (§2.3); the shadow's old partial values `0x01`–`0x0F`
and every test of `== 0` for "addressable" (§7.1); the clearing of a frame
at return (§9); the per-granule shadow vectors of the frame layout (§4);
the 24-byte site record (§11); `scanMapping`'s lowest-stack-pointer rule
(§1). The evaluation branches' toggles (low-fat masks, arena frames, a
fake stack, escape checks) are not merged.

### 14. Tests

- **Cases.** `test/cases/determinism/` gains a case per row of the
  motivation's *Silent reuse* table and its loop, tag, atomic, VLA and
  fault probes, each with a correct twin; `test/cases/library/` a case per
  call of *Library calls* this RFC guards; `test/cases/colors/` the
  twenty jump probes, the ten use-after-return probes and the false-trap
  probes (biased bases, one-based arrays, `container_of`, `qsort`
  callbacks, `realloc` loops, tagged bases), with `p + (q - p)` as an
  expected stop. The three rows of RFC 0036's motivation left to this RFC
  become `TRAP`.
- **Lit.** `test/Guards/` pins the color check's IR (`ir-colors.c`), the
  returned frame's shadow (`ir-frames.c`), the loop range's overflow test
  (`ir-loops.c`), the thunks, and each new report line; `test/Driver/`
  the site records' section.
- **Runtime test.** `rt_test.c` covers the roots of §1 (each with a thread
  that holds the only pointer), the thread caches (fork, thread exit,
  double free from a cache), full classes, the workers, the fault
  handler, a signal handler that allocates during a sweep, and the color
  encoding's functions.

## Annotation surface

None.

## Diagnostics

New run-time report kinds (RFC 0035 §5.3):

- `stack-use-after-return`:
  `weavec: stack-use-after-return at <file>:<line>:<col>: <read|write> of <n> bytes at <address>`,
  with the second line `weavec: <address> is in a frame that returned`.

  ```c
  int *f(void) { int a[4] = {0}; return a; }
  int main(void) { int *p = f(); return p[1]; }
  ```

- `memory-fault`: the two lines of §6.

  ```c
  int main(void) { return *(volatile int *)0x7000000000; }
  ```

A color mismatch reports the accessed memory's existing kind, with the
second line
`weavec: <address> is <n> bytes from its base <root>, in another object`:

```c
#include <stdlib.h>
int main(int argc, char **argv) {
  char *a = malloc(16), *b = malloc(16);
  (void)b;
  return a[32 * argc];   /* lands in b's slot */
}
```

`intra-object-overflow` also covers §10's field limits. Each kind gets its
`test/Guards/` test, its row in the command-line reference
(`docs/pages/reference/cli.md`) and its entry in `scripts/run-cases.py`.

## Implementation plan

The stages land on one branch and ship as one change:

- **S0 The RFC.**
- **S1 The roots** (§1) and the signal mask, with the runtime tests.
- **S2 The allocator** (§2): thread caches, shared state, full classes,
  the quarantine's default.
- **S3 Parallel marking** (§3).
- **S4 The guards' holes** (§4) and faults (§6).
- **S5 Library calls** (§5).
- **S6 Colors** (§7, §8): the encoding first, measured alone, then the
  checks.
- **S7 Returned frames** (§9) and field limits (§10).
- **S8 Site records** (§11).
- **S9 Fresh iterations**: the cases, the corpus and the gates on
  darwin-arm64; the weekly Linux corpus workflow fixed and green; the
  documentation; RFC 0036 marked Implemented; this RFC's *Measured gates*.

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference is
`$WEAVEC_LLVM_PREFIX/bin/clang`; v0.17.0 is built from its tag on the same
machine; darwin-arm64 (Apple M3) unless named; ratios of best-of-three
cycles on an idle machine.

**Detection**

- **G1. Holes.** Every case of §14 for the motivation's silent reuses,
  overflows and faults stops with its kind, in trap and verify mode, on
  darwin-arm64 and linux-x86_64 (where the platform has the mechanism);
  the statistical ones stop in every one of 500 rounds.
- **G2. Library.** Every call of *Library calls* but `dlsym`'s and
  `setvbuf`'s stops.
- **G3. Colors and frames.** At least 15 of the 20 jump probes and 7 of
  the 10 use-after-return probes stop; their twins and the false-trap
  probes but `p + (q - p)` run clean.
- **G4. No loss.** The detection, detection-blind and soundness suites stop
  at least as many bugs as v0.17.0 (113 of 117), no twin stops, and verify
  mode reports no `weavec.proven` on any case.

**Drop-in**

- **G5. No regression.** Every corpus config builds and passes its suite in
  trap mode with no report (`scripts/corpus-gate.py --full`), on
  darwin-arm64 and in the weekly linux-x86_64 run.
- **G6. Untuned programs.** CPython 3.13.7, SQLite 3.50.4 and coreutils
  9.5 pass as in RFC 0036's G4, and Redis 8.10.2 passes its test suite.

**Cost**

- **G7. Run.** The corpus benchmarks' geometric mean at most 1.10 times
  v0.17.0's (1.80 the reference), and no benchmark above 1.25 times its
  v0.17.0 ratio.
- **G8. Build.** No project's build more than 1.10 times v0.17.0's;
  `char a[1L << 40]` and `alloca((size_t)1 << 40)` compile in at most twice
  the reference's memory; SQLite's `__DATA` at most 300 KB (v0.17.0: 714).
- **G9. Memory.** Peak memory of each benchmark at most 1.5 times
  v0.17.0's, or `2 × WEAVEC_RT_QUARANTINE` above it, whichever is larger.
- **G10. Pauses.** The longest sweep at most 50 ms with 256 MiB of live
  heap and at most 150 ms with 1 GiB, at pointer densities of 0, 25 and
  100%; a 1 GiB heap with dangling words finishes, with sweeps under a
  quarter of its run time; under flat-out churn with 256 MiB live, sweeps
  under a quarter of the run time.
- **G11. Threads.** Thread-local small allocations at 8 threads at least
  three times the 1-thread rate and at least ASan's; a 256-byte class at
  3 GiB live at least half its rate at 1 GiB.
- **G12. Servers.** Redis at ~1 GiB: `SET` and `GET` requests per second at
  least 0.9 times the reference's, the longest request at most 60 ms, at
  most 75 ms during `BGSAVE`; a 1 kHz timer whose handler allocates runs
  60 s.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes, with the library at most
  9,000 lines and the runtime at most 7,000.
- **H2.** The documentation describes the change (README, guarantees, CLI,
  compatibility, troubleshooting, architecture, roadmap, AGENTS.md);
  `npm test && npm run build` in `docs/`; RFC 0036 and this RFC are marked
  Implemented.

## Implementation amendments

These record where the implementation departs from the design above. They
override the body.

1. **A thread's own stack (§1).** The stack bounds of another thread
   (`pthread_get_stackaddr_np`, `pthread_getattr_np`) take locks a stopped
   thread may hold, so the sweep tells a thread's own stack otherwise. On
   Darwin a mapping libpthread or the kernel made for a stack carries the
   tag `VM_MEMORY_STACK`: such a mapping is read from its lowest stack
   pointer up, and any other mapping that holds a stack pointer (a
   coroutine's, a fiber pool's, one given to `pthread_attr_setstack`) is
   read whole, where its pages are written. On Linux only the main thread's
   stack is named (`[stack]`); below a stack pointer anywhere else the
   resident pages are read (`mincore`). A thread's dead stack below its
   stack pointer is then read too, which can keep a block a stale word
   there points to, never reuse one.
2. **Thread-specific data (§1).** On Darwin a thread's TSD array is at the
   handle `THREAD_IDENTIFIER_INFO` returns (a Mach call, no lock), and a
   key is the slot's index. Only the arrays outside every stack mapping are
   read, that is the main thread's, in libpthread's data in the shared
   cache: 1,024 slots, up to the end of the region that holds it.
3. **File mappings (§1).** Darwin reports a file mapped once as private,
   whatever its sharing, so the rule is by protection: every resident page
   of a file mapping the program can write is read (a private one's clean
   pages hold the file, which costs only reading), and a mapping that is
   executable but writable (JIT code) is read as data.
4. **Thread caches (§2.1).** A cached slot is its region and index, with
   the top bit set, not its address: the sweep reads the caches' memory, and
   an address there would keep its slot. A release claims the slot with a
   compare-and-swap of its word from live to *claimed* (released, with the
   value `CachedDead`), which a racing second release finds and reports as a
   double free; a sweep leaves claimed slots as they are. A sweep first
   moves the sweeping thread's own caches to the shared lists. Cached slots
   are handed out in address order (RFC 0036 amendment 4). On Darwin the
   cache is found by reading the thread's TSD slot inline (`tpidrro_el0`),
   as libpthread's own accessor does: the call cost as much as the rest of
   a small allocation's fast path.
5. **Spare regions (§2.3).** The arena reserves 160 regions: one per class
   (96 at 2 GiB a region) and 64 spares any class may take, not up to 16
   per class. A heap slot's color is `color(slot + 41 × region)`. When the
   spares are spent the fallback to a mapping per block stays, after a
   sweep.
6. **Workers (§3).** The workers wait on a mutex and a condition variable,
   on stacks of the runtime's own (not scanned), and are created with every
   signal blocked.
7. **Large frames and dynamic objects (§4).** `__weavec_rt_frame_shadow`
   takes one run (`address, bytes, value`), called once per run of 4,096
   granules or more. A dynamic object's shadow is written only when the
   object lies on the thread's stack, from its bottom to its top.
8. **Formatted calls (§5).** A `printf`-family call with a constant format
   counted as cheap to guard inline, and its `%s` arguments were then not
   guarded at all: a row with a format and a wrapper is always redirected,
   and the wrapper reads the format, constant or not. A wide literal is not
   measured from its bytes (`getConstantStringInfo` read `L"…"` as a
   one-character string). A thunk's call has no source location, so its
   report says `<unknown>`; `recvfrom`'s wrapper takes the limit of its
   fortified form.
9. **The color report (Diagnostics).** The second line is `weavec:
   <address> is <n> bytes after its base <root>, in another object` (or
   `before`). A loop range's color test is the runtime's
   `__weavec_rt_color_ok(root, address)`, which decides as the slow path
   does.
10. **Site records (§11).** A 32-bit offset could not survive the runtime's
    copies of a site, on the stack, far from the image: the offset is 64
    bits, self-relative, with a 16-bit column and 16 bits of flags, still 16
    bytes. The runtime fills a copy in place (`weavecRtSiteSet`) rather
    than returning one by value.
11. **Signal handlers that allocate (§1, §2.1).** Blocking signals in the
    sweep was not enough once threads had caches: a handler that allocated
    while its thread was inside an operation on its own cache corrupted the
    cache (the 1 kHz probe died within a fifth of a second). Each thread's
    cache counts the thread's calls into the allocator in progress and the
    allocator locks it holds. A call made while another is in progress (a
    handler's) uses no cache and sweeps nothing; while the thread holds an
    allocator lock it allocates nothing (`ENOMEM`), and its releases wait
    until the outer call returns (16 of them; a seventeenth block stays
    allocated). C and POSIX do not allow `malloc` in a handler; this keeps
    a program that does it from waiting forever or corrupting the heap.
    The count costs a single-threaded allocation about 6%.
12. **Heap colors (§8.3).** LZ4's and zstd's match finders keep a window
    base biased by more than 4 KiB into a live block and index far from it;
    §8.3's 4 KiB rule trapped both in the corpus gate. On the heap only an
    object's start is held to its color: a root inside a live heap object
    anywhere but at its start passes, at any distance. A root in uncolored
    memory (`0x00`: untracked memory, a huge block, a frame a non-local
    exit cleared) passes. An access to uncolored memory from a colored
    root passes only on the stack after a non-local exit cleared it. The
    jump probe whose root is an interior pointer near its object
    (`j20_interior_base_near`) passes since; a root at a block's start (the
    pointer `malloc` returned, an array's name) is checked as before.
13. **Library rows and thunks (§5).** `mbrlen` and `mbrtowc` take a byte
    string, not a wide one; their `wide` clause made every call through
    `xz` stop as `unterminated-string`. A library function's address in a
    constant initializer (a table of function pointers) was replaced in
    place, which corrupted the constant; the thunk now replaces each use
    that takes the address but its own (`test/Guards/library-thunk.c`).
14. **Threads that exit during a stop (§1).** `pthread_join` can return
    before the kernel drops the thread from the process's list. The sweep
    took such a thread for one it could not stop, or for a change in the
    list, and recycled nothing (the `thread-caches` runtime test failed
    about one run in five). A thread that has terminated is no root: on
    Darwin, one whose port is dead when it is suspended; on Linux, one that
    `tgkill` no longer finds while the sweep waits for it to answer. The
    lists before and after are still compared by identity, so a thread
    that started during the sweep still holds up recycling.
15. **Review fixes (§1, §4–§6, §8.5).** A wide string's elements are each
    checked before they are read, so an unterminated one is reported
    before the scan leaves its object (the narrow scan still reads up to
    the terminator first; it is the hot path). The fault handler raises
    the signal again under the default disposition before it returns, so
    a `SIGSEGV` or `SIGBUS` sent with `kill` or `raise` ends the program
    rather than being taken for a fault that retries; it is not reported
    where the platform says it was sent (Linux's `si_code`, the arm64
    syndrome of a system call on Darwin). On Linux a private mapping the
    program cannot write is read only where `/proc/self/pagemap` says a
    page is present or swapped and not the file's (resident pages when
    it cannot be read), as Darwin reads only the pages written: a mapped
    file is not read again by every sweep. Without colors, a loop whose
    accesses are computed from a pointer it moves is versioned as on
    v0.17.0; with colors it stays the guarded loop. A frame's shadow run
    and an `alloca`'s shadow on a stack the thread does not own (a
    coroutine's) are written up to 1 GiB, as `__weavec_rt_unpoison_stack`
    clears one; on the thread's own stack only what fits is.

## Measured gates

Measured 2026-11-02 on the branch (darwin-arm64, Apple M3, `release`
preset, reference `clang` 23.1.0; v0.17.0 built from `1f12e5cd`; cycles of
13 benchmarks, best of three). The RFC stays **Accepted**: linux-x86_64 is
not measured (the local Docker daemon does not answer; CI measures it once
the branch is pushed), G6's untuned programs need their sources fetched,
and G8, G11 and G12 miss in part (below).

| Gate | Result |
| --- | --- |
| G1 Holes | **Pass on darwin-arm64** in single runs: the runtime tests of every root (`read-only-memory-is-scanned`, `file-mapping-is-scanned`, `stack-pool-is-scanned`, `thread-data-is-scanned`, `vector-registers-are-scanned`) and `signals-during-sweeps` fail on v0.17.0's sweep and pass; the cases `loop-range-wrap`, `large-atomic` and the lit tests `loop-range-wrap.c`, `memory-fault.c` stop. The 500-round statistical probes were not rerun; linux-x86_64 not measured. |
| G2 Library | **Pass**: 58 of the assessment's 84 library probes stop (v0.17.0: 24); of the 32 ASan stopped and v0.17.0 did not, all but `dlsym`'s and `setvbuf`'s. |
| G3 Colors and frames | **Pass**: 15 of the 20 jump probes and 7 of the 10 use-after-return probes stop (the five that pass: a same-color slot, three pointers used in another function, and since amendment 12 an interior root); the 16 false-trap probes run clean but the two `p + (q - p)` ones, which stop as accepted; 3 of the 4 field probes stop (`memcpy` past a field passes, §10), and the 2 that must pass run clean. |
| G4 No loss | **Pass**: 553 cases pass, 4 `XFAIL`, no failure; the detection suites stop 114 of 117 (v0.17.0: 113); verify mode reports no `weavec.proven`. |
| G5 No regression | **Fails on zstd** (darwin-arm64, `scripts/corpus-gate.py --full --no-asan`): every config builds in both modes and passes its suite with no trap but zstd, whose dictionary trainer stops (*Accepted false traps*; its report build passes with that one report). LZ4 and xz, which stopped before amendments 12 and 13, pass. 33 of the 39 injections stop; v0.17.0 misses the same six. The gate's absolute build- and run-time limits fail (Lua runs 2.64 times the reference, zlib builds in 2.18); G7's and G8's ratios put v0.17.0 over them too (Lua 2.3, zlib at least 1.9), and v0.17.0's full gate was not rerun. Guarding more library calls lowered jansson's and zlib's proven share by 0.2%; the ratchet is recorded again. The weekly Linux run not measured. |
| G6 Untuned programs | **In part**: Redis 8.10.2 (the corpus's revision) builds with `CC=weavec-cc` and passes its whole suite (`./runtest --clients 4`: 7,210 tests, "All tests passed without errors"). CPython, SQLite and coreutils **not measured** (their sources are not on the machine). |
| G7 Run | **Pass**: geometric mean 1.721 the reference (v0.17.0: 1.634), 1.05 times v0.17.0; the worst, brotli and zlib-ng at 1.15 and Lua at 1.14 times their v0.17.0 ratios. Instructions 2.38 (2.14). |
| G8 Build | **Fails per project**: LMDB 1.20, brotli 1.19, Lua 1.14, libdeflate 1.14, zlib-ng 1.13 times v0.17.0's build (all 13 together 1.08). The color checks add IR to every guard with a root; merging their cold path with the guard's cut Lua's `lvm.c` from 1.49 to 1.32 times v0.17.0's compile instructions. `char a[1L << 40]` and `alloca((size_t)1 << 40)` compile in 47–100 MiB (v0.17.0: past 1.5 GiB). SQLite's `__DATA` is 41 KB (v0.17.0: 732; `__const` 19 KB from 708). |
| G9 Memory | **Pass**: the most above v0.17.0 is curl (87 MiB against 32) and Janet (103 against 59), under the 128 MiB allowance; the rest within 1.2 times. |
| G10 Pauses | **Pass**: longest sweep 19 ms at 256 MiB and 71 ms at 1 GiB (densities 0, 25, 100%; v0.17.0: 72 and 289); a 1 GiB heap with dangling words finishes in 7.1 s with sweeps 20% of it (v0.17.0: killed at 300 s); flat-out churn at 256 MiB, sweeps 17% of the run. |
| G11 Threads | **In part**: thread-local small allocations reach 63–66 million a second at 8 threads (v0.17.0: 5.3; ASan 26.4), 2.2 times the single-thread rate, not 3 (the M3's last four cores are efficiency cores; 4 threads reach 82 million); producer and consumer at 8 threads 55 million (v0.17.0: 5.3). A 256-byte class at 3 GiB live replaces 7.3 million blocks a second, as at 1 GiB (v0.17.0: 6 a second at 2 GiB). Single-threaded throughput is 28.6–30.5 million, 0.80–0.86 of v0.17.0's, unexplained but for amendment 11's 6%. |
| G12 Servers | **In part**: Redis 8.10.2 at ~1 GiB: `SET` 216,000 and `GET` 224–226,000 requests a second, 0.86 and 0.88 times the reference's (250,000 and 255,000), short of 0.9; the longest request 60–62 ms (v0.17.0: 149–162; the limit 60), 68–70 ms during `BGSAVE` (limit 75); peak memory 1,736 MiB as v0.17.0. The 1 kHz timer whose handler allocates runs 60 s (59,320 handler calls). |
| H1 | **Pass**: library 7,912 lines of 9,000, runtime 6,918 of 7,000. |
| H2 | **Pass but for the status**: the documentation describes the change (README, guarantees, CLI, compatibility, troubleshooting, architecture, roadmap, AGENTS.md); `npm test` and `npm run build` in `docs/` and the documentation examples pass; `ctest --preset dev` passes; RFC 0036 is marked Implemented; this RFC stays Accepted. |

## Drawbacks

- **Colors are probabilistic** for objects that are not neighbors, and
  an attacker who knows the layout can choose a collision. They raise the
  bar for jumps; they do not close it.
- **New false traps** on arithmetic between objects, which C leaves
  undefined and some code does anyway.
- **Threads the program did not create.** The mark workers appear in the
  process's thread list.
- **Memory.** The quarantine's default grows fourfold; thread caches hold
  up to 64 KiB per class per thread.
- **The runtime grows** by about a fifth, and the line budget with it.
- **Use after return stays partial**: a later call at the same depth hides
  it, where ASan's fake stack does not.

## Alternatives

### Cutting the guards' cost (option C)

Measured on v0.17.0 against the same runtime (instructions and cycles,
geometric mean of 13 benchmarks, ratio to the build without the change):

| Change | Instructions | Cycles | Gives up |
| --- | --- | --- | --- |
| Followers test the root's shadow word | 0.99 | 1.009 | nothing |
| Guards survive calls known not to free | 1.00 | 1.001 | nothing beyond rule 6.2's |
| A lifetime end kills only guards that may name the local | 1.00 | 1.000 | nothing |
| A dominating null test removes later ones | 1.00 | 1.001 | nothing |
| A constant shadow base and mask | 0.99 | 0.993 | the unguarded fallback; sweeps 80% slower (unexplained) |
| No scope writes for locals that do not escape | 1.00 | 1.006 | nothing |
| All of them | 0.97 | 0.990 | |
| Guards inserted after the optimizer (ASan's position) | 0.93 | 0.970 | guards on accesses the optimizer deleted, merged or moved; rule 6.4 |
| Partial granules bounded, not exact | 0.992 | 0.994 | the tail byte's precision (A) |

The guard is already three instructions on AArch64, and 56–69% of executed
guards have no redundancy; a rule that removed 8% of Lua's instructions
saved none of its cycles. Late insertion is the only measured gain and is a
decision about "every access", not an optimization; dropping the ASan
contract gains 2%, almost all of it QuickJS's, and loses its detection.

### Spatial designs (option B's)

Measured against v0.17.0 (cycles, geometric mean; worst benchmark):

| Design | Cycles | Worst | Catches |
| --- | --- | --- | --- |
| The encoding alone | 1.022 | Lua 1.137 | |
| Colors (§8), returned frames (§9), field limits (§10) | 1.066 | zlib-ng 1.157 | 15 of 20 jumps, 7 of 10 uses after return |
| Colors on every root (no selectivity) | 1.061 | | |
| Low-fat slot masks (heap only) | 1.078 | | heap jumps exactly |
| Returned frames alone | 1.031 | | 7 of 10 |
| Escaping locals in arena blocks | 1.487 | cmark 3.64 | 10 of 10 |
| Escaping locals on a fake stack | 1.346 | QuickJS 3.38 (peak memory 1.55 times) | 10 of 10 |
| Colors with escape checks | +7.8% over colors | | the moved pointers of §8.4 |
| Field limits for `memcpy` too | 1.021 | | false traps on copies over fields |

Low-fat masks are exact but heap-only and need power-of-two slots; arena
and fake-stack frames are RFC 0036's first draft's cost again.

### Concurrent marking

A mark in a forked child that holds the parent's memory copy-on-write
(300–500 lines; a visible child process), or one that tracks pages the
program writes while it marks (600–900 lines, high risk). Parallel marking
(150–200 lines) is expected to cut pauses about threefold first; the gates
say whether that is enough.

### Others

- **Do only A.** Leaves the README's two gaps for another milestone; B is
  measured, cheap, and catches what neither v0.17.0 nor ASan catches.
- **A larger quarantine only** (128 MiB): 3.6% faster, at 1.67 times the
  reference's peak memory.
- **Coloring large blocks**: costs a sixteenth of each block in shadow
  pages that are mapped afresh today; the slow path's slot lookup decides
  within a block anyway.

## Prior art

- **HWASan and Arm MTE**: a tag per granule compared with a tag in the
  pointer (8 bits, or 4 with MTE), with neighbors given different tags.
  Colors keep the tag in the shadow at the root, not in the pointer, so
  they need no pointer bits and no hardware, and lose what a moved pointer
  carries.
- **AddressSanitizer**: the fake stack (`detect_stack_use_after_return`),
  the `SEGV` report, and interceptors for the library calls of §5.
- **Boehm–Demers–Weiser GC**: parallel marking, and pushing every
  register (vector ones too) and every thread-specific value as roots.
- **tcmalloc, jemalloc and mimalloc**: per-thread caches over shared
  size-class lists, and padded per-class state.
- **MarkUs and MineSweeper**: concurrent marking for quarantined memory.

## Unresolved questions

- **Why a 1 GiB heap with dangling words did not finish**: the
  per-allocation sweep of a full class (§2.3) is the likely cause; S2
  measures it.
- **The quarantine's default**: 64 MiB is chosen between two measured
  points; G7 and G9 decide whether 128 MiB is better.
- **Colors on untuned programs**: the false-trap probes found none but
  `p + (q - p)`; the corpus found biased window bases (LZ4, zstd;
  amendment 12) and zstd's indexing of one block from another's start,
  kept as an accepted false trap. G6 is the first measurement on large
  programs WeaveC was never run on. Its sources are not in the repository
  and have to be fetched.
- **Linux**: nothing in this RFC's motivation was measured there; the
  color check's cost on x86-64 is reasoned (one compare more than the
  sign test), not measured.
- **The rest of the run time**: with every mechanism switched off the
  benchmarks still run at 1.23–1.27 times the reference, and the system
  allocator is slower in a WeaveC link than in the reference's; neither is
  explained.

## Future work

- Concurrent marking, if parallel marking misses G10 or G12.
- Pointers checked where they leave a function (§8.4), and colors carried
  by a moved pointer.
- Hardware tags (Arm MTE, Apple MIE) where the processor has them.
- Adoption: relocatable installs, packages, a vendored `weavec.h`.
