# Integration tests

End-to-end tests that run `weavec-cc` over small C programs and check its
output with [FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html),
driven by [lit](https://llvm.org/docs/CommandGuide/lit.html).

```
test/
  Guards/        the guard passes of RFC 0035 and RFC 0036: their IR
                 (FileCheck on -emit-llvm: inline checks, removal rules,
                 frames and shared frame space, globals, loop ranges) and
                 the programs they build, which must stop at the faulty
                 access with the runtime's report
  Driver/        command-line behaviour of weavec-cc (compile, link,
                 -fweavec-* flags, fortification, ASan's macros, the
                 runtime's link line and its fallbacks, one runtime per
                 process)
  Inputs/        shared headers/fixtures (not run as tests)
  cases/         the case tree (detection programs, determinism cases,
                 soundness probes, semantics), run by scripts/run-cases.py
                 rather than lit; see cases/README.md
  corpus/        the pinned third-party projects, run by
                 scripts/corpus-gate.py; see corpus/README.md
```

Run everything with `ctest -L integration` (or the `check-weavec-lit`
target), or a single test with `lit -v build/dev/test/Guards/heap-overflow.c`.

Each test is a `.c` file whose first lines contain `// RUN:` commands.
`%weavec_cc` is the compiler driver, which finds `weavec.h` itself;
`%resource_dir` is the directory that holds the header. Use `not
%weavec_cc ...` when a compile is expected to fail, `not --crash %t` when
a program must stop at a guard, and `... | count 0` to assert that nothing
was printed.

Unit tests for individual components live in `unittests/` instead
(`GuardPassTest` runs the passes on IR text). The runtime's own test is
`runtime/test/rt_test.c`, a C program that CTest runs as `runtime`
(`ctest -R '^runtime$'`); the executable cases of the runtime are under
`cases/semantics/runtime/`.

`cases/` and `corpus/` are the directories lit does not run. The case tree is
run by `ctest -R cases-` (or `python3 scripts/run-cases.py`), which checks
every marker in every case and reports per area; `test/cases/README.md` gives
the marker grammar.
