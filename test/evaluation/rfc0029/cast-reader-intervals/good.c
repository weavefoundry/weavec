#include "reader.h"
int main(void) {
    unsigned char input[6] = {'a', 'b', 'c', 0, 0, 0};
    struct reader r = {input, sizeof input, 0, 7};
    return prefix(&r);
}
