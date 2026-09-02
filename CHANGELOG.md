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
  diagnostics (`docs/rfcs/`), with RFC 0001 (ownership model), RFC 0002
  (sound intra-procedural checking), RFC 0003 (signature inference) and
  RFC 0004 (unsafe boundaries).
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
- Raw pointers (RFC 0004): a pointer cast from an integer, declared
  `WEAVEC_RAW`, loaded through another raw pointer, or handed out as raw by
  a callee is tracked as *raw* (`core::RawTracker`, `AnalysisState::raw`,
  `ValueSource::raw()`). Dereferencing it, releasing it, passing it to an
  owning or dereferencing parameter, or asserting a safe kind for it outside
  a `WEAVEC_UNSAFE` region is the new `unsafe-operation` error, with a note
  saying why the pointer is raw. Copying, comparing and converting it back
  to an integer are fine anywhere.
- `WEAVEC_RAW` annotation for pointer parameters, returns, variables, fields
  and function-pointer types ("no ownership guarantee"). `weavec.h` is at
  header version 0.2.
- Laundering (RFC 0004): inside a `WEAVEC_UNSAFE` region, assigning a raw
  pointer to a place declared `WEAVEC_OWNED`/`WEAVEC_BORROWED`/`WEAVEC_MUT`
  or returning it from a function whose return type is so annotated asserts
  that kind, so pointers can be brought back into the model at one explicit
  point (`WEAVEC_UNSAFE { n = (struct node *)handle; } free(n);`).
- Calls through function pointers (RFC 0004) are checked: the signature
  comes from ownership annotations on the function-pointer type (`typedef
  void (*dtor_t)(void *WEAVEC_OWNED);`, fields, parameters), else from the
  join of the summaries of every function of that type whose address is
  taken in the translation unit (`ops.drop = node_free;`, `qsort(..., cmp)`).
  Callbacks are analysed before their callers. Pointers with neither get one
  `annotation-required` warning per function-pointer type.
- POSIX and common GNU/BSD functions in the shipped library table (about 490
  entries, up from 85): `<unistd.h>`, `<fcntl.h>`, `<sys/stat.h>`,
  `<dirent.h>` (`opendir`/`closedir`/`readdir`), `<stdio.h>` extensions
  (`getline`, `getdelim`, `asprintf`, `popen`/`pclose`, `fmemopen`,
  `open_memstream`), `<stdlib.h>` extensions (`posix_memalign`,
  `reallocarray`, `realpath`, `mkstemp`, `qsort_r`), `<string.h>` extensions
  (`strtok_r`, `strsep`, `stpcpy`, `strlcpy`, `memmem`, ...), `<time.h>`,
  `<sys/mman.h>` (`mmap`/`munmap`), `<pthread.h>`, `<sys/socket.h>`,
  `<netdb.h>` (`getaddrinfo`/`freeaddrinfo`), `<arpa/inet.h>`, `<dlfcn.h>`,
  `<regex.h>`, `<signal.h>`, `<sys/wait.h>`, `<poll.h>`, `<pwd.h>`,
  `<grp.h>`, `<iconv.h>`, `<glob.h>`, `<wchar.h>` and more.
- `--dump-analysis` prints the raw component of the exit state (`raw{r@3:13
  integer-cast}`), `raw` as a place kind and as a value source.
- `core::FunctionSummary::inferredReturnKind` reports `Raw`;
  `--report-unannotated` offers `WEAVEC_RAW` for a result inferred raw.
- `SummaryStore::lookupIndirect`, `addAddressTaken`, `candidatesFor`;
  `analysis::collectFunctionTypeAnnotations`; `TranslationUnitAnalyzer::
  collectAddressTaken`.
- Whole-program analysis (RFC 0005): the unit of analysis is the program,
  not the translation unit. Every unit exports the summaries of its
  external-linkage definitions and address-taken functions; a
  `ProgramDatabase` (Analysis) collects them and `SummaryStore` consults it
  between a unit's own inference and the libc table, so `node_free(n);
  n->v` is caught when `node_free` is defined in another file, a program's
  own `strdup` is checked against its own body, and a callback installed in
  one file is joined into the signature of the function-pointer type it is
  called through in another. Units are analysed dependencies-first by
  strongly connected component (`core::stronglyConnectedComponents`,
  `Core/Scc.h`); mutually dependent units iterate to a fixpoint.
- `weavec --whole-program [files] -- <flags>` (or `-p build/` for every
  file of a compilation database) analyses the files as one program;
  `--dump-analysis` then ends with the program database.
- `weavec-cc`, a drop-in C compiler: Clang's driver plans the jobs, each
  `-cc1` runs in-process with WeaveC's consumer multiplexed beside Clang's
  code generation (one parse; a WeaveC error fails the compile). The
  compile step writes the unit's exports, its cc1 command and the
  diagnostics it reported to `<object>.weavec`; the link step reads the
  sidecars of the objects on the link line, re-analyses the units whose
  results depend on other units, reports what needs two files (once), and
  refuses to link on an error. Objects without a sidecar are unknown code;
  a sidecar older than its object is ignored with a warning; sidecars of
  one-step builds (`weavec-cc a.c b.c -o prog`) are read and removed with
  the temporaries. `-cc1as` and other jobs are delegated to `clang`
  (`WEAVEC_CLANG`, the configured `WEAVEC_CLANG_EXECUTABLE`, or `PATH`).
- Driver flags: `-fweavec`/`-fno-weavec`, `-fweavec-strict`,
  `-fweavec-report-unannotated`, `-fweavec-analyze-headers`,
  `-fweavec-dump-analysis`, `-fweavec-link`/`-fno-weavec-link`. Warning
  control for both tools (`frontend::DiagnosticControl`):
  `-Wno-weavec-<id>` (warnings only), `-Wweavec-<id>`,
  `-Wno-error=weavec-<id>`, `-Werror=weavec-<id>`, `-Wno-weavec`,
  `-Wno-error=weavec`, `-Werror=weavec`. Disabling an error is refused
  with a message naming the `-Wno-error=` form.
- `core::FunctionSummary` text format (`Core/SummaryIO.h`:
  `printSummary`/`parseSummary`, `SummaryFormatVersion`), Clang-free and
  round-trip tested; globals are spelled by name through callbacks.
  `core::FunctionSummary::remapGlobals`. `core::diag::All`, `isKnown`,
  `isWarningByDefault`.
- `analysis::UnitExports`, `ExportedFunction`, `functionTypeKey`,
  `ProgramDatabase`; `SummaryStore::setDatabase`, `SummarySource::Program`,
  `isAddressTaken`, `unknownCalleeNames`, `unknownIndirectTypeKeys`;
  `TranslationUnitAnalyzer::setDatabase`, `discover()`, `exports()`;
  `AnalysisOptions::deferBoundary`.
- `frontend::ProgramAnalysis` (over an abstract `ProgramUnit`;
  `CompilationDatabaseUnit`), `frontend::Sidecar.h` (`UnitRecord`,
  `readSidecar`/`writeSidecar`, `weavec-summaries 1`),
  `frontend::Driver.h` (`DriverOptions`, `runDriver`, `runCc1`),
  `createWeaveCConsumer`, `FrontendOptions::{database, alreadyReported,
  boundaryOnce, silent, discoverOnly, onResult}`, `UnitResult`.
- `WeaveCFrontendTests` unit-test binary; `test/WholeProgram/` and
  `test/Driver/rfc0005-*.c` lit suites.
- `scripts/corpus.py`: `"whole_program": true` projects are analysed as one
  program (`--local-whole-program` for `--local`); `cJSON-program` and
  `linenoise-program` added to the corpus, with triage in
  `scripts/corpus/README.md`.

### Changed

- A whole-struct assignment or initialisation (`b = a`, `struct buf b = a;`,
  `*p = s`) copies the facts of every pointer-typed field (aliases, loans,
  moves, raw records, kinds) instead of forgetting them (RFC 0005), so
  `b = a; free(a.data); b.data[0]` is a `use-after-free`. Fields with no
  place under the source are still forgotten.
- `annotation-required` fires for a callee with no definition anywhere in
  the program (previously: in the translation unit), once per callee per
  program; in `weavec-cc` it is deferred from the compile step to the link
  step. The second note now ends `or define it in this program`; the
  indirect form says `in this program` instead of `in this translation
  unit`.
- `--dump-analysis` in whole-program mode prefixes each unit's dump with
  `unit '<source>':`.
- `tools/weavec` accepts zero source files with `-p` (every file of the
  database is analysed).
- `SccFinder` moved from `TranslationUnitAnalysis.cpp` to
  `core::stronglyConnectedComponents` (`Core/Scc.h`).
- `cmake/WeaveCLLVM.cmake` records `WEAVEC_CLANG_EXECUTABLE`;
  `frontend::getClangExecutable()`.

- `WEAVEC_UNSAFE` blocks and function bodies are analysed instead of skipped
  (RFC 0004): diagnostics inside the region are suppressed, raw operations
  are permitted, but ownership effects flow out of it, so
  `WEAVEC_UNSAFE { free(p); } p[0]` is now a `use-after-free` at `p[0]` and
  a `WEAVEC_UNSAFE` function's callers see its inferred effects. A
  `WEAVEC_UNSAFE` declaration without a body is unchanged (empty summary, no
  boundary warning).
- Pointer arithmetic (`p + 1`, `p++`, `&a[i]`) and casts between pointer
  types preserve the identity of the pointed-to object instead of producing
  an untracked value (RFC 0004), so `q = p + 1; free(p); q[0]` is a
  `use-after-free` and `bind(fd, (struct sockaddr *)&addr, len)` borrows
  `addr`. Only integer-to-pointer casts lose identity, and they yield a raw
  pointer.
- `--strict-externs` (RFC 0004): a call the checker cannot resolve is now an
  `unsafe-operation` error at every call site outside a `WEAVEC_UNSAFE`
  region (system headers included) and its pointer result is raw, instead
  of an `annotation-required` error once per callee. Inside an unsafe
  region it is silent.
- `annotation-required`'s notes mention `WEAVEC_RAW`; a new form covers
  calls through function pointers with no signature.
- `analysis::AnnotationSet::safeKind()` returns
  `std::optional<core::OwnershipKind>`; `AnnotationSet` gained `raw`,
  `ownership()` and `merge()`; `analysis::ValueOrigin` gained `Raw` and
  `rawReason`.

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
