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

## Milestone 1 — Sound intra-procedural checking

Design: [RFC 0002 — Sound intra-procedural checking](rfcs/0002-intraprocedural-checking.md)
(Accepted). The items below summarise it; the RFC is authoritative.

- [ ] Replace the AST walk with a forward dataflow over `clang::CFG`
      (fixpoint for loops, proper handling of `switch`, `goto`, short-circuit;
      diagnostics emitted in a post-fixpoint reporting pass).
- [ ] Alias partition so copies of an owned pointer share one resource.
- [ ] Model allocation/release functions: `malloc`, `calloc`, `realloc`,
      `strdup`, `free`, plus user-annotated allocators; `realloc` failure
      idiom accepted via null-edge reinstatement.
- [ ] Drive `BorrowState` from address-of, array decay and annotated calls;
      emit `conflicting-borrow`.
- [ ] Drive `LifetimeConstraints` from scopes; emit `lifetime-too-short` for
      escaping pointers to locals.
- [ ] Field-sensitive places (`s->p`, `a[i]` as a summary place).
- [ ] `--dump-analysis` for debugging inferred facts.

## Milestone 2 — Signature inference and annotations

RFC: to be written (signature inference; unsafe blocks).

- [ ] Per-function summaries (parameter/return ownership, outlives relations)
      computed bottom-up over the TU call graph.
- [ ] Honour `WEAVEC_OWNED` / `WEAVEC_BORROWED` / `WEAVEC_MUT` on
      declarations; report inference/annotation disagreements.
- [ ] `annotation-required` on by default at ABI boundaries, with fix-it hints
      that insert the inferred annotation.
- [ ] Unsafe blocks: pointers escaping an unsafe block become `Raw`;
      `unsafe-operation` for `Raw` use outside unsafe.

## Milestone 3 — Compiler driver

- [ ] `weavec` as a drop-in `cc`: parse driver flags with Clang's driver,
      run the analysis, then delegate code generation to Clang.
- [ ] `-fweavec-strict` / `-fno-weavec` and warning-group style control over
      diagnostic identifiers (`-Wweavec-use-after-free`).
- [ ] Clang plugin packaging so existing builds can add `-fplugin=weavec`.

## Milestone 4 — Whole-program

RFC: to be written (cross-TU summaries).

- [ ] Summary database alongside `compile_commands.json` for cross-TU
      inference.
- [ ] Summaries for libc and common libraries shipped with WeaveC.
- [ ] Incremental re-analysis.

## Ongoing

- Corpus testing against real C projects (false-positive rate as a tracked
  metric).
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
