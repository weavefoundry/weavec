// RFC 0032 §3, §2.4: a pointer into a released heap object finds a dead object, and its use traps.
// STAGE: S3
// 'b' is found before 'remove_item' releases the item it points to. The analysis cannot
// tell which item was removed (unknown-callee); the 'live' guard can: the block is in the
// quarantine.
// RUN-INPUT: b
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
int main(int argc, char **argv) {
  struct list l = {0};
  push(&l, "a");
  push(&l, "b");
  push(&l, "c");
  struct item *b = find(&l, "b");
  if (argc > 1) remove_item(&l, argv[1]);
  if (b) return b->name == NULL; // BUG: use-after-free // TRAP
  return 0;
}
