#include "buffer.h"
int main(int argc, char **argv) {
    (void)argv;
    unsigned n = (unsigned)argc;
    if (n > 1000000) return 0;
    struct buffer b = {0};
    for (unsigned i = 0; i < n; ++i) {
        if (append(&b, 7)) { destroy(&b); return 0; }
    }
    size_t length = b.length; unsigned char *p = steal(&b); int result = length ? p[length - 1] : 0; free(p);
    destroy(&b);
    return result;
}
