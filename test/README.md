# Integration tests

End-to-end tests that run the `weavec` binary over small C programs and check
its diagnostics with [FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html),
driven by [lit](https://llvm.org/docs/CommandGuide/lit.html).

```
test/
  Analysis/      checker behaviour (use-after-free, double-free, ...)
  Annotations/   the weavec.h macros and annotation handling
  Driver/        command-line behaviour of weavec and weavec-cc (compile,
                 link, sidecars, -fweavec-*/-W flags)
  WholeProgram/  several files analysed as one program (RFC 0005), with
                 their shared sources under WholeProgram/Inputs/
  Inputs/        shared headers/fixtures (not run as tests)
```

Run everything with `ninja check-weavec-lit` (or `ctest -L integration`), or a
single test with `lit -v build/dev/test/Analysis/use-after-free.c`.

Each test is a `.c` file whose first lines contain `// RUN:` commands. The
`%weavec` substitution expands to the built binary with the annotation header
directory already on the include path, so tests can `#include <weavec.h>`;
`%weavec_cc` is the compiler driver, which finds the header itself.
Use `not %weavec ...` when the run is expected to fail and `... | count 0`
to assert that nothing was printed.

Unit tests for individual components live in `unittests/` instead.
