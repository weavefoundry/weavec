/* RFC 0019: the frozen caller across a translation-unit boundary. */
#include <stdlib.h>
void copy(char *,const char *,unsigned);
int main(void) {char a[8];a[0]=1;char b[8];copy(b,a,8);return b[7];}
