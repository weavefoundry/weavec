// RUN: %weavec --checked-function=main %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %s -- -DPARTIAL 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0019: checked facts follow the returned constructor's nested objects.
#include <stdlib.h>
struct box { char *bytes; };
static struct box *make(void) {
  struct box *box = malloc(sizeof *box);
  if (!box) return NULL;
  box->bytes = malloc(4);
  if (!box->bytes) { free(box); return NULL; }
  box->bytes[0] = 7;
  return box;
}
int main(void) {
  struct box *box = make();
  if (!box) return 0;
#ifdef PARTIAL
  int result = box->bytes[1];
#else
  int result = box->bytes[0];
#endif
  free(box->bytes);
  free(box);
  return result;
}
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// BAD: error: cannot establish checked safety: read interval must be initialized [weavec::checking-incomplete]
