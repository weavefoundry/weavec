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
- Precision (RFC 0006). Loans end at the last use of their holder, not at
  the end of its scope: a backward liveness pass over the CFG expires the
  loans held by dead locals before each element, so `char *p = buf;
  use(p); buf[0] = 0;` and `int *a = &n->v; *a = 1; free(n);` are clean
  while `free(n); *a = 1;` is still `cannot free 'n' while it is borrowed`.
  Loans held through a pointer, by a global or by an address-taken local
  last until the holder is reassigned (`BorrowState::expireHolders`).
- Condition facts on CFG edges (RFC 0006): `p == q` / `p != q` unite or
  separate the two pointers on the edge where the test holds, so `if (l ==
  sentinel) return; free(l); use(sentinel);` is clean; `!=` separates only
  *exact* aliases. `AliasRelation` records per edge whether two places hold
  the same value or point into the same object (`unite(a, b, exact)`,
  `separateExact`, `isExact`, `edge`); pointer arithmetic makes a copy
  interior (`ValueOrigin::interior`, `ValueSource::interiorCopy`, printed
  `interior <path>`), and the libc table says which results are the
  argument itself (`memcpy`, `strcpy`, `fgets`, `getcwd`, ...) and which
  point into it (`strchr`, `strstr`, `strtok`, `bsearch`, `readdir`, the
  `strto*` end pointers, ...).
- Element witnesses (RFC 0006): `a[*]` is still one place, but a move
  record remembers which element was named (`core::ElementWitness`: whole,
  a constant, a variable, or unknown) and only an access with a matching
  witness is a use of it. `free(a[i]); a[i][0] = 0;` and `free(a[0]);
  free(a[0]);` are reported; `for (i) free(a[i]); free(a);`, `free(a[0]);
  use(a[1]);`, `free(a[i]); a[i] = NULL;` and `free(a[i]); use(a[j]);` are
  clean. Writing to, incrementing or taking the address of the index
  variable makes its witnesses unknown (`MoveTracker::forgetWitness`).
  `*a` on an array is `a[0]`.
- Outcome-conditional summaries (RFC 0006): a callee that frees or moves an
  argument only on the paths returning some class of value (`null` /
  `nonnull`, `zero` / `positive` / `negative`) is summarised per class
  (`core::Outcome`, `FunctionSummary::outcomes`, `addOutcome`,
  `consumesUnconditionally`), and a caller's test of the result (`if (!q)`,
  `q == NULL`, `rc != 0`, `rc < 0`, `rc == -1`, `(rc = f(p)) == 0`, ...)
  retracts the consumption on the edge where it did not happen. `int rc =
  try_take(p); if (rc != 0) free(p);` is clean and `if (rc == 0) use(p);`
  is a `use-after-move`; wrappers around `realloc` (`if (!q) return NULL;
  return q;`) and around error-returning functions inherit the conditional
  behaviour, across units too. `realloc`'s null edge is now this mechanism
  (`AnalysisState::pending`, `PendingOutcome`, `AnalysisState::consumed`;
  `reallocLike`/`isRealloc` are gone). Summary text format version 2:
  `outcome <class>`, `outcome <class> <path> <flags>`, `interior <path>`;
  sidecar format version 2 (`weavec-summaries 2`); `--dump-analysis` prints
  the classes (`outcome zero{n: freed} outcome negative{}`).
- A callee's `written` effect forgets every fact below the written place
  (RFC 0006), so `free(root->string); memcpy(root, &tmp, sizeof *root);
  use(root->string);` is clean and the summary of such a body no longer
  claims the field is freed. Fortified `__builtin___memcpy_chk`-style
  variants of the `mem*`/`str*` copy functions share their entries.
- `--exclusive-borrows` (`weavec`) / `-fweavec-exclusive-borrows`
  (`weavec-cc`), `AnalysisOptions::exclusiveBorrows`: enforce RFC 0001's
  exclusivity between borrows (see *Changed*).
- zlib (15 units) and Lua (34 units) added to the corpus as whole programs.
  zlib is clean apart from two callback boundaries; every Lua report is
  triaged in `scripts/corpus/README.md`. The corpus baseline now has no
  `use-after-free` outside Lua, no `conflicting-borrow` outside Lua and two
  `double-free` outside Lua (down from 15, 4 and 10).
- Resource lifecycle (RFC 0007). Every allocation, `WEAVEC_OWNED` parameter
  and fresh result puts a resource on the function's books
  (`core::ResourceTracker`, `AnalysisState::resources`, `ResourceRecord`),
  and the checker reports what happens to it:
  - `leak` (new, **warning**): the resource's last holder goes out of reach
    (`return`, scope end, the statement after its last use, a value that is
    never read) without it having been released, moved, returned, stored
    into caller-visible memory, handed to unknown code or cast to an
    integer; also `'<p>' is leaked: it is overwritten without being
    released`, `'<b>->p' is leaked when '<b>' is freed`, `result of '<f>'
    is leaked` for a discarded allocating call. Copies share one record and
    one report; a callee summarised from its body that records no effect on
    a pointer parameter is trusted not to retain it; the null edge of a
    test of the holder owns nothing (through `__builtin_expect` and
    `(c) != 0` too); a block ending in a `noreturn` call is not a death
    point, nor is the edge into one; a value stored below a pointer whose
    object nobody here owns or names (`box = lua_touserdata(L, 1);
    box->buf = malloc(n)`) escapes; a resource kept by a global or `static`
    local is not a leak.
  - `mismatched-release` (new, **error**): every allocator in the shipped
    table belongs to the family of its releaser (`malloc`/`strdup`/`realloc`
    → `free`, `fopen` → `fclose`, `opendir` → `closedir`, `getaddrinfo` →
    `freeaddrinfo`, `mmap` → `munmap`, `popen` → `pclose`, ...), and
    releasing a resource of one family with a function of another
    (`free(fopen(...))`, `fclose((FILE *)malloc(8))`, `realloc(f, n)` on a
    `FILE`) is reported, through wrappers defined in the program and across
    units. Inferred summaries carry the family (`fresh(fclose)`,
    `freed(free)`, `moved(free)`; `ValueSource::family`,
    `PlaceEffect::family`, `FunctionSummary::freshReturnFamily`).
  - `WEAVEC_OWNED` on a struct field is now enforced: `free(b)` with `b->p`
    neither freed, moved, tested nor nulled is a `leak` of `b->p`, for
    objects that came from outside (a parameter, a global, a load). A field
    this function stored an owned value into is checked the same way
    without the annotation (`b->data = malloc(8); free(b);`), as are array
    elements (`a[0] = strdup("x"); free(a);`).
  - Per-outcome null facts (`FunctionSummary::nullOn`, `PendingOutcome::
    nullOn`): a constructor that reports failure through its result (`int
    make(char **out) { *out = malloc(n); return *out != NULL; }`, or `if
    (*out == NULL) return -1;`) is summarised with the classes on which
    `*out` is null or holds nothing the callee stored (the guard returns
    before the store, `if (strm == NULL) return Z_STREAM_ERROR;`), so `if
    (!make(&s)) return; free(s);` and `err = init(&s); if (err != 0) return
    err;` are clean (RFC 0007, *Per-outcome null stores*).
  - `weavec.h` header version 0.2 → 0.3 (documentation of `WEAVEC_OWNED` on
    fields). `-Wno-weavec-leak` disables the warning; `-Werror=weavec-leak`
    promotes it.
  - Summary text format version 2 → 3: `freed(<family>)`, `moved(<family>)`,
    `fresh(<family>)` and `null <class> <path>` lines; sidecar format
    version 3 (`weavec-summaries 3`). `--dump-analysis` prints the exit
    state's resources (`owned{p@3:14 allocated free}`) and the families in
    summaries; the program dump uses the same spellings.

### Fixed

- Applying a callee's summary consumed shallower paths before deeper ones,
  so `static void both(struct box *b) { free(b->p); free(b); }` followed by
  a use of a caller's copy of `b->p` was not reported: freeing `b` first
  dropped the facts under it before `b->p`'s consumption reached the alias.
  Consumed paths are now applied deepest first; memory below an object the
  same call frees is consumed on its own only when the caller knows the
  place, and reported once (at the object) when the object is already gone
  (RFC 0007, *Applying a summary: deepest paths first*).
- `putenv` keeps its string and `setvbuf` keeps its buffer in the shipped
  table (RFC 0007, *Assumptions*).
- `a->f` on an array (`printbuffer buffer[1]; buffer->buffer = ...`) is the
  place `a[0].f`, as `*a` already was `a[0]`; it used to be the element
  summary `a[*].f`, whose null store could not clear a record.
- Condition facts on the right operand of `||` / `&&`: the block that
  evaluates `(p = malloc(n)) == NULL` in `if (fd == -1 || (p = malloc(n))
  == NULL) return NULL;` branches on that operand, but Clang hands back
  the whole condition; the null edge did not clear `p`'s record and the
  return was a false `leak`.
- Freeing memory reached *below* a borrowed object is not a
  `conflicting-borrow`: `z_streamp strm = &state->strm;
  inflateInit2(&state->strm, ...)`, whose callee frees
  `state->strm.state->window`, left `strm` pointing at storage nothing
  released. Loans on ancestors of the consumed place no longer count for
  the consume query (RFC 0006, *Conflict rules*, amended).
- A `leak` at the end of an address-taken local's scope is reported on the
  statement the scope ends after (the `return`), not on the declaration.
- A block ending in a `noreturn` call no longer flows into the function's
  exit state, so what such a path frees (Lua's `os_exit` closing the state
  behind `exit`) does not become a `freed` effect of the summary and a
  `double-free` at every caller; the summary describes the state after a
  *return* (RFC 0007, *Interaction with existing RFCs*).
- On the null edge of a pointer test every fact below the pointer is dropped
  (there is nothing there), so records, moves and aliases of the pointer's
  fields no longer report on a path where the pointer is null.
- A whole write to a field also clears the move records of the same cell
  under the pointer's aliases (`free(L->stack); L->stack = fresh` with
  `L->twups ~ L` left `L->twups->stack` freed for good), which on Lua made
  every second `luaD_reallocstack` a `double-free` (RFC 0002, *Alias
  relation*, implementation notes).
- An `&&`/`||` under `!`, `__builtin_expect` or `!= 0` is computed as a value
  before the branch, so only the whole value is known on an edge: a true `&&`
  or a false `||` now holds each operand (Lua's `l_unlikely(newblock == NULL
  && nsize > 0)` makes `newblock` null, and overwriting it is no `leak`),
  and the other edges say nothing, where the right operand alone used to be
  taken (`if (__builtin_expect(n > 0 && p != NULL, 1)) { free(p); return 0;
  } return -1;` was wrongly clean).
- Two paths of one callee summary that name the same cell through the
  caller's aliases (`g->allgc` and `g->twups->l_G->allgc` with `g->twups ~
  L`, Lua's `luaC_freeallobjects`) are one release, not a `double-free`
  (RFC 0007, *Applying a summary: deepest paths first*).
- A value stored below a local that borrows caller memory or a global
  (`tb = &G(L)->strt; tb->hash = newvect`, Lua's `luaS_resize`) escapes:
  the store landed in an object that outlives the function under a name the
  summary cannot report, and the last local name's death was a false `leak`
  (RFC 0007, *Escape*).

### Changed

- The RFC 0007 leak check found real leaks in the existing unit-test
  snippets (`realloc` success paths, an `OWNED` parameter that was only
  looked at, a `switch` without `default`, a loop whose `free` is behind
  `break`); their expectations now include the `leak`.
- A callee's summary store into a place that holds a resource this function
  made owned escapes the old value instead of reporting an overwrite:
  RFC 0003 shows the caller only the store for the far more common
  `free(b->data); b->data = NULL;` (RFC 0007, *Deliberately not caught*).
- A call with a mutably borrowed argument copies the callee's written paths
  below the argument into the caller's summary (`L->nci: written`,
  `L->ci->top.p: written`, ...) instead of marking the pointee itself
  `written`, which told every caller the whole object had been overwritten
  and made it forget everything it knew below the argument (Lua's
  `close_state` lost the frees `freestack` reported to it). Summaries on
  Lua-sized programs are an order of magnitude longer as a result; the
  paths are resolved incrementally, cached per call while no new place is
  interned, and copied once per call and pointee, but the Lua program still
  takes about twice as long as before (RFC 0006, *What a callee wrote, its
  caller wrote*; RFC 0007, *Performance*). The three new Lua
  `conflicting-borrow` reports are facts this used to erase (`close_state`
  frees `L->l_G` while `L`, an interior pointer into it, is live), of the
  interior-pointer kind RFC 0001 already accepts.
- A callee that consumes a path below an argument or a global and returns a
  `copy` of it (`t->array = resizearray(L, t, ...)`) makes the result the
  same resource, now owned by the result, as RFC 0003 already did for
  consumed parameters (RFC 0006, *Interaction with existing RFCs*). Only
  when the path is moved or consumed on every class: a copy of a path
  `freed` on some classes only (zlib's `gz_look` copying `state->out`,
  which `gz_fetch` frees on its error class) is not tracked, otherwise the
  copy's later overwrite was a false `leak` (RFC 0007, *Applying a
  summary: deepest paths first*).
- `(res = feed()) == sentinel` and `(p = f()) != q` are pointer-equality
  condition facts about `res` / `p` (RFC 0006), so the "loop until the
  reader returns something other than the sentinel" idiom is clean.
- Analysis time on large functions and programs. A dead local's alias edges
  are dropped with its loans (RFC 0006, *Performance*); `AnalysisState::join`
  reports whether it changed instead of the engine copying and comparing
  states; `PlaceTable::descendants` walks the subtree instead of the table;
  `BorrowState` keeps its loans sorted; loan expiry runs only where a
  variable dies; the whole-program database is rebuilt only after a unit's
  exports changed. Lua's `lvm.c` went from 56 s to 3 s and the Lua program
  from 23 minutes to under 6. No diagnostic changed.

- `conflicting-borrow` no longer rejects two live borrows of one object or
  a write to a borrowed object by default (RFC 0006, *Conflict rules*):
  `int *a = &x; int *b = &x;`, `char *p = buf; snprintf(buf, ...); use(p);`
  and `int *pa = &s->a; s->a = 2; use(pa);` are accepted. Freeing or moving
  a borrowed object is still reported. `--exclusive-borrows` restores the
  RFC 0001 rule with the messages unchanged.
- `free(a[0]); use(a[1]);` and the loop-free idiom are no longer reported
  (RFC 0006, *Element witnesses*); `free(a[i]); use(a[i]);` still is.
- `if (free_if(p, c)) return; p[0] = 1;` is clean when `free_if` frees only
  on the paths returning non-zero (RFC 0006); untested, the may-free is
  still reported.
- `ValueSource::copy` from `strchr`-like table entries is now
  `interiorCopy`; `Store` values for `strto*` end pointers and `strtok_r`'s
  save pointer likewise.
- `SummaryFormatVersion` 1 → 2 and `SidecarFormatVersion` 1 → 2: version-1
  sidecars are rejected by their header and the objects re-analysed.
- `MoveTracker::markMoved`/`movedAt`/`reinitialize` take an
  `ElementWitness`; `AliasRelation::unite` takes exactness and witnesses;
  `FunctionSummary::reallocLike` and `CallEffects::isRealloc` are removed;
  `AnalysisState::reallocs` is replaced by `pending`.

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
