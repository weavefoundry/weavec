// Well-formed ownership patterns must not produce any diagnostics.
// RUN: %weavec %s -- 2>&1 | count 0
#include "../Inputs/prelude.h"

void alloc_use_free(void) {
  int *p = malloc(sizeof(int));
  *p = 1;
  use(p);
  free(p);
}

void reassign_after_free(void) {
  int *p = malloc(4);
  free(p);
  p = malloc(8);
  use(p);
  free(p);
  p = NULL;
  use(p);
}

void free_on_one_path_only(int c) {
  int *p = malloc(4);
  if (c) {
    use(p);
  } else {
    free(p);
    p = NULL;
  }
  use(p);
}

void unrelated_pointers(int *a, int *b) {
  free(a);
  use(b);
}
