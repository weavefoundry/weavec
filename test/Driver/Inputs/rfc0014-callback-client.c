#include <stdlib.h>
void invoke(void (*)(void *), void *);
static void drop(void *p) { free(p); }
int main(void) {
  int *p = malloc(sizeof *p);
  if (!p)
    return 0;
  invoke(drop, p);
  return *p;
}
