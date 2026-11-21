// RFC 0051 §5 (N6), amendment 17: an LTO object's record is its bitcode's,
// and the comparison reads it as it reads an object's: two definitions of
// one class do not link, with full and thin LTO, from objects and from an
// archive's members. The comparison runs before the linker, so no LTO
// plugin is needed to see it.
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/definitions/* %t/
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -flto -c cache.cpp index.cpp
// RUN: cd %t && not %weavec_cxx -flto cache.o index.o -o prog 2>&1 | FileCheck %s
// RUN: cd %t && rm -f lib.a && llvm-ar rc lib.a cache.o 2>/dev/null || ar rc lib.a cache.o
// RUN: cd %t && not %weavec_cxx -flto index.o lib.a -o prog 2>&1 | FileCheck %s --check-prefix=ARCHIVE
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -flto=thin -c cache.cpp index.cpp
// RUN: cd %t && not %weavec_cxx -flto=thin cache.o index.o -o prog 2>&1 | FileCheck %s
// RUN: cd %t && not ls prog
// CHECK: weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
// CHECK-NEXT: cache.cpp:7:8: note: here it is 8 bytes: 'long hits' (cache.o)
// CHECK-NEXT: index.cpp:8:8: note: here it is 56 bytes: 'std::array<long, 6> slots', 'long hits' (index.o)
// ARCHIVE: weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
// ARCHIVE: (lib.a(cache.o))
