/* RFC 0019: the frozen caller across a translation-unit boundary. */
#include <stdlib.h>
char *make(int);
int main(void) {char *p=make(1);if(!p)return 0;int v=p[0];free(p);return v;}
