#include "state.h"
int main(void) {
    struct output b = {0};
    if (reserve(&b, 8)) return 0;
    b.used = 8;
    int result = last(&b);
    free(b.bytes);
    return result;
}
