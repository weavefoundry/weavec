#include <stddef.h>
struct reader { unsigned depth; const unsigned char *data; size_t limit, pos; };
int take(struct reader *r);
