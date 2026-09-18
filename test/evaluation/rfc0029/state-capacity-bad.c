#include "state.h"
int main(void) {
    struct output b = {0};
    b.bytes = malloc(1);
    if (!b.bytes) return 0;
    b.available = 100;
    b.used = 99;
    int result = append(&b, 7);
    free(b.bytes);
    return result;
}
