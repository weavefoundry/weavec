/* RFC 0019: the frozen caller across a translation-unit boundary. */
#include <stdlib.h>
void fill(char *,unsigned);
int main(void) {char a[8];fill(a,7);return a[7];}
