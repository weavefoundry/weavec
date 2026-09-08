// RUN: not %weavec --checked-function=readonly %s -- 2>&1 | FileCheck %s --check-prefix=READONLY
// RUN: not %weavec --checked-function=unknown %s -- 2>&1 | FileCheck %s --check-prefix=UNKNOWN
// RUN: %weavec --checked-function=guarded %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=broken -Wno-error=weavec %s -- -DBROKEN 2>&1 | FileCheck %s --check-prefix=BROKEN
// RFC 0018: coverage failure is separate from a demonstrated violation, and
// lowering warnings never discharges selected proof obligations.
int unknown(int i) {
  int a[4] = {0};
  return a[i];
}
// UNKNOWN: error: cannot establish checked safety: access interval must fit its object [weavec::checking-incomplete]
// UNKNOWN: error: checked safety requirements were not established
int guarded(int i) {
  int a[4] = {0};
  if (i < 0 || i >= 4) return 0;
  return a[i];
}
// CLEAN-NOT: checking-incomplete
#ifdef BROKEN
void free(void *);
void *malloc(unsigned long);
void broken(void) {
  int *p = malloc(sizeof *p);
  free(p);
  free(p);
}
// BROKEN: warning: checked safety failed: {{.*}} [weavec::checking-failed]
// BROKEN: error: checked safety requirements were not established

#endif

void readonly(void) {
  char *p = "constant";
  *p = 1;
}
// READONLY: error: checked safety failed: cannot write to read-only storage [weavec::checking-failed]
