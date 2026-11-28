// RFC 0055 G10: a fast unit's pool key is the type's name and its layout
// (RFC 0054 §1.1: its size and where it keeps pointers), so that two
// definitions of one name that no link compares (one in a dynamic library
// or an archive, which `definition-mismatch` does not see) never share a
// pool: a stale pointer of one cannot read a pointer the other forged from
// an integer. A definition that differs only in its members' names is the
// same layout, and the same pool.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | grep -o '@__weavec.type = private global { i64, i64, ptr, i64 } { i64 [-0-9]*' > %t.pointer
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -DINTEGERS -S -emit-llvm %s -o - | grep -o '@__weavec.type = private global { i64, i64, ptr, i64 } { i64 [-0-9]*' > %t.integers
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -DRENAMED -S -emit-llvm %s -o - | grep -o '@__weavec.type = private global { i64, i64, ptr, i64 } { i64 [-0-9]*' > %t.renamed
// RUN: not diff %t.pointer %t.integers
// RUN: diff %t.pointer %t.renamed
#include <memory>

#if defined(INTEGERS)
struct Node {
  long n;
  long m;
};
#elif defined(RENAMED)
struct Node {
  long *q;
  long k;
};
#else
struct Node {
  long *p;
  long n;
};
#endif

std::unique_ptr<Node> make() { return std::make_unique<Node>(); }
