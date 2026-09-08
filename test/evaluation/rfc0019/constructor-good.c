/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static char *make(int initialize) {char *p=malloc(4); if(p && initialize) p[0]=7; return p;}
int main(void) {char *p=make(1);if(!p)return 0;int v=p[0];free(p);return v;}
