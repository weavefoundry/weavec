/* RFC 0019: practical checked memory contracts.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */
#include <string.h>
int main(void) {char a[8]={1};memcpy(a+3,a,4);return a[4];}
