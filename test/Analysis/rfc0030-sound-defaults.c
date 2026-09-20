// RFC 0030 §5 and §3.1: the defaults for code the analysis cannot see. None
// of them is a diagnostic; they are ledger rows, and errors only under a
// require level.
// RUN: %weavec --ledger=%t.json %s -- 2>&1 | FileCheck --check-prefix=QUIET %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
// RUN: not %weavec --require=checked %s -- 2>&1 | FileCheck --check-prefix=REQUIRE %s
// RUN: %weavec --no-zero-init --ledger=%t.nozero.json %s -- 2>&1
// RUN: FileCheck --check-prefix=NOZERO %s < %t.nozero.json
#include <stdlib.h>
#include <sys/ioctl.h>

// QUIET-NOT: {{warning|error}}:

void consume(char *p);

// §5.1: 'consume' may have released or kept 'p'; the release after it is
// unresolved too, and replaces its record.
int unknown(void) {
  char *p = malloc(8);
  if (!p)
    return 0;
  // LEDGER: "text": "consume(p)",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "declare 'consume' with WEAVEC_BORROWED on 'p' if it neither keeps nor frees it",
  // LEDGER-NEXT: "fixit": {
  // REQUIRE: rfc0030-sound-defaults.c:[[@LINE+1]]:3: error: call to 'consume' is neither proven nor checkable: 'consume' may have freed or kept 'p' [unknown-callee] [weavec::unresolved-operation]
  consume(p);
  // LEDGER: "text": "p[0]",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "consume",
  // REQUIRE: rfc0030-sound-defaults.c:[[@LINE+1]]:11: error: dereference of 'p' is neither proven nor checkable: 'consume' may have freed or kept 'p' [unknown-callee] [weavec::unresolved-operation]
  int v = p[0];
  // LEDGER: "text": "free(p)",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "unknown-callee",
  free(p);
  return v;
}

// §5.2: a platform function without a table entry borrows its arguments.
int system_api(int fd) {
  struct winsize ws;
  // LEDGER: "text": "ioctl(fd,TIOCGWINSZ,&ws)",
  // LEDGER: "outcome": "trusted",
  // LEDGER-NEXT: "reason": "system-api",
  if (ioctl(fd, TIOCGWINSZ, &ws) == -1)
    return 80;
  return ws.ws_col;
}

// §5.7: inline assembly handed `p` is unknown code.
int assembly(void) {
  char *p = malloc(8);
  if (!p)
    return 0;
  __asm__ volatile("" : : "r"(p) : "memory");
  // LEDGER: "text": "p[0]",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "inline assembly",
  int v = p[0];
  free(p);
  return v;
}

// §3.1: `b` may be the released `a` (both point to char).
void two(char *a, char *b) {
  free(a);
  // LEDGER: "text": "b[0]",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "may-alias-released",
  // LEDGER-NEXT: "detail": "'b' may point into an object released earlier",
  b[0] = 1;
}

// §11: without zero-initialisation a pointer that may be uninitialised may
// hold garbage no null check catches.
int deref(int c, int *q) {
  int *p;
  if (c)
    p = q;
  // NOZERO: "text": "*p",
  // NOZERO: "null": {
  // NOZERO-NEXT: "outcome": "unresolved",
  // NOZERO-NEXT: "reason": "no-zero-init",
  // NOZERO-NEXT: "detail": "'p' may be uninitialised",
  return *p;
}
