// RFC 0007, *Mismatched releases*: every allocator belongs to the family of
// its releaser; releasing a resource with a function of another family is an
// error, through wrappers and through `realloc` too.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The RFC's snippets that must be reported.

void family(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+2]]:3: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  // CHECK: rfc0007-families.c:[[@LINE-4]]:13: note: allocated here
  free(f);
}

void family2(void) {
  char *p = malloc(8);
  if (!p)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: error: 'p' is released with 'fclose' but must be released with 'free' [weavec::mismatched-release]
  fclose((FILE *)p);
}

void family3(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:7: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  f = realloc(f, sizeof *f);
  free(f);
}

// Through a wrapper: the family is the canonical releaser's name, not the
// wrapper's.
static void xfree(void *p) { free(p); }

void via_wrapper(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  xfree(f);
}

// The mismatch travels with copies.
void via_copy(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  void *v = f;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: error: 'v' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  free(v);
}

// Clean: the matching releaser, a wrapper of it, and `strdup` is family free.
void fine(const char *path) {
  FILE *f = fopen(path, "r");
  if (f)
    fclose(f);
  char *s = strdup(path);
  xfree(s);
}

// The inferred summaries carry the family (RFC 0007, *Summary text format*).
// DUMP: function 'xfree':
// DUMP: summary: p: freed(free); stores{} returns{}
// DUMP: function 'opens':
// DUMP: summary: *path: read; stores{} returns{fresh(fclose)}
FILE *opens(const char *path) { return fopen(path, "r"); }

// CHECK: 5 errors generated.
