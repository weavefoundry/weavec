/* RFC 0019: the frozen caller across a translation-unit boundary. */
#include <stdlib.h>
struct buffer { char *data; unsigned capacity; }; int get(struct buffer *);
int main(void) { char a[1]; struct buffer b={a,1}; return get(&b); }
