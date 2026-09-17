#include "reader.h"
int scan(struct reader *r) {
    if (!r || !r->data) return 0;
    for (size_t i = 0; r->position + i < r->capacity; ++i) {
        r->position=r->capacity;
        if ((r->data + r->position)[i] == 'x') return 1;
    }
    return 0;
}
int main(void) { unsigned char input[1]={'0'}; struct reader r={input,1,0,0}; return scan(&r); }
