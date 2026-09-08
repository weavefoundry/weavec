/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
static void touch(char *p){p[0]=1;}
int main(void) {char *p=malloc(4);if(!p)return 0;char x;touch(&x);int v=p[0];free(p);return v;}
