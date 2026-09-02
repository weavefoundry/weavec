// RFC 0003: the shipped libc summary table describes the common allocator,
// releaser and aliasing functions, so real headers need no annotations.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *alias_from_strchr(void) {
  char *s = strdup("a=b");
  if (!s)
    return NULL;
  char *eq = strchr(s, '=');
  free(s);
  // CHECK: rfc0003-libc.c:[[@LINE+1]]:10: error: use of 'eq' after it was freed [weavec::use-after-free]
  return eq;
}

void end_pointer_from_strtol(const char *text) {
  char *copy = strdup(text);
  char *end;
  long v = strtol(copy, &end, 10);
  free(copy);
  // CHECK: rfc0003-libc.c:[[@LINE+1]]:8: error: use of 'end' after it was freed [weavec::use-after-free]
  if (*end)
    (void)v;
}

void double_close(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  fclose(f);
  // CHECK: rfc0003-libc.c:[[@LINE+1]]:3: error: 'f' is freed twice [weavec::double-free]
  fclose(f);
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

// CHECK: 3 errors generated.
