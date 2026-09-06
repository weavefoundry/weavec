// RFC 0012, *String facts*, *Length places*, *Sources of string facts* and
// *String checks*: what the checker knows about the NUL-terminated string an
// object holds, where the knowledge comes from, and the copies and reads it
// checks against it.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

size_t strlen(const char *s);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strdup(const char *s);
int sprintf(char *buf, const char *fmt, ...);
int puts(const char *s);
int printf(const char *fmt, ...);
void *memset(void *dst, int c, size_t n);

// -- Literals against declared extents ----------------------------------------

void literals(void) {
  char buf[4];
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcpy' accesses 6 bytes of 'buf', which has 4 bytes [weavec::out-of-bounds]
  strcpy(buf, "hello");
  // CHECK: rfc0012-strings.c:[[@LINE-3]]:8: note: 'buf' is declared here
  strcpy(buf, "abc");
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcat' accesses 5 bytes of 'buf', which has 4 bytes [weavec::out-of-bounds]
  strcat(buf, "d");
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:11: error: 'sprintf' accesses 5 bytes of 'buf', which has 4 bytes [weavec::out-of-bounds]
  sprintf(buf, "%s!", "abc");
}

// A format's minimum output: one digit for `%d`, the literal text, the NUL.
void formats(int x) {
  char buf[4];
  sprintf(buf, "%d", x);
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:11: error: 'sprintf' accesses at least 5 bytes of 'buf', which has 4 bytes [weavec::out-of-bounds]
  sprintf(buf, "%d!!!", x);
}

// -- Length places -------------------------------------------------------------

// `strlen(s)` is a place; the allocation's extent and the copy's need are
// both stated in it.
// DUMP-LABEL: function 'short_by_one':
// DUMP: scalars{strlen(s) zero|positive} spatial{s string=len(strlen(s))}
void short_by_one(const char *s) {
  char *d = malloc(strlen(s));
  if (!d)
    return;
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcpy' accesses 'strlen(s)' + 1 bytes of 'd', which has 'strlen(s)' bytes [weavec::out-of-bounds]
  strcpy(d, s);
  // CHECK: rfc0012-strings.c:[[@LINE-5]]:13: note: 'd' is allocated here
  free(d);
}

// Through a variable the relation `n == strlen(s)` carries the fact.
void through_a_variable(const char *s) {
  size_t n = strlen(s);
  char *d = malloc(n);
  if (!d)
    return;
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcpy' accesses 'strlen(s)' + 1 bytes of 'd', which has 'n' bytes ('strlen(s)' equals 'n') [weavec::out-of-bounds]
  strcpy(d, s);
  free(d);
}

// The exact allocation is clean, and the string's length is known after
// the copy: an index at the terminator is fine, one past it is not.
void exact(const char *s) {
  char *d = malloc(strlen(s) + 1);
  if (!d)
    return;
  strcpy(d, s);
  d[strlen(s)] = 0;
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:3: error: 'd[strlen(s) + 1]' is out of bounds: it reaches 'strlen(s)' + 2 bytes into 'd', which has 'strlen(s)' + 1 bytes [weavec::out-of-bounds]
  d[strlen(s) + 1] = 0;
  free(d);
}

// `strdup` measures its argument: the result has the length and one more
// byte of extent.
void duplicated(const char *s) {
  char *d = strdup(s);
  if (!d)
    return;
  d[strlen(s)] = 0;
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcat' accesses 'strlen(s)' + 2 bytes of 'd', which has 'strlen(s)' + 1 bytes [weavec::out-of-bounds]
  strcat(d, "x");
  free(d);
}

// -- Unterminated objects -----------------------------------------------------

// `strncpy` with a source at least as long as the count leaves no
// terminator in a buffer the count fills; every terminator-seeking read of
// it is reported, with the note at the copy.
void unterminated(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:23: error: 'strlen' reads past the end of 'name', which is not NUL-terminated [weavec::out-of-bounds]
  size_t len = strlen(name);
  // CHECK: rfc0012-strings.c:[[@LINE-3]]:3: note: 'name' is left without a terminator here
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:8: error: 'puts' reads past the end of 'name', which is not NUL-terminated [weavec::out-of-bounds]
  puts(name);
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:18: error: 'printf' reads past the end of 'name', which is not NUL-terminated [weavec::out-of-bounds]
  printf("%s\n", name);
  (void)len;
}

// A NUL stored into the object terminates it again.
void repaired(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  name[sizeof name - 1] = 0;
  puts(name);
}

// An initialiser as long as the array, or with no NUL among its elements.
void initialisers(void) {
  char a[4] = "abcd";
  char b[3] = {'a', 'b', 'c'};
  char c[4] = "abc";
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:8: error: 'puts' reads past the end of 'a', which is not NUL-terminated [weavec::out-of-bounds]
  puts(a);
  // CHECK: rfc0012-strings.c:[[@LINE-5]]:8: note: 'a' is left without a terminator here
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:8: error: 'puts' reads past the end of 'b', which is not NUL-terminated [weavec::out-of-bounds]
  puts(b);
  puts(c);
}

// `memset` with a non-zero byte over the whole object; with zero it is the
// empty string.
void filled(void) {
  char a[4];
  char b[4];
  memset(a, 'x', sizeof a);
  memset(b, 0, sizeof b);
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:8: error: 'puts' reads past the end of 'a', which is not NUL-terminated [weavec::out-of-bounds]
  puts(a);
  puts(b);
  strcat(b, "abc");
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcat' accesses 5 bytes of 'b', which has 4 bytes [weavec::out-of-bounds]
  strcat(b, "d");
}

// -- Facts are on the object --------------------------------------------------

// A pointer to the array shares its facts; a copy at an offset sees the
// length from there.
void aliased(void) {
  char buf[8] = "abcde";
  char *p = buf + 2;
  strcat(p, "fg");
  // CHECK: rfc0012-strings.c:[[@LINE+1]]:10: error: 'strcat' accesses 9 bytes of 'p', which has 8 bytes [weavec::out-of-bounds]
  strcat(p, "h");
  // CHECK: rfc0012-strings.c:[[@LINE-5]]:8: note: the object behind 'p' is declared here
}

// -- Deliberately not caught --------------------------------------------------

// Nothing is known about `src`: the copy may or may not fit, and the buffer
// may or may not be terminated afterwards (RFC 0012, *Bugs deliberately not
// caught*).
void unknown_source(const char *src) {
  char name[8];
  strcpy(name, src);
  strncpy(name, src, sizeof name);
  puts(name);
}
