# RFC 0045: Type-stable memory

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-11-13
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Carries out what
  [RFC 0043](0043-ownership.md) (_Future work_: "release without the
  quarantine") and [RFC 0044](0044-safe-cxx.md) (decision D2 and _Future
  work_: "Typed C++: spend the rules") deferred. Supersedes, for blocks a
  safe function releases, RFC 0036 §3 ("reuse only after a sweep") and
  guarantee (T); RFC 0042's (M3); RFC 0043's (O4) in part; RFC 0044 §12
  and its gate G7 ("a safe C++ function is compiled exactly as a guarded
  one") and the last sentence of its (X6). Extends RFC 0044's (D4) (the
  standard library is trusted) from what the rules assume to what the
  passes guard, and the rule `escape`. Amends the driver's flags, the
  library table, the enforcement ledger, the runtime's ABI and the defaults
  for allocation stacks and container annotations. Adds no annotation and
  no report kind.

This RFC was written at the request of the project owner on 2026-11-13,
who asked a fifth time for the single change that would do most to make
an AI rewrite into C or C++ as attractive as an AI rewrite into Rust, to
be implemented next, with breaking changes and the removal of old code
welcome. The measurements in _Motivation_ were taken on main at
`c1056dbf` (v0.24.0 and Safe C++), darwin-arm64, Homebrew LLVM 23.1.0,
on the two ports of RFC 0044's gate G4 (`build/rfc44/g4/`). The rows
marked "prototype" used a patch to `GuardExpand.cpp` that was not kept;
the patch, the commands and the output are in `build/rfc45/probe/` (not
committed).

The owner accepted the design as drafted on 2026-11-13, before any of it
was built, and with it the six decisions that were theirs to make. They
are marked **(D1)** to **(D6)** where they are argued, and collected here.
The thresholds of §14 and the _Unresolved questions_ are still open, and
S0 is where they are settled; question 9 may reopen (D5) with what gate
G3 measures.

- **(D1) Safe code's memory safety rests on types, not on being
  watched.** A block a safe function releases is reused at once, by an
  allocation of the same type and no other. A stale pointer the rules did
  not reject then reaches memory of its own type: never another type,
  never out of bounds, and no longer a certain trap. That is what a stale
  index does in the Rust port of the same program.
- **(D2) Two modes, and the loop uses the slow one.**
  `-fweavec-safe-checks=full` keeps every guard and the quarantine, as a
  safe unit is compiled today; an agent builds its tests with it. The
  default is the fast mode.
- **(D3) A reference is believed.** In a safe unit a reference parameter,
  `this`, and the reference a safe or trusted function returns are what
  C++ says they are, and accesses at constant offsets through them carry
  no guard. Code that is not safe is trusted to pass valid ones.
- **(D4) The standard library's owners are trusted with their own
  storage**, as safe Rust trusts `std`: their member functions run
  without guards, behind the checks the library's hardening makes.
- **(D5) One class of error comes to rest on the rules alone**: a borrow
  into storage that changes type in place (the alternative of a
  `std::variant`), used after the change. Today's guards do not reliably
  stop it either; after this RFC nothing at run time tries.
- **(D6) Not yet Rust's speed.** Stack frames and the guards on views stay
  as they are. The allocating port is expected to go from 3.1 to about
  1.7 times the reference, the other from 1.45 to about 1.25. The
  library's line limit rises from 20,850 to about 22,300 and the
  runtime's from 10,500 to 11,000.

## Summary

Four RFCs gave an agent Rust's loop in C and C++: a mistake with a bound,
an owner, a move or a borrow stops the compile with the rule and the fix,
and what the rules cannot follow stops when a test reaches it. What they
did not give is Rust's other half, the reason a rewrite is proposed in
the first place: the result is fast. A safe C++ port of Python's `json`
module costs 3.1 times the same C++ compiled by Clang and 3.2 times the
Rust port. RFC 0044 said so and named this RFC as the one to fix it.

The obvious fix is the one RFCs 0030 to 0034 tried: where a rule proved an
access, drop its check. That put a young analysis under the program's
safety and the assessments found wrong proofs. This RFC does not trust a
proof. It changes what the runtime keeps true, so that the checks become
unnecessary whether or not the rules are right:

- **Type-stable memory.** Every allocation a safe unit makes carries its
  element type, which Clang already computes. A block released by safe
  code goes back to a pool for that type and slot size and is reused at
  once, only from that pool. Any other reuse waits for a sweep, as today.
  A dangling pointer therefore points, forever, at memory of its own
  type, laid out as it expects.
- **Believed references.** On such a heap a reference needs no check to
  be used at the offsets its type allows. In a safe unit a reference
  parameter, `this` and the result of a safe function are believed, and
  the guard goes away from each access at a constant offset through them.
  A pointer that comes from anywhere else (out of a field, out of an
  iterator, out of arithmetic) is tested once, where it becomes a
  reference, or guarded where it is used, as today.
- **A trusted standard library.** A member function of a standard owner
  (`std::vector::operator[]`, `std::string::append`,
  `std::unique_ptr::operator*`) checks its own preconditions when the
  library is hardened, which WeaveC already turns on. Such functions are
  compiled without guards, and the references they return are believed.
  Iterators, views and algorithms keep their guards.

The quarantine and the sweep stay for everything else: blocks of unknown
type, blocks released by code that is not safe, and every block in a
program built with `-fweavec-safe-checks=full`, which is how an agent
builds its tests and is exactly today's safe unit.

Expected result (gates, §14): the `json` port at no more than 1.9 times
the reference (3.1 today) and the LRU port at no more than 1.3 (1.45);
Safe C's allocating conversions faster by what the sweep costs them; no
change to guarded code; every seeded bug of RFC 0043's and RFC 0044's
sets still stopped in `full` mode; and in the default mode, under a
monitor, no access outside memory of its pointer's type.

## Motivation

### The loop is there and the benchmark is not

RFC 0044's gate G4 gave two specifications to a safe C++ agent and a Rust
agent:

| | Rounds | Wall time | Benchmark, times the reference |
| --- | --- | --- | --- |
| LRU cache, Rust | 1 | 1 min 37 s | 1.14 |
| LRU cache, safe C++ | 2 | 2 min 18 s | 1.45 |
| `json`, Rust | 1 | 7 min 29 s | 0.95 |
| `json`, safe C++ | 3 | 9 min 7 s | 3.1 |

The first two columns are within a factor of 1.4. The last is not, and it
is the column a team reads after a rewrite. A port that is safe and three
times slower than the Rust port is not an alternative to it, whatever the
loop felt like. Safe C is closer (1.45 times the reference over RFC
0042's six conversions) for one reason: RFC 0042 spent its rules on the
guards. Nothing has been spent on the allocator in either language, and
nothing at all in C++ (RFC 0044, D2).

### Where the time goes

The `json` port's benchmark (`jsonport bench 20000`: 5.4 million
allocations), user and system seconds, best of five. Each row removes one
thing from the row above.

| Build | Seconds | Removed |
| --- | --- | --- |
| Safe C++ today | 1.36 | |
| `WEAVEC_RT_HEAP_STACKS=0` | 1.15 | allocation stacks: 0.21 |
| Guards off at constant offsets inside a `dereferenceable` argument (prototype) | 1.00 | 0.15 |
| Guards off on every access the standard library's code makes (prototype) | 0.93 | 0.07 |
| `WEAVEC_RT_CONTAINERS=0` | 0.88 | container annotations: 0.05 |
| No access guard at all (prototype) | 0.84 | the project's own guards: 0.04 |
| No tracked frame either (prototype) | 0.74 | frames: 0.10 |
| The reference's objects linked to the runtime | 0.66 | string and library-call guards, landing pads: 0.08 |
| The same with a quarantine budget the run never reaches | 0.61 | the sweep: 0.05 |
| The reference (`clang++ -O2`) | 0.44 | the allocator's own path: 0.17 |

The LRU port, ten runs, without allocation stacks: reference 0.54, safe
C++ 0.80, without access guards 0.64, without frames 0.60, the allocator
alone 0.60.

Four things follow.

1. **No one cost dominates.** Stacks, guards, the allocator and frames
   each take between a tenth and a quarter of the overhead. An RFC that
   removes one of them moves the number from 3.1 to 2.6.
2. **The guards that cost are the library's.** Of 1,072 guards left in the
   `json` port's functions after today's removal rules, 456 are at a
   constant offset inside the bytes of a reference parameter or `this`:
   they check, at every use, what the caller's reference already is. Of
   the 409 guarded rows in its ledger, 350 are at source lines of libc++.
   The agent's own code costs 0.04 s.
3. **The sweep's cost grows with the heap.** At `bench 20000` it is 0.05
   s. At `bench 100000` a sample of the process attributes a third of its
   CPU time to `weavecRtMarkWords`: marking is proportional to live
   memory, and a program that allocates and releases steadily pays it
   again for every 64 MiB it releases. Rust pays nothing.
4. **Zero-initialization and the library's hardening cost nothing**
   measurable here (0.66 with both against 0.65 with neither). The checks Rust also makes are
   not where the time is.

### Why the checks cannot simply be dropped where a rule holds

RFC 0044's gate G2 is the honest measure of the rules: 82 of 110 seeded
temporal bugs stop the compile, 14 more stop as their fixed twin does,
and 14 compile. All 28 stop when they run, because nothing at run time
depends on the rules (RFC 0043's decision, RFC 0044's D2). Dropping a
guard or the quarantine "where ownership proved the release" would turn
each of those 14, and every hole not yet found in a checker that was
merged this week, into silent corruption. RFCs 0030 to 0034 are this project's
record of doing that.

The way out is to stop asking the rules for more than they can give. What
makes a dangling pointer dangerous is not that its object is gone; it is
that something of another type is there instead: a string where a vector
was, bytes where a pointer was. If that cannot happen, a stale pointer is
a logic error with the reach of a stale index, and the question "is this
pointer still live?" no longer needs an answer at each access. The
runtime can make it impossible without knowing anything the rules claim.

### This is what the Rust port does

Both agents of gate G4 wrote the LRU cache's links as indices into a
vector of nodes, because that is how Rust expresses a doubly linked list
without `unsafe`. A stale index in that program reads another node: no
trap, no error, memory safe. Type-stable memory gives a stale pointer in
safe C++ the same standing, and the rules of RFCs 0043 and 0044 remain
what tells the agent about it before it runs.

## Soundness

### Type-stable memory

A **pool** is the set of heap slots of one slot size that have held
elements of one type since a sweep last found no pointer to them. A block
is **typed** when its allocation named its element type (§2). The
allocator keeps:

- **(P1)** A typed block released by safe code returns to its pool, and
  is handed out again only for an allocation of the same element type and
  slot size, with its first element at the same address.
- **(P2)** A slot leaves its pool only through the quarantine and a sweep
  that finds no pointer to it, as every block does today.
- **(P3)** Every other release (a block of unknown type, a release made
  by code that is not safe, any release in a program without a safe unit
  in the default mode) goes through the quarantine and the sweep.

So a pointer to a heap block points, for as long as the pointer exists,
at one of: the live block; the released block, its bytes as the program
left them; or another block of the same element type, on the same
element boundaries, live or zeroed by its allocation. Call that _memory
of its type_.

Safe code cannot tell the three apart by corrupting anything. It forges
no pointer and makes no cast between unrelated types (RFC 0041's (S5),
RFC 0044's (X5)), so every pointer it holds has the static type its
block was allocated with, and every field it reads through one is a
field of that type: a pointer field holds a pointer of the declared
type, a counted pair is a pair. That is typed memory as RFC 0042 defines
it, extended over a block's whole life.

### The guarantee

For a program that meets the assumptions below, in the default mode:

- **(Y1) Reuse by type.** (P1) to (P3).
- **(Y2) Believed pointers point to memory of their type.** In a safe
  unit, an access the passes leave without a check is at a constant
  offset inside the object its pointer's type describes, through a
  pointer that is _believed_ (§4): a reference parameter or `this`, a
  local, a global, the reference a safe or trusted function returned, a
  pointer a reference test passed. A believed pointer points to memory
  of its type, or into a frame that is live or covered by (U).
- **(Y3) Everything else is checked as today.** An index is checked by
  the library's hardening or by a bounds comparison. An access through an
  iterator, a `std::string_view`, a `std::span`, a pointer that came out
  of arithmetic or out of code the passes do not trust is guarded
  against the shadow. A pointer loaded from a field that is not an owner
  is tested when it is loaded (RFC 0043's (O4), now also in C++). A
  pointer that is not believed is tested over the bytes of its type
  before it is passed or returned as a reference.
- **(Y4) No result into the returning frame.** A safe function's pointer
  or reference result does not point into its own frame (`stack-escape`,
  one comparison at the return), by whatever path it was obtained.
- **(Y5) The checked mode is today.** A safe unit compiled with
  `-fweavec-safe-checks=full` keeps every guard, every library check,
  the quarantine for every block it allocates, allocation stacks and
  container annotations. Its guarantee is RFC 0043's and RFC 0044's, to
  the letter.

(Y1) holds whatever the rules say. (Y2) holds whatever the ownership and
borrow rules say, with one exception, which is the next section.

### What comes to rest on the rules (D5)

Type stability is a property of blocks. It says nothing about storage
that changes type _inside_ a live block: the alternative of a
`std::variant` or a `std::expected`, the target of a `std::any` or a
`std::function`, the characters of a short `std::string`, which share
their bytes with its pointer when it grows. A borrow into such storage
(`auto &s = std::get<std::string>(v);`) used after the object switched
(`v = std::vector<int>{};`) reads a string's fields out of a vector's
bytes, in trusted library code, with no guard.

`stale-borrow` rejects that in the function that does it, and
`call-alias` and `owner-path` across calls: assigning to an object ends
every borrow into it. In the default mode nothing else stands behind
them. This RFC adds what can be added:

- A borrow into switching storage is not stored where the rules stop
  following it: making a reference (a pointer member or element) from
  one is `escape`, unless the pointee holds no pointer and is `const`
  (a `const char *` into a string reads stale characters, which is not
  a memory error).
- Stage S0 lists the standard types that switch, by structure (a union
  with two or more members, one of which holds a pointer), and seeds
  bugs on each. Gate G3 requires every one to stop the compile.

Today's guards are not a guarantee here either: the bytes are live, so
the first access passes, and what stops the program is that the pointer
it then follows usually leads nowhere. RFC 0044's (X6) says such errors
"stop when they run"; for this class that was never certain, and this
RFC corrects the sentence in both modes.

### What gets weaker, in plain terms

In the default mode, for a program with a safe unit:

- **(T) and (M3).** A use after free of a typed block that safe code
  released no longer always traps, and is no longer confined to the dead
  block. If the block was reused, the access reaches another object of
  the same type. This holds for guarded code in the same program that
  kept a pointer to such a block.
- **(O4).** A reference test passes for a block that was released and
  reused. It still traps for one that was released and not yet reused.
- **A double release** of a block that was reused releases the object
  that now lives there. `owner-moved` rejects the double release when it
  compiles; the allocator still reports one of a block not yet reused.
- **The library's code is unguarded** where it is trusted (§5). A wrong
  answer from the trust criterion, or a bug in the standard library, is
  a memory error and not a trap.
- **Container annotations are off** (`container-overflow`). An iterator
  that runs past a vector's elements but inside its capacity reads
  elements that were destroyed, or zeroes.
- **A stale pointer into a returned frame** that arrives as a believed
  result or parameter is not tested at its use. (Y4) closes the direct
  path; (U) is otherwise as today, including its limit (a frame laid
  over the returned one).
- **Reports carry no allocation stack**, as in Safe C today.

Nothing gets weaker for a program with no safe unit, for a unit built
with `full`, or for the blocks code that is not safe releases.

### Bugs caught

Unchanged when the program compiles: every rule of RFCs 0041 to 0044.
Unchanged when it runs: an index out of range (`library-assertion`,
`index-out-of-bounds`, `pointer-out-of-bounds`), an iterator or a view
out of the block it points into, a null owner, an unterminated string, a
reference to a released block not yet reused, a local's redzone. New:

```cpp
const std::string &name(const Config &c) {
  std::string copy = c.name;
  return pick(copy, c.fallback);   // pick returns one of its arguments
}
```

If `escape` misses this (the result of `pick` may point into `copy`), the
function now stops at its return with `stack-escape`, before any caller
uses the result. Today it stops only when a caller reads the string, and
only until another frame is laid over it.

### Bugs deliberately not caught

In the default mode:

- **A stale pointer the rules did not reject**: a dangling reference in
  a pointer member, a callable that captured `this`, re-entrancy through
  a stored callable (the 14 of RFC 0044's G2 that compile). It reaches
  memory of its type. Built with `full`, each stops as it does today,
  which gate G4 checks.
- **A borrow into storage that changed type** (above).
- **Data races**, and a thread that releases what another uses. A block
  may now be reused while a second thread still reads it; RFC 0044's
  X-A3 already assumed it away.
- **Leaks**, as before.

### Accepted false traps

- `stack-escape` at a return, for a safe function that returns a pointer
  into its own frame and whose callers never use it. None is known that
  is not a bug.
- None from reuse: a program cannot observe which slot it is given.

### Assumptions

| | Assumption |
| --- | --- |
| **Y-A1** | Code that is not safe passes and returns references that refer to live objects of their declared types (extends RFC 0044's X-A1 and X-A2, and is what C++ already requires), and does not use a block after its owner released it. |
| **Y-A2** | A block is used as the type its allocation named: the type Clang infers for `malloc(n * sizeof(T))`, for `new T` and for `std::allocator<T>::allocate` is the type the program stores there. Safe code cannot do otherwise; code that is not safe is trusted. |
| **Y-A3** | Types with different definitions get different pools (§2: the name, the size, and the unit for a type without external linkage). |
| **Y-A4** | **(D4)** The standard library's owner types keep their own invariants given the preconditions their hardening checks, and an object of a standard type whose bytes are all zero is empty or faults at a null address when used. S0 checks the second for libc++ and libstdc++. |
| **Y-A5** | No thread releases or changes an object while another uses it (X-A3). |

## Detailed design

### 1. Modes

`-fweavec-safe-checks=bounds` (the default) is the fast mode for C and
now for C++; `full` is the checked mode. The flag gains its meaning in a
C++ unit, where it has none today.

| | `bounds` | `full` |
| --- | --- | --- |
| Rules when it compiles | all | all |
| Access guards in safe functions | where a pointer is not believed (§4) | every one |
| The standard library's code | trusted functions unguarded (§5) | guarded |
| Allocations | typed (§2) | not typed |
| Releases by safe code | to the pool (§3) | quarantine and sweep |
| Allocation stacks, container annotations | off unless asked | on |

A program may mix units of both modes and guarded units. A block is
typed only if the unit that allocated it was in the fast mode, so a
`full` unit's objects keep (T) wherever they are released.

The guide tells an agent one thing: build and run the tests with `full`,
ship the default. It is the split Rust makes between a debug and a
release build, for a different check.

### 2. Allocation types

Clang computes the allocated type of a call to an allocation function
and attaches it as `!alloc_token` metadata (the type's name and whether
it holds a pointer) when asked (`-fsanitize=alloc-token`, LLVM 22). On
LLVM 23.1 it does so for `malloc(sizeof(T))`, `calloc(n, sizeof(T))`,
`malloc(sizeof(T) + n)`, `realloc`, a `malloc` whose result is returned
or assigned as `T *`, `new T`, `std::make_unique`, `std::make_shared`
(as the control block's type), `std::allocator<T>::allocate` (a vector's
buffer, a map's node, a string's characters), and gives none to an
allocation whose result stays `void *`.

- The driver asks for the metadata in a safe unit in the fast mode.
  `GuardInsert` reads it, replaces the call with the runtime's typed
  entry point, and removes the metadata; LLVM's own token pass does not
  run. The runtime also answers LLVM's `__alloc_token_*` names as
  untyped allocations, for objects another compiler built with the flag.
- The pool's key is a 64-bit hash of the type's name, its size, and, for
  a type without external linkage (every C type; a C++ type in an
  unnamed namespace or a function; a lambda), the unit's name. Two units
  that allocate one C struct get two pools, which costs sharing and
  nothing else: a block's pool is recorded when it is allocated, and its
  release needs no type.
- An allocation whose first element does not start the block is not
  typed: an array `new` with a cookie, an over-aligned form the pass
  cannot place. Nor is a huge block (one the arena maps by itself).
- A call site passes a pointer to a 16-byte record (the key, the
  element size, and a word the runtime fills with the pool it resolved),
  so that the lookup happens once per site and thread cache.

### 3. Pools

`runtime/weavec_alloc.c`. A pool is a per-thread list of free slots of
one key and slot size, with a shared list behind it, built like the
thread caches of RFC 0037 §2.

- **Allocate** (`__weavec_rt_alloc_typed`): pop a slot from the pool; if
  it is empty, take a slot of that size from the arena as today and
  record the pool in its slot word. Zero the bytes requested, as every
  allocation is zeroed today. Write the shadow only if the size
  requested differs from the one the slot last had.
- **Release** (`__weavec_rt_release_typed`, and the sized and aligned
  forms of `operator delete`): check the block is live (else
  `double-free`, `invalid-free`, `alloc-release-mismatch`, as today),
  mark it released in the shadow, push it on its pool. No quarantine, no
  release record, no stack. A block that is not typed takes today's
  path, whichever entry point released it.
- **`realloc`** of a typed block allocates from the pool of the new slot
  size and releases the old block to its own.
- **Surplus.** A pool that holds more free bytes than a budget (a share
  of the quarantine's, so that the two together stay within
  `WEAVEC_RT_QUARANTINE`) moves its oldest slots to the quarantine. They
  come back as ordinary slots after a sweep finds no pointer to them:
  the only way a slot changes type, and (P2).
- **The sweep** treats a pooled free slot as it treats a quarantined
  one that it does not intend to recycle: its bytes are not roots.
- **Statistics**: `WEAVEC_RT_STATS` reports pools, typed allocations,
  releases to pools and slots moved to the quarantine.

The entry points are added to `weavec_rt.h` and `weavec_rt.exports`;
`WEAVEC_RT_ABI` becomes 44.

**Which releases are typed.** `GuardInsert` rewrites a release call when
it is in a safe function, in a trusted library function (§5), or in
standard-library code compiled into a safe unit (§6): `free` and the
library table's `release` rows in C; every form of `operator delete` in
C++. A release in a `WEAVEC_GUARDED` or `WEAVEC_UNSAFE` function, in a
guarded unit, or in code WeaveC did not compile is today's release.

### 4. Believed pointers

`GuardInsert` runs before inlining, on each function alone. In a safe
unit in the fast mode it places no guard on an access whose address is a
constant offset from a believed pointer and lies inside the bytes that
pointer is known to cover. Believed, and their bytes:

1. A parameter with `dereferenceable(N)` (a reference, `this`): N. In a
   safe function, a pointer parameter: the size of its pointee, as RFC
   0041's single-object bound, behind the null test of RFC 0035 §2.4
   when the offset is larger than a guard page.
2. A local and a global, by today's rules (RFC 0035 §6), unchanged.
3. The result of a call to a safe function or a trusted library function
   whose return carries `dereferenceable(N)`: N. The result of a call to
   a project function that is not safe: the same (Y-A1).
4. A pointer loaded from a field the frontend annotated (C: every
   pointer field, RFC 0042 §2; C++: a raw pointer member, a reference
   member, a by-reference capture of the unit's own classes and
   lambdas), after its reference test (`testReferences`, RFC 0043 §7):
   the pointee's size. Safe C++ gets `annotateFields` and the reference
   test, which Safe C has.

Everything else is not believed: the result of a library function that
is not trusted (`*it`, `sv.data()`, `std::get_if`), a pointer loaded
inside such a function, anything computed at a variable offset. For
those:

- An access keeps its guard, as today.
- Passing one as a `dereferenceable(N)` argument, or returning it as
  such a result from a safe function, places a range guard of N bytes
  before the call or return (`__weavec.range`; `GuardPrune` and
  `GuardExpand` merge and remove dominated ones as they do now). This is
  the one place a check is added: today the callee guards each access
  instead.

A safe function that returns a pointer, a reference, or a small
aggregate with pointer members compares each against its own frame's
bounds before returning (Y4; `__weavec_rt_escape_check` exists for RFC
0041's parameters and gains this caller).

`full` mode skips this section: `GuardInsert` behaves as today.

### 5. Trusted library functions (D4)

A function is **trusted** when:

- it is declared in a system header in namespace `std`, with a name the
  standard does not reserve for the implementation, as a free function
  or as a member of a class with such a name;
- its class, if it has one, is not a view (`SafeCxxKinds.h`: an
  iterator, `std::string_view`, `std::span`, `std::reference_wrapper`,
  a type Clang marks `gsl::Pointer`);
- none of its parameters is a pointer, an iterator, another view, or a
  callable; and
- the library table does not list it (`class std::X : unchecked(name,
  …)`, for a member whose precondition the library's hardening does not
  check and that S0 finds; none is known for libc++'s `fast` mode among
  the functions the ports call).

`v[i]`, `v.push_back(x)`, `v.back()`, `s.append(t)`, `s += c`, `m.find(k)`,
`m[k]`, `*p` and `p->` on a smart pointer, `*opt`, `std::get<T>(v)`,
`std::move`, `std::swap`, `std::make_unique`, and the copy, the move and
the destruction of every owner are trusted. `v.insert(it, x)`, `v.erase(it)`,
`s.append(ptr, n)`, `std::string(ptr)`, `std::sort`, `std::copy`,
`std::ranges::sort(v, cmp)`, and every member of an iterator or a view
are not, and are compiled as today.

The checker decides this where it already looks at every call a safe
function makes, and marks the callee with an implicit
`annotate("weavec.trusted")`. Then, before `GuardInsert`:

- A trusted function gets no guard on any access.
- Each standard-library function a trusted function calls, directly or
  through others, is cloned (`<name>.weavec.t`, `linkonce_odr` in its
  own comdat), the clone is treated as trusted, and trusted code calls
  the clone. So `std::vector::push_back` reaches an unguarded
  `__emplace_back_slow_path` and an unguarded relocation loop, while
  `std::copy` called from the project still reaches the guarded one. A
  call from library code into the project (an element's constructor, a
  comparison, a hash) is not cloned and runs as what it is.
- A trusted function and its clones never share a symbol with the
  guarded instantiation another unit may emit, so a guarded unit never
  links against an unguarded copy.

What stands where the guards stood is the library's hardening:
`_LIBCPP_HARDENING_MODE_FAST` and `_GLIBCXX_ASSERTIONS`, which the
driver has defined since RFC 0039 and which report as
`library-assertion`. S0 decides whether a safe unit should ask libc++
for its `extensive` mode.

### 6. Standard-library code the unit can see

Most of `std::string` is not instantiated in the program: the library's
shared object defines it, and its growth allocates and releases there,
where no type is known and no release is typed. At `-O1` and above Clang
still gives the unit the bodies, as `available_externally` definitions
for the inliner. In a safe unit in the fast mode, a standard-library
function the unit calls and has a body for is kept as the unit's own
(`linkonce_odr`, renamed as §5's clones are) instead of being discarded,
so its allocations are typed, its releases are typed, and it is trusted
or guarded by §5 like any other. The same holds for libstdc++'s
explicit instantiations. S0 measures what this costs in compile time and
code size on the ports.

### 7. The rule added

`escape` (RFC 0044 §8) gains one case: a pointer member or element is
not assigned from a borrow into switching storage unless its pointee
type holds no pointer and is `const`. `SafeCxxKinds.h` gains the kind
(_switches_: the class, a base or a member has a union of two or more
members of which one holds a pointer; structural, never by name). The
error and its note:

```text
error: a pointer into 'v', whose contents change type, is stored in
       member 'current_' [weavec-safe:escape]
note: keep the index or the key and look the value up when it is used;
      'weavec-cc --weavec-explain escape' shows the forms
```

### 8. What does not change, and why

- **Frames.** A local whose address escapes is laid out with redzones
  and poisoned at its scope's end and its function's return, as today,
  and costs the `json` port 0.10 s. Leaving such a local in a plain
  frame needs to know that no callee makes a view of it (an iterator
  over a short string's characters) and that no pointer to it survives
  in memory. The rules know some of that; a wrong answer would be a
  write over a return address. _Future work_.
- **Views.** An access through an iterator, a `std::string_view` or a
  `std::span` keeps its guard. Neither standard library checks them
  without a change of ABI, and Rust has nothing to compare: its
  iterators cannot be out of range.
- **String and library-call guards, landing pads**: 0.09 s together.
- **Guarded code**: a unit with no safe function compiles to the object
  it compiles to today.

### 9. Allocation stacks, container annotations, reports

- A program that links a safe unit in the fast mode records no
  allocation stacks unless `WEAVEC_RT_HEAP_STACKS` asks. RFC 0042 §9
  made this so for C; C++ units were left out and pay 0.21 s of 1.36.
- A safe unit in the fast mode is compiled without the container
  annotations (RFC 0039 §5), and a program that links one ignores those
  its other units make, since the two would disagree about the same
  vector. `WEAVEC_RT_CONTAINERS=1` is for programs built with `full`.
- A heap report in such a program ends with the line that says how to
  get the rest: `weavec: note: built for speed; rebuild with
  -fweavec-safe-checks=full for allocation stacks and exact
  use-after-free reports`.

### 10. Verify mode, the ledger and the audit

- **`-fweavec-checks=verify`** keeps each guard this RFC removes as a
  monitor, as RFC 0035 §6 requires of every removal. A monitor on a
  believed access passes when its bytes are in a live block, in a
  released block, or in a pooled slot, and its pointer's pool is the
  block's; anything else is a `weavec.proven` report. That is (Y2)
  stated as a test, and gate G2 runs every suite under it.
- **The ledger** (`weavec-ledger` version 8): the reasons `believed` and
  `trusted` join `typed` and `reference`; the unit's `config` records
  the mode; the summary line counts typed allocation sites and typed
  releases.
- **`--weavec-audit`** (version 4) lists, per unit, the standard-library
  functions it trusts with their source locations, and the allocation
  sites that are not typed with the reason. What a reader of the audit
  must believe is then on one page: the code that is not safe, and this
  list.

### 11. Safe C

Safe C already has typed memory, believed fields, reference tests and
heap-placed locals. It gains §2 and §3: `malloc(sizeof(T))` and its kin
are typed, and `free` of an owner in a safe function releases to the
pool. Its locals placed on the heap (RFC 0041 §6) are allocated and
released through a pool of the local's type, which makes that placement
two list operations. RFC 0042's treatment of referenced released blocks
at a sweep stays for blocks that take the quarantine.

### 12. Cleanups and breaking changes

- A safe C++ unit is no longer compiled as a guarded one. RFC 0044's
  gate G7 and the half of `test/Driver/safe-cxx-units.cpp` that pins
  identical objects are replaced by a test that pins them under `full`.
- `-fweavec-safe-checks` documents one meaning for both languages;
  "no effect in a C++ unit" is removed.
- The default for allocation stacks and container annotations changes
  for programs with a safe C++ unit.
- The guarantees page states (T), (M3), (O4) and (X6) per mode, and
  drops "the ownership rules are not load-bearing" in favor of this
  RFC's statement of what is.
- The hidden `-mllvm -weavec-reference-tests` stays for measuring;
  `-mllvm -weavec-pools=0` and `-weavec-believe=0` join it and are not
  documented as interfaces.
- `WEAVEC_RT_ABI` 44; ledger 8; audit 4; `weavec.h` unchanged.

### 13. Documentation

`docs/pages/reference/guarantees.md` ((Y1) to (Y5), the per-mode table,
the corrected (X6)); `docs/pages/guides/safe-cxx.md` and `safe-c.md` (a
section "Fast and checked builds": what each mode stops, the loop);
`docs/pages/reference/cli.md` (the flag, the variables, the note line);
`resources/safe/rules.md` (`escape`'s new case with a compiled example);
`docs/architecture.md`; `AGENTS.md` (the paragraph, the table rows for
pools and trusted functions, the reading order).

### 14. Stages and gates

- **S0, on paper and by probe, before code.** Which allocation forms
  carry a type on LLVM 23 with libc++ and with libstdc++, and what Clang
  names types without linkage. The census of trusted functions the two
  ports and RFC 0044's G3 files call, each read against the library's
  checks. The zero state of each standard type (Y-A4). The switching
  types. What §6 costs. The attribution table again, on linux-x86_64.
  S0 may move a threshold below or a line of §5's criterion; it reports
  either as an amendment.
- **S1** Pools in the runtime, with `rt_test` tests; typed allocation
  and release for Safe C. **S2** The same for C++; §6. **S3** Believed
  pointers, the reference test and (Y4) in safe C++. **S4** Trusted
  functions and clones. **S5** The rule, the modes, the defaults, the
  reports. **S6** `verify`, the ledger, the audit, documentation.
  **S7** Gates.

| Gate | Passes when |
| --- | --- |
| G1 Tests | Each mechanism is pinned: `test/Guards/ir-pools.c`, `ir-believed.cpp`, `ir-trusted.cpp`, `typed-reuse.c`, `typed-reuse.cpp`, `stack-escape-result.cpp`; `rt_test` `pool-*`; `test/Safe/cxx-switching.cpp`; `test/Driver/safe-checks-modes.cpp`. |
| G2 Memory of its type | Every test suite, both ports, RFC 0043's and RFC 0044's seeded programs that compile, and a new stress test that keeps stale references on purpose, run in the fast mode under `-fweavec-checks=verify` with no `weavec.proven` report. |
| G3 Switching storage | Every bug S0 seeds on a switching type stops the compile. One that does not is fixed in the rules or the gate fails; it is not waived. |
| G4 The checked mode | Built with `full`: RFC 0044's 28 that compile or stop as their twin does all stop with today's reports; RFC 0043's G2 unchanged; the object file of a safe C++ unit is byte for byte what main produces today. |
| G5 Speed | User and system time, best of five, against the same source by `clang++ -O2`: `json` port at most 1.9 (3.1 today), target 1.7; LRU port at most 1.3 (1.45), target 1.25. Safe C's conversions: sds and cJSON, the two that allocate most (2.09 and 2.11 after RFC 0043), faster by at least a tenth, and none slower. Reported with the attribution table, whatever the result. |
| G6 Memory | Peak resident memory of both ports' benchmarks no higher than today. |
| G7 Build | Compile time of the ports' libraries at most 1.25 times today's; text size at most 1.3 times. |
| G8 No change elsewhere | The corpus gate's ratchet is unchanged; a unit without a safe function compiles to an identical object; the `dev` and `ci-release` presets pass on darwin-arm64 and in the Linux container. |
| H1 Hygiene | Library within 22,300 lines, runtime within 11,000. |

## Annotation surface

None. `resources/include/weavec.h` is untouched. `weavec.trusted` is an
implicit annotation the checker adds to standard-library declarations,
as `weavec.field` is; it has no spelling a program can write, and
`include/weavec/Frontend/SafeCheck.h` holds its constant.

## Diagnostics

Compile time: `escape` gains the case of §7, pinned in
`test/Safe/cxx-switching.cpp`, with a section in the rule's page.

Run time: no new kind. Two lines are new.

- `stack-escape` for a result:
  `weavec: stack-escape at <file>:<line>:<col>: result of '<function>'
  points into its own frame at <address>`

  ```cpp
  static const int *kept;
  WEAVEC_GUARDED void keep(const int &x) { kept = &x; }
  WEAVEC_GUARDED const int &last() { return *kept; }
  const int &worst() { int local = 1; keep(local); return last(); }
  ```

  (The guarded helpers hide the pointer from `escape`, which rejects
  every direct form.) Pinned in `test/Guards/stack-escape-result.cpp`.
- The note of §9 after a heap report in a program built for speed,
  pinned in `test/Guards/typed-reuse.cpp`.

`heap-use-after-free` from a reference test, `library-assertion`,
`double-free` and the rest keep their lines. The command-line
reference's list of kinds gains the second `stack-escape` form;
`scripts/run-cases.py` needs no new kind.

## Drawbacks

- **A weaker default (D1).** WeaveC's first promise was that the program
  traps at the first bad access. In a fast safe program a use after free
  the rules missed does not trap; it reads the wrong object of the right
  type. That is a real bug running silently in production, where today
  it would stop. The answer is that the rules catch most of them when
  the program compiles, the checked mode catches the rest when it is
  tested, and what is left cannot corrupt memory; it is still a
  retreat, and the reason the decision was the owner's.
- **Two modes (D2)** to explain, test and keep honest. A team that never
  runs `full` has only the rules.
- **Trust in two large libraries (D4)**, now with nothing behind it. A
  criterion written in four lines decides which of several thousand
  functions run without guards. A function that criterion admits and the
  hardening does not protect is a hole until S0 or a user finds it.
- **One class rests on a checker merged this week (D5).**
- **Memory.** Pools do not share slots between types. A program that
  allocates many types in bursts holds more free memory than a general
  allocator would, until the surplus drains through a sweep.
- **Code size and compile time**: clones of library functions, and the
  bodies of `std::string` in every unit that uses it.
- **Still not Rust's number (D6)** where a program lives in small
  functions with local strings: frames remain.
- **More of the same budget**: about 1,400 library lines.

## Alternatives

- **Trust the rules**: skip the quarantine where ownership proved the
  release unique, drop guards where a borrow was checked. Faster than
  this RFC and simpler in the runtime. Fourteen of 110 seeded bugs say
  what it would cost today, and RFCs 0030 to 0034 say how such a design
  ages.
- **An "exposed" bit per block**: release at once only blocks no pointer
  to which was ever stored in memory. Exact where it applies, and it
  needs a barrier on every pointer store in every unit and still rests
  on the rules for pointers in locals.
- **Make the guards and the sweep cheaper for everyone.** RFC 0037's
  _Alternatives_ measured the first and declined it; the second is
  bounded below by marking live memory. Neither changes what a safe
  program pays for being safe.
- **Guards only (believed references, a trusted library), no pools.**
  Half the gain on the `json` port, and none of the part that grows with
  the heap.
- **Pools only.** Removes the sweep's 0.05 s and part of the allocator's
  0.17, of 0.92. Pools matter because they make the other half sound,
  not for the sweep alone.
- **One pool for all pointer-free types.** Fewer pools, and wrong: a
  `bool` or an enum read from another type's bytes can hold a value the
  optimizer assumed impossible when it dropped a bounds check.
- **A second standard library** with bounded iterators and checked
  cells (Circle, Fil-C's direction for the runtime half). It would make
  views safe without guards and it is the "no existing C++ compiles"
  that RFC 0044 declined.
- **Data races first.** Rust's other guarantee, and nothing here gives
  it. Go has none either and is a common rewrite target, because it is
  safe enough and fast; that ordering is this RFC's bet.
- **Do nothing**: keep Safe C++ as a checker in front of guarded C++.
  It remains a good way to test C++ and not a reason to choose it.

## Prior art

- **Type-safe memory reuse**: Cling (Akritidis, 2010) and
  Type-After-Type (van der Kouwe et al., 2018) segregate the heap by
  allocation type so that a use after free cannot cross types; Apple's
  `kalloc_type` and typed `xzone` malloc, and LLVM's allocation tokens
  (Elver, 2025), are the same idea in production. They got right that it
  is nearly free. As mitigations for C they leave same-type confusion
  exploitable, because C code casts. Here the code on the other side of
  the pool is checked not to: that is the difference between a
  mitigation and a guarantee, and what this RFC takes is the mechanism
  and Clang's implementation of the type inference.
- **Cyclone** and region-based memory made dangling pointers impossible
  by type; **CCured** and Checked C kept a collector or left temporal
  safety out. Type stability is the cheap middle: dangling pointers
  exist and are harmless.
- **Rust**: `std` is `unsafe` inside and trusted; a release build drops
  overflow checks a debug build makes; arenas of indices are the
  accepted way to write a graph. (D4), (D2) and (D1) each lean on one of
  the three.
- **MarkUs, MineSweeper** (RFC 0036): reuse after a sweep. Kept, as the
  only way a slot changes type.
- **libc++ hardening modes** and `_GLIBCXX_ASSERTIONS`: the checks (D4)
  relies on, with documented coverage that S0 reads function by
  function.
- **SafeStack / CPI** (Kuznetsov et al., 2014): two stacks, one for
  locals whose address escapes. The likely shape of the frame work this
  RFC leaves out.

## Unresolved questions

1. **Is the trust criterion right?** S0's census decides. The known
   risks: a public member with a precondition neither library checks; a
   free function template that takes an owner and derives unchecked
   iterators from a second argument; `std::vector<bool>`; allocator-aware
   members with a user's allocator.
2. **Type names.** What Clang prints for local classes, lambdas and
   types in unnamed namespaces, and whether salting every project type
   with its unit is simpler than deciding linkage in the pass.
3. **Array `new` and cookies.** Which library paths reach an array form
   (`std::make_unique<T[]>`), and whether the pass can tell a cookie is
   present.
4. **Who rewrites the calls**: `GuardInsert` from the metadata, or
   LLVM's pass with the runtime answering `__alloc_token_*`. The second
   is less code and cannot salt a key.
5. **The surplus budget**, and whether pools need a per-type cap for
   programs with thousands of instantiated node types.
6. **`extensive` hardening** in safe units: what it adds to (Y-A4) and
   what it costs.
7. **The default.** Should `full` be the default at `-O0` and the fast
   mode only with optimization, so that an agent's unoptimized test
   build is the checked one without a flag?
8. **Mixed programs.** A guarded unit that frees what a fast unit
   allocated takes the quarantine; is the reverse (a fast unit's
   destructor releasing a guarded unit's untyped block) common enough to
   type blocks when they are released, from the type the release site
   names?
9. **Switching storage (D5).** If gate G3 cannot be met, the fallback is
   to believe nothing a switching type's accessor returns, which keeps
   today's guards around every `std::get` and gives back part of the
   `json` port's gain. Is there a cheaper check (a generation in the
   variant's index) worth a table row?
10. **Linux and libstdc++**: `available_externally` bodies for
    `std::string`, the zero states, and whether `_GLIBCXX_ASSERTIONS`
    covers what `fast` hardening does.

## Future work

- **Frames**: plain frames for locals no view and no stored pointer can
  reach, or a second stack for the others; 0.10 s of the 0.3 this RFC
  leaves on the `json` port.
- **Views without guards**: bounds carried by the view (libc++'s bounded
  iterators where the ABI allows), or loop ranges for iterator walks.
- **The allocator's path** for pooled slots, once it is the largest
  remaining cost.
- **Data races**: sendable and shareable kinds (RFC 0044's _Future
  work_), now with a concrete stake: reuse while another thread reads.
- **A checked cell** for shared mutable state and for switching storage.
- **G5 of RFC 0044**: conversions of existing C++, which this RFC makes
  worth their cost.
- **Typing a block at its release** (question 8), and typed `void *`
  callbacks in Safe C (RFC 0042's _Future work_).

## Implementation amendments

These record where the implementation departs from the design above, from
building it and from what the gates found. They override the body.

1. **A fast unit is one compiled with `-fweavec-safe` (§1).** The
   allocation types come from Clang's `-fsanitize=alloc-token` metadata,
   which the driver must ask for before the unit is parsed, so a unit
   whose only safe functions are marked `WEAVEC_SAFE` is not fast: its
   allocations are untyped and its releases take the quarantine. A fast
   unit's guarded functions allocate typed (the type is the allocation
   site's) and release untyped.
2. **The type travels in an operand bundle (§2).** The inliner drops
   `!alloc_token` when it turns a call into an `invoke` (Clang's own token
   pass loses the same types: an inlined `std::make_unique` in a function
   with a landing pad gets token 0), so GuardInsert reads the metadata
   before inlining and gives each typed call a private record of its key,
   named by a `weavec.type` operand bundle, which the inliner keeps; at
   the end GuardExpand turns each such call into the runtime's entry point
   (`markAllocations`, `typeAllocations`). LLVM's token pass is kept from
   instrumenting anything by removing `sanitize_alloc_token`.
3. **The typed forms are rows of the library table (§2).** A `typed(N, F)`
   clause names the runtime's entry point and its flags: `malloc` and
   `calloc`, and new rows for `_Znwm`, `_Znam` and their `nothrow` forms
   (an over-aligned `new` is not typed). `realloc` is not typed: the block
   it moves to is untyped and the ledger says `reallocated`; the runtime's
   typed `realloc` of §3 was not built.
4. **The key (§2)** is a hash of the type's name, salted with the unit's
   absolute source path for every C type and for a C++ name with `(` or
   `anonymous` in it; an array `new` is keyed apart (a cookie may come
   first). The size is not in the key; the class is in the pool's identity.
5. **Releases are typed by a site flag (§3).** No typed release entry
   points: the release wrappers the passes already call (`free_at`, the
   `delete_*_at` forms) take a site record, and a release whose site has
   `WeavecRtSiteTyped` returns a typed block to its pool. GuardLibrary sets
   it in safe code (in C, the public entry of a split safe function too),
   in the standard library's code a fast C++ unit compiles and in trusted
   clones.
6. **A pooled block is zeroed when it is released (§3).** Zeroing at reuse
   left a pooled slot's stale pointers in place, and with the slot read as
   a root the sweep kept every quarantined block they named: the JSON
   port ran 2,021 sweeps and took 15 s. A typed release now zeroes the
   object's bytes (inline under 128 bytes), and reuse writes nothing; the
   sweep still reads a pooled slot's bytes, for what a stale pointer
   writes there. The pools hold at most the larger of the quarantine's
   budget and the live heap; past that a release goes to the quarantine.
   A sweep does not flush the threads' pool caches, and a slot spilled to
   the quarantine never starts a sweep itself (a sweep started from a
   flush inside a sweep recursed). The per-thread pool cache holds 64
   slots of each of 64 pools.
7. **Darwin: what the system's libraries allocate takes any release.**
   libc++'s own `operator new` (in its shared library, which grows a
   `std::string` through `push_back`) reaches the runtime's malloc zone as
   `malloc`, and an inlined `operator delete` of the program then freed it
   as a mismatch. The zone's allocations now have no form, and the release
   check accepts any form for them; guarantee (R) says so.
8. **Believed pointers (§4)** are a parameter's or a safe, trusted or
   other project function's result's `dereferenceable` bytes, looked for
   through the local Clang spills a parameter or a reference to before the
   optimizer runs (a local stored once and only read), at offsets below
   4,096. A raw pointer parameter is not believed, and Safe C++ gets no
   field annotations or reference tests (rule 4 was not built): a pointer
   loaded from a member keeps its guard. Believed accesses are removed in
   the fast unit's safe code and in the standard library's code it
   compiles, not in its `WEAVEC_GUARDED` functions. Safe C keeps RFC 0042's
   bounds; only C++ units believe.
9. **Trusted functions (§5)** are found by the checker (`TrustedCalls`)
   among the callees, constructors and destructors of safe functions and
   the members the implicit members of safe classes use; their symbols,
   and the unit's own functions' (safe and other), reach the passes as
   lists of mangled names. `cloneTrusted` clones each, everything a clone
   calls that the unit defines from the library, and every constant table
   of functions a clone reads (a variant's visitors, which dispatch copies,
   moves and destruction: without them most of the JSON port's remaining
   guard cost stayed); clones are `linkonce_odr` and hidden, `.weavec.t`.
   The table clause `unchecked(…)` was not needed and not built. An
   `available_externally` body of a trusted function (`std::string`'s)
   is cloned too, so §6 holds for what safe code reaches through trusted
   calls; the rest of `std::string` still runs in the C++ library and its
   blocks are untyped (§6 was not built further).
10. **Frames share no bytes in a fast C++ unit (§8).** A reference into a
    local whose scope ended is believed, so the fast mode keeps that
    local's bytes its own for the whole call: tracked frames are laid out
    without shared space, and untracked locals lose their lifetime
    markers, which the code generator would otherwise use to overlap
    them. Guarantee (Y2) says a believed pointer reaches memory of its
    type or a live frame; (Y6) says a dead local is read as itself.
11. **The result check (Y4)** runs in GuardExpand, after inlining (an
    inlined callee's result into its caller's frame is not into its own),
    on safe functions' pointer results: a result in [the stack pointer −
    64 KiB, the frame record's end) calls `__weavec_rt_frame_escape`, a
    `stack-escape` report with the function's name. An aggregate result is
    not checked.
12. **Safe C sizes allocations by their type (Y-A2).** Clang infers the
    type from the `sizeof` in the size before the conversion, so `struct a
    *p = malloc(sizeof(struct b))` was typed `b` and safe C could reuse an
    `a`'s block as a `b`. `pointer-cast` now rejects an allocation
    converted to `T *` whose size names another type, unless `T` is a
    byte type and the named type holds no pointer. It found one case in
    RFC 0043's conversions (sds: `sds *vector = malloc(sizeof(void *))`,
    rewritten `sizeof *vector` for the gates).
13. **Switching storage (§7)** is a union with two members that are more
    than a one-character placeholder or an empty class, one of which holds
    the stored pointer's type, found in the object, its bases and members,
    and a library template's type arguments (a container's elements). It
    does not need a pointer among the members: libstdc++'s `std::string`
    keeps its pointer outside the union, whose other member is the
    capacity the library's own code believes. `std::optional` does not
    switch; `std::any`'s byte buffer is not seen (an `any_cast` pointer is
    a known gap). A pointer pushed into a member container is checked as
    a member is. The flow now follows a pointer result of a library
    accessor that takes a pointer (`std::get_if(&v)`) into what the
    pointer points to: `stale-borrow` missed a stale `get_if` pointer
    before, which this RFC would have left to the rules alone.
14. **Verify mode (§10).** A believed or trusted access's monitor has the
    site flag `WeavecRtSiteBelieved`: a released heap block passes it, and
    a fault at a null address (a zeroed object's pointer, Y-A4) is
    reported as an ordinary `null-dereference`, not `weavec.proven`. That
    a pointer's pool is the block's is not checked.
15. **The ledger and the audit (§10).** The unit's `config` has `safe`
    (`fast` or `full`); the summary has `typedSites`; a `library` list
    holds the trusted functions (`called`, or `reached` from one) and the
    allocations not typed (`no type`, `no typed form`, `reallocated`). The
    summary line ends `; <n> typed allocation sites, <m> trusted library
    functions`. The audit (version 4) lists them.
16. **Container annotations (§9).** A fast C++ unit is compiled with
    `__SANITIZER_DISABLE_CONTAINER_OVERFLOW__` (libc++) and without
    `_GLIBCXX_SANITIZE_VECTOR` (libstdc++); a program with a fast unit
    ignores the annotations other units make unless
    `WEAVEC_RT_CONTAINERS=1`.
17. **Mixed programs.** A standard-library function that a fast unit and
    a guarded unit both emit under one name (not a trusted clone) may be
    either unit's copy at link time; the fast copy's believed accesses
    carry no guard, so a guarded caller that passes it an invalid
    reference loses that trap (D3), and a release through it may return a
    typed block to its pool.
18. **Hidden switches (§12):** `-mllvm -weavec-pools=0` (no typed
    allocation or release) and `-mllvm -weavec-believe=0` (no believed
    access, trusted clone, unshared frame or result check), for measuring.

## Measured gates

Measured on darwin-arm64 (Homebrew LLVM 23.1, the `dev` preset) on
2026-11-14, and checked on linux-aarch64 with libstdc++ (gate G8).
"main" is `c1056dbf` built from a worktree beside the branch. Times are
user and system seconds, best of five. The evidence is in `build/rfc45/`
(not committed): `run-g44.py`, `g2-runtime.py` and its output, `g5c/`
(the Safe C conversions, their build and time scripts), the corpus gate's
log, the Linux logs.

| Gate | Result |
| --- | --- |
| G1 Tests | **Pass.** `test/Guards/ir-pools.c`, `ir-believed.cpp`, `ir-trusted.cpp`, `typed-reuse.c`, `typed-reuse.cpp`, `stack-escape-result.cpp`; `test/Safe/cxx-switching.cpp` (12 errors beside their accepted forms, `std::get_if` included) and the allocation-size cases in `test/Safe/casts.c`; `test/Driver/safe-checks-modes.cpp`; `rt_test` `pool-reuses-by-type`, `pool-untyped-release`, `pool-slots-are-scanned`, `pool-new`; one row expectation per new table row in `LibrarySpecTest`. The rules' pages gained two compiled examples. |
| G2 Memory of its type | **Pass for what was run.** RFC 0044's seeded programs that compile as safe C++ (12 of the 28 its gate G8 ran; the other 16 do not compile with `-fweavec-safe`), built fast under `-fweavec-checks=verify`: no `weavec.proven` report (two believed accesses through a zeroed object's null pointer report `null-dereference`, amendment 14). Both ports under `verify`: the `json` port's 646 checks give 645 as before and no report; the LRU's tests pass. The case suites under `verify`: every safe and typed case passes but `safe/use-after-free-own-block.c` and `typed/dangling-chain.c`, which read a block released under a safe C function by design (RFC 0041, RFC 0042 §9) and fail the same way on main. The stress test the gate names was not written. |
| G3 Switching storage | **Pass for the cases seeded.** `std::variant` alternatives (through `std::get` and `std::get_if`, in a member, a member container, a constructor's initializer and a vector of variants), `std::string` and `std::wstring` characters (libc++ and libstdc++), an element string's characters in a `std::array` all stop the compile; a pointer to a variant, to an element, into a `std::pair` or a `std::tuple` that holds a string, and into an `std::optional` is accepted. A stale `std::get_if` pointer is `stale-borrow` (amendment 13; RFC 0044's flow missed it). Not seen: a pointer from `std::any_cast`. RFC 0044's sets give their numbers exactly: 82 of 110 seeded bugs stop the compile, 21 fixed twins rejected, 86 of 90 correct files compile. |
| G4 The checked mode | **Pass.** Built with `-fweavec-safe-checks=full`, the 12 seeded programs that compile as safe all stop with a report (11 `heap-use-after-free`, one `null-dereference`); in the fast build 5 of them run to the end, reading an object of the same type, 2 fault at a null address, and 5 still report. A safe C++ unit's object under `full` is the one it gives without `-fweavec-safe` (`test/Driver/safe-cxx-units.cpp`). RFC 0043's G2 was not rerun. |
| G5 Speed | **C++: pass. Safe C: not met.** The `json` port: 0.44 by Clang, 1.34 on main (3.05x), **0.76 fast (1.73x; gate 1.9, target 1.7)**, 1.36 checked, Rust 0.41 (fast is 1.85x Rust). The LRU port, 20 runs: 1.10 by Clang, 1.59 on main (1.45x), **1.30 fast (1.18x; gate 1.3, target 1.25)**, 1.60 checked, Rust 1.25 (fast is 1.04x Rust). Of the `json` port's 5.36 million allocations 5.36 million are typed, and it runs no sweep. Safe C's conversions, fast against main: jsmn 1.26 / 1.26, sds 2.04 / 2.09, cJSON 2.22 / 2.24, inih 1.71 / 1.70, tinyexpr 1.16 / 1.16; with pools off, as main. sds allocates 85% typed and gains 2%; cJSON allocates through a hook pointer and gains nothing: the tenth the gate asked of them is not there, and none is slower. |
| G6 Memory | **Pass.** Peak resident memory: `json` 180 MiB on main, 103 MiB fast (88 by Clang); LRU 9 and 9 MiB. |
| G7 Build | **Pass.** Compiling the `json` port's library takes 1.02 times main's time and the LRU's 0.82; text is 0.74 and 0.18 times main's. |
| G8 No change elsewhere | **Pass on darwin-arm64 and linux-aarch64.** The `dev` preset's tests pass on darwin and the `ci-release` preset's in the Linux container (48 of 48, libstdc++); the documentation's examples compile; the quick corpus gate passes with no ratchet regression; a guarded unit (the `json` and LRU libraries without `-fweavec-safe`, cJSON) compiles to the IR main gives it but the frame record's ABI number. linux-x86_64 was not run. |
| H1 Hygiene | **Pass**: library 21,946 of 22,300 lines, runtime 10,506 of 11,000. |

What the numbers say about the owner's goal: a port that an agent wrote in
1.2 to 1.4 times the Rust agent's time (RFC 0044's G4) now runs within 4%
of the Rust port where it allocates little and within 1.85 times where
it allocates millions of small objects, against 3.2 times before, and its
checked build still stops every seeded bug the rules let through. What
they do not say: that Safe C gains (it does not, unless its allocations
name their type), and that a stale pointer the rules miss is reported in
the build that ships (it is not, by D1).
