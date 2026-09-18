#include "hex.h"
#include <stdlib.h>
int encode_client(size_t count) {
    if (count > 4096) return 0;
    unsigned char *input = calloc(count, 1);
    if (!input) return 0;
    struct text output = {0};
    int success = hex_encode(&output, input, count);
    hex_dispose(&output);
    free(input);
    return success;
}
int main(void) { return encode_client(33) < 0; }
