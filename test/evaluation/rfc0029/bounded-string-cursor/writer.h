#include <stddef.h>
struct writer { unsigned char *data; size_t capacity, length; int format; };
void finish(struct writer *w);
