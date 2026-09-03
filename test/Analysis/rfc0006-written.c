// RFC 0006, *`written` forgets what lies below*: a callee that overwrites
// an object (`memcpy` into it) makes every fact about its sub-objects
// stale; the freed record of a field does not survive the overwrite.
// RUN: %weavec %s -- 2>&1 | FileCheck --allow-empty %s
// RUN: %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

void *memcpy(void *dest, const void *src, size_t n);

struct n {
  char *string;
  int v;
};

struct n tmp;

// Clean: `root->string` was freed, then `*root` was overwritten wholesale.
void replace(struct n *root) {
  free(root->string);
  memcpy(root, &tmp, sizeof *root);
  use(root->string);
}

// The summary does not claim `param 0 *.string` is freed either: the exit
// state no longer has it.
// DUMP: replace
// DUMP-NOT: param 0 *.string: freed

// CHECK-NOT: error:
