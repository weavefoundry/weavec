// RFC 0018 fixed checked-code evaluation: complete initialized copy.
#include <string.h>
int main(void) { char a[4]={1,2,3,4},b[4]; memcpy(b,a,4); return b[3]; }
