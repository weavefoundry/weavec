# Roadmap

Rough ordering of the major pieces of work. Items link to the design notes
where they exist; everything here is subject to change as we learn.

## Milestone 0 — Scaffolding (done)

- [x] Layered library structure (`Core` / `Analysis` / `Frontend`), build,
      tests, CI, docs.
- [x] End-to-end pipeline with a minimal local ownership checker.
- [x] Annotation header and recognition.

## Milestone 1 — Sound intra-procedural checking

- [ ] Replace the AST walk with a forward dataflow over `clang::CFG`
      (fixpoint for loops, proper handling of `switch`, `goto`, short-circuit).
- [ ] Model allocation/release functions: `malloc`, `calloc`, `realloc`,
      `strdup`, `free`, plus user-annotated allocators.
- [ ] Drive `BorrowState` from address-of, array decay and pointer copies;
      emit `conflicting-borrow`.
- [ ] Drive `LifetimeConstraints` from scopes; emit `lifetime-too-short` for
      escaping pointers to locals.
- [ ] Field-sensitive places (`s->p`, `a[i]` as a summary place).
- [ ] `--dump-analysis` for debugging inferred facts.

## Milestone 2 — Signature inference and annotations

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

- [ ] Summary database alongside `compile_commands.json` for cross-TU
      inference.
- [ ] Summaries for libc and common libraries shipped with WeaveC.
- [ ] Incremental re-analysis.

## Ongoing

- Corpus testing against real C projects (false-positive rate as a tracked
  metric).
- Fuzzing the analyzer with generated C.
- Windows support once the analysis stabilises.
