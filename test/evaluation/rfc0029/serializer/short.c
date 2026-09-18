#include "hex.h"
int main(void) {
    unsigned char input[2] = {1, 2};
    struct text output = {0};
    int result = hex_encode(&output, input, 8);
    hex_dispose(&output);
    return result;
}
