#include "reader.h"
int main(void) {
    unsigned char input[2] = {'a', 'b'};
    struct reader r = {input, 6, 0, 7};
    return prefix(&r);
}
