#include "forest.h"
static void *fail(size_t n) { (void)n;return 0; }
static void *render(const struct node *p) {
  if(p->flags!=1)return malloc(1);
  void *out=global_hooks.allocate(1);if(!out)return 0;
  *(char *)out='a';return out;
}
int main(void) {
  reset(0);struct node *p=create();if(!p)return 0;
  struct hooks h={fail,free};reset(&h);
  
  char *out=render(p);if(out){out[2]=1;free(out);}destroy(p); return 0;
}
