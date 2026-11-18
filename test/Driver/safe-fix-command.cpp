// RFC 0049 §6: `weavec-cc --weavec-fix -- <a compile command>` surveys the
// command itself (with -fsyntax-only, writing no object), then fixes and
// marks as with ledgers. The file left with no error is marked safe and
// then compiles with the rules on.
// RUN: rm -rf %t && mkdir -p %t && cp %s %t/one.cpp
// RUN: cd %t && %weavec_cxx --weavec-fix -- %weavec_cxx -std=c++20 -c one.cpp -o one.o | FileCheck %s
// RUN: test ! -e %t/one.o
// RUN: FileCheck %s --check-prefix=FILE < %t/one.cpp
// RUN: cd %t && %weavec_cxx -std=c++20 -fsyntax-only one.cpp
#include <cstddef>

int pair(const int *p) { return p[0] + p[1]; }

std::size_t count(const char *text, std::size_t n, char c) {
  std::size_t hits = 0;
  for (std::size_t i = 0; i < n; ++i)
    hits += text[i] == c ? 1 : 0;
  return hits;
}

// CHECK: weavec: fix: round 1: 2 edits in 1 files (0 certain, 2 likely); 0 errors after
// CHECK-NEXT: weavec: fix: round 2: nothing to apply
// CHECK-NEXT: weavec: fix: marked 1 files WEAVEC_SAFE_FILE
// CHECK-NEXT: weavec: fix: {{[0-9]+}} edits in 1 files; 2 errors before, 0 after: 0 have a choice or a fix withdrawn, 0 have no fix

// FILE: #include <cstddef>
// FILE-NEXT: #include <weavec.h>
// FILE-NEXT: WEAVEC_SAFE_FILE
// FILE: int pair(const int *WEAVEC_COUNTED_BY(2) p)
// FILE: std::size_t count(const char *WEAVEC_COUNTED_BY(n) text, std::size_t n, char c)
