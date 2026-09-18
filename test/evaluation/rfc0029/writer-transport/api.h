#include <stddef.h>
struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
int emit(const unsigned char *,size_t,struct writer*);
