// RFC 0011, *Deferred lifetime checks*: storing the address of a local in
// longer-lived memory is reported when the local dies still pointed at, not
// at the store. The linked-frame idiom (push on entry, pop before return) is
// clean; forgetting the pop is `lifetime-too-short` at the closing brace.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct frame { struct frame *prev; int depth; };
struct state { struct frame *fs; int *p; };
int cond(void);

// Clean: the store is undone before `f` dies.
void enter(struct state *st) {
  struct frame f;
  f.prev = st->fs;
  st->fs = &f;
  st->fs = f.prev;
}

// Clean: reset to null.
void enter_reset(struct state *st) {
  struct frame f;
  st->fs = &f;
  st->fs = NULL;
}

// The pop is missing: reported where `f` goes out of scope.
void enter_bad(struct state *st) {
  struct frame f;
  f.prev = st->fs;
  // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:3: error: 'st->fs' may outlive 'f', which it points to [weavec::lifetime-too-short]
  st->fs = &f;
  // CHECK: rfc0011-lifetimes.c:[[@LINE-4]]:16: note: 'f' is declared here
  // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:1: note: 'f' goes out of scope here
}

// A store on one path only is still a store when the local dies.
void one_path(struct state *st) {
  int local = 1;
  if (cond())
    // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:5: error: 'st->p' may outlive 'local', which it points to [weavec::lifetime-too-short]
    st->p = &local;
  // CHECK: rfc0011-lifetimes.c:[[@LINE-4]]:7: note: 'local' is declared here
  // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:1: note: 'local' goes out of scope here
}

// Overwriting the escaped pointer with another local's address moves the
// report to the store that is live at the death.
int *g;
void to_global(void) {
  int local = 1;
  g = &local;
  g = NULL;
  // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:3: error: 'g' may outlive 'local', which it points to [weavec::lifetime-too-short]
  g = &local;
}

// Clean: the pointer to the local dies with it.
void same_scope(void) {
  int local = 1;
  int *p = &local;
  use(p);
}

// Returning the address of a local is still reported at the return
// (RFC 0002): the deferral is for stores, which can be undone.
int *escape(void) {
  int local = 1;
  // CHECK: rfc0011-lifetimes.c:[[@LINE+1]]:10: error: returned pointer may outlive 'local', which it points to [weavec::lifetime-too-short]
  return &local;
}
