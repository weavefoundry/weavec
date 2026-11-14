// RFC 0045 §5: a safe function's call of a trusted standard library
// function (a member of an owner whose parameters are no pointer, view or
// callable) goes to a clone of it, `.weavec.t`, compiled without guards, as
// are the library functions it calls; the original keeps its guards for the
// unit's other code and for guarded units, and a variant's table of
// visitors is copied to name their clones. The ledger and the audit list
// what a fast unit trusts. -fweavec-safe-checks=full makes no clone.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cxx -std=c++20 -O0 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O0 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -fweavec-summary -fweavec-ledger=%t/ -c %s -o %t/trusted.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cc --weavec-audit %t | FileCheck %s --check-prefix=AUDIT
#include <vector>

// CHECK-LABEL: define {{.*}} @_Z4fill{{[^(]*}}(
// CHECK: call {{.*}}@_ZNSt{{[^(]*}}vector{{[^(]*}}9push_back{{[^(]*}}.weavec.t(
// FULL-NOT: .weavec.t
[[gnu::noinline]] void fill(std::vector<int> &values, int n) {
  for (int i = 0; i < n; ++i)
    values.push_back(i);
}

// CHECK-LABEL: define linkonce_odr hidden {{.*}}@_ZNSt{{[^(]*}}vector{{[^(]*}}9push_back{{[^(]*}}.weavec.t(
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret void

// SUMMARY: weavec: {{.*}}ir-trusted.cpp: {{.*}}; {{[0-9]+}} typed allocation sites, {{[1-9][0-9]*}} trusted library functions
// AUDIT: library functions fast units run without guards
// AUDIT: std::{{.*}}vector<int, std::{{.*}}allocator<int>{{ ?}}>::push_back
