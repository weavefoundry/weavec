// RFC 0049 §7: in a ledger directory, two objects of one name (`pair0/pair.c.o`
// and `pair1/pair.c.o`) do not share a ledger: the first keeps
// `<object>.ledger.json`, the other's name adds a hash of its path, and
// compiling the first again writes its own ledger again. Two sources of
// one name in two directories, surveyed with -fsyntax-only, do not share a
// ledger either.
// RUN: rm -rf %t && mkdir -p %t/a %t/b
// RUN: %weavec_cc -fno-weavec-summary -fweavec-ledger=%t/led/ -c %s -o %t/a/x.o
// RUN: %weavec_cc -fno-weavec-summary -fweavec-ledger=%t/led/ -DOTHER -c %s -o %t/b/x.o
// RUN: %weavec_cc -fno-weavec-summary -fweavec-ledger=%t/led/ -c %s -o %t/a/x.o
// RUN: ls %t/led | FileCheck %s
// RUN: FileCheck %s --check-prefix=FIRST < %t/led/x.o.ledger.json
// CHECK: x.o.{{[0-9A-F]+}}.ledger.json
// CHECK-NEXT: x.o.ledger.json
// CHECK-NOT: ledger.json
// FIRST: "object": "{{.*}}a/x.o"
// RUN: cp %s %t/a/x.c && cp %s %t/b/x.c
// RUN: cd %t/a && %weavec_cc -fno-weavec-summary -fweavec-safe-survey -fweavec-ledger=%t/syntax/ -fsyntax-only x.c
// RUN: cd %t/b && %weavec_cc -fno-weavec-summary -fweavec-safe-survey -fweavec-ledger=%t/syntax/ -fsyntax-only x.c
// RUN: ls %t/syntax | FileCheck %s --check-prefix=SYNTAX
// SYNTAX: x.c.{{[0-9A-F]+}}.ledger.json
// SYNTAX-NEXT: x.c.{{[0-9A-F]+}}.ledger.json
int first(void) { return 1; }
