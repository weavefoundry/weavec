# RFC 0030: Prove or trap — one safety semantics with compiler-enforced checks

- **Status**: Accepted
- **Authors**: WeaveC authors
- **Created**: 2026-09-18
- **Tracking issue**: TBD
- **Supersedes / superseded by**: Supersedes RFCs 0018–0029 in full.
  Replaces the guarantee statement in RFC 0001, *Soundness*, and
  `docs/pages/reference/guarantees.md`. Amends RFCs 0002–0017 wherever they
  set a default severity, define `annotation-required` or
  `analysis-incomplete`, give unknown callees borrow-only effects, let
  `WEAVEC_UNSAFE` suppress diagnostics, trust `WEAVEC_ASSUME`, or state that
  WeaveC adds no runtime instrumentation (§19 lists each amendment).

This RFC was drafted before implementation. On 2026-09-18 the project owner
accepted every recommendation of the milestone analysis. The owner
explicitly authorized drafting this RFC and then implementing it end to end
in one large commit. The authorization covers four decisions: runtime checks
are on by default in `weavec-cc`, locals and heap allocations are
zero-initialised in enforcing modes, checked mode is deleted outright, and
the mechanical deletion of generated evidence lands in the same commit.
Accepted status records that authorization; it is not a separate review. The
status becomes Implemented only when every gate in *Acceptance gates* passes
on the final tree.

Where this RFC departs from a recorded resolution of the owner's decisions,
because review showed the resolution unsound or infeasible as written, the
text says so at that point with the word **Departure** and the reason.

## Summary

WeaveC records exactly one outcome for every safety facet (spatial, null,
temporal) of every memory operation it compiles: *proven* by the analysis,
*checked* by a compiler-inserted runtime check, a definite *violation* (a
build error), or *unresolved* or *trusted* with a reason from a closed list.
`weavec-cc` turns every unproven spatial or null obligation it can express
into a trapping check, inserted into the AST through Sema before a deferred
CodeGen, with no ABI change and no runtime library in the default mode.
Temporal safety stays static: possible temporal bugs remain visible as
warnings and ledger rows, and only definite violations break builds. Each
translation unit gets a guarantee statement under five named assumptions
and a complete JSON/SARIF ledger; unknown code gets sound defaults,
`WEAVEC_UNSAFE` means trusted raw operations rather than silence,
`WEAVEC_ASSUME` becomes a checked assertion, one declarative `LibrarySpec`
table replaces three library models, and ecosystem attributes discharge
obligations. Checked mode (RFCs 0018–0029) is deleted with its idiom
families, formats, reports, cache, 243 CTest populations and about 283K
lines of generated evidence. The existing `FunctionDataflow` engine stays as
the prover behind an engine-agnostic seam that RFC 0031 can replace, and
library code (`lib`, `include`, `tools`) shrinks from 77.2K lines to at most
62K.

## Motivation

WeaveC v0.10.0 has two modes. Neither delivers the README's promise, and
the costs are measured. The numbers below come from a Release build of
e0e2bd6 on 2026-09-18; S0 imports the probe programs, root-cause repros
and corpus configuration into `test/cases` and `test/corpus`, so each
number can be reproduced.

**The default mode is noisy.** On the 11-configuration corpus (sds, cJSON,
jsmn, log.c, printf, linenoise, cJSON-program, zlib, lua, linenoise-program,
jansson) it reports:

- 301 bug-claiming reports, among them 56 double-free, 32 use-after-free,
  186 null-dereference and 22 leak;
- 40 `annotation-required`;
- 4,239 `analysis-incomplete`, 3,990 of them on Lua.

Hand triage of 62 of the 301 bug-claiming reports found:

- 52 false positives;
- 7 true reports that are only out-of-memory paths;
- 2 benign leaks at exit from `main`;
- 1 boundary case.

Extrapolated, about 92% of the bug claims are false, and none is a bug
upstream would fix.

The causes are structural. Allocator and realloc wrappers lose the
correlation between their result and what they freed (about 46%; jansson's
`jsonp_realloc` alone yields 65 reports). Object and state-machine
invariants cause about 18%, integer–nullness correlation about 12%, array
cells in cleanup loops about 10%, and guard functions such as
`cJSON_IsString` that do not refine their callers about 6%. Because
may-errors are errors, zlib's own `make` fails with 11 null-dereference
errors that look false.

**The default mode is also silently unsound.** 113 soundness probes were
checked: 85 ASan-confirmed bug programs and 28 correct twins. The default
mode catches 36 of the 85 bugs. **42 produce no output at all**: exit 0, not
even an incompleteness warning. The silent bugs include:

- use-after-free through globals, through fields across helpers and through
  `qsort`, thread and signal callbacks;
- `strtok` and `getenv` static state;
- pointers copied byte by byte;
- unknown system APIs;
- 16 spatial bugs: `strcpy` of `argv` into a stack buffer, negative
  indices, malloc size overflow, begin/end walks that overrun by one,
  flexible-array overflow, and others.

The engine already computes that 86% (linenoise), 99% (sds) and 80% (cJSON)
of spatial checks are unresolved, and never says so. Several unsound
choices are deliberate:

- unknown callees are assumed to borrow their arguments with no effects;
- system-header callees are exempt from `annotation-required`;
- library callback arguments are ignored;
- `WEAVEC_UNSAFE` suppresses even a definite use-after-free (probe 45);
- a contradicted `WEAVEC_ASSUME` is trusted (probe c03);
- a lying cross-TU annotation is never verified (probe 38).

The code `weavec-cc` emits is byte-identical to clang's: at runtime nothing
is enforced. RFC 0001 claims completeness, while `guarantees.md` says a
quiet run certifies nothing.

**Checked mode plateaued and is not affordable.** Complete contracts across
RFCs 0020–0028 stopped growing:

| After RFC | 0020 | 0021 | 0022 | 0023 | 0024 | 0025 | 0026 | 0027 | 0028 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Complete contracts (of 1,619 corpus definitions) | 99 | 120 | 124 | 130 | 142 | 148 | 148 | 148 | 150 |

RFCs 0026–0029 added about 157K lines, including data, for +2 contracts.
That is the evidence for not re-deriving any idiom recognizer here. Other
measured costs:

- cJSON.c takes 45–65 s in checked mode (0.8–1.3 s ordinary) and produces
  no object.
- Lua takes more than 1,188 s at 4.7 GB, which is RFC 0029's open blocker.
- Checked mode rejects 17 of the 28 correct probe twins.
- It accepts two definite double frees through aliased globals and fields
  (c10, c10c).
- Enabling `--checked-report` suppresses correct ordinary diagnostics.
- Acceptance is unpredictable: `drop(l)` is complete, but `drop_clear(l)`,
  the same loop plus `l->head = NULL`, is incomplete.

**Adoption breaks at the entry points.**

- `weavec --whole-program -p build`, the README's headline command,
  segfaults (tools/weavec/main.cpp:224-241).
- `-fdiagnostics-format=sarif` crashes both tools.
- Sidecars are not carried into static archives, so Lua's real `make` links
  `liblua.a` and its link step silently sees one unit.
- Checked-mode populations take 82% of Linux CTest time (275 of 334 s) and
  79% of the ASan job's CTest time (907 of 1,143 s); the ASan job as a
  whole runs 41 minutes of its 45-minute limit.
- About 283K lines (18 MB) of generated per-RFC results are committed.

**What works elsewhere.** Every system that has delivered memory safety for
legacy C proves what it can and checks the rest at runtime: CCured, Checked
C, Clang's `-fbounds-safety` (checks pruned by ConstraintElimination), and
Rust's own bounds checks. Purely static tools either change the language,
restrict the domain, or disclaim a guarantee. For WeaveC, imprecision should
cost a branch, not a false error or a silent miss. Measured on Lua, a
trapping null check on every dereference costs 1.02x.

The riskiest mechanism has already been exercised. A prototype deferred
Clang CodeGen until after an analysis pass and inserted checks through Sema.
It produced byte-identical objects for 111 of 111 corpus TUs at `-O2`, at
`-O0 -g` and with `-flto=thin`. Its traps were correct in load, store,
compound-assignment, `++`, `&` and `->` contexts.

The cost of not doing this is another milestone of idiom recognizers on a
mode nobody can run on Lua, while the default mode stays both noisy and
silent.

## Soundness

This section replaces the guarantee statement of RFC 0001, *Soundness*, and
the text of `docs/pages/reference/guarantees.md`.

### The guarantee

Let *U* be a translation unit compiled by `weavec-cc` in an *enforcing
mode*: `-fweavec-checks=trap` (the default) or `verify`, or `report` with
`WEAVEC_RT_ABORT=1` in the environment, with zero-initialisation on (the
default). The guarantee covers every execution of a program containing *U*'s
object code, and every operation site *s* emitted from *U* outside a
`WEAVEC_UNSAFE` region:

- **(S)** If the spatial facet of *s* is *proven* or *checked*, then the
  bytes *s* accesses lie inside the object its pointer operand was derived
  from (§7.4 defines the object). For a *checked* facet, the program instead
  traps at *s* before the access.
- **(N)** If the null facet of *s* is *proven* or *checked*, then *s* does
  not dereference a null pointer. For a *checked* facet, the program instead
  traps first.
- **(T)** If the temporal facet of *s* is *proven*, then the object *s*
  accesses has not been released and its lifetime has not ended. For a
  release site, the object is released at most once and is the start of a
  live allocation of the releasing family.
- **(V)** No *violation* outcome reaches emitted code unguarded. A violation
  fails the compile; if its error is lowered to a warning
  (`-Wno-error=weavec-<id>`), the site is emitted behind a check that traps
  (§3.4).

`-fweavec-checks=report` without `WEAVEC_RT_ABORT=1` is a rollout aid, not
an enforcing mode: a failed check is reported and the access proceeds, so
(S) and (N) do not hold for its checked facets. Its ledger is the same as in
trap mode, because the ledger always describes the enforcing build.

These hold under assumptions A1–A5:

- **A1 — callers.** Callers outside *U* of *U*'s exported and
  address-taken functions pass arguments that satisfy what the callee relies
  on. Each pointer argument is null or satisfies the kind the callee relies
  on: its declared kind, or by default Single, at least one live object of
  its pointee type (§7.3). Declared requirements hold. No two arguments that
  *U* treats as owning refer to the same object.
- **A2 — trusted callees.** Every callee whose effect a facet records as
  *trusted* behaves as its contract says: `system-api`, `library-spec`,
  `extern-contract` or `external-unit` (§2.4). Values stored from outside
  into a function-pointer slot with known targets behave like those targets.
- **A3 — other code maintains the heap invariants.** Code outside *U*
  (other units, libraries, the runtime) leaves every pointer *U* can reach
  through parameters, results and globals in one of two states. It is either
  null, or it points to a live object with at least one element of its type.
  Pointers stored in owning slots are unique. Exact counted-field invariants
  on header structs that *U* relies on hold (§7.6).
- **A4 — no concurrency outside trust.** No other thread, signal handler or
  `longjmp` changes memory that *U* accesses during *U*'s operations, except
  at sites marked for it: `trusted(concurrency)` or `unresolved(setjmp)`
  facets, and the checked facets §5.3–5.4 substitute for flow proofs.
- **A5 — initialisation.** The linked allocator answers the usable-size
  query (`malloc_usable_size`, `malloc_size`) consistently with its
  `malloc`. Pointer-typed memory that *U* reads and that did not come from a
  zero-initialising source (automatic storage whose declaration no jump
  bypasses, static storage, and the lowered allocation family of §11) was
  written before *U* loads it. This covers memory from non-lowered
  allocators, unknown callees and *U*'s own free lists. The ledger counts
  the non-lowered allocation calls *U* makes (§12.1), and the link step
  warns when a unit defines the allocator (§13.2).

The link step (§13) verifies A1 and A3 where the callers and stores are
visible in units with WeaveC records, and prints one line listing what
remains.

**Blame property.** Suppose an execution of *U*'s code performs a
memory-safety violation at site *s* (an out-of-object access, a null
dereference, or a use or release of a released object). Then at least one
of these holds:

1. the program trapped at a *checked* site first;
2. the violated facet of *s* is *unresolved* or *trusted*;
3. the fact that proved *s* was established at a *creation point*, a store,
   a call, a boundary or a function entry whose own facet is *unresolved*
   or *trusted*, or that rests on A1–A5 at an interface of *U*;
4. WeaveC has a bug.

The ledger names the site in cases 2 and 3; in case 3 it is not always the
nearest cause. The ledger records outcomes, not proof dependencies. Case 3
is therefore stated per facet. A spatial or null proof depends only on the
pointer's kind at its creation and on flow facts local to the function
(§7.4); where a kind rests on a default, the call or store that supplied
the value carries a row (§7.3). A temporal proof depends on the function's
entry assumptions and on the boundaries it crossed; a boundary row that
breaks an entry assumption is propagated to every use that relied on it
(§9.4). `-fweavec-checks=verify` is the monitor for case 4 (§10.7).

### Bugs caught

Classes caught at compile time (definite violations are errors):

- use of a released or moved object, double release, release of non-heap or
  interior storage, and a release through the wrong family, when the
  release holds on every path (§3.1);
- dereference of a pointer that is null on every path from a non-allocator
  source;
- an access past an object of exact extent that is out of bounds for every
  value the facts allow, including library calls whose required length is
  known (for example `strcpy` into a smaller constant buffer, and a VLA
  `vla[n]`);
- overlapping `memcpy`/`strcpy`-family copies with known ranges (reported
  as `out-of-bounds`);
- writes into string literals (writable extent zero) and format strings
  whose conversions read more arguments than are passed;
- a contradicted `WEAVEC_ASSUME`;
- a declaration whose annotation contradicts its definition, including
  across translation units at the link step (probe 38).

Classes caught at runtime by a trap at the offending access (the *checked*
outcome):

- every dereference whose null outcome is not proven (unknown, maybe-null
  or allocation-failure values, and uninitialised pointers made null by
  zero-initialisation);
- every index, cursor dereference and library-call length whose extent is
  exact (an array or VLA object, an allocation size in scope, a surviving
  field invariant) or declared, and has a nameable term (§7.1, §10.3).

Temporal bugs on some paths only are reported as warnings with the same id
and "may" wording. Examples: a free inside a loop that a later iteration
repeats (zlib `minigzip`'s `fclose(stdout)`), and a use after a `qsort`
comparator freed the object.

Illustrative rejections (each has a case in `test/cases`):

```c
char *p = malloc(4); if (!p) return; p[4] = 0;           // error: out-of-bounds
free(p); p[0] = 1;                                        // error: use-after-free
if (n == NULL) return n->v;                               // error: null-dereference
char b[4]; int i = 10; WEAVEC_ASSUME(i < 4); b[i];        // error: contradicted-assumption
memcpy(buf + 2, buf, 8);                                  // error: out-of-bounds (overlapping ranges)
WEAVEC_UNSAFE { free(p); p[1] = 2; }                      // error: use-after-free (no longer hidden)
```

and runtime traps:

```c
int a[4]; int i = atoi(argv[1]); if (i < 4) return a[i]; // traps for i < 0
char buf[8]; strcpy(buf, argv[1]);                         // traps when argc < 2 or strlen(argv[1]) >= 8
int first(const char *s) { return s[0]; } ... first(c->name) // traps on NULL, also when first is exported
```

### Bugs deliberately not caught

- **Temporal bugs at runtime.** There is no quarantine allocator and no
  memory tagging. A temporal facet that is not proven is a warning (when
  possible) or an unresolved row. Enforcing it is RFC 0031's temporal
  backstop.
- **Accesses whose extent is not exact or declared, or has no nameable
  term.** Examples are the sds header idiom `s[-1]`, `container_of`,
  cursors stored in unannotated fields, buffers from unknown callees indexed
  beyond element 0, and a `qsort` comparator's `const struct T *a = pa`.
  These are `unresolved(unknown-extent)` or `unresolved(unknown-index)`,
  never checked against a lower bound (§7.1). Closing them needs a
  heap-bounds runtime (RFC 0031) or annotations.
- **Pointers created by reinterpretation.** An integer-to-pointer
  conversion makes an RFC 0004 raw pointer: using it outside an unsafe
  region is an `unsafe-operation` error, as today, and inside one it is
  `trusted(unsafe)`. Union punning, byte-wise or partial copies of pointer
  objects, and `va_arg` of pointer type are not raw under RFC 0004; uses of
  such pointers are `unresolved(raw-cast)`. The null check still runs, but a
  forged non-null pointer passes it.
- **Uninitialised scalars.** They are outside the three facets. Enforcing
  modes define them as zero, which neutralises the class (probes 08, 32);
  with `-fno-weavec-zero-init` they are not covered.
- **Data races and asynchronous signals.** These are assumption A4. For
  places shared with thread or signal entry points (not `atexit` handlers,
  §5.3), temporal facets are `trusted(concurrency)`, and null and spatial
  facets that only flow facts would prove are checked instead.
- **`setjmp`/`longjmp`.** Every temporal facet of a function that calls
  `setjmp` is `unresolved(setjmp)`. A `longjmp` into a dead frame is not
  detected.
- **Leaks.** A leak is a warning and never part of the guarantee.
- **Integer overflow as such, type confusion that stays within an object,
  and floating point.** RFC 0017's `invalid-integer-operation` is
  unchanged. A size overflow is caught only where it leads to an
  out-of-bounds access (probe 06 traps).
- **Inline assembly.** An `asm` statement applies the unknown-callee
  default to its pointer operands (§5.7). Its own memory accesses are
  outside the model.
- **Code without WeaveC metadata** (archives, shared libraries, objects
  compiled by another compiler). Its calls are `trusted(external-unit)`,
  and the link step names each such input once (`unanalyzed-input`).

### Accepted false positives and false traps

- **Definite errors are designed to be true.** A definite error needs a
  record on every path that no lossy summary produced (§3.1, §9.1), a null
  value from a non-allocator source, or a bounds verdict against an exact
  extent that fails for every value. The remaining source of false errors
  is engine imprecision in values: a fact that is must in the abstraction
  but not in the program, such as an infeasible path the engine cannot
  refute. Gate G9 allows at most 10 definite errors on the corpus, each
  triaged true.
- **Possible temporal warnings can be false.** Examples are correlated
  conditions (`if (c) free(p); … if (!c) use(p);`, probe 41e) and
  state-machine invariants. They are warnings, which `-Werror` in project
  flags does not promote. Gate G10 caps them at 60 on the corpus, every one
  triaged.
- **False traps.** A trap in a correct program needs a requirement or
  invariant tighter than the real one. Six rules prevent this:
  - a check compares only against an exact extent or a declared kind, never
    against a lower bound such as the Single default (§7.1);
  - the extent of a pointer made from a member or element is the whole
    object, not the sub-object, and trailing arrays are flexible (§7.4);
  - parameter requirements come only from *must-access* rules, guarded by
    loop entry and void when a call before the access may not return
    (§7.5);
  - field invariants must be exact, and are dropped on any write the
    analysis cannot see (§7.6);
  - `a + i`, `&a[i]` and conversions outside required positions are never
    checked;
  - inferred requirements are enforced at call sites only for static
    functions.

  A correct program can still trap when it breaks C's rules in a way that
  happens to work. Examples are reading one element past an array that is
  followed by known memory, and dereferencing a pointer built from an
  integer inside a struct hack. Such code needs `WEAVEC_UNSAFE`. Gate G11
  requires 0 traps in every corpus project's own test suite.
- **Zero-initialisation changes behaviour only for programs that read
  indeterminate values.** Such programs are already undefined in C.

### Where this is less or more sound than v0.10.0

More sound:

- unknown callees, callbacks, `setjmp` and system APIs no longer default to
  silent success;
- `WEAVEC_UNSAFE` no longer hides definite violations;
- `WEAVEC_ASSUME` is checked;
- cross-TU annotations are verified at link;
- a use through a pointer that may alias a released object is no longer
  proven (§3.1);
- every unproven null or spatial obligation is either checked or listed.

Less sound in two respects:

- Possible temporal findings are warnings rather than errors, so a build
  that used to fail on a real path-dependent use-after-free now succeeds.
  The ledger and the warning keep the finding visible, and
  `-fweavec-require=checked` restores fail-closed behaviour.
- With the `weavec` tool, or with `-fweavec-checks=none`, possible null and
  spatial findings that v0.10.0 reported as errors ("may be null", "may be
  out of bounds") produce no diagnostic and no check. They are counted as
  "checkable (not enforced)" in the summary line and the ledger, and
  `-fweavec-require=proven` turns them into `unchecked-operation` errors.
  `weavec` alone is therefore not a bug finder for these classes; the
  checks in a `weavec-cc` build are.

This trade is deliberate: 92% of today's errors are false, and a build
that cannot pass is not a guarantee.

### Projected outcomes on the soundness probes

The projection below classifies the 85 probe bugs; gate G4 measures the
actual outcomes.

| Class | Probes | Count |
| --- | --- | --- |
| Reported today, still reported (error or warning) | the 36 caught today, plus 26 and 41 (mislabelled today) | 38 |
| New: definite error | 09 (call-site requirement), 19, 20b, 21, 23, 25, 25b, 38 (link), 45, c03, c06, 18 (format arity), 46 | 13 |
| New: runtime trap | 06, 07, 07b, 20, 24, 29, 31, 36, 39, 49, c09, c12 | 12 |
| New: possible warning | 25c (`setenv` invalidates `getenv`), 47 (`qsort` comparator) | 2 |
| Non-silent ledger row only | 02, 02c, 02d (dangling-escape, propagated to the bug site, §9.4); 10, 17, 42, c04, c14 (raw-cast); 14 (unknown-callee); 14b, 37, c13 (unknown-extent); 14c (caller-contract); c01 (inexpressible); 15, 15b (concurrency); 16, 16b (setjmp) | 18 |
| Neutralised by zero-initialisation | 08, 32 | 2 |

The projected total reported is 65 of 85, with 0 silent. The gate floor is
55, against 36 today.

## Detailed design

### 1. Architecture and pipeline

Components by layer. New files are listed without "(edited)":

| Layer | Component | Role |
| --- | --- | --- |
| Core | `Ledger.h`/`.cpp` | `Site`, `Facet`, `SiteOutcome`, `UnresolvedReason`, `TrustReason`, counts, rollup, summary line text |
| Core | `PointerKind.h`/`.cpp` | the kind lattice and extent terms (§7.1) |
| Core | `LibrarySpec.h`/`.cpp` + `LibrarySpec.txt` | the declarative library table and its parser (§8) |
| Core | `CheckPlan.h` | check templates and operand terms as pure data (§10.1) |
| Core | `FnSlots.h`/`.cpp` | function-pointer slot constraints and solver (§9.3) |
| Core | `Summary`, `SummaryIO` (edited) | summary format 27: checked languages removed; kinds, reliance flags, lossy bits and outcome cases added (§7.3, §9.1) |
| Core | `Moves`, `Nullness`, `Borrow` (edited) | certainty bits (§3) |
| Analysis | `AttributeReader` | declared kinds from attributes and annotations (§7.2) |
| Analysis | `KindInference` | slot kinds, parameter kinds and reliance, must-access requirements, field invariants (§7.3–7.6) |
| Analysis | `SiteCollector` | syntactic enumeration of every site before any dataflow (§2.6) |
| Analysis | `SlotCollector` | per-TU function-pointer slot constraints (§9.3) |
| Analysis | `BoundaryInvariants` | dangling-escape and second-owner decisions and their propagation (§9.4) |
| Analysis | `CheckPlanner` | checked requirements → `core::CheckPlan`, and the expressibility decision (§10) |
| Analysis | `LedgerAdapter`, `SafetyEngine` | the engine seam (§14) |
| Analysis | `DataflowEngine` | `SafetyEngine` implemented over `FunctionDataflow` |
| Frontend | `DeferredCodeGenConsumer` | buffers CodeGen callbacks until the analysis and rewrites finish (§10.5) |
| Frontend | `CheckEmitter` | Sema-built AST rewrites from `CheckPlan` (§10.6) |
| Frontend | `Prelude` | the helper definitions injected into predefines (§10.2) |
| Frontend | `ZeroInit` | allocation lowering and `-ftrivial-auto-var-init=zero` (§11) |
| Frontend | `LedgerWriter` | JSON and SARIF writers and fingerprints, over `llvm::json` and `llvm::SHA256` (§12) |
| Frontend | `UnitRecord` | format-28 framing and payload codec, over `llvm::json` and `llvm::SHA256` (§13) |
| Frontend | `ProgramAnalysis`, `Sidecar`, `Driver` (edited) | link verification, program ledger, CLI |
| runtime | `runtime/weavec_rt.c` | about 100 lines, linked only for `-fweavec-checks=report` |
| runtime | `runtime/weavec_chk.c` | out-of-line copies of the helpers, linked only for PCH and module builds (§10.9) |

The writers and the record codec live in Frontend because Core may not use
LLVM, and a Core JSON and SHA-256 implementation would duplicate LLVM's.

The components in the Analysis rows above `LedgerAdapter` read ASTs, the
`LibrarySpec` and the ledger; they never include `Dataflow.h`. Only
`DataflowEngine.cpp`, `FunctionAnalysis.cpp`, `CallbackSummaries.cpp`,
`CallContextSummaries.cpp` and the `Dataflow*.cpp` files may include it.

The `weavec-cc` compile step for one C translation unit runs as follows:

1. Clang parses the unit. `DeferredCodeGenConsumer` forwards Sema set-up
   and records every CodeGen callback without running it.
2. At `HandleTranslationUnit`:
   - `AttributeReader` and the syntactic part of `KindInference` compute
     declared kinds, slot kinds, parameter kinds and must-access
     requirements. None of this uses engine facts.
   - `SiteCollector` enumerates the sites of every emitted function. It
     runs after the kinds, because PtrArith and Cast sites exist only in
     required positions (§7.4).
   - `SlotCollector` collects the unit's slot constraints and solves them
     locally.
   - The engine runs through `LedgerAdapter`: `DataflowEngine`, i.e.
     `TranslationUnitAnalyzer` over `FunctionDataflow`.
   - `BoundaryInvariants` and the field-invariant part of `KindInference`
     consume the facts the engine published.
   - `LedgerAdapter::finish` fills the default outcomes, propagates
     boundary rows, runs `CheckPlanner::plan` to decide which checked
     requirements are expressible (§10.3), and produces the unit `Ledger`
     and its `CheckPlan`. This happens in every mode and in `weavec`, so
     the ledger never depends on whether checks are emitted.
3. Definite violations are reported as errors and possible temporal
   findings as warnings. Under `-fweavec-require`, `unresolved-operation`
   and `unchecked-operation` errors are added, from the planned ledger.
4. If an error was reported, the recorded callbacks are replayed unchanged
   and CodeGen drops the module, as today. Otherwise, when checks are on,
   `CheckEmitter` applies the `CheckPlan`, and then the callbacks are
   replayed. A rewrite that Sema rejects at this point is a WeaveC bug: it
   is reported as an internal error that fails the compile (§10.6), never
   silently dropped.
5. The object is written. The format-28 record is written to
   `<object>.weavec`, the unit ledger to `-fweavec-ledger` if given, and
   the summary line to stderr under `-fweavec-summary` or `-fweavec-ledger`.

The link step (§13) reads the records and solves function-pointer slots
program-wide. It verifies declarations and exported requirements across
units, then re-runs the engine for temporal refinement. Finally it writes
the program ledger.

`weavec` (the libTooling tool) runs the same steps 1–3 and 5, without
CodeGen, per source or as one program with `--whole-program`.

Refinement is split by facet. Spatial, null and assertion outcomes are
decided once per TU, because they decide the emitted code, and the link
step copies them verbatim. Temporal outcomes are refined at link.

### 2. Sites, facets and outcomes

#### 2.1 Site kinds

A *site* is one operation in one emitted function. Its identity is
`{function, ordinal, kind, location}`, where the ordinal is the site's
0-based position in source order within the function.

| Kind | JSON | Created by | Facets |
| --- | --- | --- | --- |
| Deref | `deref` | `*p`, `p->f`, and `p[0]` with an integer constant zero index, where `p` points to an object type (dereferences of function pointers are Call sites) | spatial, null, temporal; only null when the lvalue is used solely as the operand of `&` or decays to a pointer (`&p->f`, `p->arr`) |
| Index | `index` | `e[i]` other than the Deref case; `*(p + i)` and `*(p - i)`; subscripts of array-typed lvalues | spatial; plus null and temporal when the base is a pointer value; no site when the lvalue is used solely as the operand of `&` (`&p[i]` is PtrArith) |
| PtrArith | `ptr-arith` | `p + i`, `p - i`, `++p`, `p++`, `&p[i]` whose result flows directly into a *required position* (§7.4) | spatial |
| Cast | `cast` | a conversion from `T *` to `U *` (explicit or implicit, including from `void *`) where `U` is a complete object type wider than `T` (`void` and character types count as size 1), whose result flows directly into a required position (§7.4); a store of a pointer into a slot with a declared kind | spatial |
| IntToPtr | `int-to-ptr` | a conversion from an integer to a pointer other than a null pointer constant (an RFC 0004 raw origin); `va_arg(ap, T *)` | spatial, temporal |
| LibCall | `lib-call` | a direct call whose callee resolves to a `LibrarySpec` entry (§8, including its aliases, and only when the program does not define the function), or an indirect call whose solved slot contains such a function (§9.3), where the entry has no release effect | spatial, null and temporal, as the entry's requirements name them; the temporal facet also carries the boundary invariants when the entry has a callback clause (§9.4) |
| Release | `release` | a call whose `LibrarySpec` entry releases or reallocates an argument (`free`, `realloc`, `fclose`, `freeaddrinfo`, …) | spatial (start of allocation), null (when the entry forbids null), temporal (live, of the releasing family) |
| Call | `call` | every other call expression, direct or indirect; and every function exit (each `return`, the end of the body, and each call to a function that does not return: `exits` or `noreturn` in the table, or declared `noreturn`), distinguished by `boundary: "call"` or `"exit"` | temporal; null on the callee operand of an indirect call; spatial and null for each argument with a declared requirement, an enforced inferred requirement (§7.2, §7.5) or a parameter whose callee relies on its default kind (§7.3) |
| Assume | `assume` | each `WEAVEC_ASSUME(e)` | assertion |
| Raw | `raw` | a dereference, subscript or release through a pointer with an RFC 0004 raw origin: declared `WEAVEC_RAW`, converted from an integer, loaded through a raw pointer, or handed out as raw by a callee | spatial, null, temporal |

A facet applies only where it has meaning:

- The null facet exists only when the operand is a pointer value, not an
  array lvalue.
- The temporal facet exists only when the pointed-to object's lifetime can
  end before the site. An array lvalue of automatic storage in scope, or of
  static storage, has none.

`&*p` and a member designation on a null pointer constant (the old
`offsetof` idiom, `&((T *)0)->f`) create no site. Sites in unevaluated
operands (`sizeof`, `_Alignof`, `typeof`, unselected `_Generic` branches)
do not exist, except in operands of variably modified type, which C
evaluates. Sites in constant expressions (file-scope initialisers, case
labels, array bounds) are decided statically. They can be *proven* or a
*violation*, and are otherwise `unresolved(inexpressible)`; they are never
*checked*. Sites whose operand is a pointer into a non-default address
space, and sites inside the shared operand of a `BinaryConditionalOperator`
(`a ?: b`) or another `OpaqueValueExpr` source, are
`unresolved(inexpressible)` for every facet a check would serve, because the
helper cannot take the operand or the operand cannot be replaced in place.
`SiteCollector` walks the semantic form of initialiser lists and
deduplicates sites by `Stmt *`, so shared subtrees are counted once.

The **assertion** facet is used only by Assume sites; the three safety
facets are spatial, null and temporal. **Departure:** the resolutions name
three facets. An assumption is not a memory access, and folding it into
one of the three would make the per-facet counts meaningless, so it gets a
fourth facet that no other site kind has.

#### 2.2 Outcomes

`core::SiteOutcome` (the name `core::Outcome` is taken by the RFC 0006
result classes):

| Outcome | JSON | Meaning |
| --- | --- | --- |
| Proven | `proven` | The facet holds on every execution of an enforcing build, under A1–A5 |
| Checked | `checked` | Not proven. A check at the site traps before the operation if the facet fails. The check is emitted when checks are enabled; with `-fweavec-checks=none` or in `weavec`, the ledger still says `checked` and the summary line says "checkable (not enforced)" |
| Violation | `violation` | Fails on every execution that reaches the site; always paired with an error diagnostic (a warning only when lowered by `-Wno-error`, in which case the site traps, §3.4) |
| Unresolved | `unresolved` | Neither proven nor checkable; carries an `UnresolvedReason` |
| Trusted | `trusted` | Holds if a named trust assumption holds; carries a `TrustReason` |

#### 2.3 Unresolved reasons (closed list)

| Reason | Meaning |
| --- | --- |
| `unknown-extent` | No exact extent or declared kind (§7.1) covers the access for the object the pointer points into. A lower-bound kind (Single, a default, an inferred requirement) covers only the elements it guarantees |
| `unknown-index` | The object's extent is known but the pointer's offset from the object's start is not (the base is not identifiable) |
| `inexpressible` | The analysis knows the obligation's terms, but no check can be written at the site: a term has no C name there (it lives only in a snapshot place), is volatile, needs a function call other than the prelude's, is wider than 64 bits, names a place the call's arguments or the wrapped operand may write, or the site is in a constant expression or a shared operand (§2.1, §10.3) |
| `may-released` | The object may have been released on some path reaching the site; always paired with a possible-warning diagnostic |
| `may-moved` | The value may have been moved out on some path; paired with a warning |
| `may-alias-released` | Earlier on some path, an object was released that the site's pointer may alias: the pointer was loaded from a place the analysis cannot prove distinct from the released one (§3.1); no diagnostic |
| `may-invalid-release` | A Release site may release a non-heap, literal or interior pointer on some path (spatial facet); paired with an `invalid-release` warning |
| `may-mismatched-release` | A Release site may release an object of another family on some path (temporal facet); paired with a `mismatched-release` warning |
| `may-dangle` | A `return` or store at a Call exit boundary may let a value outlive its storage on some path (temporal facet); paired with a `lifetime-too-short` warning |
| `may-conflict` | A move or release may conflict with a loan that is live on some path (temporal facet); paired with a `conflicting-borrow` warning |
| `unknown-callee` | An earlier call to a function with no body, summary, `LibrarySpec` entry or declared ownership contract (or an `asm` statement) may have released, retained or replaced the value (§5.1) |
| `callback` | As `unknown-callee`, for an indirect call through a function-pointer slot with no known target (§9.3) |
| `setjmp` | The function calls `setjmp`; values may be stale after a `longjmp` (§5.4) |
| `budget` | The function exceeded its analysis budget; default outcome (§5.5) |
| `unanalysed` | The engine did not reach the site or cannot model the construct (the old "unsupported …" and internal-limit incompleteness reasons) |
| `raw-cast` | The pointer was created by reinterpretation that RFC 0004 does not mark raw: `va_arg` of pointer type, a union member whose last visible write was a non-pointer member, or a byte-wise or partial copy of a pointer object. Also the outcome of an IntToPtr site's own facets outside an unsafe region |
| `dangling-escape` | At this boundary, a place reachable from a parameter or global may hold a released pointer or a pointer to storage whose lifetime has ended; or the site relies on the entry assumption for a place class that such a boundary breaks elsewhere (§9.4) |
| `second-owner` | At this boundary, two owning places reachable from parameters or globals may hold the same owned object; or, as above, the site relies on an owner-uniqueness assumption a boundary breaks (§9.4) |
| `no-zero-init` | The pointer may be uninitialised: zero-initialisation is disabled, a jump can bypass the variable's declaration, or the storage came from an `alloca` that was not zeroed (§11) |

#### 2.4 Trust reasons (closed list)

| Reason | Meaning |
| --- | --- |
| `unsafe` | The site is inside a `WEAVEC_UNSAFE` region (§6.1): its spatial and null facets, and every facet of a Raw or IntToPtr site (the only temporal use of this reason) |
| `system-api` | The facet depends on a function declared in a C library, POSIX or platform header (§5.2), and not in the `LibrarySpec`, having borrow-only effects, or on such a header's attributes |
| `library-spec` | The facet depends on a `LibrarySpec` statement about the callee's own behaviour: a `static(S)` or `interior-state(S)` result, a `retain`/`reads`/`invalidates` relation, or a callback clause (§8.2) |
| `extern-contract` | The facet depends on the declared ownership contract of a function defined outside the TU (§5.1), or on external values in a function-pointer slot behaving like its known targets (§9.3) |
| `caller-contract` | The facet depends on an inferred extent requirement of an exported or address-taken function that callers outside the TU must meet (A1, §7.5). Never used for nullability |
| `external-unit` | The facet depends on code in a link input that has no WeaveC record |
| `concurrency` | The temporal facet concerns an object shared with a thread entry point or signal handler, or a null or spatial facet of such an object has no expressible check (§5.3) |

Adding a reason to either list requires an RFC.

#### 2.5 Merging and rollup

Within the one authoritative pass over a function (§2.6), the engine may
record the same facet of the same site more than once, for example from two
paths in the final pass or from two requirements of one library call.
Records merge by rank:

> violation > unresolved > checked > trusted > proven

Merging by rank applies only within that pass. `LedgerAdapter::beginFunction`
discards every row an earlier pass recorded for the function, so a stale
decision from a superseded run never survives (§14).

Each record is kept in the row's `requirements` list with its own outcome
(§12.1), and checks are planned per record, not per merged facet: a checked
requirement gets its check even when another requirement of the same site
is unresolved (§10.1). `char dst[16]; memcpy(dst, src, n);` with an unknown
source extent therefore checks `n <= 16` for the destination, while the
merged spatial facet is `unresolved(unknown-extent)`. The *site outcome*
used by the summary line is the highest-ranked outcome among its facets.

#### 2.6 Completeness by construction

The ledger is complete by construction:

1. `SiteCollector` runs before the engine. It walks the body of every
   *emitted* function and assigns ordinals. Emitted functions are the
   definitions, not in system headers, whose body CodeGen may emit in any
   form: those `ASTContext::DeclMustBeEmitted` accepts, referenced
   internal-linkage definitions, and C99 `inline` and `gnu_inline`
   (`extern inline`) definitions, which CodeGen emits
   `available_externally` and inlines at `-O1` and above. All of them are
   analysed and instrumented, whether or not they are referenced; ledger
   rows are kept for the ones that are externally visible or used.
2. The engine publishes decisions for the sites it reaches (§14). A
   decision for an expression that `SiteCollector` did not enumerate is an
   internal error: a debug-build assertion and a release-build
   `unresolved(unanalysed)` row in the enclosing function.
3. `LedgerAdapter::finish` fills every undecided facet with its default:

| Facet | Undecided, function analysed | Function over budget |
| --- | --- | --- |
| null | `checked` (`nonnull` is always expressible, §10.3) | `checked` |
| spatial | `checked` if the extent is exact from the type or declared, and its terms are constants, `const` locals and parameters that are never assigned or address-taken, else `unresolved(unanalysed)` | the same, with `unresolved(budget)` |
| temporal | `unresolved(unanalysed)` | `unresolved(budget)` |
| assertion | `checked` | `checked` |

Without engine facts, rule 4 of §10.3 (the term's places are unmodified
since the extent was derived) can only be established syntactically, which
is why the default's terms are restricted.

The ledger's outcomes come from one *authoritative pass* per emitted
function: the context-insensitive analysis of its body, which is the body
CodeGen emits for every caller. It runs after the unit's callback-slot and
recursive-SCC fixpoints, with the unit's sized-field facts in force (the
RFC 0012 second reporting pass is folded into it) and with only the §7.6
invariants that survived (§7.6 says which functions are re-run for that).
Every other run (fixpoint rounds, later Houdini rounds, the link step's
refinement rounds before the last) publishes into a discarding adapter.

Context-specialised analyses (RFC 0016 call contexts) never decide the rows
of the function they analyse. They sharpen the summaries callers see, and
they keep their diagnostics: a finding a context run makes in the callee is
reported where v0.10.0 reports it, at the use in the callee, with a note
naming the call, and its ledger link is that call's Call site in the
caller, whose temporal facet becomes a violation (definite in the context)
or `unresolved(may-released)` (possible). **Departure:** the resolutions
say context runs only sharpen summaries. Dropping their diagnostics would
lose catches v0.10.0 makes, such as `two(p, p)` with
`void two(char *a, char *b) { free(a); b[0] = 1; }`, against the
resolution that every current catch is still reported (gates G1–G4). The
callee's own site is not proven either way (§3.1, `may-alias-released`).

Functions defined in system headers are not in the ledger.

### 3. Certainty and severity

Every diagnostic is *definite* or *possible*. Definite violations are
errors. Possible findings are warnings for the temporal ids and produce no
diagnostic for spatial and null facets, which become *checked* instead.

#### 3.1 Temporal certainty

`core::MoveRecord` (`include/weavec/Core/Moves.h`) has no path-coverage
information today: `MoveTracker::join` (`lib/Core/Moves.cpp`) inserts the
other side's records and keeps set union. This RFC adds three fields:

```cpp
/// RFC 0030: every predecessor merged since the record was made had it.
bool allPaths = true;
/// RFC 0030: made from a callee effect that holds only on some outcome
/// classes or paths (a `PendingOutcome` class, or a summary effect that is
/// not `consumesUnconditionally`). Cleared when a test of the result
/// narrows the pending classes to ones that all consume the place, unless
/// the effect is `lossy` (§9.1), in which case it is never cleared.
bool conditional = false;
/// RFC 0030: made by the unknown-callee default (§5.1) or an open slot
/// (§9.3). Never diagnosed.
bool unknownOrigin = false;
```

`MoveTracker::join` gains a second loop over this side's records. The rules:

- A record present on one side only gets `allPaths = false`.
- A record on both sides gets:
  - `allPaths = a.allPaths && b.allPaths && (a.unknownOrigin == b.unknownOrigin)`;
  - `conditional = a.conditional || b.conditional`;
  - `unknownOrigin = a.unknownOrigin && b.unknownOrigin`.
- The existing rules for reason, location, witness, guard and `ownValue`
  are unchanged.

A record is **definite** when

```
allPaths && !conditional && !unknownOrigin && guard.trivial()
```

where `guard` is the RFC 0009 `PlaceGuard` and `trivial()` means no
conjuncts.

**Amendment (S3).** Only the *callee's* part of the guard counts. A
summary `when` condition that is still undecided at the call makes the
record `conditional`, which the rule above already excludes. The path-fact
conjuncts the engine adds when it makes the record held on every path that
made it, so they do not make it indefinite. Pruning them at the use was
tried: numeric predicates are often undecidable there, and plain
`free(p); p[0]` after a loop became a warning. So the implemented test is
`allPaths && !conditional && !unknownOrigin`. A second unconditional
consume of a place reaffirms its record (`allPaths` again, guard = the
facts at the second consume): `while (n--) { free(p); use(p); }` is a
possible double free and a definite use after free. At a use or release that hits record *r*
(`FunctionDataflow::reportUseOfMoved`, the double-free branch of the
consume path):

| Record | Outcome of the temporal facet | Diagnostic |
| --- | --- | --- |
| definite, reason `Freed`/`Released` | violation | `use-after-free` or `double-free`, error |
| definite, reason `Moved` | violation | `use-after-move` (or `double-free`), error |
| definite, reason `Uninitialized` | violation of the null facet for a pointer (§3.2); no facet for a scalar (§3.4) | `use-of-uninitialized`, error |
| not definite, not `unknownOrigin`, `Freed`/`Released` | `unresolved(may-released)` | same id, warning, "may" wording |
| not definite, not `unknownOrigin`, `Moved` | `unresolved(may-moved)` | same id, warning |
| not definite, `Uninitialized` | null facet `checked` (zero-initialised) or `unresolved(no-zero-init)` | none |
| `unknownOrigin` | `unresolved(unknown-callee)` or `unresolved(callback)` | none |
| no record, but a may-alias release (below) | `unresolved(may-alias-released)` | none |
| no record | proven (under the entry assumptions and §9.4) | none |

**A known release after an unknown one.** `MoveTracker::markMoved` keeps
the original record when a place is consumed again. When a consume through
a known effect (a `free`, a summary's unconditional consume) hits an
`unknownOrigin` record, the release's own temporal facet is
`unresolved(unknown-callee)` without a diagnostic, because the unknown
callee may already have released the object. The record is then *replaced*
by the new one (`unknownOrigin = false`, `allPaths = true`), so
`consume(p); free(p); p[0] = 1;` still reports the definite use-after-free
that v0.10.0 reports.

**Aliases of a released object.** A place with no `MoveRecord` can still
alias a released object, as in `free(b->data); b->cur[0] = 0;` when `cur`
points into `data`, or `free(a); b[0] = 1;` when `a` and `b` are the same
argument. After a release on some path, the temporal facet of an access
through a pointer loaded from a parameter- or global-rooted place, or
copied from a parameter, is `unresolved(may-alias-released)` unless the
pointer is provably distinct from every object released so far on some
path. Provably distinct means one of:

- the pointee types could not designate the same object under C's
  effective-type rules: neither is a character type and they are not
  compatible (a unit compiled with `-fno-strict-aliasing` treats every pair
  as compatible);
- both pointers were loaded from owning places (§9.4), which the
  owner-uniqueness assumption keeps distinct, so `free(s->a); free(s->b)`
  stays proven;
- the engine's place identity shows different allocations or storage;
- the pointer was stored after the release.

The rule adds no diagnostic. A caller that passes aliased arguments is
still reported through the context runs (§2.6), as v0.10.0 reports it.

`core::Loan` (`Borrow.h`) gains the same `allPaths` bit, cleared by
`BorrowState::join` for a loan present on one side only.
`conflicting-borrow` is an error only when the loan has `allPaths` and the
conflicting move or release is reached unconditionally from the loan;
otherwise it is a warning, and the move or release's temporal facet is
`unresolved(may-conflict)`.

The rule is conservative on purpose. A join always clears `allPaths`, even
when a guard encodes the condition, because guards are bounded
(`MaxGuardConjuncts = 8`) and can be weakened by dropping conjuncts, which
would make a false definite finding possible. A correlated bug
(`if (n == 0) free(p); … if (n == 0) use(p);`) is therefore a warning, not
an error.

#### 3.2 Null certainty

`core::NullRecord` (`Nullness.h`) gains
`bool allocatorSource = false`. It is set when the value is the result of a
call whose `LibrarySpec` entry allocates or whose summary `returnsFresh()`.
It is preserved by copies and tests, and joins by `||`. The null facet of a
dereference, and of an argument passed where the callee or the library
entry requires non-null, is decided from the engine's fact about the
operand:

| Fact (`NullTracker`) | Null facet | Diagnostic |
| --- | --- | --- |
| `NonNull` (tested, dereferenced before, declared `WEAVEC_NONNULL`/`nonnull`/`_Nonnull`, `[static N]`, or `LibrarySpec` non-null result) | proven | — |
| `Null`, not `allocatorSource` | violation | `null-dereference`, error |
| `Null`, `allocatorSource` | checked | `allocation-failure` (off by default) |
| `MaybeNull` | checked | `allocation-failure` if `allocatorSource` |
| no record (parameter, loaded field, unknown result) | checked | — |
| possibly uninitialised with `-fno-weavec-zero-init` | `unresolved(no-zero-init)` | — |

`Null` is must by construction: the `NullTracker::join` table turns `Null`
joined with anything else into `MaybeNull`. A finding that rests only on a
system-header attribute is never definite and produces no check; the
argument's null facet is `trusted(system-api)` (§5.2, §7.2).

**Refinement after a dereference.** After a dereference whose null facet is
*proven* or *checked*, the existing `markDereferenced` rule makes the
pointer `NonNull` downstream. This is sound in the enforcing modes, where
the check would have trapped, and the ledger always describes those modes
(§Soundness). A dereference whose null facet is trusted (`unsafe`,
`system-api`) or unresolved (`no-zero-init`) refines nothing, and neither
does a spatial facet that is not proven or checked: an index check bounds
the index downstream only when it was planned. The same holds for an
assumption (§6.2).

**Requirements at calls.** The engine's `FunctionSummary::requiresNonNull`
and `requiresExtent` are *may*-facts: `noteRequirement` records any
dereference of a parameter with no nullness record, on any path. From this
RFC on they feed only summaries and fix-its. The definite
"`'<p>', which is null, is passed to '<f>', which dereferences it`" error,
call-site checks, and any post-call `NonNull` or extent fact come only from
the must-access requirements of §7.5, or from a call site whose own
`nonnull` check is planned. `checkRequiredArguments` stops calling
`markDereferenced` on an argument of unknown nullness otherwise: after
`f(q, 0)` with `void f(int *p, int c) { if (c) *p = 1; }`, a later `*q` is
still checked.

#### 3.3 Spatial certainty

The engine's spatial decision is `core::checkSpatialBounds`, which returns
`core::SpatialCheck{outcome, reason, violation}` (`Spatial.h`). It maps to
the ledger as follows, and `core::SpatialOutcome`/`SpatialReason` are then
removed:

| Engine result | Facet |
| --- | --- |
| `Proven` | proven |
| `Violation` with `BoundsVerdict::Kind` `OutOfBounds`, `BeforeStart` or `AtLeastPastEnd`, against an exact extent | violation (`out-of-bounds`, error) |
| the same, where the only extent is a declared kind (an access inside the body past the declaration) | checked if expressible, else `unresolved(inexpressible)`; never an error, since the object may be larger. An argument whose exact extent is below a declared requirement is a violation at the call, as for any exact extent |
| the same, when the extent rests only on a system-header attribute | `trusted(system-api)`, no check and no diagnostic |
| `Violation` with `MayBeOutOfBounds` or `MayReachPastEnd` | checked if the extent is exact or declared and expressible, else `unresolved(inexpressible)`; no diagnostic |
| `Unresolved`, `UnknownIndex` (extent exact or declared) | checked if expressible, else `unresolved(inexpressible)` |
| `Unresolved`, `UnknownIndex` against a lower-bound kind, or `UnknownExtent` | `unresolved(unknown-extent)` |
| `Unresolved`, `UnknownOffset` | `unresolved(unknown-index)` |
| `Unresolved`, `Arithmetic` or `UnsupportedExpression` | `unresolved(unanalysed)` |
| `Unresolved`, `InterfaceRequirement` | checked at static call sites when the requirement matches a §7.5 must-access requirement with a trivial `when` guard; otherwise, inside the body, `unresolved(unknown-extent)`, or `trusted(caller-contract)` for an exported or address-taken function whose §7.5 requirement covers the access |

A *violation* needs an exact extent (§7.1). A declared kind is a lower
bound: the object may be larger, so an access past it breaks the
declaration, not necessarily the object. Library-call requirements (§8)
follow the same table, each requirement separately. A definite overlap of a
disjointness requirement is reported as `out-of-bounds`, with the message
"`'<f>' copies <n> bytes between overlapping ranges of '<obj>'`".

#### 3.4 The other ids

| Id | Definite (error) when | Otherwise | Facet and possible reason |
| --- | --- | --- | --- |
| `invalid-release` | the released pointer exactly aliases a non-heap object, literal or interior position on every path | warning | Release spatial; `may-invalid-release` |
| `mismatched-release` | the resource's family is exact (not joined from different families) | warning | Release temporal; `may-mismatched-release` |
| `lifetime-too-short` | the escaping value exactly aliases the dead storage (exact alias edge) | warning | Call exit temporal; `may-dangle` |
| `conflicting-borrow` | as §3.1 | warning | temporal of the move or release; `may-conflict` |
| `leak` | never | warning; not reported at a return from `main` or after an exit-family call (`LibrarySpec` `exits`) | none: a leak is not a facet |
| `invalid-integer-operation` | unchanged (RFC 0017: already definite-only) | — | none |
| `unsafe-operation` | unchanged (RFC 0004) | — | the Raw site's facets |
| `annotation-mismatch` | always | — | the facet the contradicted declaration or relied-upon fact governs |
| `invalid-annotation` | never | warning | none |
| `use-of-uninitialized` on a scalar | as §3.1 | no diagnostic | none: scalars have no facet (§Soundness) |

`-Werror` in project flags still does not promote WeaveC warnings.
`-Werror=weavec[-<id>]` does. Errors cannot be disabled, only lowered to
warnings, as today.

**Lowered violations still trap.** When a definite violation's error is
lowered to a warning and the unit therefore produces an object,
`CheckPlanner` guards the site anyway in the enforcing modes. A spatial,
null or assertion violation gets its facet's check, which traps whenever
the violation actually happens. Any other violation (temporal, a release),
and one whose check is not expressible, gets an unconditional
`__builtin_verbose_trap("weavec", "violation")` before the operation.
With `-fweavec-checks=none` nothing is emitted, as for every other check.
This is what makes (V) hold under `-Wno-error`.

### 4. Worked examples

Each example gives the source, the ledger outcomes of its interesting
sites, the diagnostic if any, and the emitted check written as equivalent
C. Helper names are defined in §10.2. Every example is also a case under
`test/cases/semantics/`.

**Proven.** A declared kind discharges both facets:

```c
int sum4(const int a[static 4]) { return a[0] + a[3]; }
```

`a[0]` is a Deref site: null proven (`[static 4]` implies non-null, A1),
spatial proven, temporal proven. `a[3]` is an Index site with spatial
proven (3 < 4). No code is emitted.

**Checked, null.** Unknown nullness costs a branch:

```c
struct node { struct node *next; int v; };
int second(struct node *n) { return n->next->v; }
```

Both Deref sites have null checked. Spatial is proven, because `n` is
Single under A1 and the slot `node.next` is Single (§7.3). Temporal is
proven. Emitted:

```c
return ((struct node *)__weavec_chk_nonnull(
          ((struct node *)__weavec_chk_nonnull(n))->next))->v;
```

**Checked, index.** A declared counted pointer:

```c
int at(const int *WEAVEC_COUNTED_BY(n) p, size_t n, size_t i) { return p[i]; }
```

Spatial checked with the index template, null checked, temporal proven.
Emitted:

```c
return ((const int *)__weavec_chk_nonnull(p))[__weavec_chk_index(i, n)];
```

**Checked, library length.** A copy into a fixed buffer:

```c
void greet(const char *name) { char buf[16]; strcpy(buf, name); puts(buf); }
```

`strcpy` is a LibCall site with two spatial requirements. The destination
requirement (`strlen(name) + 1 <= 16`) is checked against the exact extent
of `buf`, computing the length with the bounded prelude helper, which reads
at most 16 bytes. The source requirement, that `name` is NUL-terminated
within its object, is inferred by R4 (§7.5); `greet` is exported, so it is
`trusted(caller-contract)`. The merged spatial facet is `checked`, and its
`requirements` list both. The null facet of `name` is checked: nullability
is never left to a caller contract. `name` is side-effect free, so the
terms are expressible:

```c
(__weavec_chk_len(__weavec_strnlen(name, sizeof buf) + 1, sizeof buf),
 strcpy(buf, (const char *)__weavec_chk_nonnull(name)));
```

`__weavec_strnlen` traps on a null pointer itself, so the length term
never reads through null before the argument's own check runs.

**Violation, spatial.**

```c
void f(void) { char *p = malloc(4); if (!p) return; p[4] = 0; free(p); }
```

`p[4]`: spatial violation (`OutOfBounds`), reported as
``error: 'p[4]' is out of bounds: … an object of 4 bytes [weavec::out-of-bounds]``.
No object is produced.

**Violation versus possible, temporal.**

```c
void g(char *p)        { free(p); p[0] = 1; }           // error: use of 'p' after it was freed
void h(char *p, int c) { if (c) free(p); p[0] = 1; }    // warning: use of 'p' after it may have been freed
```

In `h`, the record for `p` reaches `p[0]` through a join with a
predecessor that lacks it, so `allPaths` is false. The temporal facet is
`unresolved(may-released)` with a warning. The null facet is checked, and
the program builds.

**Violation, null.**

```c
int k(struct node *n) { if (n == NULL) return n->v; return 0; }
```

On the null edge, `n` is `Null` with reason `Tested`, not from an
allocator. The result is
``error: dereference of 'n', which is null [weavec::null-dereference]``.

**Unresolved, unknown extent.**

```c
char *get_buffer(void);                   /* no definition, entry or annotation */
int f(void) { char *b = get_buffer(); return b[1000]; }
```

`b` is Single-or-nullable under A3 (§7.3). Single is a lower bound, so
element 0 is covered but element 1000 has no extent, and no check is
written against the lower bound. `b[1000]`: spatial
`unresolved(unknown-extent)`, null checked, temporal proven. The call is a
Call site whose temporal facet is `unresolved(unknown-callee)`, with a
fix-it suggestion in the ledger.

**Unresolved, unknown callee.**

```c
void consume(char *p);
int f(void) { char *p = malloc(8); if (!p) return 0; consume(p); return p[0]; }
```

`p[0]`: temporal `unresolved(unknown-callee)`, because `consume` may have
freed `p`. There is no diagnostic. If a unit that defines `consume` and
frees `p` is linked with `weavec-cc`, the link step reports
`use-after-free` as an error and the link fails (§13).

**Unresolved, inexpressible.**

```c
struct buf { char *WEAVEC_COUNTED_BY(cap) data; size_t cap; };
char get(struct buf *b, struct buf *other, size_t i) {
  char *d = b->data;
  b = other;               /* the extent was 'b->cap' of the old 'b' */
  return d[i];             /* spatial: unresolved(inexpressible) */
}
```

The engine still knows the extent, as a snapshot place, but it has no C
name at `d[i]`.

**Trusted, unsafe.**

```c
void poke(uintptr_t addr) {
  WEAVEC_UNSAFE { volatile unsigned *reg = (volatile unsigned *)addr; *reg = 1; }
}
```

The IntToPtr site's spatial and temporal facets are `trusted(unsafe)`.
`reg` is an RFC 0004 raw pointer (cast from an integer), so `*reg` is a Raw
site, and its spatial, null and temporal facets are all `trusted(unsafe)`.
No checks are emitted. Outside the region, `*reg` would be an
`unsafe-operation` error, as today.

**Trusted, system API.** `ioctl` has no `LibrarySpec` entry, because the
layout of its third argument depends on the request:

```c
int cols(int fd) { struct winsize ws; if (ioctl(fd, TIOCGWINSZ, &ws) == -1) return 80; return ws.ws_col; }
```

The `ioctl` call is a Call site with temporal `trusted(system-api)`. The
address of `ws` is borrowed for the call.

**Trusted, concurrency.** In probe 15, `worker` is a `pthread_create`
target and frees the global `shared`. `main`'s later `shared[0]` has
temporal `trusted(concurrency)`, and so does every access to `shared`
inside `worker`; the summary line counts them under A4. A null test of
`shared` in `main` followed by a use does not prove the use: its null facet
is checked (§5.3).

**Assumptions.**

```c
int a(char *b, int n) { WEAVEC_ASSUME(n > 0); return b[n - 1]; }   // assertion: checked
int c(void) { char b[4] = {0}; int i = 10; WEAVEC_ASSUME(i < 4); return b[i]; }
```

In `a` the assertion is not proven, so it is emitted as
`__weavec_chk_assert((n > 0) != 0)`. In `c` the assertion is refuted:
``error: assumption 'i < 4' is false here [weavec::contradicted-assumption]``.

**Require levels.** Under `-fweavec-require=checked`, the unknown-extent
example above fails with
``error: access 'b[1000]' is neither proven nor checkable: the extent of 'b' is unknown [unknown-extent] [weavec::unresolved-operation]``.
Under `-fweavec-require=proven`, `second` above also fails with two
`unchecked-operation` errors.

### 5. Sound defaults for code WeaveC cannot see

#### 5.1 Unknown callees

A direct callee is *unknown* when all of the following hold:

- it has no body in the TU;
- it has no summary from the program database (only available at link);
- it has no `LibrarySpec` entry;
- it is not declared in a C library, POSIX or platform header (§5.2).

The unknown-callee default applies per pointer parameter. A pointer
argument whose parameter has no *ownership contract* receives the effects
below. An ownership contract is a WeaveC ownership annotation
(`WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`,
`WEAVEC_RELEASES`, `WEAVEC_RETAINS`) or an ecosystem ownership attribute
(`ownership_takes`, `ownership_holds`, and `malloc`/`ownership_returns` for
the result). Nullability and extent attributes (`nonnull`, `_Nonnull`,
`counted_by`, …) never lift the temporal default: they only decide null and
spatial facets. A callee whose pointer parameters all carry ownership
contracts applies those contracts, and its Call site's temporal facet is
`trusted(extern-contract)`. **Departure:** the resolutions define an
unknown callee as one with "no declared contract". Read as "any attribute",
that let a destructor declared only `__attribute__((nonnull))` act as
borrow-only, which is v0.10.0's unsoundness again; only ownership contracts
count.

For each argument without an ownership contract, a call to an unknown
callee:

- marks the argument's place, every pointer-typed place reachable through
  a non-`const` pointee of it, every escaped place and every pointer-typed
  global that external code can reach (external linkage, or address taken)
  with a `MoveRecord` of reason `Freed`, `unknownOrigin = true` and
  `allPaths = false`;
- forgets the nullness, extents and scalar facts of the places reachable
  through its non-`const` pointees, of escaped places and of reachable
  globals. The argument value's own nullness and extent are kept: the
  callee cannot change the caller's copy of the pointer or the size of the
  object, only its lifetime, which the record covers.

The result gets an unknown nullness, the Single kind (A3) and no ownership,
so it is not tracked as a leak.

A later use of a marked place is `unresolved(unknown-callee)` without a
diagnostic. So is a later release: the callee may have retained the value.

The Call site's temporal facet is `unresolved(unknown-callee)` and carries
a fix-it suggestion. For example: "declare 'consume' with WEAVEC_BORROWED
on 'p' if it neither keeps nor frees it". The per-call
`annotation-required` warning is removed. An indirect call through an open
slot with no known target (§9.3) behaves the same way with reason
`callback`.

In a per-TU compile, calls into other units of the same program are
unknown. The link step (§13) replaces these defaults with the definitions'
summaries.

#### 5.2 System APIs outside the table

A *C library, POSIX or platform header* is a system header
(`SourceManager::isInSystemHeader`) that is either under the toolchain's
resource directory, or under the SDK or sysroot and named, relative to its
include directory, in the `LibrarySpec` header list (`stdio.h`,
`sys/ioctl.h`, `pthread.h`, …; on Darwin every header of the SDK counts).
The same definition decides which link inputs are *system* (§13.2).
Third-party libraries installed as system headers (`jansson.h` in
`/usr/include` or `/opt/homebrew/include`, `sqlite3.h`) are not in the list,
so their functions are unknown callees (§5.1). **Departure:** the
resolutions trust every system-header function. Under that rule
`json_decref(j); json_string_value(j)` against an installed jansson would be
proven, so the trust is limited to the platform's own headers.

A function declared in a C library, POSIX or platform header without a
`LibrarySpec` entry:

- borrows its arguments for the call, with no release, retain or store
  effect;
- returns a Single-or-nullable result of unknown nullness;
- puts `trusted(system-api)` on the Call site's temporal facet.

Later uses are unaffected, and are proven under A2. Clang attributes on
such declarations (for example glibc's `__nonnull`) never make a finding
definite and never produce checks. They record `trusted(system-api)` on
the argument's null facet when the argument is not proven non-null. The
system-header exemption in `FunctionDataflow`'s boundary check
(`!options.reportUnannotated && sm.isInSystemHeader(...)`) is removed:
every such call is in the ledger.

#### 5.3 Callbacks, threads and signals

`LibrarySpec` `callback` clauses (§8) say how a function-pointer argument
is invoked.

- **`sync`** (`qsort`, `qsort_r`, `bsearch`, `lfind`, `lsearch`,
  `tsearch`, `tfind`, `tdelete`, `twalk`, `scandir`): at the call, the
  target's summary is applied as a may-effect. The may-effect covers zero
  or more invocations, with parameters bound to borrowed pointers into the
  argument the clause names and to the context argument. The target is
  resolved from the argument's function value or its slot (§9.3). An
  unresolvable target gets the unknown-callee default with reason
  `callback`. Probe 47 thereby becomes a possible use-after-free warning.
- **`entry`** (`pthread_create`, `thrd_create`, `signal`, `sigaction`,
  `pthread_atfork`): the target is analysed as an entry point, as every
  emitted function already is. Define G = the pointer-typed globals read,
  written or released by any function reachable from an entry target (call
  graph plus solved slots), together with every object reachable from the
  argument passed to the target. In every function, entry targets and the
  functions they reach included, a facet whose pointer is loaded from a
  place rooted in G is treated like a `setjmp` function's facets (§5.4):
  - its temporal facet is `trusted(concurrency)`;
  - a null or spatial facet that only flow facts would prove (a test
    followed by a use, which another thread or a handler can invalidate in
    between) is checked instead, because the check tests the value the
    access actually uses; when the check is not expressible, the facet is
    `trusted(concurrency)`;
  - facets proven by type alone (a constant index into an array member)
    stay proven.

  **Departure:** the resolutions put only the shared globals'
  `trusted(concurrency)` on record. Applying it to temporal facets only,
  and only outside the entry targets, left `if (g_buf) g_buf[0] = 1;`
  racing with a handler that sets `g_buf = NULL`, and the worker's own use
  of an object `main` frees, proven.
- **`at-exit`** (`atexit`, `at_quick_exit`, `on_exit`): the target is
  analysed as an entry point. It does not contribute to G, because it runs
  after the main flow has ended. A dangling global left for it is caught
  instead by the exit boundary at the `exit` call (§9.4).

#### 5.4 `setjmp`

In a function that calls a `returns-twice` function (`setjmp`,
`sigsetjmp`, `_setjmp`, or a declaration with Clang's `returns_twice`
attribute):

- every temporal facet is `unresolved(setjmp)`;
- every spatial or null facet that would be proven from flow facts about
  non-volatile locals is downgraded to checked, or to
  `unresolved(setjmp)` when not expressible. Facets proven by type alone
  (a constant index into an array) stay proven.

`longjmp` and `siglongjmp` are `noreturn` in the table.

#### 5.5 Budgets

Each run of `FunctionDataflow` on one function body counts the CFG blocks
it transfers, both in the fixpoint and in the final pass. The count is
deterministic and does not depend on timing.

- When the default-context analysis of a function exceeds the budget, it
  stops. The function's facets take the default outcomes of §2.6 with
  reason `budget`. Its summary's effects become the unknown-callee effects
  (§5.1), so callers stay sound; its result keeps the syntactic kind of
  §7.3.
- The context-specialised analyses of one function share a second budget
  of the same size, counted over all of its contexts together. Once it is
  exhausted, further contexts use the default-context summary. This bounds
  the context runs that dominate Lua's analysis time (gate G15).
- The summary line and the ledger list over-budget functions.

A summary the engine marks *incomplete* (the 32
`inferred.incomplete.insert(…)` reasons, `SummaryStore::markIncomplete`) or
produces when `MaxFixpointRounds` is exhausted may under-approximate the
callee. Callers apply its known effects plus the unknown-callee may-effects
of §5.1 on every pointer argument, and its result keeps only its §7.3 kind.
The function's own sites that the engine did not decide take the defaults
of §2.6 with reason `unanalysed`.

The budget is set by `-fweavec-budget=<n>` (`--budget=<n>` in `weavec`);
`0` means unlimited. The initial default is 200,000 block transfers. S3
calibrates it once: the final default is the smallest multiple of 50,000
that is at least four times the 99.9th percentile of per-function counts
over the corpus. The chosen value is recorded in the CLI reference. At most
1% of corpus functions may exceed it (gate G15). This budget replaces the
internal "iteration limit reached" incompleteness paths. The existing
`MaxFixpointRounds` guard stays as a bug guard.

#### 5.6 Headers

`-fweavec-analyze-headers` is removed. Every emitted function (§2.6) is
analysed and instrumented, including `static inline` functions from user
headers. Each TU instruments its own copy. Definitions in system headers
are never analysed or instrumented.

#### 5.7 Inline assembly

An `asm` statement with pointer-typed operands applies the unknown-callee
default (§5.1) to those operands, with detail "inline assembly". An `asm`
statement with a `"memory"` clobber also applies it to every escaped place
and to every reachable global.

### 6. `WEAVEC_UNSAFE`, `WEAVEC_ASSUME` and require levels

#### 6.1 `WEAVEC_UNSAFE`

Sites inside an unsafe region (a function or block annotated
`weavec.unsafe`):

- Spatial and null facets are `trusted(unsafe)`, and no checks are
  emitted. A definite spatial or null violation is still an error.
- Temporal state is tracked exactly as outside the region. A definite
  temporal violation is still an error (probe 45). A possible temporal
  finding is reported exactly as outside the region: a warning with "may"
  wording, and `unresolved(may-released)` or `unresolved(may-moved)`.
- Raw sites (§2.1: pointers with an RFC 0004 raw origin, including those
  converted from integers) and IntToPtr sites inside the region are
  `trusted(unsafe)` for every facet, temporal included; this is the only
  temporal use of the reason. Outside a region, a Raw site remains an
  `unsafe-operation` error (RFC 0004).
- An Assume site inside the region keeps its assertion facet, and its
  check is emitted: the region trusts raw memory operations, not
  assumptions.
- A trusted dereference inside the region refines nothing after it
  (§3.2): the pointer is not `NonNull` downstream because of it.

The `inUnsafe` suppression (in `FunctionDataflow::report` and about 22
other sites across 7 files) is replaced by these rules; no diagnostic is
dropped for being inside a region. What the region does to the surrounding
code (a free, a store) is still visible there, as RFC 0004 specifies.

#### 6.2 `WEAVEC_ASSUME(e)`

The Assume site's assertion facet is:

- **proven** when the engine proves `e` at the site. The call stays as
  `weavec.h` wrote it: a no-op `weavec_assume_`, which the optimiser
  removes.
- **violation** when the engine refutes `e`. This is the
  `contradicted-assumption` error.
- **checked** otherwise. `CheckEmitter` replaces the call with
  `__weavec_chk_assert((e) != 0)`. With `-fweavec-checks=none` the call is
  left alone.

In every case the analysis assumes `e` after the site, as today. That is
sound in the enforcing modes, because the program would have trapped
otherwise, or the violation fails the build (or traps, §3.4). The header's
`weavec_assume_` keeps its no-op body, so the behaviour is the same with
any compiler and in every WeaveC mode except the rewrite itself. The
header's doc comment for `WEAVEC_ASSUME`, which today says it is "trusted
like every annotation", is rewritten to describe these three outcomes.

#### 6.3 Require levels and `WEAVEC_REQUIRE_SAFE`

`-fweavec-require=none|checked|proven` (default `none`; `--require` in
`weavec`) makes the ledger fail-closed:

| Level | Error for each facet that is | Id |
| --- | --- | --- |
| `none` | — | — |
| `checked` | unresolved | `unresolved-operation` |
| `proven` | unresolved; checked | `unresolved-operation`; `unchecked-operation` |

Trusted facets are allowed at every level: trust is explicit and listed.
The `dangling-escape` and `second-owner` reasons are therefore errors only
under `checked` or `proven`.

`WEAVEC_REQUIRE_SAFE`, before a function definition, holds that function's
sites to `checked` whatever the command line says. It replaces
`WEAVEC_CHECKED`. `-fweavec-require=proven` is the only fail-closed tier
without runtime checks, and replaces `--checked`.

### 7. Pointer kinds and where spatial obligations sit

#### 7.1 The lattice

```cpp
namespace weavec::core {
enum class PointerShape : std::uint8_t { Single, Counted, Sized, EndedBy, NulTerminated, Unknown };
enum class Nullability : std::uint8_t { Nonnull, Nullable };
enum class KindSource : std::uint8_t { Declared, Inferred, Default };
/// k * <path> + c, or the constant c; the path is a sibling parameter
/// (`param <i>`) or a sibling field (`.<field>`) of the same object.
struct ExtentTerm { std::optional<SummaryPath> path; std::int64_t scale = 1; std::int64_t offset = 0; };
struct PointerKind {
  PointerShape shape = PointerShape::Unknown;
  ExtentTerm extent;              // Counted: elements; Sized: bytes; EndedBy: the end pointer's path
  Nullability nullability = Nullability::Nullable;
  KindSource source = KindSource::Default;
};
}
```

The meaning of each shape, for a value `p` of type `T *` (as a guarantee
about values, or as a requirement on arguments):

| Shape | Meaning when `p` is not null |
| --- | --- |
| `Single` | at least one `T` is accessible from `p` (at least `sizeof(T)` bytes, 1 for `void`) |
| `Counted(e)` | at least `e` elements of `T` are accessible from `p` |
| `Sized(e)` | at least `e` bytes are accessible from `p` |
| `EndedBy(q)` | `p <= q`, and `[p, q)` lies in one object |
| `NulTerminated` | a zero element lies at or after `p` within `p`'s object |
| `Unknown` | nothing |

"At least" is deliberate: a kind never claims the object ends there. That
makes every kind a *lower bound*, and it gives one rule for what a check may
compare against. An *exact extent* is one of:

- the object type of an array or VLA object (the VLA's extent term is
  `sizeof vla / sizeof *vla`, which never goes stale when the bound
  variable changes);
- an allocation-size value in scope (§7.4, *Arithmetic*);
- a surviving §7.6 field invariant, which is exact by construction.

A check operand is either an exact extent or a *declared* kind, which the
programmer asserted. Checks against exact extents are never tighter than
the object. Checks against declared kinds enforce the declaration and may
be tighter than the allocation, which is the point of declaring them.
*Lower-bound kinds* (Single, the A1 and A3 defaults of §7.3, and §7.5
requirements inferred inside the body) discharge the accesses they cover
and nothing else. Every other access through them is
`unresolved(unknown-extent)`, never checked against the bound: checking
`const struct T *a = pa;` in a `qsort` comparator against `pa`'s one-byte
Single default would trap every correct comparator. Only an exact extent
can make an access a *violation* (§3.3).

The join (merge of guarantees at a CFG merge or across stores):

- `x ⊔ x = x`;
- two kinds with different shapes or different extent terms join to
  `Unknown`. The exception is `Single ⊔ Counted(e) = Single` when `e ≥ 1`
  is proven at the join;
- `Nullable ⊔ Nonnull = Nullable`.

Requirements combine by conjunction: every requirement is checked. The
spelling in summaries and records is `single`, `counted(<term>)`,
`sized(<term>)`, `ended-by(<path>)`, `nul-terminated` or `unknown`,
followed by `nonnull` or `nullable`. Terms are spelled as in the RFC 0011
`extent` grammar.

#### 7.2 Declared kinds

`AttributeReader` reads these sources; each yields a declared kind:

| Source | Applies to | Kind |
| --- | --- | --- |
| `WEAVEC_COUNTED_BY(n)` / `WEAVEC_SIZED_BY(n)` | parameter, field | `Counted(n)`; for `void *` and character pointees `Sized(n)`. The two macros are synonyms: `WEAVEC_SIZED_BY` keeps its RFC 0011 meaning, which is elements (bytes for `void *`), unlike Clang's byte-counting `sized_by`. New code should use `WEAVEC_COUNTED_BY` |
| `WEAVEC_ENDED_BY(q)` | parameter, field | `EndedBy(q)` |
| `WEAVEC_STRING` | parameter, field, result | `NulTerminated` |
| `WEAVEC_NONNULL` / `WEAVEC_NULLABLE` | parameter, field, result, variable | nullability |
| `counted_by(n)`, `counted_by_or_null(n)`, `sized_by(n)`, `sized_by_or_null(n)` | field (and parameter where Clang accepts it) | `Counted(n)` / `Sized(n)`, nonnull unless `_or_null` |
| `alloc_size(i[, j])` | function | result `Sized(arg i [* arg j])` |
| `nonnull`, `nonnull(i, …)`, `returns_nonnull`, `_Nonnull`, `_Nullable` | function, parameter, result | nullability |
| `malloc`, `ownership_returns(m)`, `ownership_takes(m, i)`, `ownership_holds(m, i)` | function | fresh result of family `m`; releases or retains argument `i` |
| `T p[static N]` | parameter | `Counted(N)`, nonnull |
| `T p[n]` with `n` an earlier parameter (VLA parameter) | parameter | `Counted(n)`, nullable |

A constant `T p[N]` without `static` is not a requirement, because C gives
it none and code commonly passes fewer elements. It only yields a fix-it
suggestion.

The `WEAVEC_*` extent macros are resolved by name: the argument is the name
of any sibling parameter (in any position) or sibling field. This works on
GCC, and without Clang's rule that the count must be declared before use.
An unresolvable name is an `invalid-annotation` warning, and the kind is
dropped.

Precedence, from strongest:

1. `WEAVEC_*` annotations;
2. ecosystem attributes in non-system code;
3. the `LibrarySpec` entry;
4. attributes in system headers.

A conflict at one level is an `invalid-annotation` warning, and the weaker
kind is used. Findings that rest only on level 4 are never definite and
never produce checks: their facets are `trusted(system-api)`.

A kind declared on a declaration of a function defined elsewhere is checked
at every call site in this TU (declared requirements are checked
everywhere). Inside the definition it holds under A1. At link, every
definition is verified against every declaration any unit saw
(`annotation-mismatch`, §13).

#### 7.3 Defaults and slot kinds

Parameters, results and fields without a declared kind get these defaults.

**Parameters.**

- A parameter of a `static` function whose address is never taken gets
  the join (§7.1) of the argument kinds at all its direct calls, judged by
  the Single-valid rules below or by an engine proof of at least one
  element. A pointer-arithmetic argument that is neither makes the kind
  `Unknown`. No assumption is involved. A must-access requirement (§7.5)
  checked at every call still covers the accesses it names. A static
  function with no direct call keeps the A1 default.
- Every other parameter is Single-or-nullable by A1, unless must-access
  inference (§7.5) gives more. `KindInference` records a per-parameter
  flag, `reliesOnSingle`, when some access in the body, or some store of
  the parameter into a slot (below), is proven only by that default.
- A call in the unit to such a function, whose argument for a
  `reliesOnSingle` parameter is neither Single-valid nor proven to have an
  element, gets a spatial facet on its Call site:
  `unresolved(unknown-extent)`, never checked, because a correct program
  may pass a one-past-the-end value the callee never dereferences on that
  path. The flag is exported in the unit record, and the link step does
  the same for calls between units (§13.2). What no unit can see stays
  under A1.
- The parameter `argv` of `main(int argc, char **argv[, char **envp])`
  has the declared kind `Counted(argc + 1)`, nonnull, as long as `argc` and
  `argv` are never assigned. Each `argv[i]` with `i < argc` is
  `NulTerminated` and nonnull, and `argv[argc]` is null; facets resting on
  these are `trusted(system-api)`.

**Results.** Results of calls to functions outside the TU (unknown, system
or declared-only) are Single-or-nullable by A3. For functions in the TU,
the result kind is inferred as the join over all return values, by the
Single-valid rules.

**Fields and globals (slots)** get their kind from `KindInference` before
the engine runs, as a greatest fixpoint over every store in the TU. No
engine facts are involved, so there is no circularity. Every slot starts as
`Single`. A store demotes its slot to `Unknown` unless the stored value is
syntactically *Single-valid*:

- null;
- `&object`, or an array decaying to its first element;
- an allocation whose `LibrarySpec` extent is at least `sizeof(T)` with
  constant operands (`malloc(sizeof *p)`, `calloc(1, sizeof *p)`);
- a parameter that is never assigned, incremented or address-taken in the
  function. Storing it sets the parameter's `reliesOnSingle` flag, so a
  caller that passes a cursor gets the Call-site row above;
- a call result whose kind is `Single`;
- a load from a slot that is still `Single`.

Stores the syntax does not show also demote:

- a store through an lvalue `*q` of pointer type `T *` demotes every
  address-taken slot of a type compatible with `T *`; a store through a
  `void **` or `char **` lvalue demotes every address-taken pointer slot;
- a slot whose address is passed to a callee is `Unknown`, unless the
  callee's summary shows only Single-valid stores through that parameter;
- a byte-wise write into an object (a `LibrarySpec` argument with `w` or
  `rw` access, a character-typed store, `fread`, `read`) demotes every
  pointer field of the object's type.

Demotion repeats until nothing changes. Pointer arithmetic results and
conversions from integers are never Single-valid, so storing a cursor
demotes its slot. A slot with a surviving §7.6 invariant is `Counted` or
`Sized` instead. The unit record exports each slot's kind and the stores
that demoted it; the link step reports a header struct whose slot is Single
in one unit and demoted in another as unverified under A3, and uses the
demoted kind. Stores by units without records are covered by A3.

**Locals** carry their value's facts flow-sensitively (the engine's
`SpatialTracker`); they have no slot kind.

A Single pointer guarantees one element, not "exactly one". `p[i]` with a
non-zero `i` on a Single pointer is `unresolved(unknown-extent)` unless an
exact extent or declared kind gives one. It is never checked against the
Single bound, and never a violation.

#### 7.4 Where spatial obligations sit

**Objects and extents.** (S) speaks of "the object the pointer was derived
from". It is defined as follows, and the engine's extent computation
(`DataflowDynamicExtents`, which today takes `min(fixed, tail)` for a fixed
trailing array) changes to match (§15):

- A trailing array member is *flexible* when `-fstrict-flex-arrays`
  allows it; at the default level 0 that is every trailing array, whatever
  its declared bound. Its extent is the rest of the allocation when that is
  known, else `unresolved(unknown-extent)`; it never comes from the
  declared bound. Lua's `TValue upvalue[1]` and `UpVal *upvals[1]`, indexed
  up to `nupvalues`, are flexible.
- A pointer made by `&member`, `&a[i]` or the decay of a sub-array has the
  extent of the complete object, as `__builtin_object_size` mode 0 does.
  `memset(&s->first, 0, sizeof *s)`, a flat walk `int *q = &m[0][0];
  for (k = 0; k < 12; k++) q[k] = 0;`, and a `memcpy` into
  `(char *)&ts->contents` for Lua's short strings are all in bounds.
- Two cases use the sub-object's bound. A direct subscript of a
  non-flexible array-typed lvalue (`m[i][j]` needs `j < 4` for
  `int m[3][4]`) uses the array's own bound, as C does. The destination of
  a `str*`-family `LibrarySpec` row uses the member's bound, as
  `_FORTIFY_SOURCE=2` does.
- The *object width* of a struct type `T` is `sizeof(T)`, or the offset of
  its flexible array member when it has one. It is the width a Single
  pointer to `T` guarantees and the width a dereference needs.

**Arithmetic.** Extents and offsets are compared as mathematical integers,
not in wrapping `size_t` arithmetic. An allocation's extent is the byte
count the program actually passed, a `size_t` value. It may be rewritten as
an element count (`bytes / sizeof(T)`, or a symbolic `n` for `malloc(n *
sizeof(T))`) only when the multiplication is proven not to wrap. Otherwise
a check compares the byte offset, computed with overflow checking (§10.2),
against the byte value. In probe 06 (`unsigned bytes = n * 4u; int *a =
malloc(bytes); for (unsigned i = 0; i < n; i++) a[i] = 0;`), `i < n` does
not prove `a[i]`; the check `index(i, bytes / 4)` traps, and the probe
carries an exact `TRAP: index` marker.

1. **Dereferences** (`*p`, `p->f`, `p[0]`). The spatial facet is discharged
   by `p`'s kind or local facts:
   - proven when `p` is Single, or has a kind that guarantees at least one
     element;
   - checked with `index(0, e)` when the kind is a declared or exact
     `Counted(e)` and `e ≥ 1` is not proven;
   - checked with `span` when `p` is a cursor whose base and extent are
     exact and expressible;
   - otherwise `unresolved(unknown-extent)` or `unresolved(unknown-index)`.
2. **Indexing** (`p[i]`, `a[i]`, `*(p ± i)`) needs `0 ≤ i < count`: proven,
   checked with `index` against an exact or declared count, a violation
   against an exact count, or unresolved.
3. **`p + i`, `p - i`, `++p`, `&p[i]` and `&p->f` are never checked by
   themselves.** One-past-the-end values are legal. Arithmetic beyond one
   past the end is undefined in C, but it is not an access, so it is not
   part of the guarantee; the value is checked where it is used.
4. **Creation points** carry an obligation only where the created value
   must satisfy a kind, that is, when it flows directly into a *required
   position*: an argument for a parameter with a declared kind, or with an
   enforced inferred requirement (§7.5); a store into a slot with a
   declared kind; or a `return` from a function with a declared result
   kind.
   - A PtrArith result in a required position must satisfy the kind.
   - A Cast to a wider pointee type in a required position must leave at
     least the object width of `U` from the pointer. Outside required
     positions a conversion carries no obligation: its result gets kind
     Single when the source's extent (exact or lower bound) covers the
     object width of `U` from the pointer, and otherwise Unknown, or a
     cursor with the source's base when that base is exact. The obligation
     then sits at the dereferences. So `(struct s *)malloc(sizeof(struct
     s))` is Single, an end sentinel `(struct S *)(buf + len)` compiles
     without a trap, and a flexible-array allocation of
     `offsetof(struct S, data) + 3` bytes is enough for `p->a`.
   - An IntToPtr site is `unresolved(raw-cast)`, or `trusted(unsafe)`
     inside a region. A null pointer constant is not a site.

   **Departure:** the resolutions place an obligation at every cast to a
   larger pointee type. Checking at the conversion trapped correct code
   (end sentinels built by casting, flexible-array structs smaller than
   `sizeof`), so outside required positions the obligation moves to the
   uses, as it does for pointer arithmetic.
5. **Stores into unannotated slots are not checked.** A store that is not
   Single-valid makes the slot's inferred kind `Unknown` (§7.3), which moves
   the cost to the slot's uses. This is what lets `s->end = s->buf + cap`
   compile without a false trap.
6. **Library calls** check each buffer argument against the entry's
   requirement (§8).
7. **Grouping.** Stores that update a pointer and its count, per a
   declared or inferred counted relation, form a group when they occur in
   one basic block with no intervening call, loop or access through the
   object. This follows `-fbounds-safety`. The obligation is checked once,
   after the last store of the group. Between the stores, accesses use
   local facts, not the relation.

#### 7.5 Parameter must-access inference

`KindInference` computes a requirement for each pointer parameter `p`. It
uses only the function's CFG and dominator and post-dominator trees; the
engine is not involved.

A *must-access* is an access that executes on every path from entry to
normal exit, provided that:

- its block post-dominates the entry, or it is inside a *canonical counted
  loop* (below) that does;
- `p` is not assigned before it;
- no earlier branch tests `p`'s nullness;
- no path reaches it after an early `return`, `goto` out of the loop, or
  call to a `noreturn` function;
- every call on a path from entry to it, including calls in the loop body
  before it, is *known to return*: a `LibrarySpec` entry without
  `noreturn` or `exits`, a function in the TU whose summary says it always
  returns, or a function in a C library, POSIX or platform header without
  a `noreturn` attribute. Unknown and indirect callees, and a
  `fatal()`-style helper whose summary says it may not return, disqualify
  the access.

Rules:

- **R1.** A must-access `*p`, `p->f` or `p[0]` gives `Single`, nonnull.
- **R2.** In a canonical counted loop `for (i = c; i < e; ++i)` whose body
  must-accesses `p[i]` or `p[i + k]`, the requirement is
  `c < e → Counted(e + k)` and, for the nullability, `c < e → nonnull`.
  The loop is canonical when:
  - `c ≥ 0` is a constant;
  - `e` is an affine term over parameters;
  - the body contains no `break`, `return` or `goto` out;
  - `i`, `e` and `p` are not otherwise modified in the body.

  `i != e` counts the same as `i < e`, and `i <= e` gives the guard
  `c <= e` and `Counted(e + k + 1)`. The guard is part of the requirement,
  because a loop that runs zero times accesses nothing: `f(NULL, 0)` meets
  the requirement of `for (i = 0; i < n; ++i) p[i + 1] = 0;`. A requirement
  that evaluates to zero or less elements is empty.
- **R3.** A canonical loop `for (x = p; x < q; ++x)`, or
  `while (p < q) … p++`, that must-accesses `*x` gives
  `p < q → EndedBy(q)` and `p < q → nonnull`. `x <= q` gives
  `p <= q → EndedBy(q + 1)`.
- **R4.** A canonical scan `while (p[i]) i++`, `while (*s) s++` or
  `for (; *s; ++s)`, or a must-call passing `p` to a `LibrarySpec` argument
  that requires a string, gives `NulTerminated`.
- **R5.** An unguarded must-access `p[e]` with `e` an affine term over
  parameters gives `Counted(e + 1)`.
- Anything else gives no requirement.

Requirement terms are evaluated as mathematical integers: `e + k` with a
negative `k` and an unsigned `e` does not wrap to a huge count, and a
result of zero or less means no requirement. At a call, the guard and the
terms are checked with the overflow-checked term arithmetic of §10.2.
Requirements are therefore necessary: a correct caller never violates one
in the covered cases.

Enforcement depends on the function and on the part of the requirement.
**The nullability part is never trusted.** In every body, the null facet of
a must-access dereference of a parameter stays as §3.2 decides it (checked
unless proven), because `nonnull` is always expressible:

- **Static functions whose address is not taken.** At every direct call
  site, the Call site gets a spatial or null facet for the argument:
  proven, checked (`nonnull`, `len`, `span`, each under the requirement's
  guard), a violation (an error, as in probe 09), or
  `unresolved(unknown-extent)` when the argument's extent is unknown.
  Inside the body the requirement holds, so the accesses it covers are
  proven, null facets included. A definite violation at a call is reported
  only when the must-access holds under every rule above.
- **Exported or address-taken functions.** Inside the body, accesses
  covered only by an inferred extent requirement beyond Single (`Counted`,
  `Sized`, `EndedBy`, `NulTerminated`) are `trusted(caller-contract)`.
  Their null facets stay checked. The requirement is exported in the
  record, verified at link against every caller the program contains, and
  offered as a fix-it annotation.

Declared kinds are enforced at every call site whatever the linkage.

#### 7.6 Counted-field invariants (the designated first cut)

For struct types defined in the TU or in user headers, `KindInference`
runs Houdini over candidate invariants.

- **Candidates.** For a pointer field `d` and an integer field `f` of the
  same struct, the candidates are `count(d) == f + c` and
  `bytes(d) == f + c`, with `c ∈ {0, 1}`.
- **Disqualification.** Before round 1, every candidate of a struct is
  dropped when any of these holds anywhere in the unit: the address of `d`
  or `f` is taken; an object of the struct is written byte-wise (a
  `LibrarySpec` argument with `w` or `rw` access that can reach it, or a
  character-typed store into it); or an object of the struct is copied or
  converted from an object of another type. These are the writes the
  store-group rule cannot see.
- **Round 1** is the unit's authoritative engine run (§2.6), with every
  candidate assumed at function entry (`EngineInput::fieldAssumptions`,
  §14). Every creation site (allocation of the struct, declaration,
  compound literal) and every store group (§7.4 rule 7) touching `d` or `f`
  must re-establish each candidate by the engine's facts at the end of the
  group. A zeroing creation (`calloc`, a zero-initialised declaration, a
  lowered allocation, §11) creates `d` null and `f` zero; a null `d`
  satisfies a candidate only when `f + c` is zero, so `c = 1` fails there.
  The engine publishes one `storeVerdict` per candidate and group.
  Candidates that are not re-established are dropped.
- **Further rounds** re-analyse only the functions containing a store
  group of a struct whose candidate was dropped, with the reduced set, and
  publish into a discarding adapter. They repeat until nothing is dropped,
  for at most three rounds in total. Candidates still standing after the
  third round are dropped.
- **The authoritative rows.** Round 1's rows stand for every function that
  loads or stores no field of a struct with a dropped candidate. Every
  function that does is analysed once more after the last round, with only
  the surviving invariants, and that run replaces its rows
  (`LedgerAdapter::beginFunction`). No published decision rests on a
  dropped candidate.
- **Application.** A spatial facet that the engine left
  `unresolved(unknown-extent)` may have a witness showing a load from
  `o->d`, where `d`'s struct has a surviving invariant. `LedgerAdapter::finish`
  upgrades such a facet to checked against `o->f + c` when that term is
  expressible (§10.3), and to `unresolved(inexpressible)` otherwise.

Only exact equalities survive, so a check against the invariant is never
tighter than the allocation. A surviving invariant becomes the field's
inferred `Counted` or `Sized` kind, and it is an exact extent (§7.1). For
a struct defined in a header, the unit record carries the invariant, the
per-TU verdict (`holds`, `violated` or `unknown`) and whether the unit
relied on it. The link step verifies these (§13; A3).

**Departure:** the resolutions describe templates `extent(p) == k*f + c`.
The candidates are narrowed to `k = 1` with `c ∈ {0, 1}` in element or
byte units. Those two units give the scales an allocation such as
`malloc(f * sizeof *d)` or `malloc(f)` produces, and each extra template
multiplies the store verdicts the engine must publish; wider templates are
left to RFC 0031.

This subsection is the designated first cut. If S6 overruns, it is removed
and fields fall back to §7.3. Gate G13 is sized to pass without it.

### 8. The library table

One declarative table, `lib/Core/LibrarySpec.txt`, replaces three
scattered library models:

- `lib/Analysis/Builtins.cpp` (529 entries);
- `lib/Analysis/RuntimeModels.{h,cpp}`;
- about 130 `name == "…"` tests for 54 libc functions in the transfer code.

CMake embeds the text in `libweavec-core` as a string. `core::LibrarySpec`
parses it once on first use; a parse error is a build-time test failure.
The engine and `CheckPlanner` both consult it. Gate H2 requires that no
libc name comparison remain outside it.

**Which calls a row governs.** A row applies to a direct call when both
of these hold:

- the callee's name is the row's name, or one of its aliases: every row
  `X` also answers to `__builtin_X`, and a `chk(…)` clause maps the
  fortified forms (`__builtin___X_chk`, `__X_chk`) onto the row with their
  argument positions remapped. On macOS at `-O1` and above the SDK turns
  `memcpy(d, s, n)` into `__builtin___memcpy_chk(d, s, n,
  __builtin_object_size(d, 0))` and `sprintf(d, f, …)` into
  `__builtin___sprintf_chk(d, 0, <size>, f, …)`, so without aliases every
  fortified call would miss the table;
- neither the TU nor, at link, the program defines a function of that
  name. A program's own `strlcpy`, `getline` or `malloc` uses its own body
  and summary, and zero-initialisation never redirects a call to it (§11).

A row applies to an indirect call whose solved slot contains the row's
function (`fp = memcpy; fp(d, s, n)`): exactly, when the slot is closed
with that one target; otherwise the row's spatial requirements are
`unresolved(callback)`.

#### 8.1 Entry schema

```cpp
namespace weavec::core {
struct LibraryParam {
  enum class Type : std::uint8_t { Int, Pointer, Function, Other } type;
  enum class Access : std::uint8_t { None, Read, Write, ReadWrite } access; // bytes behind the pointer
  std::optional<LibTerm> bytes;          // required accessible bytes from the pointer
  bool string = false;                   // must be NUL-terminated within its object
  enum class Null : std::uint8_t { Forbidden, Allowed, AllowedIfZero } null;
  std::optional<LibTerm> zeroTerm;       // AllowedIfZero: null is allowed when this term is 0
  enum class Effect : std::uint8_t { Borrow, Release, Realloc, Retain, Escape } effect;
  std::string family;                    // Release/Realloc: the release family
  std::string state;                     // Retain: the hidden state slot
  std::optional<LibCallback> callback;   // Function parameters
};
struct LibraryResult {
  enum class Kind : std::uint8_t { Void, Int, Fresh, Static, Arg, Interior, InteriorState, Unknown } kind;
  std::string family;                    // Fresh
  std::string state;                     // Static, InteriorState: the hidden state slot it points into
  std::uint8_t arg = 0;                  // Arg / Interior
  std::optional<LibTerm> extent;         // Fresh/Static: accessible bytes
  enum class Null : std::uint8_t { Never, OnFailure, May } null;
  bool zeroInit = false;                 // eligible for zero-init lowering (§11)
};
struct LibraryChk {                     // a fortified alias (§8, "Which calls a row governs")
  std::string name;                      // __builtin___memcpy_chk
  std::vector<std::int8_t> argumentOf;   // for each alias argument, the row's argument index, or -1 (dropped)
};
struct LibraryEntry {
  std::string name, header;
  std::vector<LibraryChk> chk;
  std::vector<LibraryParam> params;
  bool variadic = false;
  LibraryResult result;
  std::vector<LibDisjoint> disjoint;     // {a, b, length term}
  std::vector<std::string> invalidates;  // state slots whose borrows end (setenv → "environ")
  std::vector<std::string> reads;        // state slots read (strtok(NULL, …) → "strtok")
  bool noreturn = false, exits = false, returnsTwice = false;
  std::optional<LibFormat> format;       // printf/scanf family: format argument index, first vararg
};
}
```

`LibTerm` is a small expression over arguments:

```
term ::= INT | 'a' N | 'strlen(a' N ')' | 'fmtlen(a' N ')'
       | term '*' term | term '+' term | term '-' INT | 'min(' term ',' term ')'
```

`fmtlen(aN)` is the length of the output `snprintf` would produce for the
format at argument N and the arguments after it. It states a requirement;
it is never evaluated as a check term. `CheckPlanner` enforces a `fmtlen`
requirement by the `snprintf` lowering of §10.4, or proves it from a static
maximum for a literal format whose conversions are all bounded (`%d`,
`%c`, `%1.15g`, a `%s` with a precision).

#### 8.2 Text syntax

One entry per line, with `#` comments and `\` continuation. A pointer
parameter is `access[:size][:flags]`. A pointer parameter without `bytes`
or `str` requires a Single value (one element of its pointee type), and a
`fn` parameter without a callback clause is only stored or compared.

```
directive ::= 'header' NAME ';' | 'header' DIR '/*' ';' | 'builtins' ';'
entry    ::= NAME '(' params ')' '->' result { clause } ';'
params   ::= [ param { ',' param } ] [ ',' '...' ]
param    ::= 'int' | 'other' | 'fn' { ':' fflag } | access { ':' pflag }
fflag    ::= callback | 'null-ok'
access   ::= 'r' | 'w' | 'rw' | 'none'
pflag    ::= 'bytes(' term ')' | 'count(' term ')' | 'str' | 'null-ok'
           | 'null-if-zero(' term ')' | 'release(' FAMILY ')'
           | 'realloc(' FAMILY ')' | 'retain(' STATE ')' | 'escape'
           | 'init(' FAMILY ')' | 'fini(' FAMILY ')' | 'out(' value ')' | callback
callback ::= 'sync(' [ INT { ',' INT } ] ')' | 'entry(' [ INT ] ')' | 'at-exit'
result   ::= 'void' | 'int' [ ':value(' term ')' ] | 'noreturn' | value
value    ::= rkind { ':' rflag }
rkind    ::= 'fresh(' FAMILY ')' | 'static(' STATE ')' | 'arg(' INT ')'
           | 'interior(' INT ')' | 'interior-state(' STATE ')' | 'ptr'
rflag    ::= 'extent(' term ')' | 'null-on-failure' | 'nonnull' | 'null-ok'
           | 'zero-init' | 'no-zero-init' | 'str' | 'replaces'
           | 'or-fresh(' FAMILY ')' | 'or-static(' STATE ')' | 'offset(' term ')'
           | 'zero-filled'
clause   ::= 'disjoint(' INT ',' INT ',' term ')' | 'copies(' INT ',' INT ',' term ')'
           | 'fills(' INT ',' term ',' term ')' | 'writes-str(' INT [ ',' term ] ')'
           | 'invalidates(' STATE ')' | 'reads(' STATE ')' | 'exits'
           | 'returns-twice' | 'printf(' INT ',' INT ')' | 'scanf(' INT ',' INT ')'
           | 'chk(' NAME ':' INT { ',' INT } ')'
term     ::= INT | 'a' N | 'strlen(a' N ')' | 'fmtlen(a' N ')' | MACRO
           | term '*' term | term '+' term | term '-' INT
           | 'min(' term ',' term ')' | '(' term ')'
```

**Amendment (S4 preparation).** Writing the full table showed that the
first grammar could not express several effects that the `name ==` sites
of §8 encode, so these constructs were added; the header comment of
`LibrarySpec.txt` documents each one and is kept in step with this list:

- `header NAME;`, `header DIR/*;` and `builtins;` directives. They define
  the §5.2 header list and set each following entry's header.
- `count(t)`, a requirement in elements rather than bytes (the wide
  functions), and `MACRO` terms such as `BUFSIZ` and `PATH_MAX`, evaluated
  in the calling unit. The never-defined `__WEAVEC_UNBOUNDED` marks a
  requirement that is never proven or checked (`gets`).
- `out(v)`: the argument points to a pointer slot into which the call
  stores `v` (`asprintf`, `strtol`'s end pointer); `replaces` consumes the
  slot's previous value first (`getline`).
- `init(F)` and `fini(F)`: the object behind the argument acquires or ends a
  resource of family `F` while its own storage stays valid (`regcomp`,
  `regfree`, `glob`, `globfree`), so `fini` on a local is not an invalid
  release.
- `arg(N):or-fresh(F)` and `arg(N):or-static(S)`: when argument `N` is null
  the result is fresh (`realpath(p, NULL)`, `getcwd(NULL, n)`) or static
  (`tmpnam(NULL)`).
- String and copy facts that the engine's string and memory transfer used
  to hard-code: `copies(d,s,t)`, `fills(d,v,t)`, `writes-str(d[,t])`,
  `int:value(t)` (`strlen` returns `strlen(a0)`), `offset(t)` on interior
  results (`stpcpy`), `str` on string results and `zero-filled` on
  `calloc`.
- Overloads chosen by signature (glibc and BSD `qsort_r`, GNU and XSI
  `strerror_r`), `fn:null-ok` (`signal(SIG_DFL)`), an empty `sync()` for a
  callback that receives only the library's own storage (`nftw`), and
  callback flags on pointer parameters for function pointers stored in the
  pointee (`sigaction`'s `act`).

The C++ schema in `include/weavec/Core/LibrarySpec.h` extends §8.1 with the
corresponding fields and is authoritative for them.

`entry(N)` means that argument `N` of the call is passed to the target;
`entry()` means the target receives no pointer from the call (`signal`).
`chk(name: i0, i1, …)` lists, for each argument of the fortified alias, the
row argument it becomes, with `-1` for the extra size and flag arguments.
`interior-state(S)` is a pointer into hidden state slot `S`. Every
`fresh(free)` row carries `zero-init` or `no-zero-init` (§11).

Representative rows (the full table has about 600):

```
malloc   (int) -> fresh(free):extent(a0):null-on-failure:zero-init;
calloc   (int, int) -> fresh(free):extent(a0*a1):null-on-failure:zero-init;
realloc  (rw:null-ok:realloc(free), int) -> fresh(free):extent(a1):null-on-failure:zero-init;
reallocarray (rw:null-ok:realloc(free), int, int) -> fresh(free):extent(a1*a2):null-on-failure:zero-init;
aligned_alloc (int, int) -> fresh(free):extent(a1):null-on-failure:zero-init;
strdup   (r:str) -> fresh(free):extent(strlen(a0)+1):null-on-failure:zero-init;
free     (none:null-ok:release(free)) -> void;
memcpy   (w:bytes(a2):null-if-zero(a2), r:bytes(a2):null-if-zero(a2), int) -> arg(0) disjoint(0,1,a2)
         chk(__builtin___memcpy_chk: 0,1,2,-1) chk(__memcpy_chk: 0,1,2,-1);
memmove  (w:bytes(a2):null-if-zero(a2), r:bytes(a2):null-if-zero(a2), int) -> arg(0);
memset   (w:bytes(a2):null-if-zero(a2), int, int) -> arg(0);
strcpy   (w:bytes(strlen(a1)+1), r:str) -> arg(0) disjoint(0,1,strlen(a1)+1)
         chk(__builtin___strcpy_chk: 0,1,-1);
strncpy  (w:bytes(a2):null-if-zero(a2), r:bytes(min(a2,strlen(a1)+1)):null-if-zero(a2), int) -> arg(0);
strlen   (r:str) -> int;
sprintf  (w:bytes(fmtlen(a1)+1), r:str, ...) -> int printf(1,2)
         chk(__builtin___sprintf_chk: 0,-1,-1,1,2);
snprintf (w:bytes(a1):null-if-zero(a1), int, r:str, ...) -> int printf(2,3);
write    (int, r:bytes(a2):null-if-zero(a2), int) -> int;
read     (int, w:bytes(a2):null-if-zero(a2), int) -> int;
system   (r:str:null-ok) -> int;
getenv   (r:str) -> static(environ):null-ok reads(environ);
setenv   (r:str, r:str, int) -> int invalidates(environ);
strtok   (rw:str:null-ok:retain(strtok), r:str) -> interior-state(strtok):null-ok reads(strtok);
qsort    (rw:bytes(a1*a2), int, int, fn:sync(0,0)) -> void;
pthread_create (w, r:null-ok, fn:entry(3), none:null-ok:escape) -> int;
signal   (int, fn:entry()) -> ptr;
atexit   (fn:at-exit) -> int;
fopen    (r:str, r:str) -> fresh(fclose):null-on-failure;
fclose   (rw:release(fclose)) -> int;
freeifaddrs (none:release(freeifaddrs)) -> void;
exit     (int) -> noreturn exits;
longjmp  (other, int) -> noreturn;
setjmp   (other) -> int returns-twice;
alloca   (int) -> fresh(stack):extent(a0):nonnull:zero-init;
```

Semantics:

- **`fresh(F)`** creates an owned resource of family `F`. `stack` is the
  frame's family: it is released at return, and returning it is
  `lifetime-too-short` (probe 23).
- **`static(S)`** returns a borrow of hidden state slot `S`, a global
  place named `<S>`.
- **`retain(S)`** stores the argument in slot `S` for later `reads(S)`
  calls. Releasing a retained value while it is retained makes the next
  `reads(S)` a use-after-free (probe 25).
- **`invalidates(S)`** ends every borrow of `S` with a `Released` record.
  It is `conditional` (possible), because C does not guarantee that the
  storage moves (probe 25c).
- **`realloc(F)`** releases the argument on the non-null result class. On
  the null class it keeps the argument when the size argument is non-zero,
  and releases it when the size is zero, because glibc's `realloc(p, 0)`
  frees `p` and returns null (C23 makes a zero size undefined). The cases
  use the §9.1 grammar: `outcome null a0 freed when param 1 =0`. With a
  size known to be zero, `q = realloc(p, 0); if (!q) free(p);` is a
  definite double free; with an unknown size the record is conditional and
  the same code is a possible one. In the enforcing modes the zero-init
  wrapper maps a zero size to one (§11), which makes the behaviour the same
  on every C library.
- **`interior-state(S)`** returns a pointer into the value `S` retains:
  `strtok(NULL, …)` points into the string an earlier call retained, not
  into its own argument.
- Facets whose proof uses a `static(S)` or `interior-state(S)` extent, a
  `retain`/`reads`/`invalidates` relation or a callback clause are
  `trusted(library-spec)`.

String literals have a writable extent of zero. A write through a pointer
to one, directly or by a library argument with `w` access, is a definite
`out-of-bounds` (probe 25b).

With a literal format, a `printf`/`scanf` family entry checks that the
format's conversions match the number of arguments. Too few is a definite
`out-of-bounds`, because the call reads past the variadic arguments. A
non-literal format makes the call's spatial facet
`unresolved(inexpressible)`.

#### 8.3 Model fixes carried by the table

These rows fix the model:

- `system(NULL)` is allowed.
- Zero-length `read`, `write`, `memcpy`, `memmove`, `memset`, `memcmp`,
  `snprintf` and `strncpy` accept null. Their null facets use the
  zero-length form of the `nonnull` check (§10.2), which traps only when
  the length is non-zero, so an empty vector's `memcpy(d->data, s->data,
  0)` with null pointers runs.
- `getenv`, `strtok`, `localtime`, `strerror`, `getpwnam` and similar
  functions return static storage with the right lifetime.
- `longjmp` is `noreturn`.
- `realloc(p, 0)` may release `p` on the null class (§8.2).
- `freeifaddrs`, `freeaddrinfo`, `globfree` and `regfree` are releasers.

Clang 23 does not turn a declaration's `nonnull` attribute (glibc's
`__nonnull` on `memcpy`) into an IR `nonnull` argument, and LLVM does not
treat a `llvm.memcpy` with a non-constant length as proof of non-null; both
were checked against the reference toolchain. So a zero-length null
argument does not let the optimiser fold away a later WeaveC null check. A
rewrite-oracle case (`memcpy(NULL, x, 0)` followed by a checked
dereference that must trap) keeps it that way across LLVM upgrades.

Model changes made by converting Builtins, RuntimeModels and the `name ==`
sites are listed in the S4 ledger diff (§Implementation plan). Every row
has a unit test in `unittests/Core/LibrarySpecTest.cpp`. The expectations
come from an independent hand-written table (effect, nullability, extent
and clauses per function), not from the row itself, so a wrong row fails
its test. The macOS and glibc fortified spellings each have a test.

#### 8.4 Allocation failures and leaks

The new `allocation-failure` id is off by default. It reports a use of an
allocation result that is null or maybe-null from `allocatorSource`
without a test. The use is checked either way, so an out-of-memory path
traps instead of corrupting memory. This covers the only true positives
the corpus produced (7 unchecked `malloc` results).

`leak` is always a warning. It is not reported for resources live at a
`return` from `main`, or at a call whose entry has the `exits` clause
(`exit`, `_Exit`, `quick_exit`, `abort`).

### 9. Temporal precision

#### 9.1 Outcome-keyed effects

`FunctionSummary::outcomes` (RFC 0006) already keys consumption by the
result's class, and RFC 0009 guards can condition it on arguments. The
dump for Lua's `l_alloc` is:

```
outcome null ptr freed when nsize =0
outcome nonnull ptr moved
```

What is missing is the derivation for the common wrapper shape, where the
free happens on a path that returns a *local* (repros `realloc2`–`realloc4`,
jansson's `jsonp_realloc`):

```c
void *my_realloc(void *ptr, size_t old, size_t n) {
  void *m = malloc(n);
  if (m && ptr) { memcpy(m, ptr, old < n ? old : n); free(ptr); }
  return m;
}
```

The free's `MoveRecord` carries the guard `m nonnull ∧ ptr nonnull`.
Today, deriving the summary drops conjuncts on locals, so the summary says
`ptr: freed` whatever the result.

**Derivation rule.** At each `return e` where `e` names a local place `r`,
every conjunct on `r` in the guard of a consume record is translated into
the result's class set before the summary is derived:

- `r nonnull` becomes class `nonnull`;
- `r null` becomes class `null`;
- `r = 0` becomes class `zero`;
- `r positive|negative` becomes the matching integer classes.

The record's contribution goes to `outcomes[class]` instead of the
unconditional `effects`. Conjuncts on parameters stay as guards. All
other conjuncts are dropped, which keeps the effect on more classes.

**Case keys.** After derivation, each consume effect's key is canonicalised
to the grammar below:

```
case   ::= 'result' class-set [ 'and' ptest ]
class-set ::= class { '|' class }                 (class as in SummaryIO: null nonnull zero positive negative)
ptest  ::= 'param' INT ( '=0' | '!=0' )
```

A guard conjunct that is not a single parameter zero-test is dropped, and
the effect then holds on more paths. At most **two distinct cases** with a
non-empty consume set are kept per function. If there are more, all
classes are joined into the unconditional may-effect.

**Lossy effects.** Dropping a conjunct makes an effect hold on paths where
the program does not perform it. That is safe for proofs but not for
definite errors: a wrapper that frees `ptr` only when `m nonnull ∧ flag`,
with `flag` a local, would otherwise yield a definite use-after-free in a
caller on paths where `flag` was false. So every effect produced by
dropping a conjunct, or by folding classes into the may-effect under the
case limit, carries a `lossy` bit. A record made from a lossy effect stays
`conditional` even after a result test selects its class (§3.1), so it can
only ever give a possible warning.

SummaryIO format 27 spells cases with the existing
`outcome <class> <path> <flags> [when …]` lines, restricted to this
grammar, and spells the bit as the flag `lossy`.

At a call site, the cases are applied through the existing
`PendingOutcome` machinery:

- A consume selected by a test of the result, or by a known argument value
  satisfying `ptest`, becomes unconditional; `conditional` is cleared
  (§3.1) unless the effect is `lossy`.
- Without a test, it is a `conditional` record, which gives a possible
  warning.

Repros `realloc0`–`realloc5` and `luaalloc` become exact-expectation cases.

#### 9.2 Non-null facts from guard functions

This rule fixes repro `pred.c` and the `cJSON_Is*` cluster. For parameter
`i`, let `K` be the may-set of result classes over *every* path on which
the parameter is not proven non-null at the return: paths where it is
`Null` or `MaybeNull`, and paths where its nullness is unknown because it
was never tested. Then for every class `c ∉ K`, `nonNullOn[c]` gains
`param i`: a result in class `c` implies that the parameter was non-null.
The rule is the contrapositive, and it is applied when the summary is
finalised. It is derived only from the default-context analysis within
budget, never from a context run or an incomplete summary.

`if (!cJSON_IsString(x)) return; x->valuestring` then proves `x` non-null
on the fall-through edge. For `int ok(struct x *p, int k) { if (k) return
1; if (!p) return 0; return 1; }`, the `k` path returns 1 without testing
`p`, so `K` contains both classes and no fact is derived: after
`if (!ok(p, 1)) return;`, `p->v` keeps a checked null facet. This is a case
under `test/cases/semantics/`.

#### 9.3 Function-pointer slots

`SlotCollector` builds flow-insensitive, field-based constraints for every
function-pointer value in the TU. Slot keys:

- `field <record type key> <field>` (array elements share the field);
- `global <name>` (a TU-local static becomes `static <unit>:<name>`);
- `param <function> <i>` and `result <function>`;
- `local <function> <name>` (TU-private, eliminated before export).

Constraints:

- `f ∈ S`: a function designator stored, assigned, passed or returned into
  `S`, including in static initialisers.
- `S ⊆ T`: a copy of a value from slot `S` into `T`. Direct calls add
  argument-to-parameter and result-to-receiver copies. Casts between
  function-pointer types preserve the value.
- `open(S)`: `S` receives a value from outside the solved program. Sources
  are a parameter of an exported function, the result of an unknown
  callee, a load from memory the analysis cannot name, or an integer
  conversion.
- Dynamic call constraints: for an indirect call through `S`, and each
  target `f ∈ S`, the arguments flow into `param f <i>` and `result f`
  flows to the receiver.

The solver (`core::FnSlots`) iterates set inclusion to a fixpoint over
function names; it is linear in the number of constraints times the number
of targets. Each unit's record exports its constraints, with local slots
eliminated. The link step and `weavec --whole-program` solve them over all
records. This replaces the RFC 0014 callback-global fixpoint and its
`callbackGlobals` export, which are deleted in S7; their lit and
evaluation cases are kept as cases and must still pass.

**Closed slots.** A slot is *closed* when every value it can hold is
visible to the solver.

- In a per-TU compile, only two kinds of slot can be closed: fields of
  struct types defined in the main file (not in a header) whose objects
  never flow through parameters, results, unknown callees or externally
  visible globals, and `static` globals whose address does not escape.
  Every other slot is open, because objects of a header-declared struct
  can come from other units with other targets.
- At link, when the output is an executable, a slot is closed when every
  unit that can store into it has a record. A parameter of an exported
  function is closed only when no `unanalyzed-input` exists, the function's
  address never reaches an open position, and none of `-shared`, `-r`,
  `-rdynamic` or `-Wl,-export-dynamic` is given.

At an indirect call through `S`:

- **`S` closed, one target:** the call is analysed exactly as a direct
  call.
- **`S` closed, several targets:** the join of their summaries. A may-effect
  from any target is a may-effect. A consume is unconditional only if
  every target consumes unconditionally.
- **`S` open, with known targets:** as closed for temporal facts only. The
  call's temporal facet is `trusted(extern-contract)`, and the detail names
  the open source (for example "values stored by `json_set_alloc_funcs`
  parameter 1"). Spatial and null facts of the result never come from an
  open slot: the result gets the default of §7.3 for functions outside the
  TU (Single-or-nullable, null checked). Spatial and null outcomes are
  decided per TU and copied verbatim at link (§1), so they may not rest on
  a per-TU view of an open slot.
- **`S` open, without known targets:** the unknown-callee default with
  reason `callback` (§5.1).

Every indirect call also has a null facet on its callee operand, checked
with the function-pointer form of `nonnull` (§10.2) unless proven.

Lua's `g->frealloc` is stored from `lua_newstate`'s parameter, and
`luaL_newstate` passes `l_alloc`. So in whole-program mode the slot is
`{l_alloc}`, and the two injected allocator bugs are hard gate G12. In a
per-TU compile of `lstate.c` the slot is open and empty: the calls are
`unresolved(callback)`, not silent.

#### 9.4 Boundary invariants

`BoundaryInvariants` checks two invariants at every boundary. The
boundaries are every Call site (`boundary: call`), every LibCall site whose
entry has a `sync`, `entry` or `at-exit` callback clause (`boundary: call`),
and every function exit (`boundary: exit`), including calls that do not
return (`exit`, `_Exit`, `abort`, `longjmp`, `siglongjmp`, and any other
`exits` or `noreturn` entry or declaration), because `atexit` handlers,
signal handlers and `setjmp` sites run after them. It works over
the places reachable from parameters and globals, which the engine
publishes as `BoundaryFacts` (§14):

- **Validity (`dangling-escape`).** A reachable place that may hold a
  released pointer, or a pointer to storage whose lifetime has ended
  (automatic storage, a compound literal, `alloca` storage), and that is
  not overwritten before the boundary, gives the boundary
  `unresolved(dangling-escape)`.
- **Owner uniqueness (`second-owner`).** A slot is *owning* if some
  function in the TU releases a value loaded from it, which is a
  flow-insensitive fact. Two distinct reachable owning places that may hold
  the same owned object give the boundary `unresolved(second-owner)`.

Both invariants are assumed at function entry (A1 and A3). That is what
makes `free(s->a); free(s->b)` proven inside `cleanup`, and proven
temporal facets of loads from parameters sound. They never produce an
error except under require levels.

**Propagation.** An assumption that a boundary elsewhere breaks cannot
prove anything. Each boundary row names the *place class* it concerns: a
global `g`, or a field path `<struct>.<field>…` for places reached through
parameters. `LedgerAdapter::finish` then downgrades, in the whole unit,
every temporal facet that relied on the entry assumption for a place of
that class to the boundary's reason. The link step does the same over the
program ledger. In probe 02, `remember(p); free(p); return peek();` leaves
the global `g_cache` dangling at the call to `peek`; the propagation makes
`g_cache[0]` inside `peek`, the ASan-reported bug site,
`unresolved(dangling-escape)` instead of proven.

For probes c10 and c10c, the ordinary rules still report `double-free`
at the call. The boundary adds `unresolved(second-owner)`, so neither
program is ever proven.

### 10. Check planning and emission

#### 10.1 `CheckPlan`

`CheckPlanner` turns each checked *requirement record* (§2.5) into a
`core::CheckPlan` entry, whatever its facet's merged outcome, plus a guard
for each lowered violation (§3.4). `CheckPlanner::plan` is pure: it runs
inside `LedgerAdapter::finish` in every mode, including `weavec` and
`-fweavec-checks=none`, and decides expressibility (§10.3) before the
require-level errors and the summary line. A record whose terms are not
expressible becomes `unresolved(inexpressible)` there, so the ledger is the
same whether or not checks are emitted.

```cpp
struct CheckTerm;   // constant | place handle (a decl plus a field/deref path) | sizeof
                    // | + - * (evaluated by the term helpers) | strnlen(term, term)
struct CheckPlanEntry {
  SiteId site; Facet facet; std::uint16_t requirement; // index into the row's requirements
  enum class Template : std::uint8_t { Nonnull, Index, Span, Len, Disjoint, Assert } kind;
  enum class Form : std::uint8_t { Plain, IfNonZero, Function, Result, Violation } form;
  enum class Placement : std::uint8_t { WrapOperand, WrapIndex, WrapArgument, ReplaceAccess,
                                        BeforeCall, ReplaceCall } placement;
  std::uint8_t argument = 0;               // WrapArgument
  std::vector<CheckTerm> operands;         // the template's extra operands, in order
  std::optional<CheckTerm> guard;          // §7.5 requirement guard: check only when it holds
  bool proven = false;                     // verify mode: a check of a proven facet
};
```

The six templates keep their names, and two have extra *forms*. `nonnull`
has a zero-length form (`IfNonZero`, for `null-if-zero` arguments) and a
function-pointer form (`Function`, for indirect callees). `len` has a
result form (`Result`) for the `snprintf` lowering of `sprintf`. The
`Violation` form, of any template, is the unconditional trap for a lowered
violation that has no expressible check of its own (§3.4).
**Departure:** the resolutions list six templates with fixed signatures.
The forms are needed to avoid false traps on zero-length null arguments,
to cover null callbacks, and to check `sprintf` without evaluating its
arguments twice; `span` also takes an index operand (§10.2) so that a
cursor subscript never forms an out-of-bounds pointer.

A place handle is resolved in Analysis to a `clang::ValueDecl` plus a
member path. Core never sees Clang.

#### 10.2 Templates and the prelude

The helpers are `static` functions with `always_inline`, `nodebug` and
`unused`, appended to the predefines buffer in `BeginSourceFileAction`
under a pragma that silences `-Weverything`. The prelude uses no macros
(`__SIZE_TYPE__` and friends are not expanded in preprocessed `.i` input),
spells sizes as `__typeof__(sizeof 0)` and 64-bit values as
`unsigned long long`/`long long`, and calls only builtins. The trap
reason is the template name, which is also what the `TRAP` markers of §17.3
match. **Amendment (S2).** Because the helpers are `nodebug`, the
debugger attributes the trap to the access's own source line and column, but
the reason string that `__builtin_verbose_trap` places in debug info is lost
with the helper's frame. The location is the more useful of the two, so
`nodebug` stays; report mode and the verify category still name the
template. In trap mode (abridged; `A` stands for
`static __inline__ __attribute__((always_inline, nodebug, unused))`):

```c
A void *__weavec_chk_nonnull(const volatile void *p) {
  if (__builtin_expect(p == 0, 0)) __builtin_verbose_trap("weavec", "nonnull");
  return (void *)p;
}
/* nonnull, zero-length form: null is allowed when n == 0 (§8.3). */
A void *__weavec_chk_nonnull_n(const volatile void *p, unsigned long long n) {
  if (__builtin_expect(p == 0 && n != 0, 0)) __builtin_verbose_trap("weavec", "nonnull");
  return (void *)p;
}
/* nonnull, function-pointer form: the callee operand of an indirect call. */
A void (*__weavec_chk_nonnull_fn(void (*f)(void)))(void) {
  if (__builtin_expect(f == 0, 0)) __builtin_verbose_trap("weavec", "nonnull");
  return f;
}
A unsigned long long __weavec_chk_index(unsigned long long i, unsigned long long n) {
  if (__builtin_expect(i >= n, 0)) __builtin_verbose_trap("weavec", "index");
  return i;
}
/* The element p[i] of `width` bytes lies inside [base, base + bytes).
 * The address is computed in integers; p + i is never formed unchecked. */
A void *__weavec_chk_span(const volatile void *p, long long i, const volatile void *base,
                          unsigned long long bytes, unsigned long long width) {
  long long off = (long long)((unsigned long long)p - (unsigned long long)base), step, at;
  if (__builtin_expect(p == 0 || __builtin_mul_overflow(i, (long long)width, &step) ||
                       __builtin_add_overflow(off, step, &at) || at < 0 ||
                       bytes < width || (unsigned long long)at > bytes - width, 0))
    __builtin_verbose_trap("weavec", "span");
  return (char *)p + step;
}
A unsigned long long __weavec_chk_len(unsigned long long need, unsigned long long have) {
  if (__builtin_expect(need > have, 0)) __builtin_verbose_trap("weavec", "len");
  return need;
}
/* len, result form: the snprintf lowering of sprintf (§10.4). */
A int __weavec_chk_len_r(int written, unsigned long long have) {
  if (__builtin_expect(written >= 0 && (unsigned long long)written >= have, 0))
    __builtin_verbose_trap("weavec", "len");
  return written;
}
A void *__weavec_chk_disjoint(const volatile void *d, const volatile void *s,
                              unsigned long long n) {
  unsigned long long a = (unsigned long long)d, b = (unsigned long long)s;
  if (__builtin_expect(n != 0 && (a < b ? b - a < n : a - b < n), 0))
    __builtin_verbose_trap("weavec", "disjoint");
  return (void *)d;
}
A void __weavec_chk_assert(int c) {
  if (__builtin_expect(!c, 0)) __builtin_verbose_trap("weavec", "assert");
}
/* Bounded string length for str requirements; traps on null. */
A unsigned long long __weavec_strnlen(const char *s, unsigned long long max) {
  unsigned long long n = 0;
  if (__builtin_expect(s == 0, 0)) __builtin_verbose_trap("weavec", "nonnull");
  while (n < max && s[n]) ++n;
  return n;
}
```

**Term arithmetic.** Extra terms are never computed with C's own
arithmetic, which wraps for unsigned types, is undefined on signed
overflow, and turns a negative count into a huge unsigned one. `+`, `-` and
`*` in a term are calls to small term helpers over 64-bit integers, with
explicit signedness:

- a signed leaf enters as `long long`; a negative value used as an extent
  or count is 0, so every access past a negative count field traps
  (`struct b { char *WEAVEC_COUNTED_BY(len) d; int len; }` with
  `len == -1`);
- on overflow, a helper computing a *need* (a length, an offset, a
  requirement) saturates to the maximum, and a helper computing a *have*
  (an extent) saturates to 0. Both fail closed, so a wrapping `nmemb *
  size` for `qsort` traps instead of passing;
- `x - k` in an extent saturates at 0.

A term that no longer fits in 64 bits after conversion, or a leaf of an
unsigned type narrower than `size_t` that the program itself computed with
wraparound, is taken as the value the program computed (an allocation's
actual size argument, §7.4) or is `inexpressible`. The index operand of
`index` is converted to unsigned 64 bits, so a negative index fails
`i >= n`; the index of `span` stays signed, so `p[-1]` on a cursor inside
its object passes.

The helpers differ by mode:

- **Report mode.** The same names take three extra trailing arguments,
  `(const char *file, unsigned line, unsigned column)`. Instead of
  trapping they call
  `void __weavec_rt_report(const char *template, const char *file, unsigned line, unsigned column)`
  and return normally, which gives no guarantee (§Soundness). With
  `WEAVEC_RT_ABORT=1` the runtime aborts instead.
- **Lowered violations.** An unconditional
  `__builtin_verbose_trap("weavec", "violation")` inline at the site
  (§3.4); in report mode, a report with template `violation`.
- **Verify mode.** A second family, `__weavec_prv_*`, is identical except
  for the trap category `"weavec.proven"`. It is used for checks of proven
  facets.
- **Compilers without `__builtin_verbose_trap`.** The helpers use
  `__builtin_trap()`. `weavec-cc` always embeds a Clang that has it; the
  fallback exists for `runtime/weavec_chk.c` (§10.9), which the host
  compiler that builds WeaveC compiles.

#### 10.3 Expressibility

An operand that the rewrite wraps in place is evaluated exactly once and
is always expressible: the pointer of a dereference, the index of a
subscript, the argument of a call. That is why the null facet always has a
check.

Every *extra* term is expressible only if all of the following hold:

1. It is built from constants, `sizeof`, parameters, non-volatile locals
   whose address is not taken, `const` locals, and fields reached from
   those through `.` and `->`.
2. It contains no call except the prelude's own helpers: the term
   arithmetic helpers and `__weavec_strnlen` over a side-effect-free
   pointer. A `strlen(aN)` in a need term is computed as
   `__weavec_strnlen(aN, have)`, which reads at most `have` bytes; the
   check fails the same way whenever the true length is at least `have`.
   A `str` requirement is checked as
   `__weavec_strnlen(p, have) < have` against an exact extent `have` of
   `p`; without one it stays unresolved, or trusted when a caller contract
   covers it. `fmtlen` is never a term (§8.1).
3. It has no side effects: `Expr::HasSideEffects(ctx, true)` is false, it
   has no volatile access, and it contains no assignment or `++`.
4. The engine proves that each place in it still holds, at the site, the
   value the extent was derived from. In other words, the engine's extent
   term is expressed over places that are unmodified since the derivation.
   A term over a snapshot place with no C name fails this, as in the
   example in §4.
5. Each of its own memory accesses is proven, trusted, or checked by the
   term helper itself, at the site. Reading `b->cap` needs `b` proven or
   trusted non-null and live; `__weavec_strnlen` checks its own pointer for
   null and reads at most `have` bytes.
6. It fits in 64 bits.
7. No argument of the call and no part of the wrapped operand may write a
   place the term names. `memcpy(++p, q, n)` checked against the remaining
   bytes of the old `p`, or `p[shrink(s)]` checked against `s->len`, would
   test the wrong value.
8. It compares against an exact extent or a declared kind, never a lower
   bound (§7.1).

A requirement with a failing term is `unresolved(inexpressible)`, decided by
`CheckPlanner::plan` before any code is emitted.

#### 10.4 Side effects and placement

Rewrites never duplicate the evaluation of a user expression:

- `WrapOperand`, `WrapIndex` and `WrapArgument` replace a subexpression
  with a helper call that returns it.
- `ReplaceAccess` replaces a cursor access `p[i]` (or `*p`, `p->f`, with
  index 0) by `*(T *)__weavec_chk_span(p, i, base, bytes, sizeof(T))`, or
  the member access on that result, so `p` and `i` are each evaluated once
  and the address is formed only after the check.
- `BeforeCall` rewrites `call` into `(check, call)` with the comma
  operator. The call's type and value category are kept, and the check's
  operands must be expressible extra terms. A §7.5 requirement guard wraps
  the check: `(guard ? check : 0, call)`.
- `ReplaceCall` is used for Assume sites and for the `printf` family.
  `sprintf(d, fmt, …)` becomes
  `__weavec_chk_len_r(snprintf(d, have, fmt, …), have)`, and `vsprintf`
  becomes the same over `vsnprintf`. The write stays in bounds, the
  arguments are evaluated once, and the call returns the same value
  whenever it does not trap. It needs `snprintf`/`vsnprintf` declared in
  the unit (they are whenever `<stdio.h>` is included); otherwise, and for
  formats containing `%n`, the requirement is `unresolved(inexpressible)`.

Sequencing between the original operands is unchanged. C leaves the order
of function arguments unspecified, and the extra terms are side-effect
free and name no place an argument writes (§10.3 rule 7), so evaluating a
term in a helper does not change the result.

When one operand has several checks, `nonnull` is innermost, then
`index`. A `span` check subsumes `nonnull` (it traps on a null pointer), so
the planner never emits both on one operand.

Call-site checks of static-callee requirements (§7.5) and declared
requirements use `WrapArgument` for `nonnull`/`span` on the argument, and
`BeforeCall` with `len` for count requirements. `null-if-zero` arguments
use the zero-length form of `nonnull`, whose length operand must be an
expressible term.

#### 10.5 `DeferredCodeGenConsumer`

`WeaveCWrapperAction::CreateASTConsumer` (lib/Frontend/Driver.cpp,
currently building a `MultiplexConsumer{WeaveC, CodeGen}`) returns a
`DeferredCodeGenConsumer`. That consumer owns the inner CodeGen consumer
and the WeaveC unit driver, and it is a `clang::SemaConsumer`.

It forwards these immediately:

- `Initialize`, `InitializeSema`, `ForgetSema`;
- `GetASTMutationListener`, `GetASTDeserializationListener`;
- `PrintStats`, `shouldSkipFunctionBody`.

It records these, in call order, and forwards none of them yet:

- `HandleTopLevelDecl` (returns `true`), `HandleInlineFunctionDefinition`,
  `HandleInterestingDecl`;
- `HandleTagDeclDefinition`, `HandleTagDeclRequiredDefinition`;
- `CompleteTentativeDefinition`, `CompleteExternalDeclaration`,
  `HandleImplicitImportDecl`;
- `HandleOpenACCRoutineReference`, which C reaches under `-fopenacc`;
- for completeness, although C never calls them:
  `HandleTopLevelDeclInObjCContainer`,
  `HandleCXXImplicitFunctionInstantiation`,
  `HandleCXXStaticMemberVarInstantiation`, `AssignInheritanceModel` and
  `HandleVTable`.

The consumer overrides every virtual of `ASTConsumer` and `SemaConsumer` in
LLVM 23, forwarding or recording each. The list is part of the
LLVM-upgrade checklist: a virtual added upstream and not overridden would
reach CodeGen out of order.

`HandleTranslationUnit(ASTContext &)` then:

1. runs the unit analysis and planning (§1 step 2) and reports its
   diagnostics;
2. if no error occurred and checks are on, runs `CheckEmitter` over the
   plan;
3. replays the recorded calls in order;
4. calls the inner `HandleTranslationUnit`. CodeGen drops the module if
   any error occurred, as it does today.

Deferral is required. Without it, CodeGen has already emitted external
functions by the time the analysis runs; the prototype showed that only
lazily emitted static functions received their checks. The consumer is
used for every C code-generating action, whatever the checks mode. With
`-fweavec-checks=none` it changes nothing (gate G7).

**Amendment (S2).** "Changes nothing" holds for the corpus (gate G7: 153
TUs × 3 configurations byte-identical), not for every C input. Because the
replay sees the finished AST, CodeGen observes redeclarations that follow a
recorded callback. Three differences were found in hand-written units:
a C99 or GNU `inline` definition that a later `extern` declaration makes
external is emitted at a different position in the object; a tentative
array completed after its first use likewise; and `weak_import`,
`availability` or `visibility` on a later redeclaration now also applies to
earlier references (an `extern_weak` or `hidden` reference). The first two
change only symbol order. The third follows the declaration's final
attributes, as a non-incremental compiler would; it is documented in
`DeferredCodeGenConsumer.h`, and matching the eager order exactly would need
CodeGen internals, which is left to RFC 0031 if it matters in practice. It is not installed
for `-E`, `-fsyntax-only`, dependency-only actions, or C++/Objective-C
inputs, which pass through to Clang unchanged.

#### 10.6 `CheckEmitter`

`CheckEmitter` builds every rewrite through Sema:

- `DeclRefExpr` to the helper;
- `Sema::BuildCallExpr(nullptr, …)`;
- `Sema::ImpCastExprToType(…, CK_BitCast)` back to the operand's pointer
  type, so a dereference of the result is still an lvalue;
- `Sema::BuildBinOp(nullptr, …, BO_Comma, …)` for `BeforeCall`.

Each rewrite runs inside a `Sema::TentativeAnalysisScope`, which
suppresses the diagnostics of provisional analysis, together with a
`DiagnosticErrorTrap` to observe whether an error occurred. A result is
rejected when `isInvalid()` or `get()->containsErrors()`, and
`DiagnosticsEngine::hasErrorOccurred()` must be unchanged afterwards; a
unit test forces an invalid rewrite and checks all three. Stores, compound
assignment, `++`, `&p[i]`, `s->f` and array-to-pointer decay all keep
working, as the prototype showed at `-O0` and `-O2`, with `-g` and ASan,
and under `-std=c89 -pedantic-errors -Weverything -Werror`.

A rewritten child is put back through a parent-map child replacement on
the semantic form of the tree (the form CodeGen reads), not only through
the typed setters the prototype used (`setRHS`, `setBase`, `setSubExpr`).
Operands under an `OpaqueValueExpr` source were already excluded by the
planner (§2.1).

A rewrite that fails at this point cannot be turned into a ledger change,
because the ledger and the require-level errors were decided from the plan
(§10.1). The subtree is restored, never partially rewritten, and the
failure is reported as an internal error that fails the compile: "`WeaveC
internal error: could not insert the check for '<text>'; build with
-fweavec-checks=none to bypass`". Failing closed keeps the guarantee; the
rewrite oracle (G8) and the corpus builds (G9, G11) make such a failure a
release blocker. The
`-O2` pipeline removes checks the program already guards: a loop
`for (i < n) p[i]` checked against `n` keeps no trap.

#### 10.7 Modes

| `-fweavec-checks=` | Unproven facets | Proven facets | Zero-init default | Guarantee |
| --- | --- | --- | --- | --- |
| `trap` (default) | `__weavec_chk_*`, trap | nothing | on | yes |
| `report` | `__weavec_chk_*` report variant; links `libweavec_rt.a` | nothing | on | only with `WEAVEC_RT_ABORT=1` |
| `verify` | `__weavec_chk_*`, trap | `__weavec_prv_*` where expressible (trap category `weavec.proven`) | on | yes |
| `none` | nothing | nothing | off | no |

Verify mode is the soundness monitor. A `weavec.proven` trap means the
engine proved something false, and CI runs the probes, the cases and every
corpus test suite in this mode (gate G6). For verify mode the engine also
publishes a witness (§14) for proven spatial facets, not only for checked
ones, so that their extent and offset terms can be checked; the ledger
records which proven facets received a verify check
(`"check": {…, "proven": true}`) and the summary counts them. Temporal
facets are never checked at runtime in any mode.

`runtime/weavec_rt.c` (installed as `lib/weavec/libweavec_rt.a`) defines
`__weavec_rt_report`. It prints
`weavec: runtime check failed: <template> at <file>:<line>:<column>` to
stderr once per site, where `<template>` is `nonnull`, `index`, `span`,
`len`, `disjoint`, `assert` or `violation`, and aborts instead if
`WEAVEC_RT_ABORT=1` is set. `weavec-cc` adds it to the link line when the
link is given `-fweavec-checks=report`. Report mode is not cuttable: the
case runner and gate G11 use it to attribute traps to lines and templates
(§17.4).

#### 10.8 Require levels at emission

The errors of §6.3 are reported in step 3 of §1, after planning and before
emission, so they see the planned expressibility and a failing unit
produces no object. `WEAVEC_REQUIRE_SAFE` is read by `AttributeReader` and
applies per function.

#### 10.9 Precompiled headers and modules

Injecting the prelude into predefines would change the predefines of a
PCH built by plain Clang. When `-include-pch`, `-fmodules` or an implicit
PCH is in use, `CheckEmitter` does not inject the prelude. It builds the
helper `FunctionDecl`s directly in the AST as `extern` declarations without
bodies (building declarations through Sema needs no function scope), and
`weavec-cc` links `lib/weavec/libweavec_chk.a`, built from
`runtime/weavec_chk.c`, which defines the same helpers out of line. The
checks then cost a call; only PCH and module builds pay it, and only they
need the library. Building helper bodies in the AST is left to a later RFC:
it needs function-scope set-up in a release Clang with assertions off,
which the prototype never exercised. Preprocessor-only jobs, dependency
files and `ccache -E` never see the prelude, because only code-emitting
actions inject it. `weavec-cc -fweavec-print-prelude` prints the prelude
for the current mode, which the rewrite-oracle tests use (G8).

### 11. Zero-initialisation

In the enforcing modes (checks other than `none`, unless
`-fno-weavec-zero-init` is given):

- `weavec-cc` adds `-ftrivial-auto-var-init=zero` to the `-cc1` job, which
  covers locals, including VLAs. A local whose declaration a jump can
  bypass (declared in a `switch` body before the first `case`, or skipped
  by a `goto`) is not initialised by that option; its pointer uses get
  null facets `unresolved(no-zero-init)`.
- `CheckEmitter` rewrites every `DeclRefExpr` to a function whose
  `LibrarySpec` entry has the `zero-init` flag, unless the program defines
  a function of that name (§8). A call becomes a call to a prelude
  wrapper. Taking the function's address yields a static non-inline
  wrapper with the same signature, so `do_malloc = malloc` is also covered.
  A unit that defines any function named `malloc`, `calloc`, `realloc`,
  `free` or another `zero-init` entry lowers nothing, so an allocator
  implementation is never wrapped into recursion.

The wrappers call `__builtin_malloc`, `__builtin_calloc`,
`__builtin_realloc` and `__builtin_memset`, which need no declaration; the
prelude declares only the usable-size query (`malloc_size` on Darwin,
`malloc_usable_size` on glibc and musl, both compatible with the system
headers' declarations). Writing `usable(p)` for that query:

- `__weavec_malloc_zero(n)`: `p = calloc(1, n)`, then zero
  `[n, usable(p))`. `calloc` avoids touching fresh pages of large
  allocations.
- `__weavec_calloc_zero(m, n)`: `p = calloc(m, n)`, then zero
  `[m * n, usable(p))`.
- `__weavec_realloc_zero(p, n)`: `old = p ? usable(p) : 0;
  q = realloc(p, n ? n : 1)`, then, if `q`, zero
  `[min(n, old), usable(q))`. This clears both the tail a shrink leaves
  behind (a later in-place growth would otherwise expose stale bytes) and
  the part of a moved block that `realloc` did not copy. Mapping a zero
  size to one makes `realloc(p, 0)` behave the same on every C library.
- The same for `reallocarray`, `aligned_alloc`, `posix_memalign` and
  `memalign`.
- Every other `fresh(free)` row with the `zero-init` flag (`strdup`,
  `strndup`, `getline`, `getdelim`, `asprintf`, …) gets a wrapper that
  zeroes from the end of the bytes the call wrote, as the row's extent
  says, to `usable(p)`. Rows marked `no-zero-init` are not lowered, and
  their memory falls under A5.
- `alloca(n)` with a side-effect-free `n` becomes
  `__builtin_memset(__builtin_alloca(n), 0, n)`. Otherwise it is left as
  is, and pointers loaded from it are `unresolved(no-zero-init)`.

The invariant is that every byte of a lowered block's usable region is
either zero or was written by the program, so no `realloc` can expose a
stale pointer. On any target without a usable-size query,
zero-initialisation of the allocation family is unavailable: `weavec-cc`
warns once and behaves as `-fno-weavec-zero-init` for it.

A comparison of a function pointer with `malloc` (`fp == malloc`) sees the
wrapper and becomes false. This behaviour change is documented, and
`-fno-weavec-zero-init` avoids it.

The effect on the guarantee: a pointer read from uninitialised automatic
or lowered heap storage is null, and the null check traps. Probe c12 traps
instead of dereferencing garbage, and probes 08 and 32 become defined
reads of zero. With `-fno-weavec-zero-init`, every null facet whose
operand may be uninitialised is `unresolved(no-zero-init)`. Memory from
other allocators, from unknown callees and from the program's own free
lists is covered by A5. The usable-size query is only valid for the
system allocator, or for a replacement that interposes the query along
with `malloc`; a unit that defines the allocator itself is flagged at link
(§13.2). This assumption is named A5 rather than folded into A3, because
A3 is about code outside *U* and this one is about *U*'s own reads.

### 12. The ledger: JSON, SARIF, fingerprints and the summary line

#### 12.1 JSON schema (`weavec-ledger`, version 1)

```jsonc
{
  "schema": "weavec-ledger", "version": 1,
  "producer": {"name": "weavec", "version": "0.11.0", "revision": "abc1234"},
  "scope": "unit",                          // or "program"
  "root": "/abs/path/to/project",           // fingerprint root (§12.3)
  "config": {"checks": "trap", "zeroInit": true, "require": "none", "budget": 200000},
  "summary": {
    "sites": 4210, "proven": 3050, "checked": 980, "violation": 0, "unresolved": 150, "trusted": 30,
    "facets": {"spatial": {"proven": 0, "checked": 0, "violation": 0, "unresolved": 0, "trusted": 0},
               "null": {…}, "temporal": {…}, "assertion": {…}},
    "unresolvedReasons": {"unknown-extent": 70, "…": 0},
    "trustedReasons": {"system-api": 12, "…": 0},
    "unresolvedShare": {"spatialNull": 0.071},
    "errors": 0, "warnings": 2, "functions": 212, "overBudget": ["cJSON_ParseWithLengthOpts"]
  },
  "assumptions": {                          // program scope only
    "A1": {"exportedRequirements": 14, "verified": 11, "reliesOnSingle": 40, "unverifiedCallers": 3},
    "A3": {"headerInvariants": 2, "unverified": 0, "inputsWithoutRecords": ["liblua.a"]},
    "A4": {"concurrencySites": 0},
    "A5": {"nonLoweredAllocations": 4, "bypassedDeclarations": 0, "allocatorDefinedBy": null}
  },
  "units": [{
    "source": "cJSON.c", "object": "cJSON.o", "target": "arm64-apple-macosx15.0",
    "summary": {…},
    "functions": [{
      "name": "cJSON_Delete", "file": "cJSON.c", "line": 253, "linkage": "external",
      "overBudget": false, "requireSafe": false, "setjmp": false,
      "sites": [{
        "ordinal": 0, "kind": "deref", "line": 258, "column": 21, "text": "item->next",
        "boundary": null, "callee": null,
        "facets": {
          "null": {"outcome": "checked", "check": {"template": "nonnull"}, "fingerprint": "…"},
          "spatial": {"outcome": "proven", "fingerprint": "…"},
          "temporal": {"outcome": "unresolved", "reason": "unknown-callee",
                       "detail": "'global_hooks.deallocate' may have released 'item'",
                       "fixit": null, "requirements": [], "diagnostic": null, "fingerprint": "…"}
        }
      }]
    }]
  }],
  "diagnostics": [{
    "id": "use-after-free", "severity": "warning", "certainty": "possible",
    "message": "use of 'p' after it may have been freed", "file": "cJSON.c", "line": 1, "column": 1,
    "function": "f", "site": 3, "facet": "temporal",
    "notes": [{"message": "freed here on some paths", "file": "cJSON.c", "line": 1, "column": 1}],
    "fingerprint": "…"
  }]
}
```

Field rules:

- `text` is the site's source text with whitespace removed, truncated to
  80 bytes.
- `file` on a function is the root-relative path of the file that holds
  its definition, which differs from the unit's `source` for a function
  defined in a header. Sites take their function's file. (Amendment, S3:
  without it, sites of header functions had ambiguous line numbers.)
- `check` is present on checked facets. In verify mode, proven facets
  that received a check carry `"check": {"template": …, "proven": true}`.
- `requirements` lists, for LibCall, Release and call-site facets, one
  entry `{arg, need, have, outcome, reason?, check?}` per requirement that
  merged into the facet (§2.5). A requirement with its own `check` is
  enforced even when the merged outcome is `unresolved`.
- Each unit's `summary` carries `"a5": {"nonLoweredAllocations": …,
  "bypassedDeclarations": …}`: the calls to allocation rows that are not
  lowered and the pointer locals whose declaration a jump can bypass. The
  program's `assumptions.A5` sums them and names a unit that defines the
  allocator (§13.2).
- Paths are relative to `root`, with `/` separators, or absolute when
  outside it.
- Output is deterministic: functions, sites and diagnostics appear in
  source order, and object keys in the order shown.

#### 12.2 SARIF 2.1.0 mapping

`-fweavec-ledger-format=sarif` (`--ledger-format=sarif`) writes one run:

| SARIF | From |
| --- | --- |
| `runs[0].tool.driver` | `name: "weavec"`, `semanticVersion`, `informationUri`; `rules`: one per diagnostic id (`id` = the WeaveC id), one per `unresolved/<reason>` and one per `trusted/<reason>` |
| `runs[0].originalUriBaseIds.SRCROOT` | `root` |
| result for each diagnostic | `ruleId` = id; `level` `error` or `warning`; `kind: "fail"`; `message.text`; `locations[0]` (`artifactLocation.uri` root-relative with `uriBaseId: "SRCROOT"`, `region.startLine/startColumn`); `relatedLocations` from notes; `partialFingerprints["weavec/v1"]`; `properties.certainty` |
| result for each unresolved facet | `ruleId: "unresolved/<reason>"`, `level: "note"`, `kind: "open"`, message `<facet> of <kind> '<text>' is unresolved: <detail>`; fingerprint as above |
| result for each trusted facet | `ruleId: "trusted/<reason>"`, `level: "none"`, `kind: "review"` |
| proven and checked facets | not results; counts in `runs[0].properties.weavec.summary` |
| `runs[0].invocations[0].executionSuccessful` | no internal error occurred |

#### 12.3 Fingerprints

```
fingerprint = hex(SHA-256(UTF-8("weavec-fp/1" ␟ key ␟ path ␟ function ␟ message ␟ ordinal)))[0:32]
```

`␟` is U+001F. The components:

- `key` is the diagnostic id for a diagnostic, and
  `<kind>/<facet>/<outcome>[/<reason>]` for a facet row.
- `path` is the root-relative path. The root is the nearest ancestor of the
  source file that contains `.git`, else the compile job's working
  directory.
- `function` is the enclosing function's name, or `<file-scope>`.
- `message` is the diagnostic message, or for a facet row the site's
  `text`, normalised: every maximal run of ASCII digits becomes `0`,
  whitespace runs become one space, and the result is trimmed.
- `ordinal` is the 0-based index of the row among rows of the same
  function with the same `key` and `message`, in (line, column) order.

The fingerprint therefore survives edits elsewhere in the file and
function, and changes in sizes and line numbers. RFC 0032's baselines key
on it.

#### 12.4 The summary line

One line per TU. `weavec` always prints it; `weavec-cc` prints it under
`-fweavec-summary` or when `-fweavec-ledger` is given, and is otherwise as
quiet as Clang:

```
weavec: cJSON.c: 4,210 sites: 3,050 proven, 980 checked, 150 unresolved, 30 trusted; 0 errors, 2 warnings
```

Variants:

- With `-fweavec-checks=none`, and in `weavec`, "checked" reads
  `checkable (not enforced)`.
- Over-budget functions append `; 1 function over budget (<name>)`.

At link:

```
weavec: program minigzip: 9,876 sites in 3 units: …; 0 errors, 1 warning; 1 input without a WeaveC record (libz.a); unverified: 12 exported requirements (A1), 0 header invariants (A3)
```

### 13. Sidecar format 28 and the link step

#### 13.1 Framing

A unit record is written to `<object>.weavec` next to the object; the
transport is unchanged from RFC 0005. The file holds exactly one record,
which is self-delimiting so that RFC 0032 can place it verbatim into an
object section:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `89 57 56 43 0D 0A 1A 0A` (`\x89WVC\r\n\x1a\n`) |
| 8 | 4 | format, little-endian `u32` = 28 |
| 12 | 4 | flags, `u32` = 0 (readers reject non-zero) |
| 16 | 32 | schema fingerprint: SHA-256 of the codec's canonical field table (every key, its type and nesting), generated from the same table the encoder and decoder use |
| 48 | 8 | header length `H`, `u64` |
| 56 | 8 | payload length `P`, `u64` |
| 64 | `H` | header: UTF-8 JSON |
| 64+`H` | `P` | payload: UTF-8 JSON |
| 64+`H`+`P` | 32 | SHA-256 of bytes `[0, 64+H+P)` |

The header holds `producer`, `source`, `cwd`, `command` (the `-cc1`
arguments), `target`, `config` (as in §12.1) and
`object: {path, digest: "sha256:<hex>"}`.

The payload holds these keys:

- `functions`: `{name, linkage, addressTaken, typeKey, summary, kinds: {params, result}, reliesOnSingle, requirements}`.
  `summary` is SummaryIO format 27 text; `reliesOnSingle` lists the
  parameters whose Single default the body relies on (§7.3);
  `requirements` are the inferred exported requirements.
- `globals`: `{name, typeKey, kind}`.
- `imports`: `{name, declared: {params, result, ownership}, location, calls}`,
  for each external function the unit declares and calls; `calls` lists,
  per call, whether each pointer argument was Single-valid, so the link step
  can check reliance across units. Declared annotations and attributes are
  recorded for link verification.
- `indirect`: type keys of indirect calls.
- `unknown`: names of callees that were unknown in this TU.
- `slots`: `{slot, targets, sources, open}`, the exported slot constraints;
  they replace RFC 0014's `callbackGlobals`.
- `slotKinds`: `{slot, kind, demotedBy}`, the §7.3 slot kinds and the
  stores that demoted them.
- `invariants`: `{struct, field, template, verdict, relied, store}`.
- `contexts`: the RFC 0016 `memoryRequests` and `callbackRequests`, carried
  unchanged, so call-context runs keep working across units.
- `countFields`: RFC 0010's count-field keys, carried unchanged.
- `sizedFields` and `sizedFieldLoads`: RFC 0012's witnesses, refutations
  and loaded keys, carried unchanged. They are not replaced by
  `invariants`, because §7.6 may be cut.
- `boundaries`: the place classes of the unit's `dangling-escape` and
  `second-owner` rows, for program-wide propagation (§9.4).
- `sites`: per function, a compact row per site:
  `[ordinal, kind, line, column, spatial, null, temporal, assertion]`,
  each facet as `"<outcome>[/<reason>]"` or `null`.
- `reported`: `{id, file, line, column}` of diagnostics already reported at
  compile time.
- `definesAllocator`: whether the unit defines `malloc`, `calloc`,
  `realloc` or `free` (§11).

Records carry no evidence: no traces, notes or explanation text. Summary
format 27 is format 26 minus every component that only checked mode reads,
as determined by reachability in S1, plus `kind`, `result-kind`, the
`lossy` flag and the restricted `outcome … when` cases of §9.1.

Readers accept only format 28 with a matching schema fingerprint and a
valid digest. Anything else is a *stale record*, and the input is treated
as having none. There are no legacy readers, and unknown fields are never
skipped silently: the schema fingerprint is derived from the codec's field
table, so it changes whenever the payload schema changes, and a round-trip
unit test compares the keys the encoder emits with that table.

#### 13.2 The link step

When `weavec-cc` links (and `weavec --whole-program`, which uses the same
`ProgramAnalysis`):

1. **Collect inputs.** `collectLinkInputs` (Driver.cpp) no longer skips an
   input silently. It sees the `InputInfo`s and, separately, the `-l`
   arguments, which it resolves the way the linker does: the `-L`
   directories in order, then the toolchain's library directories, then
   the SDK or sysroot, preferring `.tbd`, `.dylib`/`.so` and `.a` in the
   linker's order for the target. An input is *system* when it resolves
   under the toolchain, SDK or sysroot library directories (`-lc`,
   `crt*.o`, `/usr/lib`, …), the same roots §5.2 uses for headers; a
   Homebrew `-lreadline` is not system. Every non-system
   input without a valid record (object, archive, shared library, stale
   record) is named in one `unanalyzed-input` warning per link, which lists
   them all. Calls from analysed units into functions no record defines
   are `trusted(external-unit)` when such an input exists, and
   unknown-callee otherwise.
2. **Solve slots** over all records (§9.3).
3. **Verify declarations.** For every import with declared annotations or
   kinds, compare against the defining unit's summary and kinds. A
   contradiction is an `annotation-mismatch` error at the declaration.
   This covers probe 38: a declaration says `WEAVEC_BORROWED`, but the
   definition frees the parameter.
4. **Re-run the engine** over every unit with a record (RFC 0005's
   algorithm, with the program database and solved slots) to refine
   temporal facets. Only the last round publishes (§2.6). Definite temporal
   violations found at link are errors, and the link fails as today;
   possible ones are warnings.
5. **Verify interfaces.**
   - For every cross-unit call to an exported function with an inferred
     requirement, decide the requirement at the caller: proven discharges
     the callee's `trusted(caller-contract)` in the program ledger;
     violation is an error; unresolved stays listed under A1.
   - For every cross-unit call whose argument for a `reliesOnSingle`
     parameter was not Single-valid (§7.3), add the Call site's
     `unresolved(unknown-extent)` spatial row, as within a unit.
   - For every header-struct invariant a unit relied on, every unit that
     stores to the fields must report `holds` or have no stores.
     `violated` is an `annotation-mismatch` error naming the store;
     `unknown`, or a unit without a record, leaves the invariant listed
     under A3. A header slot that is Single in one unit and demoted in
     another is listed under A3 likewise.
   - **Propagate boundary rows** program-wide (§9.4).
   - If a unit defines the allocator and another unit lowered allocation
     calls (§11), `weavec-cc` prints one driver warning, "`heap
     zero-initialisation assumes the system allocator, but '<unit>' defines
     'malloc'; rebuild with -fno-weavec-zero-init`", and the ledger records
     the unit under A5.
6. **Compose the program ledger.** Spatial, null and assertion facets are
   copied from the unit records, because they decided the code; the Call
   rows of step 5 are added. Temporal facets come from step 4. The ledger
   is written under `-fweavec-ledger`, and the summary line is printed
   under `-fweavec-summary` or when `-fweavec-ledger` is given.

The link step verifies A1 and A3 only where the records cover the callers
and the stores; everything else stays listed under those assumptions in
the summary line.

Static archives, shared libraries and ccache still do not carry records;
that is RFC 0032. The difference now is that the gap is named at every
link.

### 14. The engine seam

Everything an engine produces flows through one interface. RFC 0031 can
therefore replace `FunctionDataflow` by implementing `SafetyEngine`,
without touching the ledger, kinds, library table, planner, emitter,
formats or tests.

```cpp
namespace weavec::analysis {

/// Everything an engine gets for one unit.
struct EngineInput {
  clang::ASTContext &context;
  const SiteIndex &sites;                 // SiteCollector: Expr/Stmt -> SiteId, per function
  const KindTable &kinds;                 // declared + must-access + slot kinds (§7)
  const core::LibrarySpec &library;
  const core::FnSlots &slots;             // local or program-wide solution (§9.3)
  const ProgramDatabase *database;        // other units' records, at link; else null
  const std::vector<FieldCandidate> &fieldAssumptions; // §7.6 candidates assumed at entry
  EngineOptions options;                  // budget, zeroInit, require, verify, strictAliasing, dumpStream, stats
};

/// The only channel from an engine to the ledger.
class LedgerAdapter {
public:
  /// Starts the authoritative pass over `function` and discards every row an
  /// earlier pass recorded for it (§2.5, §2.6). Houdini rounds and fixpoint
  /// rounds publish into a discarding adapter instead.
  void beginFunction(const clang::FunctionDecl &function);
  /// One decision for one facet of a known site; may be called more than once
  /// within a pass (§2.5).
  void decide(const clang::Stmt &site, core::Facet facet, core::SiteOutcome outcome,
              std::optional<core::UnresolvedReason> unresolved = {},
              std::optional<core::TrustReason> trusted = {}, std::string detail = {});
  /// A diagnostic with its certainty; `site`/`facet` link it to its row.
  void report(core::Diagnostic diagnostic, core::Certainty certainty,
              const clang::Stmt *site = nullptr, std::optional<core::Facet> facet = {});
  /// For a checked spatial or null facet, and for a proven one when
  /// `EngineOptions::verify` is set: what the check needs (extent, base,
  /// offset, lengths, whether the extent is exact or declared).
  void witness(const clang::Stmt &site, core::Facet facet, CheckWitness witness);
  /// Caller-visible places at a call or exit that may hold released pointers
  /// or aliased owners (§9.4).
  void boundary(const clang::Stmt &site, BoundaryFacts facts);
  /// A function body exceeded its budget (§5.5).
  void overBudget(const clang::FunctionDecl &function);
  /// Store verdicts for field-invariant candidates (§7.6).
  void storeVerdict(const clang::Stmt &store, FieldCandidate candidate, core::Verdict verdict);
  /// Fills defaults (§2.6), applies BoundaryInvariants and their propagation,
  /// the UNSAFE/setjmp/concurrency overrides and the §7.6 upgrades, runs
  /// CheckPlanner::plan (§10.1), and returns the unit's ledger and plan. The
  /// require-level errors are derived from the result.
  [[nodiscard]] PlannedLedger finish();
};

/// What RFC 0031 implements.
class SafetyEngine {
public:
  virtual ~SafetyEngine();
  /// Analyse every emitted function of the unit, publishing through `out`.
  virtual void analyzeUnit(const EngineInput &input, LedgerAdapter &out) = 0;
  /// Summaries and exported facts for the unit record (§13.1).
  [[nodiscard]] virtual UnitExports exports() = 0;
  /// `--dump-analysis` text for one function (unstable format).
  virtual void dump(const clang::FunctionDecl &function, llvm::raw_ostream &os) = 0;
};

} // namespace weavec::analysis
```

The auxiliary types:

- `CheckWitness` carries place handles and terms: the object base; the
  extent term (with `scale`, `offset` and the place handle); the offset
  term or index; for LibCall sites, the per-argument lengths; and, when
  the pointer was loaded from a field, that field and the object it was
  loaded from (§7.6 *Application*). Handles are resolved against
  `EngineInput::context`.
- `BoundaryFacts` lists, as summary paths from the boundary's parameters
  and globals, the places that may hold a released or dead pointer (each
  with its release location), and the pairs of owning places that may hold
  the same object, each with its place class for propagation (§9.4). The
  engine also marks, per temporal decision, the place class whose entry
  assumption it relied on.
- `FieldCandidate` is `{record type key, pointer field, integer field,
  template}`.
- `KindTable` holds, per parameter, result and slot, the declared or
  inferred kind with its `KindSource`, whether it is exact, declared or a
  lower bound (§7.1), and the `reliesOnSingle` flags.
- `PlannedLedger` is `{core::Ledger ledger; core::CheckPlan plan;}`.
- `core::Certainty` is `{Definite, Possible}`.
- `core::Verdict` is `{Holds, Violated, Unknown}`.

Two rules keep the seam honest:

- `FunctionDataflow` publishes nothing except through `LedgerAdapter`,
  including its diagnostics: `DiagnosticSink` is no longer passed to it.
- The new components (`SiteCollector`, `AttributeReader`, `KindInference`,
  `SlotCollector`, `BoundaryInvariants`, `CheckPlanner`, `LedgerAdapter`
  itself) never include `Dataflow.h`.

Gate H2 greps for both.

### 15. Changes inside `FunctionDataflow`

The edits are bounded to this list. Anything beyond it needs an amendment:

1. **S1: delete the checked hooks.** This covers
   `checkedBefore`/`checkedAfter`, about 85 `checkContracts` branches,
   about 50 per-`CallExpr` post tables, the `SafetyState` optional, the
   public checked members (`checkedOutputClasses`, `recursiveContractCalls`,
   …), and `AnalysisOptions::{checkContracts, checked,
   checkedMainFileOnly, checkedFunctions, deferCheckedCalls}`.
   `AnalysisOptions::{reportUnannotated, strictExterns, exclusiveBorrows}`
   go in S3, with the sound defaults, so that S1's output stays
   byte-identical to the golden run.
2. **Route diagnostics through the adapter.** `report()` and
   `flushDiagnostics()` go through `LedgerAdapter::report` with a
   certainty, and each diagnostic site also calls `decide` for its facet.
   The call sites are `reportUseOfMoved`, the double-free branch,
   `checkDereference`, `checkResultDereference`, the argument-null check,
   the bounds reporter, `reportLeak` and the RFC 0007/0008 ids.
3. **Replace `reportIncomplete`** with
   `decide(site, facet, Unresolved, reason, detail)`. The reason
   strings map to `budget` ("… limit reached"), `unanalysed`
   ("unsupported …", "unresolved array …", "… is ambiguous"),
   `raw-cast` ("incompatible or unknown object view …", "unsupported
   memory copy of pointer-containing storage") and `inexpressible`
   ("unrepresentable …"). The original string is kept as `detail`.
4. **Publish decisions in the final pass.** In `Phase::Final`, publish
   proven, checked and unresolved decisions for every Deref, Index,
   LibCall, Release, Call, Cast and PtrArith site the pass reaches, using
   the null, spatial and temporal rules of §3, and attach `raw-cast` to the
   Deref and Index facets of pointers the engine knows were created by
   reinterpretation (union punning, byte-wise copies, `va_arg`).
5. **Certainty bits.** Add the `MoveRecord`, `NullRecord` and `Loan` bits
   of §3, and their setting and clearing.
6. **Unknown callees.** Remove the `annotation-required` paths and the
   system-header early return; apply the §5.1 effects.
7. **Library calls.** Consult `LibrarySpec` instead of `Builtins`,
   `RuntimeModels` and the `name ==` tests (S4).
8. **`inUnsafe`.** Its suppression becomes the §6.1 decisions.
9. **Assumptions.** Refute `WEAVEC_ASSUME` into `contradicted-assumption`
   and publish the assertion decision.
10. **Budget counting.**
11. **Special cases:** `setjmp` detection, callback application, and
    concurrency marking from the `G` set computed before the unit's
    analysis.
12. **Boundary facts and witnesses.** Publish `boundary(…)` at calls,
    callback LibCalls and exits (including non-returning calls), with the
    place class of each fact, and `witness(…)` for checked facets and, in
    verify mode, for proven spatial facets.
13. **Summary derivation.** Add the case translation and `lossy` bit
    (§9.1), the non-null-from-guard rule with the complete `K` (§9.2),
    parameter kinds and result kind in the summary, and the "always
    returns" fact must-access inference needs (§7.5).
14. **Consume kinds.** Seed the engine's `KnownExtent` from
    `EngineInput::kinds` at parameter entry, at field and global loads and
    at call results: Single gives the object width of the pointee, and
    `Counted(e)` gives `e` elements with `e` as a place. Each seeded extent
    carries whether it is exact, declared or a lower bound, and the spatial
    decision maps a lower bound to `unresolved(unknown-extent)` for any
    access it does not cover (§3.3). Without this, `p->f` on a parameter
    stays unresolved and gate G13 cannot move.
15. **Object extents.** Change `DataflowDynamicExtents` to the §7.4 object
    rules: flexible trailing arrays, complete-object extents for pointers
    made from members and elements, and the arithmetic model.
16. **Requirements.** Keep `requiresNonNull`/`requiresExtent` as may-facts
    for summaries only, drop the `markDereferenced` call in
    `checkRequiredArguments` for arguments of unknown nullness, and apply
    the refinement rule of §3.2.
17. **Temporal rules of §3.1.** Replace an `unknownOrigin` record on a
    known consume, and publish `may-alias-released` decisions.
18. **Passes.** In `TranslationUnitAnalyzer::run`, call `beginFunction`
    for the authoritative pass of each function (§2.6); run a generic
    reporting pass for functions that today report only through context
    runs; fold `reportConfirmedSizedFields` into it; keep context-run
    diagnostics and link them to the caller's Call site; and send the
    callback-global and SCC rounds to a discarding adapter. The same
    applies to `ProgramAnalysis`'s link rounds. The RFC 0014
    callback-global fixpoint is replaced by `FnSlots` (§9.3).
19. **Incomplete summaries.** Apply §5.5's rule at every use of a summary
    marked incomplete or produced when `MaxFixpointRounds` was exhausted.

Context-specialised runs publish summaries and diagnostics, never rows of
the function they analyse. The generic pass added by item 18 costs CPU on
Lua, where many functions report only through contexts today; gate G15
budgets it, and §5.5 bounds the context runs. Gate H2 requires
`Dataflow*.{h,cpp}` to be at most 25K lines after these changes, down
from 41.1K.

### 16. Command-line interface

`weavec-cc` flags:

| Flag | Default | Meaning |
| --- | --- | --- |
| `-fweavec` / `-fno-weavec` | on | analyse, check and zero-initialise; off is plain Clang |
| `-fweavec-checks=trap\|report\|verify\|none` | `trap` | §10.7 |
| `-fweavec-zero-init` / `-fno-weavec-zero-init` | on unless checks are `none` | §11 |
| `-fweavec-require=none\|checked\|proven` | `none` | §6.3 |
| `-fweavec-ledger=<path>` | unset | write the unit ledger (compile) or program ledger (link). A value ending in `/` or naming a directory writes `<dir>/<object>.ledger.json` per unit and `<dir>/<output>.ledger.json` at link, which is the form to use in `CFLAGS` under `make -j`; otherwise the file, and it is an error if one invocation would write two ledgers to it. Every ledger is written to a temporary file and renamed into place, so parallel writers never interleave |
| `-fweavec-ledger-format=json\|sarif` | `json` | §12 |
| `-fweavec-summary` / `-fno-weavec-summary` | off (on when a ledger is written) | the summary line on stderr |
| `-fweavec-budget=<n>` | calibrated (§5.5) | per-function block-transfer budget; 0 = unlimited |
| `-fweavec-link` / `-fno-weavec-link` | on | kept |
| `-fweavec-dump-analysis`, `-fweavec-analysis-stats=<path>` | off | kept as debugging flags |
| `-fweavec-print-prelude` | off | print the check prelude for the current `-fweavec-checks` mode and exit (used by the rewrite-oracle tests, G8) |
| `-W[no-][error=]weavec[-<id>]` | — | kept; `-Wweavec-allocation-failure` enables the off-by-default id |

`weavec` flags:

- Kept: `--whole-program`, `-p <dir>`, `--dump-analysis`,
  `--analysis-stats=<path>` and the `-W` flags.
- Added: `--ledger=<path>`, `--ledger-format=json|sarif`,
  `--require=none|checked|proven`, `--budget=<n>` and `--no-zero-init`.
  The ledger models a `weavec-cc` build with the default checks.
- The summary line is always printed.

Removed from both tools, with no aliases:

- `--checked`, `--checked-function`, `--checked-report`,
  `--checked-report-format`;
- `-fweavec-checked`, `-fweavec-checked-function=`,
  `-fweavec-checked-report=`, `-fweavec-checked-report-format=`;
- `--exclusive-borrows` and `-fweavec-exclusive-borrows` (RFC 0006's full
  exclusivity rule goes with them);
- `--strict-externs` and `-fweavec-strict` (subsumed by §5 and the require
  levels);
- `--analysis-cache` and `-fweavec-analysis-cache=`;
- `--analyze-headers` and `-fweavec-analyze-headers` (§5.6);
- `--report-unannotated` and `-fweavec-report-unannotated` (replaced by
  ledger fix-its).

An unknown `-fweavec-*` flag is an error, as today.

Two crashes are fixed; the second has a different cause in each tool:

- **`weavec --whole-program -p build` segfault.** `tools/weavec/main.cpp`
  keeps `CommonOptionsParser`, and with it `--`, `--extra-arg` and
  `--extra-arg-before`. The defect is the early return under `ZeroOrMore`
  when no source is named, after which `getAllFiles` dereferenced a null
  database. When no source is named and `-p` is given, the tool now loads
  the database with `CompilationDatabase::autoDetectFromDirectory` and
  takes its files. A missing database is an error with a message.
- **`-fdiagnostics-format=sarif` crash in `weavec-cc`.** Deleting
  `InputIdentity` removes the re-preprocessing path through
  `SarifDocumentWriter` (Driver.cpp:227/321). The link step's internal
  re-analysis forces `-fdiagnostics-format=clang`, because its diagnostics
  are relayed through the link's own engine in the user's format.
- **The same crash in `weavec`.** Here the crash is in `ClangTool::run`
  (`SarifDocumentWriter::createRun` under `BeginSourceFile`), because the
  tool's runs are the user-visible ones and no writer is attached. `weavec`
  creates the diagnostics engine for its runs itself and, when the flag is
  given, installs Clang's SARIF printer with a `SarifDocumentWriter` that
  it owns and closes after the last run. If that proves infeasible in S3,
  the flag is rejected with an error pointing to `--ledger-format=sarif`;
  either way the tool no longer crashes (H3). The WeaveC ledger has its
  own SARIF writer in both tools.

Both fixes get lit tests (gate H3).

### 17. Tests

#### 17.1 The golden oracle

`weavec` and `weavec-cc` built from e0e2bd6 (v0.10.0), out of tree, are
the golden oracle for parity. `test/cases/GOLDEN.md` records the commit,
the build recipe and the expected location, which is taken from the
`WEAVEC_GOLDEN_DIR` environment variable. The golden binaries are not
committed.

#### 17.2 `test/cases`

One tree, organised by feature, replaces `test/evaluation`, `test/recall`
and the per-RFC populations:

```
test/cases/
  README.md               marker grammar and runner usage
  GOLDEN.md
  KNOWN-DIFFERENCES.md    lit-pin misses versus the golden run (at most 25 entries, gate G3)
  evaluation/             the 44 bug + 32 clean programs (from test/evaluation)
  pairs/                  the 24 RFC 0017 cases (12 bug/clean pairs)
  recall/<CWE>/           the recall pins (from test/recall; 67 as scripts/recall.py counts them)
  engine/                 ordinary-era lit engine pins, reduced to (line, id) from the golden run
  soundness/              the 113 probes (85 bug, 28 correct) and their *_impl.c units
  repros/                 the 12 root-cause repros (realloc0–5, luaalloc, pred, zerolen, mainleak, arrloop, loopcorr)
  proofs/                 salvaged population cases that ever caught a false proof; SOURCES.md gives origin paths and commits
  semantics/<feature>/    new cases: ledger, unsafe, assume, require, kinds, extents, library, slots, boundary,
                          aliasing, concurrency, zero-init, emission
```

Importing a case adds a `main` driver, in S0, when its pinned bug is a null
or spatial finding that becomes checked. This lets the executable oracle
observe the trap. The frozen-hash protocol is gone, and cases are edited
like any source.

The review of this RFC produced cases that `semantics/` must contain, each
pinning a rule above: trailing-array and sub-object extents (Lua's
`upvals[1]`, `memcpy` into `&ts->contents`, `memset` through a first
member, a flat walk of `m[3][4]`, §7.4); a `qsort` comparator, `p + 1`
dereferenced in a callee given `int[2]`, and a sentinel read `p[n]`
(§7.1, all clean); `two(p, p)` and `b->cur` aliasing `b->data` (§3.1);
`get(a + 4, …)` into a static function and a cursor stored through a
`char **` (§7.3); `remember(p); free(p); peek()` and `free(g); exit(0)`
with an `atexit` reader (§9.4); `realloc(p, 0)` (§8.2); an empty vector's
`memcpy(NULL, NULL, 0)` (§8.3); a negative count field and a wrapping
`qsort` size (§10.2); a zero-trip loop, a `size_t` `i - 1` loop and a call
that exits before the access (§7.5); a cast end sentinel and a
flexible-array struct smaller than `sizeof` (§7.4); the `ok()` guard
(§9.2); a lossy wrapper (§9.1); a `nonnull`-only destructor (§5.1); a
handler racing a null test and a worker using a freed global (§5.3); a
C99 `inline` definition inlined at `-O2` (§2.6); `fp = memcpy` (§8); and a
`-Wno-error`-lowered out-of-bounds store that must trap (§3.4).

Salvage for `proofs/` covers three families: the reader cursor, `p + 1`
forwarding, and the candidates 99–102 leaks. It also covers every
population case whose expected outcome changed from accept to reject in
`git log` of `test/evaluation/rfc00NN/**/manifest.json`.

#### 17.3 Marker grammar

Markers are line comments. A *line marker* applies to the line it is on;
a *file marker* appears before the first declaration.

```
// CLEAN                               file: no errors, no warnings (except ALLOW), no traps
// ALLOW: <id> [<id> …]                file: warnings with these ids do not fail CLEAN
// BUG: <id> [definite|possible]       line: a diagnostic with <id> here; definite = error, possible = warning,
//                                     omitted = either; for null/spatial ids a TRAP on the same line also satisfies it
// TRAP: <template>                    line: running the program traps here with nonnull|index|span|len|disjoint|assert|violation
// RUN-INPUT: <argv…> [< <file>]       file: run the program with these arguments (repeatable; each run independent)
// UNRESOLVED: <facet>:<reason>        line: a ledger row here has that facet unresolved with that reason
// TRUSTED: <facet>:<reason>           line: likewise, trusted
// NOT-PROVEN: <facet>                 line: no ledger row here has that facet proven
// NEUTRALISED: zero-init              line: the defect is defined away by zero-initialisation (§11)
// MISS: <reason text>                 line: a known miss; counted in the denominator, expected silent
// EXPECT-LEDGER: <json-pointer> <op> <value>   file: e.g. /summary/unresolved <= 3; op is == != <= >= < >
// FLAGS: <weavec-cc flags>            file: extra flags for every compile and link (e.g. -fweavec-require=checked)
// UNITS: <file.c> [<file.c> …]        file: further translation units linked with this one
// ASAN                                file: also run the ASan oracle
// TOOL                                file: analyse with `weavec` (no build, no run)
```

Facet names are `spatial`, `null`, `temporal` and `assertion`; reasons are
the §2.3–2.4 spellings. For G4, the runner maps a `BUG` id to its
*matching facet*:

- `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`,
  `lifetime-too-short`, `mismatched-release` and `annotation-mismatch` map
  to temporal;
- `null-dereference` and `use-of-uninitialized` map to null;
- `out-of-bounds` and `invalid-release` map to spatial;
- `contradicted-assumption` maps to assertion.

#### 17.4 The runner

`scripts/run-cases.py` options:

- `--weavec-cc PATH` and `--weavec PATH` (default: `build/<preset>/bin`);
- `--jobs N` (default: CPU count);
- `--checks trap|verify` (default `trap`);
- `--require none|checked|proven` (adds `-fweavec-require` to every build;
  used by G5);
- `--asan` (run the ASan oracle for every case, not only `// ASAN` ones);
- `--legacy` (golden semantics: diagnostics only, no ledger);
- `--no-emission` (for S3, S4 and fallback points A and B, before checks
  are emitted: a *checked* matching facet at the pinned line satisfies a
  null or spatial `BUG` marker, as in gate G3, and `NEUTRALISED` markers
  count as `MISS`);
- `--no-run` (build, diagnostics and ledger only; used by the ASan CI job,
  gate H1);
- `--compare-golden` (also runs the golden binaries and fails on any
  difference in the sorted diagnostics; the S1 gate). From S3 on, cases
  whose golden run used a flag this RFC removes are excluded and listed in
  the *Excluded* section of `KNOWN-DIFFERENCES.md` (§17.6);
- `--filter GLOB`;
- `--json OUT`.

Each case is processed in these steps:

1. **Build.**
   - Compile each unit with `weavec-cc -c <FLAGS> -fweavec-ledger=<tmp>/`,
     then link the units with `weavec-cc` into `<tmp>/a.out` when some
     unit defines `main`.
   - With no `main`, compile only. Several units without `main` are
     analysed with `weavec --whole-program`.
   - `// TOOL` cases use `weavec --ledger`.
   - Timeouts are 120 s per compile or link.
2. **Diagnostics.** Parse `file:line:col: error|warning: … [weavec::<id>]`
   from stderr. Every `BUG` must match a diagnostic by line and id with
   the right severity class, unless a `TRAP` on the same line matches at
   step 4. For a `CLEAN` case, any error, or any warning whose id is not
   in `ALLOW`, fails the case. In any case, an error on a line without a
   `BUG` marker fails it.
3. **Ledger.** Check the `UNRESOLVED`, `TRUSTED`, `NOT-PROVEN` and
   `EXPECT-LEDGER` markers against the unit and program ledgers.
4. **Run**, when the build produced `a.out`:
   - Run the trap-mode binary once per `RUN-INPUT`, or once with no
     arguments, with a timeout of 10 s. It must end by a trap signal
     (`SIGTRAP` or `SIGILL`) if and only if the case has a `TRAP` marker.
   - Rebuild with `-fweavec-checks=report`, run again, and parse the
     `weavec: runtime check failed: <template> at <file>:<line>:<col>`
     lines. Every `TRAP` must match by line and template, and a reported
     check failure on an unmarked line fails the case.
5. **ASan oracle** (`--asan` or `// ASAN`).
   - Build with `weavec-cc -fno-weavec -fsanitize=address -g`, which is
     plain Clang with `weavec.h`, and run. `-fweavec-checks=none` would
     still fail on definite errors, so the probes projected as new errors
     could never be validated.
   - A soundness bug probe must produce an ASan report, which validates
     the probe. The report's first in-case frame is the bug site: the
     ledger row there must not have the matching facet proven (G4).
   - A correct twin must run clean.
6. **Verify.** With `--checks verify`, any `weavec.proven` trap fails the
   case, whatever its markers (G6).

Every bug probe is classified as `error`, `warning`, `trap`, `row` (a
non-proven matching facet or `UNRESOLVED`/`TRUSTED` match), `neutralised`,
`miss` or `silent`. The results JSON gives per-suite tallies of these
classes and per-case failures. The exit status is non-zero on any failed
case. `--legacy` interprets the same markers against the golden binaries;
it is how S0 proves the runner reproduces v0.10.0's numbers.

CTest registers one `cases-<suite>` test per top-level directory of
`test/cases`, so `ctest -j` runs the suites in parallel. CI runs them in
the Linux Release job in full, and in the ASan job with `--no-run`.

#### 17.5 The corpus gate

`scripts/corpus-gate.py` reads `test/corpus/`:

- **`manifest.json`.** Per project: `url`, a 40-hex `sha` (pinned; no
  branch names), `support` files, and a list of `configs`. The 11 configs
  are sds, cJSON, jsmn, log.c, printf, linenoise, cJSON-program, zlib,
  lua, linenoise-program and jansson. Each config has:
  - `compile`: files and arguments for per-file `weavec-cc -c`, or
    `build`: shell commands run with `CC` set to the binary under test;
  - `test`: the project's own test commands (cJSON's tests, jansson's test
    suite, zlib's `make test`, a named Lua `testes` subset, sds's
    `sds-test`, jsmn's tests);
  - `bench`: a command, an input and a repeat count;
  - `wholeProgram`: `true` for configs also analysed by
    `weavec --whole-program`.
- **`expected.json`.** A ratchet per config: `errors`, `warnings`, ledger
  counts, `unresolvedShare.spatialNull`, `cpuSeconds`, `workCounters`
  (block transfers, functions, sites), `traps` and `overhead`. The gate
  fails when a count or share gets worse, when CPU time exceeds the
  recorded value by more than 10%, or when a work counter exceeds it by
  more than 2%. `--update` rewrites the file, and CI fails if the
  committed file differs from what the run produced, so improvements are
  ratcheted in by the PR that makes them.
- **`triage.json`.** A list of
  `{fingerprint, config, id, certainty, file, line, verdict: "true"|"false", note}`.
  Every definite error and every possible temporal warning in a run needs
  an entry. A `false` verdict on a definite error fails G9. An untriaged
  finding fails the gate. Stale entries are reported but do not fail it.
  A `build` config with a triaged-true definite error lists
  `-Wno-error=weavec-<id>` for that id in its `manifest.json` entry, next
  to the fingerprint; no other lowering is allowed. The lowered site traps
  (§3.4), and a trap there in the project's test suite is a true positive,
  not a G11 failure. In per-file `compile` configs a definite error is
  recorded as a finding of its TU, not as a build failure.
- **`injections/injections.json`** plus `injections/<project>/*.patch`.
  About 30 entries of the form
  `{id, config, patch, file, line, expect: {ids, severity} | {trap, run}, mode: "unit" | "whole-program"}`.
  They include both Lua allocator bugs (the `luaM_free` use-after-free and
  the double `luaM_freearray`) in whole-program mode, and at least one
  entry per project.
- **`bench/`** holds `lua-bench.lua`, the `cjson-bench.c` driver and the
  zlib bench input generator.

Modes:

- `--quick`: per-file compiles and whole-program analyses only. It runs on
  every PR.
- `--full`: adds project builds, test suites, injections and benchmarks.
  It runs weekly and for the release.
- `--inject`: applies each injection patch, rebuilds and checks the
  expectation (G12).
- `--bench`: runs the benchmarks only (G14).
- `--checks verify`: runs every project test suite in verify mode (G6).
- `--legacy` and `--compare-golden`: as for the runner (S0 and S1).
- `--update`: rewrites `expected.json`.

The reference compiler is `$WEAVEC_LLVM_PREFIX/bin/clang`, used for
overhead baselines and for G7. `scripts/codegen-identity.py`
implements G7 over `test/corpus/identity.txt` (at least 100 TUs, none of
which has a definite error, since such a TU produces no object).

`scripts/check-hygiene.py` implements H2. `scripts/corpus.py`,
`scripts/corpus/projects.json` and `baseline.json` are replaced, and
`scripts/corpus/support/` (jansson's configuration header and the other
support files) moves to `test/corpus/support/`.

#### 17.6 Existing lit and unit tests

Lit tests outside `rfc0018-*` … `rfc0029-*` are kept, and each kept test
has one copy: an engine pin converted into `test/cases/engine` is deleted
from `test/Analysis`, `test/Annotations` or `test/WholeProgram`, and the
rest stay where they are. About 36 kept lit files and 74 unit-test
references across 12 files (`DiagnosticControlTest`, `ArrayOwnershipTest`,
`PointerIdentityTest`, …) name removed ids or messages:

- In S3, their `CHECK` lines and expectations are updated to the new
  messages, severities and ids.
- Tests of removed flags are rewritten for the replacement behaviour:
  `--strict-externs` and `--report-unannotated` become ledger fix-its and
  require levels; `--analyze-headers` tests are inverted, since headers are
  always analysed (`headers-skipped.c` becomes `headers-analysed.c`);
  `--exclusive-borrows` tests are deleted. The 11 affected files
  (`headers-skipped.c`, `rfc0002-borrows.c`, `rfc0016-clean.c`,
  `version.c`, `rfc0004-posix.c`, `ownership-annotations.c`,
  `rfc0003-unknown-extern.c`, `rfc0016-composition.c`,
  `rfc0017-numeric.c`, `rfc0004-function-pointers.c`, `rfc0005-flags.c`)
  are excluded from the G3 denominator and, once S3 removes the flags,
  from `--compare-golden`. They are listed in an *Excluded* section of
  `KNOWN-DIFFERENCES.md`, which does not count toward G3's 25 entries.
- New tests are named by feature (`test/cases/<area>/…`,
  `test/Emission/<feature>-*.c`); existing `rfcNNNN-` names may stay.

### 18. Deletions

Library code is deleted by reachability in S1. The order is: options
first, then the guarded branches, then whatever becomes unreferenced. The
list below gives the expected result; the S1 gate decides.

| What | Approx. lines |
| --- | --- |
| `lib/Analysis`: `DataflowSafety*.cpp` (5), `DataflowContainer*.cpp` (3), `DataflowFootprints`, `DataflowBuffer*.cpp` (3), `DataflowRecursive*.cpp` (3), `RecursiveContracts.cpp`, `DataflowCursors`, `DataflowStringTraversal`, `DataflowTraversalRelations`, `DataflowSpans`, `DataflowByteContents`, `DataflowFloating`, `FloatingCastSupport.{h,cpp}`, `DataflowCallbackContracts`, `DataflowCheckedCallbacks`, `DataflowCases`, `DataflowUnions`, `DataflowRuntime`, `DataflowObjectTypes`, `InterfaceTypes.{h,cpp}`, and the residue that becomes unreferenced | ~18.1K (17.2K in `Dataflow*`) |
| Checked hooks and guarded branches in `Dataflow.{h,cpp}`, `PlaceBuilder`, `Summaries`, `TranslationUnitAnalysis`, `CallbackSummaries`, `CallContextSummaries`, `SummaryDependencies` | ~3.5K |
| The RFC 0014 callback-global fixpoint, replaced by `FnSlots` (§9.3) | ~0.4K |
| `lib/Analysis/Builtins.cpp`, `RuntimeModels.{h,cpp}`, the `name ==` libc tests | ~1.5K |
| Core: `Safety.{h,cpp}`, `CheckedContract.cpp`, `CheckedIO.{h,cpp}`, `SafetyEntryPool`, `SafetyCallPath`, `Container`, `Footprint`, `Buffer`, `Union`, `Traversal`, the checked and legacy parts of `Summary`/`SummaryIO`, `SpatialOutcome`/`SpatialReason` | ~6.5K |
| Frontend: `CheckedReport`, `CompactReport`, `CheckpointExplanations`, `CheckedArtifacts`, `AnalysisCache`, `InputIdentity` (about 1.4K in the named files and their headers), and the checked parts of `Driver`, `ProgramAnalysis`, `Sidecar`, `tools/weavec` | ~2K |
| Unit tests for the deleted code (`SafetyTest`, `ContainerTest`, `BufferTest`, `TraversalTest`, `UnionTest`, `FootprintTest`, `RecursiveC*Test`, `CheckedCodeTest`, `RuntimeContractsTest`, `UnionCasesTest`, `AnalysisCacheTest`, `CheckedReportTest`, `InterfaceTypesTest`, the checked parts of others) | ~12K |
| Lit tests for superseded RFCs (`test/*/rfc0018-*` … `rfc0029-*`, 105 files) | ~1.4K |
| `test/evaluation/` (after import: 1,959 files, including 1,431 C fixtures, 153 manifests, 161 hash inventories and 114 provenance files) and `test/recall/` (moved) | ~39.5K |
| `scripts/checked-*.py` (14), `test_checked_evaluation.py`, `incremental-evaluation.py`, `scalability-evaluation.py`, `generate-recursive-oracle.py`, `evaluate.py`, `recall.py`, `corpus.py` and their tests, and the 243 population CTest registrations | ~4.2K |
| `scripts/corpus/rfc00NN-results.json`, `rfc00NN-diagnostics.jsonl`, `rfc0014.json`, `rfc0015.json`, `baseline.json`, `scripts/corpus/README.md` | ~283K (generated) |
| `docs/validation-rfc0014.md` … `rfc0029.md`, `docs/checked-code.md`, `docs/incremental-analysis.md`, `docs/development-history.md`, `docs/examples/checked.c`, and the per-RFC changelog prose in `README.md` | ~8K |

**Kept, although named like checked code.** The RFC 0017 integer files are
ordinary-mode code, called unconditionally from the transfer functions, and
stay: `DataflowNumericInputs`, `DataflowNumericOutputs`,
`DataflowIntegerProofs` (`operationDoesNotOverflow`),
`DataflowLoopRequirements`, `DataflowGuardCompleteness` and
`DataflowCheckedIntegers` (the `__builtin_*_overflow` calls), about 1.4K
lines, with their unit tests (`LoopRequirementsTest`,
`GuardCompletenessTest`, `NumericInputsTest`). Only their `checkContracts`
branches go. `lib/Core/CheckedInteger.cpp` is kept too: it is RFC 0017's
integer-overflow evaluator.

**The line budget.** Library code is 77.2K lines today. The deletions
above remove about 32.0K from `lib`, `include` and `tools`. The authored
code is estimated at about 16.3K lines: the design's 17.5K, less the Core
JSON and SHA-256 implementations that the Frontend codecs no longer need
(§1). That gives about 61.5K against gate H2's 62K. The margin is small,
so S8 measures it before the documentation work, and any overrun is closed
by deletion, never by raising the gate. `Dataflow*` lands at about 24K
against its 25K cap.

Diagnostic ids removed: `analysis-incomplete`, `annotation-required`,
`checking-incomplete` and `checking-failed`. Annotation removed:
`WEAVEC_CHECKED`.

### 19. Documentation, RFC statuses, release tooling and CI

**RFC statuses:**

- RFCs 0018–0029 become **Superseded**. In each, the `- **Status**:` line
  becomes `Superseded`, the `- **Supersedes / superseded by**:` line
  becomes `Superseded by RFC 0030`, and a first line
  `> Superseded by [RFC 0030](0030-prove-or-trap.md).` goes under the status
  block; the text is otherwise unchanged. The docs site reads the status
  from that line (`docs/scripts/prepare-content.mjs`). RFC 0029 is
  superseded while Accepted: its cost blocker is closed by deletion, not
  resolved.
- RFC 0001 stays **Accepted**. A note under its status block and in its
  *Soundness* guarantee block points to RFC 0030, *Soundness*. Its model
  (kinds, moves, loans, lifetimes) is unchanged.
- Amendment notes, one short blockquote each, are added where earlier
  RFCs say something this RFC changes:
  - 0002: default severities;
  - 0003: `annotation-required`, the unknown-callee default,
    `--report-unannotated` and `--strict-externs`;
  - 0004: `WEAVEC_UNSAFE` suppression, `--strict-externs` and its
    `unsafe-operation` forms, boundaries;
  - 0005: sidecar format, the link step, `--analyze-headers`, and stale
    sidecars, now `unanalyzed-input`;
  - 0006: `--exclusive-borrows`;
  - 0007: leak severity and main-exit leaks;
  - 0008: null and uninitialised severities, `annotation-required`;
  - 0011: may-out-of-bounds errors;
  - 0012: `WEAVEC_ASSUME` trust;
  - 0013: "no runtime instrumentation";
  - 0014: "no runtime checks", `analysis-incomplete`, and the
    callback-global fixpoint (replaced by §9.3);
  - 0015: `analysis-incomplete`;
  - 0016: `analysis-incomplete` and `--strict-externs`;
  - 0017: "no runtime instrumentation" and `analysis-incomplete`.
- `docs/rfcs/README.md` gets an index row for 0030, and the rows of
  0018–0029 change to Superseded.

**Repository documentation:**

- `README.md`: the quick look first; one guarantee statement (this RFC's,
  shortened); the modes; no per-RFC changelog.
- `docs/pages/reference/guarantees.md`: replaced by the §Soundness
  guarantee, A1–A5 and the blame property.
- `docs/annotations.md`: every id with its severity, message and trigger,
  plus the new and removed macros.
- `docs/pages/reference/cli.md`: §16.
- `docs/architecture.md`: §1 and §14.
- `docs/roadmap.md`: milestones re-pointed at 0030, 0031 and 0032.
- `CONTRIBUTING.md`: test naming by feature, and the link to
  `development-history.md` removed.
- `AGENTS.md`, in one set of edits:
  - RFC 0030 is the current model, read after RFC 0001; there is no
    checked mode;
  - rule 6 no longer mentions `development-history.md`;
  - rule 8: new tests are named by feature (`test/cases/<area>/…`,
    `test/Emission/<feature>-*.c`), and existing `rfcNNNN-` names may stay;
  - the "Where things are" table: `LibrarySpec.txt` replaces
    `Builtins.cpp`; new checker rules go behind the seam (`LedgerAdapter`,
    `SafetyEngine`); the sidecar row points to `UnitRecord` (format 28);
    `run-cases.py` and `corpus-gate.py` replace `corpus.py`; the row that
    points at `scripts/corpus/README.md` points at `test/corpus/`.

**The docs site.** The Starlight build (`docs.yml` runs `npm test` and
`npm run build`) reads several files this RFC deletes, and H2's grep covers
its pages:

- `docs/astro.config.mjs`: the sidebar entries *Checked code*, *Checked
  builds*, *Checked annotations*, *Checked limits*, *Cache & performance*
  and *Validation records* are removed; the site description changes.
- `docs/scripts/prepare-content.mjs`: the `checked-code.md` split
  (including *Checked selection (RFC 0018)*), the `incremental-analysis`,
  `development-history` and `validation-rfc*` routes are removed; the
  "Checked guarantees" link text becomes "Guarantees", and the checked
  sentence in the diagnostics index goes.
- `docs/scripts/content.test.mjs`: its route expectations follow.
- `docs/data/diagnostic-remedies.json`: the four removed ids go and the
  five added ids get remedies, since the build fails on an id without one.
- `docs/scripts/check-examples.py` and `docs/examples/checked.c`: the
  checked example is replaced by a trap-and-ledger example.
- Pages: `docs/src/components/Home.astro`, `docs/pages/index.mdx`,
  `docs/public/social-card.svg` (tagline),
  `getting-started/{introduction,first-check}.md`,
  `guides/{adoption,build-integration,whole-program,unsafe,ownership}.md`
  and `reference/{troubleshooting,compatibility}.md` drop checked mode,
  `--strict-externs`, caching and "suppresses ordinary reporting", and
  describe the checks, the ledger and the require levels.

**Release tooling.** `scripts/package-source.py` drops
`docs/development-history.md` from its required-file list, and
`scripts/test-release.py` drops its fixtures and assertions for it. Both
land in S0, with the deletion, and `scripts/test-release.py` passes in the
S0 gate.

**CI.** In S8:

- `.github/workflows/ci.yml`: the *Recall* and *Pinned corpus* steps
  (`recall.py`, `corpus.py` with `scripts/corpus/rfc0014.json`) are
  replaced by the `cases-*` CTest entries and `corpus-gate.py --quick`;
  CTest runs with `-j`; the population entries are gone. The pinned-corpus
  step is rewired in S0, in the same change that deletes `rfc0014.json`.
- `.github/workflows/corpus.yml`: runs `corpus-gate.py --full` on the
  pinned SHAs instead of `corpus.py` with `baseline.json`.
- `check-hygiene.py` runs in the Linux Release job.

### 20. Performance expectations

The limits are gates G14, G15 and H1; this section gives only the reasoning.

- **Compile.** Deleting the checked hooks, `InputIdentity`'s second
  preprocessing, and checked sidecar encoding outweighs the new passes.
  `AttributeReader`, `KindInference` and `SiteCollector` are linear AST and
  CFG walks; slot solving is near-linear; Houdini re-runs only functions
  that touch a dropped candidate's fields. The deferred CodeGen only
  reorders work. The new cost is the generic reporting pass for functions
  that today report only through call contexts (§15 item 18), which the
  shared context budget of §5.5 offsets.
- **Runtime.** Null checks cost 1.02x on Lua (measured with
  `-fsanitize=null`, which checks every dereference). Checks against named
  extents behave like `-fsanitize=array-bounds` (0.99x measured), not like
  `local-bounds` (1.44x), and the optimiser removes the ones a loop bound
  already implies. The zero-init cost is proportional to the slack between
  requested and usable sizes, since `calloc` supplies the rest.
- **CI.** Removing the 243 populations and running CTest in parallel is
  what brings the CTest steps under H1.

## Annotation surface

`resources/include/weavec.h` changes, mirrored in
`include/weavec/Analysis/Annotations.h` (`spelling::`) and
`docs/annotations.md`. `WEAVEC_H_VERSION_MINOR` becomes 9.

Added. Like every WeaveC macro, each expands to
`__attribute__((annotate(...)))` where `annotate` exists, and to nothing
elsewhere (including GCC):

```c
/* On a pointer parameter or field: at least `n` elements (bytes for void
 * and character pointees) are accessible; `n` names a sibling parameter or
 * field, in any position. */
#define WEAVEC_COUNTED_BY(n) WEAVEC_ANNOTATE_("weavec.counted_by." #n)
/* On a pointer parameter or field: [p, q) lies in one object; `q` names a
 * sibling pointer parameter or field. */
#define WEAVEC_ENDED_BY(q)   WEAVEC_ANNOTATE_("weavec.ended_by." #q)
/* On a pointer parameter, field or return type: NUL-terminated within its object. */
#define WEAVEC_STRING        WEAVEC_ANNOTATE_("weavec.string")
/* Before a function definition: its sites are held to -fweavec-require=checked. */
#define WEAVEC_REQUIRE_SAFE  WEAVEC_ANNOTATE_("weavec.require_safe")
```

These declare kinds (§7.2). They are authoritative for callers, whose call
sites check them. Inside the definition they hold under A1, and the link
step verifies them against every declaration.

Removed: `WEAVEC_CHECKED`. Code that uses it no longer compiles; replace it
with `WEAVEC_REQUIRE_SAFE`.

Changed meaning:

- `WEAVEC_UNSAFE`: trusted raw spatial and null operations, and trusted
  raw pointers. No checks are emitted, temporal state is still tracked,
  possible temporal findings are still warnings, and definite violations
  remain errors (§6.1). Nothing inside a region is suppressed any more.
- `WEAVEC_ASSUME(e)`: proven, a checked assertion, or a
  `contradicted-assumption` error (§6.2). The macro's definition is
  unchanged and the check is inserted by `weavec-cc`; its doc comment,
  which says it is "trusted like every annotation", is rewritten.
- `WEAVEC_NONNULL` on a parameter: a possibly-null argument is now checked
  at the call instead of reported. A definitely-null argument is still an
  error.
- `WEAVEC_SIZED_BY(n)`: unchanged in spelling and in units (elements;
  bytes for `void *`), which makes it a synonym of `WEAVEC_COUNTED_BY`,
  and unlike Clang's byte-counting `sized_by`. It now also produces
  checks: at call sites, and at accesses through the annotated pointer
  when not proven. Its doc comment says so and recommends
  `WEAVEC_COUNTED_BY` for new code.

Unchanged: `WEAVEC_OWNED`, `WEAVEC_BORROWED`, `WEAVEC_MUT`, `WEAVEC_RAW`,
`WEAVEC_NULLABLE`, `WEAVEC_RETAINS`, `WEAVEC_RELEASES`, `WEAVEC_REFCOUNT`,
`WEAVEC_OWNED_BY` and `WEAVEC_ENABLED`.

The ecosystem attributes in the §7.2 table are read, but WeaveC defines no
macros for them.

## Diagnostics

After this RFC, `weavec::core::diag` has 20 ids: the 15 kept below and the
5 added, which are exactly the ids the owner's resolutions list. "Severity" means: an
error when definite, a warning when possible, unless the table says
otherwise. `diag::isWarningByDefault` is replaced by
`diag::defaultSeverity(id, certainty)`, and `diag::isEnabledByDefault(id)`
is false only for `allocation-failure`.

**Kept ids.**

| Id | Severity | Messages (changes in bold) |
| --- | --- | --- |
| `use-after-free` | definite error / possible warning | `use of '<p>' after it was freed`; **`use of '<p>' after it may have been freed`** (note **`freed here on some paths`**); the reference forms `… after its reference was released` / **`… may have been released`** |
| `double-free` | definite error / possible warning | `'<p>' is freed twice`; **`'<p>' may be freed twice`** (note **`previously freed here on some paths`**); `… released twice` forms likewise |
| `use-after-move` | definite error / possible warning | `use of '<p>' after it was moved`; **`use of '<p>' after it may have been moved`** |
| `conflicting-borrow` | definite error / possible warning (§3.1) | unchanged messages |
| `lifetime-too-short` | definite error / possible warning (§3.4) | unchanged; also for returning `alloca` storage |
| `unsafe-operation` | error | unchanged (RFC 0004): an access through a pointer with a raw origin (declared `WEAVEC_RAW`, converted from an integer, loaded through a raw pointer, handed out as raw) outside an unsafe region. The `--strict-externs` forms (`unchecked call to '<f>' outside an unsafe region`, `unchecked call through '<fp>' …`) are removed with the flag |
| `annotation-mismatch` | error | unchanged; it now covers declared *and relied-upon* interface facts. **New at link**: `'<f>' is declared <annotation> here but its definition in '<file>' <frees/stores/…> '<param>'` (note `defined here`); **`store to '<S>.<field>' breaks '<invariant>', which '<unit>' relies on`** |
| `invalid-annotation` | warning | unchanged; **new**: `'<n>' in WEAVEC_COUNTED_BY does not name a parameter or field`, `conflicting kinds for '<p>': <k1> and <k2>` |
| `leak` | warning, never error | unchanged; **not reported at a return from `main` or after an `exits` call** |
| `mismatched-release` | definite error / possible warning | unchanged |
| `null-dereference` | **error, definite only** | `dereference of '<p>', which is null`; `'<p>', which is null, is passed to '<f>', which dereferences it`. **The "may be null" forms are removed: they are checked facets now** |
| `use-of-uninitialized` | **error, definite only** | `use of '<p>' before it was initialized`. Possible uses are made defined by zero-initialisation |
| `invalid-release` | definite error / possible warning | unchanged; possible: **`'<p>' is released but may point to <x>, which is not a heap object`**, and for an interior pointer at an unknown offset **`'<p>' is released but may not point to the start of its allocation`** |
| `out-of-bounds` | **error, definite only** (an exact extent, §7.1) | existing definite forms; **new**: `'<f>' writes <n> bytes into '<b>', an object of <m> bytes`; `write through '<p>', which points to a string literal`; `format string of '<f>' reads <n> arguments but <m> are passed`; `'<a>' has <n> elements but '<f>' accesses <m> through parameter '<p>'` (a static-callee requirement at the call); `'<f>' copies <n> bytes between overlapping ranges of '<obj>'` (note `'<obj>' is declared here`). **The "may" forms are removed: they are checked facets now** |
| `invalid-integer-operation` | error | unchanged (RFC 0017) |

**Added ids.** Each gets a unit test, a lit test pinning the message, and a
row in `docs/annotations.md`.

| Id | Severity | Message and notes | Minimal trigger |
| --- | --- | --- | --- |
| `contradicted-assumption` | error | `assumption '<e>' is false here`; note `'<x>' is <value> here` at the fact's source | `char b[4] = {0}; int i = 10; WEAVEC_ASSUME(i < 4); return b[i];` |
| `allocation-failure` | warning; **off by default** (`-Wweavec-allocation-failure`) | `the result of '<f>' is used without a null test; it is null when allocation fails`; note `allocated here` | `char *p = malloc(8); p[0] = 1;` |
| `unresolved-operation` | error; only under `-fweavec-require=checked\|proven` or `WEAVEC_REQUIRE_SAFE` | `<operation> is neither proven nor checkable: <reason phrase> [<reason>]`, for example `access 'b[1000]' is neither proven nor checkable: the extent of 'b' is unknown [unknown-extent]` | `char *get(void); int f(void) { return get()[1000]; }` with `-fweavec-require=checked` |
| `unchecked-operation` | error; only under `-fweavec-require=proven` | `<operation> relies on a runtime <template> check`; note naming what the proof lacked (`nothing is known about the nullness of 'p'`) | `int f(int *p) { return *p; }` with `-fweavec-require=proven` |
| `unanalyzed-input` | warning, at link, once per link | `link input '<path>' has no WeaveC record; calls into it are trusted`, or `link input '<path>' has a stale WeaveC record (<why>); calls into it are trusted`; with several inputs, `<n> link inputs have no WeaveC record; calls into them are trusted` with one note per input | `weavec-cc -c a.c; clang -c b.c; weavec-cc a.o b.o` |

A definite overlap of a copy's ranges is reported as `out-of-bounds`
(§3.3) rather than under an id of its own, so the added ids are exactly
the five the owner's resolutions list.

`<operation>` is one of `access '<text>'`, `dereference of '<p>'`,
`call to '<f>'`, `release of '<p>'`, `conversion of '<p>' to '<T>'` or
`boundary of '<fn>'`. The reason phrases are:

| Reason | Phrase |
| --- | --- |
| `unknown-extent` | `the extent of '<p>' is unknown` |
| `unknown-index` | `the position of '<p>' in its object is unknown` |
| `inexpressible` | `its bound has no name here` |
| `may-released` | `'<p>' may have been freed` |
| `may-moved` | `'<p>' may have been moved` |
| `may-alias-released` | `'<p>' may point into an object freed earlier` |
| `may-invalid-release` | `'<p>' may not point to the start of a heap object` |
| `may-mismatched-release` | `'<p>' may belong to another allocator` |
| `may-dangle` | `'<p>' may outlive its storage` |
| `may-conflict` | `'<p>' may still be borrowed` |
| `unknown-callee` | `'<f>' may have freed or kept '<p>'` |
| `callback` | `the target of '<slot>' is unknown` |
| `setjmp` | `'<fn>' calls setjmp` |
| `budget` | `'<fn>' exceeded the analysis budget` |
| `unanalysed` | `WeaveC does not model this (<detail>)` |
| `raw-cast` | `'<p>' was made from a non-pointer value` |
| `dangling-escape` | `'<place>' may hold a freed pointer here` |
| `second-owner` | `'<a>' and '<b>' may own the same object here` |
| `no-zero-init` | `'<p>' may be uninitialised` |

**Removed ids:**

- `analysis-incomplete` becomes `unresolved(unanalysed | budget | …)`
  ledger rows plus the summary line;
- `annotation-required` becomes `unresolved(unknown-callee)` rows with
  fix-its;
- `checking-incomplete` and `checking-failed` go with checked mode.

A `-W` flag naming a removed id is an error,
`unknown WeaveC diagnostic '<id>' (removed by RFC 0030)`, like any other
unknown id. No compatibility aliases are kept.

`dangling-escape` and `second-owner` are ledger *reasons*, not ids. They
surface as `unresolved-operation` errors only under require levels.

## Implementation plan

All stages land on branch `rfc0030-prove-or-trap` and ship as one commit,
as the owner directed. Each stage ends with its gate green in a local run.
The PR description records each result: the command, the binary and the
numbers. Two fallback points and one abort rule bound the risk.

| Stage | Work | Gate to leave the stage |
| --- | --- | --- |
| **S0 Oracles and reset** | This RFC. The golden v0.10.0 binaries (§17.1). `test/cases` with every import, the `main` drivers of §17.2 and the `proofs/` salvage. `scripts/run-cases.py` including `--legacy`. `test/corpus/` (manifest with pinned SHAs, expected, triage, about 30 injections, bench, support files) and `scripts/corpus-gate.py`. Deletion of the generated `scripts/corpus/rfc00NN-*` data, the validation records, `development-history.md` and the README changelog prose, with the release-tooling and pinned-corpus CI changes of §19. | `run-cases.py --legacy` with the golden binaries reproduces v0.10.0: evaluation 44/44 bugs and 32/32 clean; RFC 0017 cases 24/24; recall 67/67; probes 36 caught / 42 silent / 4 signal / 1 leak-only / 2 mislabel; engine pins 100%. `corpus-gate.py --quick --legacy` reproduces the 301 bug claims. `--inject --legacy` records the injection baseline, matching the dossier where it measured (jansson 3/3, cJSON 1/1, sds 2/2, linenoise 1/1, Lua 0/2). `scripts/test-release.py` passes. |
| **S1 Retire checked mode by reachability** | Remove the checked-mode and analysis-cache options; then the guarded branches (§15 item 1); then everything unreferenced (§18); then the populations, runners, reports, cache, `InputIdentity`, and the checked unit and lit tests. Make CTest parallel. `--strict-externs`, `--exclusive-borrows`, `--report-unannotated` and `--analyze-headers` stay until S3, so the output can stay identical. | Sorted ordinary diagnostics (file, line, column, id, message) are byte-identical to the golden run on all of `test/cases` and on the 11 configs (`--compare-golden` in both scripts). Unit and lit tests pass. |
| **S2 Deferred CodeGen** | `DeferredCodeGenConsumer` (§10.5), with no rewrites. `scripts/codegen-identity.py` and `test/corpus/identity.txt`. | G7. |
| **S3 Ledger and semantics** | Core `Ledger`, `AttributeReader` and the syntactic kinds that `SiteCollector` needs, `SiteCollector`, the seam (§14) with `beginFunction` and the authoritative pass (§2.6, §15 item 18), `DataflowEngine`, certainty and the temporal rules (§3, §15 items 16–17), the object extents (§7.4, §15 item 15), sound defaults (§5.1; §5.2 with the header list, which is the first part of `LibrarySpec.txt` to land; §5.4–5.7), unsafe/assume/require (§6, errors only), `CheckPlanner::plan` (pure planning, no emission), budgets, `LedgerWriter` (§12), the `-p` and SARIF fixes, removal of `analysis-incomplete`, `annotation-required` and the four flags S1 kept, and the lit and unit test updates of §17.6. | Under `run-cases.py --no-emission`: G1; G2 (every pin at any severity); G3. G4 for the probes whose mechanism exists by S3, that is all but 02, 02c, 02d (S7), 09, 14c (S6), 15, 15b, 25, 25c, 47 (S4), 38 (S8), and 08, 32 (S5, counted as `MISS`): 0 silent, 0 matching facet proven, and 45 and c03 are errors. zlib `./configure && make` succeeds with no definite error triaged false. H3 for the ids added so far. |
| | **Fallback point A.** S0–S3 ship as "one semantics, visible coverage": checks are not emitted, and the summary line says `checkable (not enforced)`. The probes excluded from S3's G4 subset are not yet covered, and the release notes say so. | |
| **S4 Library table** | `LibrarySpec` (§8) replaces `Builtins`, `RuntimeModels` and the `name ==` tests, with aliases and fortified forms; model fixes; callbacks, threads and signals (§5.3); `allocation-failure`; the leak rule; the §9.2 non-null derivation. | A unit test per row, from the independent expectation table. The ledger diff S3→S4 on `test/cases` and the corpus, with every changed row explained in the PR description. G4 for probes 15, 15b, 25, 25c and 47 under `--no-emission`. Repros `pred`, `zerolen` and `mainleak` meet their exact expectations. |
| | **Fallback point B.** S0–S4 are a coherent release if check insertion fails: one semantics, recorded outcomes, the library table, and the test reset. | |
| **S5 Emission** | `CheckEmitter` over the plan for type-derived, allocation and declared extents; the prelude and its forms; the four modes, `weavec_rt` and `weavec_chk`; lowered-violation traps (§3.4); zero-initialisation (§11); the PCH fallback. | G1 and G2 in their trap form; G4 in full except probes 02, 02c, 02d (S7), 09, 14c (S6) and 38 (S8); G5; G6; G8; G11; G14. |
| | **Abort rule.** If false traps (G5, G6, G11) or overhead (G14) cannot be fixed within S5, emission is reverted. The release is then S0–S4 plus S6's declared kinds, in the ledger only. §10–§11 move to RFC 0031, and this RFC is amended to record the cut; it does not become Implemented until that amendment. | |
| **S6 Kinds** | `AttributeReader`'s full table and the macros (§7.2); the engine's consumption of kinds (§15 item 14); slot kinds with reliance flags and demotions (§7.3); must-access inference with guarded call-site checks (§7.5); grouping (§7.4). Houdini (§7.6) comes last and is the first thing cut. | A lit test per row of the §7.2 table; G13; G4 for probes 09 and 14c; G5, G11 and G14 still pass. |
| **S7 Temporal precision** | Case derivation with `lossy` bits (§9.1); slots per TU and in `--whole-program` (§9.3), replacing the RFC 0014 callback-global fixpoint; boundary invariants and their propagation (§9.4). | G9, G10, G14 and G15; G12 measured with `weavec --whole-program` (the `weavec-cc` link half waits for S8); G4 for probes 02, 02c and 02d. Repros `realloc0`–`realloc5` and `luaalloc` meet their exact expectations. |
| **S8 Link and wrap-up** | Format 28 (§13.1); the link step (§13.2), including declaration verification, reliance checks, program-wide propagation and `unanalyzed-input`; CLI cleanup (§16); documentation, the docs site and RFC statuses (§19); CI wiring: parallel CTest, the `cases-*` tests, `corpus-gate --quick` on PRs, `--full` weekly, and `check-hygiene`. | Every gate below re-run on the final tree, including probe 38 at link, G12 through `weavec-cc` linking the objects, H1 on the PR's own CI run, and H2. `npm test && npm run build` in `docs/` pass. |

Only Houdini field invariants (§7.6) can be cut without breaking the
design; G13 is sized to pass without them. The report-mode runtime cannot
be cut, because the case runner and G11 attribute traps through it
(§17.4). The slot analysis cannot be cut either, because G12 is a hard
gate. Unfinished inference always degrades to *unresolved*, never to
*proven*, so a cut is sound by construction.

## Acceptance gates

Unless stated otherwise:

- the binaries are `weavec-cc` and `weavec` from the `release` preset
  (`build/release/bin/`) on the final tree;
- the reference compiler is `$WEAVEC_LLVM_PREFIX/bin/clang` (LLVM 23);
- "reported at a line" means a diagnostic with the expected id at that
  line (an error or warning, per its marker), or a trap at that line with
  the expected template in the executable oracle.

Gates marked *(changed)* differ from the milestone plan; the reason is
given.

**Parity and recall**

- **G1.** Measured by `scripts/run-cases.py --filter 'evaluation/**' --filter 'pairs/**'`
  in trap mode.
  - The 44 evaluation bug cases have every `BUG` marker satisfied.
  - The 32 clean cases have no errors and no traps. Warnings are allowed
    only through an `ALLOW` line, and each `ALLOW` is justified in a
    comment in the case file.
  - All 24 RFC 0017 cases (12 bug/clean pairs) pass.
- **G2.** Measured by `run-cases.py --filter 'recall/**'`.
  - Every recall pin is reported (67 as `recall.py` counts them).
  - Across evaluation and recall, the runner's `errorsOrTraps / pins` is
    at least 0.75.
- **G3** *(changed)*. Measured by `run-cases.py --filter 'engine/**'`.
  - At least 95% of the lit engine pins converted from the golden run are
    reproduced at any severity.
  - A pin whose golden message said a pointer "may be null" or an access
    "may be" out of bounds counts as reproduced when the matching facet at
    that line is *checked*. This policy deliberately turns such findings
    into checks.
  - Every miss is listed in `test/cases/KNOWN-DIFFERENCES.md`, with at most
    25 entries.
- **G4** *(changed)*. Measured by `run-cases.py --filter 'soundness/**' --asan`.
  - 0 of 85 bug probes are silent. "Non-silent" means error, warning, trap,
    a non-proven row of the matching facet (§17.3), or `NEUTRALISED` for
    probes 08 and 32. Those two defects are uninitialised scalars, outside
    every facet; zero-initialisation defines them away.
  - At least 55 of 85 are reported as an error, warning or trap.
  - Every probe v0.10.0 reports (the 36 caught, plus 26 and 41) is still
    reported, at some severity.
  - 0 ASan-reported bug sites have the matching facet *proven*. For
    probes 02, 02c and 02d the bug site is inside the reader (`peek`), and
    it is non-proven through the boundary propagation of §9.4.
  - Probes 45, c03 and 38 produce errors. Probe 38 is built with
    `38_extern_impl.c` and caught at link.
  - With `--filter 'semantics/**' --asan`, the review cases of §17.2 pass:
    the aliasing, reliance, propagation and concurrency cases have no
    proven matching facet at their ASan-reported bug lines.
- **G5.** Measured on the 28 correct twins.
  - All 28 build with no errors and run their `RUN-INPUT`s with no trap.
  - With `FLAGS: -fweavec-require=checked` added by the runner's
    `--require checked`, at least 20 of 28 build with no errors (11 today
    in checked mode).
- **G6** *(changed)*. Measured by `run-cases.py --checks verify` over every
  executable case, and `corpus-gate.py --full --checks verify` over every
  project test suite.
  - 0 `weavec.proven` traps.
  - Every `proofs/` case satisfies its `NOT-PROVEN` markers.
  - Every proven Index facet whose extent is exact and expressible receives
    a verify check, and the ledger reports the verify coverage of all
    proven spatial and null facets. The change: without a coverage
    requirement, verify mode could monitor only null proofs, while the
    known false proof (the reader cursor) was spatial.

**Codegen**

- **G7.** Measured by `scripts/codegen-identity.py --list test/corpus/identity.txt`
  (at least 100 corpus TUs).
  - For each TU, in each of the configurations `-O2`, `-O0 -g` and
    `-O2 -flto=thin`, the object from `weavec-cc -fweavec-checks=none -c`
    is byte-identical (`cmp`) to the one from
    `clang -c -D__WEAVEC__=1 -isystem resources/include`.
  - The gate is at least 100 TUs × 3 configurations.
- **G8.** Measured by `lit test/Emission`.
  - At least 30 `rewrite-oracle-*.c` pairs.
  - In each pair, the `-O0` IR of the instrumented source equals, after
    `--strip-debug` normalisation, the IR clang produces for the paired
    hand-written `*.expected.c`, which calls the same helpers explicitly
    and is compiled with `-include` of the prelude that
    `weavec-cc -fweavec-print-prelude` prints.
  - The pairs include the zero-length `nonnull` form, the `span` index
    form, the `sprintf` lowering, a lowered violation, and the
    `memcpy(NULL, x, 0)` fold case of §8.3.

**Real code.** All measured by `scripts/corpus-gate.py --full`, with
`weavec-cc` in its default mode (trap, zero-init) on the pinned SHAs of
`test/corpus/manifest.json`.

- **G9** *(changed)*.
  - All 11 configs build, including zlib's own `./configure && make`.
  - At most 10 definite errors in total, each with a `triage.json` entry
    whose verdict is `true`. A `false` definite error fails the gate.
  - A `build` config reaches a successful build despite a triaged-true
    definite error only through the per-config `-Wno-error=weavec-<id>`
    recorded next to its triage entry (§17.5); in per-file `compile`
    configs the error is recorded against its TU. The change: a definite
    error drops its object, so "all build" and "up to 10 true definite
    errors" contradicted each other without this rule.
  - zlib `minigzip`'s repeated `fclose(stdout)` is still reported.
- **G10.** Possible temporal warnings number at most 60 in total, all
  triaged. jansson has at most 20 (84 today, as errors).
- **G11.** 0 traps in the project test suites: cJSON's tests, jansson's
  test suite, zlib's `make test` (`example` and `minigzip`), the Lua
  `testes` subset named in the manifest, sds's `sds-test` and jsmn's
  tests. A trap is death by `SIGTRAP` or `SIGILL`, or a
  `runtime check failed` line in the report-mode rerun. A trap at the site
  of a triaged-true definite error (G9) is a true positive and does not
  count.
- **G12** *(changed)*. Measured by `corpus-gate.py --full --inject`.
  - At least 90% of the injections are reported at the injected line.
  - Both Lua allocator injections are reported in whole-program mode: by
    `weavec --whole-program` on the lua config, and by `weavec-cc` linking
    the objects directly. The change: "caught" is made precise as
    "reported at the injected line".
- **G13.** Measured by `corpus-gate.py --quick`. The unresolved share is
  unresolved spatial and null facets divided by all spatial and null
  facets.
  - linenoise.c at most 0.25 and cJSON.c at most 0.25 (target 0.15);
    sds.c at most 0.60. Today these are 0.86, 0.80 and 0.99, all silent.
  - The values are committed in `expected.json` as a ratchet.

**Cost**

- **G14.** Measured by `corpus-gate.py --bench`: the minimum user CPU time
  of 7 runs of the `weavec-cc` default build, divided by the same for a
  `clang -O2` build, on the same machine. The limits are Lua bench
  (`bench/lua-bench.lua`) ≤ 1.10, zlib `minigzip` compress+decompress of
  the generated 64 MiB input ≤ 1.10, and cJSON parse+print
  (`bench/cjson-bench.c`) ≤ 1.15.
- **G15** *(changed)*.
  - Lua whole-program analysis (`weavec --whole-program` on the lua
    config) takes at most 214 s CPU.
  - zlib's `make -j8` with `CC=weavec-cc` takes at most 5.4 s wall (the
    golden binary measured 5.38 s; serial, 6.59 s).
  - On a machine other than the reference one, the limit is instead the
    golden binary's time measured in the same run. This is the change: a
    fixed time is only comparable on one machine.
  - Over-budget functions are at most 1% of analysed functions, and each
    is listed.

**Hygiene**

- **H1** *(changed)*. Measured at S8 on the PR's own CI run.
  - In the Linux Release job, `ctest --preset ci-release -j$(nproc)` runs
    in at most 120 s wall.
  - In the Linux ASan/UBSan job (`ci-debug`), the CTest step runs in at
    most 8 minutes, with the cases suites in `--no-run` mode.
  - The change: the milestone plan capped the whole ASan job at 20
    minutes, but installing LLVM and building under ASan already take
    about 22 minutes of today's 41 (CTest is 19), so no test change can
    meet it. The CTest step is the part this RFC controls.
- **H2** *(changed)*. Measured by `scripts/check-hygiene.py`:
  - 0 occurrences of `SafetyState`, `CheckedContract`, `checkContracts` or
    `--checked` in `lib/`, `tools/` and `docs/` outside `docs/rfcs/`. The
    change: superseded RFCs are the historical record and keep their
    text.
  - 0 `== "<name>"` comparisons against a `LibrarySpec` entry name outside
    `lib/Core/LibrarySpec*`.
  - 0 occurrences of the words `cJSON`, `jansson`, `linenoise`, `jsmn`,
    `zlib`, `lua`/`Lua`, `sds` or `minigzip` anywhere in `lib/`, comments
    included.
  - No new component (§1) includes `Dataflow.h`, and `FunctionDataflow`
    receives no `DiagnosticSink`.
  - `Dataflow*.{h,cpp}` total at most 25,000 lines.
  - All files under `lib/`, `include/` and `tools/` total at most 62,000
    lines.
- **H3** *(changed)*.
  - `test/Driver/compilation-database-p.c` and
    `test/Driver/diagnostics-format-sarif.c` exist and pass.
  - Each added id (`contradicted-assumption`, `allocation-failure`,
    `unresolved-operation`, `unchecked-operation`, `unanalyzed-input`) has
    a `docs/annotations.md` row, a `docs/data/diagnostic-remedies.json`
    entry, a unit test and a lit test pinning its message.
  - The change: `dangling-escape` and `second-owner` are reasons, not ids,
    per the owner's resolution, and the generic `unresolved` id is named
    `unresolved-operation`.

## Drawbacks

- **Binaries can now abort.** A correct program that relied on undefined
  behaviour that happened to work (§Soundness) will trap in production.
  `-fweavec-checks=report` supports canary rollouts, without a guarantee
  while it lets failed checks continue, and `-fweavec-checks=none` restores
  Clang's exact output. Both are opt-outs a team must know about.
- **Proven outcomes are only as sound as `FunctionDataflow`.** That engine
  has had false proofs (the reader-cursor case). A wrong proof now removes
  a check instead of hiding a warning. Mitigations: verify mode in CI (G6),
  the ASan oracle (G4), the salvaged `proofs/` cases, and sound defaults
  landing before emission (S3 before S5).
- **The temporal guarantee is thinner than the spatial one.** Temporal
  facets are proven or listed, never enforced. On complex code such as Lua,
  the unresolved temporal count will be large until RFC 0031. This RFC
  commits only to "never silent" for temporal safety.
- **Possible temporal bugs no longer fail the build.** This is the price
  of removing about 92% false errors. `-fweavec-require=checked` restores
  fail-closed behaviour per unit or per function.
- **Per-TU compiles see more `unknown-callee` rows.** Calls into sibling
  units are unknown until the link step, which refines them. Archives
  still hide their members until RFC 0032, though the link step now says
  so.
- **Zero-initialisation has a cost.** It costs `calloc` instead of
  `malloc`, plus time proportional to the slack between requested and
  usable sizes, and it changes two observable behaviours: comparing a
  function pointer with `malloc`, and `realloc(p, 0)`, which returns a
  one-byte block instead of freeing.
- **Mutating the AST in a release-mode Clang is delicate.** Brew's LLVM
  23 has assertions off, so a malformed rewrite could miscompile silently.
  Mitigations: only six templates, all built by Sema's own builders;
  a rejected rewrite restores the subtree and fails the compile rather
  than emitting a partial one; the rewrite oracle (G8); strict warning
  builds of instrumented code; the identity gate (G7).
- **The commit is large.** About 32K authored lines and whole-file
  deletions of about 380K, most of them generated. The stage gates and the
  two fallback points exist because of this.
- **Checked mode's 150 complete contracts are lost as a notion.** Their
  functions' spatial and null operations become proven or checked, and
  `-fweavec-require=proven` gives the same fail-closed stance, but the
  contract reports are gone.

## Alternatives

- **Replace the engine first** (a symbolic-heap "Weave Engine" with two
  reporting policies). This is the best end-state architecture (library
  code of about 36K lines), but parity has to be rebuilt from zero across
  research-grade pieces (list segments, abduction, witness rules, Houdini).
  It is 3.5 times the largest library change the project has landed, and
  even a successful rewrite certifies nothing about unproven code. It
  becomes RFC 0031, rescoped to replacing the `SafetyEngine` behind §14's
  seam and sized by the unresolved-reason histogram.
- **A manifest-first bug finder** (Pulse-style, under-approximate). It
  gives the most trustworthy findings, but it gives up the README's
  "proves" promise, keeps `WEAVEC_UNSAFE` as a suppressor, and would need
  a second engine for any later proof. Its slot analysis, repros,
  injection harness, triage gate and project-name grep are grafted here.
- **Adoption first** (object-embedded records, baselines, `weavec.toml`,
  SARIF). It polishes an analyzer whose reports are about 92% false, and
  baselines keyed to today's messages would churn. Its certainty
  mechanism, fingerprints, attribute precedence, `-p` fix and
  missing-metadata warning are grafted here; embedding goes to RFC 0032.
- **Consolidate in place** (symbolic values and a zone domain inside
  `FunctionDataflow`, keeping proof mode). It migrates about 600 sites
  inside the largest class and keeps proof-cost gates that repeat RFC
  0029's blocker. Its contract/evidence split and false-proof salvage are
  grafted here.
- **Keep checked mode behind a flag.** That keeps about 18K lines, the
  243 populations and 82% of CI time for a mode that proves 9% of the
  corpus and cannot run on Lua. `-fweavec-require=proven` covers its
  fail-closed use.
- **Use Clang's sanitizers or `-fbounds-safety` for the checks.** Neither
  can carry WeaveC's inferred extents. `counted_by` on parameters is
  rejected even with `-fexperimental-bounds-safety`, sanitizer pruning is
  per function, and `local-bounds` costs 1.44x on Lua. Emitting WeaveC's
  own six templates keeps the cost array-bounds-like.
- **Instrument at the IR level.** Keying an IR pass by source location
  would need debug locations in every build and IR-level reconstruction of
  C values. Sema rewrites keep C semantics and type-check the result.
- **Fat pointers or another ABI change** (CCured SEQ/WILD, SoftBound, CHERI,
  Fil-C). These would enforce much more, including temporal safety in some
  designs, but they break the drop-in compiler and linking with
  uninstrumented code, which is the README's first promise.
- **Do nothing.** WeaveC stays noisy (zlib does not build), silently
  unsound (42 of 85 probes) and slow in its only fail-closed mode.

## Prior art

- **CCured** (Necula, Condit et al., TOPLAS 2005). It infers pointer kinds
  (SAFE, SEQ, WILD), proves what it can and inserts runtime checks for
  the rest. The Single/Counted/Unknown split and "checks for what cannot
  be proven" come from there. CCured's fat SEQ/WILD pointers changed the
  ABI; WeaveC takes only the kinds and names extents that already exist in
  the program.
- **Checked C and 3C.** Its checked pointer types (`_Ptr`, `_Array_ptr`
  with bounds, `_Nt_array_ptr`), dynamic bounds checks and the rule that
  allocations holding checked pointers start valid-or-null inform
  §7 and §11. 3C's inference of kinds from uses parallels §7.3–7.5.
- **Clang `-fbounds-safety`.** Its lattice (`__single`, `__counted_by`,
  `__sized_by`, `__ended_by`, `__null_terminated`, `__indexable`), its
  checks at conversions from indexable to single, its grouping rule for
  pointer/count updates, and its reliance on the optimiser
  (ConstraintElimination) to remove redundant checks are adopted almost
  directly, including the one-past-the-end rule. WeaveC differs in
  inferring kinds for unannotated code and in not changing pointer
  representation.
- **Rust.** Ownership and borrowing are checked statically, and bounds are
  checked dynamically unless the optimiser removes them. This RFC gives C
  the same split: temporal safety is static; spatial and null safety are
  proven or checked.
- **Frama-C RTE and E-ACSL.** Frama-C generates a runtime-error assertion
  per operation, proves what it can and compiles the rest into runtime
  checks. The per-site ledger is the same idea, with outcomes recorded
  instead of proof obligations handed to a prover.
- **Infer Pulse** (Le et al., OOPSLA 2022). Only manifest bugs are
  reported, per-procedure budgets keep imprecision local, and disjuncts
  are bounded. This informs the definite-only error policy (§3) and the
  budgets (§5.5).
- **Houdini** (Flanagan and Leino, FME 2001). It is the fixpoint used for
  field invariants (§7.6).
- **Field-based function-pointer analysis** (Andersen-style inclusion
  constraints over fields). This is the basis of §9.3; field-based rather
  than object-based analysis trades precision for linear-size constraints
  that cross translation units.
- **SoftBound/CETS, Fil-C and CHERI.** These are runtime designs that
  enforce temporal safety too, at a cost in ABI or performance. RFC 0031's
  temporal backstop starts from them.
- **`-ftrivial-auto-var-init=zero`.** Deployed in Chrome, Windows and the
  Linux kernel, it shows that zero-initialisation is affordable and
  removes a bug class. WeaveC extends it to the malloc family (§11).
- **Clang's lifetime-safety analysis** (`-Wlifetime-safety`). It covers
  simple dangling cases in C upstream. WeaveC's niche is the
  interprocedural, ownership-aware remainder plus enforcement through a
  drop-in compiler.

## Unresolved questions

These can be settled during implementation without changing a decision
above:

- **The budget default.** S3 calibrates it by the §5.5 rule. The value is
  recorded in the CLI reference and in a one-line correction to this RFC.
- **Whether zero-initialisation meets G14 on allocation-heavy code.** If
  it does not, the allowed narrowing is to zero only allocations whose
  size expression is `sizeof` of a type containing pointers, plus
  `realloc` tails. Any narrowing must keep the §11 guarantee for pointer
  loads and must be written into this RFC.
- **Open slots with known targets.** Is `trusted(extern-contract)` the
  right outcome, or should it be `unresolved(callback)`? It is trusted
  here so that jansson's allocator hooks keep their precision. If G10
  passes either way, the stricter outcome is preferred.
- **The volume of `unresolved(may-alias-released)`.** The rule of §3.1 is
  deliberately coarse (type compatibility, owning places, place identity).
  If it floods the temporal counts on the corpus, the allowed remedy is a
  sharper distinctness proof, never a weaker rule.
- **The volume of `unresolved(unknown-callee)` in per-TU builds of
  multi-unit projects.** The summary line may need to separate "resolved
  at link" from "unresolved at link" to stay readable.
- **The SARIF size for large programs.** Lua has tens of thousands of
  rows. If consumers struggle, unresolved rows may move to a
  `-fweavec-ledger-sarif=diagnostics` mode; the JSON ledger stays
  complete.
- **The fingerprint root.** The nearest `.git`, else the working
  directory, may be wrong for monorepos. RFC 0032's `weavec.toml` is the
  intended fix, and the fingerprint's `weavec-fp/1` tag allows a v2.
- **User sanitizers.** Combining checks with user `-fsanitize=address`
  should work, because the checks are plain C. G4's oracle builds with
  `-fno-weavec` only to keep the two signals separate. This is to be
  confirmed by one lit test.
- **Real corpus numbers.** How many possible temporal warnings remain on
  Lua after slot resolution, and how many header-struct invariants are
  relied on, will only be known from the corpus. G10 and the A3 line
  report them.

## Future work

- **RFC 0031: residual enforcement and precision.** It is sized by the
  ledger's unresolved-reason histogram and covers:
  - a replacement engine behind `SafetyEngine` (§14): semantic IR,
    symbolic heap, relational domain;
  - a temporal runtime backstop (a quarantine allocator; Arm MTE and Apple
    MIE where the hardware allows);
  - an ABI-compatible heap-bounds lookup (a low-fat allocator) for the
    residual `unknown-extent` sites, such as the sds header idiom and
    `container_of`;
  - precision where the histogram shows it pays: state machines, guard
    functions, array cells;
  - proof-dependency tracking, for a sharper blame property;
  - if S5 is aborted, the check emission of §10–§11.
- **RFC 0032: adoption.** It covers:
  - format-28 records embedded in object sections (ELF `.weavec`, Mach-O
    `__DWARF,__weavec`), so archives, shared libraries, ccache and LTO
    carry them;
  - fingerprinted baselines and reasoned suppressions;
  - `weavec.toml` with path scoping and API overlays;
  - `weavec suggest --apply` for the ledger's fix-its;
  - a vendored `weavec.h`;
  - relocatable installs.
- **Later:**
  - concurrency safety beyond A4 (Send/Sync-like rules for shared
    globals);
  - C++ and Objective-C, which pass through to Clang unchanged today;
  - reading `lifetimebound`, `noescape` and `lifetime_capture_by`.
