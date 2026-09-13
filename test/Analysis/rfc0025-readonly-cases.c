// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main --checked-function=read_if %t/good.c -- 2>&1 | FileCheck %s --check-prefix=GENERIC
// RUN: not %weavec --checked-function=main %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0025: retain the whole case ledger and independently selected generic.
// CLEAN-NOT: error:
// GENERIC: error: cannot establish checked safety: integer operation may be invalid [weavec::checking-incomplete]
// BAD: error: cannot establish checked safety:

//--- good.c
static int read_if(int enabled, const int *pointer) {
  if (!enabled) return 0;
  return 100 / *pointer;
}
int main(void) { return read_if(0, 0); }

//--- bad.c
static int read_if(int enabled, const int *pointer) {
  if (!enabled) return 0;
  return 100 / *pointer;
}
int main(void) { return read_if(1, 0); }
