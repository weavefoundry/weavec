#include <stddef.h>
struct reader { const unsigned char *data; size_t capacity, position; unsigned depth; };
int prefix(struct reader *r);
