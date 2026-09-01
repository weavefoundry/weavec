// Real system and builtin headers must resolve without extra flags: weavec
// points Clang at the resource directory of the installation it was built
// against.
// RUN: %weavec %s -- 2>&1 | count 0
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <weavec.h>

size_t safe(void) {
  char *WEAVEC_OWNED s = malloc(16);
  if (s == NULL)
    return 0;
  strcpy(s, "hi");
  size_t n = strlen(s);
  free(s);
  return n;
}
