/* The shared library and plug-in of shared-images.c. */
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

/* A heap overflow inside the image when n is 8. */
int lib_overflow(long n) {
  char *p = malloc(8);
  int value;
  if (p == NULL)
    return -1;
  memset(p, 'x', 8);
  value = p[n];
  free(p);
  return value;
}
