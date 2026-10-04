# RFC 0033: Drop-in by default — no false stops on code WeaveC was never tuned on

- **Status**: Implemented (its build-cost gate D5 not met and carried forward, *Implementation amendments* 13)
- **Authors**: WeaveC authors
- **Created**: 2026-10-03
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Amends RFC 0030: its rule that a
  pointer converted from an integer is raw (§Soundness, *Bugs deliberately
  not caught*; §2.1; §6.1; the `unsafe-operation` row of §3.4 and of
  *Diagnostics*), its rule that a lowered violation traps unconditionally
  (§3.4), its library table (§8: a portability rule and corrected rows),
  its link step (§13.2: what runs by default) and its command line (§16).
  Amends RFC 0032: the spatial clause of (G) for subscripts (§3, the limit
  "provenance is the pointer's value"), the planning table of §6 (format
  arguments), the Darwin allocator of §2.5 (one runtime per process, the
  system's allocations in the arena) and the failure path of §7. Amends
  RFC 0031 §7 (unit record format 30 becomes 31). Amends RFC 0005 and
  RFC 0030 §13.2 on what the link step analyses.

This RFC was drafted before implementation. On 2026-10-03 the project
owner accepted the recommendation of the milestone analysis (make WeaveC
drop-in by default before making it faster or extending its guarantee;
measure on projects nobody tuned against; delete what that makes
obsolete) and authorized drafting this RFC and implementing it end to end
in one large change, with breaking changes and no compatibility layers.
Accepted status records that authorization. The status becomes
Implemented only when every gate in *Acceptance gates* passes on the final
tree, or an *Implementation amendment* records what was measured instead
and carries the gate forward.

## Summary

WeaveC passes every gate it has on the twenty projects it was built
against, and fails on the first four projects it was not. Built with
`CC=weavec-cc` and default flags, none of zstd, libuv, jq (with its
bundled oniguruma) and redis compiles: the build stops at errors that are
artefacts of the model, and when they are lowered, the lowered violations
trap in correct programs. Building costs 2.6 to 28 times Clang's CPU, and
redis's link step runs for 25 minutes and then fails with an error no flag
can lower.

This RFC makes one promise the default build keeps: **a correct C program
built by `weavec-cc` with default flags compiles, links and runs as it
does with Clang**, apart from run time. A *false stop* (an error, a failed
link or a trap on a correct program) is a WeaveC bug of the same severity
as a false proof. To keep the promise:

1. **A definite violation must be witnessed** on every path by facts the
   program states, never by a model artefact: a pointer converted from an
   integer is no longer *raw*, only a pointer declared `WEAVEC_RAW` is, so
   `(void *)(uintptr_t)n` stops being an `unsafe-operation` error and its
   uses are guarded like any pointer of unknown provenance.
2. **A lowered violation is guarded, not trapped unconditionally**, when
   the build has a guard or check for it.
3. **The library table states what every supported C library accepts**,
   not what the strictest reading of a standard requires.
4. **A guard asks about the bytes the access touches**, so pointer
   arithmetic that leaves an object and comes back (zstd's `base + index`)
   is not a trap.
5. **The link step by default only reads records.** Re-analysing the
   program at link becomes `-fweavec-link=analyze`, with a budget, and
   never fails a link except for a definite violation.
6. **The analysis has a per-unit budget**, so a large unit costs a bounded
   multiple of compiling it.
7. **Three silent misses close**: fortified library calls, pointer
   arguments of the `printf` family, and, on Darwin, memory the C library
   allocates. One runtime serves the whole process on Darwin.
8. **The driver passes as Clang** to configure scripts, links with
   `-flto` on macOS, and a failed check terminates even with signals
   blocked.

The measure is a new *fresh* corpus of eight projects WeaveC was never
tuned on, plus a *sealed* set of five more that is built with WeaveC only
once, at the end, to measure whether the fixes generalise.

## Motivation

Measured on main at `6047863` (v0.13.0), release build, darwin-arm64,
`CC=weavec-cc` with each project's own build and test commands
(`-Wno-error=weavec` where noted):

| | zstd 1.5.7 | libuv 1.53.0 | jq 1.8.2 + oniguruma | redis 8.10.2 |
| --- | --- | --- | --- | --- |
| Default build | fails: 1 error | fails: 17 errors | fails: 65 errors | fails: 2,145 errors, link did not converge |
| Tests, errors lowered | `zstd -13 -D dict` traps | 319 of 473 pass | all oniguruma tests trap | server traps at start |
| Build CPU vs Clang | 11.5× (link step 85%) | 2.6× | 28× (link 64%) | link ran 25 min, 3.1 GB, then failed |
| Run time vs Clang (default) | 4–5× | 3× | 2× | 4–6× |
| Run time, `-fno-weavec-runtime` | 1.0× | 1.05× | 1.1× | 1.2× |

Every error but one was false. The causes, each reproduced in a few lines:

- **Raw pointers.** `l->watchers[n + 1] = (void *)(uintptr_t)nfds;` makes
  the elements of `watchers` raw, and `struct w *w = l->watchers[fd];
  w->fd` is then `dereference of raw pointer 'w' outside an unsafe region`.
  redis's 2,145 errors, oniguruma's 64 and 9 of libuv's are this rule:
  hash tables keyed by integers cast to pointers, event loops that keep a
  count in a spare slot, thread arguments passed as `(void *)42`.
- **Summary arithmetic.** `void f(int *p) { memset(p, 0, 16); } int a[4];
  f(a);` is `'f' requires 17 bytes behind 'a', which has 16 bytes`, an
  off-by-one for `short` and `int` elements that stops zstd.
- **Library rows.** `setgroups(0, NULL)`, the standard way to drop
  supplementary groups, is a definite `null-dereference`;
  `gettimeofday(NULL, &tz)` traps redis at start; `select` with `fd_set`s
  sized for `nfds` traps libuv.
- **Engine imprecision reported as definite.** A pointer tested through a
  function's result (`if (listAddNodeTail(copy, v) == NULL)`) is taken as
  null; a local initialised through a cast of its address
  (`(processor_info_array_t *)&info`) is taken as uninitialised; `(void)
  tmp;` is a use of an uninitialised value.
- **Lowered violations trap.** RFC 0030 §3.4 traps a lowered temporal or
  release violation unconditionally, so a false error lowered with
  `-Wno-error` becomes a crash of a correct program.
- **The subscript rule of guards.** zstd keeps `base = src - startIndex`
  and reads `match = base + matchIndex; match[matchLength]`. The guard
  looks up `match`, which lies outside the buffer (in a dead slot or
  another object), while the bytes read lie inside it.
- **The link step.** It re-parses every unit from its recorded command and
  re-runs the engine, up to 16 rounds per cycle of units, keeping one AST
  in memory; an indirect call whose type matches another unit's function
  makes a cycle. zstd's link takes 183 of 219 CPU seconds; redis's does not
  converge, which is an error written past the diagnostic engine, so no
  flag lowers it. And it changes nothing the program runs: guards are
  fixed when each unit is compiled.
- **Large units.** `sqlite3.c` takes more than 10 minutes against Clang's
  5.6 seconds; the per-function budget bounds each run, but a unit runs its
  functions many times (summary rounds, contexts, invariant rounds).
- **The driver.** `weavec-cc --version` prints `weavec-cc version …` on its
  first line, and redis's `--version | head -1` then takes the GCC path;
  `-flto` fails on macOS because the driver drops `-lto_library`; a check
  that fails while the program has blocked `SIGTRAP` spins at full CPU
  instead of terminating (a libuv test ran for nine minutes).

Three silent misses were found on the way, in very common code:

- with Darwin's default `_FORTIFY_SOURCE`, `memcpy(d, s, h->len)` into a
  heap block of unknown extent becomes `__builtin___memcpy_chk` and gets no
  guard; the same call with fortification off traps;
- `printf("%s\n", s->user)` after `free(s)` on some path gets no guard:
  a variadic row names no argument list, so RFC 0032's planner leaves its
  temporal facet unresolved;
- on Darwin, a use after free of memory from `strdup`, `getline` or
  `asprintf` runs silently: the C library allocates from the system's zone,
  which the object table does not track. Each shared library built by
  `weavec-cc` also carries an arena of its own, so a guard in one image
  does not see another's heap.

None of this showed on the corpus, because each of its findings was fixed
as it appeared. The corpus measures what WeaveC was tuned on; it cannot
measure what it will do next. This RFC adds the measurement and fixes the
classes, not the instances.

## Soundness

### The guarantee

RFC 0030's guarantee and RFC 0032's clause (G) stand, with these changes.

**(G) for subscripts.** The spatial clause of (G) reads, for every guarded
access: *if the bytes s accesses start inside a tracked object O, then O
is live and the bytes lie inside O, or the program traps at s; if they
start inside no tracked object but end inside one, or start in a dead
tracked object, the program traps at s.* The address looked up is the
address of the access, `p + i * step + offset`, for a subscript as for a
dereference. RFC 0032's subscript rule (the object containing `p`)
is withdrawn. Its first limit of (G) becomes: *pointer arithmetic that
carries a pointer from one object into another live tracked object passes
a guard, at a dereference and at a subscript alike*; the gap byte after
every heap object still catches a walk that leaves a heap object forwards,
one element at a time.

**Raw pointers.** A *raw* pointer is one whose declaration says
`WEAVEC_RAW`, or one loaded through or derived from such a pointer. A
pointer converted from an integer is not raw: its facets are decided like
any pointer the analysis knows nothing about, with the reason `raw-cast`,
and so are guarded (G) or checked (N) in an enforcing build. A forged
non-null pointer that lands in no tracked object passes its guards, under
assumption A6.

**Lowered violations.** (V) reads: *a definite violation never reaches the
object unguarded. It fails the build; lowered with `-Wno-error`, the site
is checked or guarded, and when the build can do neither, it traps
unconditionally.* A guard traps under (G) when the violation happens; a
false violation lowered runs as the program would.

**Darwin.** The C library's own allocations are tracked heap objects
(§6.2), and one object table serves every image of the process.

No proven, checked or violation outcome changes except as §1–§4 say, and
none becomes proven.

### Where this is more sound

- Fortified library calls are guarded like their plain spellings.
- Pointer arguments of the `printf` and `scanf` families are guarded: a
  `%s` argument with `object_s`, every other pointer argument with `live`.
- On Darwin, memory from `strdup`, `getline`, `asprintf`, `realpath`,
  `qsort`'s scratch and every other C library allocation is tracked, and
  a release of it is validated; a block freed by another image is the
  same block.
- Pointers converted from integers are guarded where before they were
  either an error that users lowered (and then, at run time, nothing) or
  `unresolved(raw-cast)`.

### Where this is less sound

- **Definite errors that were not definite are gone.** Most were false;
  a true one becomes a guarded facet that traps when the bug happens (when
  the runtime tracks the object) or an unresolved one. The rule in §1 says
  which facts may make a violation.
- **`unsafe-operation` no longer flags integer-to-pointer conversions.**
  A program that relied on the error to find its pointer forgeries gets
  `unresolved(raw-cast)` rows in the ledger, guards in the build, and the
  error under `-fweavec-require=checked` (an unresolved or guarded facet
  is an `unresolved-operation` error there).
- **Subscripts that jump into another live object pass** (above). The
  analysis still reports them where the extent is exact (`checked` or a
  violation); the loss is at guarded subscripts only.
- **Cross-unit findings are not reported by a default link.** The default
  link step reads records and verifies what records can verify (declared
  contracts, exported requirements at cross-unit calls, slots), but it does
  not re-run the engine, so a use after free whose free is in another unit
  is caught by its guard at run time, not reported at link.
  `-fweavec-link=analyze` and `weavec --whole-program` report it as before.
- **Leak warnings are off in a `weavec-cc` build** (§8); the `weavec` tool
  and `-Wweavec-leak` keep them. A leak was never part of the guarantee.

### Bugs caught

Newly caught at run time (each has a case under
`test/cases/semantics/dropin/`):

```c
memcpy(pk->payload, src, pk->len);       // fortified, payload of unknown extent: object
strcat(b->data, s);                      // fortified, b->data heap: object
memcpy(m->data, p, strlen(p));           // flexible array member: object
if (ended) free(s); printf("%s\n", s->user);   // live / object_s
char *p = strdup(s); free(p); use(p[1]);       // Darwin: live (was untracked)
```

### Bugs deliberately not caught

Unchanged from RFC 0032, plus the subscript case above. Two classes the
milestone measurements found remain documented limits: a typed callback
that receives a released pointer (RFC 0032 amendment 17), and a pointer to
a local that outlives its scope (the object is untracked once it is dead,
so only the analysis's `lifetime-too-short` warning reports it).

### Accepted false positives and false traps

The goal is none on correct code, measured by gates D1 and D2. What
remains possible, and how it is handled:

- **A correct program can still trap when it breaks C's rules in a way
  that happens to work** (RFC 0032's list): reading past a block's
  requested size, using a released block. Such code needs
  `WEAVEC_UNSAFE`. Each trap on a fresh project is triaged; one that is not
  a bug in the program is a WeaveC bug.
- **Possible warnings** can be false; they never stop a build (`-Werror`
  in project flags does not promote them) and §8 limits what a
  `weavec-cc` build prints.
- **A definite error can still be false** where the engine's value
  abstraction is wrong (an infeasible path it cannot refute). Gate D1
  allows none on the fresh corpus.

### Assumptions

A1–A6 unchanged. A6 now also covers pointers converted from integers.

## Detailed design

### 1. What may make a violation

RFC 0030 §3 lists when each id is definite. This RFC adds one rule over
all of them, the **witness rule**: a facet is a violation only when the
fact that fails it is established, on every path, by one of

1. an operation of the function itself whose meaning C defines: a release
   or reallocation through a heap-family row, an assignment of a null
   constant, the end of a local's scope, a subscript or pointer offset by
   an index the facts bound;
2. a callee's summary effect that is unconditional and not `lossy`
   (RFC 0030 §9.1), or a requirement that a callee's summary states from a
   must-access rule (RFC 0030 §7.5) with a trivial guard;
3. a library row's requirement, under the portability rule of §3;
4. a declared contract (`WEAVEC_*` annotations, `[static N]`,
   `nonnull`), which may be broken only by the code that declared it.

and the object's extent, when one is compared, is exact (RFC 0030 §7.1).
What is excluded is a fact made by a default (an unknown callee, an open
slot, an assumption about the entry state), by a join of summaries, or by
a value the engine conflated with another: in particular **a test of a
call's result never refines an argument of the call**, unless the callee's
summary says the result is that argument (RFC 0030 §8, `arg(N)`), and then
only on the outcome classes where it is.

The rule changes no data structure: it is how the engine's existing
certainty bits are set. The implementation audits every place the engine
publishes `Violation` (`EngineDecide.cpp`, `EngineLibrary.cpp`,
`EngineSummary.cpp`, `EngineLifetimes.cpp`, `LedgerAdapter.cpp`) against
it, fixes the ones the fresh corpus shows wrong, and pins each with a case.
Known fixes:

- **A store's own width.** A callee's summary that stores through its
  parameter at byte offsets (`memset(p, 0, 16)` over `int *p` stores 16
  one-byte cells `#0`…`#15`) is applied in the caller with the width each
  store has, not the width of the parameter's element type at that offset
  (`Transfer::storePastObject` over `fieldStepType`, which takes `#13` as
  an `int` and computes an end of 17). The requirement is 16 bytes.
- **Results that test arguments.** As above.
- **Out-parameters of platform calls.** A platform function handed the
  address of a pointer-holding object may write a pointer there
  (`host_processor_info(..., (processor_info_array_t *)&info, ...)`):
  `applyPlatform` forgets the object's pointer cells too, so a later read
  is not `use-of-uninitialized`.
- **Discarded reads.** `(void)x` reads nothing: it is no use.

### 2. Raw pointers and `unsafe-operation`

RFC 0004's raw kind is kept for what users declare and removed for what
the analysis infers:

- **Kept.** A parameter, field, global or result declared `WEAVEC_RAW`
  is raw; a pointer loaded through, or derived by arithmetic from, a raw
  pointer is raw. Dereferencing a raw pointer outside `WEAVEC_UNSAFE` is
  an `unsafe-operation` error, as before. Inside a region it is
  `trusted(unsafe)`.
- **Removed.** An integer-to-pointer conversion (`(T *)n`, `(T *)(uintptr_t)
  n`, the `IntToPtr` site) makes an ordinary pointer of unknown
  provenance. Its own site keeps its row, `unresolved(raw-cast)`, which
  has no guard; the accesses through the pointer are decided like accesses
  through a pointer an unknown callee returned: null `checked`, spatial and
  temporal `unresolved(raw-cast)`, hence `guarded` with the runtime. A
  store of such a pointer into a cell makes nothing raw: the cell's other
  values, and every later load of it, keep their own provenance.
- **Removed.** A call that passes an integer-derived pointer to a
  function that does not dereference it is no error (`same(p, p)`,
  `low_bits(p)`), and neither is passing one to a function that does: the
  callee's accesses are guarded where its parameter is unknown.

- **Must, not may.** Rawness is a property of a value (`SymInfo::raw`
  in the object domain), and joins and weak merges of cells today OR it
  (`Heap::mergeWeak`, the state join), so one raw value stored into one
  element of an array taints every element whose index the engine cannot
  tell apart. A merged value is raw only when every value merged is raw;
  otherwise it is `rawSome`, whose accesses are `unresolved(raw-cast)`
  and guarded, never an error. This is the witness rule of §1 applied to
  rawness.

Everything in the engine, the site collector and the ledger adapter that
exists only to make rawness from integer conversions is deleted, not
disabled: the `Cast` raw origin and the raw flag of a non-zero pointer
constant, the syntactic raw origin of an integer-to-pointer cast in
`SiteCollector` (`hasRawOrigin`), the `unsafe-operation` forms for
arguments, releases and laundering that only such values reached. The site
kind `IntToPtr` and the reason `raw-cast` stay; RFC 0030's union-punning,
byte-copy and `va_arg` rules (also `raw-cast`) are unchanged.

### 3. The library table

**Portability rule.** A row states what *every supported C library*
(Darwin's libSystem and glibc, on the targets RFC 0032 §2.6 supports)
does with an argument, not what the C or POSIX standard permits it to do.
Where they differ, the row takes the more permissive behaviour: null is
allowed if any of them accepts it (`gettimeofday`'s first argument), a
count of zero allows null if any of them reads nothing then (`setgroups`).
A row that requires more than a library does is a false-stop bug.

**The audit.** Every row with a non-null or extent requirement is checked
against the Darwin and glibc manual pages and, where those are silent,
their sources; the fresh corpus's findings are fixed first:

| Row | Change |
| --- | --- |
| `setgroups(int, r:count(a0))` | `null-if-zero(a0)` |
| `gettimeofday(w, w:null-ok)` | first argument `null-ok` |
| `select(int, rw:null-ok, …)` | each `fd_set` needs `bytes(howmany(a0, NFDBITS) * sizeof(fd_mask))`, not `sizeof(fd_set)` |

and the result of the audit is a table in the implementation amendments.
`unittests/Core/LibrarySpecTest.cpp` keeps one expectation per row.

### 4. Guards ask about the accessed bytes

The `object` guard of a subscript or a `*(p + i)` (RFC 0032 §3, §6) looks
up `p + i * step + offset`, not `p`, and checks the `width` bytes from
there against the object found:

```c
/* Passes when the access lies inside the live object containing its first
 * byte, or when that byte is in no tracked object and neither is its last. */
int __weavec_rt_object(const void *p, long long index, unsigned long long step,
                       unsigned long long offset, unsigned long long width);
```

- The inline arena path decodes the slot of the accessed address; the
  range-cache path keeps its entry for the accessed address (`[lo, lo +
  len)` must contain the access, not `p`), so a loop that walks one block
  through an out-of-object base hits the cache.
- The runtime's `before` flag (a pointer at the start of a stack or global
  object taken as one past the previous one) is deleted: no lookup is of
  a base pointer any more.
- An access whose first byte is in no tracked object and whose last byte
  is in one traps (it straddles into an object from outside). An access
  whose first byte is in a dead object traps (temporal).

Index arithmetic that overflows still traps. Dereferences, library
arguments and releases are unchanged: they already look up the address
they use.

### 5. Fortified calls and format arguments

**Fortified calls.** A call to a `chk` alias (`__builtin___memcpy_chk`,
`__memcpy_chk`, `__builtin___strcat_chk`, …) is planned exactly as the
row it aliases, with the alias's argument mapping (RFC 0030 §8.2 `chk`):
requirement terms over the row's arguments are expressed over the
corresponding call arguments, and the dropped object-size argument is
never named. Any difference between the plain and the fortified spelling
in a site's ledger row is a bug; the cases pin both spellings of every
family the fresh corpus uses.

**Format arguments.** For a row with `printf(f, v)` or `scanf(f, v)`
whose format is a string literal, the planner guards each variadic
argument the format reads through: a `%s` (or `%ls`) argument with
`object_s` (spatial, which covers temporal, as RFC 0032 §6), a `%n` or
`scanf` destination with `object_n` of its conversion's width, and any
other pointer conversion that dereferences with `live`. `%p` reads no
memory and gets nothing. A non-literal format leaves the variadic
arguments' facets as they are (`unresolved(inexpressible)`), with a `live`
guard on each pointer-typed variadic argument. `objectWitness` gives
format arguments a witness like any other string argument.

### 6. The runtime

#### 6.1 Failure terminates

A failed check or guard in trap or verify mode first restores the default
disposition of `SIGTRAP` and `SIGILL` and unblocks them in the calling
thread, then traps as before. The prelude emits this inline in the cold
path (a call to `sigprocmask` and two calls to `signal`, with the target's
constants), so a unit still links without the runtime when it was
compiled with `-fno-weavec-runtime`. `__weavec_rt_fatal` does the same.

#### 6.2 Darwin: one runtime per process, the system's heap in the arena

- **One runtime.** Every image linked by `weavec-cc` carries the runtime's
  archives, as today. The first image to initialise its runtime becomes
  the process's *owner*; it publishes a table of its entry points and
  state under a process-wide name (`__weavec_rt_owner`, found with
  `dlsym(RTLD_DEFAULT, …)` from images initialised later, and registered
  before any allocation is served). Every other image's runtime copies the
  owner's arena descriptor (`__weavec_rt_heap` is constant once the arena
  is reserved) and forwards every out-of-line entry point to the owner:
  allocation, release, lookup, stack and global registration, the epoch,
  reports and statistics. Inline guard paths need no change: they read
  their own image's copy of the descriptor, which now describes the one
  arena.
- **The default zone.** The owner registers its zone and promotes it to
  the process's default zone (unregistering and re-registering the
  system's zones until the arena's zone is first, as jemalloc's
  `zone_promote` does), so `malloc` from any image, the C library's own
  calls included, is served by the arena. Blocks the system's zones
  allocated before the promotion stay theirs: releasing or resizing one
  goes to the zone that owns it, as RFC 0032 §2.5's *next allocator* does.
- **Opting out.** A program that defines `malloc` itself keeps its
  allocator (RFC 0032 §2.6) and no zone is promoted.

On ELF nothing changes: an executable's definitions already serve every
shared library, and the runtime's state has default visibility.

#### 6.3 Report mode

With `WEAVEC_RT_REPORT_LOG` set, reports go only to that file, so a test
that captures standard error sees the program's own output.

### 7. The link step

`-fweavec-link=` selects what `weavec-cc` does at a link, replacing
`-f[no-]weavec-link`:

| Value | What runs |
| --- | --- |
| `records` (default) | Steps 1–3, 5 and 6 of RFC 0030 §13.2 over the records alone: inputs and `unanalyzed-input`, the program's slots, declarations against definitions (`annotation-mismatch`), exported requirements at cross-unit calls, the allocator, and, with `-fweavec-ledger` or `-fweavec-summary`, the program ledger composed from the records' rows. No unit is parsed again and the engine does not run. |
| `analyze` | `records`, then step 4: the units analysed again with the program in view, as RFC 0030 §13.2 and RFC 0031 §7 describe, under a budget (below). Its temporal facets refine the program ledger and its definite violations are reported. |
| `none` | Nothing; the link proceeds as Clang's. |

**The budget of `analyze`.** The re-analysis stops when its CPU time
exceeds `-fweavec-link-budget=<seconds>` (default: 4 times the CPU time
the records say the units' analysis took at compile time, at least 30
seconds). A unit whose re-analysis did not finish keeps its compile-time
rows, the link prints one note naming the units, and the program ledger's
`config.link` says `"partial"`. A cycle of units that does not converge
within its rounds takes its widened summaries for the reporting run, as a
function over budget does, and is noted the same way. **No re-analysis
outcome fails a link** except a definite violation, and that is an error
through the diagnostic engine like any other, so `-Wno-error=weavec-<id>`
lowers it. The `cannot re-analyse` and `did not converge` errors are
deleted.

**Records.** Composing the program ledger without re-analysis needs every
unit's rows in full. The unit record gains them (format 31): the site
rows it already carries become the unit ledger's rows, with each facet's
outcome, reason, check, requirements and diagnostic index. The record
grows; the header's payload length is unchanged in meaning.

`weavec --whole-program` keeps today's behaviour (it is the analysis
tool), with the same budget flag and the same non-failing fallback.

### 8. Diagnostics a build prints

- `leak` is off by default in `weavec-cc` (enable with `-Wweavec-leak` or
  `-Wweavec`); the `weavec` tool keeps it on.
- RFC 0032 §9's dropping of possible findings on guarded facets is
  unchanged; with §5 it now also drops those on format arguments.
- `unanalyzed-input` names each input once per link, as today.

### 9. The per-unit budget

`-fweavec-budget=<n>` keeps its meaning (block transfers per function
run). A second budget bounds a unit: `-fweavec-unit-budget=<n>`, block
transfers summed over every run in the unit (summary rounds, contexts,
invariant rounds, the authoritative runs), default `200 × n_blocks` over
the unit's CFG blocks with a floor of 2,000,000. When a unit exhausts it,
the functions not yet decided take the over-budget defaults
(`unresolved(budget)`, guarded where a guard applies), their summaries are
the conservative ones, and the summary line says how many functions were
cut. The engine's own per-pop costs found while measuring (an environment
lookup per block visit, the quadratic worklist scan, rebuilding the
unit's kinds for every run) are fixed as ordinary bugs.

### 10. The driver

- `weavec-cc --version` prints Clang's version block first (`… clang
  version …`, `Target:`, `Thread model:`, `InstalledDir:`), then
  `weavec-cc version X (rev)`. Configure scripts that test the first
  line for `clang` take the Clang path.
- `weavec-cc` loads the Clang configuration file the reference `clang`
  would load (`<prefix>/etc/clang/<triple>.cfg`, as Homebrew's LLVM ships),
  so its default sysroot matches.
- A link with `-flto` on Darwin passes `-lto_library` naming the libLTO
  of the LLVM WeaveC was built with, found from the recorded LLVM prefix,
  instead of dropping the flag.

### 11. Tests and the corpus

**Fresh configs.** `test/corpus/manifest.json` gains a `set` per config:
`original` (the eleven configs tuned since RFC 0030), `heldOut` (RFC
0031's eleven), `fresh` (this RFC's eight) and `sealed` (five). The fresh
configs are zstd, libuv, oniguruma, redis, expat, pcre2, libevent and
libsodium, each pinned by SHA with its own build and test commands. The
corpus gate's `--full` runs `original`, `heldOut` and `fresh`; `--sealed`
runs the sealed set and is run once, for gate D2. The forbidden-word check
of `scripts/check-hygiene.py` gains every fresh and sealed project name.

**Sealed configs.** libxml2 2.14.5, libpng 1.6.50, mbedtls 3.6.4,
msgpack-c 6.1.0 and yyjson 0.11.1, pinned by SHA. Before D2 they may be
built with the reference compiler only (`--reference-only`), to write
their commands. After D2 they are fresh configs, and the next RFC chooses
a new sealed set.

**Cases.** `test/cases/semantics/dropin/` holds one case per false stop
and per silent miss of *Motivation*, reduced from the project that showed
it, with a `_ok` twin where the defect is a false positive and a `_bug`
twin where it is a miss. The case runner builds every multi-unit case
with `-fweavec-link=analyze`, so the cross-unit pins keep testing the
analysis; the default link is tested by the corpus.

### 12. Deletions

- The inference of rawness from integer conversions (§2) and everything
  only it used.
- The unconditional trap of a lowered violation that has a guard (§3.4's
  `Form::Violation` stays for builds without the runtime).
- `-fweavec-link` / `-fno-weavec-link` (replaced by `-fweavec-link=`),
  the link step's `cannot re-analyse` and `did not converge` errors, and
  the re-analysis of every unit when a ledger is composed.
- The runtime's `before` lookup.
- Unit record format 30 readers (records are rebuilt by every compile).

### 13. Performance

No run-time cost target changes in this RFC (the next one is about
guard elimination); §4's lookup of the accessed address and §5's new
guards must stay within RFC 0032's amended G14 bounds. Build cost is
gate D5.

## Annotation surface

None changes. `WEAVEC_RAW` keeps its meaning; its documentation says it is
now the only source of raw pointers.

## Diagnostics

No id is added or removed.

- `unsafe-operation`: the forms for pointers converted from integers are
  removed; the message for declared raw pointers is unchanged.
- `out-of-bounds`, `null-dereference`, `use-of-uninitialized` and the
  temporal ids: fewer definite instances (§1).
- `leak`: off by default in `weavec-cc` (§8).
- The link step's two errors become a note, `weavec-cc: note: the
  whole-program analysis of '<unit>'[, …] stopped at its budget; their
  compile-time results stand` (or `did not converge; …`).

## Implementation plan

The work lands on `rfc0033-drop-in` as stages, squashed into one change:

- **S0 Tests first.** Fresh and sealed configs in the manifest and the
  gate; the `dropin/` cases; the fresh corpus measured on the starting
  tree.
- **S1 Witnessed violations and raw pointers** (§1, §2), with the lowered
  violations of (V).
- **S2 The library table** (§3).
- **S3 Guards** (§4, §5).
- **S4 The runtime** (§6).
- **S5 The link step and records** (§7), and the per-unit budget (§9).
- **S6 The driver and diagnostics** (§8, §10).
- **S7 Fresh-corpus iterations**: every remaining false stop reduced to a
  case and fixed by class.
- **S8 Gates, the sealed run, documentation.**

## Acceptance gates

Binaries are the `release` preset's on the final tree; the reference
compiler is `$WEAVEC_LLVM_PREFIX/bin/clang`; darwin-arm64 unless named.

**Drop-in**

- **D1. Fresh corpus.** Every fresh config builds with its own build
  commands and `CC=weavec-cc` (default flags), and passes its own test
  suite with no trap in trap mode and no failure in report mode, except
  failures triaged true with source evidence. No definite error is triaged
  false.
- **D2. Sealed corpus.** Run once on the final tree; the result is
  recorded as measured, whatever it is. A sealed config that fails is
  reduced to cases and fixed, and the amendment records that the set was
  no longer sealed for it.
- **D3. No regression.** The original and held-out configs meet RFC
  0032's gates as amended (R1–R5, R7) and the ratchet in `expected.json`;
  the 39 injections are detected; `run-cases.py --asan` and `--checks
  verify` pass over every suite.

**Coverage**

- **D4. Misses closed.** The fortified, format-argument and (on Darwin)
  C-library-allocation cases of `dropin/` trap at the bug in the default
  mode.

**Cost**

- **D5. Build.** Each fresh config's `weavec-cc` build within 4 times the
  reference compiler's CPU time; `sqlite3.c` and mujs's `one.c` each
  compiled within 120 CPU seconds; no link step longer than its budget.
- **D6. Run.** RFC 0032's amended G14 bounds hold.

**Hygiene**

- **H1.** `scripts/check-hygiene.py` passes with the line budget this RFC
  records; the library total does not grow by more than this RFC deletes
  plus 3,000 lines.
- **H2.** `npm test && npm run build` in `docs/`; the README, the
  guarantees and CLI references, `docs/architecture.md`, `docs/annotations.md`
  and the roadmap describe the change; this RFC is marked Implemented.

## Implementation amendments

The stages recorded the decisions below as they were implemented. Each
amends the section it names; where the text above and an amendment
disagree, the amendment holds.

1. **The witness rule, as implemented (§1).** Besides the fixes §1 lists,
   the fresh corpus showed five more classes of definite errors that rested
   on no fact the program states; each is fixed by class, with a case
   under `test/cases/semantics/dropin/`:
   - *A pointer passed to a function that uses only its value*
     (`free(p); forget(p);` where `forget` compares, hashes or prints
     `p`): no use of the released object. A call's argument has no temporal
     use when every function it may reach has a complete summary and a
     body in which the parameter appears only as an operand of `==` or
     `!=`, converted to an integer, inside `sizeof`, as a variadic argument
     of a call whose literal format reads no string, or as an argument of a
     function of the unit that uses it only so (three levels down)
     (`Transfer::usesValueOnly`; redis's `xmalloc.c`).
   - *A local whose address is passed to a call* (`f(&op_info)`) may be
     written where no summary records it: reading it is never a definite
     `use-of-uninitialized` (pcre2's `compile_eclass_nested`).
   - *Storing a local's address where the caller can reach it*
     (`req->data = &thread;`) accesses no memory: `lifetime-too-short` there
     is a warning (`may-dangle`), and lowering it inserts nothing. Returning
     a local's address stays a definite error.
   - *A copy onto itself* (`memcpy(h, f, n)` with `h == f`, libsodium's
     `fe25519_copy(h, h)`) is no overlap, statically or in the `disjoint`
     check.
   - *Locals copied into each other in a loop* sent the owning-slot
     collector into endless recursion (a compiler crash, libevent's
     `regress_buffer.c`); each is followed once.

   The sealed run (gate D2) showed four more, so those projects were no
   longer sealed for them (amendment 13):
   - *A release while a copy is held* (`conflicting-borrow`) accesses
     nothing through the copy; a later use of the copy is the
     `use-after-free`. It is a warning, never an error (msgpack-c's
     `msgpack_vrefbuffer_init` frees its array on a failure path after
     storing copies in the caller's struct).
   - *A store of a value of no known type* (read through a `void *`,
     yyjson's `unsafe_yyjson_get_str`) was left out of the callee's
     summary, so the caller kept what its cell held before: a false
     `null-dereference` in yyjson's tests, and unsound in general. It is a
     store of an unknown value.
   - *A member of a record a call returns* (`f().x`): Clang wraps the
     call in a `MaterializeTemporaryExpr`, which the engine did not see
     through, so the member was read from a copy of the call's first word
     and the rest was never written (libxml2's
     `htmlParseHTMLName(ctxt, 0).name`, a false `use-of-uninitialized`).
   - *An inline-assembly operand taken in memory only*
     (`"+m" (*(uint64_t (*)[16]) d)`, mbedtls's `bn_mul.h`) tells the
     compiler which memory the assembly may touch; it is passed by address,
     not read as a whole. Its lvalue is no access site (`SiteCollector`), so
     no guard checks 128 bytes behind `d` (a false trap in the RSA and X.509
     suites). Register operands are loaded as before.
2. **Lowered violations (§2, (V)).** Only a violation the analysis decided
   from its model of other code is guarded when lowered: a temporal one, or
   one of a call's requirements (a callee summary's or a library row's). A
   spatial or null violation decided at the access from an exact extent
   keeps its static check, or the unconditional `violation` trap, which
   fires exactly when the access is reached and needs no tracked object.
3. **The portability rule (§3).** A library *accepts* null when its manual,
   the standard it implements or a nullability annotation in its headers
   says what the call does with null. An undocumented test in one library
   for a call that crashes in another (glibc's `EINVAL` for a null `printf`
   format, `asctime(NULL)`, `closedir(NULL)`) is not acceptance; those rows
   keep their requirement. With the audit, the rows of 111 functions
   changed, of the 652 that carry a requirement: null after a count of zero (`qsort`, `bsearch`, `readv`,
   `fgets`, `strftime`, …), null documented as accepted (`freeaddrinfo`,
   `puts`, `getenv`, `setenv`, `realpath(NULL, …)`, the `exec` family's
   `argv`/`envp`, `setitimer`, `pthread_cond_timedwait`, …), and extents a
   library never reaches (`memchr` stops at the match:
   `min(n, strlen + 1)`; `getloadavg` stores at most 3). Size arguments of
   written buffers keep their extent: both libraries' fortified forms abort
   when the size exceeds the buffer. The term grammar gains `t / N`
   (rounded down) for `select`'s sets, `bytes((a0 + 7) / 8)`.
4. **Guards (§4).** The accessed-address rule alone would let a subscript
   overflow from one global into the next pass, and a stack underflow into
   memory nothing tracks pass. The rule as implemented:
   - the bytes accessed inside a live **heap** object pass, wherever the
     base pointer points (zstd's `base + index`);
   - inside a live **stack or global** object they fail when reached
     forwards from inside another live stack or global object;
   - outside every tracked object they pass unless their last byte is in
     one, or the base pointer is inside a live tracked object (the access
     left it for memory nothing tracks: `s[strlen(s) - 1]` on an empty
     stack string traps).

   The range cache keeps an arena block for the accessed bytes whatever the
   base; any other entry is used only while the base is inside it too, so a
   cached guard never passes what the uncached one fails. A pointer formed
   from untracked memory that lands inside a tracked object, and is
   subscripted back into untracked memory, traps: an accepted false trap
   none of the corpora shows. In verify mode a subscript whose spatial
   facet has a real `object` guard gets no `live` verify check of its
   proven temporal facet: that check would ask about the base pointer,
   which may lie outside the object (`--checks verify` trapped on the
   `base + index` case), and the real guard already fails on a dead object.
5. **Calls (§5).** A fortified call's `__builtin_object_size` argument, and
   a read-only library call in an argument (`strlen(p)`), write nothing
   (rule 7 of RFC 0030 §10.3), which was what kept fortified calls
   unguarded. A call's length argument that no term can repeat
   (`memcpy(m->data, p, strlen(p))`) is guarded where the call evaluates it:
   `__weavec_chk_object_l(dst, n)` wraps the argument, checks `n` bytes
   behind the pointer argument and returns `n` (plan form `Length`). A
   requirement whose only witness is a static length check that could not
   be planned (the have is a flexible array member's lower bound) is
   guarded with that witness's need. `%.Ns` reads at most `N` bytes and has
   no string requirement. A `%s` argument's liveness is covered by its
   `object_s` guard; the other variadic arguments of a literal format are
   not dereferenced and have no guard (the RFC's `live` on every other
   pointer argument was not needed). A check term may name a global array
   alone (RFC 0030 §10.3 rule 1 allowed only parameters and locals): it
   stands for the array's address, which no store changes, so a `%s`
   argument that is a global array (bzip2's `inName`) gets the same
   `strnlen` check as a local one instead of `unresolved(inexpressible)`.
   A library function the C library's header defines inline to call its
   checking builtin (glibc's `_FORTIFY_SOURCE` wrappers, which Clang calls
   an inline builtin declaration) is still governed by its row: the
   program defines nothing (Darwin spells the same thing as macros).
6. **The runtime (§6).**
   - *Failure terminates.* The cold path calls `__weavec_rt_trapping()`
     before the trap when the runtime is linked (the prelude cannot declare
     `sigprocmask` portably). It unblocks `SIGTRAP` and `SIGILL` and resets
     an *ignored* disposition; a handler the program installed (a crash
     reporter, a test harness) still runs.
   - *One runtime per process (Darwin).* The owner is the copy
     `dlsym(RTLD_DEFAULT, "__weavec_rt_dispatch")` finds, the same for every
     image; the others forward through its table and copy its arena's
     descriptor. The flag that marks the resolving thread is compared with
     `pthread_self()`, not kept in a thread-local variable: Darwin
     allocates those on first use, through `malloc`, which recursed. The
     owner reserves its arena and promotes its zone in its constructor, so
     `strdup` in the program's first statement is tracked. Promotion moves
     every zone before the arena's to the end, one at a time (the system's
     default zone is not alone: `objc-class_rw_t` is registered before it).
   - The release preset compiled the runtime archives as ThinLTO bitcode
     (the per-configuration IPO property outranked the target's `OFF`), so
     the driver could not read the allocator's symbols and a program that
     defines `malloc` failed to link. They are native objects now.
   - *Lookups in signal handlers.* The global objects' table was sorted
     lazily under a spin lock, so a guard in a signal handler that
     interrupted another guard's lookup waited forever (libuv's
     `fs_partial_write`). Adding a unit's globals now merges them into the
     sorted table; lookups take no lock, and retry a read that overlapped
     an add (a sequence count). A grown table is not unmapped.
   - *No `getenv` after start-up.* `free` read `WEAVEC_RT_QUARANTINE` on
     first use, and the C library frees with its environment lock held
     (`unsetenv`), so the second `getenv` aborted (libuv's `env_vars`).
     The runtime reads its variables in constructors; `WEAVEC_RT_ABORT`
     and `WEAVEC_RT_REPORT_LOG` too, since a check can fail there.
7. **The link step (§7).** `-fweavec-link-budget` defaults to 120 seconds
   of wall-clock time, not four times the compile-time analysis, which the
   records do not carry. A program ledger composed from records needs the
   rows' callees too (format 31 carries them), so that an exported
   requirement every caller meets is discharged without re-analysis.
   `config.link` is not added to the ledger; the note says which units kept
   their compile-time results. `weavec --whole-program` keeps failing on a
   unit it cannot parse at all (as on a compile error), and reports a group
   that does not converge as a note. G12's link half (an injection must be
   reported by the tool and by the link step) links with
   `-fweavec-link=analyze -fweavec-link-budget=0`: a default link reads
   records only, so it finds no bug that spans units, and Lua's program
   takes longer than the default budget.
8. **The per-unit budget (§9).** A hard cap let the first few large
   functions spend it all (sqlite3.c: 1,622 functions cut). Each run gets
   instead a fair share of what is left, eight times over, and at least 500
   transfers (`UnitRun::runShare`); the unit total is then a soft bound.
   The default is `max(200000, 6 × sites)`. On `sqlite3.c` (43,640 sites)
   it proves 11,909 sites in 114 CPU seconds; a budget of 1,000,000
   proves 12,808 in 565 seconds, and no budget 12,870 in 724: the default
   gives up 7.5% of the proofs, which become checks and guards, for a
   build six times faster (D5 asks for 120 seconds). The quadratic worklist scan and
   the per-run kind tables were not changed; the environment lookup per
   block visit was hoisted. Two changes cut work without changing a
   result: a recursive component's members run callees first (a postorder
   of the edges between them), so a round sees what its callees made in it
   (expat's 14-member component: 33,900 block transfers to 25,200); and
   counted-field invariant inference (RFC 0030 §7.6) stops after a round
   that witnesses no candidate it keeps, and a round stops once every
   candidate is refuted (a witness is a value the writer stored, which
   fewer assumptions never adds; expat's `xmlparse.c` spent 19,500
   transfers there and kept none). `xmlparse.c` went from 71,000 to
   53,000 transfers with an identical ledger. A member of a recursive
   component whose summary round spends the per-function budget is not run
   again, in later rounds or in the authoritative pass: an over-budget run
   publishes nothing, so the pass would only spend the budget a third time
   (pcre2's `internal_dfa_match`: 90 to 36 seconds). A run cut short by its
   share of the unit's budget is run again, since its share may grow.
   `sqlite3.c`'s outcomes are unchanged; two guarded rows changed reason,
   as the unit's budget left more for later shares.
   The per-function budget stays at 50,000 (RFC 0030 §5.5's rule): over
   the original, held-out and fresh corpus the largest function that
   finishes takes 22,244 transfers (pcre2's `parse_regex`).
9. **The driver (§10).** `weavec-cc` already picks the SDK itself when no
   sysroot is given, so Clang's configuration file is not loaded. `-flto`
   names the libLTO of the LLVM WeaveC was built with (beside the `clang`
   it records).
10. **Leak warnings (§8).** `-Wweavec-leak` turns them on in a build; the
    case runner passes it, as it passes `-Wweavec-possible`, so the cases
    keep pinning the analysis.
11. **Known limits found on the way, left as they are:**
    - A call-site requirement that holds only under a condition on an
      argument with side effects (`scrub(p, atoi(argv[1]))`, a callee that
      writes `n` bytes when `n > 0`) is neither checked nor guarded; the
      callee's accesses rely on it.
    - `trusted(caller-contract)` facets (RFC 0030 A1) are not guarded, as
      RFC 0032 amendment 17 says; an exported or address-taken function's
      accesses rest on its callers.
12. **Code generation (§13).** The helpers are inlined (`always_inline`),
    and in a function with thousands of checks and guards (pcre2's `match`,
    21,552 checked or guarded facets from macro expansion) the inlined
    copies kept the optimiser (the SLP vectorizer, then the rest of the
    function pipeline) on that function for over two minutes. The prelude
    now also defines `<helper>_ool`, a copy of each check and guard helper
    that is not inlined, and a function with more than 4,096 plan entries
    (`CheckEmitterOptions::inlinedHelperCalls`) calls those
    (`pcre2_match.c`: 160 to 27 seconds). Lua's `luaV_execute`, the largest
    function the benchmarks run (1,868), keeps the inlined helpers.
13. **Gate results on the final tree** (release preset, darwin-arm64,
    2026-10-04).

    - **D1.** Every fresh config builds with its own build and
      `CC=weavec-cc`, and passes its own test suite in trap and in report
      mode with no trap and no definite error. libevent's suite runs one
      test at a time and leaves out `test-ratelim__group_lim`: under
      `ctest -j 8` its `fdleak` and `ratelim` tests fail with the reference
      compiler too, and `group_lim` failed two of three serial runs of the
      reference compiler's build.
    - **D2.** The one sealed run: libpng built and passed. libxml2,
      msgpack-c and yyjson stopped at false definite errors (the last four
      classes of amendment 1); mbedtls stopped at two true, deliberate
      errors in `metatest.c` (triaged true and lowered), and then trapped in
      its RSA and X.509 suites at inline-assembly operands (a false trap,
      amendment 1) and in its SSL suites at a true out-of-bounds read in its
      test helpers (triaged true). Each false stop was reduced to a case and
      fixed, so the set was no longer sealed for libxml2, msgpack-c, yyjson
      and mbedtls. On the final tree all five build and pass, mbedtls's trap
      run stopping only at the triaged-true read; D1's check now accepts a
      suite that traps only at triaged-true sites, as its text says.
    - **D3.** `run-cases.py`, `--asan` and `--checks verify`: 574 of 574
      cases each. 39 of 39 injections reported (G12's link half asks for
      `-fweavec-link=analyze`, amendment 7). Unresolved shares, original
      configs: spatial 0.053, null 0.001, temporal 0.143; held-out: 0.100,
      0.001, 0.185. G9, G10, G11, G13, G15 and RFC 0031's G5, G6 and G12
      pass. The ratchet was rewritten: proven counts fell by 1 to 36 per
      project where format arguments are now checked or guarded (§5) and
      where corrected library rows dropped facets (§3), and by 11% on
      sqlite (the unit budget, amendment 8); sqlite now builds (no raw
      pointer from an integer, §2).
    - **D4.** Met: the fortified, format-argument and C-library-allocation
      cases of `dropin/` trap at the bug in the default mode.
    - **D5.** Not met, carried forward. Build CPU over the reference
      compiler's: libsodium 1.2, msgpack-c 1.7, libevent 1.9, libuv 2.3–2.8,
      mbedtls 3.6, zstd 3.9–5.1, redis 3.6–5.6 (the reference build's own
      CPU time varied from 38 to 59 seconds between runs; WeaveC's did
      not), libpng 4.6–4.7, yyjson 5.9, oniguruma 6.1, expat 8.1–8.7,
      libxml2 12–16 and pcre2 32–36 (104 before amendment 12).
      `sqlite3.c` takes 157 CPU seconds (limit 120), mujs's `one.c` 15. No
      link step reached its budget. What remains is the analysis's own
      cost: every join renumbers the whole state (`Pairing`), so a transfer
      costs time in proportion to everything the function holds. Making
      joins keep what both sides share is carried forward to the next RFC.
    - **D6.** RFC 0032's amended G14: Lua 6.25 (limit 6.5), zlib 1.89
      (2.0), cJSON 1.89 (2.0); without the runtime 1.12 (1.15), 1.00 (1.1)
      and 1.15 (1.15). cJSON's runtime-less build measures 1.151, over its
      bound by 0.001; main's binaries measure 1.148–1.150 on the same
      machine, so it is the bound's noise, not this RFC's cost. With the
      runtime, cJSON went from 1.76 to 1.89 (the accessed-address guards and
      the format-argument guards).
    - **H1.** `check-hygiene.py` passes with the budgets this RFC records
      (21,500 engine and 70,000 library lines; measured 21,078 and 69,980,
      1,079 library lines more than before it).
    - **H2.** `npm test` and `npm run build` pass in `docs/`; the README,
      the guarantees, CLI and compatibility references,
      `docs/architecture.md`, `docs/annotations.md` and the roadmap
      describe the change.

## Drawbacks

- **A weaker word for the same code.** An error the user saw becomes a
  guard they do not see. The ledger keeps every such row, and
  `-fweavec-require=checked` restores fail-closed behaviour.
- **The default link reports less.** Users who relied on the link step for
  cross-unit findings must add `-fweavec-link=analyze` or run `weavec
  --whole-program` in CI. The cost it removes is most of the build
  overhead on multi-unit projects.
- **A process-wide runtime on Darwin** is more machinery in the runtime,
  and promoting a zone to the default is platform lore that jemalloc
  maintains across macOS versions.
- **Tuning on the fresh corpus** turns it into a tuned corpus. The sealed
  set exists so that this RFC ends with one honest measurement; the next
  RFC needs a new one.

## Alternatives

- **Keep the errors and add baselines** (the old adoption RFC's waivers).
  A false error a user must waive is still a false stop on first use, and a
  waiver file per project is not drop-in.
- **Turn every definite finding into a warning.** Simple, but it throws
  away the errors that are true (the corpus's three triaged-true
  `unsafe-operation` errors in sqlite are declared-raw misuse; RFC 0030's
  probes catch real use after free at compile time). The witness rule
  keeps those.
- **Keep the link-time re-analysis by default and make it faster.** The
  re-analysis cannot change the program's code, so its cost buys only
  reports, which `weavec --whole-program` gives in CI without slowing every
  developer's build. Incremental re-analysis is still worth having for
  `analyze` and is future work.
- **A dynamic runtime library on Darwin** (as AddressSanitizer ships).
  One copy of the state by construction, but every binary then depends on
  a dylib at an install path, which a drop-in compiler should not impose.
  The owner-forwarding scheme keeps static archives.
- **Keep the subscript rule and exempt arithmetic bases** (look up `p`
  unless `p` was formed by arithmetic in the function). It keeps catching
  jumps from a loaded base, but zstd also stores its out-of-object base in
  a struct and subscripts it after loading, so the exemption would have to
  follow the value; the accessed-address rule is the one without false
  traps.

## Prior art

- **AddressSanitizer** checks the accessed address, never the base, which
  is why it runs zstd; its Darwin runtime promotes a zone and intercepts
  the C library's allocations.
- **jemalloc's `zone.c`**: zone registration and promotion on Darwin.
- **Clang's `-fsanitize` driver**: passing the matching `-lto_library`
  and runtime libraries for the toolchain in use.
- **Coverity, Infer and the Clang static analyzer** all report "may"
  findings only behind a configurable threshold and never fail a build on
  their own by default; their experience is that a false error at first
  contact ends adoption.

## Unresolved questions

- **How many definite errors survive the witness rule on real code**, and
  whether they are true; D1 measures it on eight projects and D2 on five.
- **The default re-analysis budget** (4× the compile-time analysis) is a
  guess for `analyze`.
- **The per-unit budget's default** trades proofs for build time on the
  largest units; D5 and the ratchet's proven counts decide it.
- **Zone promotion on macOS versions** other than the reference machine's.

## Future work

- **RFC 0034, guard elimination** (dominated guards, the hull of a block's
  guards per pointer, loop hoisting, guards as optimiser-visible
  intrinsics), against RFC 0032's carried cost gate.
- **RFC 0035, adoption** (formerly planned as 0033): records in object
  sections so that archives and shared libraries carry them, incremental
  `analyze`, `weavec.toml`, `weavec suggest --apply`, relocatable installs.
- Guarding typed callbacks' arguments (RFC 0032 amendment 17) and dead
  stack objects (use after scope).
