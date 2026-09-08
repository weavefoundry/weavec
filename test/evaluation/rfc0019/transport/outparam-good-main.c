/* RFC 0019: the frozen caller across a translation-unit boundary. */
#include <stdlib.h>
int make(char **);
int main(void) {char *p;if(!make(&p))return 0;int v=p[0];free(p);return v;}
