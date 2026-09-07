# Roadmap

Rough ordering of the major pieces of work. This page is the one-screen view;
the design detail and the reasoning live in the [RFCs](rfcs/README.md) each
milestone links to. Everything here is subject to change as we learn.

## Milestone 0 — Scaffolding (done)

Model: [RFC 0001 — Ownership, borrowing and lifetimes](rfcs/0001-ownership-model.md)
(Accepted).

- [x] Layered library structure (`Core` / `Analysis` / `Frontend`), build,
      tests, CI, docs.
- [x] End-to-end pipeline with a minimal local ownership checker.
- [x] Annotation header and recognition.
- [x] RFC process and the model RFC.

## Milestone 1 — Sound intra-procedural checking (done)

Design: [RFC 0002 — Sound intra-procedural checking](rfcs/0002-intraprocedural-checking.md)
(Implemented). The items below summarise it; the RFC is authoritative.

- [x] Replace the AST walk with a forward dataflow over `clang::CFG`
      (fixpoint for loops, proper handling of `switch`, `goto`, short-circuit;
      diagnostics emitted in a post-fixpoint reporting pass).
- [x] Alias relation so copies of an owned pointer share one resource.
- [x] Model allocation/release functions: `malloc`, `calloc`, `realloc`,
      `strdup`, `free`, plus user-annotated allocators; `realloc` failure
      idiom accepted via null-edge reinstatement.
- [x] Drive `BorrowState` from address-of, array decay and annotated calls;
      emit `conflicting-borrow`.
- [x] Drive `LifetimeConstraints` from scopes; emit `lifetime-too-short` for
      escaping pointers to locals.
- [x] Field-sensitive places (`s->p`, `a[i]` as a summary place).
- [x] `--dump-analysis` for debugging inferred facts.

## Milestone 2 — Signature inference and annotations

Design: [RFC 0003 — Signature inference](rfcs/0003-signature-inference.md)
(Implemented) and [RFC 0004 — Unsafe boundaries](rfcs/0004-unsafe-boundaries.md)
(Implemented).

- [x] Per-function summaries (effects on parameters, paths and globals;
      stores into caller-visible memory; return-value provenance) computed
      bottom-up over the TU call graph, to a fixpoint inside recursive SCCs,
      and applied at every call site.
- [x] Honour `WEAVEC_OWNED` / `WEAVEC_BORROWED` / `WEAVEC_MUT` on
      declarations; check definitions against their own annotations
      (`annotation-mismatch`).
- [x] Shipped summaries for the C standard library (`Builtins.cpp`), so
      `strchr`, `strtol`, `fopen`/`fclose`, ... need no annotations.
- [x] `annotation-required` on by default at the external boundary (once per
      unknown callee); with `--report-unannotated`, exported functions get
      fix-its that insert the inferred annotation.
- [x] Corpus harness (`scripts/corpus.py`) with a tracked baseline.
- [x] `Raw` pointers and unsafe regions (RFC 0004): integer casts and
      `WEAVEC_RAW` yield raw pointers; `unsafe-operation` for raw operations
      outside `WEAVEC_UNSAFE`; unsafe regions are analysed with reports
      suppressed, so ownership flows through them; laundering by assertion.
- [x] Calls through function pointers (RFC 0004): annotations on the
      function-pointer type, else actual reaching function values (refined by
      RFC 0014); conservative indirect edges in the call graph.
- [x] Pointer arithmetic and pointer casts preserve identity (RFC 0004).
- [x] `--strict-externs` makes unchecked calls raw operations (RFC 0004).
- [x] POSIX / common GNU-BSD coverage in the shipped table (`<unistd.h>`,
      `<fcntl.h>`, `<dirent.h>`, `<sys/mman.h>`, `<netdb.h>`, `<pthread.h>`,
      `<time.h>`, `<pwd.h>`, `<regex.h>`, `<dlfcn.h>`, `asprintf`, `getline`,
      ...).
- [x] Follow-ups surfaced by the corpus: may-moves for `a[*]` element places
      (`free(a[i])` in a loop), pointer-equality guards (moved to RFC 0006,
      Milestone 5).
- [x] Function pointers stored in globals and returned from other TUs
      (RFC 0005 / RFC 0014: actual targets and contexts cross the program).

## Milestone 3 — Compiler driver (done)

Design: [RFC 0005 — Whole-program analysis](rfcs/0005-whole-program-analysis.md)
(Implemented), *`weavec-cc`*.

- [x] `weavec-cc` as a drop-in `cc`: Clang's driver plans the jobs, each
      `-cc1` runs in-process with WeaveC's consumer multiplexed beside
      Clang's code generation, and a WeaveC error fails the compile.
- [x] `-fweavec-strict` / `-fno-weavec` / `-fweavec-report-unannotated` /
      `-fweavec-dump-analysis` / `-fno-weavec-link`, and warning-group style
      control over diagnostic identifiers (`-Wno-weavec-annotation-required`,
      `-Wno-error=weavec-use-after-free`, `-Werror=weavec`).
- [ ] Clang plugin packaging so existing builds can add `-fplugin=weavec`
      and run `weavec --link-check` as the link step.

## Milestone 4 — Whole-program (in progress)

Design: [RFC 0005 — Whole-program analysis](rfcs/0005-whole-program-analysis.md)
(Implemented).

- [x] Cross-TU summaries: every unit exports the summaries of its external
      definitions and address-taken functions; a `ProgramDatabase` sits
      between a unit's own inference and the libc table in `SummaryStore`.
- [x] `FunctionSummary` text format (`Core/SummaryIO.h`), the on-disk and
      debugging form.
- [x] `weavec --whole-program` over a compilation database: units ordered
      by SCC, cyclic groups iterated to a fixpoint.
- [x] `weavec-cc` sidecars (`foo.o.weavec`) written at compile time and
      combined at link time; deferred `annotation-required` decided per
      program; compile-time reports not repeated at link time.
- [x] Whole-struct copies carry the facts of their pointer fields.
- [ ] Summaries for common libraries beyond libc shipped with WeaveC
      (`libz.weavec` next to `libz.a`, read like any sidecar).
- [ ] AST caching in the sidecar so the link step loads rather than parses.
- [ ] Incremental link steps (re-analyse only units whose imports changed).

## Milestone 5 — Precision (in progress)

Design: [RFC 0006 — Precision](rfcs/0006-precision.md) (Accepted).

- [x] Loans end at the holder's last use (liveness over the CFG); by
      default only invalidation (free, move, realloc) of a borrowed object
      is a `conflicting-borrow`; Rust's exclusivity rule behind
      `--exclusive-borrows` / `-fweavec-exclusive-borrows`.
- [x] Condition facts on CFG edges: pointer equality unites/separates
      aliases (with exact/interior alias edges); tests of a call result
      select the callee's outcome classes.
- [x] Element witnesses: `free(a[i])` remembers `i`; a later `a[i]` is a
      use of the same element, `a[j]` or `a[i]` after `i++` is not.
- [x] Outcome-conditional summaries (`outcome <class> <path> <flags>`),
      subsuming the `realloc` null-edge rule; summary format v2, sidecar v2.
- [x] `written` effects forget the facts below the written object.
- [ ] Corpus: clean baseline on the tracked projects; zlib and Lua added.

## Milestone 6 — Resource lifecycle (in progress)

Design: [RFC 0007 — Resource lifecycle](rfcs/0007-resource-lifecycle.md)
(Accepted).

- [x] `ResourceTracker` in the state: what this function owns, where it came
      from, its release family, and whether it escaped.
- [x] `leak` (warning) where a resource's last holder goes out of reach, is
      overwritten, is a discarded allocating call, or is a field dropped with
      its container.
- [x] `mismatched-release` (error): families on every allocator and releaser
      of the shipped table, inferred through wrappers, crossing units
      (summary format v3, sidecar v3).
- [x] `WEAVEC_OWNED` on struct fields enforced; inferred owned fields and
      array elements checked the same way.
- [x] Deepest-first application of consumed summary paths (a soundness fix
      for callees that free a field together with its object).
- [x] Per-outcome null facts for out-parameter constructors
      (`null <class> <path>`): `if (mk(&x) != 0) return -1;` is clean when
      `mk`'s error returns leave `*out` null or untouched.
- [x] Per-outcome *stores* (which of several stores holds on which class)
      (RFC 0010).
- [x] A release family for `WEAVEC_OWNED` declarations (`WEAVEC_OWNED_BY(f)`)
      (RFC 0010).
- [ ] Corpus triage of the new `leak` reports; leak rate as a tracked metric.

## Milestone 7 — Pointer validity (in progress)

Design: [RFC 0008 — Pointer validity](rfcs/0008-pointer-validity.md)
(Accepted).

- [x] Replaced values: consumption of a caller-visible path is recorded as
      it happens (`freed,replaced`), so a caller's copy of a value the callee
      released and reinitialised is dead (the `vec_grow` hole).
- [x] Struct-by-value results: the `result` summary root carries the pointer
      fields of a returned record to the caller.
- [x] `NullTracker` in the state; `null-dereference` (error) for
      dereferences and for arguments to callees that require non-null;
      `requires{...}` and `notnull{...}` in summaries; nullability of every
      shipped table entry; `WEAVEC_NULLABLE` / `WEAVEC_NONNULL` (summary
      format v4, sidecar v4).
- [x] `use-of-uninitialized` (error) for pointer locals and the pointer
      fields of record locals.
- [x] `invalid-release` (error) for stack and static objects, string
      literals and interior pointers.
- [ ] Nullness through struct fields across calls (a callee that nulls
      `b->data` and a caller that dereferences it) beyond the per-outcome
      `null{...}`/`notnull{...}` facts.
- [x] Integer-correlated tests (`if (n > 0) p = xmalloc(n); ... if (n > 0)
      *p`): the null record is guarded by `n zero|negative` and otherwise
      non-null, so the second test clears it (RFC 0009). With a plain
      `malloc` the report stays and is the unchecked-allocation one.
- [ ] Corpus triage of the new reports; null-dereference rate as a tracked
      metric.
- [ ] The consume fan-out at calls (`doConsume` over a path, its mirrors and
      their aliases, once per call site) that dominates Lua's `luaV_execute`
      since *Replaced values* (RFC 0008, *Performance*).

## Milestone 8 — Value-conditional behaviour (in progress)

Design: [RFC 0009 — Value-conditional behaviour](rfcs/0009-value-conditional-behaviour.md)
(Accepted).

- [x] `ScalarTracker` in the state: the class (`zero`, `positive`,
      `negative`) and known constant of integer places, refined by condition
      edges and `switch` cases, joined at merges.
- [x] Guards on moves, held resources and null records: each carries the
      facts of the path that created it and a later test that contradicts
      them drops it: `if (c) free(p); ... if (!c) use(p);` is clean.
- [x] Argument-conditional summaries: `when` guards on consumes, stores and
      return alternatives, translated to the arguments at the call and pruned
      against the caller's facts (summary format v5, sidecar v5).
- [x] Inferred `never-returns` for functions whose exit is unreachable,
      transitively through wrappers and across units; a call to one ends the
      path like a declared `noreturn`.
- [ ] Argument-conditional termination (`never-returns when ...`).
- [ ] Corpus: the `noreturn` group of Lua reports removed; time regression
      bounded.

## Milestone 9 — Shared ownership (in progress)

Design: [RFC 0010 — Shared ownership](rfcs/0010-shared-ownership.md)
(Accepted).

- [x] Shares on resource records: a count increment (`o->rc++`, the atomic
      builtins, a callee's `increment` path) retains its object; copies of
      a place with surplus shares carry one away (`sameShare` alias edges).
- [x] Share releases: a decrement whose zero test guards the free is a
      `freed,share` effect; the released name is dead (`use-after-free`,
      `double-free` with reference wording), siblings live on; `count` paths
      and the count-field registry for `leak` on retained shares.
- [x] Per-outcome integer facts (`fact <class> <path> <fact>`) so
      `dec_and_test` helpers compose with the release rule.
- [x] Per-outcome stores (`stored <class> <path>`): a store the callee did
      not perform on the selected class is retracted in the caller
      (summary format v6, sidecar v6).
- [x] `WEAVEC_RETAINS`, `WEAVEC_RELEASES`, `WEAVEC_REFCOUNT`,
      `WEAVEC_OWNED_BY(f)` (closes the RFC 0007 item above).
- [x] Whole-program fixpoint re-runs a cyclic group's member only when its
      imports changed.
- [ ] Corpus: a reference-counting project (jansson) added to the tracked
      set; the Lua whole-program run time as a tracked metric.

## Milestone 10 — Spatial safety (in progress)

Design: [RFC 0011 — Spatial safety](rfcs/0011-spatial-safety.md)
(Accepted).

- [x] Derived pointers: an offset (`core::PointerOffset`) on alias edges,
      resources, summary effects and returned copies replaces the boolean
      `interior` flag; `container_of` round trips; `!=` separates derived
      pointers; a field pointer kept across a free is a `use-after-free`.
- [x] `lifetime-too-short` decided at the pointee's death rather than at
      the store (the `L->fs = &fs; ... L->fs = fs.prev` idiom is clean).
- [x] Whole-program fixpoint widening after a fixed number of rounds, so
      cyclic groups converge on every input.
- [x] Extents (`core::SpatialTracker`, `core::Affine`) from allocations,
      wrappers (`return fresh extent=n`), declared sizes, string literals
      and `WEAVEC_SIZED_BY`; relations between integer places
      (`core::RelationTracker`) from condition edges.
- [x] `out-of-bounds` (error) for subscripts, pointer dereferences and the
      buffer/length pairs of the shipped table; `requires-extent` in
      summaries, checked at every call and across units (summary format
      v7, sidecar v7).
- [x] Recall check: Juliet-style cases under `test/recall/CWE-*`, run by
      `scripts/recall.py` in `ctest` and CI.
- [x] Extents through struct fields (`b->len` as the extent of `b->data`):
      RFC 0012's sized fields, below.
- [x] Constant extents across stores (a callee that sets `*out` and `*len`),
      plus stable allocation-time size identities (RFC 0013).
- [ ] Requirements that depend on two parameters (`min(n, cap)`), and
      requirements on `WEAVEC_SIZED_BY` parameters re-exported to callers.
- [ ] Corpus: `out-of-bounds` rate as a tracked metric; the Juliet test
      suite proper (CWE-121/122/124/126/127) as a recall baseline.

## Milestone 11 — Spatial safety II: strings and fields (in progress)

Design: [RFC 0012 — Spatial safety II](rfcs/0012-spatial-safety-strings-and-fields.md)
(Accepted).

- [x] String facts on spatial records (`core::StringFact`: the length as
      an `Affine`, or *unterminated*), on the object and every exact alias
      of it; `strlen(s)` as a *length place*; sources: literals and
      initialisers, `strcpy`/`stpcpy`/`strcat`/`sprintf`/`strdup`,
      `strncpy`/`memset`/`memcpy` that leave no terminator, NUL stores,
      `fgets`/`snprintf`; every other write drops them.
- [x] `out-of-bounds` for `strcpy`, `stpcpy`, `strcat` and `sprintf`
      against the destination's extent (`malloc(strlen(s))` is one short),
      and for terminator-seeking reads (`strlen`, `strcpy` source, `puts`,
      `%s`) of an object with no terminator.
- [x] `WEAVEC_SIZED_BY(g)` on pointer fields: loads get the count's extent,
      stores are checked (`annotation-mismatch`); malformed annotations are
      `invalid-annotation`.
- [x] Inferred sized fields: witnesses and refutations from every store in
      the program, confirmed when the program agrees; a second pass in the
      unit and a cross-unit pass in the whole-program driver report what
      the confirmation decides (sidecar v8: `sized-field`,
      `unsized-field`, `loads-field`).
- [x] Offset relations (`i <= n - 1`, `j = i + 1`) and constant lower
      bounds (`i >= 8`) in `core::RelationTracker`; `boundsVerdict` decides
      through them.
- [x] `WEAVEC_ASSUME(expr)`: the condition holds from the call on.
      `weavec.h` 0.7.
- [x] Known lengths through returned pointers, stored pointers and reachable
      fields, including constructors other than `strdup` (RFC 0013).
- [ ] String postconditions for in-place writes to an incoming buffer when
      no pointer value is stored or returned.
- [ ] Sized fields with a byte count on a non-`char` pointer, and counts
      one field-hop away (`b->hdr.len`).
- [ ] Corpus: the CWE-170 shape (`strncpy` without a terminator) as a
      tracked recall class; false-positive review of the string checks on
      the tracked projects.

## Milestone 12 — Interprocedural heap state and value identity

Design: [RFC 0013](rfcs/0013-interprocedural-heap-state.md).

- [x] Final heap postconditions through pointer and record returns,
      out-parameters, local aliases and whole-program summaries.
- [x] Child ownership, release families, argument aliases, shared children,
      self-links, null/raw values, bounds and known string facts.
- [x] Entry pointer identity separated from replacement output values,
      including failed replacement retaining the incoming value.
- [x] Allocation-time constant folding and bounded symbolic size snapshots.
- [x] Bounded graph projection with explicit incomplete coverage in dumps;
      summary and sidecar format 9.
- [x] A fixed good/bad evaluation matrix with known misses and independent
      execution-failure accounting, beside the existing recall regression set.

Richer arithmetic, arbitrary element identities and a verification mode that
rejects incomplete coverage need separate designs. Callback target precision
is addressed by the following milestone. This milestone does not make Lua's GC or stack-rebasing
invariants inferable.

## Milestone 13 — Pointer identity and precise call effects

Design: [RFC 0014](rfcs/0014-pointer-identity-and-call-effects.md).

- [x] Actual function-value target sets, preserving unknown and null alternatives.
- [x] Bounded callback helper specialization, including userdata associations,
      local forwarding and requests across translation units.
- [x] Equality/inequality guards on ownership effects and entry pointer snapshots.
- [x] Complete pointer and compatible record copies through `memcpy`/`memmove`.
- [x] Record-view validation for summary paths and explicit incomplete coverage.
- [x] Version 10 summaries and sidecars, callback dependencies and diagnostic controls.
- [x] Regression and evaluation pairs, strict C fixture validation, and pinned
      corpus tooling with process-failure and optional memory accounting.

Arbitrary byte fragments, unrestricted element identities, general callback
relational reasoning and GC/region invariants remain outside this milestone.
Recursive callback contexts that cannot be resolved within the bounds report
incomplete coverage. Runtime enforcement and verification mode remain separate.

## Milestone 14 — Array and container ownership

Design: [RFC 0015](rfcs/0015-array-and-container-ownership.md).

- [x] Independent selected cells, nested arrays and scalar index snapshots.
- [x] Per-cell ownership, initialization, nullness, callbacks and heap state.
- [x] Simultaneous complete pointer/record array copies and overlapping moves.
- [x] Sparse symbolic range snapshots and final helper/returned-container summaries.
- [x] Proved contiguous fill/cleanup loops and reallocation child preservation.
- [x] Format 11 summaries/sidecars, bounded import validation and global remapping.
- [x] Unit, integration and fixed evaluation pairs, including compiler link tests.
- [x] Pinned before/after corpus counts, location-level triage and measured
      performance in the [validation report](validation-rfc0015.md).

Unknown overlap can still produce conservative temporal reports. Arbitrary
strides, partial pointer representations, compositions that require retaining
unbounded range history and general loop invariants remain incomplete boundaries.
This milestone does not introduce verification mode or a tracing collector model.

## Milestone 15 — Compositional call checking

Design: [RFC 0016](rfcs/0016-compositional-call-checking.md).

- [x] Bounded caller contexts for aliases, storage identity, relative offsets,
      reference shares, distinct objects and scalar/null entry facts.
- [x] Reuse the callee CFG to preserve statement order, guards, replacement
      and saved incoming values under those relationships.
- [x] Combined callback and data contexts, with diagnostic call notes and
      unsafe-reporting state preserved through nested requests.
- [x] Cross-unit request/result convergence, strict global remapping and
      format 12 sidecars; compiler replay includes locally complete definers.
- [x] Inline/helper/cross-file evaluation pairs, compiler link tests,
      malformed-context and resource-limit regression coverage.
- [x] Published validation and pinned corpus changes, including precision
      limits and analysis cost in [the report](validation-rfc0016.md).

Calls with no established interacting relationship retain generic checking.
An absent alias edge does not prove disjointness. Enumerating arbitrary input
alias partitions, unrestricted heap invariants and enforcing complete coverage
belong to a separate verification milestone. The existing machine-width and
non-affine size-analysis gaps also remain.

## Ongoing

- Corpus testing against real C projects (`scripts/corpus.py`; false-positive
  rate as a tracked metric, `scripts/corpus/baseline.json`).
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
