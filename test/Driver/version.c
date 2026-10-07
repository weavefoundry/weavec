// RUN: %weavec --version | FileCheck %s
// RUN: %weavec --help | FileCheck --check-prefix=HELP %s
//
// CHECK: weavec version {{[0-9]+\.[0-9]+\.[0-9]+}}
// CHECK-NEXT: built with LLVM {{[0-9]+\.}}
//
// HELP: weavec options
// HELP-DAG: --budget
// HELP-DAG: --no-zero-init
// HELP-DAG: --whole-program
// HELP-NOT: --report-unannotated
// HELP-NOT: --ledger
// HELP-NOT: --require
// HELP-NOT: --dump-record
// HELP-NOT: --no-runtime
