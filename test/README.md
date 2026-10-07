# Integration tests

End-to-end tests that run `weavec` and `weavec-cc` over small C programs and
check their output with [FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html),
driven by [lit](https://llvm.org/docs/CommandGuide/lit.html).

```
test/
  Guards/        the guard passes of RFC 0035: their IR (FileCheck on
                 -emit-llvm: inline checks, removal rules, frames, globals,
                 loop ranges) and the programs they build, which must stop
                 at the faulty access with the runtime's report
  Analysis/      the advisory analysis (use-after-free, double-free, ...)
  Annotations/   the weavec.h macros and annotation handling
  Driver/        command-line behaviour of weavec and weavec-cc (compile,
                 link, -fweavec-*/-W flags, the runtime's link line and its
                 fallbacks, one runtime per process)
  WholeProgram/  several files analysed as one program (RFC 0005), with
                 their shared sources under WholeProgram/Inputs/
  Inputs/        shared headers/fixtures (not run as tests)
  cases/         the case tree (recall pins, detection programs, soundness
                 probes, semantics), run by scripts/run-cases.py rather than
                 lit; see cases/README.md
  corpus/        the pinned third-party projects, run by
                 scripts/corpus-gate.py; see corpus/README.md
```

Run everything with `ctest -L integration` (or the `check-weavec-lit`
target), or a single test with `lit -v build/dev/test/Guards/heap-overflow.c`.

Each test is a `.c` file whose first lines contain `// RUN:` commands. The
`%weavec` substitution expands to the built binary with the annotation header
directory already on the include path, so tests can `#include <weavec.h>`;
`%weavec_cc` is the compiler driver, which finds the header itself.
Use `not %weavec ...` when the run is expected to fail, `not --crash %t`
when a program must stop at a guard, and `... | count 0` to assert that
nothing was printed.

Unit tests for individual components live in `unittests/` instead
(`GuardPassTest` runs the passes on IR text). The runtime's own test is
`runtime/test/rt_test.c`, a C program that CTest runs as `runtime`
(`ctest -R '^runtime$'`); the executable cases of the runtime are under
`cases/semantics/runtime/`.

`cases/` and `corpus/` are the directories lit does not run. The case tree is
run by `ctest -R cases-` (or `python3 scripts/run-cases.py`), which checks
every marker in every case and reports per area; `test/cases/README.md` gives
the marker grammar.
