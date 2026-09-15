/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
#include <stdlib.h>
int main(int n, char **v) { (void)v; hooks_set(n>1 ? malloc : NULL,free); struct node *p=hook_node(); hook_destroy(p); }
