// RFC 0018 fixed checked-code evaluation: incomplete initialization copy.
#include <string.h>
int main(void) { char a[4]={1,2,3,4},b[4]; memcpy(b,a,2); return b[3]; }
