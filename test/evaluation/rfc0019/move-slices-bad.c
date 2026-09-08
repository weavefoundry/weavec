/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <string.h>
int main(void) {char a[8];a[0]=1;memmove(a+1,a,7);return a[7];}
