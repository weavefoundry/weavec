// RFC 0043 §2–§5: a list whose nodes own the next node and their names,
// built, searched, unlinked through a pointer to an owner and cleared, in
// safe code; it runs clean in every mode.
// CLEAN
// FLAGS: -O2 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct node {
  struct node *WEAVEC_OWNED next;
  char *WEAVEC_OWNED WEAVEC_COUNTED_BY(size) name;
  size_t size;
};
struct list { struct node *WEAVEC_OWNED head; size_t n; };

static struct node *WEAVEC_OWNED node_new(const char *WEAVEC_NULL_TERMINATED s) {
  struct node *n = calloc(1, sizeof *n);
  if (n == NULL)
    return NULL;
  size_t size = strlen(s) + 1;
  char *name = malloc(size);
  if (name == NULL) {
    free(n);
    return NULL;
  }
  memcpy(name, s, size);
  n->name = name;
  n->size = size;
  return n;
}

static void node_free(struct node *WEAVEC_OWNED n);
static void node_free(struct node *WEAVEC_OWNED n) {
  if (n == NULL)
    return;
  free(n->name);
  node_free(n->next);
  free(n);
}

static int push(struct list *l, const char *WEAVEC_NULL_TERMINATED s) {
  struct node *n = node_new(s);
  if (n == NULL)
    return -1;
  node_free(n->next); // a node given away may hold one: drop it
  n->next = l->head;
  l->head = n;
  l->n++;
  return 0;
}

static int remove_named(struct list *l, char first) {
  struct node *WEAVEC_OWNED *pp = &l->head;
  while (*pp != NULL) {
    if ((*pp)->size > 1 && (*pp)->name[0] == first) {
      struct node *WEAVEC_OWNED victim = *pp;
      *pp = victim->next;
      victim->next = NULL;
      node_free(victim);
      l->n--;
      return 1;
    }
    pp = &(*pp)->next;
  }
  return 0;
}

static void clear(struct list *l) {
  struct node *WEAVEC_OWNED n = l->head;
  l->head = NULL;
  while (n != NULL) {
    struct node *WEAVEC_OWNED next = n->next;
    n->next = NULL;
    node_free(n);
    n = next;
  }
  l->n = 0;
}

static int run(void) {
  struct list l = {NULL, 0};
  if (push(&l, "alpha") || push(&l, "beta") || push(&l, "gamma")) {
    clear(&l);
    return 1;
  }
  int removed = remove_named(&l, 'b');
  size_t left = l.n;
  clear(&l);
  return removed == 1 && left == 2 ? 0 : 1;
}

WEAVEC_GUARDED int main(void) { return run(); }
