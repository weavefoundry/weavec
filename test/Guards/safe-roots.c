// RFC 0041 §4, amendment 19: where a safe function's bounds come from.
// A pointer that a loop's phis take from a local array and from memory
// keeps its guard (the walk over the phis decides every value it reaches at
// once); a framed array declared after a local placed on the heap gets
// bounds after its own alloca; and a bounded result of a call that may
// throw (an invoke under -fexceptions) keeps its guard.
// RUN: %weavec_cc -O0 -g %s -o %t0
// RUN: %t0
// RUN: %weavec_cc -O2 -g %s -o %t2
// RUN: %t2
// RUN: %weavec_cc -O0 -fexceptions -c %s -o %t.o
// RUN: %weavec_cc -O2 -fexceptions -c %s -o %t.o
#include <weavec.h>
#include <stddef.h>
#include <stdlib.h>

struct holder { char *WEAVEC_COUNTED_BY(len) p; int len; };

WEAVEC_SAFE int alternate(const struct holder *h, int n) {
  char buf[16] = {1};
  char *p = buf;
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += p[1];
    if (i & 1) {
      p = p + 1;
      s += *p;
    } else {
      p = h->p;
    }
  }
  return s;
}

struct node { struct node *next; int v; };
static struct node *head;

WEAVEC_SAFE void push(struct node *WEAVEC_ESCAPES n) {
  n->next = head;
  head = n;
}

WEAVEC_SAFE int framed(int n) {
  struct node boxed = {0, n};
  int arr[8] = {0};
  push(&boxed);
  arr[n & 7] = 1;
  int v = arr[3] + head->v;
  head = 0;
  return v;
}

static void done(int *x) { (void)x; }

WEAVEC_SAFE char *WEAVEC_OWNED WEAVEC_COUNTED_BY(n) make(size_t n) {
  return calloc(n, 1);
}

WEAVEC_SAFE int thrown(size_t n) {
  int kept __attribute__((cleanup(done))) = 0;
  char *p = make(n);
  int c = p[n - 1] + kept;
  free(p);
  return c;
}

int main(void) {
  char big[64] = {0};
  struct holder h = {big, 64};
  return alternate(&h, 40) == 0 && framed(3) == 4 && thrown(8) == 0 ? 0 : 1;
}
