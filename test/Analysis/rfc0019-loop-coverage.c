// RUN: %weavec --checked-function=main %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %s -- -DSKIP 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0019: a skipped iteration cannot establish complete initialization.
static void fill(char *bytes, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
#ifdef SKIP
    if (i == 2) continue;
#endif
    bytes[i] = 7;
  }
}
int main(void) {
  char bytes[4];
  fill(bytes, 4);
  return bytes[2];
}
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// BAD: error: cannot establish checked safety: read interval must be initialized [weavec::checking-incomplete]
