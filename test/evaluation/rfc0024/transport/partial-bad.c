#include "runtime.h"
int main(void){char b[4];ssize_t r=pull(b,1);if(r<=0)return 0;return b[3];}
