// RFC 0032 §3: the correct twin of uaf-container_bug.c: another item is removed, and 'b' stays live.
// STAGE: S3
// CLEAN
// ALLOW: leak
// RUN-INPUT: c
// ASAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct item { char *name; struct item *next; };
struct list { struct item *head; };
static void push(struct list *l, const char *n) {
  struct item *it = malloc(sizeof *it);
  if (!it) abort();
  it->name = strdup(n);
  it->next = l->head;
  l->head = it;
}
static struct item *find(struct list *l, const char *n) {
  for (struct item *it = l->head; it; it = it->next)
    if (it->name && strcmp(it->name, n) == 0) return it;
  return NULL;
}
static void remove_item(struct list *l, const char *n) {
  struct item **pp = &l->head;
  while (*pp) {
    if ((*pp)->name && strcmp((*pp)->name, n) == 0) {
      struct item *dead = *pp;
      *pp = dead->next;
      free(dead->name);
      free(dead);
      return;
    }
    pp = &(*pp)->next;
  }
}
static void clear(struct list *l) {
  while (l->head) {
    struct item *it = l->head;
    l->head = it->next;
    free(it->name);
    free(it);
  }
}
int main(int argc, char **argv) {
  struct list l = {0};
  push(&l, "a");
  push(&l, "b");
  push(&l, "c");
  struct item *b = find(&l, "b");
  if (argc > 1) remove_item(&l, argv[1]);
  int r = b ? b->name == NULL : 0;
  clear(&l);
  return r;
}
