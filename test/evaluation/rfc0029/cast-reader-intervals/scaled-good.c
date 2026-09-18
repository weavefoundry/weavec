#include <string.h>
int main(void) {
    unsigned short input[3] = {0, 0, 0};
    return memcmp((const char *)(input + 1), "xx", 2);
}
