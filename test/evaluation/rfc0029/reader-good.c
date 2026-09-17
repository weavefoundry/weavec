#include "reader.h"
int main(void) {
    const unsigned char text[] = {1, 2, 3};
    struct reader r = {text, sizeof text, 0, 8};
    if (forwarded(&r) != 1) return 1;
    if (forwarded(&r) != 2) return 1;
    if (forwarded(&r) != 3) return 1;
    return forwarded(&r) != -1;
}
