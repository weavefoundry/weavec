# RFC 0005: Whole-program analysis: cross-translation-unit summaries and the compiler driver

- **Status**: Implemented
- **Authors**: WeaveC authors
- **Created**: 2026-09-02
- **Accepted**: 2026-09-02
- **Implemented**: 2026-09-02
- **Tracking issue**: TBD
- **Supersedes / superseded by**: supersedes RFC 0003 *`annotation-required`,
  revised* item 1 (the boundary moves from the translation unit to the
  program) and the *Cross-TU inference* row of RFC 0003 *Soundness*;
  resolves RFC 0001's open question *Inference across translation units*;
  narrows RFC 0004 *Callbacks registered from another translation unit* to
  code outside the program; records the struct-copy rule RFC 0002 left
  implicit.

## Implementation notes

The design below landed as written except where this section says
otherwise. Each item clarifies *how* a decision was built, not the decision;
where the text below disagrees with an item here, the item wins. Behaviour
is pinned by `test/WholeProgram/rfc0005-*.c`, `test/Driver/rfc0005-*.c`,
`unittests/Core/SummaryIOTest.cpp`, `unittests/Analysis/ProgramDatabaseTest.cpp`,
`unittests/Analysis/DataflowTest.cpp` (struct copies) and
`unittests/Frontend/{ProgramAnalysis,Sidecar,DiagnosticControl}Test.cpp`.

- **Exports carry the boundary sets, not just `unknown`.** `UnitExports`
  has `imports` (external callees with no body here), `indirectTypes`
  (indirect calls with no signature from the pointer type), and the two
  sets the compile step deferred: `unknownCallees` and
  `unknownIndirectTypes`. The unit graph uses the first two; the link step
  uses the last two to decide that a unit must be re-analysed even when no
  other unit defines anything it imports (its deferred warnings still have
  to be emitted or retracted). Clang builtins (`__builtin_*`) are never
  imports.
- **Discovery is a real parse without analysis.** `TranslationUnitAnalyzer::
  discover()` collects definitions, address-taken functions, external
  callees and indirect type keys from the call graph and returns exports
  with empty summaries; `exports()` after `run()` fills the summaries in.
  In driver mode the sidecar *is* the discovery result, so the link step
  parses a unit only when it analyses it.
- **The database is per unit, built from `settled` plus the component.**
  `ProgramAnalysis` keeps one `ProgramDatabase` of everything analysed so
  far (`settled`); for a cyclic component it builds a temporary database
  from `settled` plus the members' current exports each round.
  `ProgramDatabase::add` joins duplicate names and duplicate candidates of
  one type key into one summary each (the RFC's `candidates : Map<TypeKey,
  [FunctionSummary]>` is stored pre-joined). `importInto` remaps a
  summary's global roots by name into a unit's `GlobalTable`, dropping
  those the unit does not declare `extern`.
- **The program tier is consulted through `SummaryStore::programSummaryFor`.**
  It returns a summary for an external-linkage callee with no body here
  that the database defines. `isAddressTaken` and `lookupIndirect` consult
  the database's candidates by `functionTypeKey`, which spells the function
  type with a fixed `PrintingPolicy` (anonymous records by source
  location, so they never match across units, as designed).
- **Boundary deduplication is by message text.** A shared
  `std::set<std::string>` (`FrontendOptions::boundaryOnce`, owned by
  `ProgramAnalysis`) records every `annotation-required` message emitted
  in the program; `FilteringSink` drops repeats. Per program, one warning
  per callee and one per function-pointer type, as specified; the key
  happens to be the text rather than the name.
- **`reported` is a set of `(id, line, column, file)`.** `FilteringSink`
  drops a diagnostic whose primary location and id are already in the
  unit's `reported` set (`FrontendOptions::alreadyReported`, filled from
  the sidecar at link time). Notes attached to a dropped diagnostic are
  dropped with it.
- **Stale sidecars are detected by mtime, not a recorded size.** The link
  step ignores, with `weavec-cc: warning: ignoring '<sidecar>': older than
  '<object>'`, a sidecar whose modification time is older than its object's
  (RFC text: "records the object's size and mtime"). The object then stands
  as compiled with no exports, i.e. as unknown code; linking proceeds.
- **The link step's "needs analysis" test is as written**, plus the
  deferred-boundary case above. A unit that needs analysis but whose
  sidecar carries no command (a sidecar written by hand or by a future
  plugin) contributes its exports and is not re-analysed.
- **`-fweavec-*` and `-W*weavec*` reach `-cc1` as `-Xclang` arguments.**
  The driver strips them from Clang's command line and re-attaches each as
  `-Xclang <flag>` so they appear in every `-cc1` job (and therefore in
  the sidecar's recorded command); the cc1 step strips them again before
  `CompilerInvocation`. `-D__WEAVEC__=1` and `-isystem <resource
  include>` are added only when analysis is enabled. `-cc1as` and other
  jobs this build cannot run in-process are delegated to a `clang` binary:
  `$WEAVEC_CLANG`, else the one the build was configured with
  (`WEAVEC_CLANG_EXECUTABLE`), else `clang` on `PATH`.
- **`--dump-analysis` spells the database with summary paths.** The
  database has no parameter names, so the `program:` section prints
  `function 'node_free': param 0: freed; param 0 *.name: freed; stores{}
  returns{}` (the RFC's `n: freed` example is the per-unit dump's
  spelling). Candidates print as `candidate '<type key>': ...`.
- **`-Wno-weavec-<id>` on an error is rejected**, not silently ignored:
  `'-Wno-weavec-use-after-free': 'use-after-free' is an error and cannot
  be disabled; use -Wno-error=weavec-use-after-free to make it a warning`.
  `-W...weavec-<id>` with an unknown id is an error too. `-Wno-weavec` (the
  group) disables the warnings and leaves the errors alone. `-W` flags that
  are not WeaveC's are Clang's business.
- **Sidecars of temporary objects** (`weavec-cc a.c b.c -o prog`) are
  written by the compile jobs like any other, read by the link step, and
  removed by the driver with the temporaries.
- **Failures short of a diagnostic are `Result` fields.** A unit that
  cannot be parsed or re-run is listed in `Result::failed` (the rest of
  the program is still analysed); a cyclic component that does not settle
  within `MaxRounds = 8` is listed in `nonConverging` and its members are
  reported against the last round. Both fail the link and `weavec
  --whole-program`'s exit status.

## Summary

Lift RFC 0003's algorithm one level: the unit of the bottom-up fixpoint
becomes the translation unit and the object analysed becomes the *program*,
the set of translation units built together. Every translation unit exports
the summaries of its externally visible definitions and of every function
whose address is taken; a *program database* collects them and the
`SummaryStore` consults it, after the unit's own inferences and before the
shipped library table, so `other_free(o); o->v` is a `use-after-free` when
`other_free` lives in another file, and a callback installed from another
file is joined into the signature of the function-pointer type it is stored
in. `annotation-required` fires only for code defined nowhere in the
program. The `FunctionSummary` gains a stable text form, Clang-free and
unit-tested in Core, which is both the on-disk format and the debugging aid.
Two ways to run it: `weavec --whole-program` analyses every unit of a
compilation database together; `weavec-cc`, a drop-in C compiler built on
Clang's driver, compiles each file with Clang, analyses it as it goes,
writes its exports to a sidecar next to the object, and at link time runs
the whole-program analysis over the sidecars of the objects being linked,
re-analysing the units whose diagnostics depend on other units. Compiler-
style flags (`-fweavec-strict`, `-fno-weavec`, `-Wno-error=weavec-<id>`, ...)
control it. Finally, a whole-struct copy now copies the facts of every
pointer field, closing the last soundness hole RFC 0004 listed.

## Motivation

RFC 0001 promised a guarantee for "a translation unit with no
`WEAVEC_UNSAFE` regions and no `annotation-required` diagnostics". Real
programs have no translation unit that satisfies the second condition: every
`.c` file calls functions defined in other `.c` files of the same project,
and RFC 0003 made each such call a *boundary*: borrowed for the call,
nothing retained, result unknown, one warning per callee.

```c
/* list.c */                          /* main.c */
struct node *list_new(int v);         struct node *n = list_new(1);
void list_free(struct node *n);       list_free(n);
                                      return n->v;   /* not reported today  */
```

The tool reports nothing at `n->v`: `list_free` is a warning, its effect is
lost, and the warning is the only sign that the whole file is being checked
against an optimistic model of its neighbours. On the corpus every remaining
`annotation-required` (five sites across six projects) is a callback
installed by code in another translation unit, which RFC 0004 could only
name as "the single-TU limitation Milestone 4 addresses". The corpus itself
is made of single-file libraries because the tool cannot do otherwise; the
question the RFCs keep deferring to it, "what does real code look like",
cannot be answered with the tool as it stands.

Separately, `weavec` is a libTooling analyser: `weavec file.c -- -I..`. It
does not compile anything, cannot be set as `CC=` in a Makefile, and has no
compiler-style flags. The README describes a compiler, and the roadmap has
had "drop-in `cc`" as Milestone 3 since the scaffold. Teams adopt a checker
by putting it in the build; a tool they must run separately, file by file,
with hand-copied include flags, is not something a codebase can be made
safe *incrementally* with.

The two are one piece of work because the compiler is where the program is
known. A build compiles every unit and then links them; the link is the one
point at which "the set of translation units built together" exists, and it
is exactly the point at which summaries from every unit are available. A
summary database that lives anywhere else has to guess what the program is.

## Soundness

**The guarantee, restated.** RFC 0001's soundness statement now reads:

> In a *program* with no `WEAVEC_UNSAFE` regions and no
> `annotation-required` diagnostics, WeaveC reports every use of a place
> after its owned resource was released or moved, every double release,
> every violation of the aliasing rules, every borrow that may outlive its
> referent, and every raw operation, subject to the assumptions listed.

where a *program* is the set of translation units analysed together (the
units of a compilation database under `--whole-program`, or the objects
handed to one `weavec-cc` link). The statement for a single translation unit
is the special case of a one-unit program, so nothing accepted under RFCs
0002–0004 becomes rejected.

**Bugs caught after this RFC**, in addition to RFC 0002–0004's, when the
units are analysed together:

```c
/* node.c */
struct node *node_new(int v) { struct node *n = malloc(sizeof *n); n->v = v; return n; }
void node_free(struct node *n) { free(n); }
char *node_name(struct node *n) { return n->name; }         /* borrow of *n     */
static void drop_impl(void *p) { free(p); }
void (*drop_hook)(void *) = drop_impl;                      /* address taken    */

/* main.c */
struct node *node_new(int v); void node_free(struct node *n);
char *node_name(struct node *n); extern void (*drop_hook)(void *);

int uaf(void) {
  struct node *n = node_new(1);
  node_free(n);
  return n->v;                  /* error: use of 'n' after it was freed     */
}
int double_free(void) {
  struct node *n = node_new(1);
  node_free(n);
  free(n);                      /* error: 'n' is freed twice                */
}
char *escape(void) {
  struct node *n = node_new(1);
  char *s = node_name(n);       /* s borrows *n, through the summary        */
  node_free(n);
  return s;                     /* error: use of 's' after it was freed     */
}
int hook(void) {
  char *p = malloc(8);
  drop_hook(p);                 /* joins drop_impl, defined in node.c       */
  return p[0];                  /* error: use of 'p' after it was freed     */
}
```

and, in any mode, the struct-copy hole:

```c
struct buf { char *data; };
int copy(void) {
  struct buf a = { malloc(8) };
  struct buf b = a;             /* b.data aliases a.data                    */
  free(a.data);
  return b.data[0];             /* error: use of 'b.data' after it was freed */
}
```

**Deliberately not caught:**

- **Code outside the program.** Objects linked without a sidecar (a vendored
  `.a`, a system library, an object built by another compiler) are unknown
  code: their functions are boundaries exactly as RFC 0003 defines, with
  the library table and annotations on their headers as the only source of
  summaries. `annotation-required` still fires for them, so the gap is
  visible; `--strict-externs`/`-fweavec-strict` makes it an error.
- **Link-time interposition.** `-Wl,--wrap`, `LD_PRELOAD`, weak symbols
  overridden at link and `dlsym` are invisible: the definition analysed is
  the one in the program. RFC 0003 made the same assumption within a unit.
- **Function pointers that leave the program.** A callback handed to a
  library that calls it is checked as a definition (RFC 0003) but nothing
  checks the library's call. A function pointer *received* from outside
  the program with no annotation on its type is a boundary (RFC 0004).
- **Units not in the program.** `--whole-program` over a compilation
  database analyses what the database lists; a file the build system
  compiles some other way is outside the program, and calls into it warn.

**Accepted false positives**, beyond earlier RFCs':

- **Duplicate definitions are joined.** A compilation database often builds
  several executables (the library, its tests, its tools); two of them may
  define the same external name differently, which no single link would
  accept. The database *joins* the summaries of every definition of a name
  (RFC 0003's summary join, the may-summary of "whichever is linked"), so a
  caller sees the union of their effects. This is sound for every possible
  link and imprecise for each; `weavec-cc` never meets the case because a
  link set has one definition per name. `main` is never exported.
- **Unmatched function types.** Address-taken functions are matched to
  indirect calls in other units by the spelling of their canonical type. A
  type spelled with an anonymous struct or union (`struct { int a; } *`)
  has no stable spelling across units and never matches; the call stays a
  boundary (warned), as it is today.
- **Static globals in exported summaries.** An exported function that frees
  or stores into a `static` global cannot name it to another unit; the
  effect is dropped on export and a value copied from such a global becomes
  `unknown`. A caller in another unit could not have named the place
  either, so nothing checkable is lost.

**Assumptions.** RFC 0001–0004's, plus: C linkage names are the identity of
a function across units (no `asm("label")` renames; `static` functions are
private to their unit); the objects linked are the objects whose sidecars
are read (a build that rewrites objects after compiling them defeats the
sidecar, as it would defeat debug info); when `weavec-cc` re-analyses a unit
at link time, the recorded compiler command still describes it (the source
and headers have not changed since the object was built, which is what the
build system already guarantees for the object itself).

## Detailed design

### Programs, units and exports

A **unit** is one translation unit with its compiler command line. A
**program** is a set of units analysed together. Each unit produces
**exports**:

```
UnitExports = { source     : path
              , command    : [string]                -- how to re-run (driver mode)
              , functions  : Map<Name, ExportedFunction>
              , imports    : Set<Name>               -- external callees with no definition here
              , indirects  : Set<TypeKey>            -- indirect calls with no local signature
              , unknown    : Set<Name>               -- callees warned about (compile step)
              , reported   : Set<(id, file, line, column)>   -- driver mode, see below
              }
ExportedFunction = { summary : FunctionSummary, typeKey : TypeKey
                   , linkage : External | Internal, addressTaken : bool }
```

A function is exported if it has a body in the unit and either external
linkage (a caller in another unit may name it) or its address is taken (a
caller in another unit may reach it through a pointer of its type). `main`
is not exported. `TypeKey` is the canonical function type spelled with a
fixed printing policy (C, no `struct` keyword elision, no typedef names);
two units spell the same type the same way unless it involves an anonymous
record.

Exports are collected by `TranslationUnitAnalyzer` after its final pass:
the summary is the one `SummaryStore::lookup` would hand a caller *in this
unit* (annotations applied, RFC 0003 order), so a caller elsewhere sees
exactly what a caller here sees. A `WEAVEC_UNSAFE` declaration without a
body is not exported (nothing to export); an annotated declaration without a
body is not exported either, because the importing unit sees the same header
and applies the same annotations itself.

### The summary text format (`weavec::Core`)

`FunctionSummary` gains a text form, written and read by
`include/weavec/Core/SummaryIO.h` (Clang-free; global roots are spelled
through callbacks that the Analysis layer supplies with names). One summary
is one line-oriented record:

```
summary
  effect <path> <flag>[,<flag>]*        -- flags: read written freed moved
  store <path> <source>
  return <source>
  realloc-like
end
```

with

```
path   ::= param <i> <steps> | global <name> <steps>
steps  ::= ( '*' | '.' <field> | '[]' )*
source ::= fresh | null | unknown | raw | copy <path> | borrow <path>
```

Fields and global names are written as-is; C identifiers need no quoting.
Unknown lines are skipped (forward compatibility within a major format
version); a malformed line fails the record. Round-trip (`parse(print(s)) ==
s`) is a unit-tested invariant. This is the format RFC 0003 said
`FunctionSummary` was designed to be; it is stable and versioned by the
`weavec-summaries <version>` header of the files that carry it.

### The program database (`weavec::Analysis`)

`ProgramDatabase` holds the exports of every unit of a program except the
one being analysed:

- `functions : Map<Name, FunctionSummary>` for external-linkage exports,
  joined over units (see *Accepted false positives*);
- `candidates : Map<TypeKey, [FunctionSummary]>` for address-taken exports
  of any linkage;
- `defined : Set<Name>` of every external-linkage definition, used to tell
  "defined elsewhere" from "defined nowhere".

Summaries in the database name globals by *name*. `ProgramDatabase::import`
remaps a summary into a unit: for each `global <name>` root the unit's
translation-unit declarations are searched for an external-linkage variable
of that name (`extern char *g;` in a shared header is the normal case); if
found it is interned in the unit's `GlobalTable`, otherwise every effect on
that root is dropped and every `copy`/`borrow` of it becomes `unknown`.
Static globals are dropped at *export*, by the same rule, since their name
means nothing elsewhere.

`SummaryStore` gains an optional database and a new `SummarySource::Program`.
The lookup order of RFC 0003 becomes:

1. annotations on the declaration (authoritative per root, unchanged);
2. the summary inferred from a body in **this** unit;
3. **the program database**, for a callee with external linkage and a
   definition in another unit;
4. the shipped library table;
5. otherwise a boundary.

Step 3 sits above the table so a program that defines its own `strdup` is
checked against its own definition, which RFC 0002 listed as a known
limitation of name matching. A partly annotated declaration still applies
its annotations root by root on top of whatever step 2–4 produced.

`lookupIndirect` (RFC 0004) joins the summaries of the unit's own
address-taken candidates of the type **and** the database's candidates for
the same `TypeKey`; annotations on the function-pointer type still apply
root by root afterwards. A call with neither local nor program candidates
and no annotations remains a boundary.

### The whole-program algorithm (`weavec::Frontend`)

`ProgramAnalysis` runs a program given a way to parse each unit and run a
frontend action over it (a compilation database in tooling mode, recorded
cc1 commands in driver mode):

1. **Discovery.** Parse each unit once, without analysis, to collect its
   external-linkage definitions, its address-taken functions with their
   type keys, its external callees and its indirect-call type keys.
2. **Unit graph.** Unit *A* depends on unit *B* if *A* imports a name *B*
   defines, or *A* has an indirect call of a type key for which *B* exports
   a candidate. Tarjan's algorithm over units gives the components in
   reverse topological order, dependencies first (the RFC 0003 machinery,
   reused).
3. **Analysis.** A component of one unit with no self-dependency is analysed
   once, with reporting on, against a database holding the exports of every
   unit already analysed. A component with a cycle (mutual recursion across
   files, or two files that each call into the other) starts every member
   at the bottom (no exports), analyses all members with reporting off
   until no member's exports change (cap: 8 rounds), then analyses each
   member once more with reporting on against the fixpoint. Monotonicity is
   RFC 0003's argument: exports only grow, and a larger callee summary
   yields a larger exit state.
4. **Boundary report.** `annotation-required` is emitted during a unit's
   reporting pass for callees the database does not define (see
   *Diagnostics*).

Each analysis of a unit is exactly the RFC 0003 translation-unit analysis
with the database attached; nothing inside a unit changes. A unit is
therefore parsed once for discovery and once per analysis round; the cost
model is in *Performance*.

### `weavec --whole-program` (tooling mode)

`weavec --whole-program -p build/ [files...]` runs the algorithm over the
listed sources (all sources of the compilation database when none are
given), using `ClangTool` to parse each. Other flags keep their meaning:
`--strict-externs` makes every remaining boundary a raw operation,
`--report-unannotated` reports the exported surface of every unit,
`--dump-analysis` prints each unit's dump in analysis order and, at the end,
the database (`program:` followed by one summary record per exported name).
Without `--whole-program`, `weavec` behaves exactly as before: each file is
its own program.

### `weavec-cc` (driver mode)

`weavec-cc` is a C compiler: `weavec-cc -c foo.c -o foo.o -Iinclude`,
`weavec-cc foo.o bar.o -o prog`, `CC=weavec-cc make`. It is Clang's driver
with WeaveC inside:

- **Driver.** The command line, minus the `-fweavec-*` and `-W*weavec*`
  flags below, is handed to `clang::driver::Driver`, which plans the same
  jobs `clang` would. WeaveC's flags are appended to every `-cc1` job. The
  resource directory, target, SDK and linker are Clang's own.
- **Compile step.** A `-cc1` job runs in-process. The frontend action Clang
  would run (emit object, emit assembly, emit LLVM, syntax only, ...) is
  wrapped so that its AST consumer is multiplexed with WeaveC's: one parse,
  one AST, Clang's code generation and WeaveC's analysis. WeaveC's
  diagnostics go through the same `DiagnosticsEngine` as Clang's, so an
  error fails the compile and removes the output, as any compile error
  does. The unit's exports are written to `<output>.weavec` (`foo.o.weavec`
  next to `foo.o`), including the cc1 command line; with `-fsyntax-only` or
  `-E` nothing is written. The compile step analyses the unit **alone**,
  as `weavec file.c` does today, with one difference: `annotation-required`
  for callees with no definition here is *deferred*, recorded in the
  sidecar (`unknown`) rather than emitted, because only the link knows
  whether the callee is defined in the program. Every other diagnostic is
  emitted as usual and recorded in the sidecar (`reported`).
- **Link step.** Before running the linker job, the driver reads the
  sidecar of every object on the link line (objects with no sidecar are
  unknown code) and runs the whole-program algorithm over them, re-parsing
  a unit from its recorded cc1 command when it must be analysed. A unit
  needs analysis at link time if it imports a name some other sidecar
  defines, has an indirect call of a type some other sidecar has a
  candidate for, or deferred at least one boundary warning; a unit that
  does none of these stands as compiled. Diagnostics from a link-time
  re-analysis that were already emitted by the compile step (same id and
  location, per `reported`) are not printed again, so a bug inside one file
  is reported once, when the file is compiled, and a bug that needs two
  files is reported once, when they are linked. Deferred boundary warnings
  for callees the program still does not define are emitted at this point,
  once per callee. Errors fail the link (the linker is not run and the
  driver exits non-zero). Sidecars of temporary objects (`weavec-cc a.c b.c
  -o prog` in one step) are read before the driver deletes them.
- **Flags.** `-fweavec` (default) / `-fno-weavec` (compile only, no
  analysis, no sidecar); `-fweavec-strict` (`--strict-externs`);
  `-fweavec-report-unannotated`; `-fweavec-analyze-headers`;
  `-fweavec-dump-analysis`; `-fno-weavec-link` (skip the link step).
  Warning control, also accepted by `weavec`: `-Wno-weavec-<id>` disables a
  diagnostic whose default severity is *warning* (`annotation-required`,
  `invalid-annotation`); `-Wweavec-<id>` re-enables it;
  `-Wno-error=weavec-<id>` lowers an error to a warning and
  `-Werror=weavec-<id>` raises a warning to an error; `-Wno-error=weavec`
  and `-Werror=weavec` do the same for every WeaveC id. An error cannot be
  disabled outright (RFC 0004: `-Wno-weavec-unsafe-operation` should not
  exist); lowering it to a warning is the migration path for a codebase
  that wants to build while it works through the reports. The guarantee
  assumes default severities.

The compile step's view of a unit is the RFC 0003 single-unit view, so its
diagnostics are what `weavec file.c` reports today; the link step's view is
the whole-program view. Because an unknown callee is modelled optimistically
(borrow, retain nothing, unknown result), the compile step reports a subset
of the link step's reports in practice, and a report the link step
*retracts* (a summary that removed a conflict) is not attempted: the
compile-time report stands and the user resolves it as today.

### Struct copies

A whole-struct assignment or initialisation `b = a` (both of record type)
is the sequence of pointer copies `b.f = a.f` for every pointer-typed field
path `f` the analysis has a place for under `a`, recursively through nested
records and array summaries: `b.f` aliases `a.f`, carries its loans and its
move and raw records, and has its kind. Fields under `a` that have no place
yet are not copied (nothing is known about them; `b.f` is forgotten as
before). This replaces the RFC 0002 implementation's "forget every field of
`b`", which made `struct buf b = a; free(a.data); b.data[0]` silent. Struct
parameters and returns by value already go through this rule via the
initialisation of the receiving place. RFC 0004's *Accepted false
positives* item "struct copies drop raw records" is thereby withdrawn: they
copy them.

### Layering

New in Core: `SummaryIO.h`/`.cpp` (`printSummary`, `parseSummary` with
global-name callbacks, `SummaryFormatVersion`). Clang-free, unit-tested,
round-trip pinned.

New in Analysis: `ProgramDatabase.h`/`.cpp` (exports, import with global
remapping, candidates by type key, `functionTypeKey`);
`SummaryStore::setDatabase`, `SummarySource::Program`, the program tier in
`lookup`/`lookupIndirect`; `TranslationUnitAnalyzer::exports()` and the
unit's imports/indirect type keys; `AnalysisOptions::deferBoundary` (driver
compile step); the struct-copy rule in `FunctionDataflow::handleAssign` and
`handleDecl`.

New in Frontend: `ProgramAnalysis.h`/`.cpp` (discovery, unit graph, SCCs,
rounds, over an abstract unit runner); `Sidecar.h`/`.cpp` (the
`weavec-summaries 1` file: header, unit exports, imports, reported
diagnostics); `DiagnosticControl.h`/`.cpp` (`-W` spellings applied to
`core::Diagnostic` before `ClangDiagnosticSink`); `Driver.h`/`.cpp` (the
Clang-driver wrapper, cc1 wrapping, link step); `FrontendOptions` gains the
database, the exports receiver and the control table.

Tools: `tools/weavec` gains `--whole-program` and `-W...`; `tools/weavec-cc`
is new and thin (argument split, `Driver::run`).

### Debug output

`--dump-analysis` in whole-program mode prints each unit's dump as today,
prefixed with `unit '<source>':`, in analysis order, then:

```
program:
  function 'node_free': n: freed
  function 'node_new': returns{fresh}
  candidate 'void (void *)': p: freed
```

one line per exported name in the RFC 0003 summary spelling, then one per
type key with candidates. The sidecar's own text is the stable form; the
dump remains a debugging aid pinned only by lit tests.

### Performance

Tooling mode parses each unit once for discovery and once per analysis
round: two parses for a program with no cross-unit cycles. Driver mode adds,
at link time, one parse per unit that depends on another unit or deferred a
boundary warning (in practice most units, on a first build) and one per
round for cyclic groups. Parsing dominates analysis by an order of magnitude
on the corpus, so the link step of a *N*-unit program costs roughly the
analysis-only compile of *N* files. Caching serialised ASTs to avoid the
re-parse is future work; `scripts/corpus.py` records analysis time per
program as before and gains a whole-program column.

## Annotation surface

None. Annotations on a declaration in a shared header continue to be
authoritative in every unit that includes it (RFC 0003), and are now
checked against the definition in the unit that has it *and* used by every
other unit.

## Diagnostics

No new identifiers.

| Id                    | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `annotation-required` | The direct-call form fires only for a callee with no definition anywhere in the program (previously: in the translation unit). Message unchanged: `call to '<f>' is not checked: it has no definition or ownership annotations here`; the second note now reads `annotate its pointer parameters with WEAVEC_OWNED, WEAVEC_BORROWED, WEAVEC_MUT or WEAVEC_RAW, or define it in this program`. The indirect form fires only when no unit of the program takes the address of a function of the type; its wording changes from `in this translation unit` to `in this program`. In driver mode both forms are emitted at link time, once per callee or type per program. |
| `use-after-free`, `double-free`, `use-after-move`, `conflicting-borrow`, `lifetime-too-short`, `unsafe-operation`, `annotation-mismatch` | Unchanged wording. Now also emitted for effects that come from a callee defined in another unit, from a callback whose address is taken in another unit, and for facts carried through a struct copy. |

Tool-level failures are not diagnostics and have no id: a sidecar with an
unreadable format version (`weavec-cc: warning: ignoring 'foo.o.weavec':
unsupported format 2`), a unit that cannot be re-analysed because its
recorded command no longer runs, and a program whose cyclic component does
not converge within the round cap (`error: whole-program analysis of
'a.c', 'b.c' did not converge`) are printed by the driver and, for the last
two, fail the link.

## Drawbacks

- **Two parses per unit.** Discovery and analysis are separate parses in
  tooling mode; driver mode re-parses at link time what it already parsed
  when compiling. Serialising ASTs would fix it and is deferred (see
  *Alternatives*) because correctness and simplicity come first and the
  cost is bounded by the compile itself.
- **Sidecars are build artefacts the build system does not know about.**
  `make clean` rules that delete `*.o` leave `*.o.weavec` behind; a stale
  sidecar next to a fresh object built by another compiler is read as
  truth. Mitigation: the sidecar records the object's size and mtime and is
  ignored with a warning when they disagree.
- **Duplicate-definition joins** (see *Soundness*) make whole-program mode
  over a compilation database with several executables less precise than
  linking each one. `weavec-cc` has no such problem; tooling users can pass
  the files of one executable.
- **The compile step is optimistic.** A unit's compile-time diagnostics are
  computed without the program; the definitive report is the link. Users
  who only ever run `-c` see the RFC 0003 view, which is what they see
  today.
- **Three modes to keep consistent.** Single-unit tooling, whole-program
  tooling and the driver share every analysis component and differ only in
  how units are enumerated and parsed; the lit tests run the same programs
  through all three.

## Alternatives

- **A shared summary directory populated at compile time**
  (`-fweavec-db=DIR`, each compile reading and writing it). Rejected:
  parallel builds write it concurrently, the compile order determines what
  each unit sees, and there is no moment at which the database is known to
  be complete, so results depend on `-j`. Sidecars plus a link step have
  neither problem: every compile is independent and the link is the
  synchronisation point the build already has.
- **Analyse only at link time** (compile step writes exports and reports
  nothing). Simpler and never redundant, but `weavec-cc -c foo.c` would
  report nothing, which is not how a compiler behaves, and a project that
  never links through `weavec-cc` (a library) would get no reports at all.
  Reporting at both points with deduplication keeps compiler behaviour and
  adds the program view.
- **Serialised ASTs (`-emit-ast`) in the sidecar** to avoid re-parsing.
  Faster link step, much larger sidecars (megabytes per unit), and an ABI
  tie to the exact Clang version. Deferred until the corpus shows the
  re-parse matters.
- **Iterate every unit to a global fixpoint** (round-robin, no unit graph).
  Simpler, but parses every unit once per round for the whole program even
  when one small cycle is the only thing changing. The unit graph confines
  iteration to the units that need it, at the cost of one Tarjan over
  names, which RFC 0003 already has.
- **Key by mangled or type-qualified names.** C has one namespace per
  linkage name; a prototype mismatch between units is a C bug WeaveC is not
  the tool for. The type key is used only for indirect candidates, where
  the type *is* the identity.
- **Make `weavec` itself the compiler** (driver mode when no `--`). Rejected:
  `weavec foo.c` is ambiguous between "analyse with the compilation
  database" and "compile to `a.out`"; two binaries with one meaning each
  are easier to document and to put in a Makefile.
- **A Clang plugin instead of a driver.** `-fplugin=` runs in a stock
  `clang` and needs no `CC=` change, but a plugin cannot add a link step,
  so it gives the compile-step view only. Deferred to future work as a
  packaging of the same action; the driver is the design.
- **Do nothing.** The soundness statement remains true of no real program,
  and the corpus cannot grow.

## Prior art

- **Infer** (Calcagno et al.): *capture* (compile every file, saving the
  IR) then *analyse* (bottom-up over the global call graph with on-disk
  per-procedure summaries). Our compile step is capture, our link step is
  analyse, and our sidecar is the per-file capture output; Infer's
  experience that the capture must be *inside the build* (via `infer --
  make`) is why this RFC builds a driver rather than a database tool.
- **LTO / ThinLTO** (LLVM): per-object summaries written next to the object
  and combined at link time, with the link as the only whole-program point.
  ThinLTO's module summaries are the structural model for the sidecar; its
  "import the summaries you need, analyse locally" is our
  `ProgramDatabase::import`.
- **Clang's cross-translation-unit analysis** (`clang-extdef-mapping`, CTU
  in the static analyzer): a pre-pass maps external definitions to the file
  that has them, then the analysis loads those ASTs on demand. Our
  discovery pass is the mapping step; we import summaries rather than ASTs
  because RFC 0003 already committed to summaries.
- **Rust crate metadata** (`.rlib` `rmeta`): a crate's compiled interface,
  including the borrow-relevant signatures, is written next to its object
  and read by dependents, with a build system (`cargo`) that orders crates
  by dependency. Our unit graph plays `cargo`'s role for a C build that has
  no such ordering.
- **GCC LTO's `-flto` and `lto-wrapper`**: the driver, not the compiler,
  owns the whole-program step and re-invokes the compiler on saved state;
  our link step re-invoking `cc1` from the recorded command is the same
  shape without the saved IR.
- **CodeQL / Frama-C**: whole-program databases built by intercepting the
  build (`codeql database create -- make`). Confirms that intercepting the
  compiler is the adoption path; we intercept by *being* it.

## Unresolved questions

**Resolved at acceptance** (decisions recorded inline above):

| Question                                     | Decision                                                                                                    |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| Where summaries live                         | Sidecar per object (`<output>.weavec`); combined at link. No shared directory.                                |
| On-disk format                               | Line-oriented text, Core-defined, versioned; globals by name; round-trip tested.                             |
| Identity of a function across units          | Linkage name for direct calls; canonical type spelling for indirect candidates.                             |
| Two units define one name                    | Join (sound for any link); never arises in a single link.                                                   |
| Position of the program tier                 | Above the library table, below this unit's own inference and annotations.                                   |
| Order of analysis                            | Tarjan over units; cycles iterated to a fixpoint (cap 8) then reported.                                     |
| What the compile step reports                | Everything the single-unit view reports, except boundary warnings, which the link step decides.             |
| Duplicate reports between compile and link   | Deduplicated by id and location through the sidecar.                                                        |
| `-W` control                                 | Warnings can be disabled; errors can be lowered to warnings, not disabled.                                  |
| One binary or two                            | `weavec` (tooling) and `weavec-cc` (compiler).                                                              |
| Struct copies                                | Field-wise pointer copies.                                                                                  |

**Deferred to corpus testing:**

- Whether the link-step re-parse is a noticeable fraction of build time on
  multi-hundred-unit projects, which decides whether AST caching is worth
  its sidecar size.
- How often a compilation database mixes executables enough for the
  duplicate-definition join to lose precision.
- Whether anonymous-record function types (unmatched type keys) occur in
  real hook tables.

## Future work

- **Clang plugin packaging** (`-fplugin=libweavecPlugin`): the compile step
  inside a stock `clang`, writing the same sidecar; `weavec --link-check
  *.o` as the link step for builds that cannot change `CC`.
- **AST caching** in the sidecar to make the link step a load rather than a
  parse.
- **Shipped summaries for common libraries** (Milestone 4): the sidecar
  format is the distribution format; a `libz.weavec` next to `libz.a` would
  be read like any other sidecar.
- **Incremental link steps**: with sidecars recording what each unit
  imported and the summary it saw, a link can re-analyse only the units
  whose imports changed since the last link.
- **Non-lexical loans** and **conditional summaries** (RFC 0002/0003 future
  work), now measurable on multi-file programs.
