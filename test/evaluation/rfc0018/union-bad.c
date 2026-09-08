// RFC 0018 fixed checked-code evaluation: unsupported union identity.
#include <stdlib.h>
union pointer { int *a; int *b; };
int main(void) { union pointer u; u.a=malloc(sizeof(int)); if (!u.a) return 0; free(u.a); return *u.b; }
