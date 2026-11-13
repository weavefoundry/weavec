// RFC 0044 §1, §12: -fweavec-safe in a C++ unit. The rules check the unit
// and change nothing in what is compiled: the object is the one the unit
// gives without the flag, every guard kept. The ledger (version 7) lists
// each function's tier and each member that owns, refers, borrows, shares
// or erases; the summary line counts them; the audit (version 3) lists what
// the rules rest on. -fweavec-safe-checks has no effect here.
// RUN: rm -rf %t && mkdir -p %t/ledger
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-summary -fweavec-ledger=%t/ledger/ -c %s -o %t/safe.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cxx -std=c++20 -O2 -c %s -o %t/plain.o
// RUN: cmp %t/safe.o %t/plain.o
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=bounds -c %s -o %t/bounds.o
// RUN: cmp %t/bounds.o %t/plain.o
// RUN: FileCheck %s --check-prefix=LEDGER < %t/ledger/safe.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/ledger | FileCheck %s --check-prefix=AUDIT
// RUN: %weavec_cc --weavec-audit --format=json %t/ledger | FileCheck %s --check-prefix=JSON
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t/program
// RUN: not %t/program 2>&1 | FileCheck %s --check-prefix=RUN
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <weavec.h>

struct Item {
  int v = 0;
  std::string name;
};
struct Cache {
  std::vector<std::unique_ptr<Item>> items;
  const Item *last = nullptr;
  std::shared_ptr<Item> shared;
  std::shared_ptr<const Item> frozen;
  std::function<void()> onChange;
  std::string_view label;
  int count = 0;
  void remember() { last = items.back().get(); }
  int recall() const { return last->v; }
};
WEAVEC_GUARDED int raw(const int *p) { return p[0]; }

// A reference is not followed by the rules: the guard stops its use.
int main() {
  Cache cache;
  cache.items.push_back(std::make_unique<Item>());
  cache.remember();
  cache.items.pop_back();
  int one = 1;
  std::printf("%d\n", cache.recall() + raw(&one));
  return 0;
}

// SUMMARY: weavec: {{.*}}safe-cxx-units.cpp: {{[0-9]+}} accesses: {{.*}}; 3 of 4 functions safe; 3 owner and 1 reference fields
// LEDGER: "fields": [
// LEDGER: "field": "name"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "owner"
// LEDGER: "field": "items"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "owner"
// LEDGER: "field": "last"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "reference"
// LEDGER: "field": "shared"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "shared"
// LEDGER: "field": "frozen"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "owner"
// LEDGER: "field": "onChange"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "erased"
// LEDGER: "field": "label"
// LEDGER-NEXT: "file":
// LEDGER-NEXT: "kind": "borrowed"
// LEDGER: "functions": [
// LEDGER: "function": "Cache::remember"
// LEDGER: "tier": "safe"
// LEDGER: "function": "Cache::recall"
// LEDGER: "tier": "safe"
// LEDGER: "function": "raw"
// LEDGER: "tier": "guarded"
// LEDGER: "function": "main"
// LEDGER: "tier": "safe"
// LEDGER: "version": 7
// AUDIT: 3 of 4 functions safe
// AUDIT: functions that are not safe
// AUDIT-NEXT: safe-cxx-units.cpp:{{[0-9]+}}: raw (guarded)
// AUDIT: reference fields
// AUDIT-NEXT: safe-cxx-units.cpp:{{[0-9]+}}: Cache.last
// AUDIT: shared members
// AUDIT-NEXT: safe-cxx-units.cpp:{{[0-9]+}}: Cache.shared
// AUDIT: erased members
// AUDIT-NEXT: safe-cxx-units.cpp:{{[0-9]+}}: Cache.onChange
// JSON-DAG: "schema": "weavec-audit"
// JSON-DAG: "version": 3
// JSON-DAG: "shared": [
// JSON-DAG: "erased": [
// RUN: weavec: heap-use-after-free at {{.*}}safe-cxx-units.cpp:39:
