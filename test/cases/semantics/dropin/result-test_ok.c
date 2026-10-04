// RFC 0033 §1: a test of a call's result does not refine the call's argument unless the
// callee returns that argument on the tested outcome (redis's listDup).
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
struct node { struct node *next; void *value; };
struct list { struct node *head, *tail; unsigned long len; };
static struct list *list_create(void) {
  struct list *l = malloc(sizeof *l);
  if (l == NULL) return NULL;
  l->head = l->tail = NULL;
  l->len = 0;
  return l;
}
static void list_release(struct list *l) {
  struct node *n = l->head;
  while (n) { struct node *next = n->next; free(n); n = next; }
  free(l);
}
static struct list *list_add_tail(struct list *l, void *value) {
  struct node *n = malloc(sizeof *n);
  if (n == NULL) return NULL;
  n->value = value;
  n->next = NULL;
  if (l->tail) l->tail->next = n; else l->head = n;
  l->tail = n;
  l->len++;
  return l;
}
static struct list *list_dup(struct list *orig) {
  struct list *copy = list_create();
  if (copy == NULL) return NULL;
  for (struct node *n = orig->head; n; n = n->next)
    if (list_add_tail(copy, n->value) == NULL) {
      list_release(copy);
      return NULL;
    }
  copy->len = orig->len;
  return copy;
}
int main(void) {
  static int x, y;
  struct list *l = list_create();
  if (l == NULL || !list_add_tail(l, &x) || !list_add_tail(l, &y)) return 1;
  struct list *d = list_dup(l);
  int ok = d != NULL && d->len == 2;
  if (d) list_release(d);
  list_release(l);
  return ok ? 0 : 1;
}
