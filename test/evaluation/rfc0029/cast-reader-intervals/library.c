#include "reader.h"
#include <string.h>
int prefix(struct reader *r) {
    if (!r || !r->data || r->position != 0) return 0;
    if (r->position + 4 < r->capacity &&
        strncmp((const char *)(r->data + r->position), "abc", 3) == 0) {
        r->position += 3;
        return 1;
    }
    return 0;
}
