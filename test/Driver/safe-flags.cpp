// RFC 0051 §4.5: a compile with -fweavec-safe refuses the flags that let
// the optimizer assume what a safe unit does not check; a later flag that
// takes one back is accepted, and so is the flag without -fweavec-safe.
// RUN: not %weavec_cxx -fweavec-safe -ffast-math -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=FAST
// RUN: not %weavec_cxx -fweavec-safe -Ofast -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=OFAST
// RUN: not %weavec_cxx -fweavec-safe -fstrict-enums -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=ENUMS
// RUN: not %weavec_cxx -fweavec-safe -fno-wrapv -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=WRAPV
// RUN: not %weavec_cxx -fweavec-safe -ffinite-loops -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=LOOPS
// RUN: not %weavec_cxx -fweavec-safe -fno-weavec-zero-init -fsyntax-only %s 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: %weavec_cxx -fweavec-safe -ffast-math -fno-fast-math -fsyntax-only %s
// RUN: %weavec_cxx -fweavec-safe -Ofast -O2 -fsyntax-only %s
// RUN: %weavec_cxx -fweavec-safe -fno-wrapv -fwrapv -fsyntax-only %s
// RUN: %weavec_cxx -ffast-math -fstrict-enums -fsyntax-only %s
// FAST: weavec-cc: error: '-ffast-math' lets the optimizer assume what a safe unit does not check (RFC 0051)
// OFAST: weavec-cc: error: '-Ofast' lets the optimizer assume what a safe unit does not check (RFC 0051)
// ENUMS: weavec-cc: error: '-fstrict-enums' lets the optimizer assume
// WRAPV: weavec-cc: error: '-fno-wrapv' lets the optimizer assume
// LOOPS: weavec-cc: error: '-ffinite-loops' lets the optimizer assume
// ZERO: weavec-cc: error: '-fno-weavec-zero-init' lets the optimizer assume
int main() { return 0; }
