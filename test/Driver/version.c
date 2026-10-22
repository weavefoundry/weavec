// RFC 0033 §10: weavec-cc prints Clang's version block first, so that a
// configure script that reads the first line takes the Clang path, and its
// own after it.
// RUN: %weavec_cc --version | FileCheck %s
//
// CHECK: clang version
// CHECK: weavec-cc version {{[0-9]+\.[0-9]+\.[0-9]+}}
// CHECK-NEXT: built with LLVM {{[0-9]+\.}}
