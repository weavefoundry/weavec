// RFC 0007, *Snippets that must be clean*, plus the idioms that a leak
// checker most often gets wrong: null-tested results, ownership handed to
// the caller or to a struct, unknown callees, `getline`, loops over arrays,
// nulled and conditionally-freed fields, wrappers, `goto` cleanup, `exit`,
// by-value struct returns, and pointers laundered through integers.
// RUN: %weavec -Wno-weavec-annotation-required %s -- 2>&1 | count 0
// RUN: %weavec -Wno-weavec-leak %s -- 2>&1 | FileCheck --check-prefix=BOUNDARY %s
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct buf {
  char *data;
  size_t n;
};

struct box {
  char *WEAVEC_OWNED p;
};

struct node {
  int v;
  struct node *next;
};

void register_thing(void *p);
static void xfree(void *p) { free(p); }

// The RFC's list.
int checked(void) {
  char *p = malloc(8);
  if (!p)
    return -1;
  free(p);
  return 0;
}
char *handed_out(void) {
  char *p = malloc(8);
  return p;
}
void stored(struct buf *b) { b->data = malloc(8); }
void kept(char *p) {
  static char *keep;
  keep = p;
}
// BOUNDARY: rfc0007-clean.c:[[@LINE+3]]:3: warning: call to 'register_thing' is not checked
void to_unknown(void) {
  char *p = malloc(8);
  register_thing(p);
}
void lines(FILE *f) {
  char *l = NULL;
  size_t n = 0;
  while (getline(&l, &n, f) != -1) {
  }
  free(l);
}
void loop(char **a, int n) {
  for (int i = 0; i < n; i++)
    free(a[i]);
  free(a);
}
void nulled(struct box *b) {
  free(b->p);
  b->p = NULL;
  free(b);
}
void maybe(struct box *b) {
  if (b->p)
    free(b->p);
  free(b);
}
void wrapper(char *p) { xfree(p); }

// More null-test spellings.
int checked2(void) {
  char *p = malloc(8);
  if (p == NULL)
    return -1;
  free(p);
  return 0;
}
int checked3(void) {
  char *p;
  if (!(p = malloc(8)))
    return -1;
  free(p);
  return 0;
}
int checked4(void) {
  char *p = malloc(8);
  if (p) {
    p[0] = 0;
    free(p);
  }
  return 0;
}
char *handed_out_or_null(int c) {
  char *p = malloc(8);
  return c ? p : NULL;
}

// Ownership kept by a global or a `static` local.
void kept_owned(void) {
  static char *keep;
  char *p = malloc(8);
  keep = p;
}

// Through an out-parameter, in both directions.
void out_param(char **out) { *out = malloc(8); }
int out_param_user(void) {
  char *s;
  out_param(&s);
  free(s);
  return 0;
}

// Into a container the caller owns.
void push(struct node **head, struct node *n) {
  n->next = *head;
  *head = n;
}
void push_use(struct node **head) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return;
  push(head, n);
}
void arr_fill(char **a, int n) {
  for (int i = 0; i < n; i++)
    a[i] = malloc(4);
}
void local_arr(void) {
  char *a[4];
  for (int i = 0; i < 4; i++)
    a[i] = malloc(4);
  for (int i = 0; i < 4; i++)
    free(a[i]);
}

// By-value structs carry their owned fields.
struct buf make(void) {
  struct buf b;
  b.data = malloc(8);
  b.n = 8;
  return b;
}
void by_value_out(struct buf *out) {
  struct buf b;
  b.data = malloc(8);
  b.n = 8;
  *out = b;
}
void tmp_struct(void) {
  struct buf b = {.data = malloc(8), .n = 8};
  free(b.data);
}

// Laundered through an integer: the checker can no longer follow it.
void to_int(void) {
  char *p = malloc(8);
  uintptr_t u = (uintptr_t)p;
  (void)u;
}

// Library functions that keep their argument (RFC 0007, *Assumptions*).
void into_env(void) {
  char *s = strdup("A=b");
  if (s)
    putenv(s);
}

// Constructors: fields of a fresh object own nothing yet, and the error path
// frees the object alone.
struct box *box_new(void) {
  struct box *b = malloc(sizeof *b);
  if (!b)
    return NULL;
  b->p = malloc(4);
  if (!b->p) {
    free(b);
    return NULL;
  }
  return b;
}
void box_free(struct box *b) {
  free(b->p);
  free(b);
}
void memset_fresh(void) {
  struct buf *b = malloc(sizeof *b);
  if (!b)
    return;
  memset(b, 0, sizeof *b);
  b->data = malloc(4);
  free(b->data);
  free(b);
}

// `goto` cleanup and `free(NULL)`.
void goto_cleanup(void) {
  char *a = NULL, *b = NULL;
  a = malloc(8);
  if (!a)
    goto out;
  b = malloc(8);
  if (!b)
    goto out;
out:
  free(a);
  free(b);
}

// `realloc` with the failure path handled (RFC 0006).
void realloc_grow(char *WEAVEC_OWNED p, size_t n) {
  char *q = realloc(p, n);
  if (!q) {
    free(p);
    return;
  }
  free(q);
}

// A block that ends in `exit` is not a death point.
void exit_path(void) {
  char *p = malloc(8);
  if (!p)
    exit(1);
  p[0] = 0;
  exit(0);
}

// Linked lists.
void list_free(struct node *n) {
  while (n) {
    struct node *next = n->next;
    free(n);
    n = next;
  }
}
struct node *list_new(void) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return NULL;
  n->next = NULL;
  return n;
}

// Both arms release.
int cond_free(int c) {
  char *p = malloc(8);
  if (!p)
    return -1;
  if (c) {
    free(p);
    return 1;
  }
  free(p);
  return 0;
}

// Swapping two owned pointers moves both.
void swap(char **a, char **b) {
  char *t = *a;
  *a = *b;
  *b = t;
}

// Owned parameters released or stored.
void owned_param_ok(char *WEAVEC_OWNED p) { free(p); }
void owned_param_stored(struct buf *b, char *WEAVEC_OWNED p) { b->data = p; }

// A callback context handed to an unknown registrar.
void cb(void *ctx);
void register_cb(void (*fn)(void *), void *ctx);
// BOUNDARY: rfc0007-clean.c:[[@LINE+3]]:3: warning: call to 'register_cb' is not checked
void with_ctx(void) {
  char *ctx = malloc(8);
  register_cb(cb, ctx);
}

// BOUNDARY-NOT: leak
// BOUNDARY-NOT: error:
