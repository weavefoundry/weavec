// RFC 0051 §5 (N6): each safe unit records what it believes of the names it
// shares, and a link compares the records of its inputs before the linker
// runs: two definitions of one class, a global of two lengths, and a
// declaration that differs from its definition do not link. A type in an
// unnamed namespace is the file's own; C's structs differ only where a
// recorded declaration shares them. Objects, archives and the summary line;
// -fno-weavec-definitions skips the comparison and says so.
// RUN: rm -rf %t && mkdir -p %t && cp %S/Inputs/definitions/* %t/
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -c cache.cpp index.cpp
// RUN: cd %t && not %weavec_cxx cache.o index.o -o prog 2>&1 | FileCheck %s --check-prefix=TYPE
// RUN: cd %t && not ls prog
// TYPE: weavec-cc: error: 'Entry' is defined differently in two safe units [weavec-safe:definition-mismatch]
// TYPE-NEXT: cache.cpp:7:8: note: here it is 8 bytes: 'long hits' (cache.o)
// TYPE-NEXT: index.cpp:8:8: note: here it is 56 bytes: 'std::array<long, 6> slots', 'long hits' (index.o)
// TYPE-NEXT: weavec-cc: note: a program has one definition of each name: put a type only one file uses in an unnamed namespace, or define it in one header both include
//
// Each file's own type, in an unnamed namespace: the units agree.
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -DPRIVATE -c cache.cpp index.cpp
// RUN: cd %t && %weavec_cxx -fweavec-summary cache.o index.o -o prog 2>&1 | FileCheck %s --check-prefix=AGREE
// RUN: %t/prog | FileCheck %s --check-prefix=OUT
// AGREE: weavec: prog: 2 objects, 2 safe; {{[0-9]+}} types, {{[0-9]+}} functions and 1 globals agree
// OUT: 1 0 4
//
// A global of two lengths; and through an archive.
// RUN: cd %t && %weavec_cxx -std=c++20 -O1 -fweavec-safe -DPRIVATE -DLIMITS=16 -c cache.cpp -o cache16.o
// RUN: cd %t && not %weavec_cxx cache16.o index.o -o prog16 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: cd %t && rm -f lib.a && llvm-ar rc lib.a cache16.o 2>/dev/null || ar rc lib.a cache16.o
// RUN: cd %t && not %weavec_cxx index.o lib.a -o prog16 2>&1 | FileCheck %s --check-prefix=ARCHIVE
// GLOBAL: weavec-cc: error: 'limits' is declared differently in two safe units [weavec-safe:definition-mismatch]
// GLOBAL: note: here it is 'const long[16]' (cache16.o)
// GLOBAL: note: here it is 'const long[4]' (index.o)
// ARCHIVE: weavec-cc: error: 'limits' is declared differently in two safe units
// ARCHIVE: (lib.a(cache16.o))
//
// -fno-weavec-definitions: linked, and said.
// RUN: cd %t && %weavec_cxx -fno-weavec-definitions cache16.o index.o -o prog16 2>&1 | FileCheck %s --check-prefix=SKIP
// SKIP: weavec-cc: note: -fno-weavec-definitions: the safe units of this link were not compared (RFC 0051)
//
// C: a struct each file keeps to itself links; one a shared declaration
// names in two forms does not.
// RUN: cd %t && %weavec_cc -O1 -fweavec-safe -c node.c list.c && %weavec_cc node.o list.o -o cprog
// RUN: cd %t && %weavec_cc -O1 -fweavec-safe -DSHARED -c node.c list.c
// RUN: cd %t && not %weavec_cc node.o list.o -o cprog 2>&1 | FileCheck %s --check-prefix=CTAG
// CTAG: weavec-cc: error: 'struct node' is defined differently in two safe units [weavec-safe:definition-mismatch]
