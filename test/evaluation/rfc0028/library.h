/* RFC 0028: public declarations deliberately hide object and module storage. */
#ifndef RFC0028_LIBRARY_H
#define RFC0028_LIBRARY_H
#include <stddef.h>
struct node;
struct buffer;
struct node *nodes(unsigned n);
void destroy(struct node *p);
struct node *reverse(struct node *p);
struct node *detach(struct node **p);
const struct node *next_node(const struct node *p);
unsigned count_nodes(const struct node *p);
void drop_then_destroy(struct node *p);
struct buffer *buffer_new(size_t capacity);
int buffer_append(struct buffer *p, char value);
const char *buffer_data(const struct buffer *p);
void buffer_destroy(struct buffer *p);
void hooks_reset(void);
void hooks_set(void *(*allocate)(size_t), void (*release)(void *));
struct node *hook_node(void);
void hook_destroy(struct node *p);
void hook_forget(void);
void configure(unsigned value);
void conditional_destroy(struct node *p);
#endif
