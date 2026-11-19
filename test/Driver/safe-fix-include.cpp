// RFC 0049 §6: two units propose the same fix on a header, one that sees
// weavec.h before it and one that does not; the header gets the include
// whichever unit's edit is kept, and the round compiles.
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/fix-pair.h %t/ && cp %S/Inputs/fix-pair-first.cpp %t/a.cpp && cp %s %t/b.cpp
// RUN: cd %t && %weavec_cxx -std=c++20 -fweavec-safe-survey -fweavec-ledger=%t/ledger/ -fsyntax-only a.cpp b.cpp
// RUN: cd %t && %weavec_cxx --weavec-fix --no-mark %t/ledger | FileCheck %s
// RUN: FileCheck %s --check-prefix=HEADER < %t/fix-pair.h
// RUN: cd %t && %weavec_cxx -std=c++20 -fsyntax-only b.cpp
#include "fix-pair.h"

int main() {
  int v[2] = {1, 2};
  return second(v) - 2;
}

// CHECK-NOT: broke a compile
// CHECK: weavec: fix: round 1: 1 edits in 1 files (0 certain, 1 likely); 0 errors after

// HEADER: #pragma once
// HEADER-NEXT: #include <weavec.h>
// HEADER: inline int second(const int *WEAVEC_COUNTED_BY(2) p)
