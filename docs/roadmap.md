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
      function-pointer type, else the join of the address-taken functions of
      that type; indirect edges in the call graph.
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
      (RFC 0005: candidates are joined across the program).

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
- [ ] Per-outcome *stores* (which of several stores holds on which class).
- [ ] A release family for `WEAVEC_OWNED` declarations (`WEAVEC_OWNED_BY(f)`
      or a family attribute), so annotated APIs get the mismatch check too.
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

## Ongoing

- Corpus testing against real C projects (`scripts/corpus.py`; false-positive
  rate as a tracked metric, `scripts/corpus/baseline.json`).
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
