#include "runtime.h"
int main(int argc, char **argv) {
    (void)argv;
    unsigned n = (unsigned)argc;
    if (n > 1000000) return 0;
    struct buffer b = {0};
    for (unsigned i = 0; i < n; ++i) {
        if (append(&b, 7)) { destroy(&b); return 0; }
    }
    truncate_buffer(&b, 0); int result = (int)b.length;
    destroy(&b);
    return result;
}
