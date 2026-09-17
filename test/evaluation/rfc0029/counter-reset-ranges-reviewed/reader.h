#include <stddef.h>
struct reader { const unsigned char *data; size_t position,capacity,extra; };
long scan(struct reader *);
