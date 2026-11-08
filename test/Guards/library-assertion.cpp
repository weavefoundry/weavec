// RFC 0039 §6: C++ compiles turn on the C++ library's bounds checks, and a
// failed one is a library-assertion report at the caller's line, with the
// library's message, in every mode.
// RUN: %weavec_cxx -O1 %s -o %t
// RUN: %t
// RUN: not --crash %t past 2>&1 | FileCheck %s
// RUN: %weavec_cxx -O1 -fweavec-checks=report %s -o %t.report
// RUN: not --crash %t.report past 2>&1 | FileCheck %s
// libc++'s trap becomes the runtime's call; libstdc++'s assertion
// function is redirected to the runtime's.
// RUN: %if host-darwin %{ %weavec_cxx -O1 -S -emit-llvm %s -o - | FileCheck --check-prefix=LIBCXX %s %}
// RUN: %if host-linux %{ %weavec_cxx -O1 -S -emit-llvm %s -o - | FileCheck --check-prefix=LIBSTDCXX %s %}
// LIBCXX: call void @__weavec_rt_library_assertion(ptr @__weavec.message
// LIBSTDCXX: call void @__weavec_rt_glibcxx_assert_fail(
#include <span>
#include <vector>

int main(int argc, char **) {
  std::vector<int> v(1);
  if (argc == 1)
    return v[0];
  // CHECK: weavec: library-assertion at {{.*}}library-assertion.cpp:[[@LINE+1]]:{{[0-9]+}}: {{.*(vector\[\] index out of bounds|__n < this->size\(\))}}
  return v[static_cast<unsigned>(argc) + 3];
}
