// RFC 0035 §9: the enforcement ledger lists each access with its outcome.
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %weavec_cc -O2 -c %s -o %t.dir/unit.o -fweavec-ledger=%t.dir/ -fweavec-summary 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: FileCheck %s < %t.dir/unit.o.ledger.json
// RUN: %weavec_cc -O2 -c %s -o %t.dir/quiet.o 2>&1 | count 0

// SUMMARY: weavec: {{.*}}ledger.c: 2 accesses: 1 proven, 1 guarded, 0 unguarded
// CHECK-DAG: "schema": "weavec-ledger"
// CHECK-DAG: "version": 3
// CHECK-DAG: "checks": "trap"
// CHECK-DAG: "accesses": 2
// CHECK-DAG: "outcome": "guarded"
// CHECK-DAG: "outcome": "proven"
// CHECK-DAG: "reason": "in-bounds"
int get(const int *p, int i) { return p[i]; }

int table[8];
int first(void) { return table[2]; }
int nth(unsigned i) { return table[i & 7]; }
