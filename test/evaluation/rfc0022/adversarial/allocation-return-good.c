/* RFC 0022: typed allocation returned through an opaque interface. */
#include <stdlib.h>
static void *create(void) {
  int *p = malloc(sizeof(int));
  if (p)
    *p = 7;
  return p;
}
int main(void) {
  int *p = create();
  if (!p)
    return 0;
  int n = *p;
  free(p);
  return n;
}
