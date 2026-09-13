#include "runtime.h"
int main(int argc, char **argv) {
    (void)argv;
    unsigned n = (unsigned)argc;
    if (n > 1000000) return 0;
    struct buffer b = {0};
    for (unsigned i = 0; i < n; ++i) {
        if (append(&b, 7)) { destroy(&b); return 0; }
    }
    int result = b.length ? b.data[b.length - 1] : 0;
    destroy(&b);
    return result;
}
