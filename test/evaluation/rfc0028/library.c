/* RFC 0028: independently compiled implementation. */
#include "library.h"
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
struct buffer { char *data; size_t length; size_t capacity; };
struct node *nodes(unsigned n) {
  struct node *head = NULL;
  for (unsigned i = 0; i < n; ++i) {
    struct node *p = malloc(sizeof *p);
    if (!p) break;
    p->value = i;
    p->next = head;
    head = p;
  }
  return head;
}
void destroy(struct node *p) {
  while (p) { struct node *next = p->next; free(p); p = next; }
}
struct node *reverse(struct node *p) {
  struct node *out = NULL;
  while (p) { struct node *next = p->next; p->next = out; out = p; p = next; }
  return out;
}
struct node *detach(struct node **p) {
  struct node *head = *p;
  if (head) { *p = head->next; head->next = NULL; }
  return head;
}
const struct node *next_node(const struct node *p) { return p ? p->next : NULL; }
unsigned count_nodes(const struct node *p) {
  unsigned n = 0;
  while (p) { ++n; p = p->next; }
  return n;
}
void drop_then_destroy(struct node *p) { if (p) p = p->next; destroy(p); }
struct buffer *buffer_new(size_t capacity) {
  if (!capacity) return NULL;
  struct buffer *p = malloc(sizeof *p);
  if (!p) return NULL;
  p->data = malloc(capacity);
  if (!p->data) { free(p); return NULL; }
  p->length = 0;
  p->capacity = capacity;
  return p;
}
int buffer_append(struct buffer *p, char value) {
  if (p->length >= p->capacity) return 0;
  p->data[p->length++] = value;
  return 1;
}
const char *buffer_data(const struct buffer *p) { return p->data; }
void buffer_destroy(struct buffer *p) { if (p) { free(p->data); free(p); } }
static struct { void *(*allocate)(size_t); void (*release)(void *); } hooks;
void hooks_reset(void) { hooks.allocate = malloc; hooks.release = free; }
void hooks_set(void *(*allocate)(size_t), void (*release)(void *)) {
  hooks.allocate = allocate;
  hooks.release = release;
}
struct node *hook_node(void) {
  struct node *p = hooks.allocate(sizeof *p);
  if (p) { p->value = 0; p->next = NULL; }
  return p;
}
void hook_destroy(struct node *p) { if (p) hooks.release(p); }
void hook_forget(void) { hooks.allocate = NULL; hooks.release = NULL; }
static unsigned configuration;
void configure(unsigned value) { configuration = value; }
void conditional_destroy(struct node *p) { if (configuration) destroy(p); }
