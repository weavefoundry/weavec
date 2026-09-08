// RUN: %weavec --checked-function=caller %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=caller %s -- -DSTALE 2>&1 | FileCheck %s --check-prefix=STALE
// RFC 0019: preserve success facts through a direct-return wrapper and retire
// old output facts when the caller replaces that output before testing status.
static int count(unsigned *output, int success) {
  if (!success) return -1;
  *output = 4;
  return 0;
}
static int forward(unsigned *output, int success) {
  return count(output, success);
}
int caller(int success) {
  int values[4] = {1, 2, 3, 4};
  unsigned size = 0;
  int status = forward(&size, success);
#ifdef STALE
  size = 8;
#endif
  if (status != 0) return 0;
  return values[size - 1];
}
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// STALE: error: checked safety failed: {{.*}}out of bounds{{.*}} [weavec::checking-failed]
