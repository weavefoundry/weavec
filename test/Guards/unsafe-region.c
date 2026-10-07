// RFC 0035 §2.6: the accesses of a WEAVEC_UNSAFE function or block are not
// guarded and recorded as unguarded; the rest are.
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %weavec_cc -O2 -c %s -o %t.dir/unsafe.o -fweavec-ledger=%t.dir/ -fweavec-summary 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: FileCheck %s --check-prefix=LEDGER < %t.dir/unsafe.o.ledger.json
#include <weavec.h>

WEAVEC_UNSAFE int peek(const int *p, int i) { return p[i]; }

int both(const int *p, int i) {
  int s = p[i];
  WEAVEC_UNSAFE {
    s += p[i + 1];
  }
  return s;
}

// SUMMARY: weavec: {{.*}}unsafe-region.c: 3 accesses: 0 proven, 1 guarded, 2 unguarded
// LEDGER-DAG: "schema": "weavec-ledger"
// LEDGER-DAG: "version": 3
// LEDGER-DAG: "outcome": "unguarded"
// LEDGER-DAG: "reason": "unsafe"
