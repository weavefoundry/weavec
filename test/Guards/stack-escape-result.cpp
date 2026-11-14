// RFC 0045 (Y4): a safe function of a fast unit whose pointer or reference
// result points into its own frame stops at its return (`stack-escape`),
// before a caller reads through it. Here guarded helpers hide the pointer
// from the rule `escape`, which rejects every direct form.
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// A `musttail` call's result gets no check: nothing goes between it and the
// return.
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --check-prefix=TAIL
#include <weavec.h>

static const int *kept;
WEAVEC_GUARDED void keep(const int &x) { kept = &x; }
WEAVEC_GUARDED const int &last() { return *kept; }

// CHECK: weavec: stack-escape at {{.*}}stack-escape-result.cpp:[[@LINE+4]]:{{[0-9]+}}: result of 'worst()' points into its own frame at 0x{{[0-9a-f]+}}
[[gnu::noinline]] const int &worst() {
  int local = 1;
  keep(local);
  return last();
}

// TAIL-LABEL: define {{.*}} @_Z7forwardPKi(
// TAIL-NOT: frame_escape
// TAIL: musttail call
// TAIL-NEXT: ret ptr
[[gnu::noinline]] WEAVEC_GUARDED const int *pass(const int *p) { return p; }
const int *forward(const int *p) { [[clang::musttail]] return pass(p); }

int main() { return worst(); }
