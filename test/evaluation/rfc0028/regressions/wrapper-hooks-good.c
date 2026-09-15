/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
#include <stdlib.h>
static void *allocate(size_t n) { return malloc(n); }
static void release(void *p) { free(p); }
int main(void) { hooks_set(allocate,release); struct node *p=hook_node(); hook_destroy(p); }
