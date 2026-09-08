/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <stdlib.h>
char *make(int initialize) {char *p=malloc(4); if(p && initialize) p[0]=7; return p;}
