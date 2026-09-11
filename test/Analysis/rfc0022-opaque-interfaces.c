// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- -Wno-everything 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0022: recovered views need positive type/alignment evidence. Disabling
// warning presentation must not turn a violated checked obligation into success.
// CLEAN-NOT: error:
// BAD: error: checked safety failed: pointer recovery has an incompatible object type or alignment [weavec::checking-failed]
// BAD: error: checked safety requirements were not established

//--- good.c
static int read_value(void *p) { return *(int *)p; }
int main(void) { int value = 7; return read_value(&value); }

//--- bad.c
int main(void) { int value = 7; void *p = &value; return *(float *)p; }
