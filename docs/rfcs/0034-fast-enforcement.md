# RFC 0034: Fast enforcement, confirmed errors and a bounded analysis

- **Status**: Implemented (gates F5 and F7, and F2's build time for opus,
  not met and carried forward, *Implementation amendments* 16)
- **Authors**: WeaveC authors
- **Created**: 2026-10-04
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0032: how a guard is emitted
  and executed (§3, §7, §13: guards become calls the backend lowers, the
  object table gains shadow memory for the arena, range caches are
  removed), when a stack object is registered (§4) and the quarantine's
  default (§2.4). Amends RFC 0033: (V) and §1 (a definite finding fails
  the build only once confirmed on a feasible path; a lowered or
  unconfirmed finding never traps unconditionally), §5 (library calls
  whose need no term can state are guarded through checked wrappers) and
  §9 (the per-unit budget counts work, not transfers). Amends RFC 0030
  §5.5 (budgets) and §10 (the prelude's guard helpers and the `violation`
  helper).

This RFC was drafted before implementation. On 2026-10-04 the project
owner accepted the recommendation of the milestone analysis (make the
default build cost less than AddressSanitizer at equal or better
detection, make a definite error mean a confirmed one, and bound the
analysis's cost; measure on projects nobody tuned against; delete what
that makes obsolete) and authorized drafting this RFC and implementing it
end to end in one large change, with breaking changes and no
compatibility layers. Accepted status records that authorization. The
status becomes Implemented only when every gate in *Acceptance gates*
passes on the final tree, or an *Implementation amendment* records what
was measured instead and carries the gate forward.

## Summary

After RFC 0033, WeaveC's run-time guards are correct on every project it
has been tried on: no false trap in any test suite of ten projects it had
never seen. Everything else about the default build falls short:

- **It costs more than AddressSanitizer and catches about as much.** On a
  blind set of 61 bug programs the default build stops 54 before the
  faulty access and ASan 55. Lua runs at 6.0× the reference compiler's
  time (ASan 2.4×), QuickJS at 18.5×, curl at 23×, libdeflate at 24.6×.
- **Six of ten fresh projects do not build.** Each stops at a definite
  error, five of them false and one true but in a configuration the project
  documents as never run. Lowered with `-Wno-error`, four of them trap
  unconditionally in correct programs.
- **Building costs 2.6 to 21 times Clang's CPU time**, the slowest single
  files 29 to 84 times its instructions, and one curl test file exhausts
  9 GB of memory.

This RFC keeps the model (prove, check, guard, record) and changes how
each part is paid for:

1. **A guard is a call the backend lowers.** The prelude declares the
   guards instead of defining them; an LLVM pass makes each one a call
   that LLVM's own redundancy elimination and loop-invariant code motion
   can remove, merges the guards one access path makes, and expands what
   is left into an inline check.
2. **The arena gets shadow memory**, one byte per 16-byte granule, so the
   inline check of a heap access is an arena range test, one load, a
   compare and a branch, instead of 28 instructions and seven loads.
   Everything else (huge blocks, stack and global objects, untracked
   memory) goes to one out-of-line slow path that rejects untracked
   memory in constant time. Range caches are deleted.
3. **A local is registered only where a guard can look it up.**
4. **Library calls are guarded through the shadow**, and calls whose
   need no term can state (`strcpy(d, argv[i])`, `sprintf` into a buffer
   of unknown extent) are guarded through checked wrappers.
5. **A definite error must be confirmed.** Each candidate is replayed
   along single paths with path conditions; it fails the build only if a
   feasible path reaches the site and every feasible path that does has
   the violation. Unconfirmed candidates are warnings, and their sites are
   checked or guarded like any other. The engine also stops making the
   five classes of false definite facts the fresh projects showed.
6. **No unconditional trap remains.** A lowered or unconfirmed violation
   gets the check or guard its facet would have had; the `violation`
   helper is deleted.
7. **The analysis has a work budget**, measured in the size of the states
   it transfers and joins rather than in block visits, and a memory budget;
   runs whose result is discarded are not made.

The measure is the ten fresh projects of the milestone analysis, a sealed
set of five more built once at the end, the 61-program detection set of
the analysis, and a new blind detection set written at the end.

## Motivation

Measured on main at `e3fb58c` (RFC 0033), release build, darwin-arm64
(Apple M3), each project's own build and test commands with
`CC=weavec-cc`. CPU times were taken on a loaded machine; ratios of
instruction counts, which do not depend on load, agree with them.

| Project | Default build | Tests (errors lowered) | Build CPU vs Clang | Run time, default | `-fno-weavec-runtime` |
| --- | --- | --- | --- | --- | --- |
| QuickJS | fails: 22 `null-dereference` | pass | 15–21× | 18.5× | 1.07× |
| LMDB | fails: `use-after-free` | pass; an API call traps | 14–21× | 3.5× | 1.01× |
| Janet | fails: 8 `mismatched-release` | `suite-ev` traps | 9–15× | 3.3× | 1.05× |
| brotli | builds | pass | 6.1× | 9.1× | 1.01× |
| xz | builds | pass | 3.6× | 3.3× | 1.03× |
| libdeflate | fails: `double-free` | `test_checksums` traps | 3.9× | 24.6× | 1.03× |
| zlib-ng | builds | pass | 2.6× | 5.4× | 1.06× |
| curl | fails: `double-free`; a test file runs out of memory | pass | 3.9× | 23× | 2.7× |
| cmark | builds | pass | 3.2× | 5.7× | 1.29× |
| libgit2 | fails: `double-free`, `use-of-uninitialized` | pass | 5.2× | 11× | 1.11× |

On RFC 0032's benchmarks, against AddressSanitizer (`-O2
-fsanitize=address`):

| | Lua | zlib minigzip | cJSON |
| --- | --- | --- | --- |
| WeaveC default | 5.97× | 1.83× | 1.94× |
| `-fno-weavec-runtime` | 1.13× | 1.00× | 1.10× |
| ASan | 2.43× | 1.40× | 2.83× |

### Where the run time goes

Measured by counting every helper call in the emitted IR and by building
cost-model variants (the same IR with the helpers swapped):

- **The guard itself.** An uncached `object` guard is 31–36 instructions
  with seven dependent loads (the descriptor, the class table, the slot
  word); a cached one about 23 with nine (the cache's four words are
  spilled to the frame). In Lua that is 28 instructions and 3.4 cycles per
  guard, 4.49 billion guards per run. The same IR with each guard replaced
  by a four-instruction shadow check runs Lua at 1.95–2.13×, zlib at
  1.21–1.39×, cJSON at 1.49–1.70×.
- **Redundancy matters less than cost.** 41% of Lua's dynamic guards are
  dominated by a guard of the same pointer that covers them, and in zlib
  57% are `live` guards of one loop-invariant state pointer. Removing
  them alone takes Lua from 6.0× to 4.6×; with cheap guards they are worth
  another 0.1–0.2×.
- **Range caches do not pay.** The 64-entry cap leaves Lua's later opcode
  handlers without one (1.74 billion uncached guards); caches held in
  memory instead of registers measured slower than none.
- **Untracked memory takes the slowest path.** 198 of libdeflate's 215
  million lookups, and 370 of QuickJS's 481 million, are of memory the
  runtime does not track (an `mmap`'d input, `alloca` frames): a full
  lookup through the stack list and the global table, to pass.
- **Stack registrations.** libdeflate registers 105 million stack objects
  for 16 MB of input, curl 17 per byte transferred: every local whose
  address is taken is entered and left with an out-of-line call, though
  most are only handed to the C library.
- **Library-call guards** take the out-of-line path every time (Lua's
  `memcmp` in `internshrstr`: 0.26×), and every runtime entry first makes
  a non-inlined call to ask whether to forward to another image.
- **cJSON's allocator cost is the quarantine**: with
  `WEAVEC_RT_QUARANTINE=0` the allocator alone matches the reference.

### Where the build time goes

- **Joins.** 50–70% of the analysis of the slow units is
  `combineStates`, which rebuilds every symbol's record (928 bytes) and
  every object's (680 bytes) at every merge, linearly in the state
  (1.5–3 µs per symbol or cell).
- **A few giant functions.** pcre2's `match` takes 97% of its unit's
  analysis and publishes nothing: it reaches the 2,048-symbol limit after
  45,600 transfers and peaks at 4.4 GB. Three functions take 88% of expat's
  `xmlparse.c` for 26% of its proofs.
- **Runs whose result is discarded**: 50–70% of the time on expat, pcre2,
  libxml2 and sqlite goes to runs after a function's most expensive one
  (a recursive component's authoritative pass repeating its last summary
  round, alias-context runs, runs cut by their share of the unit budget
  and run again).
- **Code generation of the inlined helpers**: libxml2's `parser.c` takes
  150 billion instructions with the helpers inlined and 39 billion with
  them called out of line.

### False definite errors

Each reduced to a few lines; each builds clean and runs clean under
`-fsanitize=address,undefined`, and each negative variant (the cause
removed) builds with WeaveC:

- **A path that cannot run** (QuickJS): `if (t == DIRECT) { assert(never);
  b = cur->b; } else b = NULL; … if (t == DIRECT) return b->x;`. The zone
  cannot hold `t != DIRECT`, so the second test's true edge is taken as
  feasible with `b` null.
- **Summary rows that disagree** (Janet): `get_ai` returns a `calloc`
  block or a `getaddrinfo` list, and both rows of its summary are keyed on
  the non-null outcome; the caller applies the first, so `freeaddrinfo`
  of the result is a definite `mismatched-release`.
- **Exclusive stores of one fresh object** (curl): `if (w) *a =
  strdup(x); else *b = strdup(y);` stores the same fresh object through
  both out-parameters under exclusive conditions; in the caller both
  hold it, and releasing one makes freeing the other a definite double
  free.
- **A dangling value passed as a value** (LMDB): after `munmap(map)`, the
  old address is an argument of a function that only passes it to `mmap`
  as an address hint and compares it.
- **`munmap` of part of a mapping** (libdeflate): the row releases the
  whole object, so unmapping the guard page before a buffer and then the
  one after it is a double free.
- **`const` taken as deep** (libgit2): a callee given a `const struct *`
  may write through a pointer stored in the struct; a local whose address
  is stored there is not uninitialised after the call.

And one true but vacuous error: QuickJS's `quickjs.check.o` is built with
`CONFIG_CHECK_JSVALUE`, in which `JS_GetOpaque` always returns null; the
project documents the configuration as compile-only.

### Silent misses

The blind detection set also found one false proof and two gaps:

- **A release in a loop with an early exit is lost from the summary.**
  `for (j = 0; j < p->n; j++) if (j == x) { free(p->kids[j]); return; }`
  derives no release, so a caller's copy of `p->kids[0]` keeps a `proven`
  temporal facet; `verify` mode traps where the default build reads freed
  memory.
- **A `sprintf` guard does not bound what is written**: with a heap
  destination of unknown extent, `sprintf(d, "%s", long)` writes past it
  and nothing traps until a later read.
- **`strcpy(t, argv[i])`** is `unresolved(inexpressible)`: no term can
  name `argv[i]`, so the call is neither checked nor guarded.

## Soundness

### The guarantee

RFC 0030's guarantee, RFC 0032's (G) and RFC 0033's amendments of both
stand, with these changes.

**(G) is unchanged in what it decides.** A guard asks the same question
of the same bytes (RFC 0033 §4 and its amendment 4); the shadow memory of
§2 answers it for an arena address, the slow path for every other
address, exactly as `__weavec_rt_object` answers it today. What changes
is that a guard the backend removes is one whose bytes a guard that
dominates it already checked with nothing in between that can release or
register an object (§1.4). A guard is never moved to where its access
would not happen.

**Stack objects.** (G) covers a local only while it is registered. §4
registers a local whenever a guard can reach it: its address is stored in
memory, passed to a function that is not in the library table, returned,
or may be the operand of a guarded site of its own function. A local
that only the C library and proven or checked sites see is not
registered; no guard looks it up, so (G) loses nothing.

**(V), definite findings.** RFC 0033's (V) is replaced by: *a definite
finding fails the build when it is confirmed (§6.1); lowered with
`-Wno-error`, or unconfirmed, its site gets the check or guard its facet
would have as a possible finding, and when it has none it is
`unresolved(lowered)`.* No site traps unconditionally.

**Library calls.** The wrappers of §5.2 guard what RFC 0033 left
`unresolved(inexpressible)`: the destination of `strcpy`, `stpcpy` and
`strcat` against the source's length, and of `sprintf` and `vsprintf`
against the length written.

No proven, checked or violation outcome becomes proven. Facets that the
work budget (§7) cuts become the over-budget defaults (guarded,
checked or unresolved), never proven.

### Where this is more sound

- `strcpy`, `stpcpy` and `strcat` into a destination of unknown extent
  from a source no term can state, and `sprintf` and `vsprintf` into a
  destination of unknown extent, are guarded.
- A summary keeps a release made inside a loop that is left early.
- `memcpy` and `strcpy` with arguments that overlap at run time are
  checked by their wrappers when no term can state them (§5.2).

### Where this is less sound

- **Fewer compile-time errors.** A true definite finding that the replay
  of §6.1 cannot confirm within its budget is a warning; its site is
  guarded or checked, so a guarded use after free still traps when it
  happens (if the runtime tracks the object), but the build no longer
  stops. The 61-program detection set measures how many.
- **Fewer proofs on the largest functions.** The work budget gives up
  proofs to bound the build (gate F5 records how many); each becomes a
  check or a guard.

### Bugs caught

Newly caught (each has a case):

```c
for (j = 0; j < p->n; j++) if (j == x) { free(p->kids[j]); return; }  // caller's copy: live
sprintf(h->buf, "%s-%d", name, n);        // h->buf of unknown extent: room checked
strcpy(t, argv[i]);                       // t of unknown extent: room checked
memcpy(b + i, b + j, n);                  // run-time overlap: disjoint checked
```

### Bugs deliberately not caught

Unchanged from RFC 0033.

### Accepted false positives and false traps

Unchanged from RFC 0033: none on correct code, measured by gates F1 and
F2. A definite error that the replay confirms can still be false where
the engine's abstraction is wrong on every feasible path it explored; F1
allows none on the fresh projects.

### Assumptions

A1–A6 unchanged. A5 also covers the shadow memory: the program does not
write the runtime's metadata (a write there is a memory-safety bug the
guard on that write catches, unless the write is unguarded).

## Detailed design

### 1. Guards are calls the backend lowers

#### 1.1 The prelude declares the guards

The guard templates of RFC 0032 §7 (`object`, `live`, and the call forms
`object_n`, `object_l`, `object_s`, `release`) are no longer defined in
the prelude. It declares them, with the signatures they have today and a
name per mode:

```c
extern void *__weavec_chk_object(const volatile void *p, long long i,
    unsigned long long step, unsigned long long off, unsigned long long width);
extern void *__weavec_chk_live(const volatile void *p);
/* … and the report forms, `_report` with (file, line, column), and the
   verify mode's `__weavec_prv_*` forms. */
```

The emitter, the planner and the plan format are unchanged: a guarded
access still reads through the helper's result (`*__weavec_chk_object(p,
i, 4, 0, 4)`), so the access cannot be evaluated before its guard. The
runtime defines every declared guard (§2.5), so an object compiled without
the backend pass (an `-O0` build without the pass, an LTO bitcode link of
an older object) links and is checked, out of line.

#### 1.2 Canonical form

`GuardCanonicalize`, a function pass at `PipelineStartEP` (every
optimisation level), rewrites each call of a declared guard:

```llvm
%r = call ptr @__weavec_chk_object(ptr %p, i64 %i, i64 %step, i64 %off, i64 %w)
; becomes
%q = getelementptr i8, ptr %p, i64 (%i * %step)     ; what the helper returned
call void @__weavec.guard(ptr %p, ptr %q + %off, i64 %w, i32 kind, ptr loc)
; and every use of %r uses %q
```

`@__weavec.guard` is declared `nounwind memory(inaccessiblemem: read)`
and is not `willreturn` (it may trap): LLVM may remove a guard that a
dominating identical guard makes redundant (EarlyCSE, GVN), may hoist it
out of a loop it is guaranteed to execute in (LICM), and never moves it
above a condition or a call, or an access above it. Every access now goes
through a `getelementptr` of the original pointer, so alias analysis sees
it as it would without WeaveC, which the helper's opaque result
prevented. The multiplication's overflow, which the helper checked, is
checked by the guard (`kind` carries a flag when `%i * %step` may
overflow). `live` becomes the same call with width 0 and the `live`
kind; `object_n`, `object_l` and `object_s` keep their own forms
(`@__weavec.guard.n`, `@__weavec.guard.s`), with the same attributes.
`release` stays a call of the runtime: it precedes a call of `free`
anyway.

Program stores do not clobber a guard (they do not write the runtime's
memory, A5); a call of anything that may allocate, release, register or
unregister an object does, which LLVM already assumes of every call that
is not known not to touch inaccessible memory.

#### 1.3 Merging

`GuardMerge`, at `OptimizerLastEP`, takes what the optimiser left:

- **Covered guards.** A guard whose pointer is a constant offset from a
  guarded pointer, whose bytes lie inside bytes the dominating guard
  checked, with no instruction between them that may write inaccessible
  memory, is removed. (The standard passes remove only identical calls.)
- **One block, one base.** Guards in one basic block of the same base
  pointer at constant offsets, with nothing between them that may write
  inaccessible memory or leave the block, are replaced by one guard of
  their hull at the first, when the hull is at most 4,096 bytes. The bytes
  between them are bytes of one C object when the program is correct
  (the accesses are through one pointer to one object), so the hull adds
  no trap to a correct program.
- **`live` under `object`.** A `live` guard of a pointer whose bytes an
  `object` guard dominating it checked is removed.

#### 1.4 Expansion

`GuardExpand`, at `OptimizerLastEP` after merging, replaces each
remaining guard with the inline check of §2.3 and a cold call of the slow
path (§3). It loads the arena descriptor once per function, in the entry
block, with `!invariant.load`: a function that started before the arena
existed sees an empty arena and sends every guard to the slow path, which
is correct for every address. At `-O0` it runs too, so even an unoptimised
build has the inline check.

`SplitDispatchEdges` (RFC 0031 §9.2) runs after expansion.

#### 1.5 Deletions

The range caches (`object_c`, `live_c`, `__weavec_range_check`,
`__weavec_rt_object_range`, `__weavec_rt_live_range`, `planCache`,
`declareCaches`, `isQuietLoop`, the cache variables and their tests), the
runtime's epoch, and the inline guard helpers' `_ool` copies are deleted.
The check helpers (`nonnull`, `index`, `span`, `len`, `disjoint`,
`assert`) stay inline C: they are a compare and a branch, and the
optimiser folds them.

### 2. Shadow memory for the arena

#### 2.1 Encoding

The arena reserves, next to its regions and slot words, one shadow byte
for each 16-byte granule of its data. Slots are multiples of 16 bytes and
start at multiples of 16, and an object of `n` bytes takes a slot of more
than `n` bytes (RFC 0032 §2.1), so a granule belongs to one slot. A
shadow byte holds:

| Value | Meaning |
| --- | --- |
| `0` | Not part of a live object: never allocated, free, or in the quarantine. |
| `1`–`16` | Part of a live object: its first `k` bytes are the object's. |
| `0xFF` | Part of a live object's slot, after the object's last byte. |

The shadow is mapped readable and writable without reserve, so pages no
allocation touched read as zero: a pointer into a slot the arena never
reached reads "not live", as its slot word does.

#### 2.2 Maintenance

The allocator writes the shadow of a slot when it hands it out (`16` for
each whole granule of the object, `n mod 16` for a partial last one,
`0xFF` for the rest of the slot), when it resizes a block in place, and
when it retires a block to the quarantine (all `0`). A recycled slot is
written when it is handed out again. A slot large enough to give its pages
back (RFC 0032 §2.4) gives back its shadow's pages too. The slot words
stay: they are what the slow path, `release` and the usable-size query
read, and the shadow is derived from them under the class lock.

#### 2.3 The inline check

For bytes `[a, a + w)` with `1 <= w <= 16`, where `base`, `bytes` and
`shadow` come from the descriptor:

```c
off = a - base;
if (off < bytes) {                       /* in the arena */
  e = (a & 15) + w;                      /* 1 .. 31 */
  v = (signed char)shadow[off >> 4];
  if (e <= 16 ? v < e : (v != 16 || (signed char)shadow[(off >> 4) + 1] < e - 16))
    slow(...);                           /* fails; the slow path reports */
} else
  slow(...);                             /* not in the arena */
```

Every byte of a live object's slot after the object is `0xFF` or the tail
of a partial granule, so two granules both reading `16` belong to one
object, and checking the first and last granule of an access of at most
16 bytes checks all of them. A `live` guard of pointer `a` checks
`shadow[off >> 4] != 0`. A guard of more than 16 bytes, or of a width the
compiler does not know, calls the runtime, which scans the shadow.

#### 2.4 What does not change

Huge blocks (requests no class holds) keep their table and are found by
the slow path. The slot words, the quarantine, the free lists, the Darwin
zone, and the one-runtime-per-process forwarding of RFC 0033 §6.2 stay;
the descriptor a forwarding image copies gains the shadow's address.

#### 2.5 The runtime's guard entry points

The runtime defines every guard the prelude declares (§1.1), as the slow
path does them; the backend's slow-path call is

```c
void __weavec_rt_guard(const void *from, const void *at, unsigned long long w,
                       unsigned kind, const struct __weavec_rt_site *site);
```

which returns when the guard passes and otherwise fails as the mode says:
it traps (calling `__weavec_rt_trapping` first, RFC 0033 §6.1), or, in
report mode, prints `weavec: runtime check failed: <kind> at
<file>:<line>:<column>` and returns. `site` is a constant the backend
emits per report-mode guard, null otherwise.

### 3. The slow path

`__weavec_rt_guard` answers RFC 0033 §4's question for any address. For
an address outside the arena it tests, in order, each with no lock and
in constant time before any search:

1. the huge blocks' address span (two loads) and then their table;
2. the span of the registered globals (two loads) and then their table;
3. the calling thread's live stack, from the caller's frame to the stack's
   top (one thread-local read), and then the stack list;

and an address in none of them is untracked and passes (RFC 0033's rule
for untracked bytes reached from a tracked object stays). The epoch, which
only range caches read, is deleted. `weavecRtForward` becomes an inline
test of a global that the runtime sets once, at start-up, when it forwards.

### 4. Stack objects

A local is registered (RFC 0032 §4) only when a guard can look it up:

- its address, or a pointer derived from it, may be stored in memory, be
  returned, or be passed to a function that is not in the library table;
- or the analysis guards a site of its own function whose operand may
  point into it.

A local whose address is only read through in its own function at sites
that are proven or checked, or handed to library functions (whose rows
the call's own checks and guards cover), is not registered. The decision
uses the engine's points-to facts at the function's guarded sites
(`ObjectPlan` gains them from the planned ledger), and the escape walk the
registration plan already makes. A function's loose marker (RFC 0032
§4.3) is registered only when some local of the function is.

### 5. Library calls

#### 5.1 Through the shadow

The call guards `object_n`, `object_l` and `object_s` check an arena
range by scanning its shadow (one byte per 16 bytes of the range) inline
for ranges up to 64 bytes and in the runtime above; `object_s` finds the
end of its object from the shadow and bounds its terminator search by it.

#### 5.2 Checked wrappers

When a library call's need is one no term can state (RFC 0030 §10.3: the
argument is not a parameter, a local or a field of one), and the call is
one of a fixed family, the emitter replaces its callee with a prelude
wrapper that computes the need at run time and guards it:

| Call | Wrapper's guard |
| --- | --- |
| `strcpy(d, s)`, `stpcpy(d, s)` | `strlen(s) + 1` bytes at `d`; `d` and `s` disjoint |
| `strcat(d, s)` | `strlen(d) + strlen(s) + 1` bytes at `d` |
| `sprintf(d, f, …)`, `vsprintf(d, f, ap)` | the call becomes `vsnprintf(d, room(d), f, ap)`; a result of `room(d)` or more fails |
| `memcpy(d, s, n)` | `d` and `s` disjoint (the `disjoint` check, when its terms cannot be stated) |

`room(d)` is the number of bytes from `d` to the end of the tracked
object it points into, or the maximum when it points into none (the
call then behaves as `sprintf`). A wrapper is used only for a facet that
would otherwise be `unresolved(inexpressible)` or guarded on the wrong
bytes; a call whose need is stated keeps its check.

#### 5.3 The quarantine and zero-initialisation

The quarantine's default budget becomes 16 MiB, held per class in
proportion to the class's share of recent releases, and a class reuses its
most recently released slot past the budget first, so a program that frees
and allocates in a tight loop does not walk cold memory. With the runtime
linked, zero-initialisation's usable-size query is `__weavec_rt_size`,
not the C library's (Darwin's `malloc_size` asks every zone).

### 6. Confirmed errors

#### 6.1 Candidates and the replay

What RFC 0030 §3 and RFC 0033 §1 call a definite finding is now a
*candidate*. After a function's authoritative run, if it published any
candidate, the engine replays the function in *witness mode*:

- a depth-first search over its CFG from the entry, transferring one
  state along one path with no joins, with the same transfer functions,
  summaries and library rows as the authoritative run;
- each loop is taken at most twice on a path, after which the path is
  abandoned as inconclusive;
- each branch edge refines the state as the authoritative run does, plus
  the disequality facts of §6.2, and an edge the refinement empties ends
  the path as infeasible;
- at the site of a candidate, the replay decides the site as the
  authoritative run would, on this path's state.

A candidate is **confirmed** when at least one feasible path reaches its
site, every feasible path that reaches it decides it the same definite
way, and the search finished within its budget (256 paths and 20,000
block transfers per function, both shared by its candidates). Otherwise it
is **unconfirmed**: reported as a warning with the same id and the note
`not confirmed on a feasible path`, and planned as a possible finding of
the same facet (checked, guarded or unresolved).

The replay runs only for functions with candidates, which are rare in
code that builds; its cost is part of the work budget (§7).

#### 6.2 Disequalities

A symbol's record gains the small set (at most eight) of constants it is
known not to equal. The false edge of `x == c` and the true edge of `x !=
c` add `c`; an edge that requires `x == c` for an excluded `c` is
infeasible; a join keeps the constants both sides exclude. This holds in
every run, not only the replay, so the QuickJS path above is infeasible in
the authoritative run too.

#### 6.3 The engine's false definite facts

Each class the fresh projects showed is fixed where the fact is made, with
a case:

- **Rows that disagree.** A summary's rows whose outcomes overlap are
  applied together: a fact the rows give different values (a release
  family, an extent, nullness) takes their join, and a family two rows
  disagree on is unknown, which makes no `mismatched-release`.
- **Exclusive stores of one fresh object.** A fresh object a summary
  stores through two places under different conditions is instantiated in
  the caller as a non-singular object unless the caller decides the
  conditions; releasing a non-singular object makes the other places
  that may hold it *possibly* released, never definitely.
- **Values passed as values.** RFC 0033 amendment 1's value-only rule
  covers a library function's parameter that its row gives no access
  requirement and no effect (`mmap`'s address hint): passing a dangling
  pointer there is no use.
- **Partial unmapping.** `munmap`'s row releases its object only when the
  unmapped range is the whole object; any other `munmap` of it is a
  possible release, which makes no definite finding.
- **Shallow `const`.** An unknown or external callee given a pointer to a
  `const` object may still write through pointers stored in that object:
  the objects they point to are havocked as for a non-`const` argument.
- **A release in a loop left early.** The summary of a function that
  releases an element inside a loop and then leaves the loop by `return`
  or `break` records the release on the exits it reaches (the false
  proof).

#### 6.4 Lowered violations

`-Wno-error=weavec-<id>` lowers a confirmed violation to a warning; its
site gets the check or guard the facet would have as a possible finding,
or none (`unresolved(lowered)`). The prelude's `violation` helper and the
planner's `violationGuard` forms are deleted.

### 7. A bounded analysis

#### 7.1 Work

A run's *work* is the sum, over its block transfers and joins, of the size
of the state transferred or joined (symbols plus cells). It replaces the
transfer count as the unit of every budget:

- `-fweavec-budget=<n>`: work per run (default 2,000,000; 0 unlimited);
- `-fweavec-unit-budget=<n>`: work per unit, shared by its runs as RFC
  0033 amendment 8 shares transfers (default `max(20,000,000, 400 ×
  sites)`);
- a run whose retained entry states hold more than 1,000,000 symbols and
  cells together, or a unit whose runs hold more than 1 GiB, stops as over
  budget.

The defaults are calibrated on the fresh and corpus units to keep every
build within the bound of gate F5; the amendments record the calibration.

#### 7.2 Runs that are not made

- **A component's last round.** When a recursive component's summary
  round changes no summary, the authoritative pass of each member reuses
  that round's run if its inputs are the same, instead of running again
  (each round runs publishing into a buffer that is kept only for the last
  one).
- **Alias-context runs** (RFC 0031 §6.6) are skipped for a function whose
  authoritative run cost more than a tenth of the per-run budget; they
  only add diagnostics.
- **A run cut by its share** of the unit's budget is not run again in the
  same unit pass; its sites take the over-budget defaults.
- **The heap collection** runs at joins and exits only (RFC 0031 §4.6),
  not at the end of every block.

### 8. The driver

- A Clang driver diagnostic that a `-Werror=` option promotes
  (`-Werror=unused-command-line-argument`, which CMake's flag probes use) is
  an error under `weavec-cc` as under `clang`.
- A static archive whose members were built by `weavec-cc` (they carry the
  global-descriptor section) is named in a note, not in the
  `unanalyzed-input` warning: its records are not read, but its code is
  checked and guarded.

### 9. Tests and the corpus

- **Fresh configs** (`set: fresh34` in the manifest): QuickJS, LMDB,
  Janet, brotli, xz, libdeflate, zlib-ng, curl, cmark and libgit2, pinned
  at the SHAs the milestone measured, each with its own build, its test
  suite and a run-time workload (`bench`).
- **Sealed configs** (`set: sealed34`): libjpeg-turbo, opus, flac, giflib
  and wren, pinned now, built with the reference compiler now and with
  WeaveC once, at the end.
- **Detection sets.** The milestone's 61 bug programs and their fixes
  become `test/cases/detection/` (each a bug and a twin; the runner checks
  that the bug stops and the twin does not). A second blind set of at
  least 40 programs is written at the end by someone who has not seen the
  first, and measured once (gate F6).
- Every false stop and every miss above is a case under
  `test/cases/semantics/confirm/` or `test/cases/semantics/runtime/`.
- The runtime test covers the shadow's encoding through allocation,
  in-place resizing, release, recycling and decommitting.

### 10. Deletions

The range caches and the epoch (§1.5), the guard helpers' inline bodies
and `_ool` copies, the `violation` helper and `violationGuard`, the
transfer-count budgets, and the stack registrations of §4 that no guard
can reach.

## Annotation surface

None changes.

## Diagnostics

No id is added or removed.

- Every definite id: an unconfirmed candidate is a warning with the note
  `not confirmed on a feasible path`.
- `unresolved-operation` (under `-fweavec-require=guarded`): `lowered`
  becomes a reason.
- The ledger gains the reason `unconfirmed` for a facet whose candidate
  was not confirmed, and `lowered` for a lowered violation with no check.

## Implementation plan

The work lands on `rfc0034-fast-enforcement` as stages, squashed into one
change:

- **S0 Tests first.** The fresh and sealed configs, the detection cases,
  the confirm cases, and the starting tree measured.
- **S1 Shadow memory** in the runtime (§2), with the runtime's guard entry
  points and the slow path (§3); range caches and the epoch deleted.
- **S2 The backend passes** (§1): declared guards, canonicalisation,
  merging, expansion.
- **S3 Stack objects and library calls** (§4, §5).
- **S4 Confirmed errors** (§6).
- **S5 The bounded analysis** (§7).
- **S6 The driver** (§8).
- **S7 Fresh-project iterations.**
- **S8 Gates, the sealed run, the blind detection set, documentation.**

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference
compiler is `$WEAVEC_LLVM_PREFIX/bin/clang`; darwin-arm64 unless named.
Ratios are of user CPU time, or of instructions retired where the machine
is loaded (the amendments say which).

**Drop-in**

- **F1. Fresh projects.** Every `fresh34` config builds with its own
  build and `CC=weavec-cc` (default flags), and passes its own test suite
  with no trap in trap mode and no failure in report mode. A build that
  stops does so only at definite errors triaged true with source evidence
  (QuickJS's `quickjs.check.o` is expected to be one), which the config
  lowers.
- **F2. Sealed projects.** Run once on the final tree and recorded as
  measured. A sealed config that fails is reduced to cases and fixed, and
  the amendment records that the set was no longer sealed for it.
- **F3. No regression.** RFC 0033's D1 and D3 hold on the final tree (the
  fresh and sealed sets of RFC 0033, the original and held-out configs,
  the injections, every case in normal, `--asan` and `--checks verify`
  runs), with the ratchet rewritten where §7 changes proven counts.

**Detection**

- **F4. Detection set.** Of the 61 programs of `test/cases/detection/`,
  at least 54 stop at or before the faulty access (compile-time error or
  run-time trap), no fixed twin stops, and no bug runs past a facet whose
  outcome is `proven` (no `--checks verify` trap goes unnoticed in the
  default mode).
- **F6. Blind set.** On the blind set written at the end, the default
  build stops at least as many bugs as ASan (`-O1 -fsanitize=address`)
  less two, with no false stop on a twin and no false proof. Recorded as
  measured.

**Cost**

- **F5. Build.** Each `fresh34` config's build within 5 times the
  reference compiler's CPU time, and each original, held-out and RFC 0033
  config within 5 times; `sqlite3.c` within 120 CPU seconds; no
  compilation above 2 GiB peak memory. The ratchet records how many proofs
  the budget gave up.
- **F7. Run.** With the runtime: Lua at most 2.5×, zlib at most 1.5×,
  cJSON at most 1.5× (RFC 0032's G14 benchmarks); every `fresh34`
  workload at most 4× and their geometric mean at most 2.5×. Without the
  runtime, RFC 0033's bounds.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes with the line budget this RFC
  records.
- **H2.** `npm test && npm run build` in `docs/`; the README, the
  guarantees and CLI references, `docs/architecture.md` and the roadmap
  describe the change; this RFC is marked Implemented.

## Implementation amendments

The stages recorded the decisions below as they were implemented. Each
amends the section it names; where the text above and an amendment
disagree, the amendment holds.

1. **The shadow covers the address space (§2.1, §2.3).** One shadow byte
   per 16-byte granule of the whole address space, reserved at start-up
   without backing (2^44 bytes on 64-bit targets) and indexed by
   `(address >> 4) & mask`; the descriptor gains `mask`, which is 0 until
   the reservation succeeds, so a guard before then reads the byte at the
   shadow's base, 0, and asks the runtime. The encoding as built:

   | Value | Meaning |
   | --- | --- |
   | `0` | No live tracked object: untracked memory outside the arena, or a dead, free, quarantined or never-allocated arena slot (`base` and `bytes` tell the two apart). |
   | `1`–`16` | A live heap object's granule (arena slot or huge block): its first `k` bytes are the object's. |
   | `0xFE` | A live heap object's slot after the object. |
   | `0x41`–`0x50` | The last granule of a registered stack or global object that starts on a granule, with `k - 0x40` of its bytes there. |
   | `0x81`–`0xBF` | An earlier granule of such an object, r granules before its last: `0x80 + r` for r up to 48, and `0x80 + 48 + c` (c from 1 to 15) for r at least 2^(c + 4). Two granules are of one object when their distance is at most the earlier one's least r; an object's end, and its start, are a logarithmic number of jumps away (a capped run made each lookup in http-parser's 80 KiB test buffer walk thousands of granules). |
   | `0xFB` | The untracked granule after such an object that ends on a granule (for a global, its padding, amendment 2): an address one past it belongs to it. Not written in a frame with unnamed automatic storage (an `alloca`, a compound literal), which may start there; a local that a guard can reach is registered (amendment 3), so no other neighbour is reached through a guard. |
   | `0xFC` | A granule of a registered object that does not start on a granule (a parameter passed in memory), or the granule after it; the runtime looks it up. It never overwrites an exactly encoded neighbour's bytes, and a granule an exact object shares with a live one stays `0xFC`. |
   | `0xFD` | A released huge block. |

   So a stack or global object's bytes are checked inline too: the inline
   check passes heap bytes as §2.3 says, untracked bytes outside the arena
   reached from untracked memory, and a registered object's bytes reached
   from inside it (two or more granules after a granule hold a whole next
   granule, so a crossing access of up to 16 bytes passes too).

2. **Registered objects on granules (§4, RFC 0032 §4, §5).** A registered
   local is declared `aligned(16)`, a parameter passed in registers too
   (its spill slot), and is entered inline, exactly, by the prelude's
   `__weavec_stack_enter`; the runtime's list holds only the objects that
   do not start on a granule (parameters passed in memory). A registered
   global is aligned to 16 and padded by the `GlobalPadding` pass to whole
   granules, and by one more granule when it fills its last, so that
   nothing the linker places after it (a string literal, an unregistered
   global) shares its last granule or starts at its one-past address. The
   loose marker of RFC 0032 §4.3 is deleted.

3. **Which locals are registered (§4).** As built, the decision is the
   registration walk's: a local whose address reaches anything but a
   library function that takes no callback, whose returned pointer (which
   may point into the argument: `strtok`) the program does not keep, and
   whose call guards none of its arguments. A library call's argument positions the plan guards
   count (`ObjectPlan` takes the plan); a callback argument is a function
   name as passed (decayed), not as written. The engine's points-to facts
   at guarded sites are not used.

4. **Call-argument guards (§5.1).** A call argument's guard of a constant
   need (`memcpy(&v, p, 4)`, an unaligned load) is canonicalised to the
   access guard of that many bytes and expanded inline; one of a run-time
   need calls `__weavec_rt_object`, which scans at most four granules and
   looks a longer heap range up in its slot (constant time). `object_s` is
   unchanged.

5. **Compact guards (§1.4).** A function with more than 1,024 guards gets
   a compact expansion, the arena's bounds loaded at each guard (the
   shadow's base and mask once, at the entry): the whole-
   granule test, then one block that passes untracked bytes, a registered
   object's bytes and a crossing heap access; anything else calls the
   runtime. Thousands of uses of hoisted descriptor values made the code
   generator's common-subexpression pass quadratic.

6. **The quarantine (§5.3).** 16 MiB by default (`WEAVEC_RT_QUARANTINE`
   overrides), one budget for the process; past it, the class of the slot
   being released recycles its oldest dead slot first.

7. **The replay as built (§6.1).** Each block on a path is replayed with
   the authoritative run's evidence for it (the facts its entry state had
   at the fixpoint); a loop is left after two turns from its fixpoint
   state rather than abandoned; a context finding (RFC 0031 §6.6) is
   replayed in its context run. The budgets are as §6.1 says (256 paths,
   20,000 transfers). A spatial or library violation publishes its witness,
   so that a lowered or unconfirmed one keeps its precise check.

8. **Budgets as calibrated (§7.1).** Work per run 20,000,000 (default:
   Lua's `luaV_execute` takes about 10,000,000, and cut short it lost a
   third of `lvm.c`'s proofs, which made Lua without the runtime 1.47 times
   slower); per unit `max(20,000,000, 400 × sites)` as §7.1 says; a run is over budget when its
   retained entry states exceed 1,000,000 symbols and objects, or its graph
   100,000 blocks (curl's generated `lib1521` test has 577,057: the
   pre-passes alone are quadratic in it); a run's share of the unit's work
   is eight fair shares, at least 100,000 (at 20,000 a spent unit cut every
   remaining function, 187 of sqlite3.c's; at 100,000, 84, and the unit
   takes 63 CPU seconds; with no unit budget it takes 412). No unit-wide memory bound is
   kept: the per-run bounds and the releases of amendment 10 hold memory
   down. Of §7.2, only the skipped context runs (a callee whose own run
   took more than 200,000 work) are implemented; the component round reuse
   and the no-rerun of share-cut runs are not.

9. **The driver (§8).** A link input without a record is named in a note
   instead of the `unanalyzed-input` warning when one of its objects names
   a `__weavec_` symbol (the runtime's, the helpers' or a global
   descriptor's): the section is not what tells, an object with no global
   has none.

10. **Compile and link memory (§7.1, gate F5).** The engine keeps no
    unit-wide parent map (`ASTContext::getParents` builds one for the whole
    unit); carried-value liveness is sparse; kind inference drops each
    definition's CFG once done with it; the record's payload is taken when
    the analysis ends and the plan is released once the checks are
    emitted, before the code generator runs; a unit record lists at most
    eight distinct demoting stores per slot; and the link step drops each
    record's JSON once read.

11. **Unknown callees and escaped locals.** A local that a global reaches
    escapes, and every escaped local is havocked at a call of unknown
    code: the code may keep its address when a later call wipes the
    global (libgit2's `test_online_clone`), which made a false definite
    `use-of-uninitialized`.

12. **A cursor's span (RFC 0030 §7.4).** A span check needs one object:
    a pointer that may point into one of several objects of one size gets
    no span witness (it was checked against the first object's span), and
    its access is guarded.

13. **Copies the wrapper checks (§5.2).** `memcpy` and `memmove` get a
    wrapper whose `room` and `source` options check the bytes behind the
    destination and behind the source when no term states the length
    (`(*n - i) * sizeof *a`); the library table gains the `source` option.
    The wrappers' `what` gains bit 4, the source. A wrapper is still used
    only where the call would otherwise have no check or guard.

14. **`%.Ns` arguments (RFC 0033 §5).** A `%.Ns` argument of a literal
    format gets a requirement record that states no need (at most N bytes,
    no terminator), so its spatial facet is unresolved rather than proven,
    and the call's `live` guard covers it: a dangling one traps.

15. **The rewind after `setjmp` (RFC 0032 §4.3).** A returns-twice call's
    first return abandoned no frame, so only a second return (after a
    `longjmp`) clears the shadow below its frame; a large range is cleared
    by mapping the shadow's whole pages afresh. Janet calls `setjmp` for
    every VM call, under GNU make's 64 MiB stack limit, and its event-loop
    test timed out clearing 4 MiB of shadow each time.

16. **Gates as measured (2026-10-05, darwin-arm64, the final tree).**
    - **F1 met.** All ten `fresh34` configs build with `CC=weavec-cc` and
      pass their own test suites in trap mode with no trap and in report
      mode with no report; no definite error stops a build.
    - **F2 met but for opus's build time, which is carried forward with
      F5.** The first run of the `sealed34` set found a false trap in flac
      and a test of opus past its time limit (amendment 18); after their
      fixes, one run of all five: each builds with the default flags,
      passes its tests in trap mode with no trap and in report mode with no
      report, and has no definite error. Build CPU over the reference
      compiler's: flac 2.79, wren 2.76, libjpeg-turbo 3.22, giflib 3.63,
      opus 5.78 (4.42 in a run of opus and flac alone on the same tree),
      most of it in two units (`pitch_analysis_core_FLP.c` 7.5 CPU seconds,
      `celt_encoder.c` 5.6), the work budget's cost F5 records.
    - **F3 met for detection and soundness, with the ratchet rewritten.**
      Every case passes (725, in normal, `--asan` and `--checks verify`
      runs); G9, G10, G11, G12, G15 (85 of 9,588 functions over budget,
      0.89%), R4 and RFC 0033's D1 hold; the ratchet was rewritten from the
      final run, where the work budget moves proofs both ways (sqlite3.c
      gains, small units' counts shift). Two new warning sites in cJSON's
      tests were triaged false (the class already triaged there).
    - **F4 met.** 57 of the 61 programs of `test/cases/detection/` stop
      (54 required), no fixed twin stops, and the four known misses are
      marked in their cases.
    - **F5 not met, carried forward.** Build CPU over the reference
      compiler's: within 5 times for 34 of the 41 configs measured; over
      it: pcre2 13.6, sqlite 9.7 (sqlite3.c alone 63 CPU seconds, under
      120), mujs 7.5, lmdb 7.4, janet 6.0, quickjs 5.8, http-parser 5.6,
      inih 5.5, expat 5.1. Every compile but curl's `libtests.c` (2,271 MiB
      at its peak, the reference compiler 1,250 MiB) stays under 2 GiB. The
      budgets of amendment 8 trade build time for proofs: at a tenth of the
      unit budget the builds met the bound but 3.1% of the functions ran
      over budget (G15) and Lua without the runtime ran 1.47 times slower.
    - **F6 recorded.** On the 48 blind programs of `test/cases/detection-blind/`,
      written after the first set by an author who had not seen it, the
      default build stops 41 and ASan (`-O1`) 44, with no false stop on a
      twin. WeaveC alone stops 2 (an intra-object overflow, a negative index
      past ASan's red zone); ASan alone 5: three stack uses after return
      (RFC 0032 leaves a dead frame's objects untracked; ASan sees them only
      through inlining), a use after free through a borrowed reference at a
      call boundary whose liveness is not guarded, and a never-set pointer
      that zero-initialisation makes null (and free(NULL) harmless).
    - **F7 not met, carried forward.** Run time over the reference
      compiler's: quickjs 4.00, curl 3.51, brotli 3.27, libdeflate 3.16,
      libgit2 2.84, cmark 2.69, zlib-ng 2.39, lmdb 1.77, janet 1.62, xz
      1.54, geometric mean 2.55 (2.5); G14: Lua 2.43 (2.5, met), zlib 1.62
      and cJSON 1.59 (1.5, not met); without the runtime 1.09, 1.00 and
      1.15.
    - **H1 met** with the budget of amendment 17; **H2** with this RFC
      marked Implemented.

17. **Line budget (gate H1).** The engine's limit is raised to 22,000 lines
    and the library's to 72,000, for the guard passes, the replay, and the
    work and memory budgets.

18. **What the sealed run found (gate F2).** Two of the five `sealed34`
    configs failed their first run, so the set was no longer sealed for
    them; each was reduced to a case and fixed:
    - **flac: a false trap from term arithmetic (RFC 0030 §10.2).** A term
      clamped each signed leaf on its own, so `memset(p, 0, count + 8)`
      with `count == -5` (FLAC__MD5Final's padding) needed the maximum and
      trapped; as a have, `n + 5` with `n == -2` was 5. A term inside
      arithmetic is now exact over `long long` and only its result is
      clamped (a negative need is the maximum, a negative have 0); a need
      saturates to `LLONG_MAX` and a have to `LLONG_MIN`, and both stay
      there, so every direction still fails closed. Division is floor
      division with the same saturation. Cases:
      `semantics/runtime/term-signed-sum_{ok,bug}.c`.
    - **opus: a test past its time limit from stack-object entry (§4).**
      `test_opus_extensions` enters two 229 KiB arrays 100 million times;
      the inline enter found each granule's run byte by a search, so a
      million iterations took 82 seconds (the reference compiler 0.45).
      The granules of one run byte are now written with one `memset`, and
      a leave clears a run byte's least run at once (6.3 seconds; most of
      what remains is the zero-initialisation of the arrays, RFC 0033).

## Drawbacks

- **A pass in the backend.** WeaveC's checks have been C that any Clang
  compiles; guards now need WeaveC's LLVM pass to be cheap (without it
  they are correct but out of line). The pass runs in `weavec-cc`, which
  owns the pipeline; a bitcode link of an object built without it calls the
  runtime.
- **More memory for the arena.** The shadow is a sixteenth of the arena's
  used bytes.
- **Fewer compile-time errors**, by design (§6). The detection gates
  measure what is lost; the guards still stop most of those bugs at run
  time.
- **Fewer proofs on giant functions**, by design (§7).

## Alternatives

- **Confirm candidates with Clang's static analyzer.** It is silent on
  all six false errors above, but it also misses five of the seven true
  compile-time errors of the detection set (it does not tie `strchr`'s
  result to its argument's block, so `free(copy); use(eq + 1)` is not a
  use after free to it). The replay confirms with WeaveC's own facts.
- **Make every definite finding a warning.** Simpler, and no false stop
  could come from the analysis; it gives up the errors that are true,
  which the replay keeps.
- **Whole-address-space shadow memory (ASan's layout)** with red zones
  around stack and global objects. Faster for stack and global guards, but
  it needs the frames and data sections laid out again, and stack and
  global guards are under 1% of the executed guards measured.
- **Hardware tagging (Arm MTE, Apple MIE).** Not available on the
  reference machine (`hw.optional.arm.FEAT_MTE` is 0); future work.
- **Keep the C helpers and only make them cheaper.** The cost model shows
  most of the gain comes from the shadow alone, but the opaque helper
  result keeps alias analysis blind, the redundant guards stay, and the
  inlined bodies keep code generation slow.
- **Joins that share structure** (persistent states with stable symbol
  numbers) would cut the analysis's time without giving up proofs. It is
  a redesign of the domain's symbol numbering (2,000–4,000 lines, high
  risk) and is left to its own RFC; the work budget bounds the build now.

## Prior art

- **AddressSanitizer**: shadow memory with a partial-granule encoding,
  instrumentation late in the pipeline, a quarantine.
- **HWASan and Arm MTE**: tag checks of one load and compare.
- **Clang's static analyzer** and **Infer's Pulse**: a finding is
  reported only along a feasible path, and Pulse keeps *latent* issues
  that need a precondition as warnings.
- **SafeStack**: separating the locals whose address escapes from the
  rest; §4 registers only those a guard can reach.
- **Low-fat pointers** (Duck and Yap): object bounds from size-class
  regions, which the arena already has.

## Unresolved questions

- **The hull bound of §1.3** (4,096 bytes) and the replay's budget of
  §6.1 are guesses the fresh projects calibrate.
- **How many true errors the replay leaves unconfirmed** on real code:
  gates F4 and F6 measure it on the detection sets.
- **Whether the work budget's proofs are worth their cost**: the ratchet
  records what it gives up; joins that share structure are the next step
  if it is too much.

## Future work

- Joins that share structure (stable symbols across states).
- Guard hoisting by induction-variable ranges (one guard for a loop's
  `p[i]`, `0 <= i < n`).
- Hardware tagging where the processor has it.
- RFC 0035, adoption: records in object sections (archives, shared
  libraries, LTO), incremental `analyze`, `weavec.toml`.
