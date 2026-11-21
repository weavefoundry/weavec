// RFC 0051 §5 (N6), amendment 17: an ELF partial link (`ld -r`) joins its
// objects' records one after another, and the comparison reads each. (A
// Mach-O partial link keeps no record: amendment 16.)
// REQUIRES: host-linux
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/definitions/* %t/
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -c cache.cpp index.cpp
// RUN: cd %t && %weavec_cxx -fno-weavec-definitions -r cache.o index.o -o joined.o 2>/dev/null
// RUN: cd %t && not %weavec_cxx -fweavec-summary joined.o -o prog 2>&1 | FileCheck %s
// RUN: cd %t && not ls prog
// CHECK: weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
// CHECK-NEXT: cache.cpp:7:8: note: here it is 8 bytes: 'long hits' (joined.o)
// CHECK-NEXT: index.cpp:8:8: note: here it is 56 bytes: 'std::array<long, 6> slots', 'long hits' (joined.o)
