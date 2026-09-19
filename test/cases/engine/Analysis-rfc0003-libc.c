// Engine pin converted from test/Analysis/rfc0003-libc.c; markers are the v0.10.0 golden diagnostics.
// RFC 0003: the shipped libc summary table describes the common allocator,
// releaser and aliasing functions, so real headers need no annotations.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *alias_from_strchr(void) {
  char *s = strdup("a=b");
  if (!s)
    return NULL;
  char *eq = strchr(s, '=');
  free(s);
  return eq; // BUG: use-after-free definite
}

void end_pointer_from_strtol(const char *text) {
  char *copy = strdup(text);
  if (!copy)
    return;
  char *end;
  long v = strtol(copy, &end, 10);
  free(copy);
  if (*end) // BUG: use-after-free definite
    (void)v;
}

void double_close(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  fclose(f);
  fclose(f); // BUG: double-free definite
}

void fine(const char *path) {
  char *s = strdup(path);
  if (!s)
    return;
  size_t n = strlen(s);
  char *copy = malloc(n + 1);
  if (copy) {
    memcpy(copy, s, n + 1);
    puts(copy);
    free(copy);
  }
  FILE *f = fopen(s, "r");
  free(s);
  if (f) {
    fputs("x", f);
    fclose(f);
  }
}
