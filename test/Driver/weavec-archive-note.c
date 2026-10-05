// RFC 0034 §8: a link input without a record whose objects `weavec-cc`
// built (an archive: `ar` keeps no records) is checked and guarded, only not
// analysed with the program. It is named in a note, not in the
// `unanalyzed-input` warning, which still names the inputs the compiler did
// not build.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -c %s -DUNIT_B -o %t/b.o
// RUN: %clang -c %s -DUNIT_C -o %t/c.o
// RUN: llvm-ar rcs %t/libbuilt.a %t/b.o
// RUN: llvm-ar rcs %t/libforeign.a %t/c.o
//
// RUN: %weavec_cc -fweavec-link=analyze %t/main.o %t/libbuilt.a %t/libforeign.a -o %t/prog 2>&1 | FileCheck %s
// RUN: %t/prog
// RUN: %weavec_cc -fweavec-link=analyze %t/main.o %t/libbuilt.a %t/c.o -o %t/prog 2>&1 | FileCheck --check-prefix=OBJECT %s

// CHECK: weavec-cc: note: '{{.*}}libbuilt.a' was built by weavec-cc without records: checked and guarded, not analysed with the program
// CHECK-NEXT: weavec-cc: warning: link input '{{.*}}libforeign.a' has no WeaveC record; calls into it are trusted [weavec::unanalyzed-input]
// CHECK-NOT: libbuilt

// OBJECT: weavec-cc: note: '{{.*}}libbuilt.a' was built by weavec-cc
// OBJECT-NEXT: weavec-cc: warning: link input '{{.*}}c.o' has no WeaveC record

#if defined(UNIT_B)
int table[4];
int pick(int *p, int i) { return p[i] + table[i & 3]; }
#elif defined(UNIT_C)
int other(void) { return 7; }
#else
int pick(int *p, int i);
int other(void);

int main(void) {
  int values[2] = {1, 0};
  return pick(values, 1) + other() - 7;
}
#endif
