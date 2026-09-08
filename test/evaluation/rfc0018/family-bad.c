// RFC 0018 fixed checked-code evaluation: release family.
#include <stdio.h>
#include <stdlib.h>
int main(void) { FILE *f=fopen("x","r"); if (!f) return 0; free(f); return 0; }
