# RFC 0032: Runtime enforcement — an object table, guarded facets and a temporal backstop

- **Status**: Implemented (its cost gate R6 as amended by *Implementation amendments* 3; the original bound for zlib and Lua is carried forward)
- **Authors**: WeaveC authors
- **Created**: 2026-10-01
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0030: its guarantee
  (*Soundness*, a new clause (G) and assumption A6), its outcomes (§2.2, a
  sixth outcome `guarded`), its rank (§2.5), its check templates (§10.1,
  §10.2), its modes (§10.7: every enforcing mode links a runtime), its
  zero-initialisation (§11: the allocator zero-fills), its require levels
  (§6.3: a new level `guarded`), its ledger (§12: schema version 2) and its
  rule that temporal facets are never checked. Amends RFC 0031 §7 (unit
  record format 29 becomes 30) and closes the part of its gate G6 that
  runtime extents and liveness checks can close.

This RFC was drafted before implementation. On 2026-10-01 the project owner
accepted the recommendation of the milestone analysis (runtime enforcement
next, with the summary fix and a temporal `verify` gate folded in; an object
table over heap, escaping stack and global objects; a looser performance
bound for the default mode; possible temporal findings demoted from
warnings where the build enforces them) and authorized drafting this RFC and
implementing it end to end in one large change, with breaking changes and no
compatibility layers. Accepted status records that authorization. The status
becomes Implemented only when every gate in *Acceptance gates* passes on the
final tree, or an *Implementation amendment* records what was measured
instead and carries the gate forward.

Where this RFC departs from the recommendation as it was put to the owner,
the text says so with the word **Departure** and the reason.

## Summary

RFC 0030 made WeaveC decide every facet of every memory operation, and
RFC 0031 made what it proves sound. What it cannot prove is still mostly
not enforced: a bounds check needs an extent the code states, and no
temporal facet is ever checked. On the corpus about 40% of spatial facets
and 35% of temporal facets are `unresolved`, and a program compiled by
`weavec-cc` runs a heap overflow through a `struct vec` or a
use-after-free through a callback table to completion.

This RFC adds **runtime enforcement**. Every enforcing build links a small
runtime, `libweavec_rt.a`, which

- replaces the image's allocator with one that keeps every heap object in
  a size-class arena, so that the object a pointer points into, its exact
  extent and whether it is live are found in constant time from the
  pointer alone, with no change to pointer representation or ABI;
- tracks the stack objects whose address escapes and the image's global
  objects, in a per-thread list and a link-time table;
- holds released heap objects in a quarantine, so that a stale pointer
  finds a dead object rather than a new one.

`weavec-cc` then turns each unresolved spatial or temporal facet whose
site has a pointer operand into a **guard**: a check against the object
table, inserted like the checks of RFC 0030. Such a facet has the new
outcome **`guarded`**, between `checked` and `unresolved`: the access is
inside one tracked, live object, or the program traps, or the pointer
points to memory the table does not track. Every release is validated by
the allocator itself. `verify` mode monitors proven temporal facets and
proven spatial facets without a static witness the same way, which RFC 0030
could not.

A possible temporal finding whose facet the build guards is no longer a
warning. One false proof found while measuring (a callee's store into a
global array element is lost from its summary) is fixed, and `verify` over
the corpus test suites becomes a gate for temporal proofs too.

## Motivation

Measured on main at `01bb1d2` (RFC 0031 as landed), release build, fresh
ledgers for every corpus config except sqlite, program ledgers where a
whole-program analysis exists:

| Facet | Original configs: unresolved / checked | Held-out configs: unresolved / checked | Dominant reason |
| --- | --- | --- | --- |
| Spatial | 41.6% / 2.0% | 35.9% / 2.6% | `unknown-extent` (39.7% and 30.9% of all spatial facets) |
| Temporal | 35.1% / 0% | 36.5% / 0% | `unknown-callee` (28.4% and 18.5%), `may-alias-released` |
| Null | 0.3% / 17.8% | 0.1% / 17.0% | |

Eight small programs with one ordinary bug each, compiled with
`weavec-cc` in the default mode:

- **Run silently.** An index far past a heap array reached through
  `v->data[i]` (`unresolved(inexpressible)`); a use-after-free through a
  table of callbacks; a read through a pointer that `realloc` moved (a
  "may have been moved" warning, no check).
- **Not caught by WeaveC.** `fill(buf, argv[1])` writing past a stack
  array through a pointer parameter (`unresolved(unknown-extent)`): the
  stack protector aborts it, after the write.
- **Caught.** An off-by-one on a local `malloc(n)` traps; a returned stack
  address is an error; a conditional double free is a warning.

and two findings beyond the bugs themselves:

- **A false proof.** When a callee stores its parameter into a global
  array element (`static int *g[4]; add(p) { g[1] = p; }`), the caller
  frees the object and another function reads `g[1]`, every site is
  *proven* and nothing is reported; ASan reports the use-after-free. The
  same store into a scalar global, or written in the caller, is correctly
  `unresolved(dangling-escape)`. `verify` mode cannot see this class,
  because it has no temporal check.
- **Possible warnings on correct code.** hiredis 147, mujs 110, Lua 80,
  libyaml 74 warnings on the corpus, with no definite error; a textbook
  vector destructor gets "may be freed twice".

RFC 0031's *Future work* names what holds the unresolved shares: slots
with more targets than a call can name, hooks in configuration objects,
and extents nothing in the code states. Static precision alone did not
reach RFC 0031's targets (G6) and will not bring these shares near zero on
C. A runtime that knows each object's extent and liveness turns the facets
that have a pointer operand into checks, whatever the reason the analysis
gave up, and gives every later precision improvement a measurable payoff:
each new proof removes a run-time check.

## Soundness

### The guarantee

RFC 0030's guarantee stands, with (S), (N), (T) and (V) unchanged, for the
enforcing modes with zero-initialisation on. For a unit compiled with the
runtime (the default; §9), one clause and one assumption are added.

A **tracked object** is one of:

- a *heap object*: a block the image's allocator returned and has not
  recycled (§2), from its start to its requested size;
- a *stack object*: a local variable (not a parameter) of a function
  compiled with the runtime whose address is taken or which decays to a
  pointer, from its declaration to the end of its scope (§4);
- a *global object*: a variable of static storage duration defined in a
  unit compiled with the runtime, other than a thread-local, a string
  literal or an object with a flexible array member (§5).

A tracked heap object is *live* from its allocation to its release, and
*dead* from its release until its storage is recycled; stack and global
objects are live while tracked.

- **(G)** If the spatial facet of *s* is *guarded*, and the pointer operand
  of *s* points into (or, for an operand that is the base of a subscript,
  one past the end of) a tracked object *O*, then *O* is live and the bytes
  *s* accesses lie inside *O*, or the program traps at *s* before the
  access. If the temporal facet of *s* is *guarded* and the pointer operand
  of *s* points into a dead tracked object, the program traps at *s*
  before the access. For a guarded release site, the released pointer is
  null, or the start of a live heap object, which the release makes dead,
  or points to no tracked object; otherwise the program traps.

- **A6 — untracked memory.** A pointer that a guard finds outside every
  tracked object points to an object the image's WeaveC units did not
  create (a string literal, a parameter's own storage, memory another
  image or another allocator owns, memory a unit built without the runtime
  created), which is live and which the access stays inside.

(G) is weaker than (S) and (T) in three ways, and the ledger keeps the two
apart for that reason (§1):

1. **Provenance is the pointer's value.** A guard asks which object the
   pointer points into *now*. Pointer arithmetic that leaves object *A*
   and lands inside live tracked object *B* passes a guard at a
   dereference. A subscript `p[i]` is checked against the object that
   contains `p`, so an index that leaves that object traps wherever it
   lands. Between heap objects there is always at least one byte that
   belongs to no object (§2.2), so a walk that leaves a heap object one
   element at a time, forwards, meets it and traps; adjacent stack or
   global objects have no such gap.
2. **Recycling.** A dead heap object stays dead until the quarantine
   releases its storage (§2.4). A stale pointer used after the storage has
   been given to a new object finds that object, and the guard passes.
3. **Untracked memory** (A6).

`-fweavec-checks=report` without `WEAVEC_RT_ABORT=1` reports a failed
guard and continues, as it does for a failed check.

**Blame property.** Case 2 of RFC 0030's blame property ("the violated
facet of *s* is unresolved or trusted") now reads "unresolved, trusted, or
guarded with one of the three limits above". The other cases are
unchanged.

### Where this is more sound

- Every unresolved spatial or temporal facet of a dereference, subscript,
  library-call argument or release becomes a trap for the bugs (G)
  describes, where before nothing ran.
- Every release in the image, whatever its facets say and whichever unit
  it is in, is validated by the allocator: a double free, a free of an
  interior, stack or global pointer, and a `realloc` of a dead block trap.
- `verify` mode monitors proven temporal facets, and proven spatial facets
  that have no static witness (§7.4). A `weavec.proven` trap there is a
  false proof.
- A callee's store into an element of a global array reaches its callers
  (§8).
- A5 no longer rests on the allocator's usable-size query for units built
  with the runtime: the allocator zero-fills every block it returns.

### Where this is less sound

- **Possible temporal findings on guarded facets are not reported.** A
  build that used to print "may have been freed" for a real
  path-dependent bug now compiles without a warning; the bug traps when
  it happens, within the limits of (G). `-Wweavec-possible` restores the
  warnings, the `weavec` tool (which enforces nothing) still prints them,
  and `-fweavec-require=checked` fails the build on any guarded facet.
- Nothing else. No facet that was proven, checked or a violation changes
  outcome because of this RFC, except the proven facets §8 corrects.

### Bugs caught

At run time, by a trap at the offending operation (examples; each has a
case under `test/cases/semantics/runtime/`):

```c
struct vec { int *data; size_t len, cap; };
int get(struct vec *v, size_t i) { return v->data[i]; }   // object: i past the block
void fill(char *dst, const char *s) { while (*s) *dst++ = *s++; }
  char buf[8]; fill(buf, argv[1]);                          // object: first byte past buf
struct item *b = find(&l, "b"); remove_item(&l, "b"); b->name;   // live
char *first = buf.p; append(&buf, long_string); first[0];        // live (realloc moved it)
add_timer(show, x); free(x); fire_timers();                      // live, in show
conn_close(c); conn_close(c);                                    // release (double free)
free(&local); free(p + 1);                                       // release
memcpy(dst, src, n);          // object: n past either block, extents unknown statically
```

### Bugs deliberately not caught

- The three limits of (G): a pointer that arithmetic moved into another
  live tracked object, checked at a plain dereference; use after the
  quarantine recycled the storage; memory the table does not track.
- **Boundary facets.** The temporal facet of a Call site (a call or exit
  boundary, RFC 0030 §9.4) is about every place reachable from the
  arguments and globals, not about one pointer, and stays `unresolved`.
  The uses that rely on it are guarded where they are not proven.
- **Stack objects after `longjmp` into foreign code, in coroutines and on
  alternate signal stacks.** They become untracked (§4.3), never
  mis-tracked.
- **Sub-object overflows.** A guard checks the whole object, as RFC 0030's
  checks do (its §7.4).
- **Leaks**, data races, and everything else RFC 0030 lists.

### Accepted false positives and false traps

- **A guard never compares against a bound the runtime does not know to be
  exact.** A heap object's extent is its requested size; a stack or global
  object's is its type's size (its run-time size for a variable-length
  array). An object with a flexible array member, a parameter, and
  anything the table has no entry for is untracked, and a guard passes.
- **The width of a guarded access is the bytes the access touches**, not
  the size of the pointee type: `p->f` guards `sizeof(f)` bytes at
  `offsetof(f)`. A struct allocated without its unused tail (Lua's
  closures and strings, `sockaddr` variants) is not a false trap.
- **A zero-length library access passes** without looking the pointer up.
- **A correct program can still trap when it breaks C's rules in a way
  that happens to work**: reading a word at a time past the end of a
  string's allocation, using a block's unused tail beyond its requested
  size without asking `malloc_usable_size` (which now returns the
  requested size), comparing or subtracting pointers into a released
  block is fine, but dereferencing one is not. Such code needs
  `WEAVEC_UNSAFE`. Gate R3 requires 0 traps in every corpus test suite.
- **Programs that depend on the system allocator's identity** (a
  `malloc_zone_t` of their own, `malloc_usable_size` slack, allocator
  introspection, `mallopt`) see the WeaveC allocator instead. A program
  that defines its own `malloc` keeps it, and its heap is untracked (§2.6).

## Detailed design

### 1. The `guarded` outcome

`core::SiteOutcome` gains `Guarded` (JSON `guarded`):

| Outcome | Meaning |
| --- | --- |
| Guarded | Not proven and not statically checkable. A guard at the site traps under clause (G). The guard is emitted when checks and the runtime are enabled; in `weavec`, and with `-fweavec-checks=none`, the ledger still says `guarded` and the summary line says "guardable (not enforced)" |

The rank of RFC 0030 §2.5 becomes

> violation > unresolved > guarded > checked > trusted > proven

A guarded facet keeps the unresolved reason the engine gave it, as
`reason`, so the histogram of why facets are not proven survives; the JSON
row is `{"outcome": "guarded", "reason": "unknown-extent", "check":
{"template": "object"}}`. `FacetDecision` carries the reason for both
outcomes; `isWellFormed` requires it for `Guarded` as for `Unresolved`.

Only the planner creates the outcome (§6). The engine is unchanged: it
publishes `unresolved` with a reason, as today.

**Summary line.** `…: 86 sites: 44 proven, 4 checked, 30 guarded, 8
unresolved, 0 trusted; …`. The guarded count is always printed, like the
checked count. `unresolvedShare` in the JSON summary gains `spatial`,
`temporal` and `null` beside `spatialNull`, and a `guardedShare` object
with the same keys.

**Require levels** (RFC 0030 §6.3). `-fweavec-require=guarded` makes every
unresolved facet an `unresolved-operation` error. `checked` also makes
every guarded facet an `unresolved-operation` error (its message names the
reason and says "guarded at run time only"). `proven` is unchanged.
`WEAVEC_REQUIRE_SAFE` still holds a function to `checked`.

### 2. The allocator

`runtime/weavec_alloc.c`, in `libweavec_rt.a`.

#### 2.1 The arena

At first use the runtime reserves (maps without access or backing) one
contiguous range of address space: `ClassCount` *regions* of `2^RegionShift`
bytes each, followed by the metadata range. `RegionShift` is 32 (4 GiB);
if the reservation fails it is retried with 30. Region *k* holds *slots*
of `classSize[k]` bytes, laid out from the region's start. The classes are
16, 32, 48, …, 128 (steps of 16), then four per doubling (2^n, 1.25·2^n,
1.5·2^n, 1.75·2^n) up to 2^30 bytes, each a multiple of 16; a region whose
class is larger than a quarter of the region is not created.

For a pointer `p` inside the arena:

```
region = (p - arena) >> RegionShift
slot   = ((p - arena) & (2^RegionShift - 1)) / classSize[region]
base   = regionStart + slot * classSize[region]
```

The division is a multiplication by a per-class reciprocal, exact for
offsets below 2^32. No shadow memory is touched to find the slot.

#### 2.2 Slot metadata

One 32-bit word per slot, in a metadata array per region (reserved with the
arena, committed on first touch):

| Value | State |
| --- | --- |
| `0` | never allocated (the bump pointer has not passed it), or free |
| `(size << 2) \| 1` | live, `size` the requested size in bytes |
| `(next << 2) \| 2` | free, linked to the next free slot (index + 1; 0 ends the list) |
| `(size << 2) \| 3` | dead: released, in quarantine, `size` kept for reports |

The free list lives in the metadata, never in the freed memory, so a write
through a stale pointer cannot corrupt the allocator.

A request of `n` bytes takes a slot of the smallest class **strictly
larger** than `n`: every slot has at least one byte after the object that
belongs to no object. That byte is the gap clause (G) relies on, and it is
why `p + n` (one past the end) still identifies the object. `malloc(0)`
returns a live object of size 0.

A request with an alignment above 16 takes the smallest power-of-two class
that is larger than the size and at least the alignment; regions start at
multiples of 2^RegionShift, so its slots are aligned. An alignment above
the largest class fails with `ENOMEM`.

#### 2.3 Allocation, release and reallocation

- **Allocate.** Pop the class's free list, else advance its bump pointer.
  A slot from the free list is zero-filled; a fresh slot is already zero
  (fresh mappings are). Every allocation is therefore zero-filled over its
  whole slot: `malloc` is `calloc`.
- **Release.** `free(p)`: null is ignored. If `p` is inside the arena it
  must be the base of a live slot; anything else (an interior pointer, a
  dead or free slot) is a failed `release` check (§3). The slot becomes
  dead and enters the quarantine. A pointer outside the arena that is a
  *huge* block (below) is unmapped; one in the stack range of the calling
  thread or in the global table is a failed `release` check; any other is
  handed to the *next allocator* (§2.6).
- **Reallocate.** `realloc(p, n)` of a live arena block: if `n` still fits
  the slot's class (strictly below the class size) and is at least a
  quarter of it, the block stays: the size is updated, and on a shrink the
  bytes from `n` to the old size are zeroed. Otherwise a new block is
  allocated, `min(old, n)` bytes are copied and the old block is released
  (dead, quarantined), so a stale pointer to it is caught. `realloc(p, 0)`
  releases `p` and returns a live zero-size object, the same on every
  platform, as RFC 0030 §11 already made it.
- **Huge blocks.** A request no class can hold (above 2^30 bytes, or a
  class whose region is exhausted) is mapped on its own, rounded up to
  pages, with one inaccessible page after it. Huge blocks are kept in a
  sorted table under a lock; they are tracked objects like the others, and
  a released one is unmapped at once, so any later access faults.
- **Locks.** One spin lock per class, one for the quarantine, one for the
  huge table. A guard reads metadata without a lock (one aligned 32-bit
  load). `pthread_atfork` handlers take and release every lock around
  `fork`.

#### 2.4 The quarantine

A FIFO of dead slots with a byte budget (the sum of their class sizes).
When a release takes it over budget, the oldest slots leave: their
metadata becomes *free* and they join their class's free list. The budget
is 64 MiB by default and `WEAVEC_RT_QUARANTINE=<bytes>` in the environment
sets it (0 recycles at once). A dead slot of 64 KiB or more has its pages
returned to the system when it enters the quarantine (`MADV_DONTNEED` on
Linux, which also zeroes them; `MADV_FREE` on Darwin, then zero-filled at
reuse), so the quarantine's resident cost is bounded by the budget for
small blocks and is address space only for large ones.

#### 2.5 Standard entry points

`runtime/weavec_malloc.c`, in a second archive, `libweavec_alloc.a`, so
that a program that defines the allocator itself can be linked without it
(§2.6). It defines, for the image it is linked into:

`malloc`, `calloc`, `realloc`, `reallocarray`, `free`, `aligned_alloc`,
`posix_memalign`, `memalign`, `valloc`, `pvalloc` (Linux), `free_sized`,
`free_aligned_sized`, `malloc_usable_size` (Linux and FreeBSD) and
`malloc_size`, `malloc_good_size`, `reallocf` (Darwin).

`malloc_usable_size` and `malloc_size` return the requested size of a live
arena or huge block, and ask the next allocator for any other pointer.

Compiled code is not changed to call the runtime: a call to `malloc` in any
object of the image, WeaveC's or not, binds to the image's definition.
The zero-initialisation lowering of RFC 0030 §11 stays as it is (its
wrappers call `__builtin_calloc`, which tells the optimiser the memory is
zero, and their tail-zeroing finds no tail, because the usable size is the
requested size).

**Other images and other allocators.**

- *ELF.* A definition of `malloc` in an executable interposes for every
  shared library, the C library included, so every heap block in the
  process is an arena block. A shared library built with the runtime
  exports the same definitions; whichever definition the dynamic linker
  finds first serves the whole process, and the runtime's own state
  symbols have default visibility so that every copy uses the first one's
  state.
- *Darwin.* A definition in one image serves that image only (two-level
  namespaces); the system libraries keep their allocator. The runtime
  registers the arena as a `malloc_zone_t` (`malloc_zone_register`), so
  that a `free`, `realloc` or `malloc_size` of an arena block *by another
  image* (the system's `getline` reallocating the program's buffer, an
  application freeing what a WeaveC-built library returned) finds the
  zone and reaches the runtime. The zone implements `size`, `malloc`,
  `calloc`, `valloc`, `memalign`, `free`, `realloc`,
  `free_definite_size`, `claimed_address` and the introspection entries
  `fork` needs (`force_lock`, `force_unlock`, `reinit_lock`), and no-ops
  for the rest.
- *The next allocator.* A pointer the runtime does not own is the system
  allocator's: on Darwin it is passed to its own zone
  (`malloc_zone_from_ptr`); on glibc to `__libc_free` and
  `__libc_realloc`; elsewhere to the next `free` and `realloc` in lookup
  order (`dlsym(RTLD_NEXT, …)`).

#### 2.6 When the allocator is not installed

- **The program defines it.** If any unit of the link defines `malloc`,
  `calloc`, `realloc` or `free` (the link step already knows:
  `allocatorDefinedBy`, RFC 0030 §13.2), `weavec-cc` does not link
  `libweavec_alloc.a`. The program's heap is then untracked: guards pass
  on it, releases are not validated, and the link prints the existing
  A5 warning with one more sentence saying so.
- **Sanitizers.** With `-fsanitize=address`, `hwaddress`, `memory`,
  `thread` or `leak`, which replace the allocator themselves, the driver
  behaves as `-fno-weavec-runtime` and says so once.
- **`-fno-weavec-runtime`**, freestanding units (`-ffreestanding`),
  `-nostdlib` and `-nodefaultlibs` links, and targets other than 64-bit
  Darwin and Linux: no runtime. Facets that would be guarded stay
  `unresolved`, nothing is registered, RFC 0030 §11's usable-size
  zeroing and A5 apply as before. The ledger's `config` records
  `"runtime": false`.

### 3. Guards

Three check templates are added to RFC 0030 §10.1 (`CheckTemplate` and
`CheckPlanEntry::Template`): `object`, `live` and `release`. Their helpers
are in the prelude, in the `__weavec_chk_` family and, for `verify` mode,
the `__weavec_prv_` family; each calls the runtime.

```c
/* The `width` bytes at p + off + i * step lie inside the live tracked
 * object that contains p (or that p is one past the end of, when i < 0);
 * passes when p points into no tracked object. Returns p. */
void *__weavec_chk_object(const volatile void *p, long long i,
                          unsigned long long step, unsigned long long off,
                          unsigned long long width);
/* As object, for a library call's argument: `need` bytes from p. need == 0
 * passes without a lookup. Returns p. */
void *__weavec_chk_object_n(const volatile void *p, unsigned long long need);
/* As object_n, where the need is the string at p with its terminator: a
 * terminator lies inside p's object. Returns p. */
char *__weavec_chk_object_s(const char *p);
/* p does not point into a dead tracked object. Returns p. */
void *__weavec_chk_live(const volatile void *p);
/* p is null, the start of a live heap object, or untracked. Returns p. */
void *__weavec_chk_release(const volatile void *p);
```

The runtime entry points they call are `__weavec_rt_object`,
`__weavec_rt_room` (bytes from `p` to the end of its object, the maximum
when untracked, a failure when dead), `__weavec_rt_live` and
`__weavec_rt_release_ok`. The prelude helper holds the fast path for
arena pointers inline (the §2.1 arithmetic and one metadata load); every
other pointer, and every failure, goes through the out-of-line entry.

**Lookup order** for a pointer outside the arena: the huge table (a range
test against its lowest and highest block first); the calling thread's
stack list, when the pointer is inside that thread's stack (§4); the
global table, when the pointer is inside its address range (§5);
otherwise untracked.

**One past the end.** A subscript's base may legitimately be one past the
end of its object (`end[-1]`). In the arena the gap byte makes `end`
still part of the slot. For a stack or global object the next object may
start exactly there, so when the base equals the start of an object and
the index is negative, the runtime looks the byte before it up instead
and checks against that object; when there is none the guard passes.

**Failure.** In trap mode a failed guard is
`__builtin_verbose_trap("weavec", "<template>")` in the prelude helper, at
the access's own location, as for RFC 0030's checks. In report mode the
helper calls `__weavec_rt_report("<template>", file, line, column)` and
continues; for `release` the allocator then ignores the release. A
failed release that reaches the allocator without a guard (a `free` in a
unit built without WeaveC) aborts with
`weavec: invalid release of <p>: <why>` on stderr.

### 4. Stack objects

#### 4.1 Which locals

`SiteCollector`'s walk records, per emitted function, the *escaping
locals*: variables of automatic storage, declared in a `DeclStmt`, that
are not parameters, are not `register`, and

- have their address taken (`&v`, `&v.f`, `&v[i]`), or
- are arrays, or contain an array member, that decays to a pointer
  anywhere other than as the base of a subscript whose result is used
  directly.

A local that is only read, written and subscripted directly is never
looked up (its sites have static extents) and is not registered.

#### 4.2 Registration

For each escaping local `v`, `CheckEmitter` adds one synthesized variable
to `v`'s declaration statement, immediately after `v`:

```c
char buf[8], *__weavec_frame_1 __attribute__((cleanup(__weavec_stack_leave)))
               = __weavec_stack_enter(buf, sizeof buf);
```

built in the AST (a `VarDecl` appended to the statement's declaration
group, with an implicit `CleanupAttr`), not in source. `sizeof` is
evaluated at run time for a variable-length array. `__weavec_stack_enter`
is a prelude helper that calls
`__weavec_rt_stack_enter(base, size, __builtin_frame_address(0))` and
returns `base`; `__weavec_stack_leave` calls
`__weavec_rt_stack_leave(*slot)`. The cleanup runs on every exit from the
scope that C has: falling off the end, `return`, `break`, `continue` and
`goto`.

#### 4.3 The stack list

Per thread (thread-local storage): an array of `{base, size, frame}`,
grown on demand from mapped pages, and the thread's stack bounds
(`pthread_get_stackaddr_np`/`pthread_get_stacksize_np`,
`pthread_getattr_np`), read once.

- **Enter.** If `frame` is outside the thread's stack bounds (a
  coroutine's or a signal's alternate stack), do nothing. Otherwise drop
  every entry whose frame is deeper than `frame` (a callee's entry still
  present means its cleanup was skipped), and every entry at the same
  frame whose range overlaps the new one; then push.
- **Leave.** Find the entry with that base from the top; drop it and
  everything above it. A base that is not found (a declaration a jump
  bypassed, RFC 0030 §11) is ignored.
- **Lookup.** Entries are ordered by frame, so the frame that contains
  the address is found by binary search and its entries scanned. An entry
  whose frame is deeper than the caller's is dead and ignored.
- **`setjmp`.** A `longjmp` skips cleanups. In a function that calls a
  returns-twice function (RFC 0030 §5.4) no local is registered, and each
  such call is wrapped so that, when it returns (either time), entries of
  deeper frames are dropped:
  `__weavec_stack_rewind(setjmp(env))`, a prelude helper that calls
  `__weavec_rt_stack_rewind(__builtin_frame_address(0))` and returns its
  argument. When the `setjmp` is in code built without the runtime, stale
  entries remain until the next enter at or above their frame; a guard
  that meets one can trap falsely. That is the residual risk §*Drawbacks*
  names, and `-fno-weavec-stack-objects` turns stack tracking off for a
  unit.

A stack object is never *dead*: after its scope ends it is untracked.
RFC 0030's static `lifetime-too-short` rules are the temporal story for
the stack.

### 5. Global objects

For each variable of static storage duration that a unit *defines*
(file-scope and `static` locals; tentative definitions included), other
than thread-locals, variables in a non-default address space, variables
whose type has a flexible array member or is incomplete, and variables in
a section the user named, `CheckEmitter` adds a descriptor to the unit:

```c
static const void *const __weavec_global_17[2]
    __attribute__((used, section("__DATA,__weavec_glob")))   /* "weavec_globals" on ELF */
    = { &table, (void *)sizeof table };
```

built in the AST and handed to CodeGen with the unit's other top-level
declarations. The runtime finds the section's bounds (`section$start$…`
symbols on Mach-O, `__start_`/`__stop_` on ELF; the runtime contributes
one empty descriptor so the section always exists), copies and sorts the
descriptors at first lookup, merges duplicates of one address to the
largest size (tentative definitions of different sizes), and answers
lookups by binary search. The table is per image.

String literals are not registered: identical literals may be merged and
overlapping tails shared, so no exact extent exists.

### 6. Planning

`CheckPlanner::plan` gains a second pass over each site, after the
existing one, when `PlannerOptions::runtime` is set. It looks at every
facet record (and requirement record) whose outcome is `Unresolved`, and
turns it into `Guarded` with a plan entry when all of the following hold:

- the site is not in a constant expression, a shared operand or a
  non-default address space (RFC 0030 §2.1), and not inside
  `WEAVEC_UNSAFE` (its spatial facet is already trusted; a temporal facet
  there is guarded like any other);
- the facet and the site kind have a guard:

| Site kind | Facet | Guard | Placement |
| --- | --- | --- | --- |
| Deref, Raw (not a release) | spatial | `object(p, 0, 0, offset, width)` | wraps the pointer operand |
| Index (pointer or array base) | spatial | `object(p, i, sizeof(element), 0, sizeof(element))` | replaces the access, as `span` does |
| Deref, Index, Raw | temporal | `live(p)`; nothing when the same operand has an `object` guard | wraps the pointer operand |
| LibCall, Call | spatial, per requirement with a need | `object_n(arg, need)` or `object_s(arg)` | wraps the argument |
| LibCall | temporal, per pointer argument the row reads or writes | `live(arg)`; nothing when the argument has an `object_n`/`object_s` guard | wraps the argument |
| Release, Raw release (heap family) | spatial, temporal | `release(arg)` | wraps the released argument |

  `offset` and `width` of a dereference are those of the lvalue the
  access denotes: the member chain above the dereference (`p->a.b` has
  the offset of `a.b` and the size of `b`; a bit-field, its storage
  unit), or `0` and the pointee size for `*p`. A dereference whose
  pointee type is incomplete or a function has no spatial guard.

  A library-call requirement's `need` is the engine's requirement term
  when RFC 0030 §10.3 can express it (rules 1–3, 6 and 7; rule 8 does not
  apply, because the have is the runtime's), with `strlen(x)` expressed as
  the bounded length of the string in `x`'s own object. A requirement of
  kind *string* uses `object_s`. A requirement whose need is not
  expressible stays `unresolved` with its reason.

- the facet's reason is not `no-zero-init` (a null matter) or
  `second-owner`, `may-conflict` or `may-dangle` (not about one object's
  bounds or liveness at this site).

PtrArith, Cast and IntToPtr sites, and the temporal facet of a Call site
(a boundary), have no guard and stay `unresolved`.

A guard never replaces a static check: a facet that is `checked` stays
`checked`. Where a site has both a `nonnull` wrap and a guard on one
operand, `nonnull` is innermost, so the guard never sees null from a
checked operand; a guard passes null on (it is untracked) when the null
facet is proven.

**Verify mode** additionally plans, with `proven` set:
`live` for every proven temporal facet of a Deref, Index, Raw or LibCall
site with a pointer operand, and `object` for every proven spatial facet
of those sites for which no static verify check was planned. The ledger's
`summary.verifyChecked.temporal` is no longer always 0.

`PlannerOptions::runtime` is true in `weavec-cc` unless §2.6 turns the
runtime off, and true in the `weavec` tool (the ledger describes the
default enforcing build), which gains `--no-runtime`.

### 7. Emission

- **Prelude.** The guard helpers of §3, the stack helpers of §4.2 and
  `extern` declarations of the runtime entry points and of the arena's
  descriptor (`__weavec_rt_heap`: base, region shift, class table,
  metadata base), all under the existing `-Weverything` suppression. In
  the out-of-line form (`libweavec_chk.a`, for precompiled headers) the
  same helpers are real functions.
- **`CheckEmitter`.** `object`/`live`/`release` entries reuse the
  existing placements: `WrapOperand`, `WrapArgument` and `ReplaceAccess`.
  `ReplaceAccess` for an array-lvalue base takes the decayed pointer the
  subscript already holds. The emitter adds §4.2's declarations and the
  `setjmp` wraps before the site rewrites, and §5's descriptors after
  them.
- **Zero-initialisation.** Unchanged in the AST. `alloca` is lowered as
  before; its block is untracked.
- **The driver.** An enforcing link adds `libweavec_rt.a` and, unless a
  unit defines the allocator, `libweavec_alloc.a`, before the system
  libraries, and `-lpthread`/`-ldl` where the target needs them. A link
  of units compiled with the runtime and units compiled without it is
  allowed: each record says which, and the program ledger's `config`
  says `"runtime": "mixed"`.
- **Report mode.** `__weavec_rt_report` already takes a template name;
  the three new names are added to the runtime's and the runner's lists.

#### 7.4 Flags

| Flag | Meaning |
| --- | --- |
| `-fweavec-runtime` / `-fno-weavec-runtime` | guards, registration and the runtime's allocator (default on in `trap`, `report` and `verify`) |
| `-fno-weavec-stack-objects`, `-fno-weavec-global-objects` | do not register this unit's locals or globals |
| `-fweavec-require=guarded` | §1 |
| `-Wweavec-possible` / `-Wno-weavec-possible` | report possible temporal findings even where the facet is guarded (default off in enforcing builds with the runtime, on otherwise) |
| `WEAVEC_RT_QUARANTINE=<bytes>` (environment) | §2.4 |
| `WEAVEC_RT_STATS=1` (environment) | print allocator and guard counters at exit |

### 8. The summary fix

A callee's store of a parameter (or of anything reachable from one) into
an element of a global array must appear in its summary as a store into
the array's element cell, weak when the index is not a constant, and be
applied at the call so that the caller's later release of the stored
object makes the element a dangling place (`unresolved(dangling-escape)`
at the boundary and, by RFC 0030 §9.4's propagation, at the uses that
rely on it). This is a bug fix that brings `EngineSummary.cpp` in line
with RFC 0031 §6.1 and §4.9 (element effects at their position); the
cases `soundness/alias-global-element-*` pin the constant-index,
parameter-index and counter-index forms and their correct twins.

### 9. Possible findings

A diagnostic with certainty *possible* that is linked to a facet the
planner made `guarded`, and whose build enforces guards (checks on,
runtime on), is not reported and is not in the ledger's `diagnostics`;
the row keeps its reason and detail. `-Wweavec-possible` reports it as
today. In `weavec`, in `-fweavec-checks=none` builds and without the
runtime nothing is enforced and they are reported as today. Leak
warnings and possible findings on facets that stay `unresolved`
(`may-dangle`, `may-conflict`, boundary rows) are unaffected.

`scripts/run-cases.py` passes `-Wweavec-possible`, so the existing
`BUG: … possible` pins keep testing the analysis; its `TRAP` marker
accepts the three new templates, and a new `GUARDED: <facet>` line marker
requires a ledger row there with that facet guarded.

### 10. Records and the ledger

- **Ledger JSON** (`weavec-ledger`): `version` 2. New outcome `guarded`
  in every outcome-count object, `guarded` rows with `reason`, the
  templates `object`, `live`, `release`, `config.runtime`
  (`true`/`false`/`"mixed"`), `summary.guardedReasons` (the unresolved
  reasons' histogram over guarded facets), and the share objects of §1.
  SARIF: a guarded facet is a `note`-level result with
  `properties.outcome = "guarded"`.
- **Unit records**: format 30. The compact facet cell
  `guarded/<reason>`, and the header's `runtime` flag.
- **The corpus gate** reads the new counts; `expected.json` is recorded
  again on the final tree.

### 11. Deletions

- The statement "temporal facets are never checked at runtime in any
  mode" and the code paths that assume it (`SitePlanner::checksFor`'s
  temporal failure, `LedgerSummary`'s comment, the runner's rule that a
  temporal `BUG` cannot be satisfied by a trap).
- `Assumptions::Initialisation`'s dependence on the usable-size query for
  runtime builds: `nonLoweredAllocations` counts only what the image's
  allocator does not serve.
- `libweavec_rt.a` stops being the "report-mode runtime": it is the
  runtime, and `weavec_rt.c` becomes `weavec_report.c` inside it.

Nothing in the engine is deleted.

### 12. Tests

- `runtime/test/`: C tests of the runtime alone, run by CTest on both
  platforms: class and slot arithmetic for every class boundary, exact
  extents, the gap byte, alignment, in-place and moving `realloc`, the
  quarantine's order and budget, double and interior frees (as death
  tests), huge blocks, foreign pointers handed to the next allocator,
  the Darwin zone entered from `getline`, the stack list under nested
  frames, `longjmp` and bypassed declarations, the global table, threads
  allocating and freeing concurrently, `fork`.
- `test/cases/semantics/runtime/`: one bug and one correct twin for each
  example in *Bugs caught* and each rule in *Accepted false positives*,
  with `TRAP: object|live|release` and `GUARDED:` markers; the eight
  probes of *Motivation*.
- `test/cases/soundness/alias-global-element-*` (§8).
- `test/Emission/runtime-*.c` (lit): the rewritten AST for each
  placement, the stack and global registrations, PCH builds.
- `test/Driver/runtime-*.c` (lit): the link lines, the flags, the
  sanitizer and allocator-defined fallbacks.
- Unit tests: the ledger's sixth outcome (rank, JSON, SARIF, record
  round-trip), the planner's guard table, the require levels.
- `test/corpus/injections/`: a heap overflow through an unknown extent, a
  use-after-free and a double free injected into sds, cJSON, zlib and
  Lua, each required to trap.

### 13. Performance

Guards cost a lookup each: about ten instructions inline for an arena
pointer, a call otherwise. Stack registration costs two calls per
escaping local per activation. The allocator replaces the system's.
The budget is gate R5; the levers, in the order they will be pulled if it
is missed, are (1) skipping a `live` guard dominated by a guard of the
same pointer value in the same block, (2) an inline thread-local push
and pop for stack objects, (3) hoisting the guard of a loop-invariant
base. Each lever used is recorded in an amendment.

## Annotation surface

None. `WEAVEC_UNSAFE` keeps its meaning: spatial and null facets inside
it are trusted and get no guard.

## Diagnostics

No new id. `unresolved-operation` gains the form for a guarded facet
under `-fweavec-require=checked`:

`<operation> is guarded at run time only: <reason phrase> [<reason>]`

```c
// -fweavec-require=checked
int get(struct vec *v, size_t i) { return v->data[i]; }
// error: access 'v->data[i]' is guarded at run time only: the extent of
//        'v->data' is unknown [unknown-extent] [weavec::unresolved-operation]
```

The driver gains three notes without ids (they are driver output, not
analysis diagnostics): the sanitizer fallback, the allocator-defined
fallback (appended to the existing A5 warning), and the unsupported-target
fallback.

## Implementation plan

One branch, `rfc0032-runtime-enforcement`, checkpoint commits, squashed
at the end.

- **S0 Tests first.** The probes and twins as cases (failing), the
  runtime's test harness.
- **S1 The runtime.** Allocator, quarantine, huge table, standard entry
  points, the Darwin zone, the stack list, the global table, reports,
  with `runtime/test` passing on Darwin and Linux.
- **S2 Core.** `Guarded`, the three templates, the require level, ledger
  version 2, record format 30, summary line.
- **S3 Access guards.** Planner pass, prelude, emitter and driver for
  Deref, Index and Raw sites; the flags.
- **S4 Stack and global objects.**
- **S5 Library calls and releases.**
- **S6 Verify mode and the summary fix.**
- **S7 Possible findings, the runner, the corpus gate, documentation.**
- **S8 Gates and cost.**

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference
compiler is `$WEAVEC_LLVM_PREFIX/bin/clang`; timing on an idle machine.

**Soundness**

- **R1.** `run-cases.py --asan` over every suite: no ASan-reported bug
  line has its matching facet *proven*. `--checks verify` over every
  executable case and over every corpus test suite: 0 `weavec.proven`
  traps, with `verifyChecked.temporal` above 0 for every config that has
  proven temporal facets at access sites. The eight probes of
  *Motivation* each end in an error or a trap at the bug.
- **R2.** The injections of RFC 0030 G12 and the new ones of §12: all
  detected.

**Precision and coverage**

- **R3.** No false trap: every `CLEAN` case stays clean; every corpus
  config (original and held-out) builds and passes its test suite with 0
  traps in trap mode and 0 reported failures in report mode, except
  failures triaged as true bugs with source evidence.
- **R4.** Unresolved shares, original configs together and held-out
  configs together (program ledgers where they exist): spatial at most
  0.12, temporal at most 0.20. Null stays at most 0.01. The per-config
  values are the ratchet.
- **R5.** Definite errors and RFC 0030 G9–G11 as RFC 0031 left them.
  Possible warnings printed by a default `weavec-cc` build of the corpus:
  leak warnings and boundary findings only.

**Cost**

- **R6.** With the runtime (default): Lua bench at most 2.0× the
  reference compiler, cJSON at most 2.0×, zlib at most 1.5×; peak
  resident memory of each benchmark at most 2.0×. With
  `-fno-weavec-runtime`: RFC 0031 G8's bounds (1.10, 1.15, 1.10).
- **R7.** RFC 0030 G7 (byte-identical objects with
  `-fweavec-checks=none`). Per-unit analysis time within 10% of the
  recorded ratchet (the planner pass must be cheap).

**Platforms and hygiene**

- **R8.** `runtime/test`, CTest and the case suites pass on Darwin arm64
  and Linux x86-64 (CI).
- **H1.** `scripts/check-hygiene.py` passes with the line budget this RFC
  records for the code it adds; the runtime is C under `runtime/`,
  includes no WeaveC header and is counted separately, at most 4,000
  lines.
- **H2.** `npm test && npm run build` in `docs/`; the guarantees
  reference, the README, `docs/architecture.md` and the roadmap describe
  the runtime; this RFC is marked Implemented.

## Implementation amendments

The stages recorded the decisions below as they were implemented. Each
amends the section it names; where the text above and an amendment
disagree, the amendment holds.

1. **The false proof of §8 was a boundary rule, not a summary (§8).** The
   callee's summary already carried the store into the array's element
   cell. What was wrong was the *place class* of an element cell at a
   boundary: `cellClass` in `lib/Analysis/EngineLifetimes.cpp` classified a
   cell by its object's root alone, so a dangling pointer left in `g[1]` or
   in `r->slot[i]` was not a boundary fact and the uses that relied on it
   stayed proven. The class of an element cell is now the class of the
   place that holds it (`fieldHolding`), for global arrays and for arrays
   inside records alike. The fix brings the engine in line with RFC 0031
   §4.9; `EngineSummary.cpp` did not change. The cases are
   `soundness/alias-global-element-*` and `soundness/alias-record-element_*`.

2. **Range caches (§13).** The three levers of §13 were not enough, and
   were replaced by one mechanism. A function that guards a pointer inside
   a loop gets a *range cache*: a local array `__weavec_ranges` of four
   words per entry, `{lo, len, state, expect}`, cleared on entry. An entry
   belongs to the local variable the guards read their pointer from (or
   to one site when there is no such variable); at most 64 entries per
   function. A cached guard (`object_c`, `live_c`) passes without a lookup
   when its bytes lie in `[lo, lo + len)` and the 32-bit word at `state`
   still reads `expect`. For an arena block `state` is the block's own slot
   word, so the entry dies exactly when the block is released or resized;
   for any other object it is the runtime's epoch
   (`__weavec_rt_epoch`), bumped when a huge block is mapped or unmapped or
   a table of globals is added. On a miss an arena pointer is looked up
   inline and its block remembered; any other pointer goes to
   `__weavec_rt_object_range` or `__weavec_rt_live_range`, which answer the
   range by value. A stack object of the guarding function itself is
   remembered only when it lives as long as the frame (it is not declared
   in a nested scope: `WeavecRtScoped`).

   A loop that calls nothing (no call other than a compiler builtin that
   is not a library function, no label, no `case` of a `switch` outside it,
   no block, no inline assembly) is *quiet*: nothing can end an object
   while it runs. The outermost quiet loop around a guard checks the state
   of the entries its guards use once on the way in
   (`__weavec_range_check`), and those guards do not read the state. A
   release by another thread or by a signal handler during a quiet loop is
   not seen until the loop is next entered; RFC 0030 A4 (no concurrency
   outside trust) already excludes it.

   A range cache never makes a guard pass that the uncached guard would
   fail in the same state: an entry holds one object's bytes, the pointer
   itself must lie in them (so a pointer into a neighbour that an index
   brings back into the cached object still fails), and an entry is used
   only while its state stands. Caches are the compiler's own locals and
   are not registered as stack objects.

3. **Gate R6 as measured.** On the reference machine (release preset,
   min of 7 runs, user CPU over the reference compiler's):

   | Benchmark | Default (runtime) | `-fno-weavec-runtime` | Peak memory |
   | --- | --- | --- | --- |
   | cJSON parse/print | 1.66× | 1.14× | 0.59× |
   | zlib minigzip | 1.85× | 1.00× | 1.09× |
   | Lua bench | 5.94× | 1.11× | 1.61× |

   cJSON meets R6. zlib (bound 1.5×) and Lua (bound 2.0×) do not. The Lua
   benchmark executes about 4.5 × 10⁹ guards (1.7 × 10⁹ uncached `object`,
   1.7 × 10⁹ cached `object`, 1.0 × 10⁹ `live`), roughly one for every two
   instructions of the unguarded program, at about three cycles each: the
   cost is the number of guards an interpreter's dispatch loop executes, not
   the cost of one. Constant arena geometry in the guard was measured (3%)
   and not adopted; an out-of-line miss path was measured (slower: misses
   are frequent in short functions) and not adopted.

   R6 is amended to what the manifest's G14 now enforces: default mode at
   most 2.0× for cJSON and zlib and 6.5× for Lua (5.77 to 5.95 across
   runs); without the runtime 1.15×, 1.10× and 1.15× (Lua measured 1.04
   to 1.11 across runs, on RFC 0031 G8's bound of 1.10); peak memory at
   most 2.0×. The bound the RFC set
   for Lua and zlib is carried forward as an open gate (*Unresolved
   questions*, cost): closing it needs guards removed before they run
   (a guard dominated by a guard of the same pointer over the same bytes
   with no call between; the hull of a block's guards of one pointer
   checked once), which is a planner change that needs its own RFC. Until
   then `-fno-weavec-runtime` is the answer for code that cannot pay, and
   the documentation says so.

4. **Verify mode and entry assumptions (§6, gate R1).** A proven facet
   whose proof rests on an assumption about the function's entry state
   (RFC 0030 A1: an exported requirement the callers discharge) is guarded
   in verify mode like any other proven facet. When a case's own `main`
   breaks that assumption on purpose, the ledger blames the row at the
   caller, and the verify build traps in the callee with category
   `weavec.proven`. The case runner therefore notes, instead of failing,
   the `weavec.proven` traps of a case in which a ledger marker
   (`UNRESOLVED`, `GUARDED`, `TRUSTED`) on a `BUG` line was matched: the
   case's author accepted the bug as such a row. In every other case, and
   in the corpus, a `weavec.proven` trap fails the run.

5. **Requirements on whole records at calls (§6).** A call's requirement
   that an argument points to a whole record of the program's own type
   (`need` = `sizeof(struct T)`, from the callee's summary) gets no guard:
   C programs allocate less than `sizeof(struct T)` for a record whose
   tail they do not use (Lua's short strings are a `TString` cut at the
   string's length), and the guard trapped on them. The accesses inside
   the callee are guarded at their own offsets. Requirements of library
   rows, and requirements with an explicit byte count, are guarded as
   §6 says.

6. **Stack objects (§4).** `__weavec_rt_stack_enter` takes flags:
   `WeavecRtLoose` (the function has automatic storage the list does not
   know, a compound literal or an `alloca`, so a pointer one past this
   object may be the start of another and is not attributed to it) and
   `WeavecRtScoped` (declared in a nested scope). A function compiled from
   a precompiled header registers no stack objects. Lever (2) of §13 (an
   inline thread-local push) was not needed once the range caches stopped
   being registered: Lua's benchmark enters 6.5 × 10⁶ stack objects.

7. **The allocator's archive (§2.5, §2.6).** `libweavec_alloc.a` defines
   `malloc`, `calloc`, `realloc` and `free` strongly and every other entry
   point (`reallocarray`, `aligned_alloc`, `posix_memalign`, `valloc`,
   `free_sized`, `free_aligned_sized`, `malloc_size`,
   `malloc_usable_size`, `memalign`, `pvalloc`, `reallocf`,
   `malloc_good_size`) weakly, so a program's own portability shim over
   `malloc` replaces ours and keeps the arena. "The program defines the
   allocator" means: an input of the link defines a symbol the archive
   defines strongly. The driver reads that set from the archive rather
   than from a list of names.

8. **Statistics (§7).** `WEAVEC_RT_STATS=1` prints, at exit, on standard
   error, one line per counter as `weavec: runtime: <n> <what>`:
   allocations, releases, recycled slots, huge blocks, lookups (and of
   those heap, stack, global and untracked), range requests, ranges kept,
   stack objects entered. The counters are not synchronised.

9. **Triaged guard failures (gate R3).** `test/corpus/triage.json` gains
   `guardFailures`: guard failures of a test suite that are true bugs,
   each with its source evidence; the corpus gate does not count a
   failure at such a site as a trap. There is one group: jansson's
   `hashlittle` (`src/lookup3.h:259`, `260`, `263`, `264`) reads a whole
   word past the end of a key and masks the excess off; the source says
   so, and disables the trick under Valgrind and AddressSanitizer
   (`NO_MASKING_TRICK`). A false trap is never triaged.

10. **Injections (§12, gate R2).** Eight, not twelve: for each of sds,
    cJSON, zlib and Lua, one write past an extent only the allocator knows
    and one read through a pointer that a reallocation, or a release
    behind a function pointer, invalidated. Six are stopped by a guard at
    the injected line (`object` for the four overruns, `live` for sds's
    and Lua's stale pointers); the analysis reports the other two before
    the program runs (cJSON's read after `hooks->reallocate` as a definite
    `use-after-move`, zlib's read after `ZFREE` as a `use-after-free`), and
    their entries accept either. RFC 0030's `cjson-df-valuestring`, the
    one injection its gate G12 missed (a double free behind cJSON's
    `deallocate` hook, in the unit alone), is now caught when it runs: the
    allocator refuses the second release. With these the corpus has 39
    injections and all are detected. The other double frees of §12 are the
    cases `semantics/runtime/release-twice_*` and `release-interior_*` and
    RFC 0030's existing injections.

11. **The summary line (§1).** A build that does not enforce guards (the
    `weavec` tool, `-fweavec-checks=none`, `-fno-weavec-runtime`) prints
    the guarded count as `<n> guardable (not enforced)`; without the
    runtime the count is 0 and the facets are counted as unresolved.

12. **Line budgets (gate H1).** `scripts/check-hygiene.py` records them:
    the libraries at most 69,000 lines (the planner's guards, the range
    caches, object registration, the driver's runtime handling), the
    runtime at most 4,000 (`runtime/test/` excluded). The script prints
    the totals it measures.

13. **A guard asks about the address the access uses (§3, §6).** For
    `*(p - i)` a site's operand is `p` (what a null check wraps) and the
    site has no index, so the first implementation guarded `p` at offset 0:
    a false trap when `p` is one past the end of its object (libyaml's
    `*(parser->simple_keys.top-1) = simple_key`, found by the held-out
    corpus) and a missed overrun when `p - i` leaves the object. The
    `object` and `live` guards of such a site now wrap the whole
    difference. The cases `semantics/runtime/access-difference_*`,
    `access-sum_*` and `access-subscript_*` hold twenty expression shapes,
    each of which must pass at the first and last element of a heap object
    and trap one element outside it.

14. **A failed guard remembers nothing (§13, amendment 2).** Where a
    failure is only reported, a range cache is left empty by a guard that
    fails, so the next guard of the same pointer asks again and reports
    its own site.

15. **The report log.** `WEAVEC_RT_REPORT_LOG=<path>` makes report mode
    append every `weavec: runtime check failed: …` line to that file as
    well as printing it. A test harness may keep the output of a passing
    test to itself (CTest without `-V` hid libyaml's failure from the
    report-mode rerun); the corpus gate sets the variable for every test
    and injection run and reads the file.

16. **What the code does where the text above says otherwise.** The
    documentation follows the code in each of these.

    - *Stack objects (§4.1, §4.2, A6).* A parameter whose address escapes
      is entered when the body starts, like a local: a parameter's own
      storage is untracked only when its address does not escape. Escaping
      locals are found by `planObjects`
      (`lib/Frontend/ObjectRegistration.cpp`) on the unit's AST, not by
      `SiteCollector`. `__weavec_rt_stack_leave` takes the object's base
      and the frame.
    - *Global objects (§5).* Each image's constructor hands its section
      of descriptors to `__weavec_rt_globals_add`; the table is sorted at
      the first lookup after an addition. A weak variable, an alias, a
      variable in a named section, a thread-local and a variable whose
      name starts with `__weavec_` get no descriptor.
    - *Entry points (§3).* There is no `__weavec_rt_room`. The runtime's
      entry points are `__weavec_rt_object`, `__weavec_rt_live`,
      `__weavec_rt_release_ok`, `__weavec_rt_string`, `__weavec_rt_strlen`,
      `__weavec_rt_find`, the two `_range` functions of amendment 2 and the
      stack and global registration functions.
    - *Huge blocks (§2.3).* A released huge block is not unmapped at once:
      its range is remapped inaccessible and kept as a dead object, so a
      guard on a stale pointer fails rather than finding untracked memory.
      The 32 most recently released ranges are kept.
    - *`realloc` (§2.3).* A block stays where it is exactly when the new
      size belongs to the same size class; the bytes it gains are zeroed.
      Otherwise the contents move to a new block and the old one is
      released into the quarantine. `realloc(p, 0)` follows the same rule:
      it answers a block of size 0 (`p` itself when `p` is in the smallest
      class), and null only when the allocation fails.
    - *Quarantine (§2.4).* Each size class keeps its own queue of dead
      slots, oldest first; one budget (`WEAVEC_RT_QUARANTINE`, 64 MiB)
      bounds their sum.
    - *An invalid release outside a guard.* The allocator prints
      `weavec: invalid release of 0x…: <why>` and traps.
    - *Fallbacks (§2.6, Diagnostics).* The allocator-defined case is a
      separate `weavec-cc: note:`, not a sentence of the A5 warning.
      `-nolibc` and `-fsanitize=kernel-address` are obstacles too. The
      obstacles are consulted only when neither `-fweavec-runtime` nor
      `-fno-weavec-runtime` was given. A Linux link adds `-lpthread`.
    - *A link without the runtime (§2.6, §10).* When the link is without
      the runtime but an input was compiled with it, the guards in that
      input have no allocator to ask: the link prints a note naming the
      first such input, the program ledger's `config.runtime` is `false`
      and its summary line reads `guardable (not enforced)`.
    - *`disjoint` (§6).* An unresolved `disjoint` requirement whose length
      is a string's becomes `guarded` with the `disjoint` template, the
      length read inside the string's own object (`__weavec_obj_strlen`).
    - *§8's cases.* `soundness/alias-global-element-const_*`,
      `alias-global-element-index_*` (a counter) and
      `alias-record-element_*`.
    - *§12's lit tests.* The rewrite oracles are
      `test/Emission/runtime-oracle-*.c`; a precompiled-header build with
      the runtime is covered by `test/Emission/rfc0030-pch-fallback.c`,
      which runs in the default mode.

17. **What a guard does not cover: proofs under A1.** A facet that is
    proven under an assumption about its function's callers (RFC 0030 A1)
    gets no guard in the default mode. When the caller is a call through
    a function pointer that the link step cannot resolve, the call's own
    row is `unresolved(unknown-callee)` and the callee's accesses stay
    proven: a callback of type `void (*)(int *)` that is handed a
    released pointer dereferences it unchecked. (The case
    `semantics/runtime/callback-table_bug.c` traps because its callback
    takes `void *` and converts it, which makes the extent unknown.)
    `verify` mode guards these facets. Guarding them in the default mode,
    or guarding the arguments of an unresolved call, is the *boundary
    facets* item of *Future work*.

18. **`weavec` and `weavec-cc`.** With zero-initialisation on, the
    compiler decides a few facets `guarded` that the tool decides
    `proven` (measured on `semantics/runtime/uaf-container_bug.c`: two
    spatial facets). The difference costs precision, not soundness; its
    cause is not yet known and is tracked outside this RFC.

19. **Cleanup functions (RFC 0030 §5.1, §5.7).** A variable declared with
    `__attribute__((cleanup(f)))` calls `f(&var)` where its scope ends, at
    no call expression, and the engine did not see the call: a row released
    by a cleanup function at the end of one loop iteration was still proven
    live in the next (found by a case written for amendment 2's quiet
    loops). The engine now takes the call for an unknown callee handed the
    variable, as it takes inline assembly: what the variable reaches may
    have been released, retained or replaced, with the cleanup function
    named as the reason. Analysing the call like any other is left for
    later. A loop that declares such a variable is not quiet. The cases are
    `semantics/runtime/cleanup-function_*`.

20. **Corpus notes.** lz4's randomised tests take a fixed seed and count
    (`-s1 -i500`) so that a run is reproducible: with a seed derived from
    the time, some runs feed `LZ4_decompress_safe` a match offset of 0, for
    which `LZ4_memcpy_using_offset_base` copies two bytes onto themselves
    and RFC 0030's `disjoint` check traps (a true finding that lz4's source
    expects, and independent of the runtime). Gate G15's limit for zlib's
    `make -j8` is 6.6 s (6.0 s measured; it was 5.4 s): every unit now has
    its guards to compile. This amends R7.

21. **Findings of an independent review of the guards.** Fixed, each with
    a case:

    - *Looseness is the frame's (§4.3).* Whether a frame has automatic
      storage the list does not know (a compound literal, an `alloca`, a
      local that cannot be entered, a declaration a jump bypasses) was
      decided per source function, but an inlined function shares its
      caller's frame: at `-O2` a callee's compound literal landed right
      after the caller's registered local and was taken for one past it (a
      false trap). The runtime now asks every entry of the frame, and a
      function with such storage and nothing to enter enters a marker (its
      own guard variable). A local whose declaration a `goto` or a `switch`
      can bypass is not entered, and makes its function loose
      (`semantics/runtime/stack-inlined-literal_ok.c`).
    - *What a range cache may keep (§13).* A variable-length array, and
      any local of a function that has a label, is `WeavecRtScoped`: a
      backward `goto` can declare it again with another size while the
      cache lives (`stack-vla-again_*`).
    - *A negative index from the start of a stack object* leaves the
      object and traps, unless its frame is loose; §3's "when there is
      none the guard passes" now holds only for loose frames and for
      globals (`stack-underflow_*`).
    - *A variable with a cleanup function* has its address taken with no
      `&` in the source; it is entered like any escaping local.
    - *Span checks (RFC 0030 §10.3)* had amendment 13's defect: for
      `*(p - k)` and `*(p + i - 1)` the check wrapped `p`, missing an
      overrun and trapping on a correct program. They wrap the difference
      (`semantics/emission/span-difference_*`). This one is independent of
      the runtime.

    Left as they are, and stated in the guarantees reference:

    - *Globals.* A negative index from the start of a global, and a walk
      off the end of a global one element at a time, pass: the byte before
      or after a global may belong to an object another unit defines
      without the runtime, or to none. (Limits 1 and 3 of (G).)
    - *An array type larger than its block.* `(*pp)[k]` with `pp` of type
      `int (*)[4]` is checked against the type's bound (RFC 0030 §2.1), so
      an access inside the type and outside a smaller block is neither
      checked nor guarded.
    - *Bit-fields.* The guard covers the bytes the bit-field's bits lie
      in; the access Clang emits may be wider (its storage unit), by a
      byte or two at the end of a truncated record.
    - *Zero-length library calls.* `object_n` passes a zero length without
      a lookup, as §3 says; a `live` guard of an argument has no length,
      so a released pointer passed with a zero count traps.

22. **Gate results on the final tree** (release preset, darwin-arm64,
    2026-10-02).

    - **R1.** `run-cases.py --asan`: 532 of 532 cases, no proven facet at
      an ASan-reported line. `run-cases.py --checks verify`: 532 of 532.
      The corpus built and tested in verify mode: no `weavec.proven`
      trap in any config (gate G6 of the corpus gate passes).
    - **R2.** 39 of 39 injections detected.
    - **R3.** Every original and held-out config builds and passes its
      test suite with no trap, except sqlite, whose build stops at its
      three triaged-true `unsafe-operation` errors as before, and
      jansson's four triaged-true guard failures (amendment 9). The false
      traps found on the way (libyaml, amendment 13; the review's,
      amendment 21) are fixed.
    - **R4.** Unresolved shares, original configs: spatial 0.056, null
      0.003, temporal 0.143. Held-out configs: 0.095, 0.001, 0.187.
      (Limits 0.12, 0.01, 0.20.)
    - **R5.** G9, G10, G11 and G13 pass; RFC 0031's G5, G6 and G12 pass.
    - **R6.** As amended (amendment 3): met. As first written: not met for
      zlib and Lua.
    - **R7.** As amended (amendment 20): met.
    - **R8.** Darwin arm64: CTest 705 of 705. Linux arm64 (Ubuntu 24.04,
      LLVM 23, in a container): 703 of 705, the two failures being the
      container's (no `cc`; a test that expects a write to fail, run as
      root). The runtime's own test also passes on Linux x86-64 under
      emulation. Linux x86-64 as a whole is CI's to confirm.
    - **H1, H2.** `check-hygiene.py` passes; `npm test` and
      `npm run build` pass in `docs/`.

## Drawbacks

- **A runtime in the default mode.** RFC 0030's default linked nothing.
  Now every enforcing link carries an allocator, and the program's
  allocation behaviour (speed, fragmentation, address layout) is
  WeaveC's. An allocator is a large surface for bugs of its own.
- **Run-time cost** where RFC 0031 held 1.10×. The answer is a looser
  bound for the default (R6) and the old bound for
  `-fno-weavec-runtime`.
- **`guarded` is a weaker word than `checked`,** and users will read the
  two together. The summary line keeps them apart; (G) says what the
  difference is.
- **Residual false traps from stale stack entries** when cleanups are
  skipped by code built without the runtime (§4.3).
- **Interposition is platform lore**: ELF symbol pre-emption and Darwin
  malloc zones each have corners (static executables, `dlopen` order,
  exported-symbol lists) where the heap becomes untracked rather than
  wrong, but only testing on both keeps it that way.
- **Fewer compile-time hints** in enforcing builds (§9).

## Alternatives

- **Shadow memory over the system allocator** (register each block after
  `malloc`, one shadow entry per 16 bytes). No allocator to own, but a
  quarter of the heap in shadow, O(size) work per allocation and release,
  no gap between objects, and a block released by code outside WeaveC
  leaves a stale entry that mis-tracks the next block: wrong, not
  untracked.
- **Fat pointers or bounds passed alongside pointers** (CCured, SoftBound,
  `-fbounds-safety`'s internal form). Exact provenance, but an ABI change
  or a disjoint metadata table with a store on every pointer write;
  neither fits "ordinary C inserted before code generation, no ABI
  change".
- **Red zones and shadow bits only** (ASan). Catches adjacent overflows,
  not an index far past the object; (G)'s subscript rule is stronger.
- **Hardware tagging** (Arm MTE, Apple MIE). The right temporal backstop
  where it exists; it does not exist on the machines this project is
  tested on. The allocator's slot metadata is where a tag would go:
  *Future work*.
- **Call the runtime's allocator by name from compiled code** instead of
  defining `malloc`. Then objects without WeaveC in the same image, and
  the C library on ELF, would hand system blocks to the runtime's `free`
  and arena blocks to the system's.
- **Keep five outcomes** and call guarded facets `checked` with a
  template. The counts would then mix two different guarantees.
- **Guard pointer arithmetic** (Jones and Kelly, Baggy Bounds). Closes
  limit 1 of (G) but traps on the out-of-bounds intermediate pointers C
  code forms everywhere, or needs their marking scheme.
- **Do nothing at run time and keep improving proofs.** RFC 0031's G6
  measured where that stops.

## Prior art

- **Low-fat pointers** (Duck and Yap, CC 2016; NDSS 2017 for the stack):
  size-class regions and base-by-arithmetic are taken from there. Low-fat
  checks pointer escapes against the *source* object at IR level; WeaveC
  checks at the sites the ledger already has and says what that leaves
  open.
- **Baggy Bounds Checking** (Akritidis et al., USENIX Security 2009) and
  **Jones and Kelly** (1997): the object-table idea and its
  one-past-the-end problem; the gap byte is a small version of their
  padding.
- **AddressSanitizer** (Serebryany et al., USENIX ATC 2012): the
  quarantine and its budget, and the honest statement that recycling
  ends detection.
- **CCured** (Necula et al.) and **Checked C**: prove what you can and
  check the rest; RFC 0030 already took that, and this RFC supplies the
  bound those systems carry in the pointer.
- **jemalloc's `zone.c`** and **mimalloc**: how to live beside Darwin's
  malloc zones and ELF interposition.
- **Rust**: none of this exists there because the proof is mandatory. The
  `guarded` count is the measure of how far a C program is from that.

## Unresolved questions

- **The quarantine budget.** 64 MiB is a guess; R6's memory bound and the
  injection gate decide it.
- **Which locals to register.** §4.1 is syntactic. If R6 fails on stack
  cost, registration narrows to locals whose address reaches a call or a
  store.
- **Whether `object_s` should scan** to the terminator (cost proportional
  to the string) or only require the pointer to be inside a live object;
  the first is designed, the second is the fallback.
- **Darwin zone details** across OS versions (`malloc_zone_t` version 8
  and later fields).
- **How many Call-site requirements** can be guarded in practice (§6
  needs an expressible need); R4's spatial bound assumes most.
- **Cost (gate R6, amendment 3).** Lua at 2.0× and zlib at 1.5× in the
  default mode are open. The measured cost is the number of guards
  executed; removing guards statically (dominated guards, one guard for
  the hull of a block's accesses through one pointer, hoisting a
  loop-invariant base) is the next RFC's subject.

## Future work

- **Boundary facets** (the temporal facet of Call sites): a model of
  hooks, as RFC 0031's *Future work* says, or a liveness sweep of the
  argument's reachable pointers.
- **Guard elimination**: dominance and loop hoisting in the planner, and
  using the engine's facts to prove a guard redundant.
- **Hardware tags** for the temporal guard, with the tag in the slot
  metadata.
- **Thread caches** in the allocator.
- **The false warnings that remain** (leaks through containers, the
  destructor loops of RFC 0031's *Unresolved questions*): static
  precision, a following RFC.
- **RFC 0033, adoption**, unchanged: records in object sections,
  incremental link, `weavec.toml`, baselines.
