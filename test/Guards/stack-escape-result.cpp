// RFC 0045 (Y4): a safe function of a fast unit whose pointer or reference
// result points into its own frame stops at its return (`stack-escape`),
// before a caller reads through it. Here guarded helpers hide the pointer
// from the rule `escape`, which rejects every direct form.
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
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

int main() { return worst(); }
