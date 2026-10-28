// RFC 0036 §5: a precompiled header built by weavec-cc is read by the
// compiles that follow with the same flags, as with Clang: it records ASan
// in its options, as they have it.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -x c-header %S/Inputs/pch.h -o %t/pch.h.pch
// RUN: %weavec_cc -include-pch %t/pch.h.pch %s -o %t/prog
// RUN: %t/prog | FileCheck %s
// RUN: %weavec_cc -fno-weavec-asan-compat -x c-header %S/Inputs/pch.h -o %t/plain.h.pch
// RUN: %weavec_cc -fno-weavec-asan-compat -include-pch %t/plain.h.pch %s -o %t/plain
// RUN: %t/plain | FileCheck %s
// CHECK: from the header: 42

int main(void) {
  printf("from the header: %d\n", fromHeader());
  return 0;
}
