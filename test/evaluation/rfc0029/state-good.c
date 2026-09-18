#include "state.h"
int client(unsigned n) {
    if (n > 1000000) return 0;
    struct output b = {0};
    b.depth = 3;
    b.generation = 42;
    for (unsigned i = 0; i < n; ++i) {
        if (append(&b, 7)) { free(b.bytes); return 0; }
    }
    int result = last(&b);
    free(b.bytes);
    return result;
}
int main(void) { return client(19) != 7; }
