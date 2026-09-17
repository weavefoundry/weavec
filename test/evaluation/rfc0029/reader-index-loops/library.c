#include "reader.h"
int scan(struct reader *r) {
    if (!r || !r->data) return 0;
    for (size_t i = 0; r && r->position + i < r->capacity; ++i) {
        switch ((r->data + r->position)[i]) {
        case '0': case '1': break;
        default: goto done;
        }
    }
done:
    return 1;
}
