// RUN: %weavec --version | FileCheck %s
// RUN: %weavec --help | FileCheck --check-prefix=HELP %s
//
// CHECK: weavec version {{[0-9]+\.[0-9]+\.[0-9]+}}
// CHECK-NEXT: built with LLVM {{[0-9]+\.}}
//
// HELP: weavec options
// HELP-DAG: --report-unannotated
// HELP-DAG: --analyze-headers
