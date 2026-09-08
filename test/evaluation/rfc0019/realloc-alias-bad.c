/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
int main(void) {char *p=malloc(4);if(!p)return 0;p[0]=7;char *q=realloc(p,8);if(!q){free(p);return 0;}int v=p[0];free(q);return v;}
