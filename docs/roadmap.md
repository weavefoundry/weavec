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
- [ ] Follow-ups surfaced by the corpus: may-moves for `a[*]` element places
      (`free(a[i])` in a loop), pointer-equality guards.
- [ ] Function pointers stored in globals and returned from other TUs
      (needs cross-TU summaries, Milestone 4).

## Milestone 3 — Compiler driver

- [ ] `weavec` as a drop-in `cc`: parse driver flags with Clang's driver,
      run the analysis, then delegate code generation to Clang.
- [ ] `-fweavec-strict` / `-fno-weavec` and warning-group style control over
      diagnostic identifiers (`-Wweavec-use-after-free`).
- [ ] Clang plugin packaging so existing builds can add `-fplugin=weavec`.

## Milestone 4 — Whole-program

RFC: to be written (cross-TU summaries).

- [ ] Summary database alongside `compile_commands.json` for cross-TU
      inference (`core::FunctionSummary` is designed to be the on-disk
      format).
- [ ] Summaries for common libraries beyond libc shipped with WeaveC.
- [ ] Incremental re-analysis.

## Ongoing

- Corpus testing against real C projects (`scripts/corpus.py`; false-positive
  rate as a tracked metric, `scripts/corpus/baseline.json`).
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
