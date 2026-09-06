#ifndef WEAVEC_TEST_HEAP13_H
#define WEAVEC_TEST_HEAP13_H
struct heap13_box { char *data; };
struct heap13_box *heap13_new(void);
struct heap13_box *heap13_wrap(char *p);
extern struct heap13_box *heap13_singleton;
void heap13_ensure(void);
void heap13_drop_child(void);
void heap13_swap(struct heap13_box *a, struct heap13_box *b);
void heap13_reset(struct heap13_box *b);
void heap13_local_swap(struct heap13_box *a, struct heap13_box *b);
void heap13_copy_and_free(struct heap13_box *a, struct heap13_box *b);
#endif
