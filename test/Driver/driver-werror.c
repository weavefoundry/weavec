// RFC 0034 §8: the driver's own diagnostics obey the -W options as Clang's
// do. CMake's check_c_compiler_flag adds -Werror=unused-command-line-argument
// and reads the exit status, so an unused argument must fail the compile.
//
// RUN: not %weavec_cc -Werror=unused-command-line-argument -Wl,-unused -c %s -o %t.o 2>&1 | FileCheck --check-prefix=ERROR %s
// RUN: %weavec_cc -Wl,-unused -c %s -o %t.o 2>&1 | FileCheck --check-prefix=WARN %s
// RUN: %weavec_cc -w -Wl,-unused -c %s -o %t.o 2>&1 | count 0
//
// ERROR: weavec-cc: error: -Wl,-unused: 'linker' input unused [-Werror,-Wunused-command-line-argument]
// WARN: weavec-cc: warning: -Wl,-unused: 'linker' input unused [-Wunused-command-line-argument]
int f(void) { return 0; }
