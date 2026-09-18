#ifndef RFC29_API_H
#define RFC29_API_H
#include <stddef.h>
struct node { unsigned value; struct node *left, *right; };
void destroy_even(struct node *p);
void destroy_odd(struct node *p);
void *make(void *(*allocate)(size_t), size_t count);
void dispose(void (*release)(void *), void *p);
#endif
