/* The shared library of runtime-one-per-process.c. */
#include <stdlib.h>
#include <string.h>

char *lib_copy(const char *s) {
  char *p = malloc(strlen(s) + 1);
  if (p != NULL)
    strcpy(p, s);
  return p;
}

void lib_release(char *p) { free(p); }

int lib_read(const char *p, long i) {
  if (getenv("WEAVEC_TEST_SKIP") != NULL)
    return 0;
  return p[i];
}
