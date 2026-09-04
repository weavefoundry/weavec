// RFC 0009, *Inferred `noreturn`*: a function whose exit is unreachable is
// summarised `never-returns`, transitively through wrappers, and a call to it
// ends the path like a call to a function declared `noreturn` does.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

void abort(void);
typedef int jmp_buf[16];
void longjmp(jmp_buf env, int val);
static jmp_buf env;

// Unannotated wrappers around `abort` and `longjmp`.
// DUMP-LABEL: function 'die':
// DUMP: summary: never-returns; *msg: read; stores{} returns{}
static void die(const char *msg) {
  use(msg);
  abort();
}

// DUMP-LABEL: function 'fail':
// DUMP: summary: never-returns; stores{} returns{}
static void fail(int code) {
  if (code > 3)
    die("big");
  die("small");
}

// DUMP-LABEL: function 'throw_':
// DUMP: summary: never-returns; stores{} returns{}
static void throw_(int code) {
  longjmp(env, code);
}

// DUMP-LABEL: function 'spin':
// DUMP: summary: never-returns; stores{} returns{}
static void spin(void) {
  for (;;)
    ;
}

// Returns on some paths: not `never-returns` (*Future work*).
// DUMP-LABEL: function 'check':
// DUMP: summary: stores{} returns{}
static void check(int ok) {
  if (!ok)
    die("bad");
}

// Clean: the bad path never reaches the use.
void good_path(int bad) {
  char *q = malloc(8);
  if (bad) {
    free(q);
    fail(bad);
  }
  use(q);
  free(q);
}

void good_path_throw(int bad) {
  char *q = malloc(8);
  if (bad) {
    free(q);
    throw_(1);
  }
  use(q);
  free(q);
}

// Clean: nothing is leaked at the end of a block that is never left.
void no_leak_after_die(int bad) {
  char *q = malloc(8);
  if (bad)
    die("bad");
  free(q);
}

// Clean: code after the call is dead.
void dead_tail(char *p) {
  free(p);
  die("x");
  use(p);
}

void spins(char *p) {
  free(p);
  spin();
  use(p);
}

// Reported: `check` returns when `ok` is non-zero.
void check_returns(int bad) {
  char *q = malloc(8);
  if (bad) {
    free(q);
    check(bad);
  }
  // CHECK: rfc0009-noreturn.c:[[@LINE+1]]:7: error: use of 'q' after it was freed [weavec::use-after-free]
  use(q);
  // CHECK: rfc0009-noreturn.c:[[@LINE+1]]:3: error: 'q' is freed twice [weavec::double-free]
  free(q);
}

// CHECK: 2 errors generated.
