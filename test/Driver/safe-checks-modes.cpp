// RFC 0045 §1, §9: -fweavec-safe-checks=bounds (the default) is the fast
// mode, `full` the checked one. A fast C++ unit is compiled without the
// container annotations (libc++'s __SANITIZER_DISABLE_CONTAINER_OVERFLOW__
// in place of libstdc++'s _GLIBCXX_SANITIZE_VECTOR); the ledger's config
// says which mode a safe unit was compiled in.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cxx -std=c++20 -fweavec-safe -E -dM %s | FileCheck %s --check-prefix=FAST
// RUN: %weavec_cxx -std=c++20 -fweavec-safe -fweavec-safe-checks=full -E -dM %s | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cxx -std=c++20 -E -dM %s | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -c %s -o %t/fast.o -fweavec-ledger=%t/fast.json
// RUN: FileCheck %s --check-prefix=FASTLEDGER < %t/fast.json
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -fweavec-safe-checks=full -c %s -o %t/full.o -fweavec-ledger=%t/full.json
// RUN: FileCheck %s --check-prefix=FULLLEDGER < %t/full.json
// FAST: #define __SANITIZER_DISABLE_CONTAINER_OVERFLOW__ 1
// FAST-NOT: _GLIBCXX_SANITIZE_VECTOR
// FULL-NOT: __SANITIZER_DISABLE_CONTAINER_OVERFLOW__
// FULL: #define _GLIBCXX_SANITIZE_VECTOR 1
// FULL-NOT: __SANITIZER_DISABLE_CONTAINER_OVERFLOW__
// FASTLEDGER: "safe": "fast"
// FULLLEDGER: "safe": "full"
#include <vector>

int sum(const std::vector<int> &values) {
  int s = 0;
  for (int v : values)
    s += v;
  return s;
}
