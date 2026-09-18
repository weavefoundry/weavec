#include <stddef.h>
struct reader { const unsigned char *data; size_t capacity, position; unsigned depth; };
unsigned char *copy_digits(struct reader *r);
