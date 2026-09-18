#include "reader.h"
#include <stdlib.h>
int main(void) {
    unsigned char data[5]={'x','0','1','0','x'};
    struct reader r={data,5,1,0};
    unsigned char *out=copy_digits(&r); if(out)free(out); return 0;
}
