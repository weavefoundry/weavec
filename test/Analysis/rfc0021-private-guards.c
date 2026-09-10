// RUN: %weavec --checked-function=maybe_free %s -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=bad %s -- -DBAD 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0021: a private antecedent may strengthen an entry requirement at
// export, but cannot drop the release requirement or validate stack storage.

#include <stdlib.h>

static void (*reader)(void);

void maybe_free(char *line) {
  if (reader != NULL)
    free(line);
}

#ifdef BAD
static void present(void) {}

void bad(void) {
  char line[1];
  reader = present;
  maybe_free(line);
}
#endif

// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// BAD: error: checked safety requirements were not established
