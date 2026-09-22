# Integration tests

End-to-end tests that run the `weavec` binary over small C programs and check
its diagnostics with [FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html),
driven by [lit](https://llvm.org/docs/CommandGuide/lit.html).

```
test/
  Analysis/      checker behaviour (use-after-free, double-free, ...)
  Annotations/   the weavec.h macros and annotation handling
  Driver/        command-line behaviour of weavec and weavec-cc (compile,
                 link, unit records, -fweavec-*/-W flags)
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

Unit tests for individual components live in `unittests/` instead.

`cases/` and `corpus/` are the directories lit does not run. The case tree is
run by `ctest -R cases-` (or `python3 scripts/run-cases.py --weavec
build/dev/bin/weavec`), which checks every marker in every case, the recall
pins (`// RECALL:`) among them, and reports per area; `test/cases/README.md`
gives the marker grammar.
