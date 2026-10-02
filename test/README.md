# Integration tests

End-to-end tests that run the `weavec` binary over small C programs and check
its diagnostics with [FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html),
driven by [lit](https://llvm.org/docs/CommandGuide/lit.html).

```
test/
  Analysis/      checker behaviour (use-after-free, double-free, ...)
  Annotations/   the weavec.h macros and annotation handling
  Driver/        command-line behaviour of weavec and weavec-cc (compile,
                 link, unit records, -fweavec-*/-W flags); runtime-*.c pin
                 the runtime's flags, the guarded outcome in the ledger,
                 the link line and its fallbacks (RFC 0032)
  Emission/      the checks weavec-cc inserts: rewrite-oracle pairs (a source
                 and a hand-written Inputs/*.expected.c that must compile to
                 the same -O0 IR), and tests that build and run checked
                 programs; runtime-oracle-*.c are the pairs for the guards,
                 the range cache and stack and global object registration
  Prelude/       the RFC 0030 check prelude: compiled under every standard
                 and mode, and its helpers and runtimes run
  WholeProgram/  several files analysed as one program (RFC 0005), with
                 their shared sources under WholeProgram/Inputs/
  Inputs/        shared headers/fixtures (not run as tests)
  cases/         the RFC 0030 case tree (recall pins, evaluation programs,
                 soundness probes, semantics), run by scripts/run-cases.py
                 rather than lit; see cases/README.md
  corpus/        the pinned third-party projects, run by
                 scripts/corpus-gate.py; see corpus/README.md
```

Run everything with `ninja check-weavec-lit` (or `ctest -L integration`), or a
single test with `lit -v build/dev/test/Analysis/rfc0008-null.c`.

Each test is a `.c` file whose first lines contain `// RUN:` commands. The
`%weavec` substitution expands to the built binary with the annotation header
directory already on the include path, so tests can `#include <weavec.h>`;
`%weavec_cc` is the compiler driver, which finds the header itself.
Use `not %weavec ...` when the run is expected to fail and `... | count 0`
to assert that nothing was printed.

`%rewrite_oracle SOURCE EXPECTED WORKDIR [-- FLAGS]` (in `Emission/`) runs
`Emission/Inputs/rewrite-oracle.py`. Unless FLAGS name `-fweavec-runtime`,
it builds both sides with `-fno-weavec-runtime`, so that a test pins one
rewrite without guards or object registrations; the `runtime-oracle-*.c`
tests pass `-fweavec-runtime` and write those into their expected file.

Unit tests for individual components live in `unittests/` instead. The
runtime's own test is `runtime/test/rt_test.c`, a C program that CTest runs
as `runtime` (`ctest -R '^runtime$'`); the executable cases of the runtime
are under `cases/semantics/runtime/`.

`cases/` and `corpus/` are the directories lit does not run. The case tree is
run by `ctest -R cases-` (or `python3 scripts/run-cases.py --weavec
build/dev/bin/weavec`), which checks every marker in every case, the recall
pins (`// RECALL:`) among them, and reports per area; `test/cases/README.md`
gives the marker grammar.
