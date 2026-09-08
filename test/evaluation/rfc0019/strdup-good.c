/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
#include <string.h>
int main(void) {char *p=strdup("abc");if(!p)return 0;int v=p[2];free(p);return v;}
