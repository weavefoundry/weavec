// RFC 0033 §2: integer keys cast to pointers in a generic table (oniguruma's st_data_t, redis's
// dict) are ordinary pointers of unknown provenance; the table's real pointers stay usable.
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t data_t;
struct entry { data_t key; void *value; struct entry *next; };
struct table { struct entry *bins[8]; };
static void put(struct table *t, data_t key, void *value) {
  struct entry *e = malloc(sizeof *e);
  if (e == NULL) return;
  e->key = key;
  e->value = value;
  e->next = t->bins[key % 8];
  t->bins[key % 8] = e;
}
static void *get(struct table *t, data_t key) {
  for (struct entry *e = t->bins[key % 8]; e; e = e->next)
    if (e->key == key) return e->value;
  return NULL;
}
static int visit(data_t key, data_t value, void *arg) {
  const char *name = (const char *)value;   /* the value was a string */
  int *sum = arg;
  *sum += (int)key + (int)strlen(name);
  return 0;
}
int main(void) {
  struct table t;
  memset(&t, 0, sizeof t);
  char *name = strdup("alpha");
  if (name == NULL) return 1;
  put(&t, 3, name);
  put(&t, 4, (void *)(uintptr_t)42);       /* a number stored as a value */
  int sum = 0;
  char *found = get(&t, 3);
  visit(3, (data_t)found, &sum);
  int number = (int)(uintptr_t)get(&t, 4);
  for (int b = 0; b < 8; b++)
    while (t.bins[b]) { struct entry *e = t.bins[b]; t.bins[b] = e->next; free(e); }
  free(name);
  return sum == 8 && number == 42 ? 0 : 1;
}
