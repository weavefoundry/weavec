# RFC 0055: A heap for safe code

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-24
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends [RFC 0045](0045-type-stable-memory.md)
  §3 (the pools' path, and what a `realloc` does to a block's type),
  [RFC 0043](0043-ownership.md) §7 (the reference test is the checked
  build's, in Safe C as in Safe C++ since
  [RFC 0048](0048-safe-code-without-the-shadow.md) §2.1) and
  [RFC 0050](0050-checked-once.md) §3 (a pooled slot keeps its shadow in a
  program with Safe C too; the short paths are replaced). The rules, the
  guards of code that is not safe and the checked build
  (`-fweavec-safe-checks=full`) are unchanged.

## Summary

A program an agent writes allocates all the time: a string, a node, a
vector's buffer, a map's entry. Measured today, the fast build of an
all-safe program spends a third (C++) to two thirds (Safe C) of its run
time in WeaveC's allocator, and that is nearly all of what it costs over
Clang: the checks Rust also makes (an index, a zeroed local, arithmetic
that wraps) cost the same sources 0 to 2%.

It does not have to cost anything. Safe code's allocations have a type the
compiler knows, a size that is often a constant, and releases the rules
have already checked. A model of the path those facts allow (a per-thread
array of free slots for each pool, one byte of state for a slot, the slot
zeroed when it is released) runs the same four agent-written programs at
0.71 to 0.95 times Clang's build of them, where the fast build runs them
at 1.21 to 1.93. The Safe C port of Python's `json`, which
[RFC 0054](0054-one-of-several-types.md) measured at 3.0 times Rust's
port, runs in the model at 1.1 times it.

This RFC builds that path, and makes the heap of a fast safe unit one
design for both languages:

- **Every block keeps its type.** A `realloc` gives a block of the old
  block's type, from that type's pool. Today it gives a block of no type,
  which leaves through the quarantine and a sweep: every buffer and array a
  C program grows.
- **One contract for Safe C and Safe C++.** The test of a reference when
  it is loaded (O4) is the checked build's in both languages. In the fast
  build it already promises nothing once a slot is reused, and keeping it
  makes every release and every allocation of a program with one Safe C
  function write the shadow.
- **A short path.** A thread keeps an entry for each pool it uses. A take
  pops a slot and stores one word; a give compares that word, stores it,
  zeroes the object and pushes. No compare-and-swap, no stack or release
  record, no shadow, no counters.
- **In line.** Where the size is a constant (`new T`, `malloc(sizeof *p)`,
  a sized `delete`), the compiler writes the pop and the push into the
  function, and calls the runtime only for an empty or a full entry.

Nothing an agent writes changes, and no rule does. What it gets is the
other half of Rust's offer: the program that compiled is as fast as the
language it is written in, and here somewhat faster than the same source
under Clang, because a `malloc` the compiler cannot see costs more than a
pool it can.

## Motivation

### Where a fast safe program's time goes

Four programs agents wrote under earlier RFCs' gates, all of their
functions safe: RFC 0049's and RFC 0044's C++ ports of Python's `json`,
RFC 0054's Safe C port of it, and RFC 0054's trial interpreter. Each was
built three ways: by Clang at `-O2`; by Clang with the checks a Rust build
also makes (libc++'s hardening `fast`, zeroed locals, `-fwrapv`: the
_floor_); and by main's Release compiler, fast. Medians of nine interleaved
runs (`build/rfc55/s0/`, darwin-arm64, main at `13d6774d`):

| Program | Clang | Floor | Fast build |
| --- | ---: | ---: | ---: |
| `json`, C++ (RFC 0049 G7) | 683 ms | 692 ms (1.01) | 823 ms (1.21) |
| `json`, C++ (RFC 0044) | 462 ms | 466 ms (1.01) | 628 ms (1.36) |
| `json`, Safe C (RFC 0054 G4) | 677 ms | 679 ms (1.00) | 1,307 ms (1.93) |
| interpreter, C++ (RFC 0054 G3) | 1,143 ms | 1,135 ms (0.99) | 1,695 ms (1.48) |

The floor is free. What the fast build adds is WeaveC's own, and a sampled
profile of each run says where (`s0/samples/`, samples grouped by image):

| Program | Fast build: in the runtime | Clang's build: in the system allocator |
| --- | ---: | ---: |
| `json`, C++ (RFC 0049 G7) | 34% | 31% |
| `json`, Safe C | 65% | 46% |
| interpreter, C++ | 15%, and 9% in `bzero` and `memset`, which the model's run below does not show | 13% |

For the C++ port that is about 280 ms in WeaveC's runtime where Clang's
build spends about 210 ms in the system's. For the Safe C port it is most
of the run. The runtime's functions in those samples are the typed path of
RFC 0050 (`weavecRtReleaseForm`, `weavecRtAllocTyped`), the shadow
(`shadowLive`, `shadowDead`, `weavecRtShadowObject`), the zero fill, and,
in the Safe C port only, the sweep's marking (408 of 1,880 samples).

### Why the path costs what it does

Read from `runtime/weavec_alloc.c` at `13d6774d`:

- **A release is a compare-and-swap and two record stores.** `claim()`
  takes the slot's word atomically, "so a second release of it, even one
  racing this one, is a double free", and writes the release record's stack
  and size, in a fast program that records no stacks.
- **A take stores to three tables** (the word, the allocation stack's
  entry, the live and pooled counters), raises and lowers a depth around
  two signal fences, and reads the pool through a cache of 64 entries
  indexed by the pool's number modulo 64, so two hot pools that collide
  take the locked path every time.
- **It is a call into a shared library**, then two calls inside it, for
  what is a pop.
- **A program with Safe C writes the shadow at every release and every
  take.** `keepsShadow()` is false in the whole program once one unit has a
  Safe C function (RFC 0050 amendment 7), because Safe C's reference test
  reads a released block's shadow. The same program's C++ pays it too.
- **A `realloc` that moves gives a block of no type.** The Safe C port has
  three `realloc` sites: its text buffer, an array's items and an object's
  members. Every block they return leaves through the quarantine, and the
  quarantine fills and is swept. That is the marking in its profile, and
  the stops of the world it does not show.

RFC 0050 measured a typed allocation and its release at 18.7 ns a pair
against the system allocator's 17.0 on Darwin, and 18.2 against glibc's
10.5. Its closing paragraph names "the allocator's path, which is a call
through two layers and a compare-and-swap where glibc's is a few loads" as
what is left for new code. Nothing since has touched it.

### What the facts safe code has would allow

A model, not WeaveC code (`s0/model2/`): the two C++ ports' `operator new`
and `operator delete` and the C port's `malloc`, `calloc`, `realloc` and
`free` on per-thread arrays of free slots by size class. The arrays are
apart from the slots, so that a stale write cannot reach the allocator's
state; a side table has one byte per slot that says live or released, and
a second release aborts; a slot is zeroed whole when it is released. The
sources are built at the floor, with the path called and with it in line:

| Program | Clang | Fast build today | Model, called | Model, in line | Rust's port |
| --- | ---: | ---: | ---: | ---: | ---: |
| `json`, C++ (RFC 0049 G7) | 683 ms | 823 ms (1.21) | 528 ms (0.77) | 484 ms (0.71) | |
| `json`, C++ (RFC 0044) | 462 ms | 628 ms (1.36) | 391 ms (0.85) | 380 ms (0.82) | 435 ms (0.94) |
| `json`, Safe C | 677 ms | 1,307 ms (1.93) | 484 ms (0.72) | 477 ms (0.71) | 434 ms (0.64) |
| interpreter, C++ | 1,143 ms | 1,695 ms (1.48) | 1,081 ms (0.95) | 1,030 ms (0.90) | |

The control, the floor built with link-time optimization and no pools, is
within 1% of Clang on all four, so the gain is the path and not the
inlining of anything else. The interpreter _as its agent first wrote it_,
before any rule, runs in 1,025 ms under Clang: the model puts the safe
program at the speed of the program as written.

The model is an upper bound. Its pools are by size and not by type, it
never returns memory, it sends blocks over a kibibyte to the system, it has
no byte past an object, and it runs one thread. The build will not reach
it. It does not have to: the distance from 1.93 to 0.71 has room.

### Why this, of what is left

Three candidates were probed before this one was chosen.

- **Other people's code.** A safe unit that includes a third-party header
  library as a system header compiles, and the rules still follow what the
  library's classes own: a reference into a buffer class of the library,
  used after its `append`, is a `stale-borrow` error
  (`s0/probe/app.cpp`). Dependencies are not where an agent's rewrite
  stops.
- **The holes that are left.** Twelve from RFC 0053's round and four from
  RFC 0054's are open, and Safe C has no thread rules. Each is a design of
  its own and each is rare in what agents write; the runs that found them
  were adversarial. None changes a measurement a person choosing a
  language makes.
- **The loop.** RFC 0050 measured it level with Rust's for new code, and
  RFC 0054 kept it there with casts and tagged unions allowed.

Cost is the measurement that still says "Rust". RFC 0050's five cost gates
are open, RFC 0054's G4 (new C at 1.5 times Rust) failed at 3.0, and the
owner kept both as written. A person picks C or C++ over Rust for speed or
for the code they already have. A safe build a fifth to nine tenths slower
than Clang answers the first with "not here", whatever the rules do. And
the cause is one mechanism with one fix, whose bound is measured.

## Soundness

### What is guaranteed that was

Everything (Y1) to (Y6) say of the fast build, for both languages:

- A slot is handed out again only for an allocation of its type and class,
  zeroed, with its first element at the same address (Y1).
- A pointer to a heap block points, as long as it exists, at the live
  block, the released block zeroed, or another block of the same type on
  the same element boundaries (Y2).
- A slot leaves its pool only through the quarantine and a sweep that
  finds no pointer to it.
- A second release of a block in one thread stops the program
  (`double-free`), in the fast build as in the checked one.
- The allocator's own state is in no slot: a stale pointer that writes
  into a released slot writes zeroed bytes of its type's next object and
  nothing the allocator reads as a pointer.
- The checked build is unchanged: every guard, every release through the
  quarantine, the reference test, allocation and release stacks.

### What gets stronger

- **A block does not lose its type when it grows.** Today a `realloc` that
  moves returns a block of no type, which RFC 0054 (D5) then lets take the
  type of its first checked conversion, whatever the old block held. With
  (D3) the block is its old type's, and a conversion to another type stops
  (`bad-cast`).
- **Fewer releases go through the quarantine**, so fewer pointers of a
  safe program rest on what a conservative sweep can see.

### What gets weaker, in plain terms

- **(O4) is the checked build's.** A fast Safe C unit no longer tests a
  loaded reference against the shadow. The guarantees page already says of
  the fast build that "(O4)'s test passes for a block that was reused"; a
  typed block is reused at once, so the test was a stop only in the window
  before the next allocation of that type. What is lost is that window: a
  reference to a released slot that has not been reused reads zeroes where
  it stopped. Safe C++ has been so since RFC 0048.
- **Code that is not safe reads a released slot as the zeroed object it
  held**, in a program with Safe C as in one without (RFC 0050 (D3)), until
  the slot is reused.
- **Two threads that release one block at the same instant** are found in
  the checked build and, if S0 (b) drops the compare-and-swap, not always
  in the fast one: both may push the slot, and it is then handed out twice,
  to two objects of its type. Safe C++'s thread rules reject the program
  (`thread-send`, `thread-share`); Safe C has no thread rules and its
  functions may not start a thread (O-A3). The slot stays memory of its
  type either way (Y2).
- **A fast program keeps no release record.** A report about a pooled
  block says which type's pool it is in and that the program was built for
  speed; `WEAVEC_RT_HEAP_STACKS=1` asks for the stacks, and turns the short
  path off.

### Bugs caught

```c
struct node { struct node *next; int v; };
void twice(struct node *WEAVEC_OWNED n) { free(n); }   // called by guarded code, twice
```

`double-free` in both builds: the word of the slot says released.

```c
struct a { char *WEAVEC_OWNED name; };
struct b { long n; };
struct a *as = calloc(4, sizeof *as);
void *moved = realloc(as, 64 * sizeof *as);   // a block of struct a, from its pool
struct b *bs = moved;                          // bad-cast: there is a struct a here
```

On main `moved` has no type, the conversion gives it `struct b`, and
`bs[0].n = 1` writes an integer where a stale `as[0].name` reads a pointer.

### Bugs deliberately not caught

- A stale pointer that reaches a reused slot reads the object that lives
  there (Y2), as now.
- An allocation whose result stays `void *`, or that goes through a
  pointer to an allocator (cJSON's hooks), has no type: it takes today's
  path and the audit lists it, as now.
- What code that is not safe does to a block.

### Accepted rejections and stops

- A block allocated as bytes (`char *b = malloc(n)`) and, after a
  `realloc`, converted to a struct that holds pointers stops (`bad-cast`),
  where on main the move forgot that it was bytes. Allocate it as the
  struct. A view of bytes without pointers is still given (S4).
- `WEAVEC_RT_STATS=1`, `WEAVEC_RT_HEAP_STACKS=1` and a verify unit turn the
  short path off for the run; the counters, the stacks and the monitors
  are then exact, and the program is as slow as it is today.

### Assumptions

- **H-A1.** Code that is not safe does not run a safe function as a signal
  handler while that thread is inside a take or a give. Safe code cannot
  register a handler (`signal-handler`, RFC 0051), and a handler of code
  that is not safe allocates through `malloc`, which has no pool, and
  releases through the quarantine (Y1).
- T-A1, O-A1 and Y-A1 to Y-A5 as they are.

## Detailed design

### 1. The contract of the path

A take or a give in a fast safe unit keeps exactly these, and does nothing
else:

- **H1. By type.** A slot comes from, and returns to, the pool of its
  type and class (RFC 0045 §3). A slot's entry in the pool table and in the
  type map (RFC 0054 §1) is written when the slot first joins the pool and
  not again.
- **H2. Zeroed.** A slot is zeroed over its object and the byte past it
  when it is released, so a take writes nothing into it.
- **H3. One word.** A slot's word says live with its size and form, or
  released. A take stores it; a give compares it with the live word the
  site expects and stores the released one. A word that does not compare is
  the slow path, which reports `double-free`, `invalid-free` or a release
  of the wrong form as today.
- **H4. Apart.** The free slots of a pool are named in arrays that are not
  in the heap's slots.
- **H5. The shadow is kept** (RFC 0050 (D3)): a slot keeps the shadow of
  the object it held, and a take whose size is the same writes none. A
  slot whose last object had another size takes the slow path once.
- **H6. Counted in bulk.** Live and pooled bytes are added when slots move
  between a thread's entry and the pool's shared list, not at each take.

### 2. One contract for both languages (D2)

- `testReferences` (`lib/Frontend/GuardSafe.cpp`) runs for the checked
  build and for verify units only, in C as in C++. A fast Safe C unit
  believes a loaded reference as it believes a loaded pointer's bound
  (RFC 0042): by (Y2) it reaches memory of its type.
- `prepareFast` (`GuardFast.cpp`) announces `__weavec_rt_tests_released`
  for a checked or a verify unit only. A program of fast units keeps the
  shadow of its pooled slots whichever language they are in. A program
  with one checked unit marks every pooled slot released, as now.
- The guarantees page states (O4) for the checked build, and (Y1) loses
  its sentence about programs with Safe C.

### 3. Every block keeps its type (D3)

- `realloc` in a fast safe unit compiles to
  `__weavec_rt_realloc_typed(p, size, record, site)`, with the record of
  the type Clang infers for the call, as `malloc` has.
- **The old block's type wins.** If `p` is in a pool, the new block comes
  from the pool of that key and the new size's class; the bytes are moved,
  the tail is already zero (H2), and the old slot is given back. If the
  class is the same the block grows or shrinks in place, as now.
- If `p` is null, or a block of no type, the new block has the record's
  type; if there is no record either, today's path.
- The site's record never changes a typed block's type. A program that
  converts the result to another type meets RFC 0054's check.
- The ledger's reason `reallocated` goes.

### 4. A thread's entries

The thread cache's 64 direct-mapped pool caches of 64 slots are replaced:

- A thread has an **entry** for each pool, at the pool's number: a count,
  a capacity and a pointer to an array of slots. The table of entries is
  reserved whole (4,096 pools of 16 bytes) and a pool's array is mapped
  when the thread first takes from or gives to that pool, so an entry's
  address is the cache's plus a constant times the pool's number, with no
  comparison of which pool it holds.
- An array's element is what a take needs with no arithmetic: the slot's
  address and the address of its word. A third field, as today's
  `withShadow`, is the size of the object the slot last held.
- An entry with count 0 is empty and one with count equal to capacity is
  full. An entry that is **off** has both zero. The runtime turns every
  entry of every thread off, and keeps new ones off, whenever
  `shortPaths()` is false (a checked or a verify unit in the program, heap
  stacks, counters), so the path needs no flag: an off entry is empty to a
  take and full to a give, and both go to the slow path, which is today's.
- Refill and flush move half an array between the entry and the pool's
  shared list under the pool's lock, and account for the bytes (H6) and
  the budget of `givePooled` there.
- A thread's exit flushes its entries to the shared lists, as now.

### 5. The short path

`poolTake` and `poolGive` become, for a pool whose number the site's
record holds:

- **take**: find the entry; if count is 0, slow. Pop. If the element's
  last size is not this size, slow (H5). Store the live word. Return the
  slot.
- **give**: find the slot's word (from the address; with the class known
  it is a shift, a multiplication and a table's base). If it is not the
  live word of this size and form, slow. Store the released word. Zero the
  object (H2). Find the entry from the slot's pool; if it is full, slow.
  Push.

What they no longer do: the compare-and-swap (unless S0 (b) keeps it), the
allocation stack's and the release record's stores, the two counters, the
depth and its fences. The depth protects a thread's cache from a signal
handler that allocates in the middle of a take; by H-A1 no handler reaches
an entry, and the slow paths keep it.

A sweep that stops a thread inside either sequence finds a slot that is
zeroed or being zeroed, whose word says released or about to be live, and
whose address is in a register. S0 (a) confirms the sweep leaves such a
slot alone: it reads pooled slots as roots and does not reclaim them.

### 6. In line

Where the allocation's size is a constant and its record is known
(`new T`, `std::make_unique<T>`, `malloc(sizeof *p)`, `calloc(1, sizeof
*p)`), `typeAllocations` (`GuardFast.cpp`) writes the take into the
function instead of the call:

```text
index  = load the record's pool number          ; 0 until the site first runs
entry  = the thread's entries + 16 * index
count  = load entry.count
if count == 0                 -> p = __weavec_rt_take(size, record, flags)
slot   = entry.slots[count - 1]
if slot.size != SIZE          -> p = __weavec_rt_take(size, record, flags)
store entry.count = count - 1
store *slot.word = LIVE(SIZE, form)
p      = slot.address
```

A site that has not run has pool number 0, and entry 0 is always off.

Where a release's size is a constant (a sized `operator delete` of a
complete type whose destructor is not virtual, `free` of a pointer whose
allocation in the same function had a constant size), the give is written
the same way, ending in a call to `__weavec_rt_give` when the word does
not compare or the entry is full. Every other release (`free` of a pointer
of unknown extent, `deallocate` with a count, a virtual destructor's
`delete`) calls `__weavec_rt_give`, which is §5's sequence in the runtime.

The thread's entries are found as the runtime finds its cache: Darwin's
reserved thread-specific slot read from the thread register, Linux's
initial-exec thread-local, which the runtime's library exports. Both
become part of the ABI.

The in-line forms are emitted only if S0 (c) shows they pay (D4); the
short path of §5 does not depend on them.

### 7. What changes elsewhere

- **Runtime ABI 50**: `__weavec_rt_take`, `__weavec_rt_give`,
  `__weavec_rt_realloc_typed`, the exported entries; `weavec_rt.exports`
  and `weavec_rt.h` with them. `__weavec_rt_new_typed`,
  `__weavec_rt_alloc_typed` and `__weavec_rt_calloc_typed` stay for sizes
  that are not constants and call the same short path.
- **Ledger 15**: a unit's summary counts its allocation and release sites
  in line, called and not typed; the summary line says so. The audit's
  list of allocations a fast unit could not type is unchanged.
- **`rt_test`**: entries on and off, a take and a give across threads, a
  refill and a flush, a second release, a release of another form, a
  `realloc` that keeps its type, a sweep with a thread stopped in each
  sequence.
- **Pages**: `docs/pages/reference/guarantees.md` ((O4), (Y1), the
  paragraph "what gets weaker"), the guides' "Fast and checked builds",
  the command-line reference's variables, `docs/architecture.md`,
  `AGENTS.md`.

### 8. What does not change

The rules. The guards of code that is not safe, its `malloc` and `free`,
the quarantine and the sweep. The checked build. The byte past an object
(a pointer one past the end must stay in its slot for the sweep). Pools'
keys and classes. Blocks over the cached classes (32 KiB). The type map
and RFC 0054's checks.

### 9. Cleanups and breaking changes

- The direct-mapped pool caches, `lookPool`'s probe on the hot path, and
  the per-site cache word's class field go; a site's record holds a pool
  number.
- `claim()`'s release record is written by the slow path only.
- Safe C's reference test in the fast build goes, and with it the
  program-wide switch a Safe C unit set.
- A program that relied on a fast Safe C build stopping at a stale
  reference before the slot's reuse must test with the checked build,
  which is what the guides already say to do.
- A program that defines its own `malloc` keeps today's behavior: its C
  allocations have no pools.

### 10. Stages and gates

**S0, measured first (D6).** On a branch, before anything else:

- (a) (D2) and (D3) alone, in the runtime and the two passes: the Safe C
  port against Clang, and how many of its samples are the sweep's.
- (b) §4 and §5 in the runtime behind today's entry points, with the
  compare-and-swap and without: the pairs benchmark and the four programs.
- (c) §6 for takes and sized gives: the four programs again.

Limits: after (b), the Safe C port at most 1.25 times Clang (1.93 today),
RFC 0049's C++ port at most 1.05 (1.21), a pair at most 12 ns on Darwin
(18.7). The compare-and-swap stays if dropping it gains less than 1% on
every program. (c) ships if it gains 2% on two of the four. A limit missed
goes back to the owner before S1.

**S1** the entries and the short path. **S2** `realloc` and the Safe C
contract. **S3** the in-line forms, if (c) passed. **S4** the ledger, the
audit, the tests, the pages.

**Gates.** Release compilers, medians of nine interleaved runs, against
Clang's `-O2` build of the same source unless said.

| Gate | Asks |
| --- | --- |
| G1 Tests | `test/Guards/ir-pools.c` and `ir-pools.cpp` pin the in-line take and give and their calls; `typed-reuse.c` and `typed-reuse.cpp` the reuse, the kept shadow and the checked build's stop; `realloc-typed.c` the type a moved block keeps and the `bad-cast`; `reference-load.c` the test in the checked build and its absence in the fast one; `rt_test` as §7 lists. |
| G2 New code | RFC 0049's `json` port at most 1.00 (1.21 today); RFC 0044's at most 1.10 (1.36); the Safe C port at most 1.00 (1.93); the LRU port at most 1.15 (1.25); the interpreter at most 1.30 (1.48), and at most 1.45 of the program as written (1.65). |
| G3 Against Rust | The Safe C `json` port at most 1.5 times Rust's port (3.0 today; RFC 0054's G4, which then passes); RFC 0044's C++ port at most 1.15 times it (1.44). |
| G4 The path | A typed allocation and its release, in pairs: at most 0.6 of the system allocator's time on Darwin (1.10 today) and 1.0 of glibc's on Linux (1.73). |
| G5 Conversions and Safe C | tinyxml2, yaml-cpp, cJSON, sds, inih, jsmn and tinyexpr: none slower than main by more than 1%; each reported against RFC 0050's asks. |
| G6 Nothing believed is wrong | Under `-fweavec-checks=verify`, no `weavec.proven` on the ports' test vectors and tinyxml2's checks; the checked build stops RFC 0044's seeded bugs as before; every `double-free` case of `test/cases/` stops in the fast build. |
| G7 Memory | Peak resident memory of each G2 program at most main's. |
| G8 Threads | RFC 0046's indexer port not slower than main; `rt_test`'s cross-thread takes and gives pass 1,000 runs on both platforms. |
| G9 Nothing elsewhere | Guarded compiles of `test/cases/` and `test/Guards/` give main's objects but for the ABI's immediate; a guarded Lua and the checked builds of the G2 programs within 1.01 of main; compile time within 1.01. |
| G10 A round | A fresh agent tries to make a pooled slot hold two types, to have one thread's entries name a slot twice, or to reach the entries from a slot. None stands in the fast build; each found is closed and pinned. |

Expected size: about 450 lines of code in the runtime (most of it replacing
the pool caches), 350 in `GuardFast.cpp` and `GuardLibrary.cpp`.

## Annotation surface

None.

## Diagnostics

No new compile-time error and no new report kind. `double-free`,
`invalid-free` and `bad-cast` keep their lines. A heap report about a
pooled block in a fast program already says the program was built for
speed and has no release stack; it now says so for a block Safe C
released too.

## Drawbacks

- **The runtime's layout becomes the compiler's.** The entries, an
  element's fields and a live word's encoding are written into every fast
  unit, as the shadow's base and encoding already are. A change to any of
  them is an ABI change.
- **Per-thread memory.** A table of entries is 64 KiB of address space a
  thread, touched a page at a time, and a pool's array is mapped per
  thread that uses it. A program with thousands of threads and thousands
  of pools pays for it.
- **Safe C gives up a stop it had.** The window in which a fast Safe C
  build stopped at a stale reference is gone. It was never a guarantee,
  and it caught real mistakes in tests run with the fast build.
- **A benchmark that allocates little gains little.** tinyxml2 allocates
  from its own arena and the interpreter's gap is two thirds elsewhere
  (its hash lookups and moves in library clones). This RFC does not close
  those.
- **It makes a comparison easy to overstate.** A fast build that beats
  Clang on an allocation-heavy program beats Clang's _default allocator_.
  The same program under Clang with a pooled allocator would be level. The
  pages must say that the safe build costs nothing, not that it is faster
  than C++.

## Alternatives

- **Link a faster general allocator under the arena.** It does not know
  types, so pools by type would sit on top of it as they do now, and the
  cost is in that layer.
- **Keep the path in the runtime and only shorten it.** That is S0 (b),
  and the model says it is most of the gain (0.72 to 0.95 called, 0.71 to
  0.90 in line). The in-line forms are a separate stage for that reason,
  kept only if measured.
- **Keep Safe C's reference test in the fast build and pay the shadow for
  Safe C's own blocks only.** The switch is program-wide because a
  reference may point to a block C++ released. A per-slot choice needs
  the releasing site to know who may hold a reference, which nothing
  knows.
- **Give `realloc`'s result the type the site infers.** That lets a
  program change a block's type by growing it, which is the hole (D3)
  closes.
- **Drop the byte past an object in pooled slots**, so a 16-byte object
  takes 16 bytes. A pointer one past the end of slot _i_ would then point
  into slot _i_ + 1 and keep that one alive in place of its own; after a
  sweep, slot _i_ could be reused by another type while the end pointer
  exists, and an iterator stepped back from it is guarded against the
  shadow of whatever is there. Not without another design for ends.
- **Remove the zeroing.** Safe code relies on a fresh block's bytes
  (RFC 0051 amendment 18) and on a released slot reading as an empty
  object.
- **Do nothing about cost and close the open holes first.** They are
  real and they are next. None of them is why a measured comparison comes
  out for Rust today.

## Prior art

- **OCaml, GHC, HotSpot's thread-local allocation buffers**: a compiler
  that owns its allocator writes the allocation into the function. It is
  why allocation is cheap in those languages and not in C. We take the
  idea and keep C's explicit release.
- **Rust's `GlobalAlloc`**: `dealloc` is given the layout, so a release
  knows its size class without a lookup. C++14's sized deallocation gives
  the same, and §6 uses it.
- **mimalloc and tcmalloc**: per-thread free lists by size class with no
  atomic operation on the path, and a slow path that moves slots in bulk.
  mimalloc keeps its links in the free blocks; H4 does not, because a
  stale pointer of safe code may write there.
- **`kalloc_type`, typed `xzone` malloc, PartitionAlloc, LLVM's allocation
  tokens**: isolation of the heap by type, which RFC 0045 built on. Their
  paths are out of line; none has the compiler write the pop.
- **Cyclone's regions, and arenas in general**: faster still,
  with a lifetime the program must state. Out of scope: the release stays
  where the program wrote it.

## Unresolved questions

1. **How a give finds the slot's word in line** (§5, §6): by arithmetic
   from the address with the class a constant, or by a table indexed by
   address as the model has. S0 (c).
2. **The compare-and-swap** (§5): S0 (b) measures it; Safe C's lack of
   thread rules is the argument for keeping it.
3. **What the depth protects besides handlers** (§5): `releaseDeferred`
   runs when it returns to zero; S0 (a) reads every use before the short
   path drops it.
4. **Entries for 4,096 pools a thread**: a fixed table, or one grown as
   pools are numbered. S1.
5. **Which `free` calls have a constant size** (§6): only those whose
   allocation the function sees, or also a pointer to a complete struct
   that the checker knows is one object (not `WEAVEC_COUNTED_BY`). The
   second is most of Safe C's releases. S3.
6. **Linux's exported thread-local** in a `-static` program and in an
   image loaded with `dlopen`: initial-exec needs the runtime's library
   loaded when the program starts. S3 checks both, and what RFC 0038 says
   of a runtime loaded later.
7. **A slot whose last object was smaller.** Reading today's path for
   this RFC raised a question it did not settle: a release zeroes the
   object and the byte past it, so whether the rest of the slot can hold
   stale bytes that a larger object of the same pool then reads as fresh
   is to be tested in S1 on main's path too, and fixed there if so.

## Future work

- **Safe C's frames and entries.** By the samples, the Safe C port's own
  code is about 90 ms slower in the fast build than under Clang (tracked
  frames, functions split at their entries that no longer inline, frame
  records; the split between them is not measured). RFC 0048
  §10 left its frames for later; with the heap out of the way they are
  what is left of Safe C's cost.
- **The library's clones** (`.weavec.t`): the interpreter's hash lookups.
- **The runtime's start** (3 ms) and **memory** (the byte past an object
  rounds a 16-byte object to 32).
- **The open holes** of RFC 0053 and RFC 0054, and thread rules for Safe
  C.
- **Allocators passed as pointers** (cJSON's hooks): a type for an
  allocation the call does not show.

## Decisions to accept

- **D1. Scope.** The heap path of fast safe units, in C and C++. No rule,
  no annotation, nothing in guarded code or the checked build.
- **D2. The reference test (O4) is the checked build's**, in Safe C as in
  Safe C++; a fast program keeps its pooled slots' shadow whichever
  language released them.
- **D3. A block keeps its type through `realloc`**; the site's inferred
  type is for a null or untyped block only.
- **D4. The path** is §1's contract: per-pool entries, one word a take and
  a give, no record and no counter on it. The in-line forms ship only if
  S0 (c) measures 2% on two programs.
- **D5. The gates compare with Clang's build of the same source and with
  Rust's ports**, at the numbers of §10, and RFC 0050's and RFC 0054's
  open cost gates are measured again with them.
- **D6. Measured first.** S0's limits; one missed returns to the owner
  before S1.
- **D7. Versions.** Runtime ABI 50, ledger 15.

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what S0 and the gates found. They override the body.

1. **S0 (a) and (b) were built and measured together.** (D2) and (D3) were
   not measured alone. After both, no sample of the Safe C port is the
   sweep's marking (408 of 1,880 on main); 1.2% of them are releases of
   untyped blocks into the quarantine (`queueDead`).
2. **A slot whose last object had another size (H5)** is taken on the
   short path, which writes its shadow (`takeShadow`, a tail call), as
   RFC 0050's did. Arrays' and strings' buffers vary in size within a
   class, and sending each of them to the slow path made it the common
   path for them (json49 1.28 instead of 1.16 at that stage).
3. **A record whose cached class is another (one type in many classes,
   a vector's buffer)** finds its pool in the table without the lock on the
   short path (`lookPool`), as RFC 0050's did; 70% of json49's takes missed
   the record's class when they went to the slow path instead.
4. **Refill and flush have slow paths of their own** (`refillEntry`,
   `flushEntry`), entered before the general ones, with the thread's depth
   raised around the pool's lock. A refill that finds the pool's shared list
   empty takes fresh slots of the class by the half entry, which join the
   pool at once (`joinPool`: zeroed whole if used before, word released,
   pool and type written once, H1; the shadow is written at the first
   take, as amendment 2). Taken one at a time through `allocForm`, they
   were 640,042 of json49's 816,633 slow takes: that port's benchmark keeps
   every document until it ends, so its pools grow.
5. **An entry's room and its slots.** An entry holds 128 slots, or as many
   as hold 256 KiB (at least 4), and a refill or a flush moves half. A slot
   in an entry is its address, its word and its eight-byte form in the
   pools' shared lists (the slot's number, with the size of the shadow it
   keeps), 24 bytes; the shared lists keep the eight bytes, as on main. With
   the 24-byte records in the shared lists too, the interpreter's peak
   memory was 6.8% over main's, and json49's refills no faster.
6. **The entries' memory (§4, _Unresolved question_ 4).** One reservation
   of 1,024 blocks, made at the first thread's cache, each a thread's
   entries for 4,096 pools, its slow paths' counts and its slots (12 MiB of
   address space a block, touched a page at a time), registered once as the
   runtime's own so that the sweep reads none of it. A block is mapped
   afresh when its thread ends and reused. A thread that finds none uses a
   shared table of off entries, and its pools' slow paths use the shared
   lists directly.
7. **When the entries are off.** `shortPaths()` is false in a program with
   a checked or a verify unit, heap stacks, counters, or an allocator of its
   own (`weavecRtForeign`: a `malloc`, `free`, `operator new` or `operator
   delete` the program defines, found before its code runs). A thread's
   entries go off at its next slow pool operation after that changes, and
   the announcing thread's at once (`weavecRtSettleEntries`); its slots'
   counts move to `held`, which the slow paths use while the entries are
   off. A checked safe unit of either language announces it
   (`tests_released`), not only one of Safe C.
8. **The depth (§5, _Unresolved question_ 3).** The short paths do not
   raise it or fence, but they read it: a call from a signal handler that
   interrupted the allocator takes the slow path, which defers as before.
   `releaseDeferred` is reached only from the slow paths.
9. **The compare-and-swap goes** (S0 (b), _Unresolved question_ 2):
   dropping it gains 3.3% on RFC 0044's port and 0.7% to 0.9% on the
   others. A second release in one thread finds the word released and stops.
10. **The in-line forms (§6) are not built** (S0 (c), D4). Written in line
    for constant sizes (a take, and a sized give of up to 128 bytes, on
    Darwin), they gained 1.7% on json49, 1.3% on the interpreter and
    nothing on the two others; the take alone gained 0.9% on one. The
    patch is kept with the evidence (`build/rfc55/s0c-with-inline.patch`);
    the runtime exports no thread entry, and the heap descriptor no pool
    table. _Unresolved questions_ 1, 5 and 6 are moot.
11. **The entry points (§7).** `__weavec_rt_take(size, record, how)` serves
    every typed `malloc` and `new`, of any size, and `__weavec_rt_give(p,
    size, how, site)` every release by safe code (`free`, `delete`, sized
    or not, of either form), with the form in `how` and the size all ones
    when it is not given; each is §5's path and otherwise calls the form's
    old entry point (`alloc_typed`, `new_typed`, `free_at`, `delete_at`,
    …), which stay. The library table names them: `typed(take, F)` on the
    allocation rows, `typed(give, F)` on the release rows (a clause a
    release row may now carry) and `typed(realloc_typed, 0)` on `realloc`'s.
    A release and a `realloc` are redirected by GuardLibrary (the late
    pass, for a release), which takes the site's record from the call's
    bundle, and not by `typeAllocations`. A local placed on the heap (RFC
    0048 §5.1) is taken and given through them too.
12. **A `realloc` (§3)** of a block of no pool but a recorded type (one an
    RFC 0054 conversion typed) moves untyped and copies the record.
    `reallocf` and `reallocarray` have no typed form; the ledger's reason
    for them is `no typed form`, and `reallocated` is gone.
13. **Ledger 15** counts the unit's typed releases beside its typed
    allocation sites (`typedReleases`), and the summary line says `<n>
    typed releases`; with no in-line form there is nothing in line to
    count. The audit's list of reference fields reads "tested where checked
    safe code loads them".
14. **Pools are keyed by layout as well as name** (G10). The one way the
    round found to put two types in one pool was two definitions of one
    C++ name in two units that no link compares (one in a dynamic library
    or an archive, which `definition-mismatch` does not see): a stale
    pointer of one read a pointer the other forged from an integer. It is
    so on main too. A fast unit's pool key now mixes in the type's layout
    identity (RFC 0054 §1.1: its name, size and where it keeps pointers);
    `test/Guards/pool-layouts.cpp` pins it.
15. **What the model did not measure.** The model built the ports without
    WeaveC. Measured on the build, the heap is no longer what the fast
    build costs over Clang: on json49 the runtime is 26% of the samples
    where the system allocator is 28% of Clang's, and the port's own code
    (with its trusted library clones) takes about 110 ms more than Clang's
    (506 against 397). RFC 0044's port and the Safe C port are 1.21 and
    1.22 times Clang with the heap at parity. The gates of §10 that compare
    with Clang are reported below as measured.

## Measured gates

darwin-arm64 (Apple M-series, macOS 27), Release compilers of main at
`13d6774d` (M) and of this branch (B), medians of nine interleaved runs, ms
and the ratio to Clang's `-O2` build of the same source. Evidence:
`build/rfc55/` (README.txt).

**S0.** (a) and (b) together (amendment 1): RFC 0049's port 1.005 of Clang
(limit 1.05), the Safe C port 1.222 (limit 1.25), a pair 9.0 to 9.3 ns
(limit 12): met. The compare-and-swap: dropping it gains 3.3% on RFC 0044's
port, so it goes (amendment 9). (c): 1.7% on json49, 1.3% on the
interpreter, nothing on the others: not shipped (amendment 10).

| Program | Clang | Main, fast | This, fast | Main, checked | This, checked |
| --- | ---: | ---: | ---: | ---: | ---: |
| `json`, C++ (RFC 0049 G7) | 685 | 820 (1.20) | 670 (0.98) | 2,394 | 2,397 |
| `json`, C++ (RFC 0044) | 465 | 631 (1.36) | 563 (1.21) | 1,356 | 1,359 |
| `json`, Safe C (RFC 0054 G4) | 685 | 1,343 (1.96) | 812 (1.19) | 1,721 | 1,737 |
| LRU (RFC 0048) | 57 | 71 (1.25) | 71 (1.25) | 84 | 84 |
| tinyxml2 (RFC 0048 G4) | 771 | 1,098 (1.42) | 1,114 (1.44) | 1,381 | 1,388 |
| interpreter (RFC 0054 G3) | 1,145 | 1,663 (1.45) | 1,468 (1.28) | 3,312 | 3,265 |

(The Safe C row is from a run of that group alone, its checked columns
from fifteen runs: in the run of every program the machine was busy for
that group and its times rose by a fifth, the ratios unchanged. The
checked Safe C builds' ratio moved between 0.985 and 1.025 from run to run;
fifteen runs give 1.009.)

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass**, but for the in-line forms, which are not built. `test/Guards/ir-pools.c` pins `take`, `give` and `realloc_typed` and the checked unit's announcement; `typed-reuse.c` the reuse, the kept shadow in a program of Safe C and the checked build's stop; `realloc-typed.c` the type a moved block keeps and the `bad-cast`; `reference-load.c` the test in the checked build and its absence in the fast one; `pool-layouts.cpp` the layout in the key; `rt_test` `entries-on-and-off`, `entries-refill-and-flush`, `entries-across-threads` (a sweep while a thread is on the short paths), `realloc-keeps-type`. Every lit test, case suite and unit test passes on darwin-arm64 and in the Linux container (linux-aarch64, glibc, libstdc++). |
| G2 New code | **Fails** on three of five. RFC 0049's port 0.98 (ask 1.00): met. RFC 0044's 1.21 (1.10), the Safe C port 1.19 (1.00), LRU 1.25 (1.15): not met. The interpreter 1.28 (1.30) and 1.43 of the program as written (1.45): met. Amendment 15 says why: the heap is at parity, and the rest is WeaveC's code. |
| G3 Against Rust | **Fails.** The Safe C port 1.76 to 1.85 times Rust's (ask 1.5; 3.0 on main), RFC 0044's port 1.29 (ask 1.15; 1.44). RFC 0054's G4 stays open. |
| G4 The path | **Pass.** A typed allocation and its release in batches: 8.8 to 9.1 ns against the system allocator's 17.2 on Darwin (0.52; ask 0.6; main 20.0), 5.7 to 6.0 ns a single pair (main 19.0). On Linux in the container, 9.0 to 9.7 ns against glibc's 11.1 to 12.0 (0.82; ask 1.0; RFC 0050 measured 1.73). |
| G5 Conversions | **Not met for tinyxml2, the others not measured.** tinyxml2 is 1.4% slower than main (1.2% in its own loop; 0.9% with both built `-falign-functions=64`). It makes about 200 allocations a document, and its code differs from main's only in the calls of its releases and allocations: the difference is the code's layout, not the path. yaml-cpp, cJSON, sds, inih, jsmn and tinyexpr were not run. |
| G6 Nothing believed is wrong | **Pass for what was run.** Under `-fweavec-checks=verify`, RFC 0049's port and the Safe C port pass 645 of 646 vectors with no report (the one that fails, a lone surrogate printed pretty, fails the same under Clang). The checked build's stops: the case suites pass; RFC 0044's seeded bugs were not run. Every second release G10's round tried, in one thread, stopped in the fast build. |
| G7 Memory | **Fails for the interpreter by 0.9%.** Peak resident: json49 156 MiB (main 161), RFC 0044's port 101.5 (103.1), the Safe C port 72 (141), LRU 9.2 (9.2), the interpreter 185.4 (183.7). |
| G8 Threads | **Pass for what was run.** `rt_test`'s `entries-across-threads` passes 1,000 runs on Darwin and 200 in the Linux container. RFC 0046's indexer port was not run. |
| G9 Nothing elsewhere | **Pass for what was run.** Of 706 guarded objects of `test/cases/` and `test/Guards/`, 486 are main's byte for byte, and the rest differ only in the frame word's ABI number and, in three, the path of the compiler's own `asan_interface.h` in their debug information. The checked builds of the G2 programs are within 1.01 of main's (0.986 to 1.009). A guarded Lua and compile times were not measured. |
| G10 A round | **One found, closed.** A fresh agent tried two types in one slot (same-size structs and classes, array forms with cookies, `realloc` across classes, a tail after shrinking, a type compiled two ways), one slot named twice (second releases by every form and from safe and guarded code, after a `realloc` moved the block, interior pointers) and the allocator's state from stale pointers. Everything stopped or was rejected but two definitions of one C++ name across a library (amendment 14), which main has too. Closed and pinned. Its side finding, a Safe C bound kept from before a `realloc` (a false `pointer-out-of-bounds`), is on main too and is not this RFC's. |

Code: the library 47 lines of code more than main (33,333), the runtime
377 more (9,582).

The cost gates G2, G3, G5 and G7 are not met as written, so the RFC stays
Accepted; it is Implemented when a later change meets them or the owner
changes them.
