#include "reader.h"
int main(void) {
    unsigned char input[6]; input[0] = 'a'; input[2] = 'c';
    struct reader r = {input, sizeof input, 0, 7};
    return prefix(&r);
}
