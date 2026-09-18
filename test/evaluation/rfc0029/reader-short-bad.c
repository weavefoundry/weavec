#include "reader.h"
int main(void) {
    const unsigned char text[] = {1};
    struct reader r = {text, 4, 3, 8};
    return forwarded(&r);
}
