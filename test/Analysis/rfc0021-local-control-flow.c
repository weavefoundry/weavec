// RUN: %weavec --checked-function=initialized %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: %weavec --checked-function=cleanup %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=skipped %s -- 2>&1 | FileCheck %s --check-prefix=SKIPPED
// RUN: not %weavec --checked-function=computed %s -- 2>&1 | FileCheck %s --check-prefix=COMPUTED
// RFC 0021: direct gotos retain the CFG's initialization and lifetime edges.

#include <stdlib.h>

int initialized(int flag) {
  int value = 1;
  if (flag)
    goto done;
  value = 2;
done:
  return value;
}

void cleanup(void) {
  char *p = malloc(4);
  if (!p)
    goto done;
  free(p);
done:
  return;
}

int skipped(int flag) {
  if (flag)
    goto done;
  int value = 2;
done:
  return value;
}

int computed(void) {
  void *destination = &&done;
  goto *destination;
done:
  return 0;
}

// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// SKIPPED: error: cannot establish checked safety: local value must be initialized [weavec::checking-incomplete]
// SKIPPED: error: checked safety requirements were not established
// COMPUTED: error: cannot establish checked safety: unsupported checked C construct or storage type [weavec::checking-incomplete]
// COMPUTED: error: checked safety requirements were not established
