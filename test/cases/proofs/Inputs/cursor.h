#include <stddef.h>
struct cursor { unsigned flags; size_t depth; const unsigned char *data; size_t end, position; };
int take(struct cursor *, unsigned char *);
unsigned consume(struct cursor *);
