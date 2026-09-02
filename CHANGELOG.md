# Changelog

All notable changes to WeaveC are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
follows [Semantic Versioning](https://semver.org/) once it reaches 1.0.

## [Unreleased]

### Added

- Initial project scaffolding: CMake build with presets, LLVM/Clang discovery,
  strict warnings, sanitizer and LTO options, install/export rules and CPack.
- `weavec::Core`: Clang-independent ownership lattice (`OwnershipKind`),
  lifetime constraints, loan tracking (`BorrowState`), move tracking
  (`MoveTracker`) and a frontend-neutral diagnostics interface.
- `weavec::Analysis`: annotation recognition and a first path-insensitive
  local ownership checker reporting `use-after-free`, `double-free`,
  `invalid-annotation` and (opt-in) `annotation-required`.
- `weavec::Frontend`: Clang `ASTFrontendAction`, diagnostics bridging to
  Clang's `DiagnosticsEngine`, resource-directory discovery.
- `weavec` command-line tool built on libTooling (`weavec file.c -- <flags>`),
  with `--report-unannotated` and `--analyze-headers`.
- `weavec.h` annotation header: `WEAVEC_OWNED`, `WEAVEC_BORROWED`,
  `WEAVEC_MUT`, `WEAVEC_UNSAFE`, `WEAVEC_ENABLED`.
- GoogleTest unit tests and lit/FileCheck integration tests.
- GitHub Actions CI (Linux ASan/UBSan + Release, macOS Release, clang-format,
  cmake-format, clang-tidy, CodeQL), Dependabot, issue and PR templates.
- Project documentation: architecture, annotations reference, developer
  guide, roadmap.
- RFC process for changes to the model, checker rules, annotations and
  diagnostics (`docs/rfcs/`), with RFC 0001 (ownership model) and RFC 0002
  (sound intra-procedural checking), both Accepted.
- Sound intra-procedural checking (RFC 0002): a forward dataflow over
  `clang::CFG` replaces the path-insensitive AST walk, so loops, `switch`
  fall-through, `goto` and short-circuit operands are analysed on every path
  and each problem is reported once.
- Pointer copies are tracked as aliases (`core::AliasRelation`): freeing
  through one name frees every name, and the note says which
  (`freed here (through 'q')`).
- Structured places: `s.f`, `p->f`, `*pp`, nested field paths, and one
  summary place per array (`a[*]`).
- Allocator and release recognition (`malloc`, `calloc`, `realloc`, `strdup`,
  `strndup`, `aligned_alloc`, `free`, `WEAVEC_OWNED` returns/parameters),
  with the `realloc` failure idiom (`if (!q) free(p)`) accepted.
- New diagnostics: `use-after-move` (owned pointer passed to a `WEAVEC_OWNED`
  parameter or to `realloc`, then used), `conflicting-borrow` (two live
  borrows that conflict, or writing/freeing/moving a borrowed object) and
  `lifetime-too-short` (a pointer to a local stored somewhere that outlives
  it, or returned).
- Borrows from `&x`, `&s->f`, array decay and `WEAVEC_BORROWED`/`WEAVEC_MUT`
  arguments; loans are mutable unless the pointer's pointee is `const`.
- `weavec --dump-analysis` prints the inferred places, lifetimes and exit
  state of every analysed function.
- Signature inference (RFC 0003): every function definition in a translation
  unit gets a `core::FunctionSummary` (effects on parameters, paths under
  them and globals; stores into caller-visible memory; return-value
  provenance), inferred bottom-up over the call graph with a fixpoint inside
  recursive cycles, and applied at every call site. `node_free(n); n->v`,
  `buf_destroy(&b); b.data[0]`, out-parameters, helpers that free globals
  and wrappers of wrappers are now checked without annotations.
- Shipped summaries for the C standard library (`malloc` family, `str*`,
  `mem*`, `stdio`, `strtol`, `getenv`, ...), including which results alias
  which arguments (`strchr`, `strtol`'s end pointer) so real headers need no
  annotations.
- `annotation-mismatch` (error): a definition's body contradicts its own
  `WEAVEC_OWNED`/`WEAVEC_BORROWED`/`WEAVEC_MUT` annotation (frees or writes
  through a borrowed parameter, moves a `WEAVEC_MUT` one, returns a borrow
  from a `WEAVEC_OWNED` result, ...). Callers keep trusting the annotation.
- `annotation-required` is on by default at the external boundary: the
  first call to a function with no definition here, no annotations and no
  libc entry warns once per callee (system headers exempt).
  `--strict-externs` makes it an error. With `--report-unannotated`,
  exported functions get one warning per unannotated pointer position
  carrying a fix-it that inserts the inferred annotation.
- `core::Diagnostic` carries `FixItHint`s, bridged to Clang so
  `-fdiagnostics-parseable-fixits` and editors can apply them.
- `--dump-analysis` prints a `summary:` line per function.
- `scripts/corpus.py`: runs `weavec` over real C projects
  (`scripts/corpus/projects.json`), tallies diagnostics per id and compares
  with `scripts/corpus/baseline.json`; a weekly `Corpus` workflow runs it.
- `TranslationUnitAnalyzer` (Analysis) and `SummaryStore` as the public
  entry points for whole-TU analysis; `FunctionAnalyzer::analyze` now takes
  the store.

### Changed

- `double-free` and `use-after-free` are now also reported for the second
  iteration of a loop that frees a pointer declared outside it.
- `AnalysisOptions` gained `dumpStream`; `core::Loan` gained `holder` and
  `core::MoveRecord` gained `via`.
- Copying a pointer that holds a loan into a longer-lived pointer (`g = p`
  where `p = &local`) is now `lifetime-too-short`, like `g = &local` already
  was.
- Calls to unannotated functions now borrow their pointer arguments for the
  duration of the call (previously: no effect), so `set(&x)` while `x` is
  borrowed is a `conflicting-borrow`.
- `--report-unannotated` no longer reports `static` functions or `main`, and
  its message names the inferred annotation.
- `analysis::CallEffects` is now backed by a `core::FunctionSummary`
  (`releasesArgs` replaced by `frees(i)`; `classifyCall` takes a
  `SummaryStore`).

### Removed

- `LocalOwnershipChecker` (superseded by `FunctionDataflow`).

[Unreleased]: https://github.com/weavefoundry/weavec/commits/main
