#include "state.h"
int main(void) {
    struct output b = {0};
    if (append(&b, 7)) return 0;
    unsigned char *saved = b.bytes;
    if (reserve(&b, 128)) { free(b.bytes); return 0; }
    int result = *saved;
    free(b.bytes);
    return result;
}
