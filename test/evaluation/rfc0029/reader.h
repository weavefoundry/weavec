/* The initialized input extent and the consumed position are distinct. */
#include <stddef.h>
struct reader { const unsigned char *data; size_t size, position, depth; };
static int take(struct reader *r) {
    if (r->position >= r->size) return -1;
    int value = r->data[r->position];
    ++r->position;
    return value;
}
static int forwarded(struct reader *r) { return take(r); }
